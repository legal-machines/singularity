#include "Bridge/ProjectContextBridge/ProjectContextBridge.hpp"

#include "Context/MemoryKeeper/MemoryKeeper.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace singularity::bridge::project_context_bridge {
namespace {

using Json = nlohmann::json;

std::string require_string(const Json &params, const char *key) {
	if (!params.contains(key) || !params.at(key).is_string() || params.at(key).get<std::string>().empty()) {
		throw std::runtime_error(std::string("A ") + key + " is required.");
	}
	return params.at(key).get<std::string>();
}

bool require_bool(const Json &params, const char *key) {
	if (!params.contains(key) || !params.at(key).is_boolean()) {
		throw std::runtime_error(std::string("A ") + key + " value is required.");
	}
	return params.at(key).get<bool>();
}

context::ContextItemKind require_item_kind(const Json &params) {
	const std::string kind = require_string(params, "kind");
	if (kind != "fact" && kind != "working_assumption" && kind != "instruction" && kind != "conclusion") {
		throw std::runtime_error("Choose a valid Context meaning.");
	}
	return context::context_item_kind_from_string(kind);
}

Json source_json(const context::ContextItemSource &source) {
	const auto &reference = source.document_reference;
	Json document_reference = nullptr;
	if (reference) {
		document_reference = Json{
				{"documentId", reference->document_id},
				{"documentName", reference->document_name},
				{"chunkId", reference->chunk_id},
				{"documentToken", reference->document_token},
				{"chunkToken", reference->chunk_token},
				{"page", reference->page ? Json(*reference->page) : Json(nullptr)},
				{"heading", reference->heading ? Json(*reference->heading) : Json(nullptr)},
		};
	}
	return Json{
			{"kind", context::to_string(source.kind)},
			{"documentReference", std::move(document_reference)},
			{"chatOperationId", source.chat_operation_id},
	};
}

Json item_json(const context::ContextItem &item) {
	return Json{
			{"id", item.id},
			{"section", context::to_string(item.section)},
			{"sectionLabel", context::display_section_name(item.section)},
			{"label", item.label},
			{"value", item.value},
			{"authority", context::to_string(item.authority)},
			{"source", item.source ? source_json(*item.source) : Json(nullptr)},
			{"revision", item.revision},
			{"createdAt", item.created_at},
			{"updatedAt", item.updated_at},
			{"updatedByActorId", item.updated_by_actor_id ? Json(*item.updated_by_actor_id) : Json(nullptr)},
			{"entityId", item.entity_id ? Json(*item.entity_id) : Json(nullptr)},
			{"kind", context::to_string(item.kind)},
			{"isPinned", item.is_pinned},
			{"isLocked", item.is_locked},
			{"isExcludedFromAi", item.is_excluded_from_ai},
	};
}

Json conflict_json(const context::ContextMergeConflict &conflict) {
	return Json{
			{"id", conflict.id},
			{"itemId", conflict.item_id},
			{"label", conflict.label},
			{"currentValue", conflict.current_value},
			{"authoredValue", conflict.authored_value ? Json(*conflict.authored_value) : Json(nullptr)},
			{"currentActorId", conflict.current_actor_id},
			{"authoredActorId", conflict.authored_actor_id},
			{"currentTimestamp", conflict.current_timestamp},
			{"authoredTimestamp", conflict.authored_timestamp},
	};
}

Json suggestion_json(const context::ContextSuggestion &suggestion) {
	return Json{
			{"id", suggestion.id},
			{"section", context::to_string(suggestion.section)},
			{"sectionLabel", context::display_section_name(suggestion.section)},
			{"label", suggestion.label},
			{"value", suggestion.value},
			{"targetItemId", suggestion.target_item_id ? Json(*suggestion.target_item_id) : Json(nullptr)},
			{"source", source_json(suggestion.source)},
			{"entityId", suggestion.entity_id ? Json(*suggestion.entity_id) : Json(nullptr)},
			{"status", context::to_string(suggestion.status)},
			{"reason", context::to_string(suggestion.reason)},
			{"createdAt", suggestion.created_at},
			{"resolvedAt", suggestion.resolved_at},
			{"resolvedByActorId", suggestion.resolved_by_actor_id ? Json(*suggestion.resolved_by_actor_id) : Json(nullptr)},
	};
}

} // namespace

