#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Routing/RoutingLog/OutboundRecord.hpp"
#include "Routing/RoutingLog/SentPassagesFolder.hpp"

#include "Platform/AtomicFile/AtomicFile.hpp"

#include <mutex>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <iterator>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace singularity::routing {
namespace {

using Json = nlohmann::json;

std::filesystem::path previous_path(const std::filesystem::path &path) {
  return path.string() + ".previous";
}

std::filesystem::path unique_temporary_path(const std::filesystem::path &path) {
  std::random_device seed;
  std::mt19937_64 generator(seed());
  return path.string() + ".tmp-" + std::to_string(generator());
}

// The routing log and the passages kept beside it are written the way every
// file Singularity writes is (Platform/AtomicFile): the bytes on the disk before the
// rename that puts them in place, a full disk refused as DiskFull in plain
// words before anything is begun, and no path in any message.
void atomic_replace(const std::filesystem::path &source, const std::filesystem::path &destination) {
  platform::commit_staged_file(source, destination, platform::StagedCommitMode::ReplaceExisting, false);
}

Json read_json_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Singularity could not read its record of AI requests.");
  }
  return Json::parse(file);
}

void write_json_file(const std::filesystem::path &path, const std::string &text) {
  platform::write_file_durably(path, text);
}

Json usage_to_json(const providers::UsageMetadata &usage) {
  Json value{
      {"inputTokens", usage.input_tokens ? Json(*usage.input_tokens) : Json(nullptr)},
      {"outputTokens", usage.output_tokens ? Json(*usage.output_tokens) : Json(nullptr)},
      {"totalTokens", usage.total_tokens ? Json(*usage.total_tokens) : Json(nullptr)},
      {"costMicros", usage.cost_micros ? Json(*usage.cost_micros) : Json(nullptr)},
      {"costCurrency", usage.cost_currency ? Json(*usage.cost_currency) : Json(nullptr)},
      {"costClassification",
       usage.cost_classification ? Json(*usage.cost_classification) : Json(nullptr)},
      {"costMethodology", usage.cost_methodology ? Json(*usage.cost_methodology) : Json(nullptr)},
      {"servedBy", usage.served_by ? Json(*usage.served_by) : Json(nullptr)},
  };
  // Written only where the provider said, so every other entry reads as before.
  if (usage.reasoning_tokens) value["reasoningTokens"] = *usage.reasoning_tokens;
  if (usage.cached_input_tokens) value["cachedInputTokens"] = *usage.cached_input_tokens;
  if (usage.cache_write_tokens) value["cacheWriteTokens"] = *usage.cache_write_tokens;
  return value;
}

providers::UsageMetadata usage_from_json(const Json &value) {
  providers::UsageMetadata usage;
  if (!value.is_object()) return usage;
  if (value.contains("inputTokens") && value.at("inputTokens").is_number_integer()) {
    usage.input_tokens = value.at("inputTokens").get<long long>();
  }
  if (value.contains("outputTokens") && value.at("outputTokens").is_number_integer()) {
    usage.output_tokens = value.at("outputTokens").get<long long>();
  }
  if (value.contains("totalTokens") && value.at("totalTokens").is_number_integer()) {
    usage.total_tokens = value.at("totalTokens").get<long long>();
  }
  if (value.contains("costMicros") && value.at("costMicros").is_number_integer()) {
    usage.cost_micros = value.at("costMicros").get<long long>();
  }
  if (value.contains("costCurrency") && value.at("costCurrency").is_string()) {
    usage.cost_currency = value.at("costCurrency").get<std::string>();
  }
  if (value.contains("costClassification") && value.at("costClassification").is_string()) {
    usage.cost_classification = value.at("costClassification").get<std::string>();
  }
  if (value.contains("costMethodology") && value.at("costMethodology").is_string()) {
    usage.cost_methodology = value.at("costMethodology").get<std::string>();
  }
  if (value.contains("servedBy") && value.at("servedBy").is_string()) {
    usage.served_by = value.at("servedBy").get<std::string>();
  }
  if (value.contains("reasoningTokens") && value.at("reasoningTokens").is_number_integer()) {
    usage.reasoning_tokens = value.at("reasoningTokens").get<long long>();
  }
  if (value.contains("cachedInputTokens") && value.at("cachedInputTokens").is_number_integer()) {
    usage.cached_input_tokens = value.at("cachedInputTokens").get<long long>();
  }
  if (value.contains("cacheWriteTokens") && value.at("cacheWriteTokens").is_number_integer()) {
    usage.cache_write_tokens = value.at("cacheWriteTokens").get<long long>();
  }
  return usage;
}

// One relation a call carried or held back, with its local provenance
// (PrivacyMetadata.hpp: OutboundRelation). Markers, closed words, digests and
// offsets only - never a value a marker stands for.
Json relation_source_to_json(const protected_api::papinian::RelationSource &source) {
	Json value{{"marker", source.marker}, {"origin", source.origin}};
	if (source.passage_sha256.empty() == false) {
		value["passageSha256"] = source.passage_sha256;
		value["offset"] = source.offset;
		value["length"] = source.length;
	}
	if (source.cell.empty() == false) value["cell"] = source.cell;
	return value;
}

protected_api::papinian::RelationSource relation_source_from_json(const Json &value) {
	protected_api::papinian::RelationSource source;
	if (value.is_object() == false) return source;
	source.marker = value.value("marker", std::string());
	source.origin = value.value("origin", std::string());
	source.passage_sha256 = value.value("passageSha256", std::string());
	if (value.contains("offset") && value.at("offset").is_number_unsigned()) source.offset = value.at("offset").get<std::size_t>();
	if (value.contains("length") && value.at("length").is_number_unsigned()) source.length = value.at("length").get<std::size_t>();
	source.cell = value.value("cell", std::string());
	return source;
}

