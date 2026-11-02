#include "Bridge/PrivacyBridge/PrivacyBridge.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "PapinianGuard/Taxonomy/Taxonomy.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Privacy/PassageDisclosure/PassageDisclosure.hpp"
#include "Privacy/PrivacyActions/PrivacyActions.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace singularity::bridge::privacy_bridge {
namespace {

using Json = nlohmann::json;

std::string require_string(const Json &params, const char *key) {
	if (!params.contains(key) || !params.at(key).is_string() || params.at(key).get<std::string>().empty()) {
		throw std::runtime_error(std::string("A ") + key + " is required.");
	}
	return params.at(key).get<std::string>();
}

Json attention_document_json(const privacy::AttentionDocument &document) {
	return Json{
			{"documentId", document.document_id},
			{"name", document.name},
			{"reason", document.reason},
	};
}

Json occurrence_json(const privacy::EntityOccurrence &occurrence) {
	return Json{
			{"documentId", occurrence.document_id},
			{"alias", occurrence.alias},
			{"mentionCount", occurrence.mention_count},
	};
}

Json ambiguity_json(const privacy::EntityAmbiguity &ambiguity, const std::string &entity_id) {
	const bool isEntityA = ambiguity.entity_a_id == entity_id;
	return Json{
			{"id", ambiguity.id},
			{"category", protected_api::papinian::to_string(ambiguity.category)},
			{"otherEntityId", isEntityA ? ambiguity.entity_b_id : ambiguity.entity_a_id},
			{"thisEvidenceAlias", isEntityA ? ambiguity.evidence_alias_a : ambiguity.evidence_alias_b},
			{"otherEvidenceAlias", isEntityA ? ambiguity.evidence_alias_b : ambiguity.evidence_alias_a},
	};
}

Json entity_summary_json(const privacy_actions::EntitySummary &summary) {
	return Json{
			{"id", summary.entity.id},
			{"pseudonym", summary.entity.pseudonym},
			// What it is, in the person's own documents. Local to this machine,
			// exactly like the registry itself: this is the list a lawyer reads
			// to see who and what Singularity protects, and a list of pseudonyms
			// alone cannot be read that way.
			{"canonicalValue", summary.entity.canonical_value},
			{"category", protected_api::papinian::to_string(summary.entity.category)},
			{"occurrenceDocumentCount", static_cast<long long>(summary.entity.occurrences.size())},
			{"aliasCount", static_cast<long long>(summary.entity.aliases.size())},
			{"needsReview", summary.needs_review},
	};
}

Json entity_detail_json(const privacy_actions::EntityDetail &detail) {
	Json occurrences = Json::array();
	for (const auto &occurrence : detail.entity.occurrences) occurrences.push_back(occurrence_json(occurrence));
	Json ambiguities = Json::array();
	for (const auto &ambiguity : detail.pending_ambiguities) {
		ambiguities.push_back(ambiguity_json(ambiguity, detail.entity.id));
	}
	return Json{
			{"id", detail.entity.id},
			{"pseudonym", detail.entity.pseudonym},
			{"category", protected_api::papinian::to_string(detail.entity.category)},
			{"canonicalValue", detail.entity.canonical_value},
			{"aliases", detail.entity.aliases},
			{"occurrences", std::move(occurrences)},
			{"pendingAmbiguities", std::move(ambiguities)},
	};
}

Json outbound_call_json(const protected_api::papinian::OutboundCall &call) {
	Json markers = Json::array();
	for (const protected_api::papinian::OutboundMarker &marker : call.markers) {
		markers.push_back(Json{
				{"marker", marker.marker},
				{"category", marker.category},
				{"entityId", marker.project_entity_id.empty() ? Json(nullptr) : Json(marker.project_entity_id)},
		});
	}
	return Json{
			{"kind", call.kind},
			{"time", call.time},
			{"providerId", call.provider_id},
			{"modelId", call.model_id},
			{"protectionMode", call.protection_mode},
			{"markers", std::move(markers)},
			{"categoryCounts", call.category_counts},
			{"url", call.url.empty() ? Json(nullptr) : Json(call.url)},
			// Which marker numbering the call's markers belong to: "[[E1]]" of
			// two tables is two values.
			{"symbolScope", call.symbol_scope.empty() ? Json(nullptr) : Json(call.symbol_scope)},
	};
}

Json outbound_record_json(const routing::OutboundRecord &record) {
	Json calls = Json::array();
	for (const protected_api::papinian::OutboundCall &call : record.calls) calls.push_back(outbound_call_json(call));
	return Json{
			{"operationId", record.operation_id},
			{"operationType", record.operation_type},
			{"time", record.time},
			{"executionKind", record.execution_kind},
			{"providerId", record.provider_id ? Json(*record.provider_id) : Json(nullptr)},
			{"modelId", record.model_id ? Json(*record.model_id) : Json(nullptr)},
			{"protectionMode", record.protection_mode},
			{"protectedCount", record.protected_count},
			{"categoryCounts", record.category_counts},
			{"callsRecorded", record.calls_recorded},
			{"calls", std::move(calls)},
			// Only a list that names every identity replaced is ever handed on:
			// a partial one would read as "these, and nothing else".
			{"protectedEntityIds", record.protected_entities_whole ? Json(record.protected_entity_ids) : Json(nullptr)},
	};
}

// What one task of the Project says about what it sent, for the outbound
// record (routing::OperationTrace): its route and protection, and each web
// search or page its tool evidence says a research provider was called for.
routing::OperationTrace operation_trace(const matters::MatterTask &task) {
	routing::OperationTrace trace;
	trace.operation_id = task.id;
	trace.operation_type = task.operation_type;
	trace.time = task.created_at;
	trace.execution_kind = task.execution_kind;
	trace.provider_id = task.provider_id;
	trace.model_id = task.model_id;
	trace.success = task.status == matters::TaskStatus::Success;
	trace.reported_usage = task.usage.total_tokens.value_or(0) > 0 || task.usage.input_tokens.value_or(0) > 0
		|| task.usage.cost_micros.has_value();
	trace.privacy_metadata = task.privacy_metadata;
	for (const tools::ToolCallEvidence &evidence : task.tool_call_evidence) {
		if (evidence.external_provider_id.has_value() == false || evidence.external_provider_id->empty() == true) continue;
		const std::string kind = evidence.tool_id == "web.search" ? "web_search" : evidence.tool_id == "web.open" ? "web_open" : "";
		if (kind.empty() == true) continue;
		protected_api::papinian::OutboundCall call;
		call.kind = kind;
		call.time = evidence.completed_at.empty() == false ? evidence.completed_at : evidence.started_at;
		call.provider_id = *evidence.external_provider_id;
		call.model_id = evidence.external_model_id.value_or("");
		call.protection_mode = task.privacy_metadata.mode;
		trace.research_calls.push_back(std::move(call));
	}
	return trace;
}

// What left this device for the Project's latest operations, from the one
// outbound record (Routing Log, completed by the Project's own tasks). Null
// when the record cannot be read: the page then says it does not know, never
// that nothing left. `isTruncated` says older operations were left out.
Json outbound_json(const std::string &projectId, bool &isTruncated) {
	constexpr std::size_t kShownOperations = 200;
	isTruncated = false;
	try {
		const auto workspace = node::configured_workspace_path();
		if (workspace.has_value() == false) return Json::array();
		std::vector<routing::OperationTrace> traces;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			const std::optional<matters::Matter> project = matters::load_matter(*workspace, projectId);
			if (project.has_value() == true)
				for (const matters::MatterTask &task : project->tasks) traces.push_back(operation_trace(task));
		}
		Json records = Json::array();
		for (const routing::OutboundRecord &record : routing::outbound_records(
				routing::load_routing_log(*workspace), traces, projectId, kShownOperations, &isTruncated))
			records.push_back(outbound_record_json(record));
		return records;
	} catch (const std::exception &) {
		isTruncated = false;
		return Json(nullptr);
	}
}

