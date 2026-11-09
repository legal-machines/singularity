#include "Chat/ChatOutboundPreview/ChatOutboundPreview.hpp"

#include "PapinianGuard/Policy/Policy.hpp"
#include "ExternalResearch/ResearchIntent/ResearchIntent.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/MatterRequestExecution/MatterRequestExecution.hpp"
#include "Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.hpp"

#include <nlohmann/json.hpp>

#include <mutex>

#include <stdexcept>
#include <utility>

namespace singularity::chat {
namespace {

// The one place this component decides "external": a provider id that is
// not the local runtime. Matches routing::preview()'s own branch exactly,
// so a Chat that says "Stays on this Mac" is saying what the request
// pipeline itself would do.
bool is_external_target(const routing::ModelTarget &target) {
  return target.provider_id != "local";
}

} // namespace

PreparedChatRequest prepare_chat_request(const ChatRequestComposition &composition) {
	professional::ProfessionalExecutionIntent intent =
		professional::resolve_professional_execution_intent(
			composition.prompt, composition.selected_document_ids);
  // Phase 21, first and unconditionally: the accepted backend source
  // boundary owns whether this composition's explicit mode/selection pair
  // is even a legal request. Relevant with a selection, Selected with an
  // empty one, and Selected naming a document that is not a genuinely
  // AI-ready document of this Project all fail closed here - before any
  // request is prepared and long before anything could be previewed.
	const ChatSourceResolution sources = resolve_sources(composition.project_id, composition.prompt,
		composition.source_policy_mode, composition.selected_document_ids, composition.target_document_id);
	intent = professional::resolve_professional_execution_intent(composition.prompt, sources.selected_document_ids);

  // Phase 22: the accepted pre-execution setup boundary owns the legal task
  // preset, which role produces this operation's result, and which model
  // that role resolves to (including the one-time override, which only ever
  // touches that primary role).
  OperationSetupRequest setup_request;
  setup_request.project_id = composition.project_id;
  setup_request.chat_id = composition.chat_id;
  setup_request.legal_task_preset_id = composition.legal_task_preset_id;
  setup_request.legal_task_preset_version = composition.legal_task_preset_version;
  setup_request.one_time_primary_model_override = composition.one_time_primary_model_override;
  OperationSetup setup = resolve_operation_setup(setup_request);
  if (!setup.primary_model && intent.capability != professional::ProfessionalCapability::ProductState) {
    throw std::runtime_error(
        "Choose a model for this request in AI Setup before reviewing what leaves this device.");
  }

  // The exact params the canonical preparation path already reads
  // (MatterRequestExecution.cpp's task_attempt_spec_from_params()) - no new
  // request semantics are invented here, only Phase 21/22 truth expressed
  // in the shape that path has always consumed.
  nlohmann::json request_params;
  request_params["matterId"] = composition.project_id;
  request_params["prompt"] = composition.prompt;
  if (composition.chat_id) request_params["chatId"] = *composition.chat_id;
  request_params["legalTaskPresetId"] = setup.legal_task_preset_id;
  request_params["legalTaskPresetVersion"] = setup.legal_task_preset_version;
	if (setup.primary_model) {
		request_params["providerId"] = setup.primary_model->provider_id;
		request_params["modelId"] = setup.primary_model->model_id;
		// The task used to record "operation_override" for every request,
		// whatever the person had actually configured, which made a Project's
		// own AI Setup unprovable from its own operation records.
		request_params["aiSetupSource"] = routing::to_string(setup.primary_model_source);
		request_params["operationRole"] = routing::to_string(setup.primary_result_role);
	}
  // Always present, even empty: an absent key would make that path inherit
  // the Chat's own persisted starting selection, silently overriding a
  // composer that explicitly chose Relevant Project sources for this one
  // request. Empty is exactly what Relevant means to the existing
  // ContextRetriever call inside prepare_task_request().
  request_params["selectedDocumentIds"] = sources.selected_document_ids;
	if (composition.target_document_id)
		request_params["targetDocumentId"] = *composition.target_document_id;
	if (composition.retry_of_task_id)
		request_params["retryOfTaskId"] = *composition.retry_of_task_id;
	if (composition.can_ask_lawyer) request_params["canAskLawyer"] = true;
	if (composition.unattended) request_params["unattended"] = true;
	if (composition.agent_initiated) request_params["agentInitiated"] = true;
	if (!composition.agent_id.empty()) request_params["agentId"] = composition.agent_id;
	if (!composition.agent_name.empty()) request_params["agentName"] = composition.agent_name;
	if (!composition.agent_brief.empty()) request_params["agentBrief"] = composition.agent_brief;
	if (!composition.answers_message_id.empty()) request_params["answersMessageId"] = composition.answers_message_id;
	if (!composition.author_actor_id.empty()) {
		request_params["authorActorId"] = composition.author_actor_id;
		request_params["authorName"] = composition.author_name;
	}
	request_params["professionalExecutionIntent"] =
		professional::professional_execution_intent_json(intent);

  PreparedChatRequest prepared;
	prepared.is_external_destination = setup.primary_model && is_external_target(*setup.primary_model);
  prepared.sources = std::move(sources);
  prepared.setup = std::move(setup);
	prepared.professional_intent = std::move(intent);
  prepared.request_params = std::move(request_params);
  return prepared;
}

namespace {

std::mutex &kept_here_mutex() {
  static auto *mutex = new std::mutex();
  return *mutex;
}

KeptHereResolver &kept_here_resolver() {
  static auto *resolver = new KeptHereResolver();
  return *resolver;
}

} // namespace

void set_kept_here_resolver(KeptHereResolver resolver) {
  const std::lock_guard<std::mutex> lock(kept_here_mutex());
  kept_here_resolver() = std::move(resolver);
}

ChatProtectionState resolve_protection_state(const ChatRequestComposition &composition) {
  OperationSetupRequest setup_request;
  setup_request.project_id = composition.project_id;
  setup_request.chat_id = composition.chat_id;
  setup_request.legal_task_preset_id = composition.legal_task_preset_id;
  setup_request.legal_task_preset_version = composition.legal_task_preset_version;
  setup_request.one_time_primary_model_override = composition.one_time_primary_model_override;
  const OperationSetup setup = resolve_operation_setup(setup_request);

  ChatProtectionState state;
  state.primary_result_role = routing::to_string(setup.primary_result_role);
  state.primary_model = setup.primary_model;
  state.destination = !setup.primary_model ? "unassigned"
                      : is_external_target(*setup.primary_model) ? "external"
                                                                 : "local";
  // Work kept on this computer runs here, on the model the send would use,
  // whatever AI Setup would choose: the composer says what the send does.
  KeptHereResolver resolver;
  {
    const std::lock_guard<std::mutex> lock(kept_here_mutex());
    resolver = kept_here_resolver();
  }
  if (resolver) {
    std::optional<KeptHereModel> kept;
    try {
      kept = resolver(composition);
    } catch (const std::exception &) {
    }
    if (kept) {
      state.kept_here = kept->reason;
      state.kept_here_message = kept->message;
      state.primary_model = kept->model;
      state.destination = "local";
    }
  }

  // The same live policy prepare_task_request() resolves for a real
  // attempt, so the composer never displays a protection claim the request
  // pipeline would not honour.
  const auto policy = platform::load_app_settings().local_node_setup.protected_api_policy;
  state.protection_mode = protected_api::to_string(policy.mode);
  if (policy.mode != protected_api::Mode::Off && routing::is_papinian_model_loading()) {
    state.is_protection_ready = false;
    state.is_protection_loading = true;
    state.protection_message = "Papinian Guard is loading.";
    return state;
  }
  try {
    routing::require_papinian_model_ready(policy);
    state.is_protection_ready = true;
  } catch (const std::exception &error) {
    state.is_protection_ready = false;
    state.protection_message = error.what();
  }
  return state;
}

nlohmann::json preview_chat_outbound_request(const ChatRequestComposition &composition) {
  const PreparedChatRequest prepared = prepare_chat_request(composition);

  // The existing Phase 11 boundary owns everything sensitive from here on:
  // the real routing::preview() capture, the second protect() call over the
  // same prepared prompt/policy, the redacted occurrence summary and the
  // ephemeral preview session token. Nothing below inspects, recomputes or
  // reshapes any of it.
  nlohmann::json preview =
      routing::matter_request_execution::preview_outbound_request(prepared.request_params);

  // Chat-facing facts, each already resolved above by the boundary that
  // owns it - never a count, destination or task name derived here.
  preview["destination"] = prepared.is_external_destination ? "external" : "local";
  preview["sourcePolicyMode"] = to_string(prepared.sources.mode);
  preview["selectedDocumentIds"] = prepared.sources.selected_document_ids;
  preview["legalTaskPresetId"] = prepared.setup.legal_task_preset_id;
  preview["legalTaskPresetVersion"] = prepared.setup.legal_task_preset_version;
  preview["legalTaskPresetName"] = prepared.setup.legal_task_preset_name;
  preview["primaryResultRole"] = routing::to_string(prepared.setup.primary_result_role);
  preview["primaryModelSource"] = routing::to_string(prepared.setup.primary_model_source);
  preview["oneTimeOverrideApplied"] = prepared.setup.one_time_override_applied;
	const research::ResearchMode researchMode = research::resolve_research_mode(composition.prompt);
	preview["researchMode"] = research::to_string(researchMode);
	preview["webResearchUsesInternet"] = researchMode != research::ResearchMode::Off;
	preview["outboundBoundaries"] = nlohmann::json::array();
	if (prepared.is_external_destination) preview["outboundBoundaries"].push_back("model_request");
	if (researchMode != research::ResearchMode::Off) {
		preview["outboundBoundaries"].push_back("search_query");
		preview["outboundBoundaries"].push_back("external_source_retrieval");
	}
  return preview;
}

} // namespace singularity::chat