Json relation_to_json(const protected_api::papinian::OutboundRelation &relation) {
	Json sources = Json::array();
	for (const protected_api::papinian::RelationSource &source : relation.sources) sources.push_back(relation_source_to_json(source));
	Json value{
		{"id", relation.id},
		{"kind", relation.kind},
		{"sent", relation.sent},
		{"operands", relation.operands},
		{"results", relation.results},
		{"sources", std::move(sources)},
		{"derivation", relation.derivation},
		{"valueSensitivity", relation.value_sensitivity},
		{"relationSensitivity", relation.relation_sensitivity},
		{"decision", relation.decision},
		{"justification", relation.justification},
		{"requestedBy", relation.requested_by},
		{"subproblem", relation.subproblem},
	};
	if (relation.business_day_rule.empty() == false) {
		value["businessDayRule"] = relation.business_day_rule;
		if (relation.business_day_rule_source.passage_sha256.empty() == false)
			value["businessDayRuleSource"] = relation_source_to_json(relation.business_day_rule_source);
	}
	return value;
}

protected_api::papinian::OutboundRelation relation_from_json(const Json &value) {
	protected_api::papinian::OutboundRelation relation;
	if (value.is_object() == false) return relation;
	relation.id = value.value("id", std::string());
	relation.kind = value.value("kind", std::string());
	relation.sent = value.value("sent", std::string());
	const auto strings = [&](const char *key) {
		std::vector<std::string> list;
		if (value.contains(key) && value.at(key).is_array())
			for (const Json &entry : value.at(key))
				if (entry.is_string()) list.push_back(entry.get<std::string>());
		return list;
	};
	relation.operands = strings("operands");
	relation.results = strings("results");
	if (value.contains("sources") && value.at("sources").is_array())
		for (const Json &entry : value.at("sources")) relation.sources.push_back(relation_source_from_json(entry));
	relation.derivation = value.value("derivation", std::string());
	relation.business_day_rule = value.value("businessDayRule", std::string());
	if (value.contains("businessDayRuleSource"))
		relation.business_day_rule_source = relation_source_from_json(value.at("businessDayRuleSource"));
	relation.value_sensitivity = value.value("valueSensitivity", std::string());
	relation.relation_sensitivity = value.value("relationSensitivity", std::string("ordinary"));
	relation.decision = value.value("decision", std::string("sent"));
	relation.justification = value.value("justification", std::string());
	relation.requested_by = value.value("requestedBy", std::string("planner"));
	relation.subproblem = value.value("subproblem", std::string());
	return relation;
}

// The protected computation protocol's part of a call (DESIGN4 §11.1): role
// lines and table blocks exactly as they left, and the operations the model
// asked for and was answered - symbols, codes and closed words, where facts
// are stated; never a value.
Json role_to_json(const protected_api::papinian::OutboundRole &role) {
	return Json{
		{"symbol", role.symbol},
		{"line", role.line},
		{"source", role.source},
		{"roleClass", role.role_class},
		{"valueSensitivity", role.value_sensitivity},
		{"relationSensitivity", role.relation_sensitivity},
		{"withheldByPolicy", role.withheld_by_policy},
	};
}

protected_api::papinian::OutboundRole role_from_json(const Json &value) {
	protected_api::papinian::OutboundRole role;
	if (value.is_object() == false) return role;
	role.symbol = value.value("symbol", std::string());
	role.line = value.value("line", std::string());
	role.source = value.value("source", std::string());
	role.role_class = value.value("roleClass", std::string());
	role.value_sensitivity = value.value("valueSensitivity", std::string());
	role.relation_sensitivity = value.value("relationSensitivity", std::string("ordinary"));
	role.withheld_by_policy = value.value("withheldByPolicy", false);
	return role;
}

Json table_to_json(const protected_api::papinian::OutboundTable &table) {
	Json cells = Json::array();
	for (const protected_api::papinian::RelationSource &cell : table.disclosed_cells) cells.push_back(relation_source_to_json(cell));
	return Json{
		{"handle", table.handle},
		{"block", table.block},
		{"disclosedCells", std::move(cells)},
		{"requestedByOp", table.requested_by_op},
		{"justification", table.justification},
	};
}

protected_api::papinian::OutboundTable table_from_json(const Json &value) {
	protected_api::papinian::OutboundTable table;
	if (value.is_object() == false) return table;
	table.handle = value.value("handle", std::string());
	table.block = value.value("block", std::string());
	if (value.contains("disclosedCells") && value.at("disclosedCells").is_array())
		for (const Json &entry : value.at("disclosedCells")) table.disclosed_cells.push_back(relation_source_from_json(entry));
	table.requested_by_op = value.value("requestedByOp", std::string());
	table.justification = value.value("justification", std::string());
	return table;
}

Json operation_to_json(const protected_api::papinian::OutboundOperation &operation) {
	Json sources = Json::array();
	for (const protected_api::papinian::RelationSource &source : operation.sources) sources.push_back(relation_source_to_json(source));
	return Json{
		{"opId", operation.op_id},
		{"requested", operation.requested},
		{"askedInReplyTo", operation.asked_in_reply_to},
		{"status", operation.status},
		{"code", operation.code},
		{"returned", operation.returned},
		{"results", operation.results},
		{"observation", operation.observation},
		{"sources", std::move(sources)},
		{"justification", operation.justification},
		{"valueSensitivity", operation.value_sensitivity},
		{"relationSensitivity", operation.relation_sensitivity},
	};
}