Json optional_string(const std::string &value) {
	return value.empty() ? Json(nullptr) : Json(value);
}

// One run of a passage: kept words, or a hidden value - in the sent form the
// marker exactly as it left, beside the local text the original words.
Json segment_json(const privacy::sent_passages::Segment &segment) {
	Json value{{"text", segment.text}, {"hidden", segment.is_hidden}};
	if (segment.is_hidden == true) {
		value["sentAs"] = segment.sent_as;
		value["marker"] = segment.marker;
		if (segment.label.empty() == false) value["label"] = segment.label;
		if (segment.category.empty() == false) value["category"] = segment.category;
	}
	return value;
}

Json segments_json(const std::vector<privacy::sent_passages::Segment> &segments) {
	Json list = Json::array();
	for (const privacy::sent_passages::Segment &segment : segments) list.push_back(segment_json(segment));
	return list;
}

Json hidden_value_json(const privacy::sent_passages::HiddenValue &value) {
	return Json{
			{"marker", value.marker},
			{"sentAs", value.sent_as},
			{"category", value.category},
			{"label", value.label},
			{"entityId", optional_string(value.entity_id)},
			{"entityName", optional_string(value.entity_name)},
			{"originals", value.originals},
			{"missing", optional_string(value.missing)},
	};
}

