#pragma once

#include <filesystem>

#include "Matter/Types/Matter.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"
#include "Platform/PermissionAccess/PermissionAccess.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matter {

struct TextPatch {
  bool supplied = false;
  std::optional<std::string> value;
};

struct UpdateMatter {
  std::string id;
  TextPatch name;
  TextPatch description;
  TextPatch instructions;
  // The Project's short name and mark, replaced as one value when supplied.
  std::optional<matters::ProjectAppearance> appearance;
  // Archived or restored, when supplied.
  std::optional<bool> archived;
  // The rules of its correspondence intake, replaced as one list when
  // supplied.
  std::optional<std::vector<correspondence::IntakeRule>> correspondence_intake;
};

// The firm's Matters known here - never the member's desk.
std::vector<matters::Matter> list();
// The Matters the member's list shows: every Matter known here but one sealed
// for another organisation than the one signed in, which stays on this
// computer and is listed again once that organisation is.
std::vector<matters::Matter> listed();

// The member's desk: the project in this computer's own data that keeps the
// member's personal chats with the firm's AI colleagues. It is in the
// application's data, never in the workspace folder, which may be the firm's
// share, and it is never listed, shared or offered as a Matter.
std::filesystem::path desk_folder();
bool is_desk(const matters::Matter &matter);

// The folder of a Matter of the firm whose .lm this computer holds, for a
// record that travels with the Matter to each of its computers. Nothing for
// the member's desk or a personal conversation, both kept in this computer's
// own data, nor for a Matter not set up or not readable here.
std::optional<std::filesystem::path> records_folder_of(const std::string &matter_id);
// Every such folder this computer holds.
std::vector<std::filesystem::path> records_folders();
// The desk's project id, the desk opened from its folder - made the first time
// it is asked for - or "" when it cannot be. Never call it with the
// persistence lock held.
std::string desk_project_id();

// A personal conversation of two members: the project kept in the folder of
// their personal channel (Matter/MatterCoordination), in this computer's own
// data, never in a Matter's folder or the firm's storage. Its id comes from the
// channel's ("pc_<hex>" is "mt_<hex>"), so either computer of the two may make
// it and both open the same project. It is never listed, shared or offered as a
// Matter.
std::string personal_project_id(const std::string &channel_id);
bool is_personal_project(const matters::Matter &matter);
// The conversation's project in `folder`: opened when the folder holds it -
// made on this computer or arrived from the other member's - and made there,
// under `id` and the firm's seal, when it does not. Never call it with the
// persistence lock held.
matters::Matter open_personal_project(const std::filesystem::path &folder, const std::string &id);
matters::Matter create(std::string name, std::optional<std::string> description,
                       std::optional<std::string> instructions);
matters::Matter update(UpdateMatter command);
std::vector<matters::Matter> remove(const std::string &id);
// New Project or Open existing Project from a folder the user chose. A folder
// that already holds a Project's `.lm` data is opened under that Project's own
// identity - added to the list once, relinked when that Project was listed at
// a location that is gone, refused when the same Project is already open from
// another folder (a copy) or when its data is damaged. A folder without `.lm`
// becomes a new Project with its data set up before it is listed.
matters::Matter create_or_open_from_folder(const std::string &selected_path);

// The Matter a folder holds already, as New Matter says it: its id and name,
// and where it stands on this computer - "trashed" in the Trash, "listed"
// among the Matters, or "away", deleted from this computer or never opened
// here, though the firm's other computers may keep it. None for a folder that
// holds no Matter. A Matter deleted here is never a reason its folder refuses
// New Matter with no way back (a design partner, 2026-11-05).
struct FolderMatter {
  std::string id;
  std::string name;
  std::string state;
};
// `is_in_trash` says whether a Matter stands in this computer's Trash
// (Matter/MatterTrash), which the bridge keeps beside the list.
std::optional<FolderMatter> matter_in_folder(const std::filesystem::path &folder,
    const std::function<bool(const std::string &)> &is_in_trash);
