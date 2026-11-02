#pragma once

#include "Matter/Types/Matter.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents::registry {

// THE DATA ROOM'S JOURNAL is written here, and only here: every transition this
// registry records on a document - it appeared, its content changed, it was
// renamed or moved, it went away, it came back - also appends one
// documents::DocumentEvent to matter.document_events, at that moment, with that
// moment's time (Document/Types/DocumentEvent.hpp).
//
// A change Singularity makes itself reaches the registry the same way any other
// does - the file changed on disk - so whoever writes the file says so first:
// while a ChangeActorScope lives on the calling thread, the events recorded on
// it carry that actor and operation.
class ChangeActorScope final {
public:
	ChangeActorScope(std::string actor, std::string operation_id);
	~ChangeActorScope();
	ChangeActorScope(const ChangeActorScope &) = delete;
	ChangeActorScope &operator=(const ChangeActorScope &) = delete;

private:
	std::string previous_actor_;
	std::string previous_operation_id_;
	std::string previous_actor_id_;
	std::string previous_device_id_;
};

struct ReconcileOutcome {
	bool ok = true;
	std::string degraded_reason;
	bool changed = false;

	// Every real physical folder's Project-relative path (POSIX-style
	// separators), including one holding no files at all. This is Matter's
	// own transient read-model projection of physical hierarchy
	// (matters::Matter::folder_paths carries it onward - see that field's own
	// comment for why folders are a recomputed projection, never a persisted/
	// versioned object): Sec.12 of phase-08-documents-filesystem-states.md
	// requires the real folder tree, including empty folders, not a hierarchy
	// merely inferred from where documents happen to live.
	std::vector<std::string> folder_paths;
};

// A single real (or, for reconcile_observed_files()'s own tests, synthetic)
// regular file observed during a Project folder scan. content_hash is
// always populated whenever an ObservedFile is constructed at all - Sec.6
// requires a real cryptographic hash of current bytes as the sole content-
// change authority; size_bytes/last_modified stay informational display
// metadata, never a gate on whether that hash is trusted. Deciding WHICH
// files get turned into an ObservedFile at all (every file on a full scan;
// only watcher-affected paths on a targeted reconcile) is what actually
// bounds hash work in the steady state - see reconcile_affected_paths().
//
// creation_time_token strengthens filesystem identity beyond device_id/
// file_id alone (Sec.5, correction item 3): an OS can reuse a freed inode
// number for a later, unrelated file, so device+inode matching a prior
// Document is not by itself proof of continuity. See
// matters::FilesystemIdentityHint's own comment for what this token is on
// each platform.
struct ObservedFile {
	std::string relative_path;
	std::string display_name;
	std::optional<std::uint64_t> device_id;
	std::optional<std::uint64_t> file_id;
	std::optional<std::string> creation_time_token;
	long long size_bytes = 0;
	std::string last_modified;
	std::string content_hash;
	// The file exists but its content could not be read (permissions, a cloud
	// placeholder not downloaded, a lock): content_hash is empty and the file
	// is never compared by content or offered to AI until it can be read.
	bool content_unreadable = false;
};

// The platform-independent reconciliation core: matches `files` against
// matter's existing Referenced documents (by filesystem identity - device_id
// + file_id + creation_time_token together, when all three are present; see
// the function body for the conservative path+content fallback used when
// they are not - Sec.5, "absolute path alone is not identity" - applies even
// to that fallback) and updates matter.documents in place. Never touches
// disk itself, so tests can exercise identity/fallback-matching semantics
// deterministically - including inode reuse and the "no native stable
// identifier available" branch - regardless of what platform or real
// filesystem behavior the test itself runs under. detect_filesystem_changes()
// below is the real-disk, full-scan wrapper; reconcile_affected_paths()
// further below is the real-disk, targeted-scan wrapper production code
// actually calls in steady state.
[[nodiscard]] ReconcileOutcome reconcile_observed_files(matters::Matter& matter, const std::vector<ObservedFile>& files);

