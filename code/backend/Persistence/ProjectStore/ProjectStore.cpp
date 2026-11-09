#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Events/Events.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Platform/Trace/Trace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <tuple>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace singularity::persistence {
namespace {

using Json = nlohmann::json;

// "chats" here is Chat METADATA only (id/title/createdAt/updatedAt/archived
// - see MatterChat in Chat/Types/Chat.hpp). Message-level content (currently
//   modeled as MatterTask: one prompt/response exchange, optionally tagged with
//   chat_id) lives in the independent "tasks" collection below, each exchange
//   keyed by its own stable id. This split is required, not incidental:
//   objectById/makePatch diff each collection entry as its own unit, so two
//   writers appending different tasks to the same chat_id merge automatically as
//   two independent upserts - neither writer's message is ever compared against,
//   or lost to, the other's. Do not add a messages array to chat_to_json. Phase
//   10 (Privacy - entities and Project coverage) adds "entities",
//   "entityAmbiguities" and "documentProtection" - the persistent Project Entity
//   Registry and its own local protection preprocessing state. Each entry already
//   carries its own stable "id" (documentProtection's id is its own document_id -
//   one status per Document), so they collaboratively merge through the exact
//   same per-object diff this makePatch/applyPatch already gives every collection
//   above - never a second persistence mechanism. Phase 12 (Context - canonical
//   data model and authority) adds "contextItems" and "contextSuggestions" -
//   canonical Project Context and its distinct suggestion queue. Two independent
//   collections, each entry keyed by its own stable id, merge the same way: an
//   edit to one Context item and a decision on an unrelated suggestion never
//   contend, and even a same-item conflict surfaces as a normal ProjectConflict
//   rather than a special case. Phase 13 (Context - editing, provenance and
//   entities) adds "contextItemRevisions" - the durable "previous version" a
//   direct human edit leaves behind (Context/Types/ProjectContext.hpp's
//   ContextItemRevision). Each revision record is its own append-only,
//   never-updated object with its own stable id, so it merges through this exact
//   same per-object diff too - not a second persistence mechanism, and never
//   itself the subject of a later edit/delete (deletes only ever originate from
//   the baseline side of the diff above, which never happens for a collection
//   this code never removes entries from). "documentEvents" is the data room's
//   journal (Document/Types/DocumentEvent.hpp): what a device noticed happening
//   to the Project's documents. Every event is its own object with its own id,
//   written once and never edited, so two devices' journals merge by union
//   through the same per-object diff. (The journal is bounded, so its oldest
//   events are deleted from the baseline side as new ones are added - an ordinary
//   delete of an object nobody else edits.) "documentReviews" are document
//   reviews (DocumentReview/Types): each review, its versions and the decisions
//   on them one object, written whole, so a decision never merges with a run's
//   state into something nobody wrote. "assignmentRecords" are the assignments
//   colleagues carried out: each assignment, its steps and its email one object,
//   written whole, as a review is. "correspondence" is the Matter's letters: each
//   letter, its attachments and how it was filed one object, written whole. A
//   build that does not know it replays past it and never deletes it.
//   "timeEntries" is the time members recorded on the Matter: each entry one
//   object, written whole, so two computers' entries merge one by one.
//   "reviewTables" and "reviewRows" are Review Tables (WorkProduct/Types/
//   ReviewTables.hpp): a table's questions and run merge per field; each
//   document's row is written whole as its run answers it, one small object per
//   document, so a table of many documents never rewrites itself whole.
const char *collectionNames[] = {"documents", "notes", "issues", "workProductTypes", "workProducts",
	"workProductRevisions", "consiliumConfigurations", "chats",
    "tasks", "entities", "entityAmbiguities", "documentProtection", "contextItems", "contextSuggestions",
    "contextItemRevisions", "documentEvents", "reviewTables", "reviewRows", "documentReviews", "correspondence",
    "assignmentRecords", "timeEntries"};
// AI Setup overrides are a single ordered role map for the Project, not a
// collection of independently addressable domain objects.  Persist the array
// as a field so the effective-routing resolver sees exactly what the Project
// saved after a fresh .lm reload.  Omitting it here made the bridge echo an
// in-memory success while silently dropping the assignment at the Project
// boundary.
// A Project's location is where each device found it, never shared content:
// it is not written into operations, and one an older build recorded is
// ignored on replay, so two devices on one Project never conflict over it.
// The protected-computation observation ledger and the written-file anchors
// (DESIGN4 §8.3, §4.5) are single Project-lifetime values, not collections of
// addressable objects: fields, so a change to either reaches the operation
// stream and survives replay. Absent on both sides, they emit no patch.
const char *fieldNames[] = {
    "name", "description", "instructions", "appearance", "memoryDocumentDigests",
    "aiSetupOverrides", "updatedAt", "observationLedger", "writtenFileAnchors", "archivedAt", "archivedBy",
    // The Matter's correspondence intake: its rules, a single list.
    "correspondenceIntake"};

// How two writers' changes to the same object of a collection combine (see
// mergeUpsert()): PerField collections conflict only when both changed the
// same field to different values; Observed collections record what a device
// observed on disk, where the latest observation wins; Whole collections keep
// one object as one unit, so a Context item or Work Product revision never
// silently merges two authors' intent.
enum class MergePolicy { PerField, Observed, Whole };

MergePolicy mergePolicyFor(const std::string &collection) {
  if (collection == "chats" || collection == "tasks" || collection == "notes" || collection == "issues"
      || collection == "workProductTypes" || collection == "reviewTables")
    return MergePolicy::PerField;
  if (collection == "documents" || collection == "documentProtection") return MergePolicy::Observed;
  return MergePolicy::Whole;
}

// Keys that record when and by whom an object last changed rather than what it
// says. Two writers touching them is not a disagreement: the later value wins
// for timestamps and actors, the higher for revisions, and event histories
// are united.
bool isBookkeepingKey(const std::string &key) {
  return key == "updatedAt" || key == "operationUpdatedAt" || key == "updatedByActorId" || key == "revision"
      || key == "operationRevision" || key == "operationEvents";
}

Json mergedBookkeeping(const std::string &key, const Json &current, const Json &attempted) {
  if (current.is_null()) return attempted;
  if (attempted.is_null()) return current;
  if (key == "operationEvents" && current.is_array() && attempted.is_array()) {
    Json merged = current;
    std::set<std::string> known;
    for (const Json &event : current) known.insert(event.is_object() ? event.value("id", event.dump()) : event.dump());
    for (const Json &event : attempted)
      if (known.insert(event.is_object() ? event.value("id", event.dump()) : event.dump()).second) merged.push_back(event);
    std::stable_sort(merged.begin(), merged.end(), [](const Json &left, const Json &right) {
      return left.is_object() && right.is_object() && left.value("sequence", 0LL) < right.value("sequence", 0LL);
    });
    return merged;
  }
  if (current.is_number() && attempted.is_number()) return current.get<double>() >= attempted.get<double>() ? current : attempted;
  if (current.is_string() && attempted.is_string()) return current.get<std::string>() >= attempted.get<std::string>() ? current : attempted;
  return attempted;
}

// Whether `left` and `right` say the same thing, bookkeeping aside.
bool equalIgnoringBookkeeping(const Json &left, const Json &right) {
  if (!left.is_object() || !right.is_object()) return left == right;
  Json leftContent = left;
  Json rightContent = right;
  for (auto it = leftContent.begin(); it != leftContent.end();) it = isBookkeepingKey(it.key()) ? leftContent.erase(it) : std::next(it);
  for (auto it = rightContent.begin(); it != rightContent.end();) it = isBookkeepingKey(it.key()) ? rightContent.erase(it) : std::next(it);
  return leftContent == rightContent;
}

// An operation's file is named by its id, and a second file of the same name
// would replace it: the id's 32 digits are 128 bits from the system's secure
// source (Platform/SecureRandom), never a generator seeded with 32.
std::string generateId(const std::string &prefix) {
  return prefix + platform::secure_random_hex(16);
}

std::filesystem::path temporaryPath(const std::filesystem::path &path) {
  return path.string() + ".tmp-" + generateId("");
}

// One rename (Platform/AtomicFile's replace_file): a file replaces a file,
// and a directory moves only onto an absent destination, so a pre-existing
// destination fails the call rather than silently merging into a
// half-migrated .lm.
void atomicReplace(const std::filesystem::path &source, const std::filesystem::path &destination) {
  if (const auto failure = platform::replace_file(source, destination))
    throw std::runtime_error("Unable to replace Project data: " + *failure);
}

std::ifstream openProjectFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (file.is_open() == false) {
    // A file this process may not read is not damaged data: the two are
    // reported apart, so lost access is never presented as corruption.
    if (errno == EACCES || errno == EPERM)
      throw ProjectDataAccessDenied("Singularity is not allowed to read this Project's data: " + path.string());
    throw std::runtime_error("Unable to read Project data: " + path.string());
  }
  return file;
}

Json readJson(const std::filesystem::path &path) {
  std::ifstream file = openProjectFile(path);
  return Json::parse(file);
}

std::string readBytes(const std::filesystem::path &path) {
  std::ifstream file = openProjectFile(path);
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) throw std::runtime_error("Unable to read Project data: " + path.string());
  return bytes;
}

// A full disk is told apart from every other write failure, and the free-space
// check before a save is the one every staged write makes (Platform/DiskSpace).
bool isDiskFullErrno(int value) {
  return platform::is_disk_full_error(value);
}

void requireFreeSpace(const std::filesystem::path &directory, std::uintmax_t bytes) {
  platform::require_free_space(directory, bytes);
}

// Puts the file's bytes on the disk before it is renamed into place, so a
// power loss can never leave the new name over bytes that were not written.
// On macOS a barrier is enough for that order and costs far less than a full
// flush of the drive's cache, which every save of an operation would pay.
// Windows writes the file's buffers through (FlushFileBuffers) before the
// rename, as the other platforms do.
void syncFile(const std::filesystem::path &path) {
#if !defined(_WIN32)
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) return;
#if defined(__APPLE__) && defined(F_BARRIERFSYNC)
  if (::fcntl(descriptor, F_BARRIERFSYNC) != 0) (void)::fsync(descriptor);
