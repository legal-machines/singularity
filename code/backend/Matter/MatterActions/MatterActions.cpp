#include "Matter/MatterActions/MatterActions.hpp"

#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"
#include "Platform/PermissionAccess/PermissionAccess.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/RoutingLog/SentPassagesFolder.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace singularity::matter {
namespace {

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// An address of an intake rule as it is compared: on one line, without its
// spaces, in lower case; empty when it is no address.
std::string intake_address(const std::string &text) {
  std::string address;
  for (const char character : text) {
    if (std::isspace(static_cast<unsigned char>(character)) != 0) continue;
    address += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  const auto at = address.find('@');
  if (at == std::string::npos || at == 0 || at + 1 >= address.size() || address.size() > 254) return std::string();
  return address;
}

// The rules of a Matter's intake as kept (D-228): both addresses real, each
// rule once, at most fifty. A rule with an address that is none is refused.
std::vector<correspondence::IntakeRule> normalized_intake(const std::vector<correspondence::IntakeRule> &rules) {
  constexpr std::size_t kMostRules = 50;
  std::vector<correspondence::IntakeRule> kept;
  std::set<std::string> seen;
  for (const correspondence::IntakeRule &rule : rules) {
    correspondence::IntakeRule clean{intake_address(rule.mailbox), intake_address(rule.from)};
    if (clean.mailbox.empty() || clean.from.empty())
      throw std::invalid_argument("A rule of the correspondence intake needs two email addresses: the account it reaches and the mailbox it comes from.");
    if (!seen.insert(clean.mailbox + "\n" + clean.from).second) continue;
    if (kept.size() >= kMostRules) throw std::invalid_argument("A Matter's correspondence intake holds at most fifty rules.");
    kept.push_back(std::move(clean));
  }
  return kept;
}

auto find(std::vector<matters::Matter> &list, const std::string &id) {
  return std::find_if(list.begin(), list.end(),
                      [&](const matters::Matter &entry) { return entry.id == id; });
}

// The Matters removed from this computer's list. Their folders stay where they
// are, so the search for the firm's Matters would offer each again as one that
// arrived (the owner, 2026-11-02: "I deleted this d from the computer myself,
// and here it is - it should not be"); it offers none of these. Opening one's
// folder brings it back.
std::filesystem::path removed_here_path(const std::filesystem::path &workspace_path) {
  return workspace_path / ".singularity-workspace" / "removed-matters.json";
}

std::set<std::string> removed_here(const std::filesystem::path &workspace_path) {
  std::set<std::string> ids;
  std::ifstream input(removed_here_path(workspace_path), std::ios::binary);
  if (!input) return ids;
  try {
    const nlohmann::json kept = nlohmann::json::parse(input);
    const auto listed = kept.find("ids");
    if (listed == kept.end() || listed->is_array() == false) return ids;
    for (const nlohmann::json &id : *listed)
      if (id.is_string() && matters::is_valid_matter_id(id.get<std::string>())) ids.insert(id.get<std::string>());
  } catch (const std::exception &) {
  }
  return ids;
}

// Kept beside the list; a list that cannot be written leaves the Matter to be
// offered again, never anything lost.
void keep_removed_here(const std::filesystem::path &workspace_path, const std::set<std::string> &ids) {
  const std::filesystem::path target = removed_here_path(workspace_path);
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  const std::filesystem::path staged = target.parent_path() / ".removed-matters.json.staged";
  {
    std::ofstream output(staged, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << nlohmann::json{{"formatVersion", 1}, {"ids", ids}}.dump();
    if (!output) return;
  }
  std::filesystem::rename(staged, target, error);
}

matters::Matter require_matter(const std::string &id) {
  auto found = matters::load_matter(node::require_active_workspace_path(), id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  return std::move(*found);
}

std::string require_directory_path(const matters::Matter &entry) {
  if (!entry.directory_path || entry.directory_path->empty()) {
    throw std::runtime_error("Choose this Matter's project folder first.");
  }
  return *entry.directory_path;
}

ProjectDataStatus project_data_status_of(const persistence::ProjectRecord &record) {
  ProjectDataStatus status;
  status.available = true;
  status.path = persistence::project_data_path(record.path).u8string();
  status.operation_count = record.operation_count;
  status.conflict_count = record.conflict_count;
  status.state = "ready";
  return status;
}

ProjectDataStatus unusable_project_data_status(const matters::Matter &entry) {
  ProjectDataStatus status;
  status.state = entry.data_state == matters::ProjectDataState::Damaged ? "damaged" : "unavailable";
  status.state_reason = entry.data_state_reason;
  if (entry.directory_path) status.path = persistence::project_data_path(*entry.directory_path).u8string();
  return status;
}

// One background filesystem watcher per Matter, started lazily by the first
// list_documents() call for a Matter that has a real folder (see that
// function below) and torn down only by stop_watching_documents() or process
// exit. Guarded by its own mutex, entirely separate from
// persistence::persistence_mutex(): a watcher callback fires on its own
// background thread and must never be blocked waiting on - or itself
// contend for - a lock this map's own mutations are made under.
std::mutex watcher_mutex;
std::map<std::string, std::unique_ptr<platform::FileSystemWatcher>> active_watchers;
// Matters whose watch is being begun, outside the lock (watch_project_folder).
std::set<std::string> starting_watchers;
WatchStarter watch_starter_for_testing;

// Never destroyed: a folder watch's callback can still run while the process
// exits, after namespace-scope objects are gone.
struct SharedHooksState {
	std::mutex mutex;
	SharedMatterHooks hooks;
};

SharedHooksState &shared_hooks_state() {
	static SharedHooksState *state = new SharedHooksState();
	return *state;
}

SharedMatterHooks installed_shared_hooks() {
	SharedHooksState &state = shared_hooks_state();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.hooks;
}

struct SealHooksState {
	std::mutex mutex;
	MatterSealHooks hooks;
};

SealHooksState &seal_hooks_state() {
	static SealHooksState *state = new SealHooksState();
	return *state;
}

MatterSealHooks installed_seal_hooks() {
	SealHooksState &state = seal_hooks_state();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.hooks;
}

// A Matter this computer makes carries the firm's seal from its first
// moment. A seal that cannot be written leaves the Matter usable here, and
// unsealed: no other computer takes it as the firm's.
void seal_new_matter(const std::filesystem::path &folder, const std::string &matter_id) {
	const MatterSealHooks hooks = installed_seal_hooks();
	if (!hooks.seal) return;
	try {
		hooks.seal(folder, matter_id);
	} catch (...) {
	}
}

// What the person reads when a Matter is not opened for want of the firm's
// seal: it names their firm.
std::string unsealed_refusal() {
	const platform::AppSettings settings = platform::load_app_settings();
	std::string firm = settings.account ? settings.account->organisation_name.value_or("") : std::string();
	if (firm.empty()) firm = "your firm";
	return "This Matter does not carry the seal of " + firm
		+ ", so it was not opened. A Matter opens here when one of your firm's computers made it.";
}

// Whether the Matter `id` is listed here already: a Matter the person opened
// before the firm's seal existed stays theirs.
bool is_listed_here(const std::string &id) {
	try {
		const auto all = matters::load_matters(node::require_active_workspace_path());
		return std::any_of(all.begin(), all.end(), [&](const matters::Matter &entry) { return entry.id == id; });
	} catch (const std::exception &) {
		return false;
	}
}

// Whether a folder change reached the Matter's own records in its `.lm`: a
// report that lost its paths may have.
bool touches_matter_records(const std::filesystem::path &root, const platform::FileSystemChangeReport &report) {
	if (report.scope == platform::FileSystemChangeScope::kFullReconciliationRequired) return true;
	const std::filesystem::path records = (root / ".lm").lexically_normal();
	for (const std::string &changed : report.affected_paths) {
		const std::filesystem::path relative = std::filesystem::u8path(changed).lexically_normal().lexically_relative(records);
		if (relative.empty() == false && relative.native().rfind(std::filesystem::path("..").native(), 0) != 0) return true;
	}
	return false;
}

// A hook never takes the watch down with it.
void tell_reconciled(const std::string &id) {
	const SharedMatterHooks hooks = installed_shared_hooks();
	if (!hooks.reconciled) return;
	try {
		hooks.reconciled(id);
	} catch (...) {
	}
}

// The watcher callback: runs the same two persisted phases every steady-
// state list_documents() call does - a detect phase then
// process_project_documents(), both declared in MatterActions.hpp - but
// fire-and-forget on a background thread with nothing to return to. A Matter
// that has since been removed, or a transient I/O error, is silently skipped
// rather than crashing the watcher thread. The next external change (or the
// next explicit list_documents() call) recovers from any transient failure
// here; see Sec.7 of phase-08-documents-filesystem-states.md - correctness
// never depends on any one watcher callback succeeding. Running the two
// phases as two separate persisted calls here too (not a single combined
// function) is what makes a real external edit's Changed/Processing window
// genuinely observable in production - see detect_project_documents()'s own
// comment.
//
// Phase 08 correction round 3, item 1: which detect phase runs depends on
// whether the OS-reported event coverage for this notification is
// trustworthy. `report.scope == kAffectedPaths` still uses
// detect_affected_paths() - targeted, not a full-tree rehash (correction
// item 2). `report.scope == kFullReconciliationRequired` (macOS FSEvents
// MustScanSubDirs/dropped-event conditions, a directory-level rename/move
// whose descendants cannot be enumerated from the event itself, or an empty/
// incomplete batch - see Platform/FileSystemWatcher/FileSystemWatcherMacOS.mm)
// falls back to the SAME full scan the startup/restart audit path already
// runs (detect_project_documents()) rather than silently trusting a possibly-
// incomplete affected-paths list. Correctness over avoiding a rare full scan.
void reconcile_and_persist_documents(const std::string id, const platform::FileSystemChangeReport& report) {
	try {
		if (report.scope == platform::FileSystemChangeScope::kFullReconciliationRequired) {
			(void)detect_project_documents(id);
		} else {
			(void)detect_affected_paths(id, report.affected_paths);
		}
		(void)process_project_documents(id);
	} catch (const std::exception &) {
		return;
	}
	tell_reconciled(id);
}

} // namespace

// Test seam (Phase 08 correction round 3, item 1): the exact real dispatch a
// watcher callback performs above, exposed directly so it can be proven with
// a synthetic FileSystemChangeReport - a genuine FSEvents MustScanSubDirs/
// dropped-event condition is an OS-level buffer-overflow signal, not
// something a test can reliably force to occur. Never used by production
// code, which always reaches reconcile_and_persist_documents() through a
// real platform::FileSystemWatcher callback instead.
matters::Matter reconcile_from_watcher_report_for_testing(
		const std::string& id, const platform::FileSystemChangeReport& report) {
	reconcile_and_persist_documents(id, report);
	return require_matter(id);
}

// matters::load_matters() is already the full .lm-aware facade (D-002): for
// a Matter cut over to portable .lm truth it returns .lm's own canonical
// state, not a legacy snapshot - see MatterRepository.hpp.
std::vector<matters::Matter> list() {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  // A Node nobody has set up yet has no Projects - Home then offers the first
  // one - and listing them provisions nothing (New Project does that).
  const auto workspace = node::configured_workspace_path();
  if (!workspace) return {};
  std::vector<matters::Matter> all = matters::load_matters(*workspace);
  // The member's desk is no Matter of the firm (D-157), nor is a personal
  // conversation (D-042).
  all.erase(std::remove_if(all.begin(), all.end(),
      [](const matters::Matter &entry) { return is_desk(entry) || is_personal_project(entry); }), all.end());
  return all;
}

std::filesystem::path desk_folder() {
  return platform::application_data_directory() / "Personal chats";
}

bool is_desk(const matters::Matter &matter) {
  if (!matter.directory_path || matter.directory_path->empty()) return false;
  std::error_code error;
  const std::string desk = std::filesystem::weakly_canonical(desk_folder(), error).u8string();
  if (error) return false;
  const std::string folder = std::filesystem::weakly_canonical(std::filesystem::u8path(*matter.directory_path), error).u8string();
  return !error && folder == desk;
}

std::string desk_project_id() {
  static std::mutex making;
  static std::string *kept = new std::string();
  static std::string *keptFor = new std::string();
  const std::lock_guard<std::mutex> lock(making);
  try {
    const std::filesystem::path workspace = node::require_active_workspace_path();
    // The one opened before, while it is still the desk of this workspace.
    if (!kept->empty() && *keptFor == workspace.u8string()) {
      std::optional<matters::Matter> found;
      {
        const persistence::PersistenceLock persistence(persistence::persistence_mutex());
        found = matters::load_matter(workspace, *kept);
      }
      if (found && is_desk(*found)) return *kept;
    }
    std::error_code error;
    std::filesystem::create_directories(desk_folder(), error);
    if (error) return "";
    const matters::Matter desk = create_or_open_from_folder(desk_folder().u8string());
    *kept = desk.id;
    *keptFor = workspace.u8string();
    return desk.id;
  } catch (const std::exception &) {
    return "";
  }
}

matters::Matter create(std::string name, std::optional<std::string> description,
                       std::optional<std::string> instructions) {
  name = trim(name);
  if (name.empty()) throw std::runtime_error("Enter a matter name.");
  if (description) { *description = trim(*description); if (description->empty()) description.reset(); }
  if (instructions) { *instructions = trim(*instructions); if (instructions->empty()) instructions.reset(); }
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto all = matters::load_matters(workspace_path);
  matters::Matter result;
  result.id = matters::generate_matter_id();
  result.name = std::move(name);
  result.description = std::move(description);
  result.instructions = std::move(instructions);
  result.created_at = workspaces::current_timestamp();
  result.updated_at = result.created_at;
  all.push_back(result);
  matters::save_matters(workspace_path, all);
  return result;
}

namespace {

// The interface chooses a glyph, a ground and a shape by name; only short
// lower-case names are kept, so a stored appearance can never carry markup or
// an unbounded value. What a name means is the interface's to decide.
std::string appearance_token(std::string value) {
  value = trim(std::move(value));
  if (value.size() > 32) return {};
  for (const char character : value) {
    const bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                         character == '_' || character == '-';
    if (!allowed) return {};
  }
  return value;
}

// At most `maximum` characters of UTF-8 text, never cut inside a character.
std::string first_characters(const std::string &value, std::size_t maximum) {
  std::size_t index = 0;
  std::size_t characters = 0;
  while (index < value.size() && characters < maximum) {
    const auto lead = static_cast<unsigned char>(value[index]);
    const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    if (index + length > value.size()) break;
    index += length;
    ++characters;
  }
  return value.substr(0, index);
}

matters::ProjectAppearance normalized_appearance(matters::ProjectAppearance appearance) {
  appearance.short_name = first_characters(trim(std::move(appearance.short_name)), 40);
  appearance.mark_text = first_characters(trim(std::move(appearance.mark_text)), 3);
  appearance.mark_glyph = appearance_token(std::move(appearance.mark_glyph));
  appearance.mark_ground = appearance_token(std::move(appearance.mark_ground));
  appearance.mark_shape = appearance_token(std::move(appearance.mark_shape));
  return appearance;
}

} // namespace

matters::Matter update(UpdateMatter command) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto all = matters::load_matters(workspace_path);
  const auto found = find(all, command.id);
  if (found == all.end()) throw std::runtime_error("This matter no longer exists.");
  // STAT-3: a Project's name, description and instructions live in its own
  // `.lm` data, which is canonical. While that data cannot be read, such a
  // change has nowhere to go: accepting it into matters.json would show the
  // person their edit and then lose it the moment the folder came back. It is
  // refused out loud instead. Relinking a moved folder stays available, because
  // the locator is all the workspace itself keeps.
  const bool changesProjectContent = (command.name.supplied && command.name.value)
      || command.description.supplied || command.instructions.supplied || command.appearance.has_value()
      || command.archived.has_value() || command.correspondence_intake.has_value();
  if (changesProjectContent && found->data_state == matters::ProjectDataState::Damaged)
    throw matters::ProjectDataError(found->data_state,
        "This Project's Singularity data can't be read, so this change was not saved. Repair or restore its .lm "
        "folder, then try again.");
  if (changesProjectContent && found->data_state == matters::ProjectDataState::Unavailable)
    throw matters::ProjectDataError(found->data_state,
        found->data_state_reason == "access_denied"
            ? "Singularity can't access this Project's folder, so this change was not saved. Restore access to the "
              "folder, then try again."
            : found->data_state_reason == "requires_newer_version"
            ? "This Project was saved by a newer version of Singularity, so this change was not saved. Update Singularity"
              " to work in it - its data is fine."
            : "This Project's folder or Singularity data isn't available, so this change was not saved. Reconnect the "
              "folder, then try again.");
  if (command.name.supplied && command.name.value) {
    const auto name = trim(*command.name.value);
    if (name.empty()) throw std::runtime_error("Enter a matter name.");
    found->name = name;
  }
  const auto apply_optional = [](TextPatch patch, std::optional<std::string> &target) {
    if (!patch.supplied) return;
    if (!patch.value) { target.reset(); return; }
    const auto value = trim(*patch.value);
    target = value.empty() ? std::nullopt : std::optional<std::string>(value);
  };
  apply_optional(std::move(command.description), found->description);
  apply_optional(std::move(command.instructions), found->instructions);
  if (command.appearance) found->appearance = normalized_appearance(std::move(*command.appearance));
  if (command.correspondence_intake) found->correspondence_intake = normalized_intake(*command.correspondence_intake);
  found->updated_at = workspaces::current_timestamp();
  // Archived by the member here, or restored: for every computer of it.
  if (command.archived.has_value() && *command.archived != (found->archived_at.empty() == false)) {
    found->archived_at = *command.archived ? found->updated_at : std::string();
    found->archived_by_actor_id = *command.archived ? platform::resolve_actor_identity().actor_id : std::string();
  }
  matters::save_matters(workspace_path, all);
  return *found;
}

namespace {

// Whether `candidate`, symlinks resolved, stands strictly inside `root`.
bool stands_inside(const std::filesystem::path &root, const std::filesystem::path &candidate) {
  std::error_code rootError;
  std::error_code candidateError;
  const std::filesystem::path base = std::filesystem::weakly_canonical(root, rootError);
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, candidateError);
  if (rootError || candidateError) return false;
  const auto relative = resolved.lexically_relative(base);
  return !relative.empty() && relative != "." && *relative.begin() != "..";
}

} // namespace

std::vector<matters::Matter> remove(const std::string &id) {
  // Removing a Project from the list deletes Singularity's own copies of its
  // imported files, under the workspace, and nothing else. Only a Project this
  // workspace lists is removed; an id that is not a plain name - read from a
  // shared folder's .lm as "/Users/someone" before ids were checked - leaves
  // the list and touches no file; and the copies' folder is deleted only when
  // it resolves inside the store, whatever its id says. Removing such a Project
  // once took remove_all() of the folder its id named.
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto all = matters::load_matters(workspace_path);
  const auto listedBefore = all.size();
  all.erase(std::remove_if(all.begin(), all.end(),
      [&](const matters::Matter &entry) { return entry.id == id; }), all.end());
  if (all.size() == listedBefore) return all;
  matters::save_matters(workspace_path, all);
  if (!matters::is_valid_matter_id(id)) return all;
  // Never offered here again as one that arrived, until its folder is opened.
  std::set<std::string> removed = removed_here(workspace_path);
  if (removed.insert(id).second) keep_removed_here(workspace_path, removed);
  const std::filesystem::path store = workspace_path / ".singularity-workspace" / "documents";
  const std::filesystem::path copies = matters::documents_directory(workspace_path, id);
  if (stands_inside(store, copies)) {
    std::error_code remove_error;
    std::filesystem::remove_all(copies, remove_error);
  }
  // What the Project's requests sent, kept for its "What left this device".
  routing::sent_passages_folder::forget_project(workspace_path, id);
  return all;
}

matters::Matter create_or_open_from_folder(const std::string &selected_path) {
  const std::filesystem::path path(selected_path);
  workspaces::validate_directory(path);
  const std::string canonical = std::filesystem::weakly_canonical(path).u8string();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto all = matters::load_matters(workspace_path);
  const auto listed = [&](const std::string &id) {
    return std::find_if(all.begin(), all.end(), [&](const matters::Matter &entry) { return entry.id == id; });
  };
  const auto reload = [&](const std::string &id) {
    auto found = matters::load_matter(workspace_path, id);
    if (!found) throw std::runtime_error("This Project could not be opened.");
    return *found;
  };
  if (persistence::has_project_data(canonical)) {
    persistence::ProjectRecord record;
    try {
      record = persistence::load_project(canonical);
    } catch (const std::exception &) {
      throw matters::ProjectDataError(matters::ProjectDataState::Damaged,
          "This folder's Singularity data is damaged, so the Project could not be opened. Restore its .lm data, then try "
          "again.");
    }
    const std::string id = record.project.id;
    const auto known = listed(id);
    if (known != all.end()) {
      std::error_code canonical_error;
      const bool samePlace = known->directory_path
          && std::filesystem::weakly_canonical(std::filesystem::path(*known->directory_path), canonical_error).u8string()
              == canonical;
      if (samePlace) return *known;
      if (known->data_state == matters::ProjectDataState::Ready)
        throw std::runtime_error("This folder holds a Project that is already open from another folder. Open that "
            "Project instead, or remove it from the list before opening this copy.");
      // The listed location is gone: this folder is where the Project now lives.
      known->directory_path = canonical;
      known->updated_at = workspaces::current_timestamp();
      matters::save_matters(workspace_path, all);
      return reload(id);
    }
    // A Matter not listed here enters only with the firm's seal (D-167).
    const MatterSealHooks sealHooks = installed_seal_hooks();
    if (sealHooks.may_open && sealHooks.may_open(canonical, id) == false) throw std::runtime_error(unsealed_refusal());
    matters::Matter locator;
    locator.id = id;
    locator.directory_path = canonical;
    locator.storage_cutover = true;
    all.push_back(locator);
    matters::save_matters(workspace_path, all);
    // Opened by the member: one removed from this computer before is back.
    std::set<std::string> removed = removed_here(workspace_path);
    if (removed.erase(id) > 0) keep_removed_here(workspace_path, removed);
    return reload(id);
  }
  matters::Matter project;
  project.id = matters::generate_matter_id();
  project.name = std::filesystem::path(canonical).filename().u8string();
  if (project.name.empty()) project.name = "Project";
  project.created_at = workspaces::current_timestamp();
  project.updated_at = project.created_at;
  project.directory_path = canonical;
  // The Project's data exists before the Project is listed: a crash in
  // between leaves a folder that opens as this Project, never a listed
  // Project without data.
  (void)persistence::initialize_project(canonical, project, platform::resolve_actor_identity());
  seal_new_matter(canonical, project.id);
  matters::Matter locator;
  locator.id = project.id;
  locator.directory_path = canonical;
  locator.storage_cutover = true;
  all.push_back(locator);
  matters::save_matters(workspace_path, all);
  return reload(project.id);
}

std::string personal_project_id(const std::string &channel_id) {
  static const std::string kChannel = "pc_";
  const std::string hex = channel_id.rfind(kChannel, 0) == 0 ? channel_id.substr(kChannel.size()) : channel_id;
  return "mt_" + hex;
}

bool is_personal_project(const matters::Matter &matter) {
  if (!matter.directory_path || matter.directory_path->empty()) return false;
  const std::filesystem::path folder = std::filesystem::u8path(*matter.directory_path);
  return folder.filename().u8string().rfind("pc_", 0) == 0 && folder.parent_path().filename().u8string() == "Personal";
}

matters::Matter open_personal_project(const std::filesystem::path &folder, const std::string &id) {
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) throw std::runtime_error("The conversation could not be kept on this computer: its folder could not be made.");
  const std::string canonical = std::filesystem::weakly_canonical(folder).u8string();
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    // Made here when neither computer of the two has written it yet; the other
    // computer's arrives under the same id, so the two are one project.
    if (!persistence::has_project_data(canonical)) {
      matters::Matter project;
      project.id = id;
      project.name = "Personal conversation";
      project.created_at = workspaces::current_timestamp();
      project.updated_at = project.created_at;
      project.directory_path = canonical;
      (void)persistence::initialize_project(canonical, project, platform::resolve_actor_identity());
      seal_new_matter(canonical, project.id);
    }
  }
  return create_or_open_from_folder(canonical);
}

