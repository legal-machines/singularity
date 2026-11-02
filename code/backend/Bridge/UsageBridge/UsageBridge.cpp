#include "Bridge/UsageBridge/UsageBridge.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Usage/UsageQueries/UsageQueries.hpp"
#include "Usage/UsageStatement/UsageStatement.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

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

Json operation_json(const usage::OperationUsageEvidence& operation) {
  return Json{
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
  for (const usage::OperationUsageEvidence& operation : summary.operations) {
    add_to_breakdown(projects, operation.project_id, operation);
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
          {"workProducts", breakdown_json(work_products)}};
}

Json summary_json(const usage::UsageSummary& summary) {
  Json operations = Json::array();
  for (const usage::OperationUsageEvidence& operation : summary.operations) {
    operations.push_back(operation_json(operation));
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

} // namespace singularity::bridge::usage_bridge
