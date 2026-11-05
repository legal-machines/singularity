#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace singularity::bridge::platform_bridge {

nlohmann::json get_permissions(const nlohmann::json &params);
nlohmann::json decline_files_and_projects(const nlohmann::json &params);
nlohmann::json choose_files_and_projects(const std::optional<std::string> &selected_path);

// Explicit user authorization for broader, system-wide Project discovery
// beyond selected folders (the Limited state's "Allow broader access"
// action) - distinct from choose_files_and_projects's selected-folder grant.
// See Platform/AppSettings::BroadProjectDiscoveryAuthorization and
// Platform/PermissionAccess::resolve_project_discovery_scope().
nlohmann::json allow_broader_access(const nlohmann::json &params);

// Turns broader-access authorization back off. Discovery falls back to
// Limited (selected folders only) as soon as this is called - see
// resolve_project_discovery_scope().
nlohmann::json revoke_broader_access(const nlohmann::json &params);

// What the computer itself allows Singularity, asked during onboarding
// before the work that needs it (Platform/SystemPermissions):
// system.permissions -> { permissions: [{ key, state, problem }] }, as known
// without asking; system.askPermission { key } -> { key, state, problem },
// after the system's own question; system.openPermissionSettings { key } ->
// whether the system's settings opened where it is allowed.
nlohmann::json system_permissions(const nlohmann::json &params);
nlohmann::json ask_system_permission(const nlohmann::json &params);
nlohmann::json open_system_permission_settings(const nlohmann::json &params);

} // namespace singularity::bridge::platform_bridge