matters::Matter create_sample(const std::filesystem::path &bundled_matter,
                              const std::filesystem::path &destination_root) {
  namespace fs = std::filesystem;
  if (!fs::is_directory(bundled_matter)) {
    throw std::runtime_error("This installation of Singularity carries no sample matter.");
  }
  const fs::path destination = destination_root / bundled_matter.filename();
  if (!fs::exists(destination)) {
    fs::create_directories(destination_root);
    // Copied beside its final place and moved in whole: a copy cut short is
    // never opened as the sample.
    const fs::path staged = destination_root / (bundled_matter.filename().u8string() + ".copying");
    fs::remove_all(staged);
    fs::copy(bundled_matter, staged, fs::copy_options::recursive);
    fs::rename(staged, destination);
  }
  // The sample comes with Singularity itself: this computer seals its copy,
  // once, and a copy made before the seal existed when it is opened again.
  const MatterSealHooks sealHooks = installed_seal_hooks();
  if (sealHooks.is_firms && persistence::has_project_data(destination.u8string())) {
    try {
      const std::string id = persistence::load_project(destination.u8string()).project.id;
      if (sealHooks.is_firms(destination, id) == false) seal_new_matter(destination, id);
    } catch (const std::exception &) {
    }
  }
  return create_or_open_from_folder(destination.u8string());
}

