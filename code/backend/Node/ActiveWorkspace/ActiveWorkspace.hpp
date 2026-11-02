#pragma once

#include <filesystem>
#include <optional>

namespace singularity::node {

// Compatibility resolver for the active legacy Node-wide workspace. The
// `.singularity-workspace` to `.lm` migration is intentionally outside this component.
std::filesystem::path require_active_workspace_path();

// The same resolution without provisioning anything: std::nullopt only for a
// Node that has never been set up (no active workspace chosen yet), which has
// no Projects rather than a problem. An active workspace that is missing or
// no longer available still throws exactly as require_active_workspace_path().
std::optional<std::filesystem::path> configured_workspace_path();

// Same resolution as require_active_workspace_path(), except a genuinely
// unconfigured Node (a fresh install, or one where the user declined the
// separate broader-access ceremony) silently provisions one real, sensible
// default workspace instead of throwing - so a direct "New Project" action
// never needs to force that ceremony first (Beta UX correction: creating a
// Project must be as immediate as `git init`, never gated on a first
// choosing a broad folder Singularity then scans for existing Projects). The
// broader-access flow in Settings remains available, unchanged, for anyone
// who wants it - this only removes it as a hard prerequisite.
std::filesystem::path ensure_active_workspace_path();

// Records the user's access to `folder` (Settings > Permissions) as a selected
// folder without changing the active workspace whose registry lists the
// Projects: existing Projects stay listed and nothing is written into the
// folder. A Node with no active workspace first gets its own data workspace
// (ensure_active_workspace_path). Granting a folder already recorded changes
// nothing. Throws if the folder cannot be used.
void grant_folder_access(const std::filesystem::path &folder);

} // namespace singularity::node