Json hidden_values_json(const std::vector<privacy::sent_passages::HiddenValue> &values) {
	Json list = Json::array();
	for (const privacy::sent_passages::HiddenValue &value : values) list.push_back(hidden_value_json(value));
	return list;
}

Json disclosed_passage_json(const privacy::passage_disclosure::DisclosedPassage &passage) {
	Json calls = Json::array();
	for (const protected_api::papinian::OutboundCall &call : passage.calls)
		calls.push_back(Json{{"time", call.time}, {"providerId", call.provider_id}, {"modelId", call.model_id}});
	const bool isAligned = passage.reading == privacy::sent_passages::Reading::Aligned;
	return Json{
			{"kind", passage.kind},
			{"documentId", optional_string(passage.document_id)},
			{"chunkId", optional_string(passage.chunk_id)},
			{"documentName", optional_string(passage.document_name)},
			{"reading", privacy::sent_passages::to_string(passage.reading)},
			{"documentChanged", passage.is_document_changed},
			{"sent", segments_json(passage.sent)},
			{"local", isAligned ? segments_json(passage.local) : Json(nullptr)},
			{"hidden", hidden_values_json(passage.hidden)},
			{"calls", std::move(calls)},
	};
}

} // namespace

nlohmann::json get_overview(const nlohmann::json &params) {
	const std::string projectId = require_string(params, "projectId");
	const auto overview = privacy_actions::get_overview(projectId);
	bool isOutboundTruncated = false;
	Json outbound = outbound_json(projectId, isOutboundTruncated);
	return Json{
			{"coverage", Json{{"ready", overview.coverage.ready}, {"total", overview.coverage.total}}},
			{"attentionDocumentCount", overview.attention_document_count},
			{"entityCount", overview.entity_count},
			{"pendingAmbiguityCount", overview.pending_ambiguity_count},
			{"guardStatus", overview.guard_status},
			{"guardMessage", overview.guard_message},
			{"outbound", std::move(outbound)},
			{"outboundTruncated", isOutboundTruncated},
	};
}

nlohmann::json get_coverage(const nlohmann::json &params) {
	const auto overview = privacy_actions::get_overview(require_string(params, "projectId"));
	return Json{
			{"coverage", Json{{"ready", overview.coverage.ready}, {"total", overview.coverage.total}}},
			{"attentionDocumentCount", overview.attention_document_count},
			{"entityCount", overview.entity_count},
			{"pendingAmbiguityCount", overview.pending_ambiguity_count},
			{"guardStatus", overview.guard_status},
			{"guardMessage", overview.guard_message},
	};
}

nlohmann::json list_attention_documents(const nlohmann::json &params) {
	const auto documents = privacy_actions::list_attention_documents(require_string(params, "projectId"));
	Json result = Json::array();
	for (const auto &document : documents) result.push_back(attention_document_json(document));
	return Json{{"documents", std::move(result)}};
}

nlohmann::json list_entities(const nlohmann::json &params) {
	const auto entities = privacy_actions::list_entities(require_string(params, "projectId"));
	Json result = Json::array();
	for (const auto &summary : entities) result.push_back(entity_summary_json(summary));
	return Json{{"entities", std::move(result)}};
}

nlohmann::json get_entity_detail(const nlohmann::json &params) {
	const auto detail =
			privacy_actions::get_entity_detail(require_string(params, "projectId"), require_string(params, "entityId"));
	return entity_detail_json(detail);
}

nlohmann::json resolve_ambiguity(const nlohmann::json &params) {
	const auto survivor = privacy_actions::resolve_ambiguity(
			require_string(params, "projectId"), require_string(params, "ambiguityId"), require_string(params, "decision"));
	return Json{{"survivorEntityId", survivor.empty() ? Json(nullptr) : Json(survivor)}};
}

nlohmann::json split_entity_alias(const nlohmann::json &params) {
	const auto new_entity_id = privacy_actions::split_entity_alias(
			require_string(params, "projectId"), require_string(params, "entityId"), require_string(params, "alias"));
	return Json{{"newEntityId", new_entity_id}};
}

nlohmann::json add_entity_alias(const nlohmann::json &params) {
	privacy_actions::add_entity_alias(
			require_string(params, "projectId"), require_string(params, "entityId"), require_string(params, "alias"));
	return get_entity_detail(params);
}