protected_api::papinian::OutboundOperation operation_from_json(const Json &value) {
	protected_api::papinian::OutboundOperation operation;
	if (value.is_object() == false) return operation;
	operation.op_id = value.value("opId", std::string());
	operation.requested = value.value("requested", std::string());
	operation.asked_in_reply_to = value.value("askedInReplyTo", std::string());
	operation.status = value.value("status", std::string());
	operation.code = value.value("code", std::string());
	operation.returned = value.value("returned", std::string());
	if (value.contains("results") && value.at("results").is_array())
		for (const Json &entry : value.at("results"))
			if (entry.is_string()) operation.results.push_back(entry.get<std::string>());
	operation.observation = value.value("observation", false);
	if (value.contains("sources") && value.at("sources").is_array())
		for (const Json &entry : value.at("sources")) operation.sources.push_back(relation_source_from_json(entry));
	operation.justification = value.value("justification", std::string());
	operation.value_sensitivity = value.value("valueSensitivity", std::string());
	operation.relation_sensitivity = value.value("relationSensitivity", std::string("ordinary"));
	return operation;
}

Json outbound_call_to_json(const protected_api::papinian::OutboundCall &call) {
	Json markers = Json::array();
	for (const protected_api::papinian::OutboundMarker &marker : call.markers) {
		Json entry{{"marker", marker.marker}, {"category", marker.category}};
		if (marker.project_entity_id.empty() == false) entry["entityId"] = marker.project_entity_id;
		markers.push_back(std::move(entry));
	}
	Json value{
		{"kind", call.kind},
		{"time", call.time},
		{"providerId", call.provider_id},
		{"modelId", call.model_id},
		{"protectionMode", call.protection_mode},
		{"markers", std::move(markers)},
		{"categoryCounts", call.category_counts},
	};
	if (call.url.empty() == false) value["url"] = call.url;
	if (call.payload_sha256.empty() == false) value["payloadSha256"] = call.payload_sha256;
	if (call.symbol_scope.empty() == false) value["symbolScope"] = call.symbol_scope;
	if (call.relations.empty() == false) {
		Json relations = Json::array();
		for (const protected_api::papinian::OutboundRelation &relation : call.relations) relations.push_back(relation_to_json(relation));
		value["relations"] = std::move(relations);
	}
	if (call.withheld_relations.empty() == false) {
		Json withheld = Json::array();
		for (const protected_api::papinian::OutboundRelation &relation : call.withheld_relations)
			withheld.push_back(relation_to_json(relation));
		value["withheldRelations"] = std::move(withheld);
	}
	if (call.roles.empty() == false) {
		Json roles = Json::array();
		for (const protected_api::papinian::OutboundRole &role : call.roles) roles.push_back(role_to_json(role));
		value["roles"] = std::move(roles);
	}
	if (call.tables.empty() == false) {
		Json tables = Json::array();
		for (const protected_api::papinian::OutboundTable &table : call.tables) tables.push_back(table_to_json(table));
		value["tables"] = std::move(tables);
	}
	if (call.operations_requested.empty() == false) {
		Json operations = Json::array();
		for (const protected_api::papinian::OutboundOperation &operation : call.operations_requested)
			operations.push_back(operation_to_json(operation));
		value["operationsRequested"] = std::move(operations);
	}
	if (call.operations_returned.empty() == false) {
		Json operations = Json::array();
		for (const protected_api::papinian::OutboundOperation &operation : call.operations_returned)
			operations.push_back(operation_to_json(operation));
		value["operationsReturned"] = std::move(operations);
	}
	return value;
}

protected_api::papinian::OutboundCall outbound_call_from_json(const Json &value) {
	protected_api::papinian::OutboundCall call;
	if (value.is_object() == false) return call;
	call.kind = value.value("kind", std::string());
	call.time = value.value("time", std::string());
	call.provider_id = value.value("providerId", std::string());
	call.model_id = value.value("modelId", std::string());
	call.protection_mode = value.value("protectionMode", std::string("off"));
	call.url = value.value("url", std::string());
	call.payload_sha256 = value.value("payloadSha256", std::string());
	call.symbol_scope = value.value("symbolScope", std::string());
	if (value.contains("relations") && value.at("relations").is_array())
		for (const Json &entry : value.at("relations")) call.relations.push_back(relation_from_json(entry));
	if (value.contains("withheldRelations") && value.at("withheldRelations").is_array())
		for (const Json &entry : value.at("withheldRelations")) call.withheld_relations.push_back(relation_from_json(entry));
	if (value.contains("roles") && value.at("roles").is_array())
		for (const Json &entry : value.at("roles")) call.roles.push_back(role_from_json(entry));
	if (value.contains("tables") && value.at("tables").is_array())
		for (const Json &entry : value.at("tables")) call.tables.push_back(table_from_json(entry));
	if (value.contains("operationsRequested") && value.at("operationsRequested").is_array())
		for (const Json &entry : value.at("operationsRequested")) call.operations_requested.push_back(operation_from_json(entry));
	if (value.contains("operationsReturned") && value.at("operationsReturned").is_array())
		for (const Json &entry : value.at("operationsReturned")) call.operations_returned.push_back(operation_from_json(entry));
	if (value.contains("markers") && value.at("markers").is_array()) {
		for (const Json &entry : value.at("markers")) {
			if (entry.is_object() == false) continue;
			protected_api::papinian::OutboundMarker marker;
			marker.marker = entry.value("marker", std::string());
			marker.category = entry.value("category", std::string());
			marker.project_entity_id = entry.value("entityId", std::string());
			if (marker.marker.empty() == false) call.markers.push_back(std::move(marker));
		}
	}
	if (value.contains("categoryCounts") && value.at("categoryCounts").is_object()) {
		for (const auto &[category, count] : value.at("categoryCounts").items())
			if (count.is_number_unsigned()) call.category_counts.emplace(category, count.get<std::size_t>());
	}
	return call;
}