matters::Matter create_named(const std::string &requested_name,
                             const std::filesystem::path &destination_root) {
  namespace fs = std::filesystem;
  const std::string name = trim(requested_name);
  if (name.empty()) throw std::runtime_error("Name the new Project.");
  // The name becomes a folder's on every system Singularity runs on.
  const bool unfit = name.front() == '.' || name.back() == '.'
      || name.size() > 200 || name.find_first_of("/\\:*?\"<>|") != std::string::npos
      || std::any_of(name.begin(), name.end(), [](char character) {
           return static_cast<unsigned char>(character) < 0x20; });
  if (unfit) {
    throw std::runtime_error("A Project's name becomes its folder's name: leave out / \\ : * ? \" < > |, "
                             "and do not begin or end it with a dot.");
  }
  fs::create_directories(destination_root);
  fs::path folder = destination_root / fs::u8path(name);
  for (int next = 2; fs::exists(folder); ++next) {
    folder = destination_root / fs::u8path(name + " " + std::to_string(next));
  }
  fs::create_directory(folder);
  return create_or_open_from_folder(folder.u8string());
}

matters::Matter assign_folder(const std::string &id,
                              const std::optional<std::string> &selected_path) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace_path, id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  if (!selected_path) return *found;
  const std::filesystem::path path(*selected_path);
  workspaces::validate_directory(path);
  const auto canonical_wide = std::filesystem::weakly_canonical(path).u8string();
  const std::filesystem::path canonical(canonical_wide);
  found->directory_path = std::string(canonical_wide.begin(), canonical_wide.end());
  found->updated_at = workspaces::current_timestamp();
  // A Project keeps itself in the folder it was given. A folder with no `.lm`
  // of its own gets this Project's, written before the folder is recorded, so
  // there is never a Project whose folder holds nothing of it: that is what
  // create_or_open_from_folder() does when a person chooses a folder, and it
  // must not depend on which way the Project reached its folder. A folder that
  // already holds Project data is left exactly as it is - it may be this
  // Project's own data, or another Project's, and neither is written over here.
  if (persistence::has_project_data(canonical) == false) {
    (void)persistence::initialize_project(canonical, *found, platform::resolve_actor_identity());
    seal_new_matter(canonical, found->id);
  }
  matters::save_matter(workspace_path, *found);
  return *found;
}