#else
  (void)::fsync(descriptor);
#endif
  ::close(descriptor);
#else
  const HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return;
  (void)::FlushFileBuffers(handle);
  ::CloseHandle(handle);
#endif
}

// The new name on the disk too, once a file is renamed into place: without it a
// power loss can leave the folder as it was before the rename, the saved step
// gone. NTFS commits a rename in its own journal.
void syncDirectory(const std::filesystem::path &directory) {
#if !defined(_WIN32)
  const int descriptor = ::open(directory.c_str(), O_RDONLY);
  if (descriptor < 0) return;
#if defined(__APPLE__) && defined(F_BARRIERFSYNC)
  if (::fcntl(descriptor, F_BARRIERFSYNC) != 0) (void)::fsync(descriptor);
#else
  (void)::fsync(descriptor);
#endif
  ::close(descriptor);
#else
  (void)directory;
#endif
}

void writeJson(const std::filesystem::path &path, const Json &value) {
  const std::string text = value.dump(2);
  requireFreeSpace(path.parent_path(), text.size());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (file.is_open() == false) {
    if (isDiskFullErrno(errno)) throw DiskFull();
    throw std::runtime_error("Unable to write Project data: " + path.string());
  }
  errno = 0;
  file << text;
  file.flush();
  if (file.good() == false) {
    if (isDiskFullErrno(errno)) throw DiskFull();
    throw std::runtime_error("Unable to write Project data: " + path.string());
  }
  file.close();
  if (file.fail() == true) {
    if (isDiskFullErrno(errno)) throw DiskFull();
    throw std::runtime_error("Unable to close Project data: " + path.string());
  }
  syncFile(path);
}

void writeJsonAtomic(const std::filesystem::path &path, const Json &value) {
  std::filesystem::create_directories(path.parent_path());
  const std::filesystem::path temporary = temporaryPath(path);
  try {
    writeJson(temporary, value);
    atomicReplace(temporary, path);
    syncDirectory(path.parent_path());
  } catch (...) {
    std::error_code cleanupError;
    std::filesystem::remove(temporary, cleanupError);
    throw;
  }
}

Json objectById(const Json &array) {
  Json result = Json::object();
  if (array.is_array() == false) return result;
  for (const Json &value : array) {
    if (value.is_object() == false) continue;
    const std::string id = value.value("id", std::string());
    if (id.empty() == false) result[id] = value;
  }
  return result;
}

// A collection in the order its objects were created. Timestamps only have
// whole seconds, so objects made in the same second keep the order they first
// appeared in (`appearance`, see ReplayState) - never their random ids, which
// let two chat turns of one second swap and made the earlier one "the latest".
Json arrayByStableOrder(const Json &values, const std::map<std::string, std::uint64_t> &appearance) {
  struct Entry {
    std::string timestamp;
    std::uint64_t appeared;
    const std::string *id;
    const Json *value;
  };
  std::vector<Entry> ordered;
  for (auto entry = values.begin(); entry != values.end(); ++entry) {
    const std::string timestamp = entry->value(
        "createdAt", entry->value("addedAt", entry->value("updatedAt", std::string())));
    const auto seen = appearance.find(entry.key());
    ordered.push_back({timestamp, seen != appearance.end() ? seen->second : UINT64_MAX, &entry.key(), &*entry});
  }
  std::sort(ordered.begin(), ordered.end(), [](const Entry &left, const Entry &right) {
    return std::tie(left.timestamp, left.appeared, *left.id) < std::tie(right.timestamp, right.appeared, *right.id);
  });
  Json result = Json::array();
  for (const Entry &entry : ordered) result.push_back(*entry.value);
  return result;
}

Json makePatch(const Json &baseline, const Json &desired) {
  Json patch{{"fields", Json::object()}, {"collections", Json::object()}};
  for (const char *field : fieldNames) {
    const Json baselineValue = baseline.contains(field) ? baseline.at(field) : Json(nullptr);
    const Json desiredValue = desired.contains(field) ? desired.at(field) : Json(nullptr);
    if (baselineValue != desiredValue) {
      patch["fields"][field] = Json{{"base", baselineValue}, {"value", desiredValue}};
    }
  }

  // Both sides are indexed by id in place: a save compares the whole Project to
  // find the few objects that changed, and copying every object of every
  // collection to do it cost more than everything else a save does. Exactly
  // objectById()'s rule - objects with a non-empty id, the last one winning -
  // and std::map keeps the same key order its result had.
  const auto indexById = [](const Json &state, const char *collection) {
    std::map<std::string, const Json *> byId;
    const auto found = state.find(collection);
    if (found == state.end() || found->is_array() == false) return byId;
    for (const Json &value : *found) {
      if (value.is_object() == false) continue;
      const std::string id = value.value("id", std::string());
      if (id.empty() == false) byId[id] = &value;
    }
    return byId;
  };
  for (const char *collection : collectionNames) {
    const auto baselineValues = indexById(baseline, collection);
    const auto desiredValues = indexById(desired, collection);
    Json upserts = Json::object();
    Json deletes = Json::array();
    for (const auto &[id, value] : desiredValues) {
      const auto base = baselineValues.find(id);
      if (base != baselineValues.end() && *base->second == *value) continue;
      upserts[id] = Json{{"base", base != baselineValues.end() ? *base->second : Json(nullptr)}, {"value", *value}};
    }
    for (const auto &[id, value] : baselineValues) {
      if (desiredValues.count(id) == 0) deletes.push_back(Json{{"id", id}, {"base", *value}});
    }
    if (upserts.empty() == false || deletes.empty() == false) {
      patch["collections"][collection] = Json{{"upserts", upserts}, {"deletes", deletes}};
    }
  }
  return patch;
}

bool isPatchEmpty(const Json &patch) {
  return patch.at("fields").empty() == true && patch.at("collections").empty() == true;
}

// A save that changes nothing but the times things were changed changes
// nothing: written, it would be an operation every computer of a shared Matter
// takes. A protection pass that wrote the same status again each second filled
// a shared Matter with some 760 such operations in 35 minutes, each carried to
// the firm's other computer. A new or removed object is a change; so is any
// field other than updatedAt.
bool isTimestampOnly(const Json &patch) {
  for (const auto &[field, change] : patch.at("fields").items()) {
    static_cast<void>(change);
    if (field != "updatedAt") return false;
  }
  for (const auto &[collection, changes] : patch.at("collections").items()) {
    static_cast<void>(collection);
    if (changes.at("deletes").empty() == false) return false;
    for (const auto &[id, upsert] : changes.at("upserts").items()) {
      static_cast<void>(id);
      const Json &base = upsert.at("base");
      const Json &value = upsert.at("value");
      if (base.is_object() == false || value.is_object() == false) return false;
      Json before = base;
      Json after = value;
      before.erase("updatedAt");
      after.erase("updatedAt");
      if (before != after) return false;
    }
  }
  return true;
}

// Replays one operation's patch onto the Project's fields and its collections
// (each kept as an id-keyed object for the whole replay, so an operation costs
// what its patch holds, not the Project's size). A conflict (current state
// matches neither the operation's recorded base nor its intended value, and
// the collection's merge policy cannot combine them) never silently drops the
// losing alternative: it is recorded as a ProjectConflict carrying the
// competing current/attempted values plus the operation that lost, so a later
// merge-review surface has everything it needs to show both sides and let a
// person resolve it - state itself keeps whatever the earlier-replayed
// operation already established, since operations replay in the same
// previousVersion/timestamp/operationId order every time, making that outcome
// deterministic across reloads.
struct ReplayState {
  Json fields;
  std::map<std::string, Json> collections;
  // Collections an operation changed; only these are re-ordered at the end,
  // exactly as when each operation re-ordered the collection it touched.
  std::set<std::string> touched;
  // When each object first appeared, per collection: the Base's own order,
  // then replay order. Operations always replay in the same order, so this is
  // the same on every load and every device; it only settles same-second ties
  // (see arrayByStableOrder()).
  std::map<std::string, std::map<std::string, std::uint64_t>> appearance;
  std::uint64_t nextAppearance = 0;
  // Whose operation first brought each object in, per collection: the seat
  // a document's "added by" is said from (MatterDocument::added_by_actor_id).
  // An object of the Base has none.
  std::map<std::string, std::map<std::string, std::string>> firstWriter;
};

ProjectConflict conflictFor(const std::string &id, const std::string &operationId, const std::string &actorId,
    const std::string &timestamp, const Json &base, const Json &current, const Json &attempted,
    const std::map<std::string, std::tuple<std::string, std::string, std::string>> &provenance,
    const std::string &provenanceKey) {
  ProjectConflict conflict;
  conflict.id = id;
  conflict.operation_id = operationId;
  conflict.actor_id = actorId;
  conflict.timestamp = timestamp;
  conflict.base_json = base.dump();
  conflict.current_json = current.dump();
  conflict.attempted_json = attempted.dump();
  const auto currentProvenance = provenance.find(provenanceKey);
  if (currentProvenance != provenance.end())
    std::tie(conflict.current_operation_id, conflict.current_actor_id, conflict.current_timestamp) = currentProvenance->second;
  return conflict;
}

