#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace singularity::node {

std::filesystem::path require_active_workspace_path() {
  if (auto path = configured_workspace_path()) return *path;
  throw std::runtime_error("No local workspace is configured. Choose one in Settings.");
}

std::optional<std::filesystem::path> configured_workspace_path() {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  // Read on every Project read: shared, never copied.
  const std::shared_ptr<const platform::AppSettings> settings = platform::load_app_settings_shared();
  const auto &setup = settings->local_node_setup;
  if (!setup.active_workspace_id) return std::nullopt;
  const auto found = std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
      [&](const platform::LocalWorkspace &workspace) { return workspace.id == *setup.active_workspace_id; });
  if (found == setup.workspaces.end()) {
    throw std::runtime_error("No local workspace is configured. Choose one in Settings.");
  }
  if (!workspaces::directory_available(found->path)) {
    throw std::runtime_error("\"" + found->display_name +
                             "\" is no longer available. Locate it in Settings.");
  }
  return std::filesystem::path(found->path);
}

std::filesystem::path ensure_active_workspace_path() {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  auto &setup = settings.local_node_setup;
  const auto find_active = [&]() {
    return setup.active_workspace_id
        ? std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
              [&](const platform::LocalWorkspace &workspace) { return workspace.id == *setup.active_workspace_id; })
        : setup.workspaces.end();
  };
  auto active = find_active();
  if (active == setup.workspaces.end()) {
    // Genuinely unconfigured (fresh install, or a decline of the separate
    // broader-access ceremony) - provision one real, sensible default
    // workspace silently, exactly as if the user had picked it themselves.
    const std::filesystem::path defaultPath = platform::application_data_directory() / "Workspace";
    std::filesystem::create_directories(defaultPath);
    platform::LocalWorkspace workspace;
    workspace.id = workspaces::generate_workspace_id();
    workspace.display_name = workspaces::derive_display_name(defaultPath);
    const auto canonical = std::filesystem::weakly_canonical(defaultPath).u8string();
    workspace.path = std::string(canonical.begin(), canonical.end());
    workspace.created_at = workspaces::current_timestamp();
    setup.workspaces.push_back(workspace);
    setup.active_workspace_id = workspace.id;
    platform::save_app_settings(settings);
    active = find_active();
  }
  if (!workspaces::directory_available(active->path)) {
    throw std::runtime_error("\"" + active->display_name +
                             "\" is no longer available. Locate it in Settings.");
  }
  return std::filesystem::path(active->path);
}

void grant_folder_access(const std::filesystem::path &folder) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  workspaces::validate_directory(folder);
  {
    const auto settings = platform::load_app_settings();
    const auto &setup = settings.local_node_setup;
    const bool hasActive = setup.active_workspace_id
        && std::any_of(setup.workspaces.begin(), setup.workspaces.end(),
            [&](const platform::LocalWorkspace &workspace) { return workspace.id == *setup.active_workspace_id; });
    if (!hasActive) (void)ensure_active_workspace_path();
  }
  auto settings = platform::load_app_settings();
  auto &setup = settings.local_node_setup;
  const auto canonicalWide = std::filesystem::weakly_canonical(folder).u8string();
  const std::string canonical(canonicalWide.begin(), canonicalWide.end());
  const bool known = std::any_of(setup.workspaces.begin(), setup.workspaces.end(),
      [&](const platform::LocalWorkspace &workspace) { return workspace.path == canonical; });
  if (known) return;
  platform::LocalWorkspace workspace;
  workspace.id = workspaces::generate_workspace_id();
  workspace.display_name = workspaces::derive_display_name(folder);
  workspace.path = canonical;
  workspace.created_at = workspaces::current_timestamp();
  setup.workspaces.push_back(workspace);
  platform::save_app_settings(settings);
}

} // namespace singularity::node