// Phase A, persisted on its own: detects external filesystem changes
// (Document/DocumentRegistry.hpp::detect_filesystem_changes()) and, only if
// anything actually changed, commits that as its own save_matters() call -
// under its own PersistenceLock acquisition, released before returning. A
// document this finds Changed, or a brand-new document left Processing, is
// therefore genuinely ON DISK in `.lm` at the moment this function returns:
// any other caller (a concurrent matter.listDocuments, or the next
// process_project_documents() call below, which reloads fresh from storage
// rather than reusing this in-memory result) sees that real state, not an
// internal transient that only ever existed inside one combined call.
// folder_paths (Matter::folder_paths - transient, never persisted; see that
// field's own comment) is set on the returned Matter but, being transient,
// is naturally absent from the next fresh load - list_documents() below
// carries it forward explicitly for its own final result.
matters::Matter detect_project_documents(const std::string& id) {
	// The folder is read - every file stat'd and hashed - before the persistence
	// lock is taken: a large data room, a network share or a cloud drive still
	// downloading holds up only this Matter, never every other Matter, chat and
	// agent while it is read (SCAN-1). What changed in the record meanwhile is
	// read again under the lock (reconcile_folder_observation).
	std::optional<matters::Matter> readAgainst;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		readAgainst = matters::load_matter(node::require_active_workspace_path(), id);
	}
	if (!readAgainst) throw std::runtime_error("This matter no longer exists.");
	documents::registry::FolderObservation observation = documents::registry::observe_folder(*readAgainst);
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This matter no longer exists.");
	// Moved to another folder meanwhile: what was read is not this folder.
	if (found->directory_path != readAgainst->directory_path) {
		observation = documents::registry::observe_folder(*found);
		readAgainst = *found;
	}
	const auto outcome = documents::registry::reconcile_folder_observation(*found, std::move(observation), *readAgainst);
	found->folder_paths = outcome.folder_paths;
	// GAP2 closure (DocumentRegistry.hpp's own comment on
	// verify_cached_derivatives_exist()): this full-scan audit path is
	// exactly where a Ready document whose derivative cache has actually
	// gone missing - deleted, corrupted, a fresh machine restored from a
	// backup that skipped .singularity-workspace/derivatives - gets caught and demoted
	// back to Changed, so it self-heals through the ordinary
	// process_pending_documents() pipeline instead of silently staying
	// "Ready" while genuinely unreadable.
	const bool derivativesChanged = documents::registry::verify_cached_derivatives_exist(workspace_path, *found);
	if (outcome.changed == true || derivativesChanged == true) {
		found->updated_at = workspaces::current_timestamp();
		matters::save_matter(workspace_path, *found);
	}
	return *found;
}

// The targeted, steady-state counterpart of detect_project_documents() above
// (correction item 2): re-derives folder_paths cheaply and hashes only the
// files `affected_absolute_paths` actually names - see Document/
// DocumentRegistry/DocumentRegistry.hpp::reconcile_affected_paths() for what
// "targeted" means and why it never rehashes the rest of the Project.
// Persisted the same way: its own PersistenceLock/save_matters() call, only
// if anything actually changed.
matters::Matter detect_affected_paths(const std::string& id, const std::vector<std::string>& affected_absolute_paths) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This matter no longer exists.");
	const auto outcome = documents::registry::reconcile_affected_paths(*found, affected_absolute_paths);
	found->folder_paths = outcome.folder_paths;
	if (outcome.changed == true) {
		found->updated_at = workspaces::current_timestamp();
		matters::save_matter(workspace_path, *found);
	}
	return *found;
}

// A pure read for the steady-state case: no file is stat()'d or hashed at
// all beyond collect_real_folder_paths()'s cheap directory-only walk (so
// folder_paths - transient, never persisted - still reflects real external
// folder create/rename/move/delete on every call). Nothing is persisted;
// this never mutates matter.documents. The watcher's own targeted
// reconciliation (reconcile_and_persist_documents() above) is what keeps the
// persisted document state current between calls to this function.
matters::Matter read_project_documents(const std::string& id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This matter no longer exists.");
	if (found->directory_path && found->directory_path->empty() == false) {
		std::vector<std::string> folder_paths;
		if (documents::registry::collect_real_folder_paths(std::filesystem::path(*found->directory_path), folder_paths) ==
				true) {
			found->folder_paths = std::move(folder_paths);
		}
	}
	return *found;
}

// Phase B, persisted on its own, in three steps so the Project's lock is
// never held while a document is read: reading a large PDF or a data room
// takes seconds to minutes, and every save, status poll, approval and Stop of
// the whole application waits on that one lock.
//
//   snapshot  under the lock: the Project as it is saved now (with the one
//             document a "Try again" re-opens);
//   extract   without it: the real extraction pipeline over everything left
//             Processing/Changed (Document/DocumentRegistry.hpp::
//             process_pending_documents()); the derivative cache writes each
//             result atomically on its own;
//   merge     under the lock again: each document's reading is recorded on the
//             Project as it is saved THEN, and only when that document is
//             still exactly what was read - same file fingerprint, place,
//             version and state. One that changed meanwhile keeps its newer
//             state and is read again on the next pass; nothing another
//             writer saved in between is overwritten.
//
// One pass per Project at a time. A second caller does not wait for the first
// (it could hold the Project's lock itself): it leaves its request with the
// running pass and returns. The running pass reads again, once more, before it
// ends - a document saved while it was reading, a "Try again" the lawyer chose
// meanwhile - so nothing asked for during a pass is left pending until some
// unrelated event. A request that needs the documents read waits for that
// through wait_for_documents_read() instead (MatterActions.hpp), which counts
// the pass's rounds below.
namespace {

struct ExtractionPass {
	bool isRunning = false;
	bool isRerunRequested = false;
	std::set<std::string> retriedDocumentIds;
	// Snapshot - extract - merge rounds this Project's passes have begun and
	// ended (a failed round ends too). Rounds run one at a time, so round N has
	// completed exactly when roundsCompleted >= N.
	std::uint64_t roundsStarted = 0;
	std::uint64_t roundsCompleted = 0;
};

// Never destroyed: a folder watcher's callback (active_watchers, declared
// earlier and so destroyed later) can still be reading a Project while the
// process exits, and it locks this registry - a destroyed mutex there aborts
// the exit ("mutex lock failed").
std::mutex &extraction_registry_mutex = *new std::mutex;
std::map<std::string, ExtractionPass> &extraction_passes = *new std::map<std::string, ExtractionPass>;
// Signalled, under extraction_registry_mutex, whenever a round ends or a pass
// stops.
std::condition_variable &extraction_rounds_changed = *new std::condition_variable;

std::function<void(const std::string &)> &after_extraction_observer() {
	static std::function<void(const std::string &)> observer;
	return observer;
}

bool is_pending_reading(const matters::MatterDocument &document) {
	return document.source_mode == matters::DocumentSourceMode::Referenced && document.available == true
		&& (document.lifecycle_state == matters::DocumentLifecycleState::Processing
			|| document.lifecycle_state == matters::DocumentLifecycleState::Changed);
}

bool is_same_reading_input(const matters::MatterDocument &saved, const matters::MatterDocument &read) {
	return saved.fingerprint == read.fingerprint && saved.relative_path == read.relative_path
		&& saved.version == read.version && saved.lifecycle_state == read.lifecycle_state
		&& saved.available == read.available && saved.source_mode == read.source_mode
		&& saved.file_type == read.file_type;
}

// Ends the Project's pass when it fails. What was asked of it and not yet read
// stays on record for the next pass to take. A pass that finishes ends itself,
// under the lock it checked for new requests with.
struct ExtractionPassGuard {
	std::string id;
	bool isEnded = false;
	~ExtractionPassGuard() {
		if (isEnded == true) return;
		const std::lock_guard<std::mutex> lock(extraction_registry_mutex);
		ExtractionPass &pass = extraction_passes[id];
		pass.isRunning = false;
		pass.roundsCompleted = pass.roundsStarted;
		extraction_rounds_changed.notify_all();
	}
};

// One snapshot - extract - merge round over what is pending now, plus the
// documents a "Try again" re-opens. Called with the Project's pass claimed -
// or, with `only`, by a request reading just the documents it needs
// (wait_for_needed_documents_read()) outside any pass: then nothing else is
// read, and the merge below records those readings exactly as a pass's.
matters::Matter read_pending_documents_once(const std::string &id, const std::set<std::string> &retried_document_ids,
		const std::set<std::string> *only = nullptr) {
	std::filesystem::path workspace_path;
	matters::Matter loaded;
	matters::Matter reading;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		workspace_path = node::require_active_workspace_path();
		auto found = matters::load_matter(workspace_path, id);
		if (!found) throw std::runtime_error("This matter no longer exists.");
		loaded = *found;
		reading = *found;
		if (only != nullptr)
			reading.documents.erase(std::remove_if(reading.documents.begin(), reading.documents.end(),
				[&](const matters::MatterDocument &document) { return only->count(document.id) == 0; }),
				reading.documents.end());
		// A retry re-opens its document only in the copy being read: the saved
		// Project keeps "Needs attention" until the new reading is recorded.
		bool isRetried = false;
		for (const std::string &documentId : retried_document_ids)
			if (documents::registry::request_reprocessing(reading, documentId) == true) isRetried = true;
		const bool isPending = std::any_of(reading.documents.begin(), reading.documents.end(), is_pending_reading);
		if (isRetried == false && isPending == false) return *found;
	}

