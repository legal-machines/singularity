#include "Bridge/PlatformBridge/PlatformBridge.hpp"

#include "Bridge/NodeBridge/NodeBridge.hpp"
#include "Office/OfficeService/OfficeService.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/PermissionAccess/PermissionAccess.hpp"
#include "Platform/SystemPermissions/SystemPermissions.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace singularity::bridge::platform_bridge {
namespace {

using Json = nlohmann::json;

// settings is needed alongside the derived FilesAccessState because
// broad_project_discovery_authorization and the resolved discovery scope
// live on AppSettings itself, not on the selected-folder-only FilesAccessState.
Json permission_json(const platform::AppSettings &settings) {
	const platform::FilesAccessState state = platform::inspect_files_access(settings);
	const platform::ProjectDiscoveryScope scope = platform::resolve_project_discovery_scope(settings);

	Json folders = Json::array();
	int availableFolderCount = 0;
	for (const platform::SelectedFolderAccess &folder : state.folders) {
		folders.push_back(Json{
			{"id", folder.id},
			{"displayName", folder.display_name},
			{"path", folder.path},
			{"available", folder.is_available},
		});
		if (folder.is_available == true) ++availableFolderCount;
	}

	return Json{
		{"filesAndProjects", Json{
			{"decision", platform::to_string(state.decision)},
			{"status", platform::to_string(state.status)},
			// Reflects what resolve_project_discovery_scope() actually resolved
			// (authorization AND live capability), never just the authorization
			// flag alone - see broadAccess.authorized below for that.
			{"scope", scope.mode == platform::ProjectDiscoveryMode::Broad ? "broad" : "selected-folders"},
			{"availableFolderCount", availableFolderCount},
			{"unavailableFolderCount", static_cast<int>(state.folders.size()) - availableFolderCount},
			{"folders", std::move(folders)},
			{"broadAccess", Json{
				// The user's explicit authorization alone - independent of
				// whether a broad root happens to be accessible right now, so
				// the UI can distinguish "authorized" from "actually finding
				// Projects broadly at this moment" (the latter is `scope`).
				{"authorized", settings.broad_project_discovery_authorization
					== platform::BroadProjectDiscoveryAuthorization::Authorized},
			}},
		}},
	};
}

Json current_permissions() {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return permission_json(platform::load_app_settings());
}

void save_decision(platform::FilesAccessDecision decision, bool isOnlyWhenNoFolders) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	platform::AppSettings settings = platform::load_app_settings();
	if (isOnlyWhenNoFolders == true && settings.local_node_setup.workspaces.empty() == false) return;
	settings.files_access_decision = decision;
	platform::save_app_settings(settings);
}

} // namespace

nlohmann::json get_permissions(const nlohmann::json &) {
	return current_permissions();
}

nlohmann::json decline_files_and_projects(const nlohmann::json &) {
	save_decision(platform::FilesAccessDecision::Declined, false);
	return current_permissions();
}

nlohmann::json choose_files_and_projects(const std::optional<std::string> &selected_path) {
	// Granting access to a folder never switches the workspace whose registry
	// lists the Projects (BB-25): existing Projects stay listed.
	if (selected_path.has_value() == true) (void)node_bridge::grant_folder_access(*selected_path);
	if (selected_path.has_value() == true) {
		save_decision(platform::FilesAccessDecision::Granted, false);
	} else {
		save_decision(platform::FilesAccessDecision::Declined, true);
	}
	return current_permissions();
}

nlohmann::json allow_broader_access(const nlohmann::json &) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	platform::AppSettings settings = platform::load_app_settings();
	settings.broad_project_discovery_authorization = platform::BroadProjectDiscoveryAuthorization::Authorized;
	platform::save_app_settings(settings);
	return permission_json(settings);
}

nlohmann::json revoke_broader_access(const nlohmann::json &) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	platform::AppSettings settings = platform::load_app_settings();
	settings.broad_project_discovery_authorization = platform::BroadProjectDiscoveryAuthorization::NotAuthorized;
	platform::save_app_settings(settings);
	return permission_json(settings);
}

namespace {

Json system_permission_json(const platform::system_permissions::Permission &permission) {
	return Json{{"key", permission.key}, {"state", platform::system_permissions::word(permission.state)}, {"problem", permission.problem}};
}

std::string permission_key(const Json &params) {
	const std::string key = params.at("key").get<std::string>();
	if (key.empty() || key.size() > 40) throw std::invalid_argument("Name the permission.");
	return key;
}

} // namespace

// The certificate Office's panes need (D-244) is the Office domain's own, asked
// there; the system's other permissions are the platform's. The Bridge only
// names which one answers. It is always listed, not needed where no Office is
// installed, so onboarding shows its row only where Office is.
constexpr const char *kOfficeCertificate = "officeCertificate";

Json system_permissions(const Json &) {
	Json permissions = Json::array();
	for (const auto &permission : platform::system_permissions::list()) permissions.push_back(system_permission_json(permission));
	permissions.push_back(system_permission_json(office::office_certificate_permission()));
	return Json{{"permissions", std::move(permissions)}};
}

Json ask_system_permission(const Json &params) {
	const std::string key = permission_key(params);
	if (key == kOfficeCertificate) return system_permission_json(office::ask_office_certificate_trust());
	return system_permission_json(platform::system_permissions::ask(key));
}

Json open_system_permission_settings(const Json &params) {
	const std::string key = permission_key(params);
	if (key == kOfficeCertificate) return office::open_office_certificate_settings();
	return platform::system_permissions::open_settings(key);
}

} // namespace singularity::bridge::platform_bridge
