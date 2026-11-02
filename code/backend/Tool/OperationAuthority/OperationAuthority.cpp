#include "Tool/OperationAuthority/OperationAuthority.hpp"

#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>

namespace singularity::tools {
namespace {

using Json = nlohmann::json;

// One record per operation; every read-modify-write holds this.
std::mutex &authority_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::filesystem::path record_path(const std::string &project_id, const std::string &operation_id) {
  return operation_authority_directory() / (platform::sha256_hex_digest(project_id + "\n" + operation_id) + ".json");
}

std::optional<Json> read_record(const std::string &project_id, const std::string &operation_id) {
  std::ifstream input(record_path(project_id, operation_id), std::ios::binary);
  if (!input) return std::nullopt;
  const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  Json record = Json::parse(bytes, nullptr, false);
  if (!record.is_object() || record.value("projectId", std::string()) != project_id
      || record.value("operationId", std::string()) != operation_id)
    return std::nullopt;
  return record;
}

// Written whole, readable by this computer's person only.
void write_private(const std::filesystem::path &path, const Json &record) {
  std::filesystem::create_directories(path.parent_path());
  platform::restrict_to_owner(path.parent_path());
  const std::filesystem::path staged = platform::staged_sibling_path(path);
  try {
    platform::write_file_durably(staged, record.dump());
    platform::restrict_to_owner(staged);
    platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
  } catch (...) {
    platform::discard_staged_file(staged);
    throw;
  }
}

void write_record(const std::string &project_id, const std::string &operation_id, const Json &record) {
  write_private(record_path(project_id, operation_id), record);
}

Json binding_json(const ApprovalBinding &binding) {
  return Json{{"approvalId", binding.approval_id}, {"toolId", binding.tool_id},
      {"intentDigest", binding.intent_digest}, {"proposalDigest", binding.proposal_digest},
      {"targetDocumentId", binding.target_document_id ? Json(*binding.target_document_id) : Json(nullptr)},
      {"targetVersion", binding.target_version ? Json(*binding.target_version) : Json(nullptr)},
      {"targetFingerprint", binding.target_fingerprint ? Json(*binding.target_fingerprint) : Json(nullptr)},
      {"newDocument", binding.new_document ? Json(*binding.new_document) : Json(nullptr)}};
}

// The same change: every field it binds, exactly. An empty content digest binds
// nothing and never matches.
bool same_change(const Json &recorded, const ApprovalBinding &binding) {
  if (binding.proposal_digest.empty()) return false;
  const Json expected = binding_json(binding);
  for (const char *key : {"approvalId", "toolId", "intentDigest", "proposalDigest", "targetDocumentId",
           "targetVersion", "targetFingerprint", "newDocument"})
    if (!recorded.contains(key) || recorded.at(key) != expected.at(key)) return false;
  return true;
}

Json *prepared_change(Json &record, const std::string &approval_id) {
  if (!record.contains("approvals") || !record.at("approvals").is_array()) return nullptr;
  for (Json &entry : record.at("approvals"))
    if (entry.is_object() && entry.value("approvalId", std::string()) == approval_id) return &entry;
  return nullptr;
}

Json new_record(const std::string &project_id, const std::string &operation_id) {
  return Json{{"projectId", project_id}, {"operationId", operation_id}, {"approvals", Json::array()},
      {"generated", Json::array()}, {"mutations", Json::array()}};
}

// Adds `digest` to the record's list `key`, once.
void note_in_list(const std::string &project_id, const std::string &operation_id, const char *key,
    const std::string &digest) {
  if (project_id.empty() || operation_id.empty() || digest.empty()) return;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  Json record = read_record(project_id, operation_id).value_or(new_record(project_id, operation_id));
  if (!record.contains(key) || !record.at(key).is_array()) record[key] = Json::array();
  Json &list = record.at(key);
  if (std::find(list.begin(), list.end(), Json(digest)) != list.end()) return;
  list.push_back(digest);
  write_record(project_id, operation_id, record);
}

bool listed(const std::string &project_id, const std::string &operation_id, const char *key,
    const std::string &digest) {
  if (digest.empty()) return false;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  const std::optional<Json> record = read_record(project_id, operation_id);
  if (!record || !record->contains(key) || !record->at(key).is_array()) return false;
  const Json &list = record->at(key);
  return std::find(list.begin(), list.end(), Json(digest)) != list.end();
}

} // namespace

ApprovalBinding approval_binding_of_checkpoint(const Json &draft) {
  ApprovalBinding binding;
  if (!draft.is_object() || !draft.contains("approval") || !draft.at("approval").is_object()) return binding;
  const Json &approval = draft.at("approval");
  const auto text = [&](const char *key) -> std::optional<std::string> {
    if (!approval.contains(key) || !approval.at(key).is_string()) return std::nullopt;
    return approval.at(key).get<std::string>();
  };
  binding.approval_id = text("approvalId").value_or(std::string());
  binding.tool_id = text("toolId").value_or(std::string());
  binding.intent_digest = text("intentDigest").value_or(std::string());
  if (draft.contains("rawProviderResult") && draft.at("rawProviderResult").is_string())
    binding.proposal_digest = platform::sha256_hex_digest(draft.at("rawProviderResult").get<std::string>());
  binding.target_document_id = text("targetDocumentId");
  if (approval.contains("targetVersion") && approval.at("targetVersion").is_number_integer())
    binding.target_version = approval.at("targetVersion").get<int>();
  binding.target_fingerprint = text("targetFingerprint");
  binding.new_document = text("newDocument");
  return binding;
}

SeenOperations note_seen_operations(const std::string &project_id, const std::vector<std::string> &unsigned_now,
    const std::vector<std::string> &kept_here_now) {
  SeenOperations seen;
  if (project_id.empty()) return seen;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  const std::filesystem::path path = operation_authority_directory()
      / ("seen-" + platform::sha256_hex_digest(project_id) + ".json");
  Json record;
  {
    std::ifstream input(path, std::ios::binary);
    if (input) {
      const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      record = Json::parse(bytes, nullptr, false);
    }
  }
  const bool firstLook = !record.is_object() || record.value("projectId", std::string()) != project_id
      || !record.contains("earlier") || !record.at("earlier").is_array();
  bool changed = firstLook;
  if (firstLook) {
    // A record that does not read is a first look only if there is none: one
    // that is damaged keeps nothing earlier, so nothing unsigned is taken.
    const bool damaged = std::filesystem::exists(path);
    record = Json{{"projectId", project_id}, {"earlier", damaged ? Json::array() : Json(unsigned_now)},
        {"keptHere", Json::array()}};
  }
  for (const Json &id : record.at("earlier"))
    if (id.is_string()) seen.earlier.insert(id.get<std::string>());
  if (record.contains("keptHere") && record.at("keptHere").is_array())
    for (const Json &id : record.at("keptHere"))
      if (id.is_string()) seen.kept_here.insert(id.get<std::string>());
  for (const std::string &id : kept_here_now)
    if (seen.kept_here.insert(id).second) changed = true;
  if (changed) {
    record["keptHere"] = Json(seen.kept_here);
    write_private(path, record);
  }
  return seen;
}

std::filesystem::path operation_authority_directory() {
  return platform::computer_records_directory() / "Operation Authority";
}

void begin_operation_authority(const std::string &project_id, const std::string &operation_id,
    const OperationAuthority &authority) {
  if (project_id.empty() || operation_id.empty()) return;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  const std::optional<Json> record = read_record(project_id, operation_id);
  Json written = record ? *record : new_record(project_id, operation_id);
  if (record && record->contains("origin")) {
    // Only ever narrowed: an agent's stays an agent's, a ceiling is only
    // lowered, work kept on this computer stays here.
    const bool wasAgent = record->value("origin", std::string()) == "agent";
    const ProjectActionMode had = project_action_mode_from_string(record->value("ceiling", std::string()))
        .value_or(ProjectActionMode::AskForApproval);
    const ProjectActionMode ceiling = lower_of(had, authority.ceiling);
    const bool becomesAgent = !wasAgent && authority.origin == OperationOrigin::Agent;
    const bool becomesLocal = authority.local_only && !record->value("localOnly", false);
    if (ceiling == had && !becomesAgent && !becomesLocal) return;
    if (becomesAgent) {
      written["origin"] = "agent";
      written["agentId"] = authority.agent_id;
    }
    if (becomesLocal) written["localOnly"] = true;
    written["ceiling"] = to_string(ceiling);
    write_record(project_id, operation_id, written);
    return;
  }
  written["origin"] = authority.origin == OperationOrigin::Agent ? "agent" : "person";
  written["agentId"] = authority.agent_id;
  written["actorId"] = authority.actor_id;
  written["ceiling"] = to_string(authority.ceiling);
  written["localOnly"] = authority.local_only;
  write_record(project_id, operation_id, written);
}

std::optional<OperationAuthority> operation_authority(const std::string &project_id,
    const std::string &operation_id) {
  if (project_id.empty() || operation_id.empty()) return std::nullopt;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  const std::optional<Json> record = read_record(project_id, operation_id);
  if (!record || !record->contains("origin")) return std::nullopt;
  OperationAuthority authority;
  authority.origin = record->value("origin", std::string()) == "agent" ? OperationOrigin::Agent
      : OperationOrigin::Person;
  authority.agent_id = record->value("agentId", std::string());
  authority.actor_id = record->value("actorId", std::string());
  // A ceiling that does not read as one is the lowest.
  authority.ceiling = project_action_mode_from_string(record->value("ceiling", std::string()))
      .value_or(ProjectActionMode::AskForApproval);
  // Anything but a plain false keeps the work here.
  const auto localOnly = record->find("localOnly");
  authority.local_only = localOnly != record->end() && !(localOnly->is_boolean() && !localOnly->get<bool>());
  return authority;
}

ProjectActionMode lower_of(ProjectActionMode left, ProjectActionMode right) {
  const auto rank = [](ProjectActionMode mode) {
    switch (mode) {
      case ProjectActionMode::AskForApproval: return 0;
      case ProjectActionMode::ApproveSafeActions: return 1;
      case ProjectActionMode::FullProjectAccess: return 2;
    }
    return 0;
  };
  return rank(left) <= rank(right) ? left : right;
}

ProjectActionPermissionState within_ceiling(ProjectActionPermissionState permission, ProjectActionMode ceiling,
    const std::string &reason) {
  if (lower_of(permission.effective_mode, ceiling) == permission.effective_mode) return permission;
  permission.effective_mode = ceiling;
  permission.is_capped = true;
  permission.cap_reason = reason;
  return permission;
}

void note_generated_here(const std::string &project_id, const std::string &operation_id,
    const std::string &content_digest) {
  note_in_list(project_id, operation_id, "generated", content_digest);
}

bool generated_here(const std::string &project_id, const std::string &operation_id,
    const std::string &content_digest) {
  return listed(project_id, operation_id, "generated", content_digest);
}

void note_checkpoint_kept(const std::string &project_id, const std::string &operation_id,
    const std::string &checkpoint_digest) {
  if (project_id.empty() || operation_id.empty() || checkpoint_digest.empty()) return;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  Json record = read_record(project_id, operation_id).value_or(new_record(project_id, operation_id));
  if (record.value("checkpoint", std::string()) == checkpoint_digest) return;
  // Only the latest: an older checkpoint replayed into the folder is not this one.
  record["checkpoint"] = checkpoint_digest;
  write_record(project_id, operation_id, record);
}

bool checkpoint_kept_here(const std::string &project_id, const std::string &operation_id,
    const std::string &checkpoint_digest) {
  if (checkpoint_digest.empty()) return false;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  const std::optional<Json> record = read_record(project_id, operation_id);
  return record && record->value("checkpoint", std::string()) == checkpoint_digest;
}

void note_prepared_mutation(const std::string &project_id, const std::string &operation_id,
    const std::string &record_digest) {
  note_in_list(project_id, operation_id, "mutations", record_digest);
}

bool prepared_mutation_here(const std::string &project_id, const std::string &operation_id,
    const std::string &record_digest) {
  return listed(project_id, operation_id, "mutations", record_digest);
}

void note_prepared_approval(const std::string &project_id, const std::string &operation_id,
    const ApprovalBinding &binding) {
  if (project_id.empty() || operation_id.empty() || binding.approval_id.empty()) return;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  Json record = read_record(project_id, operation_id).value_or(new_record(project_id, operation_id));
  if (!record.contains("approvals") || !record.at("approvals").is_array()) record["approvals"] = Json::array();
  // An approval id names one change, once: what it was first prepared as stands.
  if (prepared_change(record, binding.approval_id) != nullptr) return;
  Json entry = binding_json(binding);
  entry["decision"] = "";
  record.at("approvals").push_back(std::move(entry));
  write_record(project_id, operation_id, record);
}

bool record_decision_here(const std::string &project_id, const std::string &operation_id,
    const ApprovalBinding &shown, bool approved, const std::string &actor_id) {
  if (project_id.empty() || operation_id.empty() || shown.approval_id.empty()) return false;
  const std::lock_guard<std::mutex> lock(authority_mutex());
  Json record = read_record(project_id, operation_id).value_or(new_record(project_id, operation_id));
  Json *entry = prepared_change(record, shown.approval_id);
  if (approved) {
    if (entry == nullptr || !same_change(*entry, shown)) return false;
    const std::string decided = entry->value("decision", std::string());
    if (decided == "rejected") return false;
  } else if (entry == nullptr) {
    if (!record.contains("approvals") || !record.at("approvals").is_array()) record["approvals"] = Json::array();
    Json added = binding_json(shown);
    record.at("approvals").push_back(std::move(added));
    entry = &record.at("approvals").back();
  }
  (*entry)["decision"] = approved ? "approved" : "rejected";
  (*entry)["decidedBy"] = actor_id;
  write_record(project_id, operation_id, record);
  return true;
}

bool prepared_here(const std::string &project_id, const std::string &operation_id, const ApprovalBinding &binding) {
  const std::lock_guard<std::mutex> lock(authority_mutex());
  std::optional<Json> record = read_record(project_id, operation_id);
  if (!record) return false;
  const Json *entry = prepared_change(*record, binding.approval_id);
  return entry != nullptr && same_change(*entry, binding);
}

bool approved_here(const std::string &project_id, const std::string &operation_id,
    const ApprovalBinding &binding) {
  const std::lock_guard<std::mutex> lock(authority_mutex());
  std::optional<Json> record = read_record(project_id, operation_id);
  if (!record) return false;
  const Json *entry = prepared_change(*record, binding.approval_id);
  return entry != nullptr && entry->value("decision", std::string()) == "approved" && same_change(*entry, binding);
}

} // namespace singularity::tools