	/* - - - - - - */

	const matters::Matter before = reading;
	const bool changed = documents::registry::process_pending_documents(workspace_path, reading);
	if (only == nullptr && after_extraction_observer()) after_extraction_observer()(id);

	/* - - - - - - */

	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	auto fresh = matters::load_matter(workspace_path, id);
	if (!fresh) throw std::runtime_error("This matter no longer exists.");
	if (changed == false || fresh->directory_path != loaded.directory_path) return *fresh;
	bool merged = false;
	for (std::size_t index = 0; index < before.documents.size() && index < reading.documents.size(); ++index) {
		const matters::MatterDocument &input = before.documents[index];
		const matters::MatterDocument &output = reading.documents[index];
		if (is_pending_reading(input) == false || input.id != output.id) continue;
		const auto saved = std::find_if(loaded.documents.begin(), loaded.documents.end(),
			[&](const matters::MatterDocument &entry) { return entry.id == input.id; });
		const auto current = std::find_if(fresh->documents.begin(), fresh->documents.end(),
			[&](const matters::MatterDocument &entry) { return entry.id == input.id; });
		if (saved == loaded.documents.end() || current == fresh->documents.end()) continue;
		// Read from the document as it was saved at the snapshot: recorded only
		// while the saved document is still that one.
		if (is_same_reading_input(*current, *saved) == false) continue;
		current->lifecycle_state = output.lifecycle_state;
		current->attention_detail = output.attention_detail;
		current->extraction_status = output.extraction_status;
		current->extracted_character_count = output.extracted_character_count;
		current->page_count = output.page_count;
		current->extractor_version = output.extractor_version;
		merged = true;
	}
	if (merged == true) {
		fresh->updated_at = workspaces::current_timestamp();
		matters::save_matter(workspace_path, *fresh);
	}
	return *fresh;
}

// Takes the Project id by value: a pass outlives many of its callers' strings
// (a folder watcher's callback can be torn down while its pass still runs), and
// a pass that lost its id would end on the wrong Project and leave this one
// marked as being read forever.
matters::Matter process_documents_outside_lock(const std::string id, const std::optional<std::string> retried_document_id) {
	std::set<std::string> retried;
	if (retried_document_id) retried.insert(*retried_document_id);
	bool isQueued = false;
	{
		const std::lock_guard<std::mutex> lock(extraction_registry_mutex);
		ExtractionPass &pass = extraction_passes[id];
		if (pass.isRunning == true) {
			pass.isRerunRequested = true;
			pass.retriedDocumentIds.insert(retried.begin(), retried.end());
			isQueued = true;
		} else {
			pass.isRunning = true;
			++pass.roundsStarted;
			// Retries left behind by a pass that failed are carried out now.
			retried.insert(pass.retriedDocumentIds.begin(), pass.retriedDocumentIds.end());
			pass.retriedDocumentIds.clear();
			pass.isRerunRequested = false;
		}
	}
	if (isQueued == true) {
		// The running pass reads it before it ends; until then the lawyer sees
		// the document they retried as being read again, not still failed.
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto found = matters::load_matter(node::require_active_workspace_path(), id);
		if (!found) throw std::runtime_error("This matter no longer exists.");
		if (retried_document_id) (void)documents::registry::request_reprocessing(*found, *retried_document_id);
		return *found;
	}

	ExtractionPassGuard passGuard{id};
	for (;;) {
		matters::Matter result = read_pending_documents_once(id, retried);
		// Checked and ended under the same lock a second caller leaves its
		// request under, so no request can arrive between the check and the end.
		const std::lock_guard<std::mutex> lock(extraction_registry_mutex);
		ExtractionPass &pass = extraction_passes[id];
		++pass.roundsCompleted;
		extraction_rounds_changed.notify_all();
		if (pass.isRerunRequested == false && pass.retriedDocumentIds.empty()) {
			pass.isRunning = false;
			passGuard.isEnded = true;
			return result;
		}
		retried = std::move(pass.retriedDocumentIds);
		pass.retriedDocumentIds.clear();
		pass.isRerunRequested = false;
		++pass.roundsStarted;
	}
}

// The documents of `id` a pass has yet to read, as saved now.
std::vector<std::string> documents_being_read(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto found = matters::load_matter(node::require_active_workspace_path(), id);
	if (!found) throw std::runtime_error("This matter no longer exists.");
	std::vector<std::string> pending;
	for (const matters::MatterDocument &document : found->documents)
		if (is_pending_reading(document)) pending.push_back(document.id);
	return pending;
}

// A request goes on without documents still being read only while a pass is
// reading them: one is started, on a thread of its own, when none runs. Its
// failure is its own - what it could not read stays pending for the next pass.
void read_pending_in_background(const std::string &id) {
	{
		const std::lock_guard<std::mutex> lock(extraction_registry_mutex);
		const auto running = extraction_passes.find(id);
		if (running != extraction_passes.end() && running->second.isRunning == true) return;
	}
	std::thread([id] {
		try {
			(void)process_documents_outside_lock(id, std::nullopt);
		} catch (...) {
		}
	}).detach();
}

} // namespace

matters::Matter process_project_documents(const std::string& id) {
	return process_documents_outside_lock(id, std::nullopt);
}

matters::Matter reprocess_document(const std::string& id, const std::string& document_id) {
	return process_documents_outside_lock(id, document_id);
}

DocumentReadingWait wait_for_documents_read(const std::string &id, std::chrono::milliseconds timeout,
		const std::function<void(std::size_t awaited_count)> &on_waiting) {
	const auto started = std::chrono::steady_clock::now();
	const auto deadline = started + timeout;
	DocumentReadingWait result;
	// A document that stays pending however often it is read (its reading is
	// never recorded because the Project moved meanwhile, say) must not hold a
	// request for the whole timeout: a few rounds are enough for anything a
	// pass can actually read.
	constexpr int kMaximumRounds = 4;
	for (int round = 0;; ++round) {
		const std::vector<std::string> pending = documents_being_read(id);
		if (pending.empty()) break;
		if (result.awaited_count == 0) {
			result.awaited_count = pending.size();
			if (on_waiting) on_waiting(pending.size());
		}
		const bool isOutOfTime = std::chrono::steady_clock::now() >= deadline;
		if (isOutOfTime || round >= kMaximumRounds) {
			result.unread_document_ids = pending;
			result.is_timed_out = isOutOfTime;
			break;
		}
		std::unique_lock<std::mutex> lock(extraction_registry_mutex);
		ExtractionPass &pass = extraction_passes[id];
		if (pass.isRunning == false) {
			// Nothing reads them: they are read here. Should another caller claim
			// the pass first, this leaves its request with it and the next round
			// of this loop joins it.
			lock.unlock();
			(void)process_project_documents(id);
			continue;
		}
		// The running round may have taken its snapshot before these documents
		// were saved: one more round is asked for, and the wait ends once a round
		// that began after this request has ended (or the pass stopped).
		pass.isRerunRequested = true;
		const std::uint64_t awaitedRound = pass.roundsStarted + 1;
		(void)extraction_rounds_changed.wait_until(lock, deadline, [&] {
			return pass.roundsCompleted >= awaitedRound || pass.isRunning == false;
		});
	}
	result.waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
	return result;
}

