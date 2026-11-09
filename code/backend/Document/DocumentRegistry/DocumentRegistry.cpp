#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include "Document/DocumentCache/DocumentCache.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/FileIdentity/FileIdentity.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace singularity::documents::registry {
namespace {

std::atomic<long long> hash_call_count{0};

// The one place DocumentRegistry.cpp ever computes a content hash - routing
// every call through here (never platform::sha256_hex_digest_of_file()
// directly, elsewhere in this file) is what makes
// hash_call_count_for_testing() a true count of real hash work, not an
// approximation.
std::mutex hash_observer_mutex;
std::function<void(const std::filesystem::path&)> hash_observer;

std::string hash_file_and_count(const std::filesystem::path& path) {
	hash_call_count.fetch_add(1, std::memory_order_relaxed);
	std::function<void(const std::filesystem::path&)> observer;
	{
		const std::lock_guard<std::mutex> lock(hash_observer_mutex);
		observer = hash_observer;
	}
	if (observer) observer(path);
	return platform::sha256_hex_digest_of_file(path);
}

constexpr const char* kUnreadableFileDetail =
		"Singularity can't read this file right now. Check that it isn't locked, still downloading or restricted by its "
		"permissions.";

// Reads what identifies a file's content. One file that cannot be read never
// aborts reconciliation of the whole Project: it is observed as unreadable.
void observe_file_content(const std::filesystem::path& path, ObservedFile& file, bool hash_content) {
	try {
		file.last_modified = file_last_modified_token(path);
		if (hash_content == true) file.content_hash = hash_file_and_count(path);
	} catch (const std::exception&) {
		file.content_hash.clear();
		file.content_unreadable = true;
	}
}

// A Document whose file cannot be read keeps its identity and last known
// content, and is marked for attention so no request uses it meanwhile.
bool mark_content_unreadable(matters::MatterDocument& document) {
	if (document.lifecycle_state == matters::DocumentLifecycleState::NeedsAttention
			&& document.attention_detail == std::optional<std::string>(kUnreadableFileDetail))
		return false;
	document.lifecycle_state = matters::DocumentLifecycleState::NeedsAttention;
	document.attention_detail = kUnreadableFileDetail;
	return true;
}

bool is_unreadable_marker(const matters::MatterDocument& document) {
	return document.lifecycle_state == matters::DocumentLifecycleState::NeedsAttention
		&& document.attention_detail == std::optional<std::string>(kUnreadableFileDetail);
}

// A Document that leaves the Project folder takes its reading limitation with
// it. The sentence about pages that were not read ("Pages 14-15 are images and
// were not read.") describes the reading of a file that is here, and whatever
// asks why a gone Document cannot be used must meet only that it is gone, never
// that sentence as its reason. The Document is read again when its file comes
// back: the cache still holds the reading of those bytes, and the sentence
// returns with it.
void set_aside_reading_limitation(matters::MatterDocument& document) {
	if (document.lifecycle_state != matters::DocumentLifecycleState::Ready) return;
	if (document.attention_detail.has_value() == false) return;
	document.attention_detail.reset();
	document.lifecycle_state = matters::DocumentLifecycleState::Processing;
}

bool is_hidden_or_technical(const std::string& filename) {
	if (filename.empty() == true) return false;
	if (filename == ".lm") return true;
	if (filename.rfind(".lm.migrating-", 0) == 0) return true;
	// Real product bug (caught live): Microsoft Word/Excel/PowerPoint's own
	// universal lock-file convention ("~$<original filename>", created the
	// instant a real Office document is opened for editing) was being
	// scanned in as a real document candidate - correctly marked unreadable
	// (it is opaque binary, never real content), but the record survived
	// even after the file itself was later deleted (Office cleans it up on
	// close) because a vanished path is truthfully marked unavailable, by
	// design, never silently forgotten (see reconcile_full_scan()'s own
	// mark_path_unavailable() comment). That permanent "unavailable" ghost
	// entry then collided with instruction_names_document()'s substring
	// match against the REAL document's own similar name, failing a real,
	// legitimate request closed with "A named document is not currently
	// available" even though the actual intended file was present and
	// ready the whole time. A "~$"-prefixed file is never real user
	// content on any platform Office runs on - it must never become a
	// document candidate in the first place.
	if (filename.rfind("~$", 0) == 0) return true;
	// A staged or crash-leftover write file of Singularity's own is never a document.
	if (platform::is_staged_file_name(filename)) return true;
	return filename.front() == '.';
}

// The OS's own identity of a file (Platform/FileIdentity): device and file id,
// and its creation time where the OS keeps one. A platform or file system
// that supplies none leaves them unset rather than fabricating one - see
// reconcile_observed_files()'s no-identity fallback for what that means for
// continuity. Returns false only when the file itself could not be inspected
// (permission race, vanished mid-scan); the caller skips just that one file
// rather than aborting the whole walk.
using FileStat = platform::FileIdentity;

bool stat_file(const std::filesystem::path& path, FileStat& result) {
	return platform::read_file_identity(path, result);
}

// True when `path` is a symlink, or is itself named/nested under a hidden or
// `.lm`-technical entry - the same exclusion collect_real_entries()'s own
// directory walk applies structurally, reused here for reconcile_affected_paths()
// where a watcher-reported path arrives already-flattened (no walk to skip
// recursion during) and must be checked component-by-component instead.
bool is_symlink_or_hidden_within(const std::filesystem::path& root, const std::filesystem::path& relative_path) {
	std::error_code symlinkError;
	if (std::filesystem::is_symlink(root / relative_path, symlinkError) == true) return true;
	for (const std::filesystem::path& component : relative_path) {
		if (is_hidden_or_technical(component.string()) == true) return true;
	}
	return false;
}

// Never follows a symlink: a symlinked file's target - or a symlinked
// directory's contents - could sit entirely outside the Project folder, and
// std::filesystem::directory_entry::is_regular_file/is_directory follow
// symlinks by default.
//
// Shared walk core for collect_real_folder_paths()/collect_real_entries()
// below: visits every real, non-symlink, non-hidden/technical entry under
// `root` - the same discovery convention (skip .lm/.lm.migrating-*/dot-
// prefixed entries, one unreadable subdirectory skipped rather than
// aborting) already established by Persistence/ProjectStore/ProjectStore.cpp's
// own Project discovery walk. `visit_file` is only invoked for regular
// files - never called at all when `hash_files` is false, so a folder-only
// caller pays no stat()/hash cost for files whatsoever. Returns false only
// when `root` itself cannot currently be listed at all.
bool walk_real_entries(const std::filesystem::path& root, bool hash_files, std::vector<ObservedFile>* files,
		std::vector<std::string>& folder_paths) {
	std::error_code rootError;
	if (std::filesystem::is_directory(root, rootError) == false || rootError) return false;

	// Probed once, deliberately WITHOUT skip_permission_denied: that flag only
	// means "keep walking past a permission failure found while descending
	// into a subdirectory," matching ProjectStore's own "one unreadable
	// subdirectory is skipped, not the whole scan" convention below. This bare
	// probe on root surfaces that failure as a real error_code instead.
	std::error_code rootListError;
	const std::filesystem::directory_iterator rootProbe(root, rootListError);
	if (rootListError) return false;

	std::error_code iteratorError;
	std::filesystem::recursive_directory_iterator iterator(
			root, std::filesystem::directory_options::skip_permission_denied, iteratorError);
	if (iteratorError) return false;
	const std::filesystem::recursive_directory_iterator end;

	for (; iterator != end; iterator.increment(iteratorError)) {
		if (iteratorError) return false;
		const std::filesystem::directory_entry entry = *iterator;
		const std::string filename = entry.path().filename().string();
		if (is_hidden_or_technical(filename) == true) {
			iterator.disable_recursion_pending();
			continue;
		}

		std::error_code symlinkError;
		if (std::filesystem::is_symlink(entry.path(), symlinkError) == true) {
			iterator.disable_recursion_pending();
			continue;
		}

		std::error_code typeError;
		if (entry.is_directory(typeError) == true) {
			if (typeError) continue;
			folder_paths.push_back(std::filesystem::relative(entry.path(), root).generic_string());
			continue;
		}
		if (files == nullptr) continue;
		if (entry.is_regular_file(typeError) == false || typeError) continue;

		FileStat stat_result;
		if (stat_file(entry.path(), stat_result) == false) continue;

		ObservedFile file;
		file.relative_path = std::filesystem::relative(entry.path(), root).generic_string();
		file.display_name = entry.path().filename().string();
		file.device_id = stat_result.device_id;
		file.file_id = stat_result.file_id;
		file.creation_time_token = stat_result.creation_time_token;
		file.size_bytes = stat_result.size_bytes;
		observe_file_content(entry.path(), file, hash_files);
		files->push_back(std::move(file));
	}
	return true;
}

bool collect_real_entries(
		const std::filesystem::path& root, std::vector<ObservedFile>& files, std::vector<std::string>& folder_paths) {
	return walk_real_entries(root, true, &files, folder_paths);
}

// ---- The journal --------------------------------------------------------------------

thread_local std::string journal_actor;
thread_local std::string journal_operation_id;
thread_local std::string journal_actor_id;
thread_local std::string journal_device_id;

// What a document was before this reconcile touched it: what its events are
// told apart by.
struct DocumentBefore {
	std::string name;
	std::string relative_path;
	std::string fingerprint;
	int version = 0;
	long long size_bytes = 0;
	bool available = true;
};

DocumentBefore before_of(const matters::MatterDocument& document) {
	return DocumentBefore{document.name, document.relative_path, document.fingerprint, document.version,
		document.size_bytes, document.available};
}

void journal(matters::Matter& matter, DocumentEventKind kind, const matters::MatterDocument& document,
		const DocumentBefore* before) {
	DocumentEvent event;
	event.id = matters::generate_document_event_id();
	event.document_id = document.id;
	event.kind = kind;
	event.at = workspaces::current_timestamp();
	event.file_modified_at = document.last_modified;
	event.name = document.name;
	event.relative_path = document.relative_path;
	event.version = document.version;
	event.fingerprint = document.fingerprint;
	event.size_bytes = document.size_bytes;
	if (before != nullptr && kind == DocumentEventKind::Moved) {
		event.previous_name = before->name;
		event.previous_relative_path = before->relative_path;
	}
	if (before != nullptr && kind == DocumentEventKind::Changed) {
		event.previous_version = before->version;
		event.previous_fingerprint = before->fingerprint;
		event.previous_size_bytes = before->size_bytes;
	}
	event.actor = journal_actor;
	event.operation_id = journal_operation_id;
	if (!journal_actor.empty()) {
		event.actor_id = journal_actor_id;
		event.device_id = journal_device_id;
	}
	matter.document_events.push_back(std::move(event));
	if (matter.document_events.size() > kMaximumDocumentEvents) {
		matter.document_events.erase(matter.document_events.begin(),
			matter.document_events.begin() + static_cast<std::ptrdiff_t>(matter.document_events.size() - kMaximumDocumentEvents));
	}
}

// What one reconcile did to a document it already knew, read off what the
// document was before and is now. A document that came back, moved and changed
// at once is all three.
void journal_transitions(matters::Matter& matter, const DocumentBefore& before, const matters::MatterDocument& after) {
	if (before.available == false && after.available == true) journal(matter, DocumentEventKind::Returned, after, &before);
	if (before.relative_path != after.relative_path && after.relative_path.empty() == false) {
		journal(matter, DocumentEventKind::Moved, after, &before);
	}
	if (before.fingerprint != after.fingerprint) journal(matter, DocumentEventKind::Changed, after, &before);
}

// A new file's document id, the same on every computer that records it. Two
// computers sharing a Matter's folder notice a new file within the same
// second, and each records it before it has read the other's record: a random
// id each left the Matter two documents for one file, one of them soon marked
// gone. The id is read off the Matter and the file's place in it, so the two
// records are one object and the Matter's store keeps one. Where a document
// already holds that id (one that was at that place before and left it), the
// next id of the same sequence is taken, which a computer that knows the same
// documents takes too.
std::string new_document_id(const matters::Matter& matter, const std::vector<matters::MatterDocument>& pending,
		const std::string& relative_path) {
	const auto isTaken = [&](const std::string& id) {
		const auto holds = [&](const matters::MatterDocument& document) { return document.id == id; };
		return std::any_of(matter.documents.begin(), matter.documents.end(), holds)
			|| std::any_of(pending.begin(), pending.end(), holds);
	};
	for (std::size_t sequence = 0;; ++sequence) {
		std::string seed = matter.id + "\n" + relative_path;
		if (sequence > 0) seed += "\n" + std::to_string(sequence);
		const std::string id = "doc_" + platform::sha256_hex_digest(seed).substr(0, 16);
		if (isTaken(id) == false) return id;
	}
}

matters::MatterDocument make_new_document(const matters::Matter& matter, const std::vector<matters::MatterDocument>& pending,
		const ObservedFile& file) {
	matters::MatterDocument document;
	document.id = new_document_id(matter, pending, file.relative_path);
	document.matter_id = matter.id;
	document.name = file.display_name;
	document.file_type = detect_document_format(document.name);
	document.source_mode = matters::DocumentSourceMode::Referenced;
	document.added_at = workspaces::current_timestamp();
	document.relative_path = file.relative_path;
	document.size_bytes = file.size_bytes;
	document.last_modified = file.last_modified;
	document.fingerprint = file.content_hash;
	document.version = 1;
	document.available = true;
	document.filesystem_identity.device_id = file.device_id;
	document.filesystem_identity.file_id = file.file_id;
	document.filesystem_identity.creation_time_token = file.creation_time_token;
	document.lifecycle_state = matters::DocumentLifecycleState::Processing;
	return document;
}

std::size_t count_utf8_characters(const std::string& text) {
	std::size_t count = 0;
	for (const char byte : text) {
		if ((static_cast<unsigned char>(byte) & 0xc0U) != 0x80U) ++count;
	}
	return count;
}

// The full identity match key: device+inode alone is not proof of
// continuity, since an OS can reuse a freed inode number for a later,
// unrelated file (Sec.5, correction item 3) - creation_time_token
// disambiguates that case whenever the platform provides one. A platform
// with no such field leaves every entry's token as the same empty string,
// which degrades this back to plain device+inode matching, unchanged from
// before.
using IdentityKey = std::tuple<std::uint64_t, std::uint64_t, std::string>;

IdentityKey identity_key(std::uint64_t device_id, std::uint64_t file_id, const std::optional<std::string>& creation_time_token) {
	return {device_id, file_id, creation_time_token.value_or("")};
}

// A Document recorded before its platform gave a creation time (Linux until
// 2026-10-23) has none to compare. A file that now brings one is matched to it
// by device and file id alone, as it was matched then, and its creation time
// is recorded from that match on; nothing else is matched this way.
using UntimedIdentityKey = std::pair<std::uint64_t, std::uint64_t>;

matters::MatterDocument* untimed_identity_match(
		const std::map<UntimedIdentityKey, matters::MatterDocument*>& untimed, const ObservedFile& file) {
	if (!file.device_id || !file.file_id || !file.creation_time_token) return nullptr;
	const auto found = untimed.find({*file.device_id, *file.file_id});
	return found == untimed.end() ? nullptr : found->second;
}

// Whether a document's record describes this very file: the same volume and
// file id. The firm's computers each hold their own copy of a shared Matter's
// file, so a record another computer wrote names that computer's copy - its
// modification time and size as that computer saw them - and they are no change
// here. Counting them made two computers rewrite each other's record without
// end: 300 records in four minutes. The content's hash alone says whether the
// document changed.
bool is_same_physical_file(const matters::MatterDocument& document, const ObservedFile& file) {
	return document.filesystem_identity.device_id.has_value() && document.filesystem_identity.file_id.has_value()
		&& file.device_id.has_value() && file.file_id.has_value()
		&& *document.filesystem_identity.device_id == *file.device_id && *document.filesystem_identity.file_id == *file.file_id;
}

// Reconciles exactly one observed file against matter.documents - the
// single-file core reconcile_affected_paths() (targeted, steady-state) uses;
// reconcile_observed_files() (full scan) below has its own, deliberately
// separate matching loop - see that function's own comment for why it does
// NOT call this helper. Rebuilds its own by_identity/by_path index from
// matter.documents on every call: deliberately not shared/cached across
// calls in the same reconcile pass, so a just-created Document's push_back
// (which can reallocate matter.documents) never leaves a caller holding a
// stale pointer. This makes each call O(document count) rather than O(1),
// which is exactly right for reconcile_affected_paths()'s handful of
// watcher-reported paths (document count, never file content, drives the
// cost).
//
// `allow_path_continuity_when_available` (Phase 08 correction round 3, item
// 2): when true, a file whose native identity does not match ANY existing
// Document (a real device/inode/creation-time triple this platform provides,
// but a genuinely different one - see stat_file()'s own comment) is still
// treated as the SAME Document as whatever Referenced Document is currently
// AVAILABLE at that exact relative_path, rather than automatically becoming
// a brand-new Document. This is reached only from reconcile_affected_paths(),
// i.e. only when a live, continuously-watching filesystem watcher reported
// this exact path as changed with no observed gap in between (matter.documents
// never independently transitioned that path to unavailable first) - the
// real-world cause overwhelmingly matching this shape is an atomic
// write-temp-file-then-rename-over-original "safe save" (the pattern most
// document editors use), which genuinely produces a new physical file object
// at the same location while remaining, product-wise, the same logical
// Document at a new version. reconcile_observed_files() (the full, offline/
// restart-time scan) applies the same rule to the same shape - see its Pass 3:
// a macOS safe save (NSFileManager replaceItemAt), a temp-file-and-rename and
// an atomic write all give the file a new inode at the same path, and an editor
// saving while Singularity is closed must not cost the lawyer the document.
//
// This is a deliberate, disclosed tradeoff, not invented certainty: the
// same shape (identity changed, path unchanged, old Document still marked
// available, observed within one live watcher notification) is also what a
// coincidental inode-reuse-by-an-unrelated-file landing at the very same
// path within the same coalescing window would look like, and the two are
// not distinguishable from filesystem facts alone in that specific
// interleaving. This rule resolves that genuine ambiguity in favor of the
// realistic common case (atomic save must not fragment Document identity)
// rather than the pathological rare one - see the Phase 08 correction
// report for this as an explicitly acknowledged limitation.
bool reconcile_one_observed_file(matters::Matter& matter, const ObservedFile& file, bool allow_path_continuity_when_available) {
	std::map<IdentityKey, matters::MatterDocument*> by_identity;
	std::map<UntimedIdentityKey, matters::MatterDocument*> by_untimed_identity;
	std::map<std::string, matters::MatterDocument*> by_path;
	for (matters::MatterDocument& document : matter.documents) {
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		by_path[document.relative_path] = &document;
		if (document.filesystem_identity.device_id && document.filesystem_identity.file_id) {
			by_identity[identity_key(*document.filesystem_identity.device_id, *document.filesystem_identity.file_id,
					document.filesystem_identity.creation_time_token)] = &document;
			if (!document.filesystem_identity.creation_time_token)
				by_untimed_identity[{*document.filesystem_identity.device_id, *document.filesystem_identity.file_id}] = &document;
		}
	}

	matters::MatterDocument* existing = nullptr;
	const bool fileHasIdentity = file.device_id.has_value() && file.file_id.has_value();
	if (fileHasIdentity == true) {
		const auto identityIterator = by_identity.find(identity_key(*file.device_id, *file.file_id, file.creation_time_token));
		if (identityIterator != by_identity.end()) existing = identityIterator->second;
		else existing = untimed_identity_match(by_untimed_identity, file);
	}
	if (existing == nullptr) {
		// Identity recorded on another device or volume (a second machine on a
		// shared Project, a remounted drive) cannot be compared: the same bytes
		// at the same Project-relative path are the same Document.
		const auto pathIterator = by_path.find(file.relative_path);
		if (pathIterator != by_path.end()
				&& (pathIterator->second->fingerprint == file.content_hash || file.content_unreadable)
				&& (fileHasIdentity == false || !pathIterator->second->filesystem_identity.device_id
					|| pathIterator->second->filesystem_identity.device_id != file.device_id)) {
			existing = pathIterator->second;
		}
	}

	// Atomic-save/replace-in-place continuity - see this function's own
	// comment above for the exact reasoning and its deliberate scope.
	if (existing == nullptr && allow_path_continuity_when_available == true) {
		const auto pathIterator = by_path.find(file.relative_path);
		if (pathIterator != by_path.end() && pathIterator->second->available == true) {
			existing = pathIterator->second;
		}
	}

	if (existing == nullptr) {
		const auto supersededIterator = by_path.find(file.relative_path);
		if (supersededIterator != by_path.end()) supersededIterator->second->relative_path.clear();
		matters::MatterDocument created = make_new_document(matter, {}, file);
		matter.documents.push_back(std::move(created));
		if (file.content_unreadable) mark_content_unreadable(matter.documents.back());
		journal(matter, DocumentEventKind::Added, matter.documents.back(), nullptr);
		return true;
	}

	const DocumentBefore before = before_of(*existing);
	bool entryChanged = false;
	if (existing->relative_path != file.relative_path) {
		existing->relative_path = file.relative_path;
		existing->name = file.display_name;
		entryChanged = true;
	}
	const bool isSameFile = is_same_physical_file(*existing, file);
	existing->filesystem_identity.device_id = file.device_id;
	existing->filesystem_identity.file_id = file.file_id;
	existing->filesystem_identity.creation_time_token = file.creation_time_token;

	if (existing->available == false) {
		existing->available = true;
		existing->unavailable_since.clear();
		entryChanged = true;
	}

	const bool metadataChanged = isSameFile == true
		&& (existing->size_bytes != file.size_bytes || existing->last_modified != file.last_modified);
	existing->size_bytes = file.size_bytes;
	if (file.content_unreadable) {
		if (mark_content_unreadable(*existing)) entryChanged = true;
	} else {
		existing->last_modified = file.last_modified;
		if (file.content_hash != existing->fingerprint) {
			existing->fingerprint = file.content_hash;
			existing->version += 1;
			existing->lifecycle_state = matters::DocumentLifecycleState::Changed;
			existing->attention_detail.reset();
			entryChanged = true;
		} else if (is_unreadable_marker(*existing)) {
			// Readable again with the content it had: processed again.
			existing->lifecycle_state = matters::DocumentLifecycleState::Changed;
			existing->attention_detail.reset();
			entryChanged = true;
		} else if (metadataChanged == true) {
			entryChanged = true;
		}
	}
	journal_transitions(matter, before, *existing);
	return entryChanged;
}

// The single-path counterpart of reconcile_observed_files()'s "not matched"
// sweep: marks the Referenced Document currently at `relative_path`, if any
// and not already unavailable, Unavailable. Used by reconcile_affected_paths()
// when a targeted reconcile confirms one specific previously-known path no
// longer exists - never sweeps any other Document, unlike the full-scan
// sweep, which is exactly why a targeted reconcile must never be given a
// partial file list and asked to run reconcile_observed_files() directly
// (every Document absent from that partial list would be wrongly swept).
bool mark_path_unavailable(matters::Matter& matter, const std::string& relative_path) {
	for (matters::MatterDocument& document : matter.documents) {
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		if (document.relative_path != relative_path) continue;
		if (document.available == false) return false;
		document.available = false;
		document.unavailable_since = workspaces::current_timestamp();
		set_aside_reading_limitation(document);
		journal(matter, DocumentEventKind::Removed, document, nullptr);
		return true;
	}
	return false;
}

} // namespace