// Combines one writer's change to an object with what earlier operations made
// of it. Returns the merged object, or nothing when the change conflicts;
// `conflictingField` then names the first contested field of a PerField object.
std::optional<Json> mergeUpsert(MergePolicy policy, const Json &current, const Json &base, const Json &attempted,
    std::string &conflictingField) {
  if (current == base || current == attempted) return attempted;
  if (policy == MergePolicy::Observed) {
    // The newer observation wins, by when the observed thing last changed
    // (a document's modification time, a protection status's update), so the
    // result never depends on which device's operation happens to replay last.
    if (current.is_object() && attempted.is_object()) {
      for (const char *key : {"lastModified", "updatedAt"}) {
        const std::string currentTime = current.value(key, std::string());
        const std::string attemptedTime = attempted.value(key, std::string());
        if (!currentTime.empty() && !attemptedTime.empty() && currentTime != attemptedTime)
          return currentTime > attemptedTime ? current : attempted;
      }
    }
    return attempted;
  }
  if (!current.is_object() || !base.is_object() || !attempted.is_object()) {
    if (policy == MergePolicy::Whole && current.is_object() && attempted.is_object() && base.is_object()
        && equalIgnoringBookkeeping(current, base))
      return attempted;
    return std::nullopt;
  }
  if (policy == MergePolicy::Whole) {
    // Only bookkeeping differs from what this writer saw, or both writers
    // arrived at the same content: one unit, no disagreement.
    if (equalIgnoringBookkeeping(current, base) || equalIgnoringBookkeeping(current, attempted)) {
      Json merged = attempted;
      for (const auto &entry : current.items())
        if (isBookkeepingKey(entry.key()))
          merged[entry.key()] = mergedBookkeeping(entry.key(), entry.value(),
              attempted.contains(entry.key()) ? attempted.at(entry.key()) : Json(nullptr));
      return merged;
    }
    return std::nullopt;
  }
  Json merged = current;
  std::set<std::string> keys;
  for (const auto &entry : base.items()) keys.insert(entry.key());
  for (const auto &entry : attempted.items()) keys.insert(entry.key());
  for (const auto &entry : current.items()) keys.insert(entry.key());
  for (const std::string &key : keys) {
    const Json baseValue = base.contains(key) ? base.at(key) : Json(nullptr);
    const Json attemptedValue = attempted.contains(key) ? attempted.at(key) : Json(nullptr);
    const Json currentValue = current.contains(key) ? current.at(key) : Json(nullptr);
    if (isBookkeepingKey(key)) {
      const Json value = mergedBookkeeping(key, currentValue, attemptedValue);
      if (value.is_null()) merged.erase(key); else merged[key] = value;
      continue;
    }
    if (attemptedValue == baseValue || attemptedValue == currentValue) continue;
    if (currentValue != baseValue) {
      conflictingField = key;
      return std::nullopt;
    }
    if (attemptedValue.is_null() && !attempted.contains(key)) merged.erase(key);
    else merged[key] = attemptedValue;
  }
  return merged;
}

void applyPatch(ReplayState &state, const Json &patch, const std::string &operationId, const std::string &actorId,
    const std::string &timestamp, std::vector<ProjectConflict> &conflicts,
		std::map<std::string, std::tuple<std::string, std::string, std::string>> &provenance) {
  // Each .value(key, default) below returns a temporary Json by copy, not a
  // reference into patch. Calling .items() directly on that temporary would
  // leave the range-for iterating a proxy into an already-destroyed object -
  // range-for only lifetime-extends the temporary bound to its hidden range
  // reference, not an intermediate temporary a member call was made on. Each
  // one is named first so the range-for iterates a value that outlives it.
  const Json fields = patch.value("fields", Json::object());
  for (const auto &entry : fields.items()) {
    if (entry.key() == "directoryPath") continue;
    const Json current = state.fields.contains(entry.key()) ? state.fields.at(entry.key()) : Json(nullptr);
    const Json base = entry.value().value("base", Json(nullptr));
    const Json value = entry.value().value("value", Json(nullptr));
    if (isBookkeepingKey(entry.key())) {
      state.fields[entry.key()] = mergedBookkeeping(entry.key(), current, value);
	  provenance["field:" + entry.key()] = std::make_tuple(operationId, actorId, timestamp);
      continue;
    }
    if (current == base || current == value) {
      state.fields[entry.key()] = value;
	  provenance["field:" + entry.key()] = std::make_tuple(operationId, actorId, timestamp);
      continue;
    }
	ProjectConflict conflict = conflictFor("conflict:" + operationId + ":field:" + entry.key() + ":update", operationId,
		actorId, timestamp, base, current, value, provenance, "field:" + entry.key());
	conflict.field = entry.key();
	conflict.kind = "update";
	conflicts.push_back(std::move(conflict));
  }

  const Json collections = patch.value("collections", Json::object());
  for (const auto &collection : collections.items()) {
    const MergePolicy policy = mergePolicyFor(collection.key());
    state.touched.insert(collection.key());
    Json &currentValues = state.collections[collection.key()];
    if (!currentValues.is_object()) currentValues = Json::object();
    const Json upserts = collection.value().value("upserts", Json::object());
    for (const auto &entry : upserts.items()) {
      const Json current = currentValues.contains(entry.key()) ? currentValues.at(entry.key()) : Json(nullptr);
      const Json base = entry.value().value("base", Json(nullptr));
      const Json value = entry.value().value("value", Json(nullptr));
      std::string conflictingField;
      const std::optional<Json> merged = mergeUpsert(policy, current, base, value, conflictingField);
      const std::string provenanceKey = "collection:" + collection.key() + ":" + entry.key();
      if (merged) {
        std::map<std::string, std::uint64_t> &appeared = state.appearance[collection.key()];
        if (appeared.count(entry.key()) == 0) {
          appeared.emplace(entry.key(), state.nextAppearance++);
          if (!actorId.empty()) state.firstWriter[collection.key()].emplace(entry.key(), actorId);
        }
        currentValues[entry.key()] = *merged;
		provenance[provenanceKey] = std::make_tuple(operationId, actorId, timestamp);
        continue;
      }
	  ProjectConflict conflict = conflictFor(
		  "conflict:" + operationId + ":collection:" + collection.key() + ":" + entry.key() + ":update", operationId,
		  actorId, timestamp, base, current, value, provenance, provenanceKey);
	  conflict.collection = collection.key();
	  conflict.object_id = entry.key();
	  conflict.field = conflictingField;
	  conflict.kind = "update";
	  conflicts.push_back(std::move(conflict));
    }
    for (const Json &deletion : collection.value().value("deletes", Json::array())) {
      const std::string id = deletion.value("id", std::string());
      if (currentValues.contains(id) == false) continue;
      const Json base = deletion.value("base", Json(nullptr));
      const Json current = currentValues.at(id);
      if (current == base || policy == MergePolicy::Observed || equalIgnoringBookkeeping(current, base)) {
        currentValues.erase(id);
		provenance["collection:" + collection.key() + ":" + id] = std::make_tuple(operationId, actorId, timestamp);
        continue;
      }
	  ProjectConflict conflict = conflictFor(
		  "conflict:" + operationId + ":collection:" + collection.key() + ":" + id + ":delete", operationId, actorId,
		  timestamp, base, current, Json(nullptr), provenance, "collection:" + collection.key() + ":" + id);
	  conflict.collection = collection.key();
	  conflict.object_id = id;
	  conflict.kind = "delete";
	  conflicts.push_back(std::move(conflict));
    }
  }
}

std::function<void()> &afterBaseChecked() {
  static std::function<void()> told;
  return told;
}

struct StoredOperation {
  std::filesystem::path path;
  Json value;
};

