#include "Bridge/ChatBridge/ChatBridge.hpp"

#include "Bridge/MatterBridge/MatterBridge.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Chat/ChatSourceResolver/ChatSourceResolver.hpp"
#include "Routing/AiSetupAssignment/AiSetupAssignment.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Routing/LegalTaskPresets/LegalTaskPresets.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace singularity::bridge::chat_bridge {
namespace {

nlohmann::json ai_setup_override_json(const matters::AiSetupRoleOverride &override_entry) {
  return nlohmann::json{
      {"role", override_entry.role}, {"providerId", override_entry.provider_id}, {"modelId", override_entry.model_id}};
}

nlohmann::json serialize_chat(const matters::MatterChat &chat) {
  nlohmann::json ai_setup_overrides = nlohmann::json::array();
  for (const auto &override_entry : chat.ai_setup_overrides) {
    ai_setup_overrides.push_back(ai_setup_override_json(override_entry));
  }
  return nlohmann::json{
      {"id", chat.id}, {"projectId", chat.project_id}, {"title", chat.title},
      {"titleAuthority", matters::to_string(chat.title_authority)},
      {"createdAt", chat.created_at}, {"updatedAt", chat.updated_at}, {"revision", chat.revision},
      {"createdByActorId", chat.created_by_actor_id}, {"updatedByActorId", chat.updated_by_actor_id},
      {"operationId", chat.operation_id}, {"archived", chat.archived},
      {"deletedAt", chat.deleted_at},
      {"selectedDocumentIds", chat.selected_document_ids},
      {"aiSetupOverrides", std::move(ai_setup_overrides)},
      {"draftText", chat.draft_text},
      {"workingDocumentId", chat.working_document_id},
      // The agent whose thread this chat is, or "" (Agent/).
      {"agentId", chat.agent_id},
      // Which earlier turns the model now receives as a compaction record, so
      // the conversation can say so where it happened.
      {"compaction", chat.compaction ? nlohmann::json{
          {"createdAt", chat.compaction->created_at},
          {"coveredTaskIds", chat.compaction->covered_task_ids}} : nlohmann::json(nullptr)},
  };
}

} // namespace

nlohmann::json list(const nlohmann::json &params) {
  const matters::ChatList result = chat::list(params.at("projectId").get<std::string>());
  nlohmann::json chats = nlohmann::json::array();
  for (const matters::MatterChat &entry : result.chats) chats.push_back(serialize_chat(entry));
  nlohmann::json task_links = nlohmann::json::array();
  for (const matters::ChatTaskLink &link : result.task_links) {
    task_links.push_back({{"taskId", link.task_id}, {"chatId", link.chat_id}});
  }
  return nlohmann::json{{"projectId", result.project_id}, {"projectName", result.project_name},
                        {"chats", std::move(chats)}, {"taskLinks", std::move(task_links)},
                        {"projectInstructionsActive", result.project_instructions_active}};
}

nlohmann::json edit_message(const nlohmann::json &params) {
  const matters::MatterTask message = chat::edit_member_message(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.at("messageId").get<std::string>(), params.at("text").get<std::string>());
  return nlohmann::json{{"messageId", message.id}, {"editedAt", message.edited_at}};
}

nlohmann::json delete_message(const nlohmann::json &params) {
  const matters::MatterTask message = chat::delete_member_message(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.at("messageId").get<std::string>());
  return nlohmann::json{{"messageId", message.id}, {"deletedAt", message.deleted_at}};
}

nlohmann::json react(const nlohmann::json &params) {
  const auto reactions = chat::react_to_message(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.at("messageId").get<std::string>(), params.at("emoji").get<std::string>());
  nlohmann::json list = nlohmann::json::array();
  for (const auto &[emoji, who] : reactions) list.push_back(nlohmann::json{{"emoji", emoji}, {"actorIds", who}});
  return nlohmann::json{{"messageId", params.at("messageId").get<std::string>()}, {"reactions", std::move(list)}};
}

nlohmann::json mark_received(const nlohmann::json &params) {
  const std::size_t marked = chat::mark_received(params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>());
  return nlohmann::json{{"marked", marked}};
}

nlohmann::json read_marks(const nlohmann::json &) {
  return nlohmann::json{{"marks", chat::read_marks(platform::resolve_actor_identity().actor_id)}};
}

nlohmann::json mark_read(const nlohmann::json &params) {
  return nlohmann::json{{"marks", chat::mark_read(platform::resolve_actor_identity().actor_id,
      params.at("conversation").get<std::string>(), params.at("upTo").get<std::string>())}};
}

