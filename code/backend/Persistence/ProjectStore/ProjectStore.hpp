#pragma once

#include "Matter/Types/Matter.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/DiskSpace/DiskSpace.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::persistence {

// A same-object conflict found while replaying a persisted operation whose
// recorded base value no longer matched the state produced by an earlier
// operation. Both competing values are kept, never just a count, so a later
// human merge-review surface can show what each writer intended and let a
// person keep/use/combine - this type is only the durable data that surface
// will read; it implements no review UI itself.
struct ProjectConflict {
	std::string id;
  std::string collection; // empty for a top-level Project field conflict
  std::string object_id;  // empty for a top-level Project field conflict
  std::string field;      // populated only for a top-level Project field conflict
  std::string kind;       // "update" or "delete"
  std::string operation_id;
  std::string actor_id;
  std::string timestamp;
  std::string base_json;
  std::string current_json;
  std::string attempted_json;
	std::string current_operation_id;
	std::string current_actor_id;
	std::string current_timestamp;
};

struct ProjectRecord {
  matters::Matter project;
  std::filesystem::path path;
  long long operation_count = 0;
  long long conflict_count = 0;
  std::vector<ProjectConflict> conflicts;
  // Operation files that could not be read as an operation and so were not
  // replayed: half brought in by a sync client, cut short by a crash, or
  // damaged. Each is left exactly where it is, and read again once it
  // changes. Their file names only.
  std::vector<std::string> unreadable_operations;
};

struct ProjectSummary {
  std::string id;
  std::string location_id;
  std::string name;
  std::string path;
  std::string status;
  long long operation_count = 0;
  long long conflict_count = 0;
  // True when discover_projects() found another location - anywhere among
  // the roots it searched - sharing this same Project id. Two authorised
  // locations sharing an id are a copy/shared-drive duplicate, never
  // silently read as one location having "moved": both are returned, each
  // carrying this flag, so a caller cannot mistake either for the sole
  // canonical location without inspecting the rest of the result.
  bool has_duplicate_location = false;
};

std::filesystem::path project_data_path(const std::filesystem::path &projectPath);
bool has_project_data(const std::filesystem::path &projectPath);

// What this process can actually tell about a Project folder's `.lm` data:
// Present, confirmed Absent, or AccessDenied - the operating system refuses to
// show it (a permission the user revoked, a folder Singularity may no longer read).
// Absent and AccessDenied are never the same answer: only a confirmed absence
// may ever lead to setting Project data up again.
enum class ProjectDataPresence { Present, Absent, AccessDenied };

[[nodiscard]] ProjectDataPresence probe_project_data(const std::filesystem::path &projectPath);

// Thrown when reading Project data fails because this process is not allowed to
// read it, rather than because the data is missing or damaged.
class ProjectDataAccessDenied final : public std::runtime_error {
public:
	explicit ProjectDataAccessDenied(const std::string &message) : std::runtime_error(message) {}
};

// Nothing is wrong with the data, and nothing about it should be repaired,
// started over or removed - the person needs a newer Singularity, and until
// then this build must not touch it. Distinct from damage precisely so the
// product can say that instead of steering someone towards a destructive action
// against perfectly good data.
class ProjectDataRequiresNewerVersion final : public std::runtime_error {
public:
	explicit ProjectDataRequiresNewerVersion(const std::string &message) : std::runtime_error(message) {}
};

// The disk that holds this Project's data has no room for what Singularity has to
// save. Nothing was changed: the save is refused before its file is written,
// or the partial file it left is removed, and the Project stays as it was.
// The same failure as a full disk under a staged Project document write
// (Platform/DiskSpace), so one catch answers both in the same plain words.
using DiskFull = platform::DiskFull;

// Returns the Project as a full replay of the files on disk gives it. The last
// replay of each Project is kept and reused while its files are unchanged, and
// extended when only newer operations were added (ProjectStore.cpp); the
// result never differs from a full replay.
ProjectRecord load_project(const std::filesystem::path &projectPath);

// Lets go of the kept replay of every Project not among `projectPaths` - the
// Projects a workspace currently lists. Keeping every listed one is what lets
// a workspace-wide read reuse them all.
void keep_only_projects(const std::vector<std::filesystem::path> &projectPaths);

// Test-only: forgets every kept replay, so the next load_project() replays
// everything from disk - what a freshly started process does.
void forget_kept_projects_for_testing();

// Test-only: which Projects an action actually touched, by Project folder -
// every load_project(), how many of those replayed the whole log, and every
// save_project() (including one that found nothing to write). Never consulted
// by the app.
struct ProjectAccessCounts {
	std::size_t loads = 0;
	std::size_t full_replays = 0;
	std::size_t saves = 0;
};
std::map<std::string, ProjectAccessCounts> project_access_counts_for_testing();
void reset_project_access_counts_for_testing();
// Test-only: the free space every save of Project data reads instead of the
// disk's own answer, or the disk's again when unset.
void set_available_space_for_testing(std::optional<std::uintmax_t> bytes);
// Test-only: told while a Project is read, once its Base has been checked
// against the manifest's checksum and before it is parsed; unset with {}.
void set_after_base_checked_for_testing(std::function<void()> told);
// Test-only: how long a read is refused when a step this process read before
// cannot be read now (load_project()); returns the window it replaces.
std::chrono::milliseconds set_unreadable_step_window_for_testing(std::chrono::milliseconds window);
ProjectRecord initialize_project(const std::filesystem::path &projectPath, const matters::Matter &project,
    const platform::ActorIdentity &actor,
    const std::optional<std::filesystem::path> &legacySource = std::nullopt);

// Persists project's differences from its loaded merge baseline as one
// attributed operation, then advances project.storage_baseline in place to
// the resulting version. Callers that save repeatedly on the same in-memory
// Matter must reuse the updated object so each save diffs against the
// version it actually produced, not a stale one - otherwise a later revision
// would be replayed against a baseline that never saw the earlier changes.
void save_project(matters::Matter &project, const platform::ActorIdentity &actor);

// Writes an ordinary attributed Project operation plus the durable record
// that it resolves one previously detected conflict. The patch may be empty
// when the person deliberately keeps the current value; the human decision
// is still a real operation and survives reload/recovery.
void save_project_conflict_resolution(
		matters::Matter &project, const platform::ActorIdentity &actor, const std::string &conflictId);

// The folders under `roots` that hold a Project's data, each once. It reads
// only the folders' listings - the slow part over a large folder, a home or a
// Desktop - so it needs no lock of the store.
std::vector<std::filesystem::path> find_project_folders(const std::vector<std::filesystem::path> &roots);
// Each found folder's Project as read from its data, a duplicate location
// marked on every one of its copies, by name. Reads the Projects' data: under
// the store's lock where anything may write them.
std::vector<ProjectSummary> summarize_project_folders(const std::vector<std::filesystem::path> &folders);
// Both, one after the other.
std::vector<ProjectSummary> discover_projects(const std::vector<std::filesystem::path> &roots);
void reveal_project_data(const std::filesystem::path &projectPath);
std::filesystem::path prepare_clean_copy(
    const std::filesystem::path &projectPath, const std::filesystem::path &destinationDirectory);
void remove_project_data(const std::filesystem::path &projectPath);

} // namespace singularity::persistence
