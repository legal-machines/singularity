#include "Usage/UsageAccounting/UsageAccounting.hpp"

#include "Provider/Types/UsageMetadata.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace singularity::usage {
namespace {

bool has_token_usage(const providers::UsageMetadata& usage) {
  return usage.input_tokens.has_value() || usage.output_tokens.has_value() ||
         usage.total_tokens.has_value();
}

bool has_complete_cost(const providers::UsageMetadata& usage) {
  const bool currency_is_valid =
      usage.cost_currency.has_value() && usage.cost_currency->size() == 3 &&
      std::all_of(usage.cost_currency->begin(), usage.cost_currency->end(),
                  [](const char value) { return value >= 'A' && value <= 'Z'; });
  return usage.cost_micros.has_value() && currency_is_valid &&
         usage.cost_classification.has_value() && usage.cost_methodology.has_value() &&
         *usage.cost_micros >= 0 && usage.cost_methodology->empty() == false;
}

bool is_iso_utc_timestamp(const std::string& value) {
  if (value.size() != 20)
    return false;
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value[19] != 'Z') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || index == 19)
      continue;
    if (std::isdigit(static_cast<unsigned char>(value[index])) == 0)
      return false;
  }
  return true;
}

// One operation can run more than once - its proposal, then its write once the
// lawyer approved it - and each run may be its own Routing Log entry. The
// operation's own usage covers every run, so it is compared with all of its
// entries added up (the chat reads them the same way), never with the first
// run alone.
std::optional<routing::RoutingLogEntry>
find_routing_entry(const OperationUsageEvidence& operation,
                   const std::vector<routing::RoutingLogEntry>& entries) {
  std::optional<routing::RoutingLogEntry> combined;
  for (const routing::RoutingLogEntry& entry : entries) {
    if (entry.operation_id.empty() || entry.operation_id != operation.operation_id)
      continue;
    if (combined.has_value() == false) {
      combined = entry;
      continue;
    }
    combined->usage = providers::accumulate_usage(combined->usage, entry.usage);
  }
  if (combined.has_value())
    return combined;
  for (const routing::RoutingLogEntry& entry : entries) {
    if (entry.matter_id == operation.project_id && entry.task_id == operation.task_id &&
        entry.attempt == operation.attempt && entry.time == operation.timestamp) {
      return entry;
    }
  }
  return std::nullopt;
}

BilledCall billed_of(const protected_api::papinian::OutboundCall& call) {
  BilledCall billed;
  billed.time = call.time;
  billed.provider_id = call.provider_id;
  billed.model_id = call.model_id;
  billed.served_by = call.served_by;
  billed.provider_request_id = call.provider_request_id;
  billed.payload_sha256 = call.payload_sha256;
  billed.response_sha256 = call.response_sha256;
  billed.input_tokens = call.input_tokens;
  billed.cached_input_tokens = call.cached_input_tokens;
  billed.output_tokens = call.output_tokens;
  billed.reasoning_tokens = call.reasoning_tokens;
  billed.cost_micros = call.cost_micros;
  billed.cost_currency = call.cost_currency;
  billed.cost_classification = call.cost_classification;
  billed.cost_methodology = call.cost_methodology;
  billed.protection_mode = call.protection_mode;
  billed.record_chain = call.record_chain;
  billed.record_index = call.record_index;
  billed.confirmation = call.confirmation;
  return billed;
}