// Every operation file of a Project, each read and parsed exactly once, in
// replay order (previousVersion, then timestamp and operation id). A copy of
// the same operation (a sync client's duplicate) is replayed once. An
// operation file that cannot be read as an operation - half brought in by a
// sync client, cut short by a crash, damaged - is left exactly where it is and
// not replayed: the Project opens without it, and the file is read again once
// it changes. It is never moved or deleted: in a shared Matter the same file
// may be whole on another member's computer, and a rename or a removal would
// travel there. Lost access is reported as itself.
std::optional<Json> readOperationFile(const std::filesystem::path &path) {
  try {
    Json value = readJson(path);
    if (value.is_object() == false) return std::nullopt;
    return value;
  } catch (const ProjectDataAccessDenied &) {
    throw;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::vector<StoredOperation> readOperations(const std::filesystem::path &dataPath,
    std::vector<std::string> *unreadable = nullptr) {
  std::vector<StoredOperation> result;
  const std::filesystem::path operations = dataPath / "Operations";
  std::error_code error;
  if (std::filesystem::is_directory(operations, error) == false) return result;
  std::set<std::string> seen;
  for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(operations)) {
    if (entry.is_regular_file() == false || entry.path().extension() != ".json") continue;
    std::optional<Json> read = readOperationFile(entry.path());
    if (!read) {
      if (unreadable != nullptr) unreadable->push_back(entry.path().filename().u8string());
      continue;
    }
    Json value = std::move(*read);
    const std::string operationId = value.value("operationId", std::string());
    if (!operationId.empty() && seen.insert(operationId).second == false) continue;
    result.push_back({entry.path(), std::move(value)});
  }
  std::sort(result.begin(), result.end(), [](const StoredOperation &left, const StoredOperation &right) {
    const long long leftVersion = left.value.value("previousVersion", 0LL);
    const long long rightVersion = right.value.value("previousVersion", 0LL);
    if (leftVersion != rightVersion) return leftVersion < rightVersion;
    const std::string leftOrder = left.value.value("timestamp", std::string()) + left.value.value("operationId", std::string());
    const std::string rightOrder = right.value.value("timestamp", std::string()) + right.value.value("operationId", std::string());
    return leftOrder < rightOrder;
  });
  return result;
}

void createProjectDirectories(const std::filesystem::path &dataPath) {
  const char *directories[] = {
      "Activity", "Cache", "Chats", "Context", "Documents", "EntityRegistry",
      "Index", "Operations", "Routing", "Usage", "WorkProducts",
  };
  for (const char *directory : directories) std::filesystem::create_directories(dataPath / directory);
}

std::string locationId(const std::filesystem::path &path) {
  return "location:" + std::to_string(std::hash<std::string>{}(path.u8string()));
}

// No contract fixes an exact recursion depth for Project discovery; this is
// a deliberate, adjustable implementation bound (not product policy) that
// keeps an enormous authorised/shared root from turning every launch into
// an unbounded filesystem walk, while still finding Projects nested a
// realistic few folders deep (practice area / client / matter / ...).
constexpr int kMaxDiscoveryDepth = 6;

// Whether the system hides `folder` from the person: macOS's own hidden flag
// (their Library) or Windows' hidden attribute (their AppData). A lawyer keeps
// no Matter where Finder or Explorer never shows it; a dot-named folder is
// passed over by its name, on every system.
bool isHiddenBySystem(const std::filesystem::path &folder) {
#if defined(__APPLE__)
  struct stat info {};
  return ::lstat(folder.c_str(), &info) == 0 && (info.st_flags & UF_HIDDEN) != 0;
#elif defined(_WIN32)
  const DWORD attributes = ::GetFileAttributesW(folder.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
  (void)folder;
  return false;
#endif
}

// Recursively collects directories under root that might be Project folders -
// root itself plus every descendant up to kMaxDiscoveryDepth, per "…". Never
// descends into a Project's own .lm/.lm.migrating-* data, and skips
// dot-prefixed folders (.git, OS metadata, ...) and folders the system hides
// as neither a candidate nor a place Projects are expected to live. Each
// folder is opened on its own: one that cannot be read (permission, a macOS
// privacy-protected folder, a disconnected network mount) is passed over
// alone, and the walk goes on. 2026-10-30: the walk of a Mac's home stopped
// at its 20th folder, the Photo Booth Library ("Operation not permitted"),
// and found no Matter; walking its hidden Library as well took 4.2 s over
// 54,000 folders, against 0.2 s. A symlinked folder is a candidate, never
// walked into. The person's home folder, as the system names it; nullopt when
// unset.
std::optional<std::filesystem::path> homeFolder() {
#if defined(_WIN32)
  const wchar_t *home = ::_wgetenv(L"USERPROFILE");
  if (home == nullptr || *home == L'\0') return std::nullopt;
#else
  const char *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return std::nullopt;
#endif
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(std::filesystem::path(home), error);
  return error ? std::filesystem::path(home) : resolved;
}

// A folder that holds no Matter and is passed over by its name: the home
// folder's media folders, which macOS guards - a walk into Music or Movies asks
// the person for their media library and waits for the answer, so the window
// listed no Matter for minutes - the system's packages (an app, a photo or
// music library) and a developer's node_modules. A "Music" folder inside a
// folder the lawyer chose is theirs, and is searched.
bool isNeverAMatterFolder(const std::filesystem::path &folder, const std::optional<std::filesystem::path> &home) {
  const std::string name = folder.filename().u8string();
  if (name == "node_modules") return true;
  std::string extension = folder.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
  static const std::set<std::string> packages{".app", ".photoslibrary", ".musiclibrary", ".tvlibrary", ".imovielibrary",
                                              ".fcpbundle", ".framework", ".bundle", ".plugin", ".kext"};
  if (packages.count(extension) > 0) return true;
  static const std::set<std::string> media{"Music", "Movies", "Pictures", "Videos"};
  if (home.has_value() == false || media.count(name) == 0) return false;
  std::error_code error;
  const std::filesystem::path parent = std::filesystem::weakly_canonical(folder.parent_path(), error);
  return !error && parent == *home;
}

void collectProjectCandidates(const std::filesystem::path &root, std::vector<std::filesystem::path> &candidates) {
  const std::optional<std::filesystem::path> home = homeFolder();
  candidates.push_back(root);
  struct Pending {
    std::filesystem::path folder;
    int depth;
  };
  std::vector<Pending> pending{{root, -1}};
  while (pending.empty() == false) {
    const Pending next = std::move(pending.back());
    pending.pop_back();
    std::error_code error;
    std::filesystem::directory_iterator entries(next.folder, error);
    if (error) continue;
    std::vector<Pending> inside;
    for (const std::filesystem::directory_iterator end; entries != end; entries.increment(error)) {
      if (error) break;
      std::error_code entryError;
      if (entries->is_directory(entryError) == false || entryError) continue;
      const std::filesystem::path &path = entries->path();
      const std::string filename = path.filename().string();
      if (filename == ".lm" || filename.rfind(".lm.migrating-", 0) == 0) continue;
      if (filename.empty() == false && filename.front() == '.') continue;
      if (isHiddenBySystem(path) == true) continue;
      if (isNeverAMatterFolder(path, home) == true) continue;
      candidates.push_back(path);
      const int depth = next.depth + 1;
      if (depth < kMaxDiscoveryDepth && entries->is_symlink(entryError) == false && !entryError) inside.push_back({path, depth});
    }
    // Walked in the order the folder lists them, as before.
    pending.insert(pending.end(), std::make_move_iterator(inside.rbegin()), std::make_move_iterator(inside.rend()));
  }
}

// Singularity's own state at any depth of a Project folder: canonical `.lm`, its
// interrupted migration staging, legacy `.singularity-workspace` folders (earlier builds kept
// patch backups beside documents in one) and staged or crash-leftover write
// files - none of it belongs in a clean copy.
bool isSingularityStateName(const std::filesystem::path &name) {
  const std::string value = name.string();
  return value == ".lm" || value == ".singularity-workspace" || value.rfind(".lm.migrating-", 0) == 0
      || platform::is_staged_file_name(value);
}

// Earlier builds kept document backups in `.singularity-workspace/backups` beside documents,
// at any depth of the Project folder. Only such backup folders are removed - a
// `.singularity-workspace` holding anything else is left alone.
void removeLegacyDocumentBackups(const std::filesystem::path &directory, int depth) {
  if (depth > 32) return;
  std::error_code error;
  std::vector<std::filesystem::path> children;
  for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error)) {
    if (entry->is_symlink(error) || !entry->is_directory(error)) continue;
    children.push_back(entry->path());
  }
  for (const std::filesystem::path &child : children) {
    if (child.filename() == ".lm") continue;
    if (child.filename() == ".singularity-workspace") {
      std::filesystem::remove_all(child / "backups", error);
      error.clear();
      if (std::filesystem::is_empty(child, error) && !error) std::filesystem::remove(child, error);
      continue;
    }
    removeLegacyDocumentBackups(child, depth + 1);
  }
}

void copyWithoutProjectData(const std::filesystem::path &source, const std::filesystem::path &destination) {
  std::filesystem::create_directories(destination);
  for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(source)) {
    if (isSingularityStateName(entry.path().filename())) continue;
    const std::filesystem::path target = destination / entry.path().filename();
    if (!entry.is_symlink() && entry.is_directory()) {
      copyWithoutProjectData(entry.path(), target);
      continue;
    }
    std::filesystem::copy(entry.path(), target, std::filesystem::copy_options::copy_symlinks);
  }
}

// A Project folder is shared, and the two devices sharing it will not always run
// the same build of Singularity. This build writes what its own structs can
// express; anything a NEWER build wrote that these structs cannot hold would
// otherwise be diffed away and the deletion replicated through the operation log
// - a newer device's work destroyed by an older one, silently.
//
// The two cases are separated exactly, without guessing: `representable` is
// this build's own round-trip of the loaded baseline, so
//   - a field or object in the baseline that does NOT survive that round trip
//     is something this build cannot represent: it is carried forward untouched;
//   - a field or object that DOES survive it, and is absent from what is being
//     saved, was genuinely removed by this build: that removal is honoured.
// Nothing is resurrected and nothing unknown is lost.
//
// At every depth, not only a record's own keys. A part this build left as it was
// is written back as stored, verbatim; inside a part it changed, a key it cannot
// represent is carried forward, and the elements of a list are matched by their
// "id", else their "number", else by place when the list kept its length - never
// guessed otherwise.
bool identifiedBy(const Json &array, const char *identity) {
	if (!array.is_array() || array.empty()) return false;
	std::set<std::string> seen;
	for (const Json &element : array) {
		if (!element.is_object() || !element.contains(identity)) return false;
		const Json &value = element.at(identity);
		if (!value.is_string() && !value.is_number_integer()) return false;
		if (!seen.insert(value.dump()).second) return false;
	}
	return true;
}

void carryUnrepresented(const Json &baseline, const Json &representable, Json &desired) {
	if (desired == representable) {
		desired = baseline;
		return;
	}
	if (baseline.is_object() && desired.is_object()) {
		for (const auto &field : baseline.items()) {
			const bool representedHere = representable.is_object() && representable.contains(field.key());
			const auto mine = desired.find(field.key());
			if (mine == desired.end()) {
				// Absent here: unknown to this build, it is carried; known to it,
				// this build removed it, and the removal stands.
				if (!representedHere) desired[field.key()] = field.value();
				continue;
			}
			if (representedHere) carryUnrepresented(field.value(), representable.at(field.key()), *mine);
		}
		return;
	}
	if (!baseline.is_array() || !desired.is_array() || !representable.is_array()) return;
	for (const char *identity : {"id", "number"}) {
		if (!identifiedBy(baseline, identity) || !identifiedBy(desired, identity)) continue;
		std::map<std::string, const Json *> stored;
		std::map<std::string, const Json *> roundTrip;
		for (const Json &element : baseline) stored[element.at(identity).dump()] = &element;
		if (identifiedBy(representable, identity))
			for (const Json &element : representable) roundTrip[element.at(identity).dump()] = &element;
		std::set<std::string> present;
		for (Json &element : desired) {
			const std::string key = element.at(identity).dump();
			present.insert(key);
			const auto storedElement = stored.find(key);
			if (storedElement == stored.end()) continue;
			const auto roundTripElement = roundTrip.find(key);
			carryUnrepresented(*storedElement->second,
				roundTripElement != roundTrip.end() ? *roundTripElement->second : Json(), element);
		}
		// An element this build could not even read is not one it removed.
		for (const Json &element : baseline) {
			const std::string key = element.at(identity).dump();
			if (present.count(key) == 0 && roundTrip.count(key) == 0) desired.push_back(element);
		}
		return;
	}
	if (baseline.size() == representable.size() && representable.size() == desired.size()) {
		for (std::size_t index = 0; index < desired.size(); ++index)
			carryUnrepresented(baseline.at(index), representable.at(index), desired.at(index));
	}
}

