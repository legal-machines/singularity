#pragma once

#include "Tool/Types/ToolTypes.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <set>
#include <vector>

namespace singularity::tools {

// While an agent's task runs on this thread (Agent/AgentProduction), no
// Project action goes further than `cap`: agents read and propose, and every
// change - and anything a tool would send to the network - waits for the
// lawyer's approval in the agent's chat, whatever the Matter's own mode. The
// lawyer's later approval runs on another thread, without the cap.
class ScopedActionModeCap {
public:
  explicit ScopedActionModeCap(ProjectActionMode cap);
  ~ScopedActionModeCap();
  ScopedActionModeCap(const ScopedActionModeCap &) = delete;
  ScopedActionModeCap &operator=(const ScopedActionModeCap &) = delete;

private:
  std::optional<ProjectActionMode> previous_;
};

ProjectActionPermissionState project_action_permission(const std::string &projectId);
ProjectActionPermissionState save_project_action_permission(const std::string &projectId,
    ProjectActionMode requestedMode);
OrganizationToolPolicy organization_tool_policy();
void set_organization_tool_policy_for_testing(const OrganizationToolPolicy &policy);
void reset_organization_tool_policy_for_testing();
// How long an answered approval nobody waits on is kept for the run that filed
// it to come and read it, when no run of its operation was begun through
// begin_operation() (a loop driven directly - a test, the console). A minute in
// production.
void set_waiter_arrival_grace_for_testing(std::chrono::milliseconds grace);

struct ToolIntent {
  std::string tool_id;
  nlohmann::json arguments = nlohmann::json::object();
};

struct ToolResult {
  ToolOutcome outcome = ToolOutcome::Failed;
  nlohmann::json value = nlohmann::json::object();
  std::string error_category;
	// Plain-language transient failure detail for the operation UI. It is not
	// part of ToolCallEvidence and must never contain tool arguments or secrets.
	std::string error_message;
};

struct ToolAttempt {
  ToolResult result;
  ToolCallEvidence evidence;
  std::optional<ToolApprovalRequest> approval;
};

using ToolValidator = std::function<std::optional<std::string>(const ToolExecutionContext &,
    const nlohmann::json &)>;
using ToolHandler =
    std::function<nlohmann::json(const ToolExecutionContext &, const nlohmann::json &)>;

struct ToolDefinition {
  std::string id;
  std::string capability;
  ToolEffectClass effect = ToolEffectClass::LocalRead;
  std::string approval_summary;
  ToolValidator validate;
  // What to tell the person when this tool's own validator refuses a call.
  // The category a validator returns is a slug for the record; a lawyer reads
  // the answer, and "invalid_or_stale_artifact_action" tells them nothing about
  // what Singularity was trying to do to which document. A tool that can say it in
  // professional words does; one that cannot leaves this empty and the slug is
  // used as before.
  std::function<std::string(const std::string &category)> explain_rejection;
  ToolHandler execute;
  // For a tool that changes a Project document: fills the approval's target
  // document, version and fingerprint from the intent's arguments.
  std::function<void(const ToolExecutionContext &, const nlohmann::json &, ToolApprovalRequest &)> describe_approval_target;
  // True when this one intent must be put to the person whatever the
  // Project's action mode - for a change, the lawyer's own request to see it
  // before it is written. Never grants anything: an organization policy that
  // denies the action still refuses it first, and Ask first asks regardless.
  std::function<bool(const ToolExecutionContext &, const nlohmann::json &)> always_asks;
	// For a tool whose calls differ in how far they reach into the Project (a
	// change that only adds or rewords, against one that deletes): the effect
	// of this one call, which the Project's action mode is then applied to. It
	// only ever moves a Project change between routine and elevated; any other
	// answer, or none, leaves `effect` as it is.
	std::function<ToolEffectClass(const ToolExecutionContext &, const nlohmann::json &)> effect_of;
  // True for a tool whose approval is an answer rather than a yes or a no: the
  // person's reply (the lawyer's answer to a question) reaches the tool as its
  // "answers" argument. Only such a tool is ever handed a reply, so the
  // approval of a web search or a document change can never carry arguments
  // a caller made up.
  bool takes_response = false;
};

class ToolBroker {
public:
  void register_tool(const ToolDefinition &definition);
  void unregister_tool(const std::string &toolId);
  void clear_registered_tools_for_testing();