// An entry written before Singularity kept its calls keeps saying so when the log
// is written again: an absent list is "unknown", never "nothing left".
Json privacy_metadata_to_json(const protected_api::papinian::PrivacyMetadata &metadata, bool isCallsRecorded) {
  Json value{
      {"mode", metadata.mode},
      {"decision", metadata.decision},
      {"decisionReason", metadata.decision_reason},
      {"papinianGuardVersion", metadata.papinian_guard_version},
      {"papinianNerVersion", metadata.papinian_ner_version},
      {"protectedCount", metadata.protected_count},
      {"categoryCounts", metadata.category_counts},
      {"responseIntegrity", metadata.response_integrity},
	  {"finalPayloadValidated", metadata.final_payload_validated},
	  {"finalPayloadSha256", metadata.final_payload_sha256},
	  {"protectedProjectEntityIds", metadata.protected_project_entity_ids},
	  {"responseIssueCode", metadata.response_issue_code},
	  {"responseMarkerCandidate", metadata.response_marker_candidate},
	  {"responseCanonicalMarker", metadata.response_canonical_marker},
	  {"responseMarkerType", metadata.response_marker_type},
	  {"responseJsonPointer", metadata.response_json_pointer},
	  {"responseNormalizationOutcome", metadata.response_normalization_outcome},
	  {"responseRepairClass", metadata.response_repair_class},
	  {"responseRepairAttempts", metadata.response_repair_attempts},
  };
	if (isCallsRecorded == true) {
		Json calls = Json::array();
		for (const protected_api::papinian::OutboundCall &call : metadata.outbound_calls)
			calls.push_back(outbound_call_to_json(call));
		value["outboundCalls"] = std::move(calls);
	}
	return value;
}

protected_api::papinian::PrivacyMetadata privacy_metadata_from_json(const Json &value) {
  protected_api::papinian::PrivacyMetadata metadata;
  if (!value.is_object()) return metadata;
  metadata.mode = value.value("mode", metadata.mode);
  metadata.decision = value.value("decision", std::string());
  metadata.decision_reason = value.value("decisionReason", std::string());
  metadata.papinian_guard_version =
      value.value("papinianGuardVersion", metadata.papinian_guard_version);
  metadata.papinian_ner_version =
      value.value("papinianNerVersion", metadata.papinian_ner_version);
  metadata.protected_count = value.value("protectedCount", std::size_t{0});
  metadata.response_integrity =
      value.value("responseIntegrity", metadata.response_integrity);
	metadata.final_payload_validated = value.value("finalPayloadValidated", false);
	metadata.final_payload_sha256 = value.value("finalPayloadSha256", std::string());
	metadata.response_issue_code = value.value("responseIssueCode", std::string());
	metadata.response_marker_candidate = value.value("responseMarkerCandidate", std::string());
	metadata.response_canonical_marker = value.value("responseCanonicalMarker", std::string());
	metadata.response_marker_type = value.value("responseMarkerType", std::string());
	metadata.response_json_pointer = value.value("responseJsonPointer", std::string());
	metadata.response_normalization_outcome = value.value("responseNormalizationOutcome", std::string());
	metadata.response_repair_class = value.value("responseRepairClass", std::string());
	metadata.response_repair_attempts = value.value("responseRepairAttempts", std::size_t{0});
	if (value.contains("protectedProjectEntityIds") && value.at("protectedProjectEntityIds").is_array())
		for (const Json &entry : value.at("protectedProjectEntityIds")) if (entry.is_string()) metadata.protected_project_entity_ids.push_back(entry.get<std::string>());
	if (value.contains("outboundCalls") && value.at("outboundCalls").is_array())
		for (const Json &entry : value.at("outboundCalls")) metadata.outbound_calls.push_back(outbound_call_from_json(entry));
  if (value.contains("categoryCounts") && value.at("categoryCounts").is_object()) {
    for (const auto &[category, count] : value.at("categoryCounts").items()) {
      if (count.is_number_unsigned()) {
        metadata.category_counts.emplace(category, count.get<std::size_t>());
      }
    }
  }
  return metadata;
}

Json entry_to_json(const RoutingLogEntry &entry) {
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
      {"processing", {{"version", entry.processing.version}, {"rule", entry.processing.rule},
                       {"localOperationUnits", entry.processing.local_operation_units},
                       {"externalOperationUnits", entry.processing.external_operation_units}}},
      {"privacyMetadata", privacy_metadata_to_json(entry.privacy_metadata, entry.outbound_calls_recorded)},
      {"status", entry.success ? "success" : "error"},
      {"latencyMs", entry.latency_ms},
      {"usage", usage_to_json(entry.usage)},
      {"finishReason", entry.finish_reason ? Json(*entry.finish_reason) : Json(nullptr)},
      {"errorCode", entry.error_code ? Json(providers::to_string(*entry.error_code)) : Json(nullptr)},
      {"routingReason", entry.routing_reason ? Json(*entry.routing_reason) : Json(nullptr)},
  };
}

RoutingLogEntry entry_from_json(const Json &value) {
  RoutingLogEntry entry;
  entry.matter_id = value.value("matterId", std::string());
  entry.operation_id = value.value("operationId", std::string());
  entry.task_id = value.value("taskId", std::string());
  entry.attempt = value.value("attempt", 1);
  entry.time = value.value("time", std::string());
  if (value.contains("providerId") && value.at("providerId").is_string()) {
    entry.provider_id = value.at("providerId").get<std::string>();
  }
  if (value.contains("modelId") && value.at("modelId").is_string()) {
    entry.model_id = value.at("modelId").get<std::string>();
  }
  entry.execution_kind = value.value("executionKind", std::string());
  entry.legal_task_preset_id = value.value("legalTaskPresetId", std::string("general"));
  entry.legal_task_preset_version = value.value("legalTaskPresetVersion", 1);
  entry.document_count = value.value("documentCount", 0);
  entry.chunk_count = value.value("chunkCount", 0);
  if (value.contains("processing") && value.at("processing").is_object()) {
    const auto &processing = value.at("processing");
    entry.processing.version = processing.value("version", 1);
    entry.processing.rule = processing.value("rule", std::string("registered-operation-units-v1"));
    entry.processing.local_operation_units = processing.value("localOperationUnits", 0);
    entry.processing.external_operation_units = processing.value("externalOperationUnits", 0);
  } else {
    entry.processing.version = 0;
    entry.processing.rule.clear();
  }
  entry.privacy_metadata = privacy_metadata_from_json(
      value.contains("privacyMetadata") ? value.at("privacyMetadata") : Json());
	entry.outbound_calls_recorded = value.contains("privacyMetadata") && value.at("privacyMetadata").is_object()
		&& value.at("privacyMetadata").contains("outboundCalls");
  entry.success = value.value("status", std::string("error")) == "success";
  entry.latency_ms = value.value("latencyMs", static_cast<long long>(0));
  if (value.contains("usage")) {
    entry.usage = usage_from_json(value.at("usage"));
  }
  if (value.contains("finishReason") && value.at("finishReason").is_string()) {
    entry.finish_reason = value.at("finishReason").get<std::string>();
  }
  if (value.contains("errorCode") && value.at("errorCode").is_string()) {
    entry.error_code =
        providers::task_error_code_from_string(value.at("errorCode").get<std::string>());
  }
  if (value.contains("routingReason") && value.at("routingReason").is_string()) {
    entry.routing_reason = value.at("routingReason").get<std::string>();
  }
  return entry;
}