DocumentReadingWait wait_for_needed_documents_read(const std::string &id,
		const std::vector<std::string> &needed_document_ids, std::chrono::milliseconds timeout,
		const std::function<void(std::size_t awaited_count)> &on_waiting) {
	const auto started = std::chrono::steady_clock::now();
	const auto deadline = started + timeout;
	DocumentReadingWait result;
	const std::vector<std::string> pending = documents_being_read(id);
	if (pending.empty()) return result;

	/* - - - - - - */

	// What this request reads before it goes on: the documents it names that are
	// still pending - or, when it names none, a few just added or saved, never a
	// data room's worth.
	std::set<std::string> awaited;
	if (!needed_document_ids.empty()) {
		for (const std::string &documentId : needed_document_ids)
			if (std::find(pending.begin(), pending.end(), documentId) != pending.end()) awaited.insert(documentId);
	} else if (pending.size() <= kFewDocumentsForARequest) {
		awaited.insert(pending.begin(), pending.end());
	}
	if (awaited.size() < pending.size()) read_pending_in_background(id);
	if (!awaited.empty()) {
		result.awaited_count = awaited.size();
		if (on_waiting) on_waiting(awaited.size());
		// Read here, on the caller's thread, one by one, whatever pass is running:
		// its round may be many minutes from reaching them. A document saved again
		// while it was read is pending again and read once more, a few times at
		// most; the time runs out between two documents, never inside one.
		constexpr int kMaximumRounds = 4;
		bool isFirstRead = true;
		for (int round = 0; round < kMaximumRounds && !awaited.empty() && !result.is_timed_out; ++round) {
			for (const std::string &documentId : awaited) {
				if (isFirstRead == false && std::chrono::steady_clock::now() >= deadline) {
					result.is_timed_out = true;
					break;
				}
				isFirstRead = false;
				const std::set<std::string> one{documentId};
				(void)read_pending_documents_once(id, {}, &one);
			}
			std::set<std::string> left;
			for (const std::string &documentId : documents_being_read(id))
				if (awaited.count(documentId) != 0) left.insert(documentId);
			awaited = std::move(left);
		}
	}

	/* - - - - - - */

	// Still being read when the request goes on: of the documents it names, only
	// those; with none named, every document the answer could have drawn from.
	if (!needed_document_ids.empty()) {
		result.unread_document_ids.assign(awaited.begin(), awaited.end());
	} else {
		result.unread_document_ids = documents_being_read(id);
	}
	result.waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
	return result;
}

DocumentReadingWait wait_for_request_documents_read(const std::string &id,
		const std::vector<std::string> &selected_document_ids, const std::optional<std::string> &target_document_id,
		std::chrono::milliseconds named_timeout, std::chrono::milliseconds relevant_timeout,
		const std::function<void(std::size_t awaited_count)> &on_waiting) {
	std::vector<std::string> named = selected_document_ids;
	if (target_document_id && !target_document_id->empty()
			&& std::find(named.begin(), named.end(), *target_document_id) == named.end())
		named.push_back(*target_document_id);
	if (!selected_document_ids.empty() || named.empty())
		return wait_for_needed_documents_read(id, named, named.empty() ? relevant_timeout : named_timeout, on_waiting);
	// A Relevant request with a target: the target first, then what was just added.
	DocumentReadingWait result = wait_for_needed_documents_read(id, named, named_timeout, on_waiting);
	const DocumentReadingWait relevant = wait_for_needed_documents_read(id, {}, relevant_timeout, on_waiting);
	result.awaited_count += relevant.awaited_count;
	result.is_timed_out = result.is_timed_out || relevant.is_timed_out;
	result.waited += relevant.waited;
	std::set<std::string> unread(result.unread_document_ids.begin(), result.unread_document_ids.end());
	unread.insert(relevant.unread_document_ids.begin(), relevant.unread_document_ids.end());
	result.unread_document_ids.assign(unread.begin(), unread.end());
	return result;
}

void set_after_extraction_observer_for_testing(std::function<void(const std::string& id)> observer) {
	after_extraction_observer() = std::move(observer);
}

// The Project's folder watcher, started once per session.
//
// The watch is begun outside the lock: beginning one can wait on the system -
// the permission prompt macOS shows for a protected folder, a network drive
// that does not answer - and that wait stalls only this caller, never
// another Matter's documents or chats. The Matter is marked as starting
// meanwhile, so no second watch is begun; one stopped meanwhile is not kept.
static void watch_project_folder(const std::string &id, const matters::Matter &detected) {
	if (!detected.directory_path || detected.directory_path->empty() == true) return;
	// No watch begins once Singularity is closing (stop_watching_all_documents).
	if (platform::lifecycle::is_closing()) return;
	WatchStarter starter;
	{
		const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
		if (active_watchers.count(id) != 0 || starting_watchers.count(id) != 0) return;
		starting_watchers.insert(id);
		starter = watch_starter_for_testing;
	}
	const std::filesystem::path root(*detected.directory_path);
	const platform::FileSystemChangeCallback onChange = [id, root](const platform::FileSystemChangeReport& report) {
		reconcile_and_persist_documents(id, report);
		// The Matter's own records changed - a colleague's computer wrote to the
		// shared folder: whoever shows this Matter here reads it again, as it
		// does after a change made here.
		if (touches_matter_records(root, report) == true) {
			platform::events::publish("matter.changed", nlohmann::json{{"projectId", id}});
		}
	};
	std::unique_ptr<platform::FileSystemWatcher> watcher;
	try {
		watcher = starter ? starter(root, onChange) : platform::start_watching(root, onChange);
	} catch (const std::exception &) {
		watcher = nullptr;
	}
	std::unique_ptr<platform::FileSystemWatcher> unwanted;
	bool isWatching = false;
	{
		const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
		const bool isStillWanted = starting_watchers.erase(id) != 0;
		if (isStillWanted == true && watcher != nullptr && active_watchers.count(id) == 0) {
			active_watchers[id] = std::move(watcher);
			isWatching = true;
		} else {
			unwanted = std::move(watcher);
		}
	}
	// The Matter is open on this computer: once a session, what the shared
	// Matter keeps of it hears so.
	const SharedMatterHooks hooks = installed_shared_hooks();
	if (isWatching == true && hooks.opened) {
		try {
			hooks.opened(id, root);
		} catch (...) {
		}
	}
}

void set_matter_seal_hooks(MatterSealHooks hooks) {
	SealHooksState &state = seal_hooks_state();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.hooks = std::move(hooks);
}

std::optional<std::string> seal_refusal(const std::filesystem::path &folder) {
	const MatterSealHooks hooks = installed_seal_hooks();
	if (!hooks.may_open || !persistence::has_project_data(folder)) return std::nullopt;
	std::string id;
	try {
		id = persistence::load_project(folder).project.id;
	} catch (const std::exception &) {
		return std::nullopt;
	}
	if (id.empty() || is_listed_here(id)) return std::nullopt;
	bool mayOpen = false;
	try {
		mayOpen = hooks.may_open(folder, id);
	} catch (...) {
	}
	if (mayOpen) return std::nullopt;
	return unsealed_refusal();
}

void set_shared_matter_hooks(SharedMatterHooks hooks) {
	SharedHooksState &state = shared_hooks_state();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.hooks = std::move(hooks);
}

// A watch begun or being begun: the Project's documents are kept current by
// it, so a steady-state list is a read, not a rescan.
static bool is_watched(const std::string &id) {
	const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
	return active_watchers.count(id) > 0 || starting_watchers.count(id) > 0;
}

void set_watch_starter_for_testing(WatchStarter starter) {
	const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
	watch_starter_for_testing = std::move(starter);
}

matters::Matter list_documents(const std::string& id) {
	const bool watcherAlreadyActive = is_watched(id);

	// The full, every-file-hashed scan runs only the first time this session
	// observes this Matter (Sec.7/18's startup/restart audit requirement).
	// Once a watcher is active, its own targeted reconciliation is what keeps
	// matter.documents current between calls - this steady-state path is a
	// pure read, never a rehash of the whole Project (correction item 2).
	const matters::Matter afterDetect =
			watcherAlreadyActive == true ? read_project_documents(id) : detect_project_documents(id);
	matters::Matter result = process_project_documents(id);
	result.folder_paths = afterDetect.folder_paths;
	watch_project_folder(id, afterDetect);
	// The first full look this session may have found changes made while Singularity
	// was closed.
	if (watcherAlreadyActive == false) tell_reconciled(id);
	return result;
}

matters::Matter list_documents_for_request(const std::string &id) {
	const bool watcherAlreadyActive = is_watched(id);
	const matters::Matter afterDetect =
			watcherAlreadyActive == true ? read_project_documents(id) : detect_project_documents(id);
	watch_project_folder(id, afterDetect);
	return afterDetect;
}

// A watcher is taken out of the table under the lock and destroyed after it:
// destroying one waits for a callback it may still be running (Platform/
// FileSystemWatcher), and that wait must never hold the lock a callback or
// another caller could need.
void stop_watching_documents(const std::string& id) {
	decltype(active_watchers)::mapped_type stopped;
	{
		const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
		starting_watchers.erase(id);
		const auto found = active_watchers.find(id);
		if (found == active_watchers.end()) return;
		stopped = std::move(found->second);
		active_watchers.erase(found);
	}
}

namespace {

// A request in one line: its spaces collapsed, at most `most` letters, cut
// with an ellipsis.
std::string one_line(const std::string &text, std::size_t most) {
	std::string collapsed;
	bool isSpace = false;
	for (const char character : text) {
		if (character == ' ' || character == '\n' || character == '\t' || character == '\r') {
			isSpace = !collapsed.empty();
			continue;
		}
		if (isSpace) collapsed += ' ';
		isSpace = false;
		collapsed += character;
	}
	std::size_t letters = 0;
	for (std::size_t index = 0; index < collapsed.size(); ++index)
		if ((static_cast<unsigned char>(collapsed[index]) & 0xC0) != 0x80 && ++letters == most)
			return collapsed.substr(0, index) + "\u2026";
	return collapsed;
}

} // namespace