// Every model call an operation made, in the order made: only what a bill
// shows of a call, never the markers or passages that left with it. The
// Matter's own record of them comes first - it travels to every computer of
// the firm, whichever ran the work; else each Routing Log entry the operation
// wrote here. `recorded` is false when neither holds them: the work ran
// before Singularity kept them, or on a computer that kept them only there.
std::vector<BilledCall> billed_calls(const OperationUsageEvidence& operation,
                                     const matters::MatterTask& task,
                                     const std::vector<routing::RoutingLogEntry>& entries,
                                     bool& recorded) {
  if (task.billed_calls.empty() == false) {
    recorded = true;
    std::vector<BilledCall> kept;
    for (const protected_api::papinian::OutboundCall& call : task.billed_calls)
      if (call.kind.empty() || call.kind == "model") kept.push_back(billed_of(call));
    std::stable_sort(kept.begin(), kept.end(),
                     [](const BilledCall& left, const BilledCall& right) { return left.time < right.time; });
    return kept;
  }
  std::vector<const routing::RoutingLogEntry*> own;
  for (const routing::RoutingLogEntry& entry : entries) {
    if (entry.operation_id.empty() == false && entry.operation_id == operation.operation_id)
      own.push_back(&entry);
  }
  if (own.empty() == true) {
    for (const routing::RoutingLogEntry& entry : entries) {
      if (entry.matter_id == operation.project_id && entry.task_id == operation.task_id &&
          entry.attempt == operation.attempt && entry.time == operation.timestamp) {
        own.push_back(&entry);
        break;
      }
    }
  }
  recorded = own.empty() == false;
  std::vector<BilledCall> calls;
  for (const routing::RoutingLogEntry* entry : own) {
    if (entry->outbound_calls_recorded == false) recorded = false;
    for (const protected_api::papinian::OutboundCall& call : entry->privacy_metadata.outbound_calls) {
      if (call.kind != "model") continue;
      calls.push_back(billed_of(call));
    }
  }
  std::stable_sort(calls.begin(), calls.end(),
                   [](const BilledCall& left, const BilledCall& right) { return left.time < right.time; });
  return calls;
}

bool optional_equal(const std::optional<std::string>& left,
                    const std::optional<std::string>& right) {
  return left == right;
}

template <typename Value>
bool compatible_when_both_present(const std::optional<Value>& left,
                                  const std::optional<Value>& right) {
  return left.has_value() == false || right.has_value() == false || left == right;
}

bool usage_compatible(const providers::UsageMetadata& left,
                      const providers::UsageMetadata& right) {
  const bool tokens_match =
      compatible_when_both_present(left.input_tokens, right.input_tokens) &&
      compatible_when_both_present(left.output_tokens, right.output_tokens) &&
      compatible_when_both_present(left.total_tokens, right.total_tokens);
  if (tokens_match == false) return false;
  if (has_complete_cost(left) == false || has_complete_cost(right) == false) return true;
  return left.cost_micros == right.cost_micros && left.cost_currency == right.cost_currency &&
         left.cost_classification == right.cost_classification &&
         left.cost_methodology == right.cost_methodology;
}