ChangeActorScope::ChangeActorScope(std::string actor, std::string operation_id)
	: previous_actor_(journal_actor), previous_operation_id_(journal_operation_id),
	  previous_actor_id_(journal_actor_id), previous_device_id_(journal_device_id) {
	// Who wrote it is resolved before the journal is: an Singularity write
	// names its person and computer (Platform/ActorIdentity).
	if (!actor.empty()) {
		try {
			const platform::ActorIdentity identity = platform::resolve_actor_identity();
			journal_actor_id = identity.actor_id;
			journal_device_id = identity.device_id;
		} catch (const std::exception &) {
			journal_actor_id.clear();
			journal_device_id.clear();
		}
	}
	journal_actor = std::move(actor);
	journal_operation_id = std::move(operation_id);
}

ChangeActorScope::~ChangeActorScope() {
	journal_actor = previous_actor_;
	journal_operation_id = previous_operation_id_;
	journal_actor_id = previous_actor_id_;
	journal_device_id = previous_device_id_;
}

ReconcileOutcome reconcile_observed_files(matters::Matter& matter, const std::vector<ObservedFile>& files) {
	ReconcileOutcome outcome;

	// Index existing Referenced documents by filesystem identity and by their
	// last-known relative path, so a rename/move can be told apart from a
	// delete+create even though both look identical as a plain path diff.
	// Built once here (unlike reconcile_one_observed_file()'s per-call index)
	// because this loop runs once per real file in the whole Project - an
	// O(document count) rebuild inside it would make a full scan
	// O(files x documents) instead of O(files + documents).
	std::map<IdentityKey, matters::MatterDocument*> by_identity;
	std::map<UntimedIdentityKey, matters::MatterDocument*> by_untimed_identity;
	std::map<std::string, matters::MatterDocument*> by_path;
	for (matters::MatterDocument& document : matter.documents) {
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		by_path[document.relative_path] = &document;
		if (document.filesystem_identity.device_id && document.filesystem_identity.file_id) {
			by_identity[identity_key(*document.filesystem_identity.device_id, *document.filesystem_identity.file_id,
					document.filesystem_identity.creation_time_token)] = &document;
			if (!document.filesystem_identity.creation_time_token)
				by_untimed_identity[{*document.filesystem_identity.device_id, *document.filesystem_identity.file_id}] = &document;
		}
	}

	// New documents are collected here rather than pushed into matter.documents
	// directly: push_back() can reallocate the vector's backing storage, which
	// would silently invalidate every MatterDocument* already captured in
	// by_identity/by_path above (and the pointer-arithmetic index_of() below)
	// mid-loop. They are appended to matter.documents only once, after every
	// existing entry has already been matched/updated against a stable buffer.
	std::vector<matters::MatterDocument> newDocuments;

	const std::size_t existingDocumentCount = matter.documents.size();
	std::vector<bool> matched(existingDocumentCount, false);
	const auto index_of = [&](const matters::MatterDocument* pointer) {
		return static_cast<std::size_t>(pointer - matter.documents.data());
	};

	const auto applyMatch = [&](matters::MatterDocument* existing, const ObservedFile& file) {
		matched[index_of(existing)] = true;
		const DocumentBefore before = before_of(*existing);
		bool entryChanged = false;

		if (existing->relative_path != file.relative_path) {
			existing->relative_path = file.relative_path;
			existing->name = file.display_name;
			entryChanged = true;
		}
		const bool isSameFile = is_same_physical_file(*existing, file);
		existing->filesystem_identity.device_id = file.device_id;
		existing->filesystem_identity.file_id = file.file_id;
		existing->filesystem_identity.creation_time_token = file.creation_time_token;

		if (existing->available == false) {
			existing->available = true;
			existing->unavailable_since.clear();
			entryChanged = true;
		}

		// Sec.6: content_hash is always a fresh hash of actual current bytes
		// (collect_real_entries() computes it unconditionally, never gated by a
		// size/mtime equality check) - it is the sole authority for whether this
		// document's content changed. size_bytes/last_modified are updated as
		// informational metadata regardless, but never decide Changed by
		// themselves.
		const bool metadataChanged = isSameFile == true
			&& (existing->size_bytes != file.size_bytes || existing->last_modified != file.last_modified);
		existing->size_bytes = file.size_bytes;
		if (file.content_unreadable) {
			if (mark_content_unreadable(*existing)) entryChanged = true;
		} else {
			existing->last_modified = file.last_modified;
			if (file.content_hash != existing->fingerprint) {
				existing->fingerprint = file.content_hash;
				existing->version += 1;
				existing->lifecycle_state = matters::DocumentLifecycleState::Changed;
				existing->attention_detail.reset();
				entryChanged = true;
			} else if (is_unreadable_marker(*existing)) {
				existing->lifecycle_state = matters::DocumentLifecycleState::Changed;
				existing->attention_detail.reset();
				entryChanged = true;
			} else if (metadataChanged == true) {
				entryChanged = true;
			}
		}

		journal_transitions(matter, before, *existing);
		if (entryChanged == true) outcome.changed = true;
	};

	// Pass 1: native identity. Every file whose (device, inode, creation time)
	// a Document already records is that Document - a rename or move included -
	// before any path is considered, so a Document that moved is never claimed
	// by whatever now sits at its old path.
	std::vector<const ObservedFile*> unmatchedFiles;
	for (const ObservedFile& file : files) {
		const bool fileHasIdentity = file.device_id.has_value() && file.file_id.has_value();
		if (fileHasIdentity == true) {
			const auto identityIterator = by_identity.find(identity_key(*file.device_id, *file.file_id, file.creation_time_token));
			if (identityIterator != by_identity.end() && matched[index_of(identityIterator->second)] == false) {
				applyMatch(identityIterator->second, file);
				continue;
			}
			matters::MatterDocument* untimed = untimed_identity_match(by_untimed_identity, file);
			if (untimed != nullptr && matched[index_of(untimed)] == false) {
				applyMatch(untimed, file);
				continue;
			}
		}
		unmatchedFiles.push_back(&file);
	}

	// Pass 2: the same bytes at the same Project-relative path as a Document no
	// file claimed by identity. Sec.5: path alone is never Document identity,
	// and the content must be exactly the same - a real edit is
	// indistinguishable here from a delete+recreate, so it becomes a new
	// logical Document rather than being merged by path alone. This is what
	// keeps identity when native identifiers cannot be compared: a platform
	// without them, a Project opened on another device, a remounted volume or
	// a copied folder. See DocumentRegistry.test.cpp's fallback tests.
	for (const ObservedFile* observed : unmatchedFiles) {
		const ObservedFile& file = *observed;
		const auto pathIterator = by_path.find(file.relative_path);
		// A file whose content cannot be read is compared by path alone: its
		// bytes cannot prove anything either way, and a duplicate Document for
		// the same unreadable file helps nobody.
		if (pathIterator != by_path.end() && matched[index_of(pathIterator->second)] == false
				&& (pathIterator->second->fingerprint == file.content_hash || file.content_unreadable)) {
			applyMatch(pathIterator->second, file);
			continue;
		}

		// Pass 3: an atomic replacement. The Document last seen AVAILABLE at this
		// exact path, whose own physical file is nowhere in the Project now (Pass 1
		// would have claimed it wherever it went), continues as the file now at
		// its path - the same logical Document at its next version. Every common
		// way of saving a document gives the saved file a new inode at the same
		// path (a macOS safe save, a temp file renamed over the original, an
		// atomic write), and an editor that saves while Singularity is closed must not
		// turn the lawyer's document into a stranger. This is the rule the live
		// watcher already applies (reconcile_one_observed_file()), with the same
		// disclosed ambiguity: an unrelated file put at that path while Singularity
		// was not looking is read as the document's next version - its content
		// change is recorded, and anything prepared against the earlier version
		// is refused as changed. A Document already seen gone keeps that truth: a
		// file appearing at its old path after the gap is a new Document. Only the
		// save's own signature qualifies - both files carry native identity on the
		// same volume, the old one gone and a new one at its path; with no identity
		// to compare there is no evidence of a save, and path alone stays no
		// identity (Sec.5; see the fallback test).
		if (pathIterator != by_path.end() && matched[index_of(pathIterator->second)] == false
				&& pathIterator->second->available == true && file.device_id && file.file_id
				&& pathIterator->second->filesystem_identity.device_id
				&& pathIterator->second->filesystem_identity.file_id
				&& *pathIterator->second->filesystem_identity.device_id == *file.device_id) {
			applyMatch(pathIterator->second, file);
			continue;
		}

		// A different, superseded Document may still claim this exact
		// relative_path (e.g. it was deleted/replaced by a physical file with
		// a different real identity). Two Documents must never appear to occupy
		// the same current location - a lookup by path (this domain's own
		// by_path map above, and the frontend's own folder-hierarchy grouping)
		// must resolve to exactly the one real file actually there. The
		// superseded Document's own identity/version/fingerprint history is
		// untouched; only its now-stale navigational path pointer is cleared,
		// so it stops being found "at" a location a different logical Document
		// now occupies.
		if (pathIterator != by_path.end() && matched[index_of(pathIterator->second)] == false)
			pathIterator->second->relative_path.clear();

		matters::MatterDocument created = make_new_document(matter, newDocuments, file);
		newDocuments.push_back(std::move(created));
		if (file.content_unreadable) mark_content_unreadable(newDocuments.back());
		outcome.changed = true;
	}

	for (std::size_t index = 0; index < existingDocumentCount; ++index) {
		matters::MatterDocument& document = matter.documents[index];
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		if (matched[index] == true) continue;
		if (document.available == false) continue; // already known-unavailable; nothing new to report
		document.available = false;
		document.unavailable_since = workspaces::current_timestamp();
		set_aside_reading_limitation(document);
		journal(matter, DocumentEventKind::Removed, document, nullptr);
		outcome.changed = true;
	}

	for (matters::MatterDocument& newDocument : newDocuments) {
		matter.documents.push_back(std::move(newDocument));
		journal(matter, DocumentEventKind::Added, matter.documents.back(), nullptr);
	}

	return outcome;
}