// The sample matter, set up so a lawyer can try Singularity without opening a
// client's files: the bundled matter folder is copied once into
// `destination_root`, keeping its name, and opened as a Project like any
// folder. A copy already there is opened as it is - never overwritten, so
// nothing the lawyer did in it is lost.
matters::Matter create_sample(const std::filesystem::path &bundled_matter,
                              const std::filesystem::path &destination_root);
// A new Project begun by its name, without a folder dialog (Home's request):
// its folder is made in `destination_root` under that name - beside a folder
// of the same name, never in it, as "Name 2", "Name 3" - and opened as a
// Project like any folder. A name no folder can carry is refused in words.
matters::Matter create_named(const std::string &name,
                             const std::filesystem::path &destination_root);

// Records where a Project lives. A folder that holds no `.lm` of its own has
// this Project's data set up in it here, so a Project never ends up listed at a
// folder that holds nothing of it; a folder that already holds Project data is
// never written over. An unset path means the caller's dialog was cancelled and
// nothing changes.
matters::Matter assign_folder(const std::string &id,
                              const std::optional<std::string> &selected_path);
void open_folder(const std::string &id);

// Phase 08 (Documents - filesystem, list and states) - the reconciliation
// phases (Document/DocumentRegistry/DocumentRegistry.hpp), each independently
// PERSISTED under its own PersistenceLock/save_matters() call: Changed and
// Processing are real states a concurrent matter.listDocuments call (or a
// direct .lm read) can observe once a detect phase has committed, before
// process_project_documents() ever runs - not merely an internal transient
// inside one combined call.
//
// detect_project_documents() is the FULL, every-file-hashed scan - the
// startup/restart audit path list_documents() below runs only the first time
// a Matter is observed in a session (Sec.7/18: correctness must never depend
// on watcher history). detect_affected_paths() is the targeted, steady-state
// counterpart the filesystem watcher callback (and every later
// list_documents() call in the same session) uses instead, so a real Project
// is not rehashed end to end on every poll/event - see
// Document/DocumentRegistry/DocumentRegistry.hpp::reconcile_affected_paths().
// read_project_documents() is a pure read (no scan/hash at all beyond a
// cheap folder-only refresh) for a steady-state list_documents() call with
// nothing new to target. All three, and process_project_documents(), are
// exposed directly so the filesystem watcher callback and tests can drive/
// observe each phase's real persisted effect on its own.
matters::Matter detect_project_documents(const std::string& id);
matters::Matter detect_affected_paths(const std::string& id, const std::vector<std::string>& affected_absolute_paths);
matters::Matter read_project_documents(const std::string& id);
matters::Matter process_project_documents(const std::string& id);

// The first time this is called for a given Matter in this process (no
// background watcher active for it yet), runs the full startup/restart audit
// (detect_project_documents()) and starts a background filesystem watcher so
// later external changes are reconciled automatically without requiring
// another full scan (see Platform/FileSystemWatcher/FileSystemWatcher.hpp).
// Every later call in the same session is a cheap read
// (read_project_documents()) plus process_project_documents() for whatever
// the watcher's own targeted reconciliation already left pending - it never
// repeats the full-tree hash. Safe to call repeatedly; a Matter with no
// folder assigned yet simply returns its current (empty) document list.
matters::Matter list_documents(const std::string& id);

// What waits to be read in the Matter's documents, read on a thread of its
// own; a pass already reading them reads these too before it ends. Each
// document read is saved, and the save tells whoever shows the Matter.
void read_documents_in_background(const std::string& id);

// The window's list of the documents: list_documents without waiting for
// any of them to be read - what waits is read in the background
// (read_documents_in_background), and the window reads the list again as
// each is read. A file a member pasted or dropped into a reply showed only
// once it had been read.
matters::Matter list_documents_without_waiting(const std::string& id);

// Phase 09's "Try again" on a document stuck at Needs attention -
// Document/DocumentRegistry/DocumentRegistry.hpp::request_reprocessing()
// immediately followed by the same process_project_documents() above. A
// no-op (document unchanged) when the document is not currently eligible -
// see that function's own comment.
matters::Matter reprocess_document(const std::string& id, const std::string& document_id);