MatterRequestSummary summarize_requests(const matters::Matter &matter) {
	struct Requests {
		long long count = 0;
		const matters::MatterTask *latest = nullptr;
	};
	std::map<std::string, Requests> perChat;
	std::set<std::string> consiliumGroups;
	for (const matters::MatterTask &task : matter.tasks) {
		if (task.consilium_group_id) consiliumGroups.insert(*task.consilium_group_id);
		if (!task.chat_id) continue;
		Requests &requests = perChat[*task.chat_id];
		++requests.count;
		if (requests.latest == nullptr || requests.latest->created_at < task.created_at) requests.latest = &task;
	}
	MatterRequestSummary summary;
	summary.consilium_group_count = static_cast<long long>(consiliumGroups.size());
	for (const matters::MatterChat &chat : matter.chats) {
		ChatRequestSummary entry;
		entry.chat_id = chat.id;
		const auto found = perChat.find(chat.id);
		if (found != perChat.end()) {
			entry.request_count = found->second.count;
			entry.last_request_at = found->second.latest->created_at;
			entry.last_request = one_line(found->second.latest->prompt, 160);
		}
		summary.chats.push_back(std::move(entry));
	}
	return summary;
}

// Every watch ends, each outside the lock, as stop_watching_documents() ends
// one. A watch being begun meanwhile is not kept (watch_project_folder).
void stop_watching_all_documents() noexcept {
	decltype(active_watchers) stopped;
	try {
		const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
		starting_watchers.clear();
		stopped.swap(active_watchers);
	} catch (...) {
		return;
	}
	try {
		stopped.clear();
	} catch (...) {
	}
}

void hold_watcher_without_events_for_testing(const std::string& id) {
	decltype(active_watchers)::mapped_type replaced;
	{
		const std::lock_guard<std::mutex> watcherLock(watcher_mutex);
		replaced = std::move(active_watchers[id]);
		active_watchers[id] = nullptr;
	}
}

void open_folder(const std::string &id) {
  std::optional<std::string> directory;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto all = matters::load_matters(node::require_active_workspace_path());
    const auto found = std::find_if(all.begin(), all.end(),
        [&](const matters::Matter &entry) { return entry.id == id; });
    if (found == all.end()) throw std::runtime_error("This matter no longer exists.");
    directory = found->directory_path;
  }
  if (!directory) throw std::runtime_error("Choose this Matter's project folder first.");
  if (!platform::open_directory_in_file_manager(*directory))
    throw std::runtime_error("This Matter folder is no longer available.");
}

matters::Matter save_ai_setup_overrides(const std::string &id,
                                        std::vector<matters::AiSetupRoleOverride> overrides) {
  // Structural validation only (a real Beta 1 role) - whether the named
  // model is actually installed/connected and compatible with the role is a
  // live-registry lookup the Bridge layer already performs before calling
  // here (Bridge/MatterBridge/MatterBridge.cpp's save_ai_setup()), so this
  // domain action stays free of the local-model/provider runtime's build
  // footprint.
  for (const auto &override_entry : overrides) {
    if (!routing::ai_setup_role_from_string(override_entry.role)) {
      throw std::runtime_error("This is not a real AI Setup role.");
    }
  }

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace_path, id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  found->ai_setup_overrides = std::move(overrides);
  found->updated_at = workspaces::current_timestamp();
  matters::save_matter(workspace_path, *found);
  return *found;
}

ProjectDataStatus get_project_data_status(const std::string &id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const matters::Matter entry = require_matter(id);
  if (entry.data_state == matters::ProjectDataState::Unavailable || entry.data_state == matters::ProjectDataState::Damaged)
    return unusable_project_data_status(entry);
  if (!entry.directory_path || entry.directory_path->empty()) return ProjectDataStatus{};
  const std::filesystem::path project_path(*entry.directory_path);
  if (persistence::has_project_data(project_path) == false) return ProjectDataStatus{};
  return project_data_status_of(persistence::load_project(project_path));
}

ProjectDataStatus set_up_project_data(const std::string &id, bool start_empty) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  matters::Matter entry = require_matter(id);
  if (entry.data_state == matters::ProjectDataState::Damaged)
    throw matters::ProjectDataError(entry.data_state,
        "This Project's Singularity data can't be read, so nothing was set up. Repair or restore its .lm folder, or "
        "remove it first.");
  if (entry.data_state == matters::ProjectDataState::Unavailable) {
    if (entry.data_state_reason == "access_denied")
      throw matters::ProjectDataError(entry.data_state,
          "Singularity can't access this Project's folder, so nothing was set up and nothing was changed. Restore access "
          "to the folder, then try again.");
    // MIG-5: data this build is too old to read is not missing data. Setting it
    // up again would write an empty Project over work that is perfectly fine.
    if (entry.data_state_reason == "requires_newer_version")
      throw matters::ProjectDataError(entry.data_state,
          "This Project was saved by a newer version of Singularity, so nothing was set up and nothing was changed. "
          "Update Singularity to open it - its data is fine and must not be replaced.");
    if (entry.data_state_reason != "data_missing")
      throw matters::ProjectDataError(entry.data_state,
          "This Project's folder isn't available, so nothing was set up. Reconnect the folder first.");
    if (!start_empty)
      throw matters::ProjectDataError(entry.data_state,
          "This Project's Singularity data isn't in its folder. If it is still syncing from another device, wait for it "
          "to arrive. To start over with empty Project data, choose Start over.");
    // Starting over keeps the Project's identity and folder; everything else
    // begins empty.
    const std::string timestamp = workspaces::current_timestamp();
    entry.data_state = matters::ProjectDataState::Legacy;
    entry.data_state_reason.clear();
    if (entry.created_at.empty()) entry.created_at = timestamp;
    entry.updated_at = timestamp;
  }
  const std::filesystem::path project_path(require_directory_path(entry));
  const platform::ActorIdentity actor = platform::resolve_actor_identity();
  const persistence::ProjectRecord record = persistence::initialize_project(project_path, entry, actor);
  seal_new_matter(project_path, entry.id);
  return project_data_status_of(record);
}

namespace {

// The words the product itself uses for each stored collection, so a conflict
// reads as "a Chat" rather than as a storage key. An unknown collection keeps
// its own name rather than being hidden: a conflict nobody can see is the
// defect this exists to remove.
std::string conflict_area_for(const std::string &collection) {
	if (collection.empty()) return "Project details";
	if (collection == "documents" || collection == "documentEvents") return "Documents";
	if (collection == "notes") return "Notes";
	if (collection == "issues") return "Issues";
	if (collection == "workProducts" || collection == "workProductRevisions"
			|| collection == "workProductTypes") return "Work Products";
	if (collection == "consiliumConfigurations") return "Consilium";
	if (collection == "chats") return "Chats";
	if (collection == "tasks") return "Requests";
	if (collection == "entities" || collection == "entityAmbiguities") return "Entities";
	if (collection == "documentProtection") return "Privacy";
	if (collection == "contextItems" || collection == "contextSuggestions"
			|| collection == "contextItemRevisions") return "Project Context";
	return collection;
}

// A name a person would recognise, taken from whichever field this kind of
// object actually carries one in.
std::string conflict_label_for(const nlohmann::json &current, const nlohmann::json &attempted,
		const std::string &object_id) {
	for (const nlohmann::json *value : {&current, &attempted}) {
		if (!value->is_object()) continue;
		for (const char *key : {"title", "label", "name", "displayName", "prompt"}) {
			if (value->contains(key) && value->at(key).is_string()
					&& !value->at(key).get<std::string>().empty())
				return value->at(key).get<std::string>();
		}
	}
	return object_id;
}

} // namespace

std::vector<ProjectConflictSummary> list_project_conflicts(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	if (!matter.directory_path || !persistence::has_project_data(*matter.directory_path)) return {};
	const persistence::ProjectRecord record = persistence::load_project(*matter.directory_path);
	std::vector<ProjectConflictSummary> result;
	for (const persistence::ProjectConflict &conflict : record.conflicts) {
		const nlohmann::json current = nlohmann::json::parse(conflict.current_json, nullptr, false);
		const nlohmann::json attempted = nlohmann::json::parse(conflict.attempted_json, nullptr, false);
		ProjectConflictSummary summary;
		summary.id = conflict.id;
		summary.collection = conflict.collection;
		summary.area = conflict_area_for(conflict.collection);
		summary.object_id = conflict.object_id;
		summary.label = conflict_label_for(current, attempted, conflict.object_id);
		summary.field = conflict.field;
		summary.kind = conflict.kind;
		summary.current_json = conflict.current_json;
		summary.attempted_json = conflict.attempted_json;
		summary.current_actor_id = conflict.current_actor_id;
		summary.current_timestamp = conflict.current_timestamp;
		summary.authored_actor_id = conflict.actor_id;
		summary.authored_timestamp = conflict.timestamp;
		result.push_back(std::move(summary));
	}
	return result;
}