nlohmann::json list_items(const nlohmann::json &params) {
	const auto views = context_actions::list_items(require_string(params, "projectId"));
	Json result = Json::array();
	for (const auto &view : views) {
		Json item = item_json(view.item);
		item["sourceStatus"] = context::to_string(view.standing.source_status);
		item["usedByAi"] = view.standing.used_by_ai;
		result.push_back(std::move(item));
	}
	return Json{{"items", std::move(result)}};
}

nlohmann::json list_suggestions(const nlohmann::json &params) {
	const auto suggestions = context_actions::list_pending_suggestions(require_string(params, "projectId"));
	Json result = Json::array();
	for (const auto &suggestion : suggestions) result.push_back(suggestion_json(suggestion));
	return Json{{"suggestions", std::move(result)}};
}

nlohmann::json search_items(const nlohmann::json &params) {
	const auto matches = context_actions::search_items(
			require_string(params, "projectId"), require_string(params, "query"));
	Json result = Json::array();
	for (const context::ContextSearchResult &match : matches) {
		result.push_back(Json{
				{"itemId", match.item_id},
				{"section", context::to_string(match.section)},
				{"sectionLabel", context::display_section_name(match.section)},
				{"label", match.label},
				{"value", match.value},
		});
	}
	return Json{{"results", std::move(result)}};
}

nlohmann::json get_page_state(const nlohmann::json &params) {
	const context::ContextPageState state = context_actions::get_page_state(require_string(params, "projectId"));
	return Json{
			{"state", context::to_string(state.state)},
			{"itemCount", state.item_count},
			{"pendingSuggestionCount", state.pending_suggestion_count},
			{"conflictCount", state.conflict_count},
			{"processedDocumentCount", state.processed_document_count},
			{"totalDocumentCount", state.total_document_count},
			{"automaticUpdatesAvailable", state.automatic_updates_available},
	};
}

nlohmann::json run_automatic_update(const nlohmann::json &params) {
	const context_actions::AutomaticContextUpdateResult result =
			context_actions::run_automatic_update(require_string(params, "projectId"));
	return Json{
			{"operationId", result.operation_id},
			{"candidateCount", result.application.candidate_count},
			{"appliedCount", result.application.applied_count},
			{"suggestionCount", result.application.suggestion_count},
			{"itemIds", result.application.item_ids},
			{"suggestionIds", result.application.suggestion_ids},
			{"providerId", result.provider_id},
			{"modelId", result.model_id},
			{"executionKind", result.execution_kind},
			{"aiSetupSource", result.ai_setup_source},
			{"runtimeId", result.runtime_id}, {"runtimeVersion", result.runtime_version},
			{"runtimeSourceCommit", result.runtime_source_commit}, {"runtimePlatform", result.runtime_platform},
			{"runtimeBackend", result.runtime_backend}, {"runtimeModelId", result.runtime_model_id},
			{"privacyMetadata", Json{{"mode", result.privacy_metadata.mode},
				{"papinianGuardVersion", result.privacy_metadata.papinian_guard_version},
				{"papinianNerVersion", result.privacy_metadata.papinian_ner_version},
				{"protectedCount", result.privacy_metadata.protected_count},
				{"categoryCounts", result.privacy_metadata.category_counts},
				{"responseIntegrity", result.privacy_metadata.response_integrity},
				{"finalPayloadValidated", result.privacy_metadata.final_payload_validated},
				{"finalPayloadSha256", result.privacy_metadata.final_payload_sha256},
				{"protectedProjectEntityIds", result.privacy_metadata.protected_project_entity_ids}}},
			{"protectedApiApplied", result.protected_api_applied},
	};
}

nlohmann::json accept_suggestion(const nlohmann::json &params) {
	const auto item = context_actions::accept_suggestion(
			require_string(params, "projectId"), require_string(params, "suggestionId"));
	return item_json(item);
}

nlohmann::json dismiss_suggestion(const nlohmann::json &params) {
	const std::string suggestion_id = require_string(params, "suggestionId");
	context_actions::dismiss_suggestion(require_string(params, "projectId"), suggestion_id);
	return Json{{"suggestionId", suggestion_id}, {"status", "dismissed"}};
}