// What waiting for a Project's documents to be read found.
struct DocumentReadingWait {
	// Documents that were still being read when the wait began (0: nothing was,
	// and the call returned at once).
	std::size_t awaited_count = 0;
	// Documents still being read when it ended: none unless the time ran out.
	std::vector<std::string> unread_document_ids;
	bool is_timed_out = false;
	std::chrono::milliseconds waited{0};
};

// A request that is about to read the Project's documents waits here for any
// reading already under way, so a file dropped into the folder a moment ago is
// read before the request is answered from it - not skipped, and never an
// error. Nothing being read: returns at once. A pass reading now: asks it to
// read once more and waits until a round that STARTED after that request has
// COMPLETED - what the running round took its snapshot before is then read
// too. No pass running but documents left pending: reads them here, on the
// caller's thread. `timeout` bounds the time spent waiting on another caller's
// pass; what is still unread when it runs out is named in the result, and the
// request goes on without it.
//
// `on_waiting` is called once, with the number of documents being read, only
// when there is something to wait for (the caller's "Reading 2 documents"
// step).
//
// MUST NOT be called while the calling thread holds the PersistenceLock: the
// pass it waits for records its readings under that lock. It holds no lock of
// its own while it waits, so the Documents list, a status poll, a save or a
// Stop are never held up by a waiting request.
DocumentReadingWait wait_for_documents_read(const std::string &id, std::chrono::milliseconds timeout,
		const std::function<void(std::size_t awaited_count)> &on_waiting = {});

// The most documents a request that names none reads before it goes on: a
// few files just added to the folder or saved again, never a data room.
constexpr std::size_t kFewDocumentsForARequest = 8;

// What a request waits for, when it is about to read the Project: only the
// documents it needs, never the whole of a reading under way.
//
// `needed_document_ids` are the documents the request names - its selection,
// its target. Each of them still pending is read here, on the caller's thread,
// one by one, whatever else the Project is reading; one already read, and every
// other document, is not waited for. None named (Relevant Project sources): a
// few documents just added or saved (kFewDocumentsForARequest at most) are read
// here the same way, so a file dropped into the folder a moment ago is read
// before the request is answered from it; more than that is a data room being
// read, and the request does not wait for it at all. What the request does not
// read itself is read by a pass of its own, started now when none runs.
//
// `timeout` is checked between documents. The result names what is still being
// read when the request goes on - of the named documents only those, with none
// named every pending document the answer could have drawn from - so the request
// can say so; awaited_count is what was read here. Never under the
// PersistenceLock (see wait_for_documents_read() above).
DocumentReadingWait wait_for_needed_documents_read(const std::string &id,
		const std::vector<std::string> &needed_document_ids, std::chrono::milliseconds timeout,
		const std::function<void(std::size_t awaited_count)> &on_waiting = {});

// The wait for one request: its selection and its explicit target are waited
// for as named documents (`named_timeout`); with no selection it also reads the
// few documents just added, as a request that names none does
// (`relevant_timeout`) - a target never turns a Relevant request into one that
// ignores a file dropped into the folder a moment ago. Never under the
// PersistenceLock.
DocumentReadingWait wait_for_request_documents_read(const std::string &id,
		const std::vector<std::string> &selected_document_ids, const std::optional<std::string> &target_document_id,
		std::chrono::milliseconds named_timeout, std::chrono::milliseconds relevant_timeout,
		const std::function<void(std::size_t awaited_count)> &on_waiting = {});

// list_documents() for a request: the same first-time detection and folder
// watcher, but the reading of what is pending is never done on the caller's
// thread - it is left to a pass of its own (started when none runs), and the
// request waits only for what it needs (wait_for_needed_documents_read()).
matters::Matter list_documents_for_request(const std::string &id);