Json preserveCollection(const Json &baselineArray, const Json &representableArray, const Json &desiredArray) {
	const auto indexById = [](const Json &array) {
		std::map<std::string, const Json *> byId;
		if (!array.is_array()) return byId;
		for (const Json &entry : array) {
			if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string()) continue;
			byId[entry.at("id").get<std::string>()] = &entry;
		}
		return byId;
	};
	const auto baselineById = indexById(baselineArray);
	const auto representableById = indexById(representableArray);
	Json result = Json::array();
	std::set<std::string> written;
	for (const Json &entry : desiredArray) {
		Json object = entry;
		if (object.is_object() && object.contains("id") && object.at("id").is_string()) {
			const std::string id = object.at("id").get<std::string>();
			written.insert(id);
			const auto baselineEntry = baselineById.find(id);
			const auto representableEntry = representableById.find(id);
			if (baselineEntry != baselineById.end()) {
				// What this build cannot represent of the stored object, at any
				// depth, is carried; an object it left as it was stays verbatim.
				carryUnrepresented(*baselineEntry->second,
					representableEntry != representableById.end() ? *representableEntry->second : Json(), object);
			}
		}
		result.push_back(std::move(object));
	}
	for (const auto &[id, value] : baselineById) {
		if (written.count(id) != 0) continue;
		// An object this build could not even materialise is not a deletion.
		if (representableById.count(id) != 0) continue;
		result.push_back(*value);
	}
	return result;
}

Json preserveUnrepresentedState(const Json &baseline, const Json &representable, const Json &desired) {
	if (!baseline.is_object() || !desired.is_object()) return desired;
	Json merged = desired;
	for (const auto &entry : baseline.items()) {
		const std::string &key = entry.key();
		if (!merged.contains(key)) {
			const bool representedHere = representable.is_object() && representable.contains(key);
			if (!representedHere) merged[key] = entry.value();
			continue;
		}
		if (!entry.value().is_array() || !merged.at(key).is_array()) {
			// A field of the Project itself: what this build cannot represent of
			// it is carried at any depth too.
			carryUnrepresented(entry.value(),
				representable.is_object() && representable.contains(key) ? representable.at(key) : Json(), merged[key]);
			continue;
		}
		merged[key] = preserveCollection(entry.value(),
			representable.is_object() && representable.contains(key) ? representable.at(key) : Json::array(),
			merged.at(key));
	}
	return merged;
}

// A saved baseline, parsed, with what this build can express of it.
struct ParsedBaseline {
	Json state;
	Json representable;
};

std::string parsedBaselineKey(const matters::ProjectStorageBaseline &baseline) {
	std::uint64_t hash = 1469598103934665603ULL;
	for (const char character : baseline.snapshot) {
		hash ^= static_cast<unsigned char>(character);
		hash *= 1099511628211ULL;
	}
	return baseline.project_path + "|" + std::to_string(baseline.version) + "|"
		+ std::to_string(baseline.snapshot.size()) + "|" + std::to_string(hash);
}

struct ParsedBaselines {
	std::mutex mutex;
	std::vector<std::pair<std::string, std::shared_ptr<const ParsedBaseline>>> kept;
};

ParsedBaselines &parsedBaselines() {
	static ParsedBaselines *value = new ParsedBaselines();
	return *value;
}

// A load already holds the state it just replayed and the Project it just
// materialised from it: handing both to the save that follows spares that save
// parsing the same text again and materialising the same Project again.
void rememberParsedBaseline(const matters::ProjectStorageBaseline &baseline,
		std::shared_ptr<const ParsedBaseline> parsed) {
	ParsedBaselines &all = parsedBaselines();
	const std::string key = parsedBaselineKey(baseline);
	const std::lock_guard<std::mutex> lock(all.mutex);
	for (auto &entry : all.kept) if (entry.first == key) { entry.second = std::move(parsed); return; }
	if (all.kept.size() >= 4) all.kept.erase(all.kept.begin());
	all.kept.emplace_back(key, std::move(parsed));
}

// Keyed by the baseline's own text (its location and version alone do not
// identify it: two writers who loaded the same version each hold a different
// next baseline). A few entries cover every Project being worked in.
std::shared_ptr<const ParsedBaseline> parsedBaselineFor(const matters::ProjectStorageBaseline &baseline) {
	ParsedBaselines &all = parsedBaselines();
	const std::string key = parsedBaselineKey(baseline);
	{
		const std::lock_guard<std::mutex> lock(all.mutex);
		for (const auto &entry : all.kept) if (entry.first == key) return entry.second;
	}
	auto parsed = std::make_shared<ParsedBaseline>();
	parsed->state = Json::parse(baseline.snapshot);
	parsed->representable = matters::serialize_matter_storage(matters::deserialize_matter_storage(parsed->state));
	rememberParsedBaseline(baseline, parsed);
	return parsed;
}

// Which Projects were touched, for tests only (project_access_counts_for_testing):
// nothing is recorded until a test asks for it.
struct ProjectAccessCounters {
	std::mutex mutex;
	bool enabled = false;
	std::map<std::string, ProjectAccessCounts> counts;
};

ProjectAccessCounters &projectAccessCounters() {
	static ProjectAccessCounters *counters = new ProjectAccessCounters();
	return *counters;
}

enum class ProjectAccess { Load, FullReplay, Save };

void countProjectAccess(const std::filesystem::path &projectPath, ProjectAccess access) {
	ProjectAccessCounters &counters = projectAccessCounters();
	const std::lock_guard<std::mutex> lock(counters.mutex);
	if (counters.enabled == false) return;
	std::error_code error;
	const std::filesystem::path canonical = std::filesystem::weakly_canonical(projectPath, error);
	ProjectAccessCounts &entry = counters.counts[(error ? projectPath : canonical).u8string()];
	if (access == ProjectAccess::Load) ++entry.loads;
	if (access == ProjectAccess::FullReplay) ++entry.full_replays;
	if (access == ProjectAccess::Save) ++entry.saves;
}

void saveProjectOperation(matters::Matter &project, const platform::ActorIdentity &actor,
		const std::optional<std::string> &resolvedConflictId) {
	if (project.storage_baseline == nullptr || project.storage_baseline->project_path.empty() == true) {
		throw std::runtime_error("Project data has no merge baseline. Open the Project again.");
	}
	countProjectAccess(std::filesystem::u8path(project.storage_baseline->project_path), ProjectAccess::Save);
	platform::trace::Span span("persist", "project.save");
	const std::shared_ptr<matters::ProjectStorageBaseline> priorBaseline = project.storage_baseline;
	// The baseline parsed, and what this build can express of it (see
	// preserveUnrepresentedState - the one honest way to tell "I do not
	// understand this" apart from "this was removed"). Both are functions of
	// the baseline text alone, and an operation saves against the same baseline
	// several times over, so they are worked out once per baseline.
	const std::shared_ptr<const ParsedBaseline> parsed = parsedBaselineFor(*priorBaseline);
	const Json &baseline = parsed->state;
	const Json &representable = parsed->representable;
	span.add("baselineMs", span.elapsed_ms());
	matters::Matter desiredProject = project;
	desiredProject.storage_baseline.reset();
	Json desired = matters::serialize_matter_storage(desiredProject);
	span.add("serializeMs", span.elapsed_ms());
	desired = preserveUnrepresentedState(baseline, representable, desired);
	const Json patch = makePatch(baseline, desired);
	if ((isPatchEmpty(patch) == true || isTimestampOnly(patch) == true) && resolvedConflictId.has_value() == false) {
		span.add("written", false);
		return;
	}
	span.add("written", true);
	span.add("diffMs", span.elapsed_ms());
	if (span.active()) span.add("patchBytes", patch.dump().size());

	const std::string operationId = generateId("op_");
	const std::filesystem::path operationPath =
			std::filesystem::path(priorBaseline->project_path) / ".lm" / "Operations" / (operationId + ".json");
	writeJsonAtomic(operationPath, Json{
			{"schemaVersion", 1},
			{"operationId", operationId},
			{"projectId", project.id},
			{"objectId", project.id},
			{"actorId", actor.actor_id},
			{"authenticatedActor", actor.is_authenticated},
			{"deviceId", actor.device_id},
			{"sessionId", actor.session_id},
			{"timestamp", workspaces::current_timestamp()},
			{"previousVersion", priorBaseline->version},
			{"resultingVersion", priorBaseline->version + 1},
			{"resolvesConflictId", resolvedConflictId ? Json(*resolvedConflictId) : Json(nullptr)},
			{"patch", patch},
	});

	project.storage_baseline = std::make_shared<matters::ProjectStorageBaseline>();
	project.storage_baseline->version = priorBaseline->version + 1;
	project.storage_baseline->snapshot = desired.dump();
	project.storage_baseline->project_path = priorBaseline->project_path;
	// What was written, by its place in the Matter's folder: whoever carries the
	// Matter to the firm's other computers sends it at once, without walking the
	// folder first.
	platform::events::publish("matter.changed",
		Json{{"projectId", project.id}, {"paths", Json::array({".lm/Operations/" + operationId + ".json"})}});
}

// ---------------------------------------------------------------------------
// The replayed Project, kept between loads.
//
// A Project's state is its Base plus every operation file, replayed in order.
// Every read in the app goes through load_project(), and a replay costs what
// the whole log holds - hundreds of milliseconds for a working data room, paid
// under the persistence lock by every screen refresh. So the last replay of
// each Project is kept, and a load first asks the file system whether anything
// changed: the manifest, the Base and every operation file by name, size and
// modification time.
//   - Nothing changed: the kept result is returned. No file is read.
//   - Only new operation files appeared, all ordered after the last one
//     replayed (the normal case after a save): only those are read and
//     replayed on top of the kept state.
//   - Anything else (a file changed or vanished, an operation that sorts into
//     the middle - another device's concurrent write): a full replay, exactly
//     as without the cache.
// The result is therefore always what a full replay of the files on disk
// gives; the cache only decides how much of it must be recomputed. Another
// process or device writing into .lm is seen through the same file stamps.
struct FileStamp {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified{};
  bool operator==(const FileStamp &other) const { return size == other.size && modified == other.modified; }
  bool operator!=(const FileStamp &other) const { return !(*this == other); }
};