ReconcileOutcome detect_filesystem_changes(matters::Matter& matter) {
	ReconcileOutcome outcome;
	if (!matter.directory_path || matter.directory_path->empty()) return outcome;

	const std::filesystem::path root(*matter.directory_path);
	std::vector<ObservedFile> files;
	std::vector<std::string> folder_paths;
	if (collect_real_entries(root, files, folder_paths) == false) {
		outcome.ok = false;
		outcome.degraded_reason = "Singularity could not read this Project's folder right now.";
		return outcome;
	}

	outcome = reconcile_observed_files(matter, files);
	outcome.folder_paths = std::move(folder_paths);
	return outcome;
}

FolderObservation observe_folder(const matters::Matter& matter) {
	FolderObservation observation;
	if (!matter.directory_path || matter.directory_path->empty()) return observation;
	observation.listed = collect_real_entries(
		std::filesystem::path(*matter.directory_path), observation.files, observation.folder_paths);
	return observation;
}

ReconcileOutcome reconcile_folder_observation(matters::Matter& matter, FolderObservation observation,
		const matters::Matter& read_against) {
	ReconcileOutcome outcome;
	if (!matter.directory_path || matter.directory_path->empty()) return outcome;
	if (observation.listed == false) {
		outcome.ok = false;
		outcome.degraded_reason = "Singularity could not read this Project's folder right now.";
		return outcome;
	}
	const std::filesystem::path root(*matter.directory_path);
	// The paths of documents whose record moved on while the folder was read:
	// both where the document was and where it is now.
	std::map<std::string, const matters::MatterDocument*> before;
	for (const matters::MatterDocument& document : read_against.documents) before[document.id] = &document;
	std::set<std::string> readAgain;
	for (const matters::MatterDocument& document : matter.documents) {
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		const auto found = before.find(document.id);
		if (found != before.end()) {
			const matters::MatterDocument& was = *found->second;
			before.erase(found);
			if (was.fingerprint == document.fingerprint && was.version == document.version
					&& was.relative_path == document.relative_path && was.available == document.available)
				continue;
			if (!was.relative_path.empty()) readAgain.insert(was.relative_path);
		}
		if (!document.relative_path.empty()) readAgain.insert(document.relative_path);
	}
	// A document the record let go of meanwhile.
	for (const auto& [id, was] : before)
		if (was->source_mode == matters::DocumentSourceMode::Referenced && !was->relative_path.empty())
			readAgain.insert(was->relative_path);
	for (const std::string& relativePath : readAgain) {
		observation.files.erase(std::remove_if(observation.files.begin(), observation.files.end(),
			[&](const ObservedFile& file) { return file.relative_path == relativePath; }), observation.files.end());
		if (is_relative_path_safe(root, relativePath) == false || is_symlink_or_hidden_within(root, relativePath) == true)
			continue;
		const std::filesystem::path absolute = root / relativePath;
		std::error_code typeError;
		if (std::filesystem::is_regular_file(absolute, typeError) == false || typeError) continue;
		FileStat stat_result;
		if (stat_file(absolute, stat_result) == false) continue;
		ObservedFile file;
		file.relative_path = relativePath;
		file.display_name = absolute.filename().string();
		file.device_id = stat_result.device_id;
		file.file_id = stat_result.file_id;
		file.creation_time_token = stat_result.creation_time_token;
		file.size_bytes = stat_result.size_bytes;
		observe_file_content(absolute, file, true);
		observation.files.push_back(std::move(file));
	}
	outcome = reconcile_observed_files(matter, observation.files);
	outcome.folder_paths = std::move(observation.folder_paths);
	return outcome;
}