nlohmann::json close_thread(const nlohmann::json &params) {
  const matters::MatterTask record = chat::close_thread(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.at("conclusion").get<std::string>());
  return nlohmann::json{{"messageId", record.id}, {"threadEvent", record.thread_event}, {"at", record.created_at}};
}

nlohmann::json reopen_thread(const nlohmann::json &params) {
  const matters::MatterTask record = chat::reopen_thread(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>());
  return nlohmann::json{{"messageId", record.id}, {"threadEvent", record.thread_event}, {"at", record.created_at}};
}

nlohmann::json create_project_chat(const nlohmann::json &params) {
  return serialize_chat(chat::create(params.at("projectId").get<std::string>()));
}

nlohmann::json rename(const nlohmann::json &params) {
  return serialize_chat(chat::rename(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.at("title").get<std::string>(),
      params.at("expectedRevision").get<long long>()));
}

nlohmann::json create(const nlohmann::json &params) {
  std::vector<std::string> selected_document_ids;
  if (params.contains("selectedDocumentIds") && params.at("selectedDocumentIds").is_array()) {
    for (const auto& document_id : params.at("selectedDocumentIds")) {
      if (document_id.is_string()) selected_document_ids.push_back(document_id.get<std::string>());
    }
  }
  return matter_bridge::serialize(chat::create(params.at("matterId").get<std::string>(),
                                               params.at("title").get<std::string>(),
                                               std::move(selected_document_ids)));
}

// The live role/model compatibility lookup happens here, not inside
// chat::save_ai_setup_overrides() itself - see
// Bridge/MatterBridge/MatterBridge.cpp's save_ai_setup() for why.
nlohmann::json save_ai_setup(const nlohmann::json &params) {
  const std::string matter_id = params.at("matterId").get<std::string>();
  const std::string chat_id = params.at("chatId").get<std::string>();
  // Only what this save changes is checked (Beta 1 #41): the overrides the
  // Chat keeps now are what it is compared with.
  std::vector<routing::RoleAssignmentEntry> previous;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    if (const std::optional<matters::Matter> kept = matters::load_matter(node::require_active_workspace_path(), matter_id))
      for (const matters::MatterChat &chat : kept->chats)
        if (chat.id == chat_id)
          for (const matters::AiSetupRoleOverride &entry : chat.ai_setup_overrides)
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
  return matter_bridge::serialize(chat::save_ai_setup_overrides(matter_id, chat_id, std::move(overrides)));
}

// The one document this Chat works on. "" clears it.
nlohmann::json set_working_document(const nlohmann::json &params) {
  return serialize_chat(chat::set_working_document(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.value("documentId", std::string())));
}

nlohmann::json save_draft(const nlohmann::json &params) {
  return serialize_chat(chat::save_draft(params.at("projectId").get<std::string>(),
      params.at("chatId").get<std::string>(), params.at("draftText").get<std::string>()));
}

nlohmann::json deletion_impact(const nlohmann::json &params) {
  const chat::ChatDeletionImpact impact = chat::deletion_impact(
      params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>());
  return nlohmann::json{
      {"turnCount", impact.turn_count},
      {"workProductCount", impact.work_product_count},
      {"contextItemCount", impact.context_item_count},
  };
}

nlohmann::json delete_chat(const nlohmann::json &params) {
  chat::delete_chat(params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>(),
      params.at("expectedRevision").get<long long>());
  return true;
}

nlohmann::json compact(const nlohmann::json &params) {
	const chat::ChatCompactionResult result = chat::compact(
		params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>());
	using JsonValue = nlohmann::json;
	return JsonValue{{"chatId", result.chat.id},
		{"coveredTurns", result.covered_turns},
		{"tokensBefore", result.tokens_before},
		{"tokensAfter", result.tokens_after},
		// What it carries when the window cannot hold even that: the same
		// conversation, every turn still named, at its smallest.
		{"tokensAfterBrief", result.tokens_after_brief},
		{"compactedAt", result.chat.compaction ? JsonValue(result.chat.compaction->created_at)
			: JsonValue(nullptr)},
		{"citedDocumentIds", result.chat.compaction ? JsonValue(result.chat.compaction->cited_document_ids)
			: JsonValue::array()},
		{"workProductIds", result.chat.compaction ? JsonValue(result.chat.compaction->work_product_ids)
			: JsonValue::array()},
		{"artifactDocumentIds", result.chat.compaction
			? JsonValue(result.chat.compaction->artifact_document_ids) : JsonValue::array()},
		{"unresolved", result.chat.compaction ? JsonValue(result.chat.compaction->unresolved)
			: JsonValue::array()}};
}

