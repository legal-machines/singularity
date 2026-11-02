#include "Bridge/RoutingBridge/RoutingBridge.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace singularity::bridge::routing_bridge {
namespace {

using Json = nlohmann::json;

// Node default AI Setup, expressed the way Routing/AiSetup/AiSetup.hpp's
// resolver expects. role_assignments is the single canonical store for
// every Beta 1 role including Reasoning - platform::load_app_settings()
// already migrates a legacy `defaultModel`-only settings.json into it, so
// this never needs to special-case Reasoning separately.
routing::AiSetupOverrideSet node_default_override_set(const platform::LocalNodeSetup &setup) {
  routing::AiSetupOverrideSet overrides;
  for (const auto &assignment : setup.role_assignments) {
    if (const auto role = routing::ai_setup_role_from_string(assignment.role)) {
      overrides.push_back(
          routing::RoleAssignment{*role, routing::ModelTarget{assignment.provider_id, assignment.model_id}});
    }
  }
  return overrides;
}

routing::AiSetupOverrideSet override_set_from(const std::vector<matters::AiSetupRoleOverride> &entries) {
  routing::AiSetupOverrideSet overrides;
  for (const auto &entry : entries) {
    if (const auto role = routing::ai_setup_role_from_string(entry.role)) {
      overrides.push_back(routing::RoleAssignment{*role, routing::ModelTarget{entry.provider_id, entry.model_id}});
    }
  }
  return overrides;
}

matters::Matter require_matter(const std::string &project_id) {
  const auto all = matters::load_matters(node::require_active_workspace_path());
  const auto found = std::find_if(all.begin(), all.end(),
      [&](const matters::Matter &entry) { return entry.id == project_id; });
  if (found == all.end()) throw std::runtime_error("This project no longer exists.");
  return *found;
}

Json resolved_role_json(const routing::ResolvedRoleAssignment &resolved) {
  return Json{
      {"role", routing::to_string(resolved.role)},
      {"source", routing::to_string(resolved.source)},
      {"providerId", resolved.model ? Json(resolved.model->provider_id) : Json(nullptr)},
      {"modelId", resolved.model ? Json(resolved.model->model_id) : Json(nullptr)},
  };
}

Json eligible_local_model_json(const hardware::LocalModelStatus &status) {
  return Json{
      {"source", "local"},
      {"providerId", "local"},
      {"modelId", status.descriptor.id},
      {"displayName", status.descriptor.display_name},
      {"family", status.descriptor.family},
      // Legal Machines-developed families (Papinian, Modestin) show their
      // real developer identity here, matching card-level provider identity
      // for provider models below - never the upstream checkpoint name
      // (LocalModelRegistry.hpp's own top comment).
      {"developer", status.descriptor.developer},
  };
}

Json eligible_provider_model_json(const std::string &provider_id, const providers::ModelDescriptor &model) {
  return Json{
      {"source", "provider"},
      {"providerId", provider_id},
      {"modelId", model.id},
      {"displayName", model.display_name ? Json(*model.display_name) : Json(model.id)},
      {"family", nullptr},
  };
}

} // namespace

nlohmann::json get_ai_setup_capabilities(const nlohmann::json &) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto settings = platform::load_app_settings();
  const auto local_statuses = routing::production_local_model_registry().list_status();

  Json roles = Json::array();
  for (const auto role : routing::all_ai_setup_roles()) {
    Json eligible_models = Json::array();
    for (const auto &status : local_statuses) {
      if (!status.installed) continue;
      if (routing::is_local_model_compatible(role, status.descriptor.family,
                                             status.descriptor.execution_profile.architecture_kind)) {
        eligible_models.push_back(eligible_local_model_json(status));
      }
    }
    for (const auto &provider : settings.local_node_setup.connected_providers) {
      for (const auto &model : provider.models) {
        if (routing::is_provider_model_compatible(role, model.text_capable)) {
          eligible_models.push_back(eligible_provider_model_json(provider.id, model));
        }
      }
    }
    const bool assignable = routing::is_role_assignable(role);
    roles.push_back(Json{
        {"role", routing::to_string(role)},
        {"label", routing::display_label(role)},
        {"requiresLocalFamily", routing::requires_local_family(role)},
        {"compatibleFamilies", routing::compatible_local_families(role)},
        // SET-2: a role Singularity fills itself is reported as such, with the
        // reason, so the UI never offers a choice that changes nothing - and
        // never implies a person's own privacy protection is theirs to weaken.
        {"assignable", assignable},
        {"fixedReason", assignable ? Json(nullptr) : Json(routing::fixed_role_reason(role))},
        {"eligibleModels", assignable ? std::move(eligible_models) : Json::array()},
    });
  }
  return Json{{"roles", std::move(roles)}};
}

nlohmann::json get_effective_ai_setup(const nlohmann::json &params) {
  std::optional<std::string> project_id;
  if (params.contains("projectId") && params.at("projectId").is_string()) {
    project_id = params.at("projectId").get<std::string>();
  }
  std::optional<std::string> chat_id;
  if (params.contains("chatId") && params.at("chatId").is_string()) {
    chat_id = params.at("chatId").get<std::string>();
  }
  if (chat_id && !project_id) {
    throw std::runtime_error("A chat's effective AI Setup needs its project.");
  }

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto node_default = node_default_override_set(platform::load_app_settings().local_node_setup);

  routing::AiSetupOverrideSet project_override;
  routing::AiSetupOverrideSet chat_override;
  if (project_id) {
    const auto matter = require_matter(*project_id);
    project_override = override_set_from(matter.ai_setup_overrides);
    if (chat_id) {
      const auto found_chat = std::find_if(matter.chats.begin(), matter.chats.end(),
          [&](const matters::MatterChat &entry) { return entry.id == *chat_id; });
      if (found_chat == matter.chats.end()) throw std::runtime_error("This chat no longer exists.");
      chat_override = override_set_from(found_chat->ai_setup_overrides);
    }
  }

  const routing::AiSetupOverrideSet no_operation_override{};
  const auto resolved =
      routing::resolve_effective_setup(node_default, project_override, chat_override, no_operation_override);
  Json roles = Json::array();
  for (const auto &entry : resolved) roles.push_back(resolved_role_json(entry));
  return Json{{"roles", std::move(roles)}};
}

} // namespace singularity::bridge::routing_bridge