std::vector<std::string> work_products_for_operation(const matters::Matter& project,
                                                     const std::string& operation_id) {
  std::vector<std::string> result;
  for (const matters::WorkProduct& product : project.work_products) {
    if (product.origin.kind == matters::WorkProductOriginKind::ChatTask &&
        product.origin.reference_id.has_value() && *product.origin.reference_id == operation_id) {
      result.push_back(product.id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

providers::UsageMetadata reconcile_provider_usage(const providers::UsageMetadata& operation_usage,
                                                  const routing::RoutingLogEntry* routing_entry) {
  providers::UsageMetadata result = operation_usage;
  if (routing_entry == nullptr) return result;
  const providers::UsageMetadata& routed = routing_entry->usage;
  if (result.input_tokens.has_value() == false) result.input_tokens = routed.input_tokens;
  if (result.output_tokens.has_value() == false) result.output_tokens = routed.output_tokens;
  if (result.total_tokens.has_value() == false) result.total_tokens = routed.total_tokens;
  if (has_complete_cost(result) == false && has_complete_cost(routed)) {
    result.cost_micros = routed.cost_micros;
    result.cost_currency = routed.cost_currency;
    result.cost_classification = routed.cost_classification;
    result.cost_methodology = routed.cost_methodology;
  }
  return result;
}

void add_currency_total(std::vector<CurrencyTotal>& totals,
                        const OperationUsageEvidence& operation) {
  if (operation.usage.cost_micros.has_value() == false ||
      operation.usage.cost_currency.has_value() == false)
    return;
  if (operation.cost_classification != CostClassification::Actual &&
      operation.cost_classification != CostClassification::Estimated) {
    return;
  }

  auto found = std::find_if(totals.begin(), totals.end(), [&](const CurrencyTotal& value) {
    return value.currency == *operation.usage.cost_currency;
  });
  if (found == totals.end()) {
    totals.push_back(CurrencyTotal{*operation.usage.cost_currency, std::nullopt, std::nullopt});
    found = std::prev(totals.end());
  }

  if (operation.cost_classification == CostClassification::Actual) {
    found->actual_micros = found->actual_micros.value_or(0) + *operation.usage.cost_micros;
    return;
  }
  found->estimated_micros = found->estimated_micros.value_or(0) + *operation.usage.cost_micros;
}

} // namespace

BilledCall billed_call_of(const protected_api::papinian::OutboundCall& call) { return billed_of(call); }

const char* to_string(CostClassification classification) noexcept {
  switch (classification) {
  case CostClassification::Actual:
    return "actual";
  case CostClassification::Estimated:
    return "estimated";
  case CostClassification::UsageOnly:
    return "usage_only";
  case CostClassification::Unavailable:
    return "unavailable";
  }
  return "unavailable";
}

UsagePeriod validate_period(const std::string& start, const std::string& end) {
  if (is_iso_utc_timestamp(start) == false || is_iso_utc_timestamp(end) == false || start >= end) {
    throw std::runtime_error("Choose a valid UTC usage period.");
  }
  return {start, end};
}

CostClassification classify_cost(const providers::UsageMetadata& usage) noexcept {
  if (has_complete_cost(usage) == true && *usage.cost_classification == "actual") {
    return CostClassification::Actual;
  }
  if (has_complete_cost(usage) == true && *usage.cost_classification == "estimated") {
    return CostClassification::Estimated;
  }
  if (has_token_usage(usage) == true)
    return CostClassification::UsageOnly;
  return CostClassification::Unavailable;
}

std::string format_cost(long long amount_micros, const std::string& currency) {
  const long long whole = amount_micros / 1'000'000;
  const long long fraction = amount_micros % 1'000'000;
  std::ostringstream result;
  result << currency << ' ' << whole;
  if (fraction == 0)
    return result.str();
  result << '.' << std::setw(6) << std::setfill('0') << fraction;
  std::string value = result.str();
  while (value.back() == '0')
    value.pop_back();
  return value;
}

UsageSummary reconcile_usage(const std::vector<matters::Matter>& projects,
                             const std::vector<routing::RoutingLogEntry>& routing_entries,
                             const UsagePeriod& period,
                             const std::optional<std::string>& project_id) {
  UsageSummary summary;
  summary.period = period;

  for (const matters::Matter& project : projects) {
    if (project_id.has_value() && project.id != *project_id)
      continue;
    for (const matters::MatterTask& task : project.tasks) {
      if (task.created_at < period.start || task.created_at >= period.end)
        continue;
      // A person's or an AI colleague's words in a thread, and a thread's
      // closing, are no request: no model read them, and no bill counts them.
      if (task.operation_type == "member_message" || task.operation_type == "agent_message")
        continue;

      OperationUsageEvidence operation;
      operation.project_id = project.id;
      operation.operation_id = task.id;
      operation.task_id = task.root_task_id.value_or(task.id);
      operation.attempt = task.attempt;
      operation.timestamp = task.created_at;
      operation.provider_id = task.provider_id;
      operation.model_id = task.model_id;
      operation.execution_kind = task.execution_kind;
      operation.operation_type = task.operation_type;
      operation.operation_role = task.operation_role;
      operation.operation_status =
          task.status == matters::TaskStatus::Success ? "success" : "error";
      // A turn's thread, or the thread an AI colleague's own call worked for.
      operation.chat_id = task.chat_id;
      if (operation.chat_id.has_value() == false && task.work_chat_id.empty() == false) operation.chat_id = task.work_chat_id;
      operation.consilium_id = task.consilium_group_id;
      operation.workflow_run_id = task.workflow_run_id;
      operation.work_product_ids = work_products_for_operation(project, task.id);
      // Whose work it was: a turn an AI colleague began itself is written
      // under its name; any other names the person who asked.
      operation.agent_id = task.agent_id;
      operation.agent_initiated = task.agent_initiated;
      if (task.agent_initiated == true) {
        operation.agent_name = task.author_name;
      } else if (task.author_actor_id.empty() == false) {
        operation.asked_by_actor_id = task.author_actor_id;
        operation.asked_by_name = task.author_name;
      }
      operation.privacy_mode =
          task.privacy_metadata.mode.empty() ? task.protected_api_mode : task.privacy_metadata.mode;
      operation.protected_external = task.protected_api_applied;
      // A request sent with protection Off records the mode "off"; only a record
      // that never said how it was protected leaves the mode empty.
      operation.protection_recorded = task.execution_kind != "external" ||
                                      task.protected_api_applied ||
                                      !task.protected_api_mode.empty();

      const std::optional<routing::RoutingLogEntry> routing_entry =
          find_routing_entry(operation, routing_entries);
      operation.usage =
          reconcile_provider_usage(task.usage, routing_entry ? &*routing_entry : nullptr);
      operation.cost_classification = classify_cost(operation.usage);
      operation.calls = billed_calls(operation, task, routing_entries, operation.calls_recorded);
      operation.evidence_status = "operation_only";
      if (routing_entry.has_value()) {
        const bool identity_matches =
            routing_entry->matter_id == project.id &&
            optional_equal(routing_entry->provider_id, task.provider_id) &&
            optional_equal(routing_entry->model_id, task.model_id) &&
            (routing_entry->execution_kind.empty() || task.execution_kind.empty() ||
             routing_entry->execution_kind == task.execution_kind);
        const bool provider_usage_matches = usage_compatible(task.usage, routing_entry->usage);
        operation.evidence_status =
            identity_matches && provider_usage_matches ? "reconciled" : "needs_review";
      }

      // A paid external model call made by one of the operation's tools (web
      // research on a hosted model) is its own usage record: its provider,
      // model and reported usage, never merged into the reasoning model's.
      for (const tools::ToolCallEvidence& call : task.tool_call_evidence) {
        if (call.external_usage.has_value() == false) continue;
        OperationUsageEvidence external;
        external.project_id = operation.project_id;
        external.operation_id = call.id.empty() ? task.id + ":" + call.tool_id : call.id;
        external.task_id = operation.task_id;
        external.attempt = operation.attempt;
        external.timestamp = operation.timestamp;
        external.provider_id = call.external_provider_id;
        external.model_id = call.external_model_id;
        external.execution_kind = "external";
        external.operation_type = call.capability;
        external.operation_role = "research";
        external.operation_status = call.outcome == tools::ToolOutcome::Executed ? "success" : "error";
        external.chat_id = operation.chat_id;
        external.consilium_id = operation.consilium_id;
        external.workflow_run_id = operation.workflow_run_id;
        // The research is the same colleague's work, asked by the same person.
        external.agent_id = operation.agent_id;
        external.agent_initiated = operation.agent_initiated;
        external.agent_name = operation.agent_name;
        external.asked_by_actor_id = operation.asked_by_actor_id;
        external.asked_by_name = operation.asked_by_name;
        external.privacy_mode = operation.privacy_mode;
        external.protected_external = operation.protected_external;
        external.protection_recorded = operation.protection_recorded;
        external.usage = *call.external_usage;
        external.cost_classification = classify_cost(external.usage);
        external.evidence_status = "operation_only";
        summary.operations.push_back(std::move(external));
      }
      summary.operations.push_back(std::move(operation));
    }
  }

  std::sort(summary.operations.begin(), summary.operations.end(),
            [](const auto& left, const auto& right) {
              if (left.timestamp != right.timestamp)
                return left.timestamp < right.timestamp;
              return left.operation_id < right.operation_id;
            });

  for (const OperationUsageEvidence& operation : summary.operations) {
    ++summary.operation_count;
    summary.input_tokens += operation.usage.input_tokens.value_or(0);
    summary.output_tokens += operation.usage.output_tokens.value_or(0);
    summary.total_tokens += operation.usage.total_tokens.value_or(0);
    if (operation.cost_classification == CostClassification::Actual)
      ++summary.actual_count;
    if (operation.cost_classification == CostClassification::Estimated)
      ++summary.estimated_count;
    if (operation.cost_classification == CostClassification::UsageOnly)
      ++summary.usage_only_count;
    if (operation.cost_classification == CostClassification::Unavailable)
      ++summary.unavailable_count;
    add_currency_total(summary.currency_totals, operation);
  }
  std::sort(summary.currency_totals.begin(), summary.currency_totals.end(),
            [](const auto& left, const auto& right) { return left.currency < right.currency; });
  return summary;
}

} // namespace singularity::usage