std::vector<RoutingLogEntry> entries_from_json(const Json &data) {
  if (!data.is_object() || !data.contains("entries") || !data.at("entries").is_array()) {
    throw std::runtime_error("Routing log has an invalid schema");
  }
  std::vector<RoutingLogEntry> entries;
  for (const auto &entry : data.at("entries")) {
    entries.push_back(entry_from_json(entry));
  }
  return entries;
}

SentPassages sent_passages_from_json(const Json &value) {
	SentPassages kept;
	if (value.is_object() == false || value.value("version", 0) != 1) return kept;
	kept.operation_id = value.value("operationId", std::string());
	kept.matter_id = value.value("matterId", std::string());
	kept.is_recorded = true;
	if (value.contains("passages") == false || value.at("passages").is_array() == false) return kept;
	for (const Json &entry : value.at("passages")) {
		if (entry.is_object() == false) continue;
		SentPassage passage;
		passage.kind = entry.value("kind", std::string());
		passage.chunk_id = entry.value("chunkId", std::string());
		passage.content_sha256 = entry.value("contentSha256", std::string());
		passage.sent = entry.value("sent", std::string());
		passage.symbol_scope = entry.value("symbolScope", std::string());
		if (passage.sent.empty() == true || passage.content_sha256.empty() == true) continue;
		if (entry.contains("calls") == true && entry.at("calls").is_array() == true) {
			for (const Json &carried : entry.at("calls")) {
				if (carried.is_object() == false) continue;
				protected_api::papinian::OutboundCall call;
				call.kind = "model";
				call.time = carried.value("time", std::string());
				call.provider_id = carried.value("providerId", std::string());
				call.model_id = carried.value("modelId", std::string());
				passage.calls.push_back(std::move(call));
			}
		}
		kept.passages.push_back(std::move(passage));
	}
	return kept;
}

Json sent_passages_to_json(const SentPassages &kept) {
	Json passages = Json::array();
	for (const SentPassage &passage : kept.passages) {
		Json calls = Json::array();
		for (const protected_api::papinian::OutboundCall &call : passage.calls)
			calls.push_back(Json{{"time", call.time}, {"providerId", call.provider_id}, {"modelId", call.model_id}});
		Json entry{
			{"kind", passage.kind},
			{"contentSha256", passage.content_sha256},
			{"sent", passage.sent},
			{"calls", std::move(calls)},
		};
		if (passage.chunk_id.empty() == false) entry["chunkId"] = passage.chunk_id;
		if (passage.symbol_scope.empty() == false) entry["symbolScope"] = passage.symbol_scope;
		passages.push_back(std::move(entry));
	}
	return Json{
		{"version", 1},
		{"operationId", kept.operation_id},
		{"matterId", kept.matter_id},
		{"passages", std::move(passages)},
	};
}

// Everything kept of what left, across operations, has a ceiling too: past
// it the operations written longest ago let go of their passages first (their
// entries, markers and counts stay in the log), never the one just written.
constexpr std::uintmax_t kKeptPassageBytesOnDisk = 256ull * 1024 * 1024;

void prune_sent_passages(const std::filesystem::path &folder, const std::filesystem::path &written) {
	std::error_code error;
	std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
	std::uintmax_t total = 0;
	for (std::filesystem::recursive_directory_iterator entry(folder, error), end; error.value() == 0 && entry != end; entry.increment(error)) {
		std::error_code fileError;
		if (entry->is_regular_file(fileError) == false || entry->path().extension() != ".json") continue;
		const std::uintmax_t size = entry->file_size(fileError);
		if (fileError) continue;
		total += size;
		const std::filesystem::file_time_type time = entry->last_write_time(fileError);
		if (fileError.value() == 0 && entry->path() != written) files.push_back({time, entry->path()});
	}
	if (total <= kKeptPassageBytesOnDisk) return;
	std::sort(files.begin(), files.end());
	for (const auto &file : files) {
		if (total <= kKeptPassageBytesOnDisk) break;
		std::error_code fileError;
		const std::uintmax_t size = std::filesystem::file_size(file.second, fileError);
		if (fileError || std::filesystem::remove(file.second, fileError) == false) continue;
		total -= size <= total ? size : total;
	}
}

