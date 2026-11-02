#pragma once

#include <filesystem>
#include <string>

namespace singularity::workspaces {

// A short, opaque, locally-unique id for a LocalWorkspace entry
// (app_settings.hpp) - never derived from the path, so renaming/moving the
// folder later (via workspace.relocate) does not orphan it.
std::string generate_workspace_id();

// A reasonable default display name for a newly added workspace: the
// folder's own name (e.g. "/Users/j/Clients/Northbridge" -> "Northbridge").
std::string derive_display_name(const std::filesystem::path &path);

// The current instant, as ISO 8601 UTC (e.g. "2026-08-19T12:00:00Z") - used
// for LocalWorkspace::created_at/last_opened_at.
std::string current_timestamp();

// Rejects a folder that does not exist, is not a directory, or cannot be
// written to, before it is ever persisted as a workspace. Probes with a
// real, immediately-removed file rather than trusting filesystem
// permission bits, which do not reliably predict write access on every
// platform (ACLs, sandboxing, network volumes).
//
// Throws std::runtime_error with a plain-language message on rejection.
void validate_directory(const std::filesystem::path &path);

// Non-throwing form of validate_directory(), for checking whether a
// workspace saved on a previous run is still there - a folder can vanish
// (moved, unmounted, deleted) between one launch and the next, and that is
// not this app's error to raise, just a state to report (see
// Bridge/NativeBridge/NativeBridge.cpp's workspace.list).
bool directory_available(const std::filesystem::path &path);

} // namespace singularity::workspaces
