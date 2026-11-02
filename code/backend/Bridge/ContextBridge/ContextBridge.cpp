#include "Bridge/ContextBridge/ContextBridge.hpp"

#include "Bridge/MatterBridge/MatterBridge.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/MatterRequestExecution/MatterRequestExecution.hpp"
#include "WorkProduct/WorkflowDefinitions/WorkflowDefinitions.hpp"

#include <nlohmann/json.hpp>

namespace singularity::bridge::context_bridge {
namespace {

using Json = nlohmann::json;

Json usage_json(const providers::UsageMetadata &usage) {
  if (!usage.input_tokens && !usage.output_tokens && !usage.total_tokens) return nullptr;
  return Json{{"inputTokens", usage.input_tokens ? Json(*usage.input_tokens) : Json(nullptr)},
              {"outputTokens", usage.output_tokens ? Json(*usage.output_tokens) : Json(nullptr)},
              {"totalTokens", usage.total_tokens ? Json(*usage.total_tokens) : Json(nullptr)}};
}

Json privacy_json(const protected_api::papinian::PrivacyMetadata &metadata) {
  return Json{{"mode", metadata.mode},
              {"papinianGuardVersion", metadata.papinian_guard_version},
              {"papinianNerVersion", metadata.papinian_ner_version},
              {"protectedCount", metadata.protected_count},
              {"categoryCounts", metadata.category_counts},
			  {"responseIntegrity", metadata.response_integrity},
			  {"finalPayloadValidated", metadata.final_payload_validated},
			  {"finalPayloadSha256", metadata.final_payload_sha256},
			  {"protectedProjectEntityIds", metadata.protected_project_entity_ids}};
}

Json workflow_step_json(const context::WorkflowStepDefinition &step) {
  const bool inference = step.kind == context::WorkflowStepKind::Inference;
  return Json{{"id", step.id}, {"name", step.name},
              {"kind", inference ? "inference" : "createIssues"},
              {"presetId", inference ? Json(step.preset_id) : Json(nullptr)},
              {"presetVersion", inference ? Json(step.preset_version) : Json(nullptr)},
              {"instructions", inference ? Json(step.instructions) : Json(nullptr)}};
}

// A legacy run hands the domain only the keys its contract declares (Beta 1
// #269). The domain's request parser also serves Singularity's own runs
// (ChatExecution), which say who asked, on whose behalf and how the work may
// proceed: authorActorId, agentId, agentInitiated, unattended, operationRole
// and more. A page says none of that, so none of it passes from here, and a
// page cannot sign a task in another member's name.
Json declared_params(const Json &params, std::initializer_list<const char *> keys) {
  Json declared = Json::object();
  for (const char *key : keys) {
    const auto found = params.find(key);
    if (found != params.end()) declared[key] = *found;
  }
  return declared;
}

} // namespace


nlohmann::json run_task(const nlohmann::json &params) {
  return matter_bridge::serialize(routing::matter_request_execution::run_task(declared_params(params,
      {"matterId", "prompt", "providerId", "modelId", "chatId", "legalTaskPresetId", "legalTaskPresetVersion",
       "promptProfileId", "promptProfileVersion", "selectedDocumentIds", "expectedDocumentVersions"})));
}
nlohmann::json preview_outbound_request(const nlohmann::json &params) {
  return routing::matter_request_execution::preview_outbound_request(declared_params(params,
      {"matterId", "prompt", "providerId", "modelId", "chatId", "legalTaskPresetId", "legalTaskPresetVersion",
       "promptProfileId", "promptProfileVersion", "selectedDocumentIds", "expectedDocumentVersions"}));
}
nlohmann::json reveal_protected_occurrence(const nlohmann::json &params) {
  return routing::matter_request_execution::reveal_protected_occurrence(params);
}
nlohmann::json close_outbound_preview(const nlohmann::json &params) {
  return routing::matter_request_execution::close_outbound_preview(params);
}
namespace {

// A Chat task is re-run as a new canonical Chat operation (tool context,
// approvals, Stop, durable lifecycle); any other task keeps the direct attempt.
std::optional<matters::MatterTask> task_of(const std::string &project_id, const std::string &task_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto projects = matters::load_matters(node::require_active_workspace_path());
  const auto project = std::find_if(projects.begin(), projects.end(),
      [&](const matters::Matter &entry) { return entry.id == project_id; });
  if (project == projects.end()) return std::nullopt;
  const auto task = std::find_if(project->tasks.begin(), project->tasks.end(),
      [&](const matters::MatterTask &entry) { return entry.id == task_id; });
  if (task == project->tasks.end()) return std::nullopt;
  return *task;
}

nlohmann::json project_after(const std::string &project_id) {
  const auto projects = matter::list();
  const auto project = std::find_if(projects.begin(), projects.end(),
      [&](const matters::Matter &entry) { return entry.id == project_id; });
  if (project == projects.end()) throw std::runtime_error("This Project no longer exists.");
  return matter_bridge::serialize(*project);
}

} // namespace

nlohmann::json retry_task(const nlohmann::json &params) {
  const std::string project_id = params.at("matterId").get<std::string>();
  const std::string task_id = params.at("taskId").get<std::string>();
  const std::optional<matters::MatterTask> original = task_of(project_id, task_id);
  if (!original || !original->chat_id || original->consilium_group_id || original->workflow_run_id)
    return matter_bridge::serialize(routing::matter_request_execution::retry_task(params));
  const bool has_provider_id = params.contains("providerId") && params.at("providerId").is_string();
  const bool has_model_id = params.contains("modelId") && params.at("modelId").is_string();
  if (has_provider_id != has_model_id)
    throw std::runtime_error("To run with another model, choose both a provider and a model.");
  chat::ChatRerunRequest request;
  if (has_provider_id && has_model_id)
    request.model_target = routing::ModelTarget{params.at("providerId").get<std::string>(), params.at("modelId").get<std::string>()};
  (void)chat::rerun_chat_task(project_id, task_id, request);
  return project_after(project_id);
}

nlohmann::json revise_task(const nlohmann::json &params) {
  const std::string project_id = params.at("matterId").get<std::string>();
  const std::string task_id = params.at("taskId").get<std::string>();
  const std::optional<matters::MatterTask> original = task_of(project_id, task_id);
  if (!original || !original->chat_id || original->consilium_group_id || original->workflow_run_id)
    return matter_bridge::serialize(routing::matter_request_execution::revise_task(params));
  chat::ChatRerunRequest request;
  if (params.contains("prompt") && params.at("prompt").is_string()) request.prompt = params.at("prompt").get<std::string>();
  // A revise with no new words prepares the same instruction again against the
  // documents as they are now - an explicit re-preparation, never a same-inputs
  // Retry (which a changed document or source would only refuse again).
  if (!request.prompt) request.prompt = original->prompt;
  if (params.contains("sourcePolicyMode") && params.at("sourcePolicyMode").is_string()) {
    const std::string mode = params.at("sourcePolicyMode").get<std::string>();
    if (mode != "relevant" && mode != "selected") throw std::runtime_error("This is not a real source policy.");
    request.source_policy_mode = mode == "selected" ? chat::SourcePolicyMode::Selected : chat::SourcePolicyMode::Relevant;
    if (params.contains("selectedDocumentIds") && params.at("selectedDocumentIds").is_array())
      for (const auto &document_id : params.at("selectedDocumentIds"))
        if (document_id.is_string()) request.selected_document_ids.push_back(document_id.get<std::string>());
  }
  (void)chat::rerun_chat_task(project_id, task_id, request);
  return project_after(project_id);
}
nlohmann::json run_consilium(const nlohmann::json &params) {
  return matter_bridge::serialize(routing::matter_request_execution::run_consilium(declared_params(params,
      {"matterId", "configurationId", "prompt", "targets", "chatId", "legalTaskPresetId", "legalTaskPresetVersion",
       "promptProfileId", "promptProfileVersion", "selectedDocumentIds"})));
}
nlohmann::json synthesize_consilium(const nlohmann::json &params) {
  return matter_bridge::serialize(routing::matter_request_execution::synthesize_consilium(params));
}
nlohmann::json list_workflows(const nlohmann::json &) {
  Json result = Json::array();
  for (const auto &workflow : context::workflow_definitions()) {
    Json steps = Json::array();
    for (const auto &step : workflow.steps) steps.push_back(workflow_step_json(step));
    result.push_back({{"id", workflow.id}, {"name", workflow.name},
                      {"version", workflow.version}, {"steps", std::move(steps)}});
  }
  return result;
}
nlohmann::json run_workflow(const nlohmann::json &params) {
  return matter_bridge::serialize(routing::matter_request_execution::run_workflow(params));
}

} // namespace singularity::bridge::context_bridge