std::optional<FileStamp> stampOf(const std::filesystem::path &path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return std::nullopt;
  const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
  if (error) return std::nullopt;
  return FileStamp{size, modified};
}

// Every operation file currently on disk, by file name. nullopt when the
// folder cannot be listed - the caller then takes the uncached path, which
// reports the real problem.
std::optional<std::map<std::string, FileStamp>> operationStamps(const std::filesystem::path &dataPath) {
  std::map<std::string, FileStamp> stamps;
  const std::filesystem::path operations = dataPath / "Operations";
  std::error_code error;
  if (std::filesystem::is_directory(operations, error) == false) return error ? std::nullopt : std::make_optional(stamps);
  for (std::filesystem::directory_iterator entry(operations, error), end; entry != end; entry.increment(error)) {
    if (error) return std::nullopt;
    if (entry->is_regular_file(error) == false || entry->path().extension() != ".json") continue;
    // The size and time the listing itself gave: on Windows no call per file, on
    // macOS and Linux one. Asked by path, two calls per file cost about 90 ms on
    // each read of a Matter of 2,050 operations.
    const std::uintmax_t size = entry->file_size(error);
    if (error) return std::nullopt;
    const std::filesystem::file_time_type modified = entry->last_write_time(error);
    if (error) return std::nullopt;
    stamps.emplace(entry->path().filename().u8string(), FileStamp{size, modified});
  }
  if (error) return std::nullopt;
  return stamps;
}

using OperationOrder = std::pair<long long, std::string>;

OperationOrder orderOf(const Json &operation) {
  return {operation.value("previousVersion", 0LL),
      operation.value("timestamp", std::string()) + operation.value("operationId", std::string())};
}

struct ReplayedProject {
  FileStamp manifest;
  FileStamp base;
  std::map<std::string, FileStamp> operationFiles;
  std::set<std::string> operationIds;
  std::string projectId;
  Json baseState;
  ReplayState replay;
  std::vector<ProjectConflict> conflicts; // every conflict found, resolved ones included
  std::set<std::string> resolvedConflictIds;
  std::map<std::string, std::tuple<std::string, std::string, std::string>> provenance;
  std::optional<OperationOrder> lastOrder;
  long long operationCount = 0;
  // Operation files not replayed because they could not be read.
  std::vector<std::string> unreadableOperations;
  ProjectRecord record;
};

void replayOperation(ReplayedProject &project, const Json &operation) {
  if (operation.value("projectId", std::string()) != project.projectId) {
    throw std::runtime_error("Project operation belongs to another Project.");
  }
  const std::string resolvedConflictId = operation.contains("resolvesConflictId")
      && operation.at("resolvesConflictId").is_string()
    ? operation.at("resolvesConflictId").get<std::string>()
    : std::string();
  if (resolvedConflictId.empty() == false) project.resolvedConflictIds.insert(resolvedConflictId);
  applyPatch(project.replay, operation.at("patch"), operation.value("operationId", std::string()),
      operation.value("actorId", std::string()), operation.value("timestamp", std::string()),
      project.conflicts, project.provenance);
  project.lastOrder = orderOf(operation);
  ++project.operationCount;
}

// Builds the record callers receive from the replayed state.
void buildRecord(ReplayedProject &project, const std::filesystem::path &projectPath) {
  platform::trace::Span span("persist", "project.build");
  Json state = project.replay.fields;
  for (const auto &[collection, values] : project.replay.collections)
    state[collection] = project.replay.touched.count(collection) != 0
        ? arrayByStableOrder(values, project.replay.appearance[collection])
        : project.baseState.value(collection, Json::array());
  span.add("stateMs", span.elapsed_ms());
  std::vector<ProjectConflict> conflicts = project.conflicts;
  conflicts.erase(std::remove_if(conflicts.begin(), conflicts.end(), [&](const ProjectConflict &conflict) {
    return project.resolvedConflictIds.count(conflict.id) > 0;
  }), conflicts.end());

  ProjectRecord record;
  // What this build can express of the state is taken from the Project while
  // it is still exactly what the state materialised to - before anything
  // about this load (its folder) is added - so it is the same value a save
  // would work out from the baseline text (parsedBaselineFor).
  auto parsed = std::make_shared<ParsedBaseline>();
  record.project = matters::deserialize_matter_storage(state);
  span.add("deserializeMs", span.elapsed_ms());
  parsed->representable = matters::serialize_matter_storage(record.project);
  span.add("serializeMs", span.elapsed_ms());
  // Who added each document, derived after what a save would store is taken,
  // so it is never written back.
  const auto writers = project.replay.firstWriter.find("documents");
  if (writers != project.replay.firstWriter.end()) {
    for (matters::MatterDocument &document : record.project.documents) {
      const auto writer = writers->second.find(document.id);
      if (writer != writers->second.end()) document.added_by_actor_id = writer->second;
    }
  }
  record.project.directory_path = std::filesystem::weakly_canonical(projectPath).u8string();
  record.path = std::filesystem::weakly_canonical(projectPath);
  record.operation_count = project.operationCount;
  record.conflicts = std::move(conflicts);
  record.conflict_count = static_cast<long long>(record.conflicts.size());
  record.unreadable_operations = project.unreadableOperations;
  record.project.storage_baseline = std::make_shared<matters::ProjectStorageBaseline>();
  record.project.storage_baseline->version = record.operation_count;
  record.project.storage_baseline->snapshot = state.dump();
  span.add("dumpMs", span.elapsed_ms());
  if (span.active()) span.add("snapshotBytes", record.project.storage_baseline->snapshot.size());
  record.project.storage_baseline->project_path = record.path.u8string();
  parsed->state = std::move(state);
  rememberParsedBaseline(*record.project.storage_baseline, std::move(parsed));
  project.record = std::move(record);
}

// The complete, uncached load: verifies the manifest and the Base checksum,
// reads every operation and replays them all.
ReplayedProject replayFromDisk(const std::filesystem::path &projectPath) {
  const std::filesystem::path dataPath = project_data_path(projectPath);
  const Json manifest = readJson(dataPath / "Project.json");
  const int storedSchemaVersion = manifest.value("schemaVersion", 0);
  if (storedSchemaVersion > 1) {
    throw ProjectDataRequiresNewerVersion(
        "This Project's data was saved by a newer version of Singularity. Update Singularity to open it - its data is "
        "fine and nothing here needs to be repaired.");
  }
  if (storedSchemaVersion != 1) {
    throw std::runtime_error("This Project uses an unsupported .lm version.");
  }
  const std::filesystem::path basePath = dataPath / "State" / "Base.json";
  const std::string expectedBaseHash = manifest.value("baseHash", std::string());
  // Read once: the bytes checked are the bytes parsed. Checked and then read
  // again, a Base a sync client put down in between was taken for the
  // Project's.
  const std::string baseBytes = readBytes(basePath);
  if (expectedBaseHash.empty() == true || platform::sha256_hex_digest(baseBytes) != expectedBaseHash) {
    throw std::runtime_error(
        "This Project's saved state does not match its recorded checksum. Restore .lm from a backup.");
  }
  if (afterBaseChecked()) afterBaseChecked()();
  ReplayedProject project;
  project.baseState = Json::parse(baseBytes);
  project.projectId = project.baseState.value("id", std::string());
  if (project.projectId != manifest.value("projectId", std::string())) {
    throw std::runtime_error("Project identity does not match its .lm state.");
  }
  // An id is joined to paths on this computer (the Project's own copies, its
  // kept versions): one a folder's .lm could carry as "/Users/someone" or
  // "../.." is damaged data, never a Project. The same rule as
  // matters::is_valid_matter_id(), which this layer does not link.
  if (project.projectId.empty() || project.projectId.size() > 128
      || !std::all_of(project.projectId.begin(), project.projectId.end(), [](char character) {
           return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
               || (character >= '0' && character <= '9') || character == '_' || character == '-';
         })) {
    throw std::runtime_error("This Project's identity in its .lm data is not valid.");
  }
  project.replay.fields = project.baseState;
  for (const char *collection : collectionNames) {
    const Json baseValues = project.baseState.value(collection, Json::array());
    project.replay.collections[collection] = objectById(baseValues);
    std::map<std::string, std::uint64_t> &appeared = project.replay.appearance[collection];
    for (const Json &value : baseValues) {
      const std::string id = value.is_object() ? value.value("id", std::string()) : std::string();
      if (!id.empty() && appeared.count(id) == 0) appeared.emplace(id, project.replay.nextAppearance++);
    }
    project.replay.fields.erase(collection);
  }
  for (const StoredOperation &stored : readOperations(dataPath, &project.unreadableOperations)) {
    project.operationIds.insert(stored.value.value("operationId", std::string()));
    replayOperation(project, stored.value);
  }
  std::sort(project.unreadableOperations.begin(), project.unreadableOperations.end());
  if (!project.unreadableOperations.empty())
    platform::trace::record("persist", "project.unreadable_operations",
        {{"count", std::to_string(project.unreadableOperations.size())}});
  buildRecord(project, projectPath);
  return project;
}

// The steps a read of a Project replayed that a later read of the same
// Project, on the same Base, could not replay again - a file a sync client is
// rewriting in place, or has taken away for a moment - and since when.
struct MissingSteps {
  FileStamp base;
  std::set<std::string> operationIds;
  std::chrono::steady_clock::time_point since;
};

