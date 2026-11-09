#include "Bridge/MatterBridge/MatterBridge.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"
#include "Tool/DeviceTrust/DeviceTrust.hpp"

#include "Activity/ActivityActions/ActivityActions.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/DocumentActionSerialization.hpp"
#include "ExternalResearch/ResearchTools/ResearchTools.hpp"
#include "Matter/SharedMatter/SharedMatterService.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterCoordination/MatterCoordination.hpp"
#include "Matter/FirmComputers/FirmComputers.hpp"
#include "Matter/MatterOffers/MatterOffersProduction.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/AiSetupAssignment/AiSetupAssignment.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Matter/MatterTrash/MatterTrash.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatConversation/ChatConversation.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include <filesystem>
#include <stdexcept>

namespace singularity::bridge::matter_bridge {
namespace {

using Json = nlohmann::json;

// What an operation cost, where the operation is read.
//
// The money was recorded on every task and persisted with it, and then dropped
// here - so a Consilium run, which is read through this projection, could be
// seen only as tokens, and what the panel cost had to be looked up separately
// in the usage ledger. The same four fields the ledger already emits from this
// same struct are emitted here, unchanged: a missing cost stays absent, never
// zero, and nothing is estimated.
Json usage_json(const providers::UsageMetadata &usage) {
  if (!usage.input_tokens && !usage.output_tokens && !usage.total_tokens && !usage.cost_micros) return nullptr;
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

Json privacy_json(const protected_api::papinian::PrivacyMetadata &metadata) {
  return Json{{"mode", metadata.mode}, {"papinianGuardVersion", metadata.papinian_guard_version},
              {"papinianNerVersion", metadata.papinian_ner_version},
              {"protectedCount", metadata.protected_count}, {"categoryCounts", metadata.category_counts},
			  {"responseIntegrity", metadata.response_integrity},
			  {"finalPayloadValidated", metadata.final_payload_validated},
			  {"finalPayloadSha256", metadata.final_payload_sha256},
			  {"protectedProjectEntityIds", metadata.protected_project_entity_ids}};
}

Json source_json(const context::ResolvedSourceReference &reference) {
  return Json{{"documentToken", reference.document_token}, {"chunkToken", reference.chunk_token},
              {"documentId", reference.document_id}, {"chunkId", reference.chunk_id},
              {"documentName", reference.document_name},
              {"page", reference.page ? Json(*reference.page) : Json(nullptr)},
              {"heading", reference.heading ? Json(*reference.heading) : Json(nullptr)}};
}

Json tool_evidence_json(const tools::ToolCallEvidence &evidence) {
	return Json{{"id", evidence.id}, {"toolId", evidence.tool_id}, {"capability", evidence.capability},
		{"effect", tools::to_string(evidence.effect)}, {"outcome", tools::to_string(evidence.outcome)},
		{"effectiveMode", tools::to_string(evidence.effective_mode)},
		{"explicitApprovalRequired", evidence.explicit_approval_required},
		{"explicitApprovalGranted", evidence.explicit_approval_granted
			? Json(*evidence.explicit_approval_granted) : Json(nullptr)},
		{"startedAt", evidence.started_at}, {"completedAt", evidence.completed_at},
		{"normalizedIntentDigest", evidence.normalized_intent_digest},
		{"errorCategory", evidence.error_category.empty() ? Json(nullptr) : Json(evidence.error_category)},
		{"rawQueryUtf8Bytes", evidence.raw_query_utf8_bytes ? Json(*evidence.raw_query_utf8_bytes) : Json(nullptr)},
		{"canonicalQueryUtf8Bytes",
			evidence.canonical_query_utf8_bytes ? Json(*evidence.canonical_query_utf8_bytes) : Json(nullptr)},
		{"intentCanonicalized",
			evidence.intent_canonicalized ? Json(*evidence.intent_canonicalized) : Json(nullptr)},
		{"estimatedInputTokens",
			evidence.estimated_input_tokens ? Json(*evidence.estimated_input_tokens) : Json(nullptr)},
		{"modelContextWindowTokens",
			evidence.model_context_window_tokens ? Json(*evidence.model_context_window_tokens) : Json(nullptr)},
		{"budgetClassification",
			evidence.budget_classification ? Json(*evidence.budget_classification) : Json(nullptr)}};
}

template <typename T, typename F>
Json array(const std::vector<T> &values, F serialize_value) {
  Json result = Json::array();
  for (const auto &value : values) result.push_back(serialize_value(value));
  return result;
}

Json finding_json(const matters::Finding &finding) {
  return Json{{"id", finding.id}, {"matterId", finding.matter_id}, {"title", finding.title},
              {"description", finding.description},
              {"recommendation", finding.recommendation ? Json(*finding.recommendation) : Json(nullptr)},
              {"severity", finding.severity ? Json(*finding.severity) : Json(nullptr)},
              {"sourceRefs", array(finding.source_refs, source_json)}};
}

Json obligation_json(const matters::Obligation &obligation) {
  return Json{{"id", obligation.id}, {"matterId", obligation.matter_id},
              {"party", obligation.party ? Json(*obligation.party) : Json(nullptr)},
              {"obligation", obligation.obligation},
              {"trigger", obligation.trigger ? Json(*obligation.trigger) : Json(nullptr)},
              {"deadline", obligation.deadline ? Json(*obligation.deadline) : Json(nullptr)},
              {"sourceRefs", array(obligation.source_refs, source_json)}};
}

Json event_json(const matters::Event &event) {
  return Json{{"id", event.id}, {"matterId", event.matter_id},
              {"date", event.date ? Json(*event.date) : Json(nullptr)},
              {"description", event.description}, {"sourceRefs", array(event.source_refs, source_json)}};
}

Json change_json(const matters::Change &change) {
  return Json{{"id", change.id}, {"matterId", change.matter_id},
              {"category", change.category ? Json(*change.category) : Json(nullptr)},
              {"description", change.description}, {"sourceRefs", array(change.source_refs, source_json)}};
}

Json issue_json(const matters::Issue &issue) {
  return Json{{"id", issue.id}, {"matterId", issue.matter_id}, {"title", issue.title},
              {"description", issue.description}, {"status", matters::to_string(issue.status)},
              {"sourceRefs", array(issue.source_refs, source_json)}, {"createdAt", issue.created_at},
              {"updatedAt", issue.updated_at}};
}

Json context_snapshot_json(const context::ContextItemSnapshot &snapshot) {
	Json source = nullptr;
	if (snapshot.source) {
		source = Json{
				{"kind", context::to_string(snapshot.source->kind)},
				{"chatOperationId", snapshot.source->chat_operation_id},
		};
		if (snapshot.source->document_reference) {
			source["documentReference"] = source_json(*snapshot.source->document_reference);
		} else source["documentReference"] = nullptr;
	}
	return Json{
			{"itemId", snapshot.item_id}, {"revision", snapshot.revision},
			{"section", context::to_string(snapshot.section)}, {"kind", context::to_string(snapshot.kind)},
			{"label", snapshot.label}, {"value", snapshot.value},
			{"authority", context::to_string(snapshot.authority)}, {"source", std::move(source)},
			{"entityId", snapshot.entity_id ? Json(*snapshot.entity_id) : Json(nullptr)},
			{"isPinned", snapshot.is_pinned}, {"isLocked", snapshot.is_locked},
			{"isExcludedFromAi", snapshot.is_excluded_from_ai},
	};
}

Json task_json(const matters::MatterTask &task) {
	Json operationCheckpoint = nullptr;
	if (task.document_operation_checkpoint) {
		const auto &progress = task.document_operation_checkpoint->progress;
		operationCheckpoint = {
			{"schemaVersion", task.document_operation_checkpoint->schema_version},
			{"stage", task.document_operation_checkpoint->stage},
			{"progress", {
				{"modelTurnsUsed", progress.model_turns_used},
				{"providerRequestsUsed", progress.provider_requests_used},
				{"successfulToolCallsUsed", progress.successful_tool_calls_used},
				{"searchCallsUsed", progress.search_calls_used},
				{"openCallsUsed", progress.open_calls_used},
				{"itemDeltasProposed", progress.item_deltas_proposed},
				{"itemDeltasAccepted", progress.item_deltas_accepted},
				{"itemDeltasRejected", progress.item_deltas_rejected},
				{"incompleteResponseRepairs", progress.incomplete_response_repairs},
				{"protectedResponseRepairs", progress.protected_response_repairs},
				{"fullActionRegenerations", progress.full_action_regenerations},
				{"documentMutationsCommitted", progress.document_mutations_committed},
			}},
		};
	}
  return Json{
      {"id", task.id}, {"createdAt", task.created_at},
	  {"operationType", task.operation_type}, {"operationRole", task.operation_role},
	  {"agentOperationId", task.agent_operation_id ? Json(*task.agent_operation_id) : Json(nullptr)},
	  {"operationExecutionState", task.operation_execution_state},
	  {"documentOperationCheckpoint", std::move(operationCheckpoint)},
	  {"aiSetupSource", task.ai_setup_source},
	  {"requestedProviderId", task.requested_provider_id ? Json(*task.requested_provider_id) : Json(nullptr)},
	  {"requestedModelId", task.requested_model_id ? Json(*task.requested_model_id) : Json(nullptr)},
      {"chatId", task.chat_id ? Json(*task.chat_id) : Json(nullptr)}, {"prompt", task.prompt},
      {"legalTaskPresetId", task.legal_task_preset_id},
      {"legalTaskPresetVersion", task.legal_task_preset_version},
      {"promptProfileId", task.prompt_profile_id}, {"promptProfileVersion", task.prompt_profile_version},
      {"selectedDocumentIds", task.selected_document_ids}, {"attempt", task.attempt},
      {"rootTaskId", task.root_task_id ? Json(*task.root_task_id) : Json(nullptr)},
      {"responseText", task.response_text ? Json(*task.response_text) : Json(nullptr)},
      {"providerId", task.provider_id ? Json(*task.provider_id) : Json(nullptr)},
      {"modelId", task.model_id ? Json(*task.model_id) : Json(nullptr)},
	  {"executionKind", task.execution_kind}, {"runtimeId", task.runtime_id},
	  {"runtimeVersion", task.runtime_version}, {"runtimeSourceCommit", task.runtime_source_commit},
	  {"runtimePlatform", task.runtime_platform}, {"runtimeBackend", task.runtime_backend},
	  {"runtimeModelId", task.runtime_model_id},
      {"protectedApiMode", task.protected_api_mode},
	  {"protectedApiApplied", task.protected_api_applied},
	  {"privacyMetadata", privacy_json(task.privacy_metadata)},
      {"status", task.status == matters::TaskStatus::Success ? "success" : "error"},
      {"error", task.error ? Json(*task.error) : Json(nullptr)},
      {"errorCode", task.error_code ? Json(providers::to_string(*task.error_code)) : Json(nullptr)},
      {"retryable", task.error_code ? Json(providers::is_retryable(*task.error_code)) : Json(nullptr)},
      {"usage", usage_json(task.usage)}, {"sourceReferences", array(task.source_references, source_json)},
	  {"toolCallEvidence", array(task.tool_call_evidence, tool_evidence_json)},
	  {"researchMode", task.research_mode},
	  {"externalEvidence", array(task.external_evidence, research::external_evidence_to_json)},
	  {"documentRoleBindings", array(task.document_role_bindings, documents::document_role_assignment_to_json)},
	  {"documentActionEvidence", array(task.document_action_evidence, documents::document_action_evidence_to_json)},
      {"contextSnapshot", array(task.context_snapshot, context_snapshot_json)},
	  {"resultContextItemIds", task.result_context_item_ids},
	  {"resultContextSuggestionIds", task.result_context_suggestion_ids},
	  {"resultWorkProductIds", task.result_work_product_ids},
	  {"directDocumentWorkDocumentId", task.direct_document_work_document_id
		  ? Json(*task.direct_document_work_document_id) : Json(nullptr)},
	  {"directDocumentWorkTitle", task.direct_document_work_title
		  ? Json(*task.direct_document_work_title) : Json(nullptr)},
      {"findings", array(task.findings, finding_json)}, {"obligations", array(task.obligations, obligation_json)},
      {"events", array(task.events, event_json)}, {"changes", array(task.changes, change_json)},
      {"consiliumGroupId", task.consilium_group_id ? Json(*task.consilium_group_id) : Json(nullptr)},
      {"consiliumRole", task.consilium_role ? Json(matters::to_string(*task.consilium_role)) : Json(nullptr)},
      // Which SEAT of the saved Consilium Configuration answered, not merely
      // which model. A panel's value to a lawyer is that "the Challenger
      // disagreed with the Reviewer" - which is unreadable from model ids
      // alone, and cannot be recovered by the frontend from prose.
      {"consiliumParticipantId",
       task.consilium_participant_id ? Json(*task.consilium_participant_id) : Json(nullptr)},
      {"consiliumParticipantRole",
       task.consilium_participant_role ? Json(*task.consilium_participant_role) : Json(nullptr)},
      {"consiliumConfigurationId",
       task.consilium_configuration_id ? Json(*task.consilium_configuration_id) : Json(nullptr)},
      {"consiliumStageId", task.consilium_stage_id ? Json(*task.consilium_stage_id) : Json(nullptr)},
      {"consiliumStageName",
       task.consilium_stage_name ? Json(*task.consilium_stage_name) : Json(nullptr)},
      {"workflowRunId", task.workflow_run_id ? Json(*task.workflow_run_id) : Json(nullptr)},
      {"workflowId", task.workflow_id ? Json(*task.workflow_id) : Json(nullptr)},
      {"workflowVersion", task.workflow_version ? Json(*task.workflow_version) : Json(nullptr)},
      {"workflowStepId", task.workflow_step_id ? Json(*task.workflow_step_id) : Json(nullptr)}};
}

Json document_json(const matters::MatterDocument &document, bool is_elsewhere = false) {
	// A Managed document whose own copy stayed on the computer that added it is
	// not ready here: its record came with the Matter, its file did not.
	const matters::DocumentLifecycleState state = is_elsewhere == true ? matters::DocumentLifecycleState::Unavailable
		: matters::document_lifecycle_state(document);
	const std::optional<std::string> attention = is_elsewhere == true
		? std::optional<std::string>("Kept on the computer that added it, so it is not on this one.") : document.attention_detail;
	return Json{
			{"id", document.id},
			{"matterId", document.matter_id},
			{"name", document.name},
			{"displayName", document.name},
			{"relativePath", document.relative_path},
			{"addedAt", document.added_at},
			{"addedByActorId", document.added_by_actor_id},
			{"sizeBytes", document.size_bytes},
			{"fileType", documents::to_string(document.file_type)},
			{"sourceMode", matters::to_string(document.source_mode)},
			{"lastModified", document.last_modified},
			{"fingerprint", document.fingerprint},
			{"contentHash", document.fingerprint},
			{"extractionStatus", matters::to_string(document.extraction_status)},
			{"extractedCharacterCount", document.extracted_character_count},
			{"pageCount", document.page_count ? Json(*document.page_count) : Json(nullptr)},
			{"extractorVersion", document.extractor_version},
			{"version", document.version},
			{"processingState", matters::to_string(state)},
			{"aiReadiness", state == matters::DocumentLifecycleState::Ready},
			{"attentionDetail", attention ? Json(*attention) : Json(nullptr)},
			{"unavailableSince", document.unavailable_since.empty() ? Json(nullptr) : Json(document.unavailable_since)},
	};
}

// The Matter's documents, each as this computer has it: a Managed one whose
// own copy is not here is said to be elsewhere.
Json documents_json(const matters::Matter &entry) {
	std::optional<std::filesystem::path> workspace;
	try {
		workspace = node::require_active_workspace_path();
	} catch (const std::exception &) {
	}
	Json result = Json::array();
	for (const matters::MatterDocument &document : entry.documents) {
		const bool is_elsewhere = workspace.has_value() == true && document.source_mode == matters::DocumentSourceMode::Managed
			&& matters::holds_managed_copy(*workspace, entry.id, document) == false;
		result.push_back(document_json(document, is_elsewhere));
	}
	return result;
}

// The rules of the Matter's correspondence intake.
Json intake_json(const matters::Matter &entry) {
  Json rules = Json::array();
  for (const correspondence::IntakeRule &rule : entry.correspondence_intake) rules.push_back(correspondence::intake_rule_to_json(rule));
  return rules;
}

Json ai_setup_override_json(const matters::AiSetupRoleOverride &override_entry) {
  return Json{{"role", override_entry.role}, {"providerId", override_entry.provider_id},
              {"modelId", override_entry.model_id}};
}

} // namespace

nlohmann::json serialize(const matters::Matter &entry) {
  Json chats = Json::array();
  for (const auto &chat : entry.chats) chats.push_back({{"id", chat.id}, {"title", chat.title},
      {"projectId", chat.project_id.empty() ? entry.id : chat.project_id},
      {"titleAuthority", matters::to_string(chat.title_authority)},
      {"createdAt", chat.created_at}, {"updatedAt", chat.updated_at}, {"revision", chat.revision},
      {"createdByActorId", chat.created_by_actor_id}, {"updatedByActorId", chat.updated_by_actor_id},
      {"operationId", chat.operation_id}, {"archived", chat.archived},
      {"deletedAt", chat.deleted_at},
      {"aiSetupOverrides", array(chat.ai_setup_overrides, ai_setup_override_json)},
      {"selectedDocumentIds", chat.selected_document_ids}});
  Json notes = Json::array();
  for (const auto &note : entry.notes) notes.push_back({{"id", note.id}, {"matterId", note.matter_id},
      {"content", note.content}, {"createdAt", note.created_at}});
  return Json{{"id", entry.id}, {"name", entry.name},
              {"appearance", Json{{"shortName", entry.appearance.short_name},
                                  {"markGlyph", entry.appearance.mark_glyph},
                                  {"markText", entry.appearance.mark_text},
                                  {"markGround", entry.appearance.mark_ground},
                                  {"markShape", entry.appearance.mark_shape}}},
              {"description", entry.description ? Json(*entry.description) : Json(nullptr)},
              {"instructions", entry.instructions ? Json(*entry.instructions) : Json(nullptr)},
              {"directoryPath", entry.directory_path ? Json(*entry.directory_path) : Json(nullptr)},
              {"createdAt", entry.created_at}, {"updatedAt", entry.updated_at},
              {"archivedAt", entry.archived_at}, {"archivedBy", entry.archived_by_actor_id},
              {"correspondenceIntake", intake_json(entry)},
							{"documents", documents_json(entry)}, {"folderPaths", entry.folder_paths},
							{"notes", std::move(notes)},
              {"issues", array(entry.issues, issue_json)}, {"chats", std::move(chats)},
              {"tasks", array(entry.tasks, task_json)},
              {"aiSetupOverrides", array(entry.ai_setup_overrides, ai_setup_override_json)},
              {"dataState", entry.data_state == matters::ProjectDataState::Ready ? "ready"
                  : entry.data_state == matters::ProjectDataState::Unavailable ? "unavailable"
                  : entry.data_state == matters::ProjectDataState::Damaged ? "damaged" : "legacy"},
              {"dataStateReason", entry.data_state_reason.empty() ? Json(nullptr) : Json(entry.data_state_reason)}};
}

nlohmann::json serialize(const std::vector<matters::Matter> &entries) {
  Json result = Json::array();
  for (const auto &entry : entries) result.push_back(serialize(entry));
  return Json{{"matters", std::move(result)}};
}

nlohmann::json list(const nlohmann::json &) { return serialize(matter::listed()); }

namespace {

Json latest_message_json(const chat::ChatLatestMessage &message) {
  return Json{{"taskId", message.task_id}, {"at", message.at}, {"authorActorId", message.author_actor_id},
              {"authorName", message.author_name}, {"agentId", message.agent_id}, {"text", message.text},
              {"mentions", message.mentions}, {"attachmentCount", static_cast<long long>(message.attachment_count)}};
}

Json summary_of(const matters::Matter &entry) {
  const matter::MatterRequestSummary requests = matter::summarize_requests(entry);
  // Each thread's latest message, by its chat: what a list of Matters says
  // under a Matter's name.
  std::map<std::string, chat::ChatLatestMessage> latest;
  for (chat::ChatLatestMessage &message : chat::latest_messages(entry)) latest.emplace(message.chat_id, std::move(message));
  Json chats = Json::array();
  for (std::size_t index = 0; index < entry.chats.size(); ++index) {
    const matters::MatterChat &chat = entry.chats[index];
    const matter::ChatRequestSummary &summary = requests.chats[index];
    const auto message = latest.find(chat.id);
    chats.push_back({{"id", chat.id}, {"title", chat.title}, {"titleAuthority", matters::to_string(chat.title_authority)},
        {"createdAt", chat.created_at}, {"updatedAt", chat.updated_at}, {"archived", chat.archived},
        {"deletedAt", chat.deleted_at}, {"agentId", chat.agent_id}, {"requestCount", summary.request_count},
        {"lastRequestAt", summary.last_request_at ? Json(*summary.last_request_at) : Json(nullptr)},
        {"lastRequest", summary.last_request ? Json(*summary.last_request) : Json(nullptr)},
        {"lastMessage", message != latest.end() ? latest_message_json(message->second) : Json(nullptr)}});
  }
  return Json{{"id", entry.id}, {"name", entry.name},
              {"description", entry.description ? Json(*entry.description) : Json("")},
              {"appearance", Json{{"shortName", entry.appearance.short_name},
                                  {"markGlyph", entry.appearance.mark_glyph},
                                  {"markText", entry.appearance.mark_text},
                                  {"markGround", entry.appearance.mark_ground},
                                  {"markShape", entry.appearance.mark_shape}}},
              {"directoryPath", entry.directory_path ? Json(*entry.directory_path) : Json(nullptr)},
              {"createdAt", entry.created_at}, {"updatedAt", entry.updated_at},
              {"archivedAt", entry.archived_at}, {"archivedBy", entry.archived_by_actor_id},
              {"correspondenceIntake", intake_json(entry)},
              {"documentCount", static_cast<long long>(entry.documents.size())},
              {"chats", std::move(chats)},
              {"consiliumGroupCount", requests.consilium_group_count},
              {"dataState", entry.data_state == matters::ProjectDataState::Ready ? "ready"
                  : entry.data_state == matters::ProjectDataState::Unavailable ? "unavailable"
                  : entry.data_state == matters::ProjectDataState::Damaged ? "damaged" : "legacy"},
              {"dataStateReason", entry.data_state_reason.empty() ? Json(nullptr) : Json(entry.data_state_reason)}};
}

} // namespace

nlohmann::json list_summaries(const nlohmann::json &) {
  Json result = Json::array();
  for (const matters::Matter &entry : matter::list()) result.push_back(summary_of(entry));
  return Json{{"matters", std::move(result)}};
}

nlohmann::json create(const nlohmann::json &params) {
  std::optional<std::string> description;
  if (params.contains("description") && params.at("description").is_string()) description = params.at("description").get<std::string>();
  std::optional<std::string> instructions;
  if (params.contains("instructions") && params.at("instructions").is_string()) instructions = params.at("instructions").get<std::string>();
  return serialize(matter::create(params.at("name").get<std::string>(), description, instructions));
}

nlohmann::json update(const nlohmann::json &params) {
  matter::UpdateMatter command;
  command.id = params.at("id").get<std::string>();
  if (params.contains("name") && params.at("name").is_string()) command.name = {true, params.at("name").get<std::string>()};
  const auto read = [&](const char *key, matter::TextPatch &patch) {
    if (!params.contains(key)) return;
    const auto &value = params.at(key);
    if (value.is_null()) patch = {true, std::nullopt};
    else if (value.is_string()) patch = {true, value.get<std::string>()};
  };
  read("description", command.description);
  read("instructions", command.instructions);
  // The Project's short name and mark arrive as one object and replace the
  // stored one whole; MatterActions bounds every value in it.
  if (params.contains("appearance") && params.at("appearance").is_object()) {
    const auto &value = params.at("appearance");
    const auto text = [&](const char *key) {
      const auto found = value.find(key);
      return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    matters::ProjectAppearance appearance;
    appearance.short_name = text("shortName");
    appearance.mark_glyph = text("markGlyph");
    appearance.mark_text = text("markText");
    appearance.mark_ground = text("markGround");
    appearance.mark_shape = text("markShape");
    command.appearance = std::move(appearance);
  }
  if (params.contains("archived") && params.at("archived").is_boolean()) command.archived = params.at("archived").get<bool>();
  // The rules of its correspondence intake, replaced as one list.
  if (params.contains("correspondenceIntake")) {
    const auto &value = params.at("correspondenceIntake");
    if (!value.is_array()) throw std::invalid_argument("correspondenceIntake must be a list of rules.");
    std::vector<correspondence::IntakeRule> rules;
    for (const auto &entry : value) rules.push_back(correspondence::intake_rule_from_json(entry));
    command.correspondence_intake = std::move(rules);
  }
  return serialize(matter::update(std::move(command)));
}

// Deleted from this computer: it stops travelling here first, so the firm's
// exchange never brings it back, then leaves the list. Its folder and
// documents stay where they are; the Matter's other computers keep it.
nlohmann::json remove(const nlohmann::json &params) {
  const std::string id = params.at("id").get<std::string>();
  try {
    (void)matter::coordination::leave_matter(id);
  } catch (const std::exception &) {
    // Not carried by the firm's exchange here: only the list changes.
  }
  return serialize(matter::remove(id));
}
namespace {

nlohmann::json trashed_json(const matter::trash::Trashed &trashed) {
  return nlohmann::json{{"id", trashed.id}, {"trashedAt", trashed.trashed_at}, {"deletesAt", trashed.deletes_at}};
}

bool is_listed_here(const std::string &id) {
  const auto listed = matter::list();
  return std::any_of(listed.begin(), listed.end(), [&](const matters::Matter &entry) { return entry.id == id; });
}

bool is_in_trash(const std::filesystem::path &workspace, const std::string &id) {
  const auto trashed = matter::trash::list(workspace);
  return std::any_of(trashed.begin(), trashed.end(), [&](const matter::trash::Trashed &entry) { return entry.id == id; });
}

} // namespace

nlohmann::json held_matter(const std::filesystem::path &folder) {
  const std::optional<matter::FolderMatter> held = matter::matter_in_folder(folder,
      [](const std::string &id) { return is_in_trash(node::require_active_workspace_path(), id); });
  if (!held) return nullptr;
  return nlohmann::json{{"id", held->id}, {"name", held->name}, {"state", held->state}};
}

nlohmann::json list_trash(const nlohmann::json &) {
  const auto workspace = node::require_active_workspace_path();
  // What has stood there 30 days is deleted from this computer first; one
  // no longer here at all simply leaves the Trash.
  for (const std::string &id : matter::trash::expired(workspace, workspaces::current_timestamp())) {
    if (is_listed_here(id)) {
      try {
        (void)remove(nlohmann::json{{"id", id}});
      } catch (const std::exception &) {
        // Deleted next time; it stays in the Trash meanwhile.
        continue;
      }
    }
    (void)matter::trash::take_back(workspace, id);
  }
  nlohmann::json matters = nlohmann::json::array();
  for (const auto &trashed : matter::trash::list(workspace)) matters.push_back(trashed_json(trashed));
  return nlohmann::json{{"matters", std::move(matters)}};
}

nlohmann::json move_to_trash(const nlohmann::json &params) {
  const std::string id = params.at("id").get<std::string>();
  if (!is_listed_here(id)) throw std::runtime_error("This Matter is no longer on this computer.");
  return trashed_json(matter::trash::put(node::require_active_workspace_path(), id, workspaces::current_timestamp()));
}

nlohmann::json restore_from_trash(const nlohmann::json &params) {
  const std::string id = params.at("id").get<std::string>();
  const bool restored = matter::trash::take_back(node::require_active_workspace_path(), id);
  return nlohmann::json{{"id", id}, {"restored", restored}};
}

// Only a Matter in the Trash is deleted from it: Delete now never reaches one
// that was restored meanwhile, on this window or another.
nlohmann::json delete_from_trash(const nlohmann::json &params) {
  const std::string id = params.at("id").get<std::string>();
  const auto workspace = node::require_active_workspace_path();
  if (!is_in_trash(workspace, id)) throw std::runtime_error("This Matter is not in the Trash any more.");
  nlohmann::json result = remove(nlohmann::json{{"id", id}});
  (void)matter::trash::take_back(workspace, id);
  return result;
}

nlohmann::json open_folder(const nlohmann::json &params) { matter::open_folder(params.at("matterId").get<std::string>()); return true; }
nlohmann::json assign_folder(const nlohmann::json &params, const std::optional<std::string> &selected_path) {
  return serialize(matter::assign_folder(params.at("matterId").get<std::string>(), selected_path));
}
nlohmann::json create_from_folder(const std::optional<std::string> &selected_path) {
  if (!selected_path) return nullptr;
  // A genuinely unconfigured Node (fresh install, or a declined broader-
  // access ceremony) still gets a real Project here - see
  // ensure_active_workspace_path()'s own comment on why this is the direct
  // "New Project" flow's job, not a prerequisite the user must clear first.
  (void)node::ensure_active_workspace_path();
  return serialize(matter::create_or_open_from_folder(*selected_path));
}
nlohmann::json create_sample(const nlohmann::json &) {
  const std::optional<std::filesystem::path> bundled = platform::bundled_sample_matter_directory();
  if (!bundled) throw std::runtime_error("This installation of Singularity carries no sample matter.");
  (void)node::ensure_active_workspace_path();
  return serialize(matter::create_sample(*bundled, platform::default_workspace_directory()));
}
nlohmann::json create_named(const nlohmann::json &params) {
  (void)node::ensure_active_workspace_path();
  return serialize(matter::create_named(params.at("name").get<std::string>(), platform::default_workspace_directory()));
}

nlohmann::json create_named_in(const nlohmann::json &params, const std::optional<std::filesystem::path> &chosen) {
  if (chosen.has_value() == false) return create_named(params);
  const std::string name = params.at("name").get<std::string>();
  std::error_code error;
  if (std::filesystem::is_directory(*chosen, error) == false)
    throw std::invalid_argument("That folder is not on this computer any more. Choose it again.");
  if (std::filesystem::exists(*chosen / ".lm", error) == true) {
    // Asked to, the Matter the folder holds is opened again, under its own
    // name: one deleted from this computer comes back.
    const auto again = params.find("openExisting");
    if (again != params.end() && again->is_boolean() == true && again->get<bool>() == true) {
      (void)node::ensure_active_workspace_path();
      return serialize(matter::create_or_open_from_folder(chosen->u8string()));
    }
    throw std::invalid_argument("That folder holds a Matter already. Open it from Matters instead.");
  }
  (void)node::ensure_active_workspace_path();
  // The lawyer gave Singularity this folder by choosing it for the
  // Matter: its documents are the Matter's.
  node::grant_folder_access(*chosen);
  matters::Matter made = matter::create_or_open_from_folder(chosen->u8string());
  // Named as the lawyer named it, not after its folder.
  const std::string trimmedName = [&name]() {
    std::string text = name;
    while (text.empty() == false && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) ++start;
    return text.substr(start);
  }();
  if (trimmedName.empty() == false && trimmedName != made.name) {
    matter::UpdateMatter renamed;
    renamed.id = made.id;
    renamed.name = matter::TextPatch{true, trimmedName};
    made = matter::update(renamed);
  }
  return serialize(made);
}
nlohmann::json add_note(const nlohmann::json &params) { return serialize(activity::add_note(params.at("matterId").get<std::string>(), params.at("content").get<std::string>())); }
nlohmann::json remove_note(const nlohmann::json &params) { return serialize(activity::remove_note(params.at("matterId").get<std::string>(), params.at("noteId").get<std::string>())); }
nlohmann::json add_issue(const nlohmann::json &params) { return serialize(activity::promote_finding(params.at("matterId").get<std::string>(), params.at("findingId").get<std::string>())); }
nlohmann::json update_issue_status(const nlohmann::json &params) {
  return serialize(activity::update_issue_status(params.at("matterId").get<std::string>(),
      params.at("issueId").get<std::string>(), params.at("status").get<std::string>()));
}

// params carries "projectId" (the public project.* wire name); it is the
// same stable id as matter::*'s "id", matching get_project_data_status()'s
// own convention above. The live role/model compatibility lookup
// (Routing::AiSetupAssignment) happens here, not inside matter::
// save_ai_setup_overrides() itself, so the Matter domain's own lightweight
// test target never has to link the local-model/provider runtime - see that
// function's own comment.
nlohmann::json save_ai_setup(const nlohmann::json &params) {
  const std::string project_id = params.at("projectId").get<std::string>();
  // Only what this save changes is checked: the overrides the Matter keeps
  // now are what it is compared with.
  std::vector<routing::RoleAssignmentEntry> previous;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    if (const std::optional<matters::Matter> kept = matters::load_matter(node::require_active_workspace_path(), project_id))
      for (const matters::AiSetupRoleOverride &entry : kept->ai_setup_overrides)
        previous.push_back({entry.role, entry.provider_id, entry.model_id});
  }
  std::vector<routing::RoleAssignmentEntry> next;
  for (const auto &entry : params.at("overrides"))
    next.push_back({entry.at("role").get<std::string>(), entry.at("providerId").get<std::string>(),
                    entry.at("modelId").get<std::string>()});
  std::vector<matters::AiSetupRoleOverride> overrides;
  for (const routing::RoleAssignmentEntry &accepted :
       routing::accept_assignment_set(previous, next, routing::validate_role_model_assignment))
    overrides.push_back(matters::AiSetupRoleOverride{accepted.role, accepted.provider_id, accepted.model_id});
  return serialize(matter::save_ai_setup_overrides(project_id, std::move(overrides)));
}

Json project_data_status_json(const matter::ProjectDataStatus &status) {
  return Json{{"available", status.available}, {"path", status.path},
              {"operationCount", status.operation_count}, {"conflictCount", status.conflict_count},
              {"state", status.state},
              {"stateReason", status.state_reason.empty() ? Json(nullptr) : Json(status.state_reason)}};
}

// params carries "projectId" (the public project.* wire name); it is the
// same stable id as matter::*'s "id" - see NativeBridge.cpp's project.*
// registration comment for why the wire namespace and the C++ domain differ.
nlohmann::json get_project_data_status(const nlohmann::json &params) {
  return project_data_status_json(matter::get_project_data_status(params.at("projectId").get<std::string>()));
}
nlohmann::json set_up_project_data(const nlohmann::json &params) {
  const bool startEmpty = params.contains("startEmpty") && params.at("startEmpty").is_boolean()
      && params.at("startEmpty").get<bool>();
  return project_data_status_json(matter::set_up_project_data(params.at("projectId").get<std::string>(), startEmpty));
}
nlohmann::json reveal_project_data(const nlohmann::json &params) {
  matter::reveal_project_data(params.at("projectId").get<std::string>());
  return true;
}
nlohmann::json prepare_project_data_clean_copy(const nlohmann::json &params,
                                               const std::optional<std::string> &selected_path) {
  return matter::prepare_project_data_clean_copy(params.at("projectId").get<std::string>(), selected_path);
}
nlohmann::json remove_project_data(const nlohmann::json &params) {
  matter::remove_project_data(params.at("projectId").get<std::string>());
  return true;
}

nlohmann::json list_project_conflicts(const nlohmann::json &params) {
	Json conflicts = Json::array();
	for (const matter::ProjectConflictSummary &conflict :
			matter::list_project_conflicts(params.at("projectId").get<std::string>())) {
		conflicts.push_back(Json{{"id", conflict.id}, {"collection", conflict.collection},
			{"area", conflict.area}, {"objectId", conflict.object_id}, {"label", conflict.label},
			{"field", conflict.field.empty() ? Json(nullptr) : Json(conflict.field)},
			{"kind", conflict.kind},
			{"currentValue", conflict.current_json.empty() ? Json(nullptr) : Json(conflict.current_json)},
			{"attemptedValue", conflict.attempted_json.empty() ? Json(nullptr) : Json(conflict.attempted_json)},
			{"currentActorId", conflict.current_actor_id}, {"currentTimestamp", conflict.current_timestamp},
			{"authoredActorId", conflict.authored_actor_id},
			{"authoredTimestamp", conflict.authored_timestamp}});
	}
	return Json{{"conflicts", std::move(conflicts)}};
}

namespace {

// An agent as its card tells the team: who answers for it, where it runs.
Json agent_card_json(const agents::AgentCard &card, const std::string &this_device, const std::string &matter_name) {
	return Json{{"agentId", card.agent_id}, {"projectId", card.matter_id}, {"projectName", matter_name}, {"name", card.name},
		{"kind", card.kind}, {"mode", card.mode}, {"character", card.character},
		{"office", Json{{"title", card.office_title}, {"practice", card.office_practice}}},
		{"goal", card.goal}, {"trigger", card.trigger}, {"followsAgentId", card.follows_agent_id},
		{"status", card.status}, {"state", card.state}, {"responsibleName", card.responsible_name},
		{"isHere", card.host_device_id.empty() == false && card.host_device_id == this_device},
		{"lastRunAt", card.last_run_at.empty() ? Json(nullptr) : Json(card.last_run_at)}, {"lastRunWords", card.last_run_words},
		{"needsReview", card.needs_review}, {"updatedAt", card.updated_at}};
}

Json shared_activity_json(const matter::SharedActivity &activity) {
	Json members = Json::array();
	for (const matter::SharedMember &member : activity.members) {
		// Each computer the member's Singularity stated, by its code, and whether
		// the person here confirmed it as theirs (Tool/DeviceTrust).
		Json computers = Json::array();
		for (const matter::SharedDeviceKey &key : member.device_keys)
			computers.push_back(Json{{"deviceId", key.device_id}, {"code", platform::device_key_code(key.public_key)},
				{"state", key.is_asking == true ? std::string("asking") : std::string(tools::to_string(tools::device_trust(member.actor_id, key.public_key)))}});
		members.push_back(Json{{"actorId", member.actor_id}, {"name", member.name}, {"organisation", member.organisation},
			{"isYou", member.actor_id == activity.my_actor_id}, {"isAccountName", member.is_account_name},
			{"position", member.position}, {"practice", member.practice},
			{"photo", member.photo.empty() ? Json(nullptr) : Json(member.photo)},
			// When their computer first joined the Matter: the overview's history
			// shows the arrival.
			{"firstSeenAt", member.first_seen_at},
			{"lastSeenAt", member.last_seen_at}, {"computers", std::move(computers)}, {"invited", member.is_invited}});
	}
	const auto nameOf = [&](const std::string &actor) {
		for (const matter::SharedMember &member : activity.members)
			if (member.actor_id == actor) return member.name;
		return std::string();
	};
	Json changes = Json::array();
	for (const matter::AttributedChange &change : activity.changes) {
		const char *author = "external";
		if (change.author == matter::ChangeAuthor::You) author = "you";
		if (change.author == matter::ChangeAuthor::Member) author = "member";
		if (change.author == matter::ChangeAuthor::Workspace) author = "workspace";
		changes.push_back(Json{{"eventId", change.event.id}, {"documentId", change.event.document_id},
			{"kind", documents::to_string(change.event.kind)}, {"at", change.event.at}, {"name", change.event.name},
			{"relativePath", change.event.relative_path}, {"previousName", change.event.previous_name},
			{"version", change.event.version}, {"previousVersion", change.event.previous_version},
			{"author", author}, {"authorName", change.author_name}});
	}
	Json overwritten = Json::array();
	for (const matter::OverwrittenChange &change : activity.overwritten)
		overwritten.push_back(Json{{"id", change.id}, {"documentId", change.document_id}, {"name", change.name},
			{"relativePath", change.relative_path}, {"foundAt", change.found_at},
			{"writtenVersion", change.written_version}, {"writtenByName", nameOf(change.written_by_actor_id)},
			{"writtenByYou", change.written_by_actor_id == activity.my_actor_id},
			{"baseVersion", change.base_version}, {"currentVersion", change.current_version},
			{"missingPassages", change.missing_passages}, {"resolution", change.resolution},
			{"resolvedAt", change.resolved_at.empty() ? Json(nullptr) : Json(change.resolved_at)},
			{"resolvedByYou", change.resolved_by_actor_id == activity.my_actor_id}});
	Json agentCards = Json::array();
	for (const agents::AgentCard &card : activity.agents) agentCards.push_back(agent_card_json(card, activity.this_device_id, ""));
	return Json{{"isShared", activity.is_shared}, {"access", activity.access}, {"members", std::move(members)},
		{"agents", std::move(agentCards)}, {"changes", std::move(changes)}, {"overwritten", std::move(overwritten)}};
}

} // namespace

Json firm_team_json(const matter::FirmTeam &team) {
	Json people = Json::array();
	for (const matter::FirmPerson &person : team.people) {
		Json matters = Json::array();
		for (const auto &[id, name] : person.matters) matters.push_back(Json{{"projectId", id}, {"name", name}});
		// Their place in the firm as their record states it: the team stands
		// in its order, by position, wherever its Matters are open.
		people.push_back(Json{{"actorId", person.member.actor_id}, {"name", person.member.name}, {"organisation", person.member.organisation},
			{"isYou", person.is_you}, {"isAccountName", person.member.is_account_name}, {"lastSeenAt", person.member.last_seen_at},
			{"position", person.member.position}, {"practice", person.member.practice},
			{"photo", person.member.photo.empty() ? Json(nullptr) : Json(person.member.photo)},
			{"title", person.member.title.empty() ? Json(nullptr) : Json(person.member.title)},
			{"about", person.member.about},
			{"archived", person.is_archived}, {"removed", person.is_removed}, {"matters", std::move(matters)}});
	}
	Json agentCards = Json::array();
	for (const matter::FirmAgent &agent : team.agents) agentCards.push_back(agent_card_json(agent.card, team.this_device_id, agent.matter_name));
	return Json{{"people", std::move(people)}, {"agents", std::move(agentCards)}};
}

nlohmann::json firm_team(const nlohmann::json &) {
	return firm_team_json(matter::firm_team());
}

namespace {

// The computers that ask to join a Matter through the firm's coordination,
// as its engine keeps them; none while its record cannot be read.
std::vector<matter::AskingDevice> asking_devices(const std::string &project_id) {
	std::vector<matter::AskingDevice> devices;
	try {
		for (const matter::coordination::LinkedMatter &link : matter::coordination::status().matters) {
			if (link.matter_id != project_id) continue;
			for (const matter::coordination::AskingComputer &computer : link.asking)
				devices.push_back(matter::AskingDevice{computer.actor_id, computer.device_id, computer.device_key});
		}
	} catch (const std::exception &) {
	}
	return devices;
}

std::optional<matter::AskingDevice> asking_device(const std::string &project_id, const std::string &actor_id,
		const std::string &device_id) {
	for (const matter::AskingDevice &device : asking_devices(project_id))
		if (device.actor_id == actor_id && device.device_id == device_id) return device;
	return std::nullopt;
}

// The Matter's activity with the computers that ask to join it among its
// members, as "asking", for the lawyer here to compare each one's code and
// confirm it. A member the Matter does not name yet is named from the firm's
// other Matters, where they work; one the lawyer said is not theirs is left out.
matter::SharedActivity with_asking(matter::SharedActivity activity, const std::string &project_id) {
	const std::vector<matter::AskingDevice> asking = asking_devices(project_id);
	if (asking.empty() == true) return activity;
	std::optional<matter::FirmTeam> team;
	matter::add_asking_devices(activity.members, asking,
		[&team](const std::string &actor_id) {
			if (team.has_value() == false) {
				try {
					team = matter::firm_team();
				} catch (const std::exception &) {
					team = matter::FirmTeam{};
				}
			}
			for (const matter::FirmPerson &person : team->people)
				if (person.member.actor_id == actor_id) return person.member.name;
			return std::string();
		},
		[](const matter::AskingDevice &device) {
			return tools::device_trust(device.actor_id, device.device_key) == tools::DeviceTrustState::Revoked;
		});
	return activity;
}

// The lawyers this computer gave the Matter to by choosing them who have no
// record of their own in it yet, among its members as invited and named as the
// firm's record names them: whoever shared it sees at once whom it went to, and
// their record takes the place once their computer opens it.
matter::SharedActivity with_invited(matter::SharedActivity activity, const std::string &project_id) {
	std::map<std::string, std::int64_t> offered;
	try {
		for (const matter::coordination::LinkedMatter &link : matter::coordination::status().matters)
			if (link.matter_id == project_id) offered = link.offered_to;
	} catch (const std::exception &) {
	}
	std::optional<matter::FirmTeam> team;
	for (const auto &[actor, at] : offered) {
		static_cast<void>(at);
		const bool isKnown = std::any_of(activity.members.begin(), activity.members.end(),
			[&actor](const matter::SharedMember &member) { return member.actor_id == actor; });
		if (isKnown == true) continue;
		if (team.has_value() == false) {
			try {
				team = matter::firm_team();
			} catch (const std::exception &) {
				team = matter::FirmTeam{};
			}
		}
		const auto person = std::find_if(team->people.begin(), team->people.end(),
			[&actor](const matter::FirmPerson &known) { return known.member.actor_id == actor; });
		if (person == team->people.end() || person->member.name.empty() == true) continue;
		matter::SharedMember invited = person->member;
		invited.device_ids.clear();
		invited.device_keys.clear();
		invited.is_invited = true;
		activity.members.push_back(std::move(invited));
	}
	return activity;
}

// The Matter's activity as the window shows it: its members, the computers
// that ask to join it and the lawyers invited to it.
nlohmann::json shown_activity(matter::SharedActivity activity, const std::string &project_id) {
	return shared_activity_json(with_invited(with_asking(std::move(activity), project_id), project_id));
}

} // namespace

// The latest document changes the window reads: who changed what, and the
// Matter's history as its overview shows it. The window drops the oldest batch
// of a full list, which may be cut (code/frontend/data/native.js).
constexpr std::size_t kSharedChanges = 200;

nlohmann::json shared_activity(const nlohmann::json &params) {
	const std::string projectId = params.at("projectId").get<std::string>();
	return shown_activity(matter::shared_activity(projectId, kSharedChanges), projectId);
}

nlohmann::json confirm_computer(const nlohmann::json &params) {
	const std::string projectId = params.at("projectId").get<std::string>();
	const std::string actorId = params.at("actorId").get<std::string>();
	const std::string deviceId = params.at("deviceId").get<std::string>();
	const std::optional<matter::AskingDevice> asking = asking_device(projectId, actorId, deviceId);
	if (asking.has_value() == true) {
		// It asked to join: it is confirmed by the key its request was signed
		// with, whose code the lawyer compared, and given the Matter's key, sealed
		// to its own.
		(void)matter::decide_asking_computer(projectId, actorId, asking->device_key, true);
		(void)matter::coordination::give_key(projectId, deviceId);
	} else {
		(void)matter::decide_member_computer(projectId, actorId, deviceId, true);
	}
	return shown_activity(matter::shared_activity(projectId, kSharedChanges), projectId);
}

nlohmann::json revoke_computer(const nlohmann::json &params) {
	const std::string projectId = params.at("projectId").get<std::string>();
	const std::string actorId = params.at("actorId").get<std::string>();
	const std::string deviceId = params.at("deviceId").get<std::string>();
	const std::optional<matter::AskingDevice> asking = asking_device(projectId, actorId, deviceId);
	if (asking.has_value() == true) {
		(void)matter::decide_asking_computer(projectId, actorId, asking->device_key, false);
		// Its request goes, and its direct link is spoken with no more.
		(void)matter::coordination::refuse(projectId, deviceId);
	} else {
		(void)matter::decide_member_computer(projectId, actorId, deviceId, false);
	}
	return shown_activity(matter::shared_activity(projectId, kSharedChanges), projectId);
}

nlohmann::json resolve_overwritten_change(const nlohmann::json &params) {
	const std::string projectId = params.at("projectId").get<std::string>();
	return shown_activity(matter::resolve_overwritten(projectId,
		params.at("changeId").get<std::string>(), params.at("choice").get<std::string>()), projectId);
}

nlohmann::json resolve_project_conflict(const nlohmann::json &params) {
	matter::resolve_project_conflict(params.at("projectId").get<std::string>(),
		params.at("conflictId").get<std::string>(), params.at("resolution").get<std::string>());
	return true;
}

namespace {

// Where the coordination stands, for the window: never a key, never what it
// knows of each file. Whether a Matter or the firm's record travels: its
// state, the computers with a live encrypted session now, when something
// sealed last came from each other computer, and what of it is on its way,
// coming here from each computer and going to each, with how much has come.
Json travel_json(const matter::coordination::LinkedMatter &link) {
  const auto transfers = [](const std::vector<matter::coordination::Transfer> &list) {
    Json found = Json::array();
    for (const matter::coordination::Transfer &transfer : list) {
      found.push_back(Json{{"deviceId", transfer.device_id}, {"files", transfer.files}, {"filesHere", transfer.files_here},
          {"bytes", transfer.bytes}, {"bytesHere", transfer.bytes_here}, {"reachable", transfer.reachable}});
    }
    return found;
  };
  Json heard = Json::array();
  for (const auto &[device, at] : link.heard) heard.push_back(Json{{"deviceId", device}, {"at", at}});
  return Json{{"state", link.state == "carried" ? "carried" : "waiting"}, {"lastSyncAt", link.last_sync_at},
      {"lastError", link.last_error}, {"connected", link.connected}, {"heard", std::move(heard)},
      {"receiving", transfers(link.receiving)}, {"sending", transfers(link.sending)}};
}

Json coordination_json(const matter::coordination::CoordinationStatus &status) {
  Json matters = Json::array();
  for (const matter::coordination::LinkedMatter &link : status.matters) {
    Json asking = Json::array();
    for (const matter::coordination::AskingComputer &computer : link.asking) {
      asking.push_back(Json{{"actorId", computer.actor_id}, {"deviceId", computer.device_id}, {"code", computer.code},
          {"at", computer.at}});
    }
    Json offering = Json::array();
    for (const matter::coordination::OfferingComputer &computer : link.offering) {
      offering.push_back(Json{{"deviceId", computer.device_id}, {"code", computer.code}, {"files", computer.files},
          {"bytes", computer.bytes}});
    }
    Json matter = travel_json(link);
    matter["matterId"] = link.matter_id;
    matter["name"] = link.name;
    matter["folder"] = link.folder.u8string();
    matter["asking"] = std::move(asking);
    matter["offering"] = std::move(offering);
    matters.push_back(std::move(matter));
  }
  // The firm's own record, apart from the Matters: whether it was heard from
  // another of the firm's computers, so a computer waiting for its firm can say
  // what has come. Null while this computer holds none.
  return Json{{"firmFolder", status.firm_folder.u8string()}, {"transports", status.transports},
      {"deviceCode", status.device_code}, {"matters", std::move(matters)},
      {"firm", status.firm.has_value() ? travel_json(*status.firm) : Json(nullptr)}};
}

std::optional<matters::Matter> loadedMatter(const std::string &id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  return matters::load_matter(node::require_active_workspace_path(), id);
}

} // namespace

nlohmann::json coordination_use_firm_folder(const nlohmann::json &params) {
  return coordination_json(matter::coordination::use_firm_folder(std::filesystem::u8path(params.at("folder").get<std::string>())));
}

nlohmann::json coordination_status(const nlohmann::json &) {
  return coordination_json(matter::coordination::status());
}

nlohmann::json coordination_share(const nlohmann::json &params) {
  const std::string id = params.at("projectId").get<std::string>();
  const std::optional<matters::Matter> found = loadedMatter(id);
  if (!found || !found->directory_path || found->directory_path->empty())
    throw std::invalid_argument("This Matter has no folder on this computer to share.");
  // The member's personal chats stay theirs.
  if (matter::is_desk(*found)) throw std::invalid_argument("Personal chats are never shared as a Matter.");
  return coordination_json(matter::coordination::share_matter(id, found->name, std::filesystem::u8path(*found->directory_path)));
}

nlohmann::json coordination_invitation(const nlohmann::json &params) {
  const std::string id = params.at("projectId").get<std::string>();
  const std::optional<matters::Matter> found = loadedMatter(id);
  if (!found) throw std::invalid_argument("This Matter is not on this computer.");
  if (matter::is_desk(*found)) throw std::invalid_argument("Personal chats are never shared as a Matter.");
  (void)matter::coordination::share_matter(id, found->name,
      std::filesystem::u8path(found->directory_path.value_or(std::string())));
  return Json{{"invitation", matter::coordination::invitation(id)}};
}

namespace {

// A Matter's own folder on this computer when it comes from another, beside
// this computer's other Matters, named as the firm names it. The lawyer gives
// Singularity this folder by asking for the Matter: it is found in it once its
// records have come, whatever other folders this lawyer gave. Given before the
// coordination knows the folder, so nothing written to test it is ever sent.
std::filesystem::path folder_for_coming(const std::string &given_name) {
  // A name another computer gave is a folder's name here, never a path: no
  // separator, no drive, no control character, and no leading dot.
  std::string name;
  for (const char letter : given_name) {
    const unsigned char byte = static_cast<unsigned char>(letter);
    if (byte < 0x20 || letter == '/' || letter == '\\' || letter == ':' || letter == '*' || letter == '?' || letter == '"'
        || letter == '<' || letter == '>' || letter == '|') {
      name += ' ';
    } else {
      name += letter;
    }
  }
  while (name.empty() == false && (name.front() == '.' || name.front() == ' ')) name.erase(name.begin());
  while (name.empty() == false && (name.back() == '.' || name.back() == ' ')) name.pop_back();
  if (name.size() > 120) name.resize(120);
  if (name.empty() == true) name = "Matter";
  const std::filesystem::path root = platform::default_workspace_directory();
  std::filesystem::path folder = root / std::filesystem::u8path(name);
  for (int next = 2; std::filesystem::exists(folder / ".lm") == false && std::filesystem::exists(folder)
       && std::filesystem::is_empty(folder) == false; ++next) {
    folder = root / std::filesystem::u8path(name + " " + std::to_string(next));
  }
  std::filesystem::create_directories(folder);
  node::grant_folder_access(folder);
  return folder;
}

} // namespace

nlohmann::json coordination_join(const nlohmann::json &params) {
  const std::optional<matter::coordination::Invitation> invitation =
      matter::coordination::read_invitation(params.at("invitation").get<std::string>());
  if (invitation.has_value() == false)
    throw std::invalid_argument("That is not an invitation to a Matter: copy it whole, from \"LMM1\" to its end.");
  const std::string name = invitation->name.empty() == true ? std::string("Matter") : invitation->name;
  const std::filesystem::path folder = folder_for_coming(name);
  // By the whole invitation: where it names the inviting computer's direct link,
  // this computer calls it there.
  matter::coordination::Invitation joining = *invitation;
  joining.name = name;
  return coordination_json(matter::coordination::join_matter(joining, folder));
}

namespace {

Json firm_computers_json() {
  Json computers = Json::array();
  for (const matter::firm_computers::View &view : matter::firm_computers::computers_as_known()) {
    computers.push_back(Json{{"seatId", view.seat_id}, {"actorId", "seat:" + view.seat_id}, {"role", view.role}, {"deviceId", view.device_id},
        {"code", view.code}, {"state", view.state}, {"announcedAt", view.announced_at}});
  }
  Json asking = Json::array();
  for (const std::string &device : matter::firm_computers::asking()) asking.push_back(device);
  return Json{{"computers", computers}, {"asking", asking}};
}

Json check_json(const matter::firm_computers::Check &check) {
  return Json{{"deviceId", check.device_id}, {"state", check.state}, {"number", check.number}, {"problem", check.problem}};
}

std::string device_param(const Json &params) {
  const std::string device = params.at("deviceId").get<std::string>();
  if (device.empty() == true) throw std::invalid_argument("Choose the computer to check.");
  return device;
}

Json offers_json() {
  Json offers = Json::array();
  for (const matter::offers::Waiting &entry : matter::offers::waiting()) {
    offers.push_back(Json{{"matterId", entry.offer.matter_id}, {"name", entry.offer.name}, {"files", entry.offer.files},
        {"bytes", entry.offer.bytes}, {"fromActorId", entry.offer.from_actor}, {"fromDeviceId", entry.offer.from_device},
        {"at", entry.offer.at}});
  }
  return Json{{"offers", offers}};
}

} // namespace

nlohmann::json coordination_firm_computers(const nlohmann::json &) {
  return firm_computers_json();
}

nlohmann::json coordination_check_computer(const nlohmann::json &params) {
  return check_json(matter::firm_computers::start_check(device_param(params)));
}

nlohmann::json coordination_computer_check(const nlohmann::json &params) {
  return check_json(matter::firm_computers::check_state(device_param(params)));
}

nlohmann::json coordination_keep_computer(const nlohmann::json &params) {
  matter::firm_computers::match(device_param(params));
  return firm_computers_json();
}

nlohmann::json coordination_refuse_computer(const nlohmann::json &params) {
  matter::firm_computers::refuse(device_param(params));
  return firm_computers_json();
}

nlohmann::json coordination_end_computer_check(const nlohmann::json &params) {
  matter::firm_computers::cancel(device_param(params));
  return firm_computers_json();
}

nlohmann::json coordination_give_matter(const nlohmann::json &params) {
  const std::string id = params.at("projectId").get<std::string>();
  const std::optional<matters::Matter> found = loadedMatter(id);
  if (!found || !found->directory_path || found->directory_path->empty())
    throw std::invalid_argument("This Matter has no folder on this computer to give.");
  // The member's personal chats stay theirs.
  if (matter::is_desk(*found)) throw std::invalid_argument("Personal chats are never given as a Matter.");
  const matter::offers::Given given = matter::offers::give(id, found->name, std::filesystem::u8path(*found->directory_path),
      params.at("actorId").get<std::string>());
  return Json{{"given", true}, {"reaches", given.reaches}};
}

nlohmann::json coordination_offers(const nlohmann::json &) {
  return offers_json();
}

nlohmann::json coordination_accept_offer(const nlohmann::json &params) {
  const std::string matterId = params.at("matterId").get<std::string>();
  const std::string fromDevice = params.at("fromDeviceId").get<std::string>();
  std::optional<std::string> name;
  for (const matter::offers::Waiting &entry : matter::offers::waiting()) {
    if (entry.offer.matter_id == matterId && entry.offer.from_device == fromDevice) name = entry.offer.name;
  }
  if (name.has_value() == false) throw std::invalid_argument("That Matter is no longer offered to you here.");
  return coordination_json(matter::offers::accept(matterId, fromDevice, folder_for_coming(*name)));
}

nlohmann::json coordination_decline_offer(const nlohmann::json &params) {
  matter::offers::decline(params.at("matterId").get<std::string>(), params.at("fromDeviceId").get<std::string>());
  return offers_json();
}

nlohmann::json coordination_accept_key(const nlohmann::json &params) {
  return coordination_json(matter::coordination::accept_key(params.at("matterId").get<std::string>(),
      params.at("deviceId").get<std::string>()));
}

namespace {

// `reaches`: another computer of the two holds the conversation's key - given
// it from here, or it gave it here - so what is written in it travels. Until
// then it waits on this computer, and the conversation says so.
Json personal_chat_json(const std::string &other_actor_id, const matters::Matter &project, bool reaches) {
  const std::vector<std::string> chats = chat::conversation_chats(project.id);
  return Json{{"otherActorId", other_actor_id}, {"projectId", project.id},
      {"chatId", chats.empty() ? std::string() : chats.front()}, {"chatIds", chats}, {"reaches", reaches}};
}

bool channel_reaches(const std::string &channel_id) {
  for (const matter::coordination::LinkedMatter &channel : matter::coordination::status().personal) {
    if (channel.matter_id == channel_id) return channel.given.empty() == false;
  }
  return false;
}

} // namespace

nlohmann::json open_personal_chat(const nlohmann::json &params) {
  const std::string other = params.at("otherActorId").get<std::string>();
  // An AI colleague's personal chat is on the member's desk, never here.
  if (other.rfind("agent_", 0) == 0) throw std::invalid_argument("A personal chat with an AI colleague is on your desk.");
  (void)node::ensure_active_workspace_path();
  const matter::coordination::PersonalChannel channel = matter::coordination::personal_channel(other);
  const matters::Matter project = matter::open_personal_project(channel.folder, matter::personal_project_id(channel.id));
  (void)chat::conversation_chat(project.id, true);
  return personal_chat_json(other, project, channel_reaches(channel.id));
}

nlohmann::json list_personal_chats(const nlohmann::json &) {
  Json conversations = Json::array();
  if (!node::configured_workspace_path()) return Json{{"conversations", conversations}};
  const std::string me = platform::resolve_actor_identity().actor_id;
  for (const matter::coordination::LinkedMatter &channel : matter::coordination::status().personal) {
    // This member's own conversations with people: never a colleague's channel
    // this computer carries for another member, never an AI colleague's.
    if (channel.state != "carried" || channel.other_actor_id.empty() || channel.other_actor_id.rfind("agent_", 0) == 0) continue;
    if (me.empty() || matter::coordination::personal_channel_id(me, channel.other_actor_id) != channel.matter_id) continue;
    // Nothing written in it yet by either member: no conversation to show.
    if (!persistence::has_project_data(channel.folder.u8string())) continue;
    try {
      const matters::Matter project = matter::open_personal_project(channel.folder, matter::personal_project_id(channel.matter_id));
      if (chat::conversation_chat(project.id, false).empty()) continue;
      conversations.push_back(personal_chat_json(channel.other_actor_id, project, channel.given.empty() == false));
    } catch (const std::exception &) {
      // A conversation that cannot be read yet is listed once it can.
    }
  }
  return Json{{"conversations", conversations}};
}

nlohmann::json discover_projects(const nlohmann::json &) {
  Json result = Json::array();
  for (const persistence::ProjectSummary &summary : matter::discover_projects()) {
    result.push_back(Json{{"id", summary.id}, {"name", summary.name}, {"path", summary.path},
        {"status", summary.status}, {"operationCount", summary.operation_count},
        {"conflictCount", summary.conflict_count}, {"hasDuplicateLocation", summary.has_duplicate_location}});
  }
  return Json{{"projects", std::move(result)}};
}

nlohmann::json open_discovered(const nlohmann::json &params) {
  const std::string path = params.at("path").get<std::string>();
  std::error_code error;
  const std::string wanted = std::filesystem::weakly_canonical(std::filesystem::u8path(path), error).u8string();
  if (error) throw std::invalid_argument("That folder is not one Singularity can open.");
  bool isDiscovered = false;
  for (const persistence::ProjectSummary &summary : matter::discover_projects()) {
    if (summary.id.empty() || summary.has_duplicate_location) continue;
    std::error_code found_error;
    const std::string found = std::filesystem::weakly_canonical(std::filesystem::u8path(summary.path), found_error).u8string();
    if (!found_error && found == wanted) isDiscovered = true;
  }
  if (!isDiscovered) {
    // Found, but not the firm's: it says so.
    if (const std::optional<std::string> refusal = matter::seal_refusal(std::filesystem::u8path(path))) throw std::invalid_argument(*refusal);
    throw std::invalid_argument("That folder is not a Matter in the folders you gave Singularity, so it was not opened.");
  }
  (void)node::ensure_active_workspace_path();
  return serialize(matter::create_or_open_from_folder(path));
}

} // namespace singularity::bridge::matter_bridge
