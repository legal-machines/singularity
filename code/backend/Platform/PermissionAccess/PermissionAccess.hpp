#pragma once

#include "Platform/AppSettings/AppSettings.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace singularity::platform {

enum class FilesAccessStatus {
	NotAsked,
	Declined,
	Limited,
	Revoked,
};

struct SelectedFolderAccess {
	std::string id;
	std::string display_name;
	std::string path;
	bool is_available = false;
};

struct FilesAccessState {
	FilesAccessDecision decision = FilesAccessDecision::NotAsked;
	FilesAccessStatus status = FilesAccessStatus::NotAsked;
	std::vector<SelectedFolderAccess> folders;
};

const char *to_string(FilesAccessStatus status);
FilesAccessState inspect_files_access(const AppSettings &settings);

// Broad access is not a single platform permission flag to detect (no
// documented, stable cross-version API for that exists) - it is derived
// from effective filesystem capability instead: candidate broad locations
// (the user's home/user-local space plus currently mounted local/external/
// shared volumes the OS exposes, e.g. macOS entries under /Volumes) are
// each tested with a real filesystem operation - actually attempting to
// open and list that location, the same access discovery itself needs -
// and only genuinely accessible candidates are returned. This deliberately
// never inspects any undocumented OS database (e.g. macOS's TCC.db) and
// never reports a synthetic boolean grant; it is only ever a direct
// measurement of what this process can actually read right now. Does not
// require every candidate to be accessible - an unreadable candidate (a
// protected volume, a permission-denied home subpath) is simply excluded
// from the result, never treated as invalidating the others.
std::vector<std::filesystem::path> broad_local_access_roots();

enum class ProjectDiscoveryMode {
	Broad,
	Limited,
};

// The real, current scope Project discovery should search - the product
// contract's own Broad/Limited distinction, resolved from actual platform
// and Phase 02 state, never fabricated. ProjectStore itself stays
// permission-agnostic (it only recursively scans whatever roots it is
// given for directories named .lm); this is the one place that decides
// which roots those are.
struct ProjectDiscoveryScope {
	ProjectDiscoveryMode mode = ProjectDiscoveryMode::Limited;
	std::vector<std::filesystem::path> roots;
};

// Broad requires BOTH explicit user authorization AND live filesystem
// capability - neither implies the other:
//
//   settings.broad_project_discovery_authorization == Authorized
//   AND
//   broad_local_access_roots() non-empty
//   =>
//   Broad, over exactly those genuinely-accessible authorized roots.
//
// Never Broad merely because broad_local_access_roots() is non-empty (e.g.
// this process' own HOME happens to be readable, which is true on almost
// every real machine regardless of any user decision) - that is capability
// without authorization. Selected-folder access - including the current
// Phase 02 UI's "Choose a folder" via FilesAccessDecision::Granted - is a
// separate, narrower grant and resolves Limited to exactly those authorised,
// currently-available folders (inspect_files_access(settings).folders), even
// though HOME is technically readable: FilesAccessDecision is never read as
// broad authorization. NotAsked, "Not now" and Declined also always resolve
// Limited. Broad authorization without any currently-accessible broad root
// (e.g. HOME temporarily unreadable) is authorization without capability and
// also falls back to Limited, never a fabricated/empty Broad scope. An
// inaccessible authorized root is skipped without invalidating the others,
// and filesystem accessibility only ever answers "can the OS currently read
// this authorized location?", never "did the user authorize broad
// discovery?". See Bridge/PlatformBridge's allow_broader_access() /
// revoke_broader_access() for the only two places that change the
// authorization half; revoking it returns discovery to Limited immediately.
ProjectDiscoveryScope resolve_project_discovery_scope(const AppSettings &settings);

// The same resolution logic, with the accessible-broad-roots half passed in
// explicitly instead of resolved from the real broad_local_access_roots() -
// the seam that lets the authorization x capability boundary (authorized but
// nothing broad currently accessible) be exercised deterministically. Real
// machines almost always have at least one broad root genuinely accessible
// (e.g. macOS's own boot volume under /Volumes), so that boundary cannot be
// reproduced by manipulating this test process' real filesystem state alone.
// Production code must call resolve_project_discovery_scope().
ProjectDiscoveryScope resolve_project_discovery_scope_for_broad_roots(
		const AppSettings &settings, const std::vector<std::filesystem::path> &accessibleBroadRoots);

} // namespace singularity::platform
