#include "Bridge/UsageBridge/UsageBridge.hpp"

#include "Agent/AgentStore/AgentStore.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Usage/UsageQueries/UsageQueries.hpp"
#include "Usage/UsageStatement/UsageStatement.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace singularity::bridge::usage_bridge {
namespace {

using Json = nlohmann::json;

Json usage_json(const providers::UsageMetadata& usage) {
  return Json{{"inputTokens", usage.input_tokens ? Json(*usage.input_tokens) : Json(nullptr)},
              {"outputTokens", usage.output_tokens ? Json(*usage.output_tokens) : Json(nullptr)},
              {"totalTokens", usage.total_tokens ? Json(*usage.total_tokens) : Json(nullptr)},
              {"costMicros", usage.cost_micros ? Json(*usage.cost_micros) : Json(nullptr)},
              {"costCurrency", usage.cost_currency ? Json(*usage.cost_currency) : Json(nullptr)},
              {"costClassification",
               usage.cost_classification ? Json(*usage.cost_classification) : Json(nullptr)},
              {"costMethodology",
               usage.cost_methodology ? Json(*usage.cost_methodology) : Json(nullptr)}};
}

Json optional_string_json(const std::optional<std::string>& value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

Json optional_integer_json(const std::optional<long long>& value) {
  return value.has_value() ? Json(*value) : Json(nullptr);
}

Json currency_totals_json(const std::vector<usage::CurrencyTotal>& totals) {
  Json result = Json::array();
  for (const usage::CurrencyTotal& total : totals) {
    result.push_back({
        {"currency", total.currency},
        {"actualMicros", optional_integer_json(total.actual_micros)},
        {"actualDisplay", total.actual_micros
                              ? Json(usage::format_cost(*total.actual_micros, total.currency))
                              : Json(nullptr)},
        {"estimatedMicros", optional_integer_json(total.estimated_micros)},
        {"estimatedDisplay", total.estimated_micros
                                 ? Json(usage::format_cost(*total.estimated_micros, total.currency))
                                 : Json(nullptr)},
    });
  }
  return result;
}

// One model call's evidence as the bill shows it (D-252): fingerprints, the
// provider's own id for the request, and its own tokens and cost.
Json billed_call_json(const usage::BilledCall& call) {
  const auto text = [](const std::string& value) { return value.empty() ? Json(nullptr) : Json(value); };
  return Json{{"time", call.time},
              {"providerId", call.provider_id},
              {"modelId", call.model_id},
              {"servedBy", text(call.served_by)},
              {"providerRequestId", text(call.provider_request_id)},
              {"payloadSha256", text(call.payload_sha256)},
              {"responseSha256", text(call.response_sha256)},
              {"inputTokens", optional_integer_json(call.input_tokens)},
              {"cachedInputTokens", optional_integer_json(call.cached_input_tokens)},
              {"outputTokens", optional_integer_json(call.output_tokens)},
              {"reasoningTokens", optional_integer_json(call.reasoning_tokens)},
              {"costMicros", optional_integer_json(call.cost_micros)},
              {"costCurrency", text(call.cost_currency)},
              {"costClassification", text(call.cost_classification)},
              {"costMethodology", text(call.cost_methodology)},
              {"protectionMode", call.protection_mode.empty() ? std::string("off") : call.protection_mode},
              {"recorded", call.record_chain.empty() == false && call.record_index.has_value()},
              {"confirmation", call.confirmation.has_value()
                                   ? Json{{"source", call.confirmation->source},
                                          {"costMicros", optional_integer_json(call.confirmation->cost_micros)},
                                          {"currency", text(call.confirmation->currency)},
                                          {"provider", text(call.confirmation->provider)},
                                          {"confirmedAt", text(call.confirmation->confirmed_at)}}
                                   : Json(nullptr)}};
}

// The AI colleagues of this computer by id, with their names: the bill says
// whose work each request was. A colleague no longer here keeps the name its
// own work recorded, if any.
using AgentNames = std::map<std::string, std::string>;

AgentNames agent_names_of(const usage::UsageSummary& summary) {
  AgentNames names;
  for (const usage::OperationUsageEvidence& operation : summary.operations) {
    if (operation.agent_id.empty() == false && operation.agent_name.empty() == false)
      names[operation.agent_id] = operation.agent_name;
  }
  bool isAnyAgent = false;
  for (const usage::OperationUsageEvidence& operation : summary.operations)
    isAnyAgent = isAnyAgent || operation.agent_id.empty() == false;
  if (isAnyAgent == false) return names;
  try {
    const std::lock_guard<std::mutex> lock(agents::agent_store_mutex());
    for (const agents::Agent& agent : agents::load_agents()) {
      if (agent.name.empty() == false) names[agent.id] = agent.name;
    }
  } catch (const std::exception&) {
    // The names the operations recorded stand.
  }
  return names;
}

Json operation_json(const usage::OperationUsageEvidence& operation, const AgentNames& names) {
  Json calls = Json::array();
  for (const usage::BilledCall& call : operation.calls) calls.push_back(billed_call_json(call));
  const auto named = names.find(operation.agent_id);
  return Json{
      {"agentId", operation.agent_id.empty() ? Json(nullptr) : Json(operation.agent_id)},
      {"agentName", named != names.end() ? Json(named->second) : Json(nullptr)},
      {"agentInitiated", operation.agent_initiated},
      {"askedBy", operation.asked_by_actor_id.empty()
                      ? Json(nullptr)
                      : Json{{"actorId", operation.asked_by_actor_id}, {"name", operation.asked_by_name}}},
      {"calls", std::move(calls)},
      {"callsRecorded", operation.calls_recorded},
      {"projectId", operation.project_id},
      {"operationId", operation.operation_id},
      {"taskId", operation.task_id},
      {"attempt", operation.attempt},
      {"time", operation.timestamp},
      {"providerId", optional_string_json(operation.provider_id)},
      {"modelId", optional_string_json(operation.model_id)},
      {"executionKind", operation.execution_kind},
      {"operationType", operation.operation_type},
      {"operationRole", operation.operation_role},
      {"operationStatus", operation.operation_status},
      {"chatId", optional_string_json(operation.chat_id)},
      {"consiliumId", optional_string_json(operation.consilium_id)},
      {"workflowRunId", optional_string_json(operation.workflow_run_id)},
      {"workProductIds", operation.work_product_ids},
      {"privacyMode", operation.privacy_mode},
      {"protectedExternal", operation.protected_external},
      {"protectionRecorded", operation.protection_recorded},
      {"usage", usage_json(operation.usage)},
      {"costClassification", usage::to_string(operation.cost_classification)},
      // An amount is shown as the operation's cost only when it is one: a
      // partial total (some calls came back unpriced) is a smaller bill than
      // the real one, and is left as usage without a stated cost.
      {"costDisplay", operation.usage.cost_micros && operation.usage.cost_currency &&
                              (operation.cost_classification == usage::CostClassification::Actual ||
                               operation.cost_classification == usage::CostClassification::Estimated)
                          ? Json(usage::format_cost(*operation.usage.cost_micros,
                                                  *operation.usage.cost_currency))
                          : Json(nullptr)},
      {"evidenceStatus", operation.evidence_status},
  };
}

struct BreakdownAccumulator {
  long long operation_count = 0;
  long long input_tokens = 0;
  long long output_tokens = 0;
  long long total_tokens = 0;
  long long actual_count = 0;
  long long estimated_count = 0;
  long long usage_only_count = 0;
  long long unavailable_count = 0;
  std::vector<usage::CurrencyTotal> currency_totals;
};

void add_to_breakdown(std::map<std::string, BreakdownAccumulator>& breakdown,
                      const std::string& id,
                      const usage::OperationUsageEvidence& operation) {
  BreakdownAccumulator& value = breakdown[id];
  ++value.operation_count;
  value.input_tokens += operation.usage.input_tokens.value_or(0);
  value.output_tokens += operation.usage.output_tokens.value_or(0);
  value.total_tokens += operation.usage.total_tokens.value_or(0);
  if (operation.cost_classification == usage::CostClassification::Actual) ++value.actual_count;
  if (operation.cost_classification == usage::CostClassification::Estimated) {
    ++value.estimated_count;
  }
  if (operation.cost_classification == usage::CostClassification::UsageOnly) {
    ++value.usage_only_count;
  }
  if (operation.cost_classification == usage::CostClassification::Unavailable) {
    ++value.unavailable_count;
  }

  if (!operation.usage.cost_micros || !operation.usage.cost_currency ||
      (operation.cost_classification != usage::CostClassification::Actual &&
       operation.cost_classification != usage::CostClassification::Estimated)) {
    return;
  }
  auto found = std::find_if(value.currency_totals.begin(), value.currency_totals.end(),
                            [&](const auto& total) {
                              return total.currency == *operation.usage.cost_currency;
                            });
  if (found == value.currency_totals.end()) {
    value.currency_totals.push_back(
        {*operation.usage.cost_currency, std::nullopt, std::nullopt});
    found = std::prev(value.currency_totals.end());
  }
  if (operation.cost_classification == usage::CostClassification::Actual) {
    found->actual_micros = found->actual_micros.value_or(0) + *operation.usage.cost_micros;
  } else {
    found->estimated_micros =
        found->estimated_micros.value_or(0) + *operation.usage.cost_micros;
  }
}

Json breakdown_json(const std::map<std::string, BreakdownAccumulator>& values) {
  Json result = Json::array();
  for (const auto& [id, value] : values) {
    auto totals = value.currency_totals;
    std::sort(totals.begin(), totals.end(),
              [](const auto& left, const auto& right) { return left.currency < right.currency; });
    result.push_back({
        {"id", id},
        {"operationCount", value.operation_count},
        {"tokens", {{"input", value.input_tokens},
                    {"output", value.output_tokens},
                    {"total", value.total_tokens}}},
        {"classificationCounts", {{"actual", value.actual_count},
                                  {"estimated", value.estimated_count},
                                  {"usageOnly", value.usage_only_count},
                                  {"unavailable", value.unavailable_count}}},
        {"currencyTotals", currency_totals_json(totals)},
    });
  }
  return result;
}

Json breakdowns_json(const usage::UsageSummary& summary) {
  std::map<std::string, BreakdownAccumulator> projects;
  std::map<std::string, BreakdownAccumulator> providers;
  std::map<std::string, BreakdownAccumulator> models;
  std::map<std::string, BreakdownAccumulator> chats;
  std::map<std::string, BreakdownAccumulator> consilia;
  std::map<std::string, BreakdownAccumulator> workflows;
  std::map<std::string, BreakdownAccumulator> work_products;
  std::map<std::string, BreakdownAccumulator> colleagues;
  for (const usage::OperationUsageEvidence& operation : summary.operations) {
    add_to_breakdown(projects, operation.project_id, operation);
    if (operation.agent_id.empty() == false) add_to_breakdown(colleagues, operation.agent_id, operation);
    if (operation.provider_id) add_to_breakdown(providers, *operation.provider_id, operation);
    if (operation.model_id) add_to_breakdown(models, *operation.model_id, operation);
    if (operation.chat_id) add_to_breakdown(chats, *operation.chat_id, operation);
    if (operation.consilium_id) add_to_breakdown(consilia, *operation.consilium_id, operation);
    if (operation.workflow_run_id) {
      add_to_breakdown(workflows, *operation.workflow_run_id, operation);
    }
    for (const std::string& work_product_id : operation.work_product_ids) {
      add_to_breakdown(work_products, work_product_id, operation);
    }
  }
  return {{"projects", breakdown_json(projects)},
          {"providers", breakdown_json(providers)},
          {"models", breakdown_json(models)},
          {"chats", breakdown_json(chats)},
          {"consilia", breakdown_json(consilia)},
          {"workflows", breakdown_json(workflows)},
          {"workProducts", breakdown_json(work_products)},
          // By the AI colleague whose work each request was (D-252).
          {"agents", breakdown_json(colleagues)}};
}

Json summary_json(const usage::UsageSummary& summary) {
  const AgentNames names = agent_names_of(summary);
  Json operations = Json::array();
  for (const usage::OperationUsageEvidence& operation : summary.operations) {
    operations.push_back(operation_json(operation, names));
  }
  return Json{
      {"period", {{"start", summary.period.start},
                  {"end", summary.period.end},
                  {"endExclusive", true}}},
      {"operationCount", summary.operation_count},
      {"tokens", {{"input", summary.input_tokens},
                  {"output", summary.output_tokens},
                  {"total", summary.total_tokens}}},
      {"classificationCounts", {{"actual", summary.actual_count},
                                {"estimated", summary.estimated_count},
                                {"usageOnly", summary.usage_only_count},
                                {"unavailable", summary.unavailable_count}}},
      {"currencyTotals", currency_totals_json(summary.currency_totals)},
      {"breakdowns", breakdowns_json(summary)},
      {"operations", std::move(operations)},
  };
}

usage::UsagePeriod period_from_params(const Json& params) {
  if (!params.contains("periodStart") || !params.at("periodStart").is_string() ||
      !params.contains("periodEnd") || !params.at("periodEnd").is_string()) {
    throw std::runtime_error("Choose a usage period.");
  }
  return usage::validate_period(params.at("periodStart").get<std::string>(),
                                params.at("periodEnd").get<std::string>());
}

std::optional<std::string> project_id_from_params(const Json& params, bool is_required) {
  if (params.contains("projectId") && params.at("projectId").is_string()) {
    const std::string project_id = params.at("projectId").get<std::string>();
    if (!project_id.empty()) return project_id;
  }
  if (is_required) throw std::runtime_error("Choose a Project for this statement.");
  return std::nullopt;
}

Json privacy_metadata_json(const protected_api::papinian::PrivacyMetadata& metadata) {
  return Json{{"mode", metadata.mode},
              {"papinianGuardVersion", metadata.papinian_guard_version},
              {"papinianNerVersion", metadata.papinian_ner_version},
              {"protectedCount", metadata.protected_count},
              {"categoryCounts", metadata.category_counts},
              {"responseIntegrity", metadata.response_integrity}};
}

Json entry_json(const routing::RoutingLogEntry& entry) {
  return Json{
      {"matterId", entry.matter_id},
      {"operationId", entry.operation_id},
      {"taskId", entry.task_id},
      {"attempt", entry.attempt},
      {"time", entry.time},
      {"providerId", entry.provider_id ? Json(*entry.provider_id) : Json(nullptr)},
      {"modelId", entry.model_id ? Json(*entry.model_id) : Json(nullptr)},
      {"executionKind", entry.execution_kind},
      {"legalTaskPresetId", entry.legal_task_preset_id},
      {"legalTaskPresetVersion", entry.legal_task_preset_version},
      {"documentCount", entry.document_count},
      {"chunkCount", entry.chunk_count},
      {"processing", {{"version", entry.processing.version},
                      {"rule", entry.processing.rule},
                      {"localOperationUnits", entry.processing.local_operation_units},
                      {"externalOperationUnits", entry.processing.external_operation_units}}},
      {"privacyMetadata", privacy_metadata_json(entry.privacy_metadata)},
      {"status", entry.success ? "success" : "error"},
      {"latencyMs", entry.latency_ms},
      {"usage", usage_json(entry.usage)},
      {"finishReason", entry.finish_reason ? Json(*entry.finish_reason) : Json(nullptr)},
      {"errorCode", entry.error_code ? Json(providers::to_string(*entry.error_code)) : Json(nullptr)},
      {"routingReason", entry.routing_reason ? Json(*entry.routing_reason) : Json(nullptr)},
  };
}

} // namespace

nlohmann::json list(const nlohmann::json&) {
  Json result = Json::array();
  for (const auto& entry : usage::list_recorded_usage()) result.push_back(entry_json(entry));
  return result;
}

nlohmann::json get_summary(const nlohmann::json& params) {
  const usage::UsagePeriod period = period_from_params(params);
  return summary_json(usage::get_usage_summary(period, project_id_from_params(params, false)));
}

PreparedStatement prepare_statement(const nlohmann::json& params) {
  const usage::UsagePeriod period = period_from_params(params);
  const std::optional<std::string> project_id = project_id_from_params(params, true);
  if (!params.contains("structuredFormat") || !params.at("structuredFormat").is_string()) {
    throw std::runtime_error("Choose JSON or CSV for the structured statement.");
  }
  PreparedStatement prepared;
  prepared.structured_format = usage::structured_statement_format_from_string(
      params.at("structuredFormat").get<std::string>());
  prepared.statement = usage::build_statement(
      usage::get_usage_summary(period, project_id), *project_id, workspaces::current_timestamp());
  return prepared;
}

nlohmann::json finish_statement(
    const PreparedStatement& prepared,
    const std::optional<std::string>& destination_directory) {
  if (!destination_directory) return Json{{"generated", false}};
  const usage::StatementBundleResult result = usage::write_statement_bundle(
      std::filesystem::path(*destination_directory), prepared.statement, prepared.structured_format);
  return Json{{"generated", true},
              {"pdfPath", result.pdf_path.u8string()},
              {"structuredPath", result.structured_path.u8string()},
              {"structuredFormat", result.structured_format},
              {"generatedAt", result.generated_at},
              {"contentHashAlgorithm", "sha256"},
              {"contentHash", result.content_hash},
              {"operationCount", result.operation_count}};
}

PreparedWorkStatement prepare_work_statement(const nlohmann::json& params) {
  const usage::UsagePeriod period = period_from_params(params);
  const std::optional<std::string> project_id = project_id_from_params(params, true);
  const usage::UsageSummary summary = usage::get_usage_summary(period, project_id);
  usage::WorkStatementNames names;
  names.matter_id = *project_id;
  names.period_name = params.contains("periodName") && params.at("periodName").is_string()
                          ? params.at("periodName").get<std::string>()
                          : period.start.substr(0, 7);
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const std::optional<matters::Matter> matter = matters::load_matter(node::require_active_workspace_path(), *project_id);
    if (matter.has_value() == false) throw std::runtime_error("This Matter is no longer available.");
    names.matter_name = matter->name;
    for (const matters::MatterChat& chat : matter->chats) names.threads[chat.id] = chat.title;
  }
  const platform::AppSettings settings = platform::load_app_settings();
  if (const std::optional<platform::DisplayPerson> person = platform::display_person(settings);
      person.has_value() && person->organisation_name.has_value())
    names.firm = *person->organisation_name;
  try {
    const std::lock_guard<std::mutex> lock(agents::agent_store_mutex());
    for (const agents::Agent& agent : agents::load_agents()) {
      const std::string title = agent.office.title.empty() == false ? agent.office.title : agent.office.ranks_with;
      names.colleagues[agent.id] = {agent.name, title};
    }
  } catch (const std::exception&) {
    // The names the operations recorded stand.
  }
  const std::string public_key = platform::device_public_key();
  PreparedWorkStatement prepared;
  const std::string key_code = platform::device_key_code(public_key);
  usage::WorkRecordExcerpt record;
  if (usage::work_record::Chain* chain = usage::work_record::this_computer())
    record = usage::work_record::excerpt_for(*chain, key_code, summary);
  prepared.signed_file = usage::signed_work_statement(
      usage::build_work_statement(summary, names, workspaces::current_timestamp(), record), public_key,
      platform::device_key_code(public_key), [](const std::string& bytes) { return platform::sign_with_device_key(bytes); });
  return prepared;
}