// Adds the passages `calls` carried to the operation's file: a passage already
// kept in the same form gains the request that carried it again, a new one is
// kept while the operation's ceiling allows. Written atomically, like the log.
void keep_sent_passages(const std::filesystem::path &workspace_path, const std::string &matter_id,
		const std::string &operation_id, const std::vector<protected_api::papinian::OutboundCall> &calls) {
	bool isCarried = false;
	for (const protected_api::papinian::OutboundCall &call : calls)
		if (call.passages.empty() == false) isCarried = true;
	if (isCarried == false || operation_id.empty() == true) return;

	if (matter_id.empty() == true) return;
	const std::filesystem::path path = sent_passages_path(workspace_path, matter_id, operation_id);
	SentPassages kept = load_sent_passages(workspace_path, matter_id, operation_id);
	kept.operation_id = operation_id;
	kept.matter_id = matter_id;
	std::size_t bytes = 0;
	for (const SentPassage &passage : kept.passages) bytes += passage.sent.size();
	for (const protected_api::papinian::OutboundCall &call : calls) {
		for (const protected_api::papinian::OutboundPassage &passage : call.passages) {
			auto found = std::find_if(kept.passages.begin(), kept.passages.end(), [&](const SentPassage &entry) {
				return entry.kind == passage.kind && entry.content_sha256 == passage.content_sha256 && entry.sent == passage.sent
					&& entry.symbol_scope == call.symbol_scope;
			});
			if (found == kept.passages.end()) {
				if (bytes + passage.sent.size() > outbound::kKeptPassageBytesPerOperation) continue;
				bytes += passage.sent.size();
				SentPassage added;
				added.kind = passage.kind;
				added.chunk_id = passage.chunk_id;
				added.content_sha256 = passage.content_sha256;
				added.sent = passage.sent;
				added.symbol_scope = call.symbol_scope;
				kept.passages.push_back(std::move(added));
				found = kept.passages.end() - 1;
			}
			if (found->chunk_id.empty() == true) found->chunk_id = passage.chunk_id;
			const bool isListed = std::any_of(found->calls.begin(), found->calls.end(),
				[&](const protected_api::papinian::OutboundCall &carried) {
					return carried.time == call.time && carried.provider_id == call.provider_id && carried.model_id == call.model_id;
				});
			if (isListed == true) continue;
			protected_api::papinian::OutboundCall carried;
			carried.kind = "model";
			carried.time = call.time;
			carried.provider_id = call.provider_id;
			carried.model_id = call.model_id;
			found->calls.push_back(std::move(carried));
		}
	}

	platform::ensure_record_directory(path.parent_path());
	const std::string text = sent_passages_to_json(kept).dump(2);
	platform::require_free_space(path.parent_path(), text.size());
	const auto temporary_path = unique_temporary_path(path);
	try {
		write_json_file(temporary_path, text);
		atomic_replace(temporary_path, path);
	} catch (...) {
		std::error_code cleanup_error;
		std::filesystem::remove(temporary_path, cleanup_error);
		throw;
	}
	// The same operation kept before passages were kept by Project is now here.
	const std::filesystem::path unscoped = sent_passages_folder::unscoped_path(workspace_path, operation_id);
	std::error_code unscopedError;
	if (std::filesystem::is_regular_file(unscoped, unscopedError) == true) {
		try {
			if (sent_passages_folder::matter_of(unscoped) == matter_id) std::filesystem::remove(unscoped, unscopedError);
		} catch (const std::exception &) {
		}
	}
	prune_sent_passages(sent_passages_folder::folder(workspace_path), path);
}

} // namespace

usage::UsageRecord to_usage_record(const RoutingLogEntry &entry) {
  usage::UsageRecord record;
  record.matter_id = entry.matter_id;
  record.operation_id = entry.operation_id;
  record.task_id = entry.task_id;
  record.provider_id = entry.provider_id;
  record.model_id = entry.model_id;
  record.execution_kind = entry.execution_kind;
  record.timestamp = entry.time;
  record.latency_ms = entry.latency_ms;
  record.usage = entry.usage;
  return record;
}

std::filesystem::path routing_log_path(const std::filesystem::path &workspace_path) {
  return workspace_path / ".singularity-workspace" / "routing-log.json";
}

std::filesystem::path sent_passages_path(const std::filesystem::path &workspace_path, const std::string &matter_id,
		const std::string &operation_id) {
	return sent_passages_folder::path(workspace_path, matter_id, operation_id);
}

SentPassages load_sent_passages(const std::filesystem::path &workspace_path, const std::string &matter_id,
		const std::string &operation_id) {
	SentPassages kept;
	kept.operation_id = operation_id;
	if (operation_id.empty() == true || matter_id.empty() == true) return kept;
	for (const std::filesystem::path &path : {sent_passages_path(workspace_path, matter_id, operation_id),
			sent_passages_folder::unscoped_path(workspace_path, operation_id)}) {
		try {
			std::error_code exists_error;
			if (std::filesystem::is_regular_file(path, exists_error) == false) continue;
			SentPassages read = sent_passages_from_json(read_json_file(path));
			// A record is read only for the Project it was written for.
			if (read.is_recorded == false || read.operation_id != operation_id || read.matter_id != matter_id) continue;
			return read;
		} catch (const std::exception &) {
		}
	}
	return kept;
}

void forget_project_sent_passages(const std::filesystem::path &workspace_path, const std::string &matter_id) {
	sent_passages_folder::forget_project(workspace_path, matter_id);
}

namespace {

// The routing log as last read, kept while its file stays the same (Beta 1
// #266): every privacy and usage read used to parse the whole file again, and
// it grows with every request ever made. Its own write forgets it, and a file
// of another size or time is read again. Never destroyed: a read may come as
// the process ends.
struct KeptRoutingLog {
  std::mutex mutex;
  bool is_kept = false;
  std::filesystem::path path;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type written{};
  std::vector<RoutingLogEntry> entries;
};

KeptRoutingLog &kept_routing_log() {
  static auto *kept = new KeptRoutingLog();
  return *kept;
}

void forget_kept_routing_log() {
  KeptRoutingLog &kept = kept_routing_log();
  const std::lock_guard<std::mutex> lock(kept.mutex);
  kept.is_kept = false;
  kept.entries.clear();
}

} // namespace