// Talking to the Project's memory: a local model answers and, when what should
// be remembered changed, rewrites the brief (ProjectContextActions.hpp).
nlohmann::json memory_chat(const nlohmann::json &params) {
	const memory_keeper::Reply reply = memory_keeper::tell(require_string(params, "projectId"), require_string(params, "message"));
	return Json{{"reply", reply.reply}, {"memoryChanged", reply.memory_changed}, {"modelId", reply.model_id},
		{"executionKind", reply.execution_kind}};
}

nlohmann::json memory_status(const nlohmann::json &params) {
	const memory_keeper::Status status = memory_keeper::status(require_string(params, "projectId"));
	return Json{
			{"modelAvailable", status.model_available},
			{"modelId", status.model_id},
			{"executionKind", status.execution_kind},
			{"isUpdating", status.is_updating},
			{"source", status.source},
			{"documentsDone", status.documents_done},
			{"documentsTotal", status.documents_total},
			{"documentsRead", status.documents_read},
			{"documentsReadable", status.documents_readable},
			{"lastError", status.last_error},
			{"ready", status.ready},
	};
}

nlohmann::json memory_read_documents(const nlohmann::json &params) {
	memory_keeper::read_documents(require_string(params, "projectId"), params.value("everything", false));
	return true;
}

nlohmann::json create_item(const nlohmann::json &params) {
	const context::ContextSection section = context::context_section_from_string(require_string(params, "section"));
	const auto item = context_actions::create_item(
			require_string(params, "projectId"), section, require_string(params, "label"), require_string(params, "value"));
	return item_json(item);
}

nlohmann::json create_item_from_chat_result(const nlohmann::json &params) {
	const context::ContextSection section = context::context_section_from_string(require_string(params, "section"));
	const auto item = context_actions::create_item_from_chat_result(
			require_string(params, "projectId"), section, require_string(params, "label"),
			require_string(params, "value"), require_string(params, "matterTaskId"));
	return item_json(item);
}

nlohmann::json update_item(const nlohmann::json &params) {
	std::optional<context::ContextItem> base_item;
	if (params.contains("baseItem") && params.at("baseItem").is_object()) {
		base_item = matters::deserialize_context_item_storage(params.at("baseItem"));
	}
	const auto item = context_actions::update_item_value(
			require_string(params, "projectId"), require_string(params, "itemId"), require_string(params, "value"),
			base_item);
	return item_json(item);
}

nlohmann::json delete_item(const nlohmann::json &params) {
	const std::string item_id = require_string(params, "itemId");
	context_actions::delete_item(require_string(params, "projectId"), item_id);
	return Json{{"itemId", item_id}, {"status", "deleted"}};
}

nlohmann::json link_item_entity(const nlohmann::json &params) {
	const auto item = context_actions::link_item_entity(
			require_string(params, "projectId"), require_string(params, "itemId"), require_string(params, "entityId"));
	return item_json(item);
}

nlohmann::json unlink_item_entity(const nlohmann::json &params) {
	const auto item =
			context_actions::unlink_item_entity(require_string(params, "projectId"), require_string(params, "itemId"));
	return item_json(item);
}

nlohmann::json update_item_controls(const nlohmann::json &params) {
	const auto item = context_actions::update_item_controls(
			require_string(params, "projectId"), require_string(params, "itemId"),
			require_item_kind(params),
			require_bool(params, "isPinned"), require_bool(params, "isLocked"),
			require_bool(params, "isExcludedFromAi"));
	return item_json(item);
}

nlohmann::json list_merge_conflicts(const nlohmann::json &params) {
	const auto conflicts = context_actions::list_merge_conflicts(require_string(params, "projectId"));
	Json result = Json::array();
	for (const auto &conflict : conflicts) result.push_back(conflict_json(conflict));
	return Json{{"conflicts", std::move(result)}};
}

nlohmann::json resolve_merge_conflict(const nlohmann::json &params) {
	std::optional<std::string> edited_value;
	if (params.contains("editedValue") && params.at("editedValue").is_string()) {
		edited_value = params.at("editedValue").get<std::string>();
	}
	const auto item = context_actions::resolve_merge_conflict(
			require_string(params, "projectId"), require_string(params, "conflictId"),
			require_string(params, "resolution"), edited_value);
	return Json{{"status", "resolved"}, {"item", item ? item_json(*item) : Json(nullptr)}};
}

} // namespace singularity::bridge::project_context_bridge