// Test-only seam: called once per pass of process_project_documents()/
// reprocess_document(), after the documents were read and before their
// readings are recorded - the moment the Project's lock is free. Empty (the
// default) in production.
void set_after_extraction_observer_for_testing(std::function<void(const std::string& id)> observer);

// Test/lifecycle seam: stops any background watcher list_documents()
// started for `id`. Not required for normal operation.
void stop_watching_documents(const std::string& id);

// A chat as a rail, Home or a header shows it: how many requests it holds, and
// its latest in one line (at most 160 letters).
struct ChatRequestSummary {
	std::string chat_id;
	long long request_count = 0;
	std::optional<std::string> last_request_at;
	std::optional<std::string> last_request;
};

// The requests of `matter` by chat, in the order of its chats, and how many
// Consilium runs it holds. Reads the Matter it is given; loads nothing.
struct MatterRequestSummary {
	std::vector<ChatRequestSummary> chats;
	long long consilium_group_count = 0;
};
MatterRequestSummary summarize_requests(const matters::Matter &matter);

// Singularity is closing: every folder watch ends, and none begins again
// once closing has begun (Platform/AppLifecycle). Once this returns no watch's
// callback is running. The Native Bridge calls it last as it shuts down, and
// a test calls it before it ends, so no callback outlives what it uses.
void stop_watching_all_documents() noexcept;

// What a Project's folder watch tells the part of Singularity that keeps a
// Matter shared between computers (Matter/SharedMatter): once when the watch
// begins - the Matter is open on this computer - and after each change it
// reconciled, the first full look included. Called with no lock held, never
// on the window's thread. Empty unless the application installs them.
struct SharedMatterHooks {
	std::function<void(const std::string &project_id, const std::filesystem::path &folder)> opened;
	std::function<void(const std::string &project_id)> reconciled;
};
void set_shared_matter_hooks(SharedMatterHooks hooks);

// The firm's seal on its Matters: every Matter this computer makes is sealed
// at once, and a Matter not listed here is offered by discovery, or opened
// from its folder, only when it is the firm's - so a `.lm` from outside the
// organisation, or one written by hand, never enters it as the firm's. The
// application installs these from Matter/SharedMatter. Empty, nothing is
// sealed or refused: a test that needs neither.
struct MatterSealHooks {
	// Seals the Matter in `folder` with this computer's seal for its firm.
	std::function<void(const std::filesystem::path &folder, const std::string &matter_id)> seal;
	// Whether the Matter in `folder` is the firm's: discovery offers it.
	std::function<bool(const std::filesystem::path &folder, const std::string &matter_id)> is_firms;
	// Whether the person may open it from its folder: the firm's, or sealed for
	// the firm by a computer not confirmed here yet.
	std::function<bool(const std::filesystem::path &folder, const std::string &matter_id)> may_open;
	// Whether the Matter in `folder` is sealed for another organisation than
	// the one signed in: the member's list leaves it out (listed()).
	std::function<bool(const std::filesystem::path &folder, const std::string &matter_id)> is_other_organisations;
};
void set_matter_seal_hooks(MatterSealHooks hooks);
// Why the Matter in `folder` is not opened here, in the words the person
// reads; nothing when it may be: it is listed here already, it may be opened
// (MatterSealHooks::may_open), or no seal is asked for (no hooks).
std::optional<std::string> seal_refusal(const std::filesystem::path &folder);

// Test-only seam: begins a Project's folder watch in place of the platform's
// own (empty restores it), so a watch that waits on a permission prompt can
// be proven to stall nothing but itself. Production code must never call this.
using WatchStarter = std::function<std::unique_ptr<platform::FileSystemWatcher>(
		const std::filesystem::path& root, platform::FileSystemChangeCallback on_change)>;
void set_watch_starter_for_testing(WatchStarter starter);

// Test-only seam: marks `id` as having an active watcher whose report has not
// arrived yet (no OS stream is started), so list_documents() takes its real
// steady-state no-rescan branch. Production code must never call this.
void hold_watcher_without_events_for_testing(const std::string& id);

