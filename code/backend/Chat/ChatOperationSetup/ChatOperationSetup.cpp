#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/LegalTaskPresets/LegalTaskPresets.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::chat {
namespace {

// Node default AI Setup, expressed the way Routing/AiSetup/AiSetup.hpp's
// resolver expects. Mirrors Bridge/RoutingBridge/RoutingBridge.cpp's own
// node_default_override_set() - both read the identical
// LocalNodeSetup::role_assignments store, so this Chat operation-setup
// resolution can never disagree with what getEffectiveAiSetup shows the
// user for the same Node.
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

// Resolves the requested legal_task_preset_id/version against
// LegalTaskPresets.hpp's own registry - an explicit version must match
// exactly; an omitted version resolves that id's current (highest) known
// version. Never silently substitutes General for an unrecognized id
// (LegalTaskPreset.hpp's own find_legal_task_preset() contract).
context::LegalTaskPreset resolve_legal_task_preset(
    const std::optional<std::string> &requested_id, const std::optional<int> &requested_version) {
  const std::string id = requested_id.value_or(std::string(context::kDefaultLegalTaskPresetId));
  if (requested_version) {
    const auto found = context::find_legal_task_preset(id, *requested_version);
    if (!found) throw std::runtime_error("This legal task is not available.");
    return *found;
  }
  std::optional<context::LegalTaskPreset> current;
  for (const auto &preset : context::legal_task_presets()) {
    if (preset.id != id) continue;
    if (!current || preset.version > current->version) current = preset;
  }
  if (!current) throw std::runtime_error("This legal task is not available.");
  return *current;
}

} // namespace

OperationSetup resolve_operation_setup(const OperationSetupRequest &request) {
  const context::LegalTaskPreset preset =
      resolve_legal_task_preset(request.legal_task_preset_id, request.legal_task_preset_version);
  const auto operation_kind = routing::operation_kind_for_legal_task_preset(preset.id);
  const routing::TaskProfile &profile = routing::task_profile_for(operation_kind);

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  const auto all = matters::load_matters(workspace_path);
  const auto found_project = std::find_if(all.begin(), all.end(),
      [&](const matters::Matter &entry) { return entry.id == request.project_id; });
  if (found_project == all.end()) throw std::runtime_error("This Project is no longer available.");

  const matters::MatterChat *chat = nullptr;
  if (request.chat_id) {
    const auto found_chat = std::find_if(found_project->chats.begin(), found_project->chats.end(),
        [&](const matters::MatterChat &entry) { return entry.id == *request.chat_id; });
    if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
    chat = &(*found_chat);
  }

  const auto node_default = node_default_override_set(platform::load_app_settings().local_node_setup);
  const auto project_override = override_set_from(found_project->ai_setup_overrides);
  const auto chat_override = chat != nullptr ? override_set_from(chat->ai_setup_overrides) : routing::AiSetupOverrideSet{};

  routing::AiSetupOverrideSet operation_override;
  if (request.one_time_primary_model_override) {
    operation_override.push_back(
        routing::RoleAssignment{profile.primary_result_role, *request.one_time_primary_model_override});
  }

  const auto resolved = routing::resolve_effective_setup(node_default, project_override, chat_override, operation_override);

  OperationSetup result;
  result.legal_task_preset_id = std::string(preset.id);
  result.legal_task_preset_version = preset.version;
  result.legal_task_preset_name = std::string(preset.name);
  result.primary_result_role = profile.primary_result_role;
  result.one_time_override_applied = request.one_time_primary_model_override.has_value();
  result.effective_roles.reserve(resolved.size());
  for (const auto &entry : resolved) {
    result.effective_roles.push_back(ResolvedOperationRole{entry.role, entry.model, entry.source});
    if (entry.role == profile.primary_result_role) {
      result.primary_model = entry.model;
      result.primary_model_source = entry.source;
    }
  }
  return result;
}

} // namespace singularity::chat