  ToolAttempt request(const ToolExecutionContext &context, const ToolIntent &intent);
  // The tool's own validator, run exactly as request() runs it before an
  // approval can exist, without asking, executing or recording anything: a
  // refusal as request() would return it, or nothing when the call could be
  // carried out. Lets a caller return an unmakeable proposal to the model that
  // made it instead of to a person.
  std::optional<ToolAttempt> validate(const ToolExecutionContext &context, const ToolIntent &intent);
  ToolAttempt wait_for_approval(const std::string &approvalId);
  // As wait_for_approval(), for at most `limit` (Beta 1 #14). Nothing when it
  // passes with no decision: the approval is then withdrawn under the same lock,
  // so a decision is either taken here or lands on the operation's kept place
  // (ResearchPause.hpp) - never both, never lost.
  std::optional<ToolAttempt> wait_for_approval_for(const std::string &approvalId, std::chrono::milliseconds limit);
  // Executes `intent` without asking again, because the caller already
  // holds durable, canonical proof that a human explicitly authorized this
  // exact action (an organization policy that denies the action outright
  // still refuses it) - MatterTask's own
  // document_operation_checkpoint.draft, NEVER ToolBroker::pending_ (which
  // is in-process memory only: a real restart loses it, even though the
  // Project/task/checkpoint on disk survives - the real architectural gap
  // this method exists to close). Never reuses a stored context/intent from
  // an earlier request() call the way wait_for_approval() does (that reuse
  // is only safe within the SAME call stack that made the original request -
  // reusing it later, e.g. on a genuine resume/restart, is a real,
  // live-reproduced dangling-reference crash this session's own
  // DirectDocumentWork approval-resume acceptance test caught). Callers
  // must independently revalidate anything version/fingerprint-sensitive in
  // `intent` themselves before calling this - the underlying tool's own
  // expected-state checks (e.g. document.patch.apply's expectedVersion/
  // expectedFingerprint) still apply and still fail closed.
  ToolAttempt execute_preapproved(const ToolExecutionContext &context, const ToolIntent &intent);
  // `response` is the person's reply, for a tool that takes one
  // (ToolDefinition::takes_response) - anything else with a reply is refused.
  // `isForOperation` is "Allow for this request": the approval is granted, and
  // every later call of the same effect in the same operation runs without
  // asking again. Only web research (ExternalNetwork) can be allowed this way:
  // a change to a Project document is always approved exactly, one at a time.
  bool respond_to_approval(const std::string &projectId, const std::string &operationId,
      const std::string &approvalId, bool isAllowed, const nlohmann::json &response = nullptr,
      bool isForOperation = false);
  // The effect classes the person allowed for the rest of this operation.
  std::set<ToolEffectClass> operation_grants(const std::string &projectId, const std::string &operationId) const;
  // A run of the operation begins: a Stop of an earlier run no longer refuses
  // its calls (reset_operation), and while it runs, an approval it filed is
  // kept once answered for as long as the run takes to come and read it.
  void begin_operation(const std::string &projectId, const std::string &operationId);
  // The operation's run is over: what was allowed for it ends with it, and an
  // answered approval it filed and never came to wait on is let go.
  void end_operation(const std::string &projectId, const std::string &operationId);
  // How many approvals the broker still holds, answered or not (tests).
  std::size_t held_approvals_for_testing() const;
  // The person's decision on an approval whose run is no longer waiting for it
  // (Singularity closed while a question or a web search waited). The resumed run
  // makes the same call again, and it is decided at once with this answer
  // instead of being put to the person a second time: matched on the exact
  // tool and normalized intent, used once, and only for this operation. A
  // call that differs in any way is asked about as usual.
  // `isForOperation` is the lawyer's "Allow for this request" on that search:
  // web research is then allowed for the rest of the resumed run, as it would
  // have been for the run that asked.
  void record_prior_decision(const std::string &projectId, const std::string &operationId,
      const std::string &toolId, const std::string &intentDigest, bool isAllowed,
      const nlohmann::json &response = nullptr, bool isForOperation = false);
	// The lawyer asked, in this request, to see its changes before they are
	// made: every change the operation makes to the Project waits for them,
	// whatever the Project's action mode, until release_changes() or the end
	// of the operation's run. Never grants anything.
	void hold_changes(const std::string &projectId, const std::string &operationId);
	void release_changes(const std::string &projectId, const std::string &operationId);
	bool changes_held(const std::string &projectId, const std::string &operationId) const;
  void invalidate_operation(const std::string &projectId, const std::string &operationId);
	void reset_operation(const std::string &projectId, const std::string &operationId);
	bool operation_invalidated(const std::string &projectId, const std::string &operationId) const;
  std::optional<ToolApprovalRequest> pending_approval(const std::string &projectId,
      const std::string &operationId) const;
  // Whether a thread waits on this approval right now (wait_for_approval). A
  // web search the research loop asked for is; a change to a Project document
  // never is: it waits on its operation's checkpoint once the run returns.
  bool is_awaited(const std::string &approvalId) const;

private:
  // The decision on `approvalId` once its wait is over, with the broker's lock
  // held on entry and released before the tool runs.
  ToolAttempt take_decision(std::unique_lock<std::mutex> &lock, const std::string &approvalId);

  struct PendingApproval {
    ToolApprovalRequest request;
    ToolExecutionContext context;
    ToolDefinition definition;
    ToolIntent intent;
    ToolCallEvidence evidence;
    nlohmann::json response;
    bool is_decided = false;
    bool is_allowed = false;
    bool is_invalidated = false;
    // Threads inside wait_for_approval() for this approval right now.
    int waiters = 0;
    std::chrono::steady_clock::time_point filed_at;
    // Filed by a run begun through begin_operation(): the entry is kept while
    // that run lives, since it may still come to wait on it, however long it
    // takes to get there. Otherwise only the arrival grace keeps it.
    bool is_run_tracked = false;
  };