// Test-only seam (Phase 08 correction round 3, item 1): runs the exact real
// dispatch a watcher callback performs - full reconciliation
// (detect_project_documents()) when `report.scope` is
// kFullReconciliationRequired, targeted reconciliation
// (detect_affected_paths()) otherwise - then process_project_documents(),
// against a synthetic platform::FileSystemChangeReport. Lets the "OS event
// coverage lost -> broader reconciliation" fallback be proven deterministically:
// a genuine FSEvents MustScanSubDirs/dropped-event condition is an OS-level
// buffer-overflow signal a test cannot reliably force to occur. Production
// code always reaches this dispatch through a real
// platform::FileSystemWatcher callback instead.
matters::Matter reconcile_from_watcher_report_for_testing(
		const std::string& id, const platform::FileSystemChangeReport& report);

// Persists this Matter's explicit Project-level AI Setup differences from
// the Node default. `overrides` replaces the full override set - it is
// itself the sparse list of explicit differences, so passing an empty
// vector is exactly "Reset to Singularity default" (never a delete of the
// Matter's other configuration). Each entry is validated against
// Routing::AiSetup's real role-compatibility rules before it is persisted;
// an incompatible role/model pairing throws instead of being silently
// saved.
matters::Matter save_ai_setup_overrides(const std::string &id,
                                        std::vector<matters::AiSetupRoleOverride> overrides);

// This is presence/health only (never durable content) so it is safe to
// query often without loading and replaying the whole Project.
struct ProjectDataStatus {
  bool available = false;
  std::string path;
  long long operation_count = 0;
  long long conflict_count = 0;
  // "ready", "not_set_up" (never created), "unavailable" (folder or data
  // missing) or "damaged" (data that cannot be read); state_reason names why
  // for the last two (folder_missing, data_missing, unreadable, ...).
  std::string state = "not_set_up";
  std::string state_reason;
};

ProjectDataStatus get_project_data_status(const std::string &id);

// Runs the explicit, versioned, recoverable migration from this Matter's
// legacy state to portable Project-folder + .lm truth (idempotent: if .lm
// already exists, this only loads and reports it). Requires the Matter to
// already have a chosen folder; legacy .singularity-workspace data is preserved untouched.
//
// Never over data it cannot see: a Project whose folder is gone, or whose data
// is damaged, is refused (ProjectDataError). A Project whose folder is present
// but whose data is missing - still syncing from another device, or removed on
// purpose - is refused unless `start_empty` confirms starting over with empty
// Project data under the same Project identity.
ProjectDataStatus set_up_project_data(const std::string &id, bool start_empty = false);

void reveal_project_data(const std::string &id);

// Returns "" if destination_directory is unset (the caller's folder-choose
// dialog was cancelled), matching assign_folder's cancel convention.
std::string prepare_project_data_clean_copy(
    const std::string &id, const std::optional<std::string> &destination_directory);

void remove_project_data(const std::string &id);
// Before a Matter's Singularity data is removed, the application may refuse:
// it returns the reason in a person's words, or nothing to allow it. The
// application installs it - a Matter other people work in keeps its data until
// the owners decide how it is removed for everyone. With none installed,
// removal is allowed, as before.
using RemovalGuard = std::function<std::optional<std::string>(const std::filesystem::path &project_folder)>;
void set_removal_guard(RemovalGuard guard);

// Two people (or one person on two devices) working in one Project folder can
// change the same Chat, task, Work Product, note, entity or instruction at
// once; the store already detects and records that, but until now only Context
// conflicts could be seen or answered, so every other one was a change that
// silently did not happen. A person can only trust a shared Project if it
// tells them that.
struct ProjectConflictSummary {
	std::string id;
	// The collection as the store records it, and the same thing in the words
	// the product uses for it ("Chats", "Work Products", ...).
	std::string collection;
	std::string area;
	std::string object_id;
	// What the object is called where it has a name, so the person recognises
	// what the conflict is about without reading JSON - always the object's own
	// name, never the field that happened to conflict.
	std::string label;
	// The conflicting field, when the conflict is about one field rather than
	// the whole object ("title", "instructions", ...). Empty otherwise.
	std::string field;
	std::string kind; // "update" or "delete"
	// What the Project holds now, and what this device tried to save. Empty
	// `attempted_json` means this device tried to delete the object.
	std::string current_json;
	std::string attempted_json;
	std::string current_actor_id;
	std::string current_timestamp;
	std::string authored_actor_id;
	std::string authored_timestamp;
};