std::vector<RoutingLogEntry> load_routing_log(const std::filesystem::path &workspace_path) {
  std::error_code exists_error;
  const auto path = routing_log_path(workspace_path);
  if (!std::filesystem::is_regular_file(path, exists_error)) {
    const auto recovery = previous_path(path);
    if (!std::filesystem::is_regular_file(recovery, exists_error)) {
      return {};
    }
    return entries_from_json(read_json_file(recovery));
  }

  std::error_code stamp_error;
  const std::uintmax_t size = std::filesystem::file_size(path, stamp_error);
  const std::filesystem::file_time_type written = stamp_error ? std::filesystem::file_time_type{}
      : std::filesystem::last_write_time(path, stamp_error);
  KeptRoutingLog &kept = kept_routing_log();
  if (!stamp_error) {
    const std::lock_guard<std::mutex> lock(kept.mutex);
    if (kept.is_kept && kept.path == path && kept.size == size && kept.written == written) return kept.entries;
  }
  try {
    std::vector<RoutingLogEntry> entries = entries_from_json(read_json_file(path));
    if (!stamp_error) {
      const std::lock_guard<std::mutex> lock(kept.mutex);
      kept.is_kept = true;
      kept.path = path;
      kept.size = size;
      kept.written = written;
      kept.entries = entries;
    }
    return entries;
  } catch (const std::exception &) {
    const auto recovery = previous_path(path);
    try {
      return entries_from_json(read_json_file(recovery));
    } catch (const std::exception &) {
      // The lawyer's words only: the path and the parser's reason are not theirs.
      throw std::runtime_error(
          "Singularity's record of AI requests on this computer is damaged and could not be recovered.");
    }
  }
}

void settle_protection_decision(RoutingLogEntry &entry) {
	protected_api::papinian::PrivacyMetadata &recorded = entry.privacy_metadata;
	if (recorded.decision.empty() == false) return;
	if (entry.execution_kind == "local") {
		recorded.decision = entry.success == true ? "local" : "failed";
		return;
	}
	if (entry.success == true) {
		recorded.decision = "sent";
		return;
	}
	const bool isBlocked = entry.error_code == providers::TaskErrorCode::ProtectedPayloadLeakDetected
		|| entry.error_code == providers::TaskErrorCode::PrivacyProtectionUnavailable;
	recorded.decision = isBlocked == true ? "blocked" : "failed";
}

void append_routing_log_entry(const std::filesystem::path &workspace_path,
                              const RoutingLogEntry &entry) {
  // The file changes below, whatever comes of it: nothing is kept from before.
  struct ForgetKept {
    ~ForgetKept() { forget_kept_routing_log(); }
  } forgetKept;
  // OBS-1. The write below is atomic, but appending is read-modify-write: two
  // requests finishing at the same moment (two Chats, two Consilium members,
  // a request and a Context build) both read the same list, each appends its
  // own entry, and the second write replaces the first - one record of what
  // left this device, silently gone. This log is provenance, so the whole
  // append is serialised, not just the file replacement.
  // The attempt the entry closes began `latency_ms` ago, on this thread.
  const auto attemptStarted = std::chrono::steady_clock::now()
      - std::chrono::milliseconds(std::max<long long>(entry.latency_ms, 0)) - std::chrono::milliseconds(5);
  static std::mutex append_mutex;
  const std::lock_guard<std::mutex> append_lock(append_mutex);
  auto entries = load_routing_log(workspace_path);
	// The result a task keeps is one call's, which carries the calls made up to
	// it; the operation's list noted since holds every one, a professional
	// review after the draft included. A task that failed, or was answered on
	// this computer, keeps no call's result at all: its calls are the ones this
	// thread noted since the attempt began (outbound::run_calls). Every list is
	// let go only once written.
	RoutingLogEntry stored = entry;
	std::vector<std::string> outboundKeys;
	const std::string &givenKey = entry.privacy_metadata.outbound_operation_key;
	if (givenKey.empty() == false) outboundKeys.push_back(givenKey);
	const outbound::RunCalls run = outbound::run_calls(attemptStarted);
	for (const std::string &key : run.keys)
		if (std::find(outboundKeys.begin(), outboundKeys.end(), key) == outboundKeys.end()) outboundKeys.push_back(key);
	std::vector<protected_api::papinian::OutboundCall> noted;
	for (const std::string &key : outboundKeys) {
		std::vector<protected_api::papinian::OutboundCall> calls = outbound::calls_for(key);
		noted.insert(noted.end(), std::make_move_iterator(calls.begin()), std::make_move_iterator(calls.end()));
	}
	std::stable_sort(noted.begin(), noted.end(),
		[](const protected_api::papinian::OutboundCall &left, const protected_api::papinian::OutboundCall &right) {
			return left.time < right.time;
		});
	if (noted.empty() == false) stored.privacy_metadata.outbound_calls = std::move(noted);
	stored.outbound_calls_recorded = givenKey.empty() == false || run.isKnown == true;
	settle_protection_decision(stored);
  entries.push_back(stored);

  const auto directory = workspace_path / ".singularity-workspace";
  platform::ensure_record_directory(directory);

  Json list = Json::array();
  for (const auto &existing : entries) {
    list.push_back(entry_to_json(existing));
  }
  const Json data{{"version", 1}, {"entries", std::move(list)}};

  const auto path = routing_log_path(workspace_path);
  const std::string text = data.dump(2);
  platform::require_room_for_record(path, text.size());
  const auto temporary_path = unique_temporary_path(path);

  try {
    write_json_file(temporary_path, text);

    std::error_code exists_error;
    if (std::filesystem::is_regular_file(path, exists_error)) {
      // Only snapshot the existing primary as ".previous" when it is itself
      // valid. Unlike matters::save_matters(), a corrupt primary here does
      // not abort the write: load_routing_log() may have just recovered
      // `entries` from ".previous" while the primary stays corrupt on disk,
      // and telemetry has nothing worth preserving from a file that never
      // parsed - the fresh write below simply self-heals the primary.
      bool current_is_valid = true;
      try {
        (void)entries_from_json(read_json_file(path));
      } catch (const std::exception &) {
        current_is_valid = false;
      }
      if (current_is_valid) {
        const auto recovery_temporary = unique_temporary_path(previous_path(path));
        try {
          platform::stage_copy_of_file(path, recovery_temporary);
          atomic_replace(recovery_temporary, previous_path(path));
        } catch (...) {
          std::error_code recovery_cleanup_error;
          std::filesystem::remove(recovery_temporary, recovery_cleanup_error);
          throw;
        }
      }
    }
    atomic_replace(temporary_path, path);
  } catch (...) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    throw;
  }
	// What the calls' passages were as they left is kept beside the log; the
	// log above is already written, and stays so if this cannot be.
	try {
		keep_sent_passages(workspace_path, stored.matter_id, stored.operation_id, stored.privacy_metadata.outbound_calls);
	} catch (const std::exception &) {
	}
	for (const std::string &key : outboundKeys) (void)outbound::take_calls(key);
}

