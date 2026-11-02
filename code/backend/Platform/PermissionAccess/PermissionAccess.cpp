#include "Platform/PermissionAccess/PermissionAccess.hpp"

#include "Platform/AppPaths/AppPaths.hpp"

#include <algorithm>
#include <filesystem>

namespace singularity::platform {

const char *to_string(FilesAccessStatus status) {
	switch (status) {
		case FilesAccessStatus::NotAsked:
			return "not-asked";
		case FilesAccessStatus::Declined:
			return "declined";
		case FilesAccessStatus::Limited:
			return "limited";
		case FilesAccessStatus::Revoked:
			return "revoked";
	}
	return "not-asked";
}

FilesAccessState inspect_files_access(const AppSettings &settings) {
	FilesAccessState state;
	state.decision = settings.files_access_decision;

	bool isAnyFolderAvailable = false;
	for (const LocalWorkspace &workspace : settings.local_node_setup.workspaces) {
		std::error_code error;
		const bool isAvailable = std::filesystem::is_directory(workspace.path, error) && !error;
		state.folders.push_back(SelectedFolderAccess{
			workspace.id,
			workspace.display_name,
			workspace.path,
			isAvailable,
		});
		if (isAvailable == true) isAnyFolderAvailable = true;
	}

	// Existing Alpha-compatible workspace choices predate this decision field.
	// A real configured folder is already an explicit user grant and must not
	// trigger the explanation again after upgrade.
	if (state.folders.empty() == false && state.decision == FilesAccessDecision::NotAsked) {
		state.decision = FilesAccessDecision::Granted;
	}

	if (state.folders.empty() == false && isAnyFolderAvailable == false) {
		state.status = FilesAccessStatus::Revoked;
		return state;
	}

	if (isAnyFolderAvailable == true || state.decision == FilesAccessDecision::Granted) {
		state.status = FilesAccessStatus::Limited;
		return state;
	}

	state.status = state.decision == FilesAccessDecision::Declined
		? FilesAccessStatus::Declined
		: FilesAccessStatus::NotAsked;
	return state;
}

namespace {

// Every location broad discovery should even consider - not yet filtered by
// whether this process can actually read it. Enumeration itself is cheap
// (readdir on /Volumes, a single env-derived path for home) and safe to
// attempt regardless of access; the real access decision happens in
// is_accessible_for_discovery() below, per candidate.
std::vector<std::filesystem::path> broad_local_access_candidates() {
	std::vector<std::filesystem::path> candidates;
	if (const std::optional<std::filesystem::path> home = home_directory()) candidates.push_back(*home);

#if defined(__APPLE__)
	std::error_code error;
	if (std::filesystem::is_directory("/Volumes", error) == true) {
		for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator("/Volumes", error)) {
			std::error_code entryError;
			if (entry.is_directory(entryError) == true && !entryError) candidates.push_back(entry.path());
		}
	}
#endif
	return candidates;
}

// The real, per-root access test: does this process hold the specific
// capability discovery itself needs from this location - opening it to list
// its immediate entries? A path can pass is_directory() while still being
// unreadable (permission bits, a macOS-protected or sandboxed volume, a
// stale/disconnected mount); only actually attempting to open it for
// listing surfaces that. Deliberately never consults any undocumented OS
// permission database - this is a direct filesystem measurement, nothing
// else.
bool is_accessible_for_discovery(const std::filesystem::path &path) {
	std::error_code error;
	if (std::filesystem::is_directory(path, error) == false || error) return false;

	std::filesystem::directory_iterator iterator(path, error);
	return !error;
}

} // namespace

std::vector<std::filesystem::path> broad_local_access_roots() {
	std::vector<std::filesystem::path> roots;
	for (const std::filesystem::path &candidate : broad_local_access_candidates()) {
		// An inaccessible candidate (a protected volume, a permission-denied
		// path) is simply excluded here - it never invalidates the other,
		// genuinely accessible candidates.
		if (is_accessible_for_discovery(candidate) == true) roots.push_back(candidate);
	}
	return roots;
}

ProjectDiscoveryScope resolve_project_discovery_scope_for_broad_roots(
		const AppSettings &settings, const std::vector<std::filesystem::path> &accessibleBroadRoots) {
	// Broad requires BOTH halves: the user's explicit
	// broad_project_discovery_authorization (never `files_access_decision`,
	// which is only ever set by choosing a specific folder or declining - see
	// Bridge/PlatformBridge's choose/decline_files_and_projects - and must
	// resolve Limited even when HOME is technically readable) AND at least
	// one broad candidate this process can genuinely read right now. Neither
	// half implies the other: authorization never means an inaccessible root
	// is readable, and accessibility never means the user authorized anything.
	if (settings.broad_project_discovery_authorization == BroadProjectDiscoveryAuthorization::Authorized
			&& accessibleBroadRoots.empty() == false) {
		// Broader access adds to the folders the user chose; it never drops one
		// - a firm's shared folder outside the home folder stays searched.
		std::vector<std::filesystem::path> roots = accessibleBroadRoots;
		for (const SelectedFolderAccess &folder : inspect_files_access(settings).folders) {
			if (folder.is_available == false) continue;
			const std::filesystem::path chosen(folder.path);
			const bool isCovered = std::any_of(roots.begin(), roots.end(), [&](const std::filesystem::path &root) {
				const auto relative = chosen.lexically_relative(root);
				return relative.empty() == false && relative.native().rfind(std::filesystem::path("..").native(), 0) != 0;
			});
			if (isCovered == false) roots.push_back(chosen);
		}
		return ProjectDiscoveryScope{ProjectDiscoveryMode::Broad, std::move(roots)};
	}
	// Authorized but nothing broad is accessible right now (e.g. every
	// authorized broad location is temporarily unreadable) falls through to
	// Limited rather than returning an empty/fabricated Broad scope, so
	// discovery still covers whatever selected folders remain available.

	const FilesAccessState access = inspect_files_access(settings);
	std::vector<std::filesystem::path> limitedRoots;
	for (const SelectedFolderAccess &folder : access.folders) {
		if (folder.is_available == true) limitedRoots.push_back(std::filesystem::path(folder.path));
	}
	return ProjectDiscoveryScope{ProjectDiscoveryMode::Limited, std::move(limitedRoots)};
}

ProjectDiscoveryScope resolve_project_discovery_scope(const AppSettings &settings) {
	return resolve_project_discovery_scope_for_broad_roots(settings, broad_local_access_roots());
}

} // namespace singularity::platform