[[nodiscard]] std::vector<ProjectConflictSummary> list_project_conflicts(const std::string &id);

// Answers one conflict: "keep_current" keeps what the Project holds and records
// that decision durably; "use_mine" re-applies exactly what this device tried
// to save. Either way the decision is a real attributed Project operation, so
// it survives a reload and the conflict never re-appears.
void resolve_project_conflict(const std::string &id, const std::string &conflict_id,
	const std::string &resolution);

// Real, actually-discovered/restored Projects - "Project discovery/ restoration" -
// recursively scanning for .lm the same way Singularity recognizes a Project at
// every launch. Not limited to Matters already known to matters.json: a Project
// copied or shared into an accessible location is found here too, with no
// per-Project registration step. Manual per-Matter folder selection
// (assign_folder) remains the fallback for a Project outside every accessible root
// either mode covers.
//
// The roots actually searched come from
// platform::resolve_project_discovery_scope() - Limited to exactly Phase 02
// (Permissions and local access)'s authorised, currently-available selected
// folders, unless the user has explicitly authorized broader Project
// discovery, in which case it is Broad with every genuinely-accessible
// platform::broad_local_access_roots() entry. This process' own HOME being
// filesystem-readable is never by itself treated as that authorization -
// see PermissionAccess.hpp for the real per-root filesystem check Broad
// resolves against once it applies, and for why Limited is what currently
// every reachable Phase 02 permission state resolves to.
//
// Multiple discovered locations that share a Project id (a copy, a
// shared-drive duplicate) are all preserved and returned in either mode,
// never deduplicated or silently read as one location having "moved" - see
// ProjectSummary::has_duplicate_location. Empty (never throws) when no root
// is accessible yet, since this backs a lightweight shell-level entry point
// (the command palette), not an onboarding flow or a general search
// feature.
//
// Also self-heals real "Project mobility": a Matter already known to matters.json
// whose folder was moved/renamed outside Singularity (Finder, a sync client) -
// real, empirical evidence is that this scan alone finds and displays it correctly
// by name, but matters.json's own stored directory_path is a separate record that
// a scan result never automatically corrects, and every actual
// Chat/Context/Document action reads that stored path, not this scan. See
// reconcile_relocated_projects below, which this calls before returning - the
// returned summaries themselves are unchanged by it (they already reflect real
// disk state).
std::vector<persistence::ProjectSummary> discover_projects();

// The same real discovery logic as discover_projects(), but with the
// resolved access scope passed in explicitly instead of resolved from real
// platform/Phase 02 state - the seam that lets both the Broad and Limited
// scanning paths be exercised and proven directly in tests, without a real
// OS-level broad-access grant needing to exist in the test environment.
// Production code should call discover_projects().
std::vector<persistence::ProjectSummary> discover_projects_for_scope(const platform::ProjectDiscoveryScope &scope);

// Repairs matters.json's stored directory_path for a Matter this workspace
// already knows about (present in list()) that `discovered` found sitting at
// a different real, unambiguous location - the actual external "I moved/
// renamed my Project folder" case. Reuses assign_folder(), so a repair is
// exactly as safe/real as a manual folder re-selection would be. Never acts
// on an id list() does not already know (never auto-imports an unfamiliar
// external Project into this workspace) and never acts on a location
// ProjectSummary::has_duplicate_location flags (a genuine copy/shared-drive
// duplicate is a human decision, never guessed). Returns the ids actually
// repaired, for tests/diagnostics; production callers can ignore it.
std::vector<std::string> reconcile_relocated_projects(const std::vector<persistence::ProjectSummary> &discovered);

} // namespace singularity::matter