bool collect_real_folder_paths(const std::filesystem::path& root, std::vector<std::string>& folder_paths) {
	return walk_real_entries(root, false, nullptr, folder_paths);
}

ReconcileOutcome reconcile_affected_paths(matters::Matter& matter, const std::vector<std::string>& affected_absolute_paths) {
	ReconcileOutcome outcome;
	if (!matter.directory_path || matter.directory_path->empty()) return outcome;
	const std::filesystem::path root(*matter.directory_path);

	std::vector<std::string> folder_paths;
	if (collect_real_folder_paths(root, folder_paths) == false) {
		outcome.ok = false;
		outcome.degraded_reason = "Singularity could not read this Project's folder right now.";
		return outcome;
	}
	outcome.folder_paths = std::move(folder_paths);

	std::error_code canonicalRootError;
	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, canonicalRootError);
	if (canonicalRootError) {
		outcome.ok = false;
		outcome.degraded_reason = "Singularity could not read this Project's folder right now.";
		return outcome;
	}
	const std::string canonicalRootString = canonicalRoot.u8string();

	// De-duplicated relative paths, resolved and boundary-checked exactly like
	// a full scan's own discovered paths would be - a watcher-reported
	// absolute path is external input (the OS's own event data) and gets the
	// same Sec.19 treatment as a persisted relative_path: never trusted
	// outright.
	std::set<std::string> relativePaths;
	for (const std::string& absolutePathString : affected_absolute_paths) {
		std::error_code resolveError;
		const std::filesystem::path canonicalChanged =
				std::filesystem::weakly_canonical(std::filesystem::path(absolutePathString), resolveError);
		if (resolveError) continue;
		const std::string changedString = canonicalChanged.u8string();
		if (changedString.size() < canonicalRootString.size()) continue;
		if (changedString.compare(0, canonicalRootString.size(), canonicalRootString) != 0) continue;
		if (changedString.size() > canonicalRootString.size() &&
				changedString[canonicalRootString.size()] != std::filesystem::path::preferred_separator) {
			continue;
		}
		if (is_symlink_or_hidden_within(canonicalRoot, std::filesystem::relative(canonicalChanged, canonicalRoot)) == true) continue;
		const std::string relativeString = std::filesystem::relative(canonicalChanged, canonicalRoot).generic_string();
		if (relativeString.empty() == true || relativeString == ".") continue;
		relativePaths.insert(relativeString);
	}

	for (const std::string& relativePath : relativePaths) {
		const std::filesystem::path absolute = root / relativePath;
		if (is_symlink_or_hidden_within(root, relativePath) == true) continue;

		std::error_code typeError;
		const bool isRegularFile = std::filesystem::is_regular_file(absolute, typeError) == true && !typeError;
		if (isRegularFile == true) {
			FileStat stat_result;
			if (stat_file(absolute, stat_result) == false) continue;
			ObservedFile file;
			file.relative_path = relativePath;
			file.display_name = absolute.filename().string();
			file.device_id = stat_result.device_id;
			file.file_id = stat_result.file_id;
			file.creation_time_token = stat_result.creation_time_token;
			file.size_bytes = stat_result.size_bytes;
			observe_file_content(absolute, file, true);
			// allow_path_continuity_when_available == true: this is the live,
			// continuously-watched targeted path (round 3, item 2) -
			// reconcile_one_observed_file()'s own comment explains why that
			// distinction matters here and not in the full-scan matcher below.
			if (reconcile_one_observed_file(matter, file, true) == true) outcome.changed = true;
		} else {
			// Missing (deleted) or no longer a regular file - any Document
			// previously at this exact path is now truthfully unavailable. A
			// path that is a real directory now (never was a file) simply finds
			// nothing to mark and is a no-op, which is correct.
			if (mark_path_unavailable(matter, relativePath) == true) outcome.changed = true;
		}
	}

	return outcome;
}