struct ProjectCache {
  std::mutex mutex;
  std::map<std::string, ReplayedProject> projects;
  // The Matters the last search of the member's folders found: kept beside
  // the listed ones, so the next search reads only what changed in them.
  std::set<std::string> found;
  std::map<std::string, MissingSteps> missing;
  // A sync client rewrites a small file in well under it.
  std::chrono::milliseconds unreadableStepWindow = std::chrono::minutes(2);
};

ProjectCache &projectCache() {
  static ProjectCache *cache = new ProjectCache();
  return *cache;
}

// Replays only the operation files that appeared since `kept` was built, when
// that is provably the same as replaying everything. False means a full
// replay is needed; `kept` may then be partly advanced and must be discarded.
bool replayNewOperations(ReplayedProject &kept, const std::filesystem::path &projectPath,
    const std::map<std::string, FileStamp> &current, std::size_t &replayedOut) {
  for (const auto &[name, stamp] : kept.operationFiles) {
    const auto found = current.find(name);
    if (found == current.end() || found->second != stamp) return false;
  }
  std::vector<Json> fresh;
  std::vector<std::string> unreadable;
  const std::filesystem::path operations = project_data_path(projectPath) / "Operations";
  for (const auto &[name, stamp] : current) {
    static_cast<void>(stamp);
    if (kept.operationFiles.count(name) != 0) continue;
    std::optional<Json> read = readOperationFile(operations / std::filesystem::u8path(name));
    if (!read) {
      unreadable.push_back(name);
      continue;
    }
    Json value = std::move(*read);
    const std::string operationId = value.value("operationId", std::string());
    // A sync client's copy of an operation already replayed changes nothing.
    if (!operationId.empty() && kept.operationIds.count(operationId) != 0) continue;
    fresh.push_back(std::move(value));
  }
  std::sort(fresh.begin(), fresh.end(), [](const Json &left, const Json &right) { return orderOf(left) < orderOf(right); });
  // An operation that sorts at or before one already replayed would have been
  // replayed earlier by a full load: only a full load gives its result.
  if (!fresh.empty() && kept.lastOrder && !(*kept.lastOrder < orderOf(fresh.front()))) return false;
  std::set<std::string> freshIds;
  for (const Json &operation : fresh) {
    const std::string operationId = operation.value("operationId", std::string());
    if (!operationId.empty() && freshIds.insert(operationId).second == false) continue;
    replayOperation(kept, operation);
    if (!operationId.empty()) kept.operationIds.insert(operationId);
  }
  replayedOut = fresh.size();
  // A file read is kept by its size and time; one that could not be read is
  // not, so every later load reads it again whatever its size and time say:
  // a sync client may finish a file without changing either.
  kept.operationFiles = current;
  for (const std::string &name : unreadable) kept.operationFiles.erase(name);
  kept.unreadableOperations = std::move(unreadable);
  if (fresh.empty() == false) buildRecord(kept, projectPath);
  else kept.record.unreadable_operations = kept.unreadableOperations;
  return true;
}

} // namespace

std::filesystem::path project_data_path(const std::filesystem::path &projectPath) {
  return projectPath / ".lm";
}

bool has_project_data(const std::filesystem::path &projectPath) {
  std::error_code error;
  return std::filesystem::is_regular_file(project_data_path(projectPath) / "Project.json", error);
}

ProjectDataPresence probe_project_data(const std::filesystem::path &projectPath) {
  const auto denied = [](const std::error_code &error) {
    return error == std::errc::permission_denied || error == std::errc::operation_not_permitted;
  };
  std::error_code error;
  const std::filesystem::path manifest = project_data_path(projectPath) / "Project.json";
  if (std::filesystem::is_regular_file(manifest, error)) {
    // Present in the listing is not the same as readable: opening it is the
    // only answer that distinguishes lost access from real data.
    std::ifstream file(manifest, std::ios::binary);
    if (file.is_open()) return ProjectDataPresence::Present;
    return (errno == EACCES || errno == EPERM) ? ProjectDataPresence::AccessDenied : ProjectDataPresence::Present;
  }
  if (denied(error)) return ProjectDataPresence::AccessDenied;
  // The manifest is not listed. Only a folder this process can actually list
  // proves it is absent rather than hidden from us.
  std::error_code listError;
  std::filesystem::directory_iterator iterator(project_data_path(projectPath), listError);
  if (denied(listError)) return ProjectDataPresence::AccessDenied;
  std::error_code projectListError;
  std::filesystem::directory_iterator projectIterator(projectPath, projectListError);
  if (denied(projectListError)) return ProjectDataPresence::AccessDenied;
  return ProjectDataPresence::Absent;
}

ProjectRecord load_project(const std::filesystem::path &projectPath) {
  platform::trace::Span span("persist", "project.load");
  countProjectAccess(projectPath, ProjectAccess::Load);
  const std::filesystem::path dataPath = project_data_path(projectPath);
  const auto manifestStamp = stampOf(dataPath / "Project.json");
  const auto baseStamp = stampOf(dataPath / "State" / "Base.json");
  const auto operationFiles = operationStamps(dataPath);
  // Anything that cannot even be listed takes the complete load, which reports
  // what is actually wrong (missing, denied, damaged) exactly as before.
  if (!manifestStamp || !baseStamp || !operationFiles) {
    span.add("path", "uncached");
    countProjectAccess(projectPath, ProjectAccess::FullReplay);
    return replayFromDisk(projectPath).record;
  }

  ProjectCache &cache = projectCache();
  const std::string key = dataPath.u8string();
  std::unique_lock<std::mutex> lock(cache.mutex);
  // The steps the last read of this Project replayed, on the same Base: a
  // complete read replays every one of them again, or is refused for a while.
  std::set<std::string> replayedBefore;
  const auto kept = cache.projects.find(key);
  if (kept != cache.projects.end() && kept->second.manifest == *manifestStamp && kept->second.base == *baseStamp) {
    if (kept->second.operationFiles == *operationFiles) {
      span.add("path", "kept");
      span.add("operations", kept->second.operationCount);
      return kept->second.record;
    }
    try {
      std::size_t replayed = 0;
      if (replayNewOperations(kept->second, projectPath, *operationFiles, replayed)) {
        span.add("path", "incremental");
        span.add("operations", kept->second.operationCount);
        span.add("newOperations", replayed);
        return kept->second.record;
      }
    } catch (const std::exception &) {
      // Whatever went wrong, the complete load below decides what to report.
    }
    replayedBefore = std::move(kept->second.operationIds);
    cache.projects.erase(kept);
  } else if (kept != cache.projects.end()) {
    if (kept->second.base == *baseStamp) replayedBefore = std::move(kept->second.operationIds);
    cache.projects.erase(kept);
  }
  const auto missing = cache.missing.find(key);
  if (missing != cache.missing.end()) {
    // A read refused a moment ago still knows what it should replay.
    if (missing->second.base != *baseStamp) cache.missing.erase(missing);
    else if (replayedBefore.empty()) replayedBefore = missing->second.operationIds;
  }

  // A complete replay reads every operation file of the Project and takes
  // seconds for a large one: it runs outside the cache's lock, so a read of
  // any other Project - kept, under the store's own lock - never waits behind
  // it. Two replays of one Project at once read the same files; the later one
  // kept is as good. The stamps were taken before the files are read, so they
  // are never newer than what is read: a file that changes during the read
  // makes the next load read again. Only a file that cannot be read is not
  // kept by its stamp.
  lock.unlock();
  countProjectAccess(projectPath, ProjectAccess::FullReplay);
  ReplayedProject replayed = replayFromDisk(projectPath);
  replayed.manifest = *manifestStamp;
  replayed.base = *baseStamp;
  replayed.operationFiles = *operationFiles;
  for (const std::string &name : replayed.unreadableOperations) replayed.operationFiles.erase(name);
  std::size_t lost = 0;
  for (const std::string &operationId : replayedBefore)
    if (replayed.operationIds.count(operationId) == 0) ++lost;
  span.add("path", "full");
  span.add("operations", replayed.operationCount);
  span.add("snapshotBytes", replayed.record.project.storage_baseline->snapshot.size());
  // Every Project the workspace lists stays kept: Home reads them all
  // together, so any bound below their number would replay every Project on
  // every such read (it did, at more than 16). What the workspace no longer
  // lists, and the last search of the folders did not find, is let go by
  // keep_only_projects().
  ProjectRecord record = replayed.record;
  lock.lock();
  if (lost > 0) {
    // A step this computer read is unreadable or gone now: read without it,
    // the Matter would lose what that step holds - its threads among them -
    // and a list would come back short.
    const auto now = std::chrono::steady_clock::now();
    const auto [entry, isNew] = cache.missing.try_emplace(key);
    if (isNew) entry->second = {*baseStamp, std::move(replayedBefore), now};
    if (now - entry->second.since < cache.unreadableStepWindow) {
      platform::trace::record("persist", "project.steps_missing", {{"count", std::to_string(lost)}});
      throw std::runtime_error(
          "Part of this Project's saved data can't be read just now. Singularity reads it again by itself.");
    }
    platform::trace::record("persist", "project.opened_without_steps", {{"count", std::to_string(lost)}});
  }
  cache.missing.erase(key);
  cache.projects.insert_or_assign(key, std::move(replayed));
  return record;
}

void forget_kept_projects_for_testing() {
  ProjectCache &cache = projectCache();
  const std::lock_guard<std::mutex> lock(cache.mutex);
  cache.projects.clear();
  cache.found.clear();
  cache.missing.clear();
}

void keep_only_projects(const std::vector<std::filesystem::path> &projectPaths) {
  std::set<std::string> listed;
  for (const std::filesystem::path &projectPath : projectPaths) listed.insert(project_data_path(projectPath).u8string());
  ProjectCache &cache = projectCache();
  const std::lock_guard<std::mutex> lock(cache.mutex);
  for (auto kept = cache.projects.begin(); kept != cache.projects.end();) {
    if (listed.count(kept->first) == 0 && cache.found.count(kept->first) == 0) kept = cache.projects.erase(kept);
    else ++kept;
  }
  for (auto missing = cache.missing.begin(); missing != cache.missing.end();) {
    if (listed.count(missing->first) == 0 && cache.found.count(missing->first) == 0) missing = cache.missing.erase(missing);
    else ++missing;
  }
}

