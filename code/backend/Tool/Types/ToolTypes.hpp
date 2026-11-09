#pragma once

#include "Provider/Types/UsageMetadata.hpp"

#include <cstddef>
#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace singularity::tools {

enum class ProjectActionMode { AskForApproval, ApproveSafeActions, FullProjectAccess };

enum class ToolEffectClass {
  Unclassified,
  LocalRead,
  LocalProposal,
  ExternalNetwork,
  RoutineProjectMutation,
  ElevatedProjectMutation,
};

enum class ToolOutcome { Executed, ApprovalRequired, Denied, Failed };
enum class PermissionDisposition { Execute, RequireApproval, Deny };

inline const char *to_string(ProjectActionMode mode) noexcept {
  switch (mode) {
    case ProjectActionMode::AskForApproval: return "ask_for_approval";
    case ProjectActionMode::ApproveSafeActions: return "approve_safe_actions";
    case ProjectActionMode::FullProjectAccess: return "full_project_access";
  }
  return "ask_for_approval";
}

inline std::optional<ProjectActionMode> project_action_mode_from_string(
    const std::string &value) noexcept {
  if (value == "ask_for_approval") return ProjectActionMode::AskForApproval;
  if (value == "approve_safe_actions") return ProjectActionMode::ApproveSafeActions;
  if (value == "full_project_access") return ProjectActionMode::FullProjectAccess;
  return std::nullopt;
}

inline const char *to_string(ToolEffectClass effect) noexcept {
  switch (effect) {
    case ToolEffectClass::Unclassified: return "unclassified";
    case ToolEffectClass::LocalRead: return "local_read";
    case ToolEffectClass::LocalProposal: return "local_proposal";
    case ToolEffectClass::ExternalNetwork: return "external_network";
    case ToolEffectClass::RoutineProjectMutation: return "routine_project_mutation";
    case ToolEffectClass::ElevatedProjectMutation: return "elevated_project_mutation";
  }
  return "unclassified";
}

inline std::optional<ToolEffectClass> tool_effect_class_from_string(
    const std::string &value) noexcept {
  if (value == "unclassified") return ToolEffectClass::Unclassified;
  if (value == "local_read") return ToolEffectClass::LocalRead;
  if (value == "local_proposal") return ToolEffectClass::LocalProposal;
  if (value == "external_network") return ToolEffectClass::ExternalNetwork;
  if (value == "routine_project_mutation") return ToolEffectClass::RoutineProjectMutation;
  if (value == "elevated_project_mutation") return ToolEffectClass::ElevatedProjectMutation;
  return std::nullopt;
}

inline const char *to_string(ToolOutcome outcome) noexcept {
  switch (outcome) {
    case ToolOutcome::Executed: return "executed";
    case ToolOutcome::ApprovalRequired: return "approval_required";
    case ToolOutcome::Denied: return "denied";
    case ToolOutcome::Failed: return "failed";
  }
  return "failed";
}

inline std::optional<ToolOutcome> tool_outcome_from_string(const std::string &value) noexcept {
  if (value == "executed") return ToolOutcome::Executed;
  if (value == "approval_required") return ToolOutcome::ApprovalRequired;
  if (value == "denied") return ToolOutcome::Denied;
  if (value == "failed") return ToolOutcome::Failed;
  return std::nullopt;
}

struct OrganizationToolPolicy {
  ProjectActionMode maximum_action_mode = ProjectActionMode::FullProjectAccess;
  bool external_network_permitted = true;
  bool project_mutation_permitted = true;
  std::string cap_reason;
};

struct ProjectActionPermissionState {
  ProjectActionMode requested_mode = ProjectActionMode::AskForApproval;
  ProjectActionMode effective_mode = ProjectActionMode::AskForApproval;
  bool is_capped = false;
  std::string cap_reason;
  bool external_network_permitted = true;
  bool project_mutation_permitted = true;
};