// Phase A, full scan (fast, but O(every real file's bytes) - see
// reconcile_affected_paths() below for the steady-state alternative that
// does not pay this cost): stats and hashes every real file under
// matter.directory_path (skips .lm/.lm.migrating-*/dot-prefixed entries and
// never follows symlinks - mirrors the same walk Persistence/ProjectStore/
// ProjectStore.cpp already uses for Project discovery), collects every real
// folder's relative path (ReconcileOutcome::folder_paths, empty ones
// included), and reconciles the observed files via
// reconcile_observed_files() above. Never runs extraction - a document left
// Processing or Changed by this call has not had process_pending_documents()
// run over it yet.
//
// This is the startup/restart audit path (Matter/MatterActions/
// MatterActions.cpp's list_documents(): the first call in a session for a
// given Matter) - it establishes/re-verifies ground truth and catches up on
// anything that changed while nothing was watching, per Sec.7/18's
// requirement that correctness never depend on watcher history. It is
// deliberately NOT what runs on every steady-state matter.listDocuments call
// once a watcher is already active for this Matter.
//
// Leaves matter.documents entirely untouched (ok == false) when the Project
// folder itself cannot currently be listed at all (permission lost,
// unmounted, ...): phase-08-documents-filesystem-states.md Sec.8 requires a
// temporary access failure never read as "the Project is empty." A Matter
// with no directory_path yet is not an error (ok == true, changed == false)
// - it simply has nothing to scan.
[[nodiscard]] ReconcileOutcome detect_filesystem_changes(matters::Matter& matter);

// detect_filesystem_changes() in two halves, so that the slow one - every
// file of the folder stat'd and hashed - runs without the persistence lock
// (MatterActions' first look at a Matter): a large data room, a network share
// or a cloud drive still downloading holds up only its own Matter, never every
// other Matter, chat and agent while it is read.
struct FolderObservation {
	// Whether the folder itself could be listed (ReconcileOutcome::ok).
	bool listed = false;
	std::vector<ObservedFile> files;
	std::vector<std::string> folder_paths;
};

// The first half: reads matter.directory_path. Touches no record.
[[nodiscard]] FolderObservation observe_folder(const matters::Matter& matter);

// The second half, on the record as it now stands. `read_against` is the
// record the folder was read against: a document whose record changed since -
// written, added, moved or let go by Singularity while the folder was being
// read - is newer than what the observation saw of its file, so that file is
// read again now, before anything is reconciled.
[[nodiscard]] ReconcileOutcome reconcile_folder_observation(matters::Matter& matter,
		FolderObservation observation, const matters::Matter& read_against);

// Phase A, targeted (the steady-state path): re-derives folder_paths cheaply
// (a directory-only walk - collect_real_folder_paths() below - never reads
// file content) and, for each `affected_absolute_paths` entry the filesystem
// watcher actually reported, stats/hashes ONLY that one file (or, if it no
// longer exists as a regular file, marks whatever Document was previously at
// that relative path Unavailable). A path outside matter.directory_path, or
// one that resolves through a symlink, is silently ignored - the same
// boundary/symlink safety detect_filesystem_changes()'s full scan already
// guarantees (Sec.19). Every real Document not named by an affected path is
// left completely untouched: this never re-hashes the rest of the Project,
// which is exactly the cost this function exists to avoid paying on every
// watcher event/matter.listDocuments call.
//
// Correction round 3, item 2: because this is the live, continuously-
// watched path, a file whose native identity does not match any existing
// Document but whose relative_path was, immediately before this call, held
// by a still-AVAILABLE Referenced Document is treated as that same Document
// continuing at a new version - not a new Document - covering a real atomic
// write-temp-then-rename-over-original "safe save" edit, which genuinely
// produces a new device/inode/creation-time at the same location. See
// DocumentRegistry.cpp's reconcile_one_observed_file() for the exact rule
// and its own comment for why this is deliberately NOT how the full/offline
// scan above matches (there, the same shape is honestly ambiguous instead).
[[nodiscard]] ReconcileOutcome reconcile_affected_paths(
		matters::Matter& matter, const std::vector<std::string>& affected_absolute_paths);

// A directory-only walk under `root` (skips .lm/.lm.migrating-*/dot-prefixed
// entries and symlinked directories, exactly like the full scan) - no file is
// ever stat()'d or hashed. Cheap regardless of how much file content the
// Project holds, so both detect_filesystem_changes() and
// reconcile_affected_paths() can afford to refresh folder_paths on every
// call. Returns false only when `root` itself cannot currently be listed.
[[nodiscard]] bool collect_real_folder_paths(const std::filesystem::path& root, std::vector<std::string>& folder_paths);

