#pragma once

#include "Tool/Types/ToolTypes.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace singularity::tools {

/*
 * What an operation may do, as THIS computer recorded it - never as a
 * Project's `.lm` says. A Matter folder may be shared, and anyone who can
 * write the folder can write its operation log: who began an operation, the
 * step a paused operation will write, and whether the lawyer approved it.
 * None of that is authority. These records live in this computer's own records
 * (Platform/AppSettings computer_records_directory), and only this process
 * writes them:
 *
 *  - who began the operation - the person at this computer, or an agent - and
 *    the most it may ever do without asking (its ceiling), fixed when it began;
 *  - whether it works only on this computer: its models are local ones and its
 *    tools reach no network, on every path that carries it on;
 *  - the content this computer generated for it, by digest;
 *  - each change it prepared for approval, exactly - action, target document,
 *    version, content digest - and the lawyer's decision on it, with who
 *    decided.
 *
 * An operation this computer did not begin, a step it did not generate, or an
 * approval it did not record runs ask-first: every change it would make is put
 * to the lawyer here.
 */

enum class OperationOrigin { Person, Agent };

struct OperationAuthority {
  OperationOrigin origin = OperationOrigin::Person;
  std::string agent_id;
  // The person at this computer when the operation began.
  std::string actor_id;
  ProjectActionMode ceiling = ProjectActionMode::FullProjectAccess;
  // It works only on this computer: an agent set to, and every request that
  // carries one of its operations on.
  bool local_only = false;
};

// The exact change an approval is for (MatterRequestExecution's checkpoint
// binding): its approval, action, target document and version, and the digest
// of the content that would be written.
struct ApprovalBinding {
  std::string approval_id;
  std::string tool_id;
  std::string intent_digest;
  std::string proposal_digest;
  std::optional<std::string> target_document_id;
  std::optional<int> target_version;
  std::optional<std::string> target_fingerprint;
  // The name a new document is written under.
  std::optional<std::string> new_document;
};

// The change a checkpoint's draft would write, as an approval binds it: the
// draft's approval fields, and the digest of the content the draft holds now -
// computed here, never the digest the draft states. Empty when the draft binds
// no approval.
ApprovalBinding approval_binding_of_checkpoint(const nlohmann::json &draft);

// Records who began `operation_id` and its ceiling. An operation's authority
// only ever narrows: a later call for the same operation may make it an
// agent's, lower its ceiling or keep it on this computer, never the reverse.
void begin_operation_authority(const std::string &project_id, const std::string &operation_id,
    const OperationAuthority &authority);

// What this computer recorded when the operation began; nullopt for one it did
// not begin here.
std::optional<OperationAuthority> operation_authority(const std::string &project_id,
    const std::string &operation_id);

// The lower of two modes.
ProjectActionMode lower_of(ProjectActionMode left, ProjectActionMode right);

// `permission`, never above `ceiling`; `reason` says why when it is held below
// the Project's own mode.
ProjectActionPermissionState within_ceiling(ProjectActionPermissionState permission, ProjectActionMode ceiling,
    const std::string &reason);

// Content this computer generated for the operation, by its SHA-256.
void note_generated_here(const std::string &project_id, const std::string &operation_id,
    const std::string &content_digest);
bool generated_here(const std::string &project_id, const std::string &operation_id,
    const std::string &content_digest);

// The operation's checkpoint as this computer last kept it, by digest
// (MatterRequestExecution's checkpoint_digest): a resume from a checkpoint that
// differs - changed in the shared .lm, or kept on another computer - runs
// ask-first.
void note_checkpoint_kept(const std::string &project_id, const std::string &operation_id,
    const std::string &checkpoint_digest);
bool checkpoint_kept_here(const std::string &project_id, const std::string &operation_id,
    const std::string &checkpoint_digest);

// A document write this computer prepared (its write-ahead record, by digest):
// only such a write is finished after an interruption.
void note_prepared_mutation(const std::string &project_id, const std::string &operation_id,
    const std::string &record_digest);
bool prepared_mutation_here(const std::string &project_id, const std::string &operation_id,
    const std::string &record_digest);

// A change this computer prepared and put to the lawyer.
void note_prepared_approval(const std::string &project_id, const std::string &operation_id,
    const ApprovalBinding &binding);

// Whether this computer prepared exactly `binding` for the lawyer.
bool prepared_here(const std::string &project_id, const std::string &operation_id, const ApprovalBinding &binding);

// The lawyer's decision, recorded with who made it. An approval is recorded
// only for a change this computer prepared, exactly as it prepared it: false,
// recording nothing, otherwise. A rejection only ever stops a change and is
// always recorded.
bool record_decision_here(const std::string &project_id, const std::string &operation_id,
    const ApprovalBinding &shown, bool approved, const std::string &actor_id);

// Whether the person at this computer approved exactly `binding`.
bool approved_here(const std::string &project_id, const std::string &operation_id,
    const ApprovalBinding &binding);

// What this computer has seen of a Matter's operations, so that shared state
// changed afterwards never lifts a restriction it saw.
struct SeenOperations {
  // The operations a Matter held unsigned the first time this computer looked
  // at it with a version that signs operations (Chat/OperationOrigin): begun
  // before, and taken as they stood then.
  std::set<std::string> earlier;
  // Every operation it has seen kept to the computer that began it.
  std::set<std::string> kept_here;
};

// Records what this computer sees of `project_id`'s operations now -
// `unsigned_now` counts only on its first look - and returns all it has seen.
SeenOperations note_seen_operations(const std::string &project_id, const std::vector<std::string> &unsigned_now,
    const std::vector<std::string> &kept_here_now);

// Where the records are kept (this computer's own records).
std::filesystem::path operation_authority_directory();

} // namespace singularity::tools