bool is_relative_path_safe(const std::filesystem::path& project_root, const std::string& relative_path) {
	if (relative_path.empty() == true) return false;
	const std::filesystem::path candidate(relative_path);
	if (candidate.is_absolute() == true) return false;
	for (const std::filesystem::path& component : candidate) {
		if (component == "..") return false;
	}

	std::error_code error;
	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(project_root, error);
	if (error) return false;
	std::filesystem::path resolved = std::filesystem::weakly_canonical(canonicalRoot / candidate, error);
	if (error) return false;

	const std::string rootString = canonicalRoot.u8string();
	const std::string resolvedString = resolved.u8string();
	if (resolvedString.size() < rootString.size()) return false;
	if (resolvedString.compare(0, rootString.size(), rootString) != 0) return false;
	if (resolvedString.size() == rootString.size()) return true;
	return resolvedString[rootString.size()] == std::filesystem::path::preferred_separator;
}

namespace {

// A document the reader itself found nothing in - a scan, a file with no text
// - under an earlier version of the reading. The cache keeps that verdict for
// those bytes and that reading, so without this it would stand until the
// lawyer asked again, even after Singularity learned to read the document (a PDF
// whose signature pages are scans is read today with the text it has).
bool is_read_again_by_better_reading(const matters::MatterDocument& document, const std::string& extractorVersion) {
	if (document.lifecycle_state != matters::DocumentLifecycleState::NeedsAttention) return false;
	// A file that cannot be read right now keeps saying so, and comes back by
	// itself once it can be read (see reconcile): reading it again now would
	// only fail, and bury why under a generic failure.
	if (is_unreadable_marker(document) == true) return false;
	if (document.extraction_status != matters::DocumentExtractionState::OcrRequired
			&& document.extraction_status != matters::DocumentExtractionState::Empty) {
		return false;
	}
	if (document.extractor_version.empty() == true) return false;
	return document.extractor_version != extractorVersion;
}

// What the lawyer is told about a document that was read. A PDF read with some
// pages that are images says which pages were not read, in the reader's own
// plain sentence; a document read whole says nothing more.
std::optional<std::string> reading_limitation(const DocumentExtractionResult& extraction) {
	for (const ExtractionDiagnostic& diagnostic : extraction.diagnostics) {
		if (diagnostic.code == "pages_not_read" && diagnostic.message.empty() == false) return diagnostic.message;
	}
	return std::nullopt;
}

} // namespace