nlohmann::json finish_work_statement(const PreparedWorkStatement& prepared, const std::optional<std::string>& folder) {
  if (folder.has_value() == false) return Json{{"saved", false}};
  const usage::WorkStatementFiles files =
      usage::write_work_statement(std::filesystem::u8path(*folder), prepared.signed_file);
  return Json{{"saved", true},
              {"pdfPath", files.pdf_path.u8string()},
              {"signedPath", files.signed_path.u8string()},
              {"statementSha256", files.statement_sha256},
              {"keyCode", prepared.signed_file.at("signature").at("keyCode")},
              {"requestCount", files.request_count}};
}

// ---- The provider's confirmation and this computer's work record (D-252) ----------

namespace {

// A call of the Matter OpenRouter keeps a record of, not confirmed yet.
struct Unconfirmed {
  std::string task_id;
  std::string payload_sha256;
  std::string request_id;
  std::string provider_id;
};

// Legal Machines' timestamp of the chain's head, from its fingerprint alone.
std::optional<usage::work_record::Stamp> stamp_of(const usage::work_record::Entry& head) {
  try {
    const platform::HttpResponse response = platform::https_post(
        std::string(node::kLegalMachinesApiBase) + "/timestamp", {{"Content-Type", "application/json"}},
        Json{{"sha256", head.hash}}.dump(), 15);
    if (response.status != 200) return std::nullopt;
    const Json body = Json::parse(response.body, nullptr, false);
    if (body.is_object() == false || body.value("sha256", std::string()) != head.hash) return std::nullopt;
    usage::work_record::Stamp stamp;
    stamp.index = head.index;
    stamp.hash = head.hash;
    stamp.time = body.value("time", std::string());
    stamp.key_id = body.value("keyId", std::string());
    stamp.signature = body.value("signature", std::string());
    if (stamp.time.empty() || stamp.key_id.empty() || stamp.signature.empty()) return std::nullopt;
    return stamp;
  } catch (...) {
    return std::nullopt;
  }
}

bool stamp_head() {
  usage::work_record::Chain* chain = usage::work_record::this_computer();
  if (chain == nullptr || chain->needs_stamp() == false) return false;
  const std::optional<usage::work_record::Entry> head = chain->head();
  if (head.has_value() == false) return false;
  const std::optional<usage::work_record::Stamp> stamp = stamp_of(*head);
  return stamp.has_value() && chain->add_stamp(*stamp);
}

}  // namespace