  // True when nothing waits on this answered approval now, and nothing will
  // come to. Called with mutex_ held.
  bool is_abandoned_locked(const PendingApproval &pending,
      std::chrono::steady_clock::time_point now) const;
  // Drops every approval that is answered and that nobody waits on or will
  // come to wait on. Called with mutex_ held.
  void erase_answered_locked();

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<std::string, ToolDefinition> definitions_;
  std::map<std::string, PendingApproval> pending_;
	std::set<std::pair<std::string, std::string>> invalidated_operations_;
	std::map<std::pair<std::string, std::string>, std::set<ToolEffectClass>> operation_grants_;
  // Operations with a run in progress (begin_operation .. end_operation).
  std::set<std::pair<std::string, std::string>> running_operations_;
  struct PriorDecision {
    std::string tool_id;
    std::string intent_digest;
    bool is_allowed = false;
    nlohmann::json response;
    bool is_for_operation = false;
  };
  std::map<std::pair<std::string, std::string>, std::vector<PriorDecision>> prior_decisions_;
	// Operations whose changes wait for the lawyer whatever the mode.
	std::set<std::pair<std::string, std::string>> held_changes_;
};

ToolBroker &production_tool_broker();
std::string normalized_intent_digest(const ToolDefinition &definition, const ToolIntent &intent);
PermissionDisposition permission_for(const ProjectActionPermissionState &permission,
    ToolEffectClass effect);

// What the lawyer's own words say about approval, read the same way whatever
// the model made of them: a floor under Full access and Apply routine changes
// that no model answer can lower.
//
// True when the words ask to see a change before it is made - "show me
// first", "don't change it yet", "do not modify the NDA", "make no changes",
// "before changing anything, show me", "I want to approve it", "get my
// approval before editing", "once I give the go-ahead", "покажи сначала", "не
// вноси изменения в файл", "покажи, а потом внесём", "пришли на
// согласование". A fixed, bounded list of English and Russian phrasings,
// matched word by word within one clause; an ordinary instruction that only
// limits a change ("fix the typo, don't change anything else", "renumber the
// clauses without changing the text", "формулировки не меняй") is not one of
// them, nor one that asks for the change it proposes ("propose a redline and
// apply it") or for a new document to review ("draft a new NDA for my
// review"), nor one that says there is no need to ask ("no need to show me
// first", "without my approval", "без моего согласования").
[[nodiscard]] bool asks_to_see_changes_first(const std::string &words);
// True when `quote` is the lawyer's own words asking for something: found word
// for word - case, accents and punctuation aside - in one of `messages`, its
// parts in order where it leaves words out ("…"). A quote of fewer than three
// words counts only as the whole of a message ("Yes, do it."). Only a sentence
// that asks for work on a document can be quoted: a request ("Restore the
// cap.", "Верни лимит."), a request put politely ("Could you restore the
// cap?", "Do you mind updating clause 5?", "Can clause 9 be tightened?",
// "Можешь вернуть лимит?"), a want or a requirement stated ("I'd like clause 9
// updated.", "The governing law needs to be New York.", "Срок должен быть 60
// дней.") or a consent ("Yes, go ahead."). A question ("What's the cap?",
// "Какой срок?"), a question whether a change may or should be made ("Can we
// delete clause 14 without breaching the SPA?", "Would you change anything in
// clause 9?", "Можно ли удалить пункт 14?"), a request to be told ("Can you
// tell me what the cap is?", "Review the NDA and tell me whether the cap is
// market.", "Подскажи срок.") and a statement ask for nothing and are never
// such a quote.
[[nodiscard]] bool quotes_lawyer(const std::string &quote, const std::vector<std::string> &messages);
// True when `after`, written in place of `before`, takes out what `before`
// said rather than rewording it: nothing is left, most of its words are gone
// ("[Reserved.]" for a clause), what is left says only that nothing is there
// ("0", "N/A", "Intentionally omitted", "Исключён"), or a word that limits it
// is dropped ("shall not be liable" as "shall be liable", "не менее" as
// "менее"). "Sixty (60) days" as "thirty (30) days" only rewords. Read the
// same way as the lawyer's words, case and accents aside.
[[nodiscard]] bool takes_out_substance(const std::string &before, const std::string &after);
// True when `after`, written in place of `before`, says anything `before` did
// not: a word changed, added or taken out. Only a correction leaves what it
// says as it was - a word spelt right ("recieve" as "receive"), an article
// added or dropped, a doubled word dropped - case, accents, spacing and
// punctuation aside. "Sixty (60) days" as "thirty (30) days" rewrites it, as
// do "Supplier" as "Customer", "New York" as "the Cayman Islands" and
// "unlimited" as "limited to $100". Text written where there was none adds,
// and rewrites nothing.
[[nodiscard]] bool rewrites_substance(const std::string &before, const std::string &after);

} // namespace singularity::tools