bool verify_cached_derivatives_exist(const std::filesystem::path& workspace_path, matters::Matter& matter) {
	bool changed = false;
	const LocalDocumentExtractor extractor;
	const DocumentCache cache(workspace_path, extractor);
	for (matters::MatterDocument& document : matter.documents) {
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		if (document.available == false) continue;
		const std::string extractorVersion = local_extractor_version(document.file_type);
		if (extractorVersion.empty()) continue;
		if (is_read_again_by_better_reading(document, extractorVersion) == true) {
			// Read again exactly as "Process again" would (request_reprocessing()):
			// the file itself did not change.
			document.lifecycle_state = matters::DocumentLifecycleState::Processing;
			document.attention_detail.reset();
			changed = true;
			continue;
		}
		if (document.lifecycle_state != matters::DocumentLifecycleState::Ready) continue;
		if (cache.holds(document.fingerprint, extractorVersion)) continue;
		document.lifecycle_state = matters::DocumentLifecycleState::Changed;
		document.attention_detail.reset();
		changed = true;
	}
	return changed;
}

bool process_pending_documents(const std::filesystem::path& workspace_path, matters::Matter& matter) {
	bool changed = false;
	const LocalDocumentExtractor extractor;
	const DocumentCache cache(workspace_path, extractor);

	for (matters::MatterDocument& document : matter.documents) {
		if (document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		if (document.available == false) continue;
		if (document.lifecycle_state != matters::DocumentLifecycleState::Processing &&
				document.lifecycle_state != matters::DocumentLifecycleState::Changed) {
			continue;
		}

		if (!matter.directory_path || is_relative_path_safe(*matter.directory_path, document.relative_path) == false) {
			// A malformed/malicious persisted relative_path (Sec.19) must never be
			// turned into a filesystem path and read - fail closed instead of
			// attempting to resolve or guess a safe location.
			document.lifecycle_state = matters::DocumentLifecycleState::NeedsAttention;
			document.attention_detail = "This document's location could not be safely resolved.";
			document.extraction_status = matters::DocumentExtractionState::Unreadable;
			changed = true;
			continue;
		}

		// An email file is read as its text, though no writer works on it: its
		// kind stays unknown, so nothing ever writes into it.
		std::string extension = std::filesystem::u8path(document.relative_path).extension().u8string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		const bool isEmail = document.file_type == DocumentFormat::unknown && extension == ".eml";
		// A picture likewise, by text recognition.
		const bool isPicture = document.file_type == DocumentFormat::unknown && (extension == ".png" || extension == ".jpg"
			|| extension == ".jpeg" || extension == ".tif" || extension == ".tiff" || extension == ".bmp" || extension == ".gif"
			|| extension == ".heic" || extension == ".heif");
		if (document.file_type == DocumentFormat::unknown && isEmail == false && isPicture == false) {
			document.lifecycle_state = matters::DocumentLifecycleState::Unsupported;
			document.attention_detail.reset();
			document.extraction_status = matters::DocumentExtractionState::Unsupported;
			changed = true;
			continue;
		}

		const std::filesystem::path source_path = std::filesystem::path(*matter.directory_path) / document.relative_path;
		const std::string extractorVersion = isEmail == true ? std::string("email-message-text/1")
			: isPicture == true ? std::string("picture-text-recognition/1") : local_extractor_version(document.file_type);
		const std::string cacheVersion = extractorVersion.empty() == true ? "unsupported/1" : extractorVersion;

		try {
			const PreparedDocument prepared = cache.prepare(source_path, document.file_type, cacheVersion);
			document.extraction_status = matters::to_document_extraction_state(prepared.extraction.status);
			document.extracted_character_count = static_cast<long long>(count_utf8_characters(prepared.extraction.text));
			document.page_count = prepared.extraction.structure.page_count
					? std::optional<long long>(static_cast<long long>(*prepared.extraction.structure.page_count))
					: std::nullopt;
			document.extractor_version = prepared.extraction.extractor_version;

			switch (prepared.extraction.status) {
				case ExtractionStatus::ready:
					document.lifecycle_state = matters::DocumentLifecycleState::Ready;
					document.attention_detail = reading_limitation(prepared.extraction);
					break;
				case ExtractionStatus::unsupported:
					document.lifecycle_state = matters::DocumentLifecycleState::Unsupported;
					document.attention_detail.reset();
					break;
				case ExtractionStatus::ocr_required:
					document.lifecycle_state = matters::DocumentLifecycleState::NeedsAttention;
					document.attention_detail = "This PDF is a scan: its pages are images with no text, so Singularity could not "
						"read it. A searchable copy saved in the Project folder can be read.";
					break;
				case ExtractionStatus::empty:
					document.lifecycle_state = matters::DocumentLifecycleState::NeedsAttention;
					document.attention_detail = "Singularity found no readable text in this document.";
					break;
				case ExtractionStatus::unreadable:
					document.lifecycle_state = matters::DocumentLifecycleState::NeedsAttention;
					document.attention_detail = "Text couldn't be extracted.";
					break;
			}
		} catch (const std::exception &) {
			document.lifecycle_state = matters::DocumentLifecycleState::NeedsAttention;
			document.attention_detail = "Text couldn't be extracted.";
			document.extraction_status = matters::DocumentExtractionState::Unreadable;
		}
		changed = true;
	}
	return changed;
}

bool request_reprocessing(matters::Matter& matter, const std::string& document_id) {
	const auto found = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument& entry) { return entry.id == document_id; });
	if (found == matter.documents.end()) return false;
	if (found->source_mode != matters::DocumentSourceMode::Referenced) return false;
	if (found->available == false) return false;
	if (found->lifecycle_state != matters::DocumentLifecycleState::NeedsAttention) return false;

	found->lifecycle_state = matters::DocumentLifecycleState::Processing;
	found->attention_detail.reset();
	return true;
}

ReconcileOutcome reconcile(const std::filesystem::path& workspace_path, matters::Matter& matter) {
	ReconcileOutcome outcome = detect_filesystem_changes(matter);
	if (outcome.ok == false) return outcome;
	const bool processedChanged = process_pending_documents(workspace_path, matter);
	outcome.changed = outcome.changed || processedChanged;
	return outcome;
}

void reset_hash_call_count_for_testing() noexcept { hash_call_count.store(0, std::memory_order_relaxed); }
long long hash_call_count_for_testing() noexcept { return hash_call_count.load(std::memory_order_relaxed); }

void set_hash_observer_for_testing(std::function<void(const std::filesystem::path&)> observer) {
	const std::lock_guard<std::mutex> lock(hash_observer_mutex);
	hash_observer = std::move(observer);
}

} // namespace singularity::documents::registry