std::vector<OutboundRecord> outbound_records(const std::vector<RoutingLogEntry> &entries,
		const std::string &matter_id, std::size_t limit) {
	return outbound_records(entries, {}, matter_id, limit, nullptr);
}

std::vector<OutboundRecord> outbound_records(const std::vector<RoutingLogEntry> &entries,
		const std::vector<OperationTrace> &traces, const std::string &matter_id, std::size_t limit,
		bool *isTruncated) {

	std::map<std::string, OutboundRecord> byOperation;
	for (const RoutingLogEntry &entry : entries) {
		if (entry.matter_id != matter_id || entry.operation_id.empty() == true) continue;
		const bool isNew = byOperation.count(entry.operation_id) == 0;
		OutboundRecord &record = byOperation[entry.operation_id];
		record.operation_id = entry.operation_id;
		if (entry.outbound_calls_recorded == false) record.calls_recorded = false;
		for (const protected_api::papinian::OutboundCall &call : entry.privacy_metadata.outbound_calls)
			record.calls.push_back(call);
		// The latest run says where the operation's answer came from.
		if (isNew == false && entry.time < record.time) continue;
		record.time = entry.time;
		record.execution_kind = entry.execution_kind;
		record.provider_id = entry.provider_id;
		record.model_id = entry.model_id;
		record.protection_mode = entry.privacy_metadata.mode;
		record.protected_count = entry.privacy_metadata.protected_count;
		record.category_counts = entry.privacy_metadata.category_counts;
	}

	/* - - - - - - */

	for (const OperationTrace &trace : traces) {
		if (trace.operation_id.empty() == true) continue;
		const auto logged = byOperation.find(trace.operation_id);
		if (logged != byOperation.end()) {
			// An entry written on another thread, or after Singularity was opened
			// again, missed the research calls its attempt made: the task kept
			// them. Each kind counts in order, so a call the entry has is never
			// listed twice.
			OutboundRecord &record = logged->second;
			if (record.operation_type.empty() == true) record.operation_type = trace.operation_type;
			std::map<std::string, std::size_t> recordedByKind;
			for (const protected_api::papinian::OutboundCall &call : record.calls) ++recordedByKind[call.kind];
			std::map<std::string, std::size_t> keptByKind;
			for (const protected_api::papinian::OutboundCall &call : trace.research_calls) {
				if (++keptByKind[call.kind] <= recordedByKind[call.kind]) continue;
				record.calls.push_back(call);
			}
			continue;
		}
		// No entry: a Context update or the Project's memory upkeep, or a run
		// whose entry could not be written. A request to a provider that ran,
		// passed the last check before sending or billed may have left: it is
		// listed with its route, protection and counts, its calls unknown.
		const bool isExternal = trace.execution_kind == "external";
		const bool isMaybeSent = isExternal == true
			&& (trace.success == true || trace.privacy_metadata.final_payload_validated == true || trace.reported_usage == true);
		if (isMaybeSent == false && trace.research_calls.empty() == true) continue;
		OutboundRecord record;
		record.operation_id = trace.operation_id;
		record.operation_type = trace.operation_type;
		record.time = trace.time;
		record.execution_kind = isMaybeSent == true ? "external" : (isExternal == true ? "" : trace.execution_kind);
		record.provider_id = trace.provider_id;
		record.model_id = trace.model_id;
		record.protection_mode = trace.privacy_metadata.mode;
		record.protected_count = trace.privacy_metadata.protected_count;
		record.category_counts = trace.privacy_metadata.category_counts;
		record.calls = trace.research_calls;
		record.calls_recorded = isMaybeSent == false;
		// Memory upkeep keeps every one of its requests' protected identities
		// (MemoryKeeper's add_protection); a Context update keeps only its last
		// batch's, so its list says nothing of the batches before.
		record.protected_entity_ids = trace.privacy_metadata.protected_project_entity_ids;
		record.protected_entities_whole = trace.operation_type == "memory_keeping";
		byOperation.emplace(trace.operation_id, std::move(record));
	}

	/* - - - - - - */

	std::vector<OutboundRecord> records;
	records.reserve(byOperation.size());
	for (auto &[operationId, record] : byOperation) {
		std::stable_sort(record.calls.begin(), record.calls.end(),
			[](const protected_api::papinian::OutboundCall &left, const protected_api::papinian::OutboundCall &right) {
				return left.time < right.time;
			});
		records.push_back(std::move(record));
	}
	std::stable_sort(records.begin(), records.end(), [](const OutboundRecord &left, const OutboundRecord &right) {
		return left.time > right.time;
	});
	if (isTruncated != nullptr) *isTruncated = records.size() > limit;
	if (records.size() > limit) records.resize(limit);
	return records;

}

} // namespace singularity::routing