nlohmann::json restore(const nlohmann::json &params) {
  (void)chat::restore(params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>());
  return true;
}

nlohmann::json set_archived(const nlohmann::json &params) {
  if (!params.contains("archived") || !params.at("archived").is_boolean()) throw std::invalid_argument("archived must be true or false.");
  return serialize_chat(chat::set_archived(params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>(),
      params.at("archived").get<bool>()));
}

nlohmann::json search(const nlohmann::json &params) {
  const std::vector<chat::ChatSearchMatch> matches = chat::search(
      params.at("projectId").get<std::string>(), params.at("query").get<std::string>());
  nlohmann::json results = nlohmann::json::array();
  for (const chat::ChatSearchMatch &match : matches) {
    results.push_back(nlohmann::json{
        {"chatId", match.chat_id}, {"chatTitle", match.chat_title},
        {"matchLocation", match.match_location}, {"snippet", match.snippet},
    });
  }
  return results;
}

nlohmann::json resolve_sources(const nlohmann::json &params) {
  // sourcePolicyMode is required, explicit backend truth (Phase 21
  // corrective closure) - never inferred from selectedDocumentIds' shape.
  // An unknown/missing mode string is rejected here, before any retrieval
  // work, exactly like every other contract-level enum validation in this
  // Bridge (e.g. save_ai_setup()'s role parsing above).
  const auto mode_value = params.at("sourcePolicyMode").get<std::string>();
  const auto mode = chat::source_policy_mode_from_string(mode_value);
  if (!mode) throw std::runtime_error("This is not a real Chat source-policy mode.");

  std::vector<std::string> selected_document_ids;
  if (params.contains("selectedDocumentIds") && params.at("selectedDocumentIds").is_array()) {
    for (const auto &document_id : params.at("selectedDocumentIds")) {
      if (document_id.is_string()) selected_document_ids.push_back(document_id.get<std::string>());
    }
  }
  const chat::ChatSourceResolution resolved = chat::resolve_sources(
      params.at("projectId").get<std::string>(), params.at("query").get<std::string>(),
      *mode, std::move(selected_document_ids));

  nlohmann::json sources = nlohmann::json::array();
  for (const auto &source : resolved.sources) {
    sources.push_back(nlohmann::json{
        {"documentId", source.document_id},
        {"documentName", source.document_name},
        {"chunkId", source.chunk_id},
        {"page", source.page ? nlohmann::json(*source.page) : nlohmann::json(nullptr)},
        {"heading", source.heading ? nlohmann::json(*source.heading) : nlohmann::json(nullptr)},
        {"textPreview", source.text_preview},
    });
  }
  return nlohmann::json{
      {"mode", chat::to_string(resolved.mode)},
      {"selectedDocumentIds", resolved.selected_document_ids},
      {"sources", std::move(sources)},
  };
}

nlohmann::json legal_task_options(const nlohmann::json &) {
  nlohmann::json options = nlohmann::json::array();
  for (const auto &preset : context::selectable_legal_task_presets()) {
    options.push_back(nlohmann::json{
        {"id", std::string(preset.id)}, {"version", preset.version}, {"name", std::string(preset.name)}});
  }
  return options;
}

namespace {

nlohmann::json resolved_operation_role_json(const chat::ResolvedOperationRole &resolved) {
  return nlohmann::json{
      {"role", routing::to_string(resolved.role)},
      {"source", routing::to_string(resolved.source)},
      {"providerId", resolved.model ? nlohmann::json(resolved.model->provider_id) : nlohmann::json(nullptr)},
      {"modelId", resolved.model ? nlohmann::json(resolved.model->model_id) : nlohmann::json(nullptr)},
  };
}

// The one-time primary-model override, validated against the live model/
// provider registry before any resolver sees it - shared by every method
// that accepts one (Phase 22's resolve_operation_setup below and Phase 23's
// protection state/outbound preview), so a Chat can never reach request
// preparation through a second, unvalidated override path.
std::optional<routing::ModelTarget> one_time_primary_model_override_from(
    const nlohmann::json &params, const std::optional<std::string> &legal_task_preset_id) {
  if (!params.contains("oneTimePrimaryModelOverride") || !params.at("oneTimePrimaryModelOverride").is_object()) {
    return std::nullopt;
  }
  const auto &override_params = params.at("oneTimePrimaryModelOverride");
  const auto provider_id = override_params.at("providerId").get<std::string>();
  const auto model_id = override_params.at("modelId").get<std::string>();
  // Which role this one-time override targets is decided by the same
  // task-profile mapping the domain resolver itself uses - computed here,
  // before persistence-free resolution, purely so the live-registry
  // compatibility lookup happens against the real primary role instead of
  // a guessed one (see chat::resolve_operation_setup()'s own comment on
  // why this live check stays out of the domain function).
  const auto preset_id = legal_task_preset_id.value_or(std::string(context::kDefaultLegalTaskPresetId));
  const auto operation_kind = routing::operation_kind_for_legal_task_preset(preset_id);
  const auto primary_role = routing::task_profile_for(operation_kind).primary_result_role;
  const routing::ModelTarget target{provider_id, model_id};
  routing::validate_role_model_assignment(primary_role, target);
  return target;
}

std::optional<std::string> optional_string(const nlohmann::json &params, const char *key) {
  if (!params.contains(key) || !params.at(key).is_string()) return std::nullopt;
  return params.at(key).get<std::string>();
}

} // namespace