// Phase 28 GAP2 closure: a Ready document's lifecycle_state is a claim that
// its SOURCE FILE has not changed since the last successful extraction -
// reconcile_observed_files()'s only trigger for Changed (a fresh content
// hash comparison). It is NOT, by itself, proof the derivative cache
// backing that Ready claim still exists: DocumentCache is a separate,
// independently-deletable, app-wide store (its own header's "provably
// reconstructable" contract), and a real controlled test
// (Matter/MatterActions/MatterActions.test.cpp) proved a Ready document
// survives a real cache wipe still claiming Ready - the cache is gone, but
// nothing re-checks it until the source file itself changes, so any real
// consumer of that "AI-ready" content (DocumentSourceResolver.cpp's own
// cache.load()) silently starts reporting Unavailable at the moment it is
// actually needed, with no visible signal anything is wrong. This closes
// that gap with a cheap existence check (DocumentCache::holds(), never a
// re-hash of file content) over every currently-Ready Referenced document,
// demoting any whose cache entry is genuinely missing back to Changed so
// the very next process_pending_documents() call transparently rebuilds it
// through its own already-correct cache-miss path. Deliberately scoped to
// the full-scan audit path only (detect_project_documents(), MatterActions.
// cpp) - never the steady-state watcher-driven path - so this real disk
// read per Ready document stays a startup/restart-audit cost, not an
// ongoing per-listing one (the exact steady-state cost concern Sec.7/18 and
// correction item 2 already protect elsewhere in this file). The same audit
// re-opens a document the reader found nothing in (a scan, a file with no
// text) under an earlier version of the reading back to Processing, so a
// reading that improved - a PDF with scanned pages is now read with the text
// it has - reaches it without the lawyer asking. Returns true if anything
// changed.
[[nodiscard]] bool verify_cached_derivatives_exist(
		const std::filesystem::path& workspace_path, matters::Matter& matter);

// Phase B: runs the real local extraction pipeline (DocumentCache /
// DocumentExtractor - the same pipeline Managed documents already use via
// MatterRepository::refresh_document_derivatives()) over every Referenced
// document left Processing or Changed by either phase-A function above,
// resolving it to Ready / NeedsAttention / Unsupported. Already bounded to
// only pending documents regardless of whether phase A was a full or
// targeted scan - nothing further needed here for the steady-state cost
// concern. Every persisted relative_path is re-validated against
// matter.directory_path with is_relative_path_safe() below before it is ever
// turned into a filesystem path to read - Sec.19. Returns true if any
// document's derived state changed.
bool process_pending_documents(const std::filesystem::path& workspace_path, matters::Matter& matter);

// Phase 09's "Try again": re-opens exactly one Referenced, available
// document currently stuck at NeedsAttention back into Processing, so the
// next process_pending_documents() call genuinely re-runs the real
// extraction pipeline over it instead of reusing DocumentCache's cached
// failure forever. This is the one legitimate exception to "never manually
// mutate MatterDocument state": there is no filesystem change to reconcile
// for a manual retry request, only a real re-invocation of the same pipeline
// - it can honestly reproduce the same failure (e.g. still needs OCR) or
// recover from a transient one. Returns false (no-op) for a document that is
// not Referenced, not available, or not currently NeedsAttention.
[[nodiscard]] bool request_reprocessing(matters::Matter& matter, const std::string& document_id);

// detect_filesystem_changes() immediately followed by
// process_pending_documents() when detection could inspect the folder. Used
// by tests and by the one-time startup/restart audit; steady-state
// production code composes reconcile_affected_paths() + process_pending_documents()
// instead - see Matter/MatterActions/MatterActions.cpp's detect_project_documents()/
// process_project_documents()/reconcile_and_persist_documents().
[[nodiscard]] ReconcileOutcome reconcile(const std::filesystem::path& workspace_path, matters::Matter& matter);

// True when `relative_path`, resolved against `project_root`, stays strictly
// within it. A persisted Document's relative_path must never be trusted to
// build a filesystem path without this check first: `.lm` state can in
// principle be hand-edited, corrupted, or (once shared-Project sync exists)
// written by another device, and a malformed/malicious relative_path must
// never let Singularity read, hash or extract content from outside the Project
// folder - Sec.19 (Security and path safety) of
// phase-08-documents-filesystem-states.md.
[[nodiscard]] bool is_relative_path_safe(const std::filesystem::path& project_root, const std::string& relative_path);

// Test-only instrumentation (correction item 2): counts real
// platform::sha256_hex_digest_of_file() calls DocumentRegistry.cpp makes -
// not a general performance framework, just the one number needed to prove
// the steady-state path does not rehash a whole Project. Never read by
// production code.
void reset_hash_call_count_for_testing() noexcept;
[[nodiscard]] long long hash_call_count_for_testing() noexcept;
// Test-only: called on the hashing thread with each file's path just before
// DocumentRegistry.cpp hashes it. Never set by production code.
void set_hash_observer_for_testing(std::function<void(const std::filesystem::path&)> observer);

} // namespace singularity::documents::registry