struct ToolExecutionContext {
  std::string project_id;
  std::optional<std::string> chat_id;
  std::string operation_id;
  ProjectActionPermissionState permission;
	// Trusted backend-only values used to minimize/protect outbound research.
	// Never provided by a model and never persisted in ToolCallEvidence.
	std::vector<std::string> project_sensitive_values;
	// The writings that name one and the same party, one group per identity the
	// Project knows ("Meridia", "Meridia Clinical Systems, Inc."). A model never
	// sees a document's own bytes, so when it quotes a passage back it often
	// writes a party another way; where that is the only difference, the passage
	// it names is the passage the document holds. Read only by the document
	// work, never sent anywhere.
	std::vector<std::vector<std::string>> project_identity_writings;
	// This operation's own protected originals (the Papinian request mapping
	// and the Project entity aliases it protected), read whenever a research
	// request is minimized and re-checked: a value protection classified as
	// sensitive for this operation never leaves in a query, even though the
	// model's response restores it. Set by routing::execute(); never persisted.
	std::function<std::vector<std::string>()> operation_protected_values;
	// Pinned by request composition; never controlled by model arguments.
	std::optional<std::string> research_backend;
	// Durable write-ahead/commit acknowledgements for mutation tools. Payloads
	// are backend-authored JSON and remain in the canonical operation checkpoint.
	std::function<void(const std::string &)> mutation_prepared;
	std::function<void(const std::string &)> mutation_committed;
	std::function<bool()> operation_is_current;
	std::function<void(const std::string &)> approval_requested;
	// The lawyer answered a question this operation asked in the middle of its
	// work (tool lawyer.ask): the question's approval id and the answer as JSON
	// text - null when the lawyer left the decision to Singularity. Set by request
	// composition, so the operation keeps the question and its answer on its
	// own record; never by a model.
	std::function<void(const std::string &, const std::string &)> lawyer_question_answered;
	// Read-only Project discovery, search and reading (capability project.read),
	// answered by the operation the request belongs to: only it knows which
	// documents this request may read and the source markers its passages are
	// cited by. Takes the tool id, its arguments as JSON text and how many
	// tokens of the model's context the answer may take; returns the tool
	// result as JSON text. Set by request composition, never by a model.
	std::function<std::string(const std::string &, const std::string &, std::size_t)> project_evidence;
	// The room the model tool loop leaves for the next project.read answer,
	// set by the loop for each such call it makes.
	std::size_t project_evidence_room_tokens = 0;
	// How long a wait on the lawyer - a question, a web search to approve -
	// holds this request's thread once its place is kept (ResearchPause.hpp):
	// past it, the request returns and waits there, and the decision resumes it
	// with no model call made twice. None: it waits in this process until
	// decided. Set by request composition, never by a model.
	std::optional<std::chrono::milliseconds> lawyer_wait_limit;
	// Nobody watches this request as it runs: an AI colleague's own work. A wait
	// whose place cannot be kept is then not held at all - a web search is
	// declined, a question is left to Singularity - so it never holds a thread.
	bool unattended = false;
};