nlohmann::json confirm_costs(const nlohmann::json& params) {
  const usage::UsagePeriod period = period_from_params(params);
  const std::optional<std::string> project_id = project_id_from_params(params, true);
  const usage::UsageSummary summary = usage::get_usage_summary(period, project_id);
  std::vector<Unconfirmed> asked;
  for (const usage::OperationUsageEvidence& operation : summary.operations) {
    for (const usage::BilledCall& call : operation.calls) {
      if (call.confirmation.has_value() || call.provider_request_id.rfind("gen-", 0) != 0) continue;
      asked.push_back(Unconfirmed{operation.task_id, call.payload_sha256, call.provider_request_id, call.provider_id});
      if (asked.size() >= 200) break;
    }
    if (asked.size() >= 200) break;
  }
  // Each OpenRouter connection's address and the firm's key for it, on this
  // computer only (AG-SEC-1): the key goes to OpenRouter and nowhere else.
  std::map<std::string, std::pair<std::string, std::string>> accounts;
  {
    const platform::AppSettings settings = platform::load_app_settings();
    for (const platform::ConnectedProvider& provider : settings.local_node_setup.connected_providers) {
      const std::string root = provider.base_url.value_or(std::string());
      if (providers::is_openrouter_root(root) == false) continue;
      const std::optional<std::string> key = platform::get_provider_credential(provider.id);
      if (key.has_value() && key->empty() == false) accounts[provider.id] = {root, *key};
    }
  }
  std::vector<std::pair<Unconfirmed, std::future<std::optional<providers::GenerationRecord>>>> lookups;
  std::size_t next = 0;
  std::map<std::string, protected_api::papinian::CostConfirmation> found;
  const std::string now = workspaces::current_timestamp();
  while (next < asked.size() || lookups.empty() == false) {
    // Four at a time.
    while (next < asked.size() && lookups.size() < 4) {
      const Unconfirmed call = asked[next++];
      const auto account = accounts.find(call.provider_id);
      if (account == accounts.end()) continue;
      const std::string root = account->second.first;
      const std::string key = account->second.second;
      const std::string id = call.request_id;
      lookups.push_back({call, std::async(std::launch::async, [root, key, id]() {
                           return providers::openrouter_generation(key, root, id);
                         })});
    }
    if (lookups.empty()) break;
    auto [call, lookup] = std::move(lookups.front());
    lookups.erase(lookups.begin());
    const std::optional<providers::GenerationRecord> record = lookup.get();
    if (record.has_value() == false) continue;
    protected_api::papinian::CostConfirmation confirmation;
    confirmation.source = "openrouter";
    confirmation.cost_micros = record->cost_micros;
    confirmation.currency = "USD";
    confirmation.input_tokens = record->input_tokens;
    confirmation.output_tokens = record->output_tokens;
    confirmation.provider = record->provider;
    confirmation.model = record->model;
    confirmation.created_at = record->created_at;
    confirmation.confirmed_at = now;
    confirmation.record_sha256 = record->record_sha256;
    found[call.task_id + "\n" + call.payload_sha256 + "\n" + call.request_id] = std::move(confirmation);
  }
  long long confirmed = 0;
  if (found.empty() == false) {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const std::filesystem::path workspace = node::require_active_workspace_path();
    std::optional<matters::Matter> matter = matters::load_matter(workspace, *project_id);
    if (matter.has_value()) {
      for (matters::MatterTask& task : matter->tasks) {
        // The calls its record keeps for its bill, this run's among them.
        for (const protected_api::papinian::OutboundCall& call : task.privacy_metadata.outbound_calls) {
          if (call.kind != "model") continue;
          const bool isKept = std::any_of(task.billed_calls.begin(), task.billed_calls.end(), [&](const auto& kept) {
            return kept.time == call.time && kept.payload_sha256 == call.payload_sha256 &&
                   kept.provider_request_id == call.provider_request_id;
          });
          if (isKept == false) task.billed_calls.push_back(call);
        }
        for (protected_api::papinian::OutboundCall& call : task.billed_calls) {
          const auto confirmation = found.find(task.id + "\n" + call.payload_sha256 + "\n" + call.provider_request_id);
          if (confirmation == found.end() || call.confirmation.has_value()) continue;
          call.confirmation = confirmation->second;
          ++confirmed;
        }
      }
      if (confirmed > 0) matters::save_matter(workspace, *matter);
    }
  }
  const bool stamped = stamp_head();
  return Json{{"confirmed", confirmed},
              {"waiting", static_cast<long long>(asked.size()) - confirmed},
              {"stamped", stamped}};
}

void start_work_record() {
  static std::once_flag once;
  std::call_once(once, [] {
    try {
      usage::work_record::install(platform::app_settings_directory() / "Work record");
    } catch (...) {
      return;
    }
    std::thread([] {
      int seconds = 0;
      while (platform::lifecycle::is_closing() == false) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++seconds < 30) continue;
        seconds = 0;
        usage::work_record::Chain* chain = usage::work_record::this_computer();
        if (chain == nullptr || chain->needs_stamp() == false) continue;
        const auto written = chain->last_written();
        if (written.has_value() && std::chrono::steady_clock::now() - *written < std::chrono::seconds(10)) continue;
        stamp_head();
      }
    }).detach();
  });
}

} // namespace singularity::bridge::usage_bridge