std::map<std::string, ProjectAccessCounts> project_access_counts_for_testing() {
  ProjectAccessCounters &counters = projectAccessCounters();
  const std::lock_guard<std::mutex> lock(counters.mutex);
  return counters.counts;
}

void reset_project_access_counts_for_testing() {
  ProjectAccessCounters &counters = projectAccessCounters();
  const std::lock_guard<std::mutex> lock(counters.mutex);
  counters.enabled = true;
  counters.counts.clear();
}

ProjectRecord initialize_project(const std::filesystem::path &projectPath, const matters::Matter &project,
    const platform::ActorIdentity &actor, const std::optional<std::filesystem::path> &legacySource) {
  if (has_project_data(projectPath) == true) {
    ProjectRecord existing = load_project(projectPath);
    // A folder's Singularity data belongs to exactly one Project: setting up
    // another Project there never silently takes over that data.
    if (existing.project.id != project.id)
      throw std::runtime_error("This folder already holds another Project's Singularity data.");
    return existing;
  }
  std::error_code error;
  if (std::filesystem::is_directory(projectPath, error) == false) {
    throw std::runtime_error("This Project folder is no longer available.");
  }

  const std::filesystem::path finalPath = project_data_path(projectPath);
  const std::filesystem::path stagingPath = projectPath / (".lm.migrating-" + generateId(""));
  try {
    createProjectDirectories(stagingPath);
    std::filesystem::create_directories(stagingPath / "State");
    matters::Matter portable = project;
    portable.directory_path = std::filesystem::weakly_canonical(projectPath).u8string();
    portable.storage_baseline.reset();
    const Json state = matters::serialize_matter_storage(portable);
    writeJson(stagingPath / "State" / "Base.json", state);
    const std::string baseHash = platform::sha256_hex_digest_of_file(stagingPath / "State" / "Base.json");
    // Initialization is itself a persisted, user-originated mutation - the
    // very first one - so it carries the same actor/device/session/operation
    // attribution as every later save_project() operation, not just an
    // actorId. See the operation object below for the equivalent fields.
    writeJson(stagingPath / "Project.json", Json{
        {"schemaVersion", 1},
        {"projectId", portable.id},
        {"createdAt", portable.created_at},
        {"migratedAt", workspaces::current_timestamp()},
        {"baseHash", baseHash},
        {"operationId", generateId("op_")},
        {"actorId", actor.actor_id},
        {"authenticatedActor", actor.is_authenticated},
        {"deviceId", actor.device_id},
        {"sessionId", actor.session_id},
        {"legacySource", legacySource.has_value() == true ? Json(legacySource->u8string()) : Json(nullptr)},
    });
    const Json verifiedManifest = readJson(stagingPath / "Project.json");
    const Json verifiedState = readJson(stagingPath / "State" / "Base.json");
    if (verifiedManifest.value("projectId", std::string()) != portable.id || verifiedState != state ||
        platform::sha256_hex_digest_of_file(stagingPath / "State" / "Base.json") != baseHash) {
      throw std::runtime_error("Singularity could not verify the new Project data.");
    }
    atomicReplace(stagingPath, finalPath);
  } catch (...) {
    std::error_code cleanupError;
    std::filesystem::remove_all(stagingPath, cleanupError);
    throw;
  }
  return load_project(projectPath);
}

void save_project(matters::Matter &project, const platform::ActorIdentity &actor) {
	saveProjectOperation(project, actor, std::nullopt);
}

void save_project_conflict_resolution(
		matters::Matter &project, const platform::ActorIdentity &actor, const std::string &conflictId) {
	if (conflictId.empty() == true) throw std::runtime_error("A conflict is required.");
	saveProjectOperation(project, actor, conflictId);
}

// Discovery only ever looks for .lm directories, never user document content:
// collectProjectCandidates() above filters to directory entries before it
// even looks at a name (is_directory() == false -> skipped), so a .docx/
// .pdf/.xlsx is never inspected, indexed or treated as discovery evidence -
// that is a separate, later feature. has_project_data(candidate) below is
// the only thing that decides a hit, by checking candidate/.lm/Project.json;
// candidate - .lm's immediate parent - becomes the Project root purely
// because that check passed, never because of its own name. Identity and
// restoration then come from .lm's own metadata (load_project() reads
// Project.json/State/Base.json), not from candidate.filename() - the one
// exception is the catch block below, which uses the folder name only as a
// last-resort DISPLAY label for a .lm that failed to load (id stays empty,
// status "needs-attention"), never as identity to match or restore against.
std::vector<std::filesystem::path> find_project_folders(const std::vector<std::filesystem::path> &roots) {
  std::set<std::string> seenPaths;
  std::vector<std::filesystem::path> folders;
  for (const std::filesystem::path &root : roots) {
    std::error_code error;
    if (std::filesystem::is_directory(root, error) == false) continue;
    // The whole file system is never walked: macOS lists its startup disk in
    // /Volumes as the root itself, and the home folder covers the person's own
    // files on it.
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(root, error);
    if (!error && resolved == resolved.root_path()) continue;
    std::vector<std::filesystem::path> candidates;
    collectProjectCandidates(root, candidates);
    for (const std::filesystem::path &candidate : candidates) {
      if (has_project_data(candidate) == false) continue;
      const std::string canonical = std::filesystem::weakly_canonical(candidate).u8string();
      if (seenPaths.insert(canonical).second == false) continue;
      folders.push_back(candidate);
    }
  }
  return folders;
}

std::vector<ProjectSummary> summarize_project_folders(const std::vector<std::filesystem::path> &folders) {
  // What this search finds stays kept until the next search, so a search that
  // runs twice a minute reads only what changed (keep_only_projects()).
  {
    ProjectCache &cache = projectCache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.found.clear();
    for (const std::filesystem::path &candidate : folders) cache.found.insert(project_data_path(candidate).u8string());
  }
  std::vector<ProjectSummary> result;
  for (const std::filesystem::path &candidate : folders) {
    try {
      const ProjectRecord record = load_project(candidate);
      result.push_back(ProjectSummary{
          record.project.id,
          locationId(record.path),
          record.project.name,
          record.path.u8string(),
          record.conflict_count > 0 ? "needs-attention" : "ready",
          record.operation_count,
          record.conflict_count,
      });
    } catch (const std::exception &) {
      result.push_back(ProjectSummary{
          "", locationId(candidate), candidate.filename().u8string(), std::filesystem::weakly_canonical(candidate).u8string(),
          "needs-attention", 0, 1,
      });
    }
  }

  // Two (or more) authorised locations sharing one Project id are a real
  // copy/shared-drive duplicate, not one location that "moved" - never
  // deduplicated away, and explicitly marked on every one of those entries
  // so a caller cannot mistake any single one of them for the sole
  // canonical location without inspecting the rest of this result.
  std::map<std::string, int> idCounts;
  for (const ProjectSummary &summary : result) {
    if (summary.id.empty() == true) continue;
    ++idCounts[summary.id];
  }
  for (ProjectSummary &summary : result) {
    if (summary.id.empty() == true) continue;
    summary.has_duplicate_location = idCounts[summary.id] > 1;
  }

  std::sort(result.begin(), result.end(), [](const ProjectSummary &left, const ProjectSummary &right) {
    if (left.name != right.name) return left.name < right.name;
    return left.path < right.path;
  });
  return result;
}

std::vector<ProjectSummary> discover_projects(const std::vector<std::filesystem::path> &roots) {
  return summarize_project_folders(find_project_folders(roots));
}

void reveal_project_data(const std::filesystem::path &projectPath) {
  const std::filesystem::path dataPath = project_data_path(projectPath);
  if (has_project_data(projectPath) == false || platform::open_directory_in_file_manager(dataPath.u8string()) == false) {
    throw std::runtime_error("Singularity could not reveal this Project’s data.");
  }
}

std::filesystem::path prepare_clean_copy(
    const std::filesystem::path &projectPath, const std::filesystem::path &destinationDirectory) {
  if (std::filesystem::equivalent(projectPath, destinationDirectory) == true) {
    throw std::runtime_error("Choose a different folder for the clean copy.");
  }
  std::filesystem::path destination = destinationDirectory / (projectPath.filename().u8string() + " — clean copy");
  if (std::filesystem::exists(destination) == true) {
    destination += " " + generateId("").substr(0, 6);
  }
  try {
    copyWithoutProjectData(projectPath, destination);
  } catch (...) {
    std::error_code cleanupError;
    std::filesystem::remove_all(destination, cleanupError);
    throw;
  }
  return destination;
}

void remove_project_data(const std::filesystem::path &projectPath) {
  const std::filesystem::path dataPath = project_data_path(projectPath);
  {
    // Nothing kept from this data may outlive it.
    ProjectCache &cache = projectCache();
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.projects.erase(dataPath.u8string());
  }
  if (has_project_data(projectPath) == false) throw std::runtime_error("This Project has no Singularity data.");
  std::error_code error;
  const std::uintmax_t removed = std::filesystem::remove_all(dataPath, error);
  if (error || removed == 0) throw std::runtime_error("Singularity could not remove this Project’s data.");
  removeLegacyDocumentBackups(projectPath, 0);
}

void set_available_space_for_testing(std::optional<std::uintmax_t> bytes) {
  platform::available_space_override_for_testing() = bytes;
}

void set_after_base_checked_for_testing(std::function<void()> told) {
  afterBaseChecked() = std::move(told);
}

std::chrono::milliseconds set_unreadable_step_window_for_testing(std::chrono::milliseconds window) {
  ProjectCache &cache = projectCache();
  const std::lock_guard<std::mutex> lock(cache.mutex);
  const std::chrono::milliseconds replaced = cache.unreadableStepWindow;
  cache.unreadableStepWindow = window;
  return replaced;
}

} // namespace singularity::persistence