struct ToolCallEvidence {
  std::string id;
  std::string tool_id;
  std::string capability;
  ToolEffectClass effect = ToolEffectClass::Unclassified;
  ToolOutcome outcome = ToolOutcome::Failed;
  ProjectActionMode effective_mode = ProjectActionMode::AskForApproval;
  bool explicit_approval_required = false;
  std::optional<bool> explicit_approval_granted;
  std::string started_at;
  std::string completed_at;
  std::string normalized_intent_digest;
  std::string error_category;
  // Privacy-safe LM-owned canonicalization diagnostics: sizes, in raw UTF-8
  // bytes (never codepoints/clusters/tokens - the unit is explicit in the
  // name precisely so a future reader never has to guess or assume), and
  // whether canonicalization actually changed the argument - populated only
  // by a tool whose intent went through an LM-owned canonicalization step
  // ahead of ToolBroker validation (today: web.search's "query", via
  // canonicalize_search_arguments() in ResearchTools.hpp). Left unset
  // (nullopt) for every tool with no such step - never a stand-in for
  // "zero" or "false", which would misrepresent a tool that never had
  // anything to canonicalize in the first place. This is raw sizing
  // telemetry only; it makes no claim about what unit or bound production
  // validation should actually use.
  std::optional<std::size_t> raw_query_utf8_bytes;
  std::optional<std::size_t> canonical_query_utf8_bytes;
  std::optional<bool> intent_canonicalized;
  // The real capacity boundary for research input: an approximate token
  // count for the canonical query (ContextBudget.hpp's estimate_tokens(),
  // the same conservative heuristic already used for continuation budgeting
  // - never exact, never a token count reported by a provider), the
  // selected research model's real context window in tokens where known
  // (known_model_context_window_tokens()), and which of three outcomes this
  // comparison produced. "context_window_unknown" means capacity could not
  // be verified at all (an unrecognized provider/model) and the request was
  // failed closed rather than assumed unlimited - see
  // ResearchToolLoop.hpp's admissible_research_input_tokens(). Populated
  // alongside the fields above, and for the same reason: this replaces the
  // removed, unjustified 1000-byte bound with the one boundary that is
  // actually real.
  std::optional<std::size_t> estimated_input_tokens;
  std::optional<std::size_t> model_context_window_tokens;
  std::optional<std::string> budget_classification;
  // A tool that itself calls a paid external model service (web research runs
  // on a hosted model) records that call here: the service's provider and
  // model, and the usage the service reported - never guessed, never merged
  // into the reasoning model's own usage. Unset for a tool that called none.
  std::optional<std::string> external_provider_id;
  std::optional<std::string> external_model_id;
  std::optional<providers::UsageMetadata> external_usage;
};

// Thrown by a tool whose paid external model service answered but whose call
// still failed: the broker keeps the reported usage on the failed call's
// evidence instead of losing it with the error.
class BilledExternalCallFailure final : public std::runtime_error {
public:
  BilledExternalCallFailure(const std::string &message, std::string providerId, std::string modelId,
      providers::UsageMetadata reportedUsage)
      : std::runtime_error(message), provider_id(std::move(providerId)), model_id(std::move(modelId)),
        usage(std::move(reportedUsage)) {}
  std::string provider_id;
  std::string model_id;
  providers::UsageMetadata usage;
};

struct ToolApprovalRequest {
  std::string approval_id;
  std::string operation_id;
  std::string project_id;
  std::string summary;
  ToolEffectClass effect = ToolEffectClass::LocalRead;
  // What exactly is being approved: the tool, the digest of its normalized
  // intent, and the Project document it changes at the version it was
  // proposed against. An approval is honoured only for that exact change.
  std::string tool_id;
  std::string intent_digest;
  std::optional<std::string> target_document_id;
  std::optional<int> target_version;
  std::optional<std::string> target_fingerprint;
  // The change itself, as a person reads a change before approving it: what
  // kind ("append", "replace", "create", "cells", or "" when the tool does not
  // say), the text that goes and the text that comes. Never rendered from
  // anything but the proposal being approved.
  std::string change_kind;
  std::string removed_text;
  std::string added_text;
  // Rows that go into a table, with the columns they fit - so the change can be
  // shown the way the document will show it. Empty for any other change.
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  // The whole change as the tool's own domain describes it (for a Project
  // document: every step, with what goes and what comes - see
  // DocumentChangeDescription.hpp), serialized JSON. The fields above are its
  // summary; this is what a person reads to approve it. Empty when the tool
  // describes nothing more, and for an approval that is not a document change.
  std::string change_detail_json;
  // For an approval that is a question to the lawyer rather than a decision
  // (tool lawyer.ask): the question exactly as Singularity puts it, serialized
  // JSON. Empty for every other approval.
  std::string question_json;
};

// A human decision on an approval Singularity cannot accept - it names another
// change, the operation was stopped, or it was already decided. `code` is the
// stable reason the native bridge reports ("approval_mismatch", ...).
class ApprovalRefused final : public std::runtime_error {
public:
  ApprovalRefused(std::string code, const std::string &message)
      : std::runtime_error(message), code_(std::move(code)) {}
  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

} // namespace singularity::tools