chat::ChatRequestComposition chat_request_setup(const nlohmann::json &params) {
  chat::ChatRequestComposition composition;
  composition.project_id = params.at("projectId").get<std::string>();
  composition.chat_id = optional_string(params, "chatId");
  composition.legal_task_preset_id = optional_string(params, "legalTaskPresetId");
  if (params.contains("legalTaskPresetVersion") && params.at("legalTaskPresetVersion").is_number_integer()) {
    composition.legal_task_preset_version = params.at("legalTaskPresetVersion").get<int>();
  }
  composition.one_time_primary_model_override =
      one_time_primary_model_override_from(params, composition.legal_task_preset_id);
  return composition;
}

chat::ChatRequestComposition chat_request_composition(const nlohmann::json &params) {
  chat::ChatRequestComposition composition = chat_request_setup(params);
  composition.prompt = params.at("prompt").get<std::string>();
	composition.target_document_id = optional_string(params, "targetDocumentId");
	// Send again names the request it replaces, so the new one runs with no
	// more authority than that one had (Tool/OperationAuthority). It can only
	// hold a request down: one it names that this computer did not begin runs
	// ask-first.
	composition.continues_operation_id = optional_string(params, "continuesOperationId").value_or(std::string());
	// Parsed here, once, so a send and its preview are prepared alike.
	if (params.contains("canAskLawyer")) {
		if (!params.at("canAskLawyer").is_boolean())
			throw std::runtime_error("canAskLawyer must be true or false.");
		composition.can_ask_lawyer = params.at("canAskLawyer").get<bool>();
	}
  // Explicit backend truth, rejected here before any resolution work -
  // exactly like resolve_sources() above (Phase 21 corrective closure).
  const auto mode = chat::source_policy_mode_from_string(params.at("sourcePolicyMode").get<std::string>());
  if (!mode) throw std::runtime_error("This is not a real Chat source-policy mode.");
  composition.source_policy_mode = *mode;
  if (params.contains("selectedDocumentIds") && params.at("selectedDocumentIds").is_array()) {
    for (const auto &document_id : params.at("selectedDocumentIds")) {
      if (document_id.is_string()) composition.selected_document_ids.push_back(document_id.get<std::string>());
    }
  }
  return composition;
}

nlohmann::json resolve_operation_setup(const nlohmann::json &params) {
  const chat::ChatRequestComposition composition = chat_request_setup(params);
  chat::OperationSetupRequest request;
  request.project_id = composition.project_id;
  request.chat_id = composition.chat_id;
  request.legal_task_preset_id = composition.legal_task_preset_id;
  request.legal_task_preset_version = composition.legal_task_preset_version;
  request.one_time_primary_model_override = composition.one_time_primary_model_override;

  const chat::OperationSetup setup = chat::resolve_operation_setup(request);

  nlohmann::json effective_roles = nlohmann::json::array();
  for (const auto &entry : setup.effective_roles) effective_roles.push_back(resolved_operation_role_json(entry));

  return nlohmann::json{
      {"legalTaskPresetId", setup.legal_task_preset_id},
      {"legalTaskPresetVersion", setup.legal_task_preset_version},
      {"legalTaskPresetName", setup.legal_task_preset_name},
      {"primaryResultRole", routing::to_string(setup.primary_result_role)},
      {"primaryModel", setup.primary_model
           ? nlohmann::json{{"providerId", setup.primary_model->provider_id}, {"modelId", setup.primary_model->model_id}}
           : nlohmann::json(nullptr)},
      {"primaryModelSource", routing::to_string(setup.primary_model_source)},
      {"effectiveRoles", std::move(effective_roles)},
      {"oneTimeOverrideApplied", setup.one_time_override_applied},
  };
}

} // namespace singularity::bridge::chat_bridge