void resolve_project_conflict(const std::string &id, const std::string &conflict_id,
		const std::string &resolution) {
	if (resolution != "keep_current" && resolution != "use_mine")
		throw std::runtime_error("Choose whether to keep the Project's current value or use yours.");
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	if (!matter.directory_path || !persistence::has_project_data(*matter.directory_path))
		throw std::runtime_error("This Project does not have shared Project data.");
	persistence::ProjectRecord record = persistence::load_project(*matter.directory_path);
	const auto found = std::find_if(record.conflicts.begin(), record.conflicts.end(),
		[&](const persistence::ProjectConflict &conflict) { return conflict.id == conflict_id; });
	if (found == record.conflicts.end()) throw std::runtime_error("This conflict was already resolved.");
	if (resolution == "use_mine") {
		// Collection-agnostic by construction: the Project's own storage shape is
		// the one place that knows how every object is written, so re-applying a
		// conflicting value never needs a rule per collection - and a collection
		// added later is answerable the day it exists.
		nlohmann::json state = matters::serialize_matter_storage(record.project);
		const nlohmann::json attempted = nlohmann::json::parse(found->attempted_json, nullptr, false);
		if (found->collection.empty()) {
			if (found->field.empty()) throw std::runtime_error("This conflict cannot be answered automatically.");
			state[found->field] = attempted;
		} else {
			if (!state.contains(found->collection) || !state.at(found->collection).is_array())
				throw std::runtime_error("This Project no longer stores that kind of object.");
			nlohmann::json &collection = state.at(found->collection);
			nlohmann::json rebuilt = nlohmann::json::array();
			bool replaced = false;
			for (nlohmann::json &entry : collection) {
				const bool sameObject = entry.is_object() && entry.value("id", std::string()) == found->object_id;
				if (!sameObject) { rebuilt.push_back(entry); continue; }
				replaced = true;
				// A null attempted value is this device's own delete.
				if (attempted.is_object()) rebuilt.push_back(attempted);
			}
			if (!replaced && attempted.is_object()) rebuilt.push_back(attempted);
			collection = std::move(rebuilt);
		}
		// The storage shape is the Project's CONTENT. Everything that describes
		// how this object was loaded - its merge baseline above all, plus how it
		// was read and whether it is a cut-over locator - is process-local and
		// deliberately absent from it, so it is carried across rather than lost
		// (a rebuilt Project with no baseline cannot be saved at all).
		matters::Matter rebuilt = matters::deserialize_matter_storage(state);
		rebuilt.storage_baseline = record.project.storage_baseline;
		rebuilt.data_state = record.project.data_state;
		rebuilt.data_state_reason = record.project.data_state_reason;
		rebuilt.storage_cutover = record.project.storage_cutover;
		rebuilt.folder_paths = record.project.folder_paths;
		record.project = std::move(rebuilt);
	}
	record.project.updated_at = workspaces::current_timestamp();
	persistence::save_project_conflict_resolution(
		record.project, platform::resolve_actor_identity(), conflict_id);
}

void reveal_project_data(const std::string &id) {
  std::string directory_path;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    directory_path = require_directory_path(require_matter(id));
  }
  persistence::reveal_project_data(std::filesystem::path(directory_path));
}

std::string prepare_project_data_clean_copy(
    const std::string &id, const std::optional<std::string> &destination_directory) {
  if (!destination_directory) return "";
  std::string directory_path;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    directory_path = require_directory_path(require_matter(id));
  }
  const std::filesystem::path destination = persistence::prepare_clean_copy(
      std::filesystem::path(directory_path), std::filesystem::path(*destination_directory));
  return destination.u8string();
}

// Deletes .lm only. Never writes name/description/instructions or any other
// .lm-owned content back into matters.json afterward: doing so would recreate
// exactly the second authoritative Project snapshot D-002 forbids, right at
// the moment .lm truth is removed. matters.json already holds nothing but
// this Matter's locator (id/directoryPath - see MatterRepository.cpp's
// locator_stub()) while cut over, so there is nothing to "restore" here; a
// restart reads that same bare locator, never a resurrected snapshot. User
// documents are untouched - only the .lm folder itself is removed.
namespace {
std::mutex &removal_guard_mutex() {
  static std::mutex mutex;
  return mutex;
}
RemovalGuard &removal_guard() {
  static RemovalGuard guard;
  return guard;
}
} // namespace

void set_removal_guard(RemovalGuard guard) {
  const std::lock_guard<std::mutex> lock(removal_guard_mutex());
  removal_guard() = std::move(guard);
}

void remove_project_data(const std::string &id) {
  std::string directory_path;
  std::filesystem::path workspace_path;
  RemovalGuard guard;
  {
    const std::lock_guard<std::mutex> lock(removal_guard_mutex());
    guard = removal_guard();
  }
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    workspace_path = node::require_active_workspace_path();
    const matters::Matter entry = require_matter(id);
    // Removing Project data is destructive and unrecoverable: it applies only
    // to this Project's own readable data, never to a folder Singularity cannot
    // read right now and never to another Project's data that happens to sit
    // where this one's location points.
    if (entry.data_state != matters::ProjectDataState::Ready
        && entry.data_state != matters::ProjectDataState::Legacy)
      throw matters::ProjectDataError(entry.data_state,
          entry.data_state_reason == "access_denied"
            ? "Singularity can't access this Project's folder, so nothing was removed."
            : "This Project's data can't be read right now, so nothing was removed.");
    directory_path = require_directory_path(entry);
    if (persistence::has_project_data(std::filesystem::path(directory_path))) {
      const std::string stored_id = persistence::load_project(std::filesystem::path(directory_path)).project.id;
      if (stored_id != entry.id)
        throw matters::ProjectDataError(matters::ProjectDataState::Damaged,
            "The data in this folder belongs to a different Project, so nothing was removed.");
    }
    if (guard)
      if (const std::optional<std::string> refusal = guard(std::filesystem::path(directory_path)))
        throw std::runtime_error(*refusal);
    // Removed under the lock, so a run or an agent saving meanwhile cannot
    // recreate half of it (audit PERS-4).
    persistence::remove_project_data(std::filesystem::path(directory_path));
  }
  // What the Project's requests sent goes with its data.
  routing::sent_passages_folder::forget_project(workspace_path, id);
}

std::vector<std::string> reconcile_relocated_projects(const std::vector<persistence::ProjectSummary> &discovered) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  std::vector<std::string> repaired;
  if (discovered.empty()) return repaired;
  const std::vector<matters::Matter> known = list();
  for (const persistence::ProjectSummary &summary : discovered) {
    if (summary.id.empty() || summary.has_duplicate_location) continue;
    const auto existing = std::find_if(known.begin(), known.end(),
        [&](const matters::Matter &entry) { return entry.id == summary.id; });
    if (existing == known.end()) continue;
    std::error_code canonical_error;
    const std::string discovered_canonical =
        std::filesystem::weakly_canonical(std::filesystem::path(summary.path), canonical_error).u8string();
    if (canonical_error) continue;
    const bool already_correct = existing->directory_path.has_value() &&
        std::filesystem::weakly_canonical(std::filesystem::path(*existing->directory_path), canonical_error).u8string() ==
            discovered_canonical &&
        !canonical_error;
    if (already_correct) continue;
    (void)assign_folder(summary.id, summary.path);
    repaired.push_back(summary.id);
  }
  return repaired;
}

std::vector<persistence::ProjectSummary> discover_projects_for_scope(const platform::ProjectDiscoveryScope &scope) {
  if (scope.roots.empty()) return {};
  // The walk reads only the folders' listings and can take minutes over a
  // large folder, a home or a Desktop: it runs before the store's lock, so
  // every other read goes on meanwhile (2026-11-02: what the sidebar's
  // Matters said last waited minutes behind a walk of the Desktop). What it
  // found is read under the lock.
  const std::vector<std::filesystem::path> folders = persistence::find_project_folders(scope.roots);
  // Each Matter found is read through the store's own cache, which keeps its
  // own lock, and never under the store's: a first read of many Matters takes
  // seconds, and the firm's exchange and every other read wait for nothing
  // meanwhile (2026-11-02: a thread archived on another computer stood 66
  // seconds on its way here behind such a read). Only the list it changes is
  // changed under the lock.
  const std::vector<persistence::ProjectSummary> discovered = persistence::summarize_project_folders(folders);
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    (void)reconcile_relocated_projects(discovered);
  }
  // What was found is offered as the firm's only when it is: listed here
  // already, or sealed by one of the firm's computers (D-167). A `.lm` from
  // outside the organisation, or written by hand, stays where it is, unoffered.
  // A Matter the member removed from this computer is never offered again.
  const MatterSealHooks hooks = installed_seal_hooks();
  std::set<std::string> listed;
  std::set<std::string> removed;
  try {
    const std::filesystem::path workspace_path = node::require_active_workspace_path();
    for (const matters::Matter &matter : matters::load_matters(workspace_path)) listed.insert(matter.id);
    removed = removed_here(workspace_path);
  } catch (const std::exception &) {
  }
  std::vector<persistence::ProjectSummary> offered;
  for (const persistence::ProjectSummary &summary : discovered) {
    if (summary.id.empty() == false && listed.count(summary.id) == 0 && removed.count(summary.id) != 0) continue;
    if (!hooks.is_firms) {
      offered.push_back(summary);
      continue;
    }
    bool isFirms = summary.id.empty() || listed.count(summary.id) != 0;
    if (!isFirms) {
      try {
        isFirms = hooks.is_firms(std::filesystem::u8path(summary.path), summary.id);
      } catch (...) {
      }
    }
    if (isFirms) offered.push_back(summary);
  }
  return offered;
}

std::vector<persistence::ProjectSummary> discover_projects() {
  const platform::AppSettings settings = platform::load_app_settings();
  return discover_projects_for_scope(platform::resolve_project_discovery_scope(settings));
}

} // namespace singularity::matter