nlohmann::json remove_entity_alias(const nlohmann::json &params) {
	privacy_actions::remove_entity_alias(
			require_string(params, "projectId"), require_string(params, "entityId"), require_string(params, "alias"));
	return get_entity_detail(params);
}

nlohmann::json get_passage_disclosure(const nlohmann::json &params) {
	const privacy::passage_disclosure::PassageDisclosure disclosure = privacy::passage_disclosure::disclose_passage(
			require_string(params, "projectId"), require_string(params, "operationId"),
			require_string(params, "documentId"), require_string(params, "chunkId"));
	return Json{
			{"state", disclosure.state},
			{"formCount", disclosure.form_count},
			{"passage", disclosure.passage.has_value() ? disclosed_passage_json(*disclosure.passage) : Json(nullptr)},
	};
}

nlohmann::json get_operation_disclosure(const nlohmann::json &params) {
	const privacy::passage_disclosure::OperationDisclosure disclosure = privacy::passage_disclosure::disclose_operation(
			require_string(params, "projectId"), require_string(params, "operationId"));
	Json passages = Json::array();
	for (const privacy::passage_disclosure::DisclosedPassage &passage : disclosure.passages)
		passages.push_back(disclosed_passage_json(passage));
	// What Singularity worked out on this device and sent: the line the model saw,
	// and the same line read with its originals here - never kept, re-derived.
	Json relations = Json::array();
	for (const privacy::passage_disclosure::DisclosedRelation &relation : disclosure.relations)
		relations.push_back(Json{
				{"id", relation.id},
				{"kind", relation.kind},
				{"sent", relation.sent},
				{"local", relation.local.has_value() ? Json(*relation.local) : Json(nullptr)},
				{"why", relation.why},
				{"sensitive", relation.sensitivity == "sensitive"},
				{"time", relation.time},
				{"providerId", relation.provider_id},
				{"modelId", relation.model_id},
		});
	// The protected computation protocol (DESIGN4 §11.3): what the model was
	// told each symbol is, the tables it was shown, and every operation it
	// asked Singularity to work out - with what it was sent back, and the same
	// read here with the originals, re-derived, never kept.
	Json roles = Json::array();
	for (const privacy::passage_disclosure::DisclosedRole &role : disclosure.roles)
		roles.push_back(Json{
				{"symbol", role.symbol},
				{"sent", role.line},
				{"local", role.local.has_value() ? Json(*role.local) : Json(nullptr)},
				{"roleClass", optional_string(role.role_class)},
				{"sensitive", role.sensitivity == "sensitive"},
				{"time", role.time},
				{"providerId", role.provider_id},
				{"modelId", role.model_id},
		});
	Json tables = Json::array();
	for (const privacy::passage_disclosure::DisclosedTable &table : disclosure.tables)
		tables.push_back(Json{
				{"handle", table.handle},
				{"sent", table.block},
				{"why", optional_string(table.why)},
				{"time", table.time},
				{"providerId", table.provider_id},
				{"modelId", table.model_id},
		});
	Json operations = Json::array();
	for (const privacy::passage_disclosure::DisclosedOperation &operation : disclosure.operations)
		operations.push_back(Json{
				{"id", operation.id},
				{"requested", operation.requested},
				{"status", operation.status},
				{"code", optional_string(operation.code)},
				{"returned", optional_string(operation.returned)},
				{"local", operation.local.has_value() ? Json(*operation.local) : Json(nullptr)},
				{"observation", operation.observation},
				{"sensitive", operation.sensitivity == "sensitive"},
				{"why", operation.why},
				{"askedTime", optional_string(operation.asked_time)},
				{"askedProviderId", optional_string(operation.asked_provider_id)},
				{"askedModelId", optional_string(operation.asked_model_id)},
				{"time", optional_string(operation.time)},
				{"providerId", optional_string(operation.provider_id)},
				{"modelId", optional_string(operation.model_id)},
		});
	return Json{
			{"operationId", disclosure.operation_id},
			{"recorded", disclosure.is_recorded},
			{"hidden", hidden_values_json(disclosure.hidden)},
			{"instruction", disclosure.instruction.has_value() ? disclosed_passage_json(*disclosure.instruction) : Json(nullptr)},
			{"passages", std::move(passages)},
			{"relations", std::move(relations)},
			{"withheldRelations", disclosure.withheld_relations},
			{"roles", std::move(roles)},
			{"tables", std::move(tables)},
			{"operations", std::move(operations)},
	};
}

} // namespace singularity::bridge::privacy_bridge
