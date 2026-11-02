#pragma once

#include "Chat/ChatConversation/ChatConversation.hpp"
#include "Chat/ChatOutboundPreview/ChatOutboundPreview.hpp"
#include "Chat/Types/Chat.hpp"
#include "Tool/Types/ToolTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <functional>
#include <string>

namespace singularity::chat {

// The real, backend-owned lifecycle of ONE Chat operation. Every state here
// is a fact this process can actually observe - never a stage invented to
// make a progress card look busier than the pipeline is
// (phase-24-chat-execution-results-sources.md's "Truthful stages only"):
//
//   Preparing  Phase 21 source resolution and Phase 22 operation setup are
//              genuinely running (prepare_chat_request()). Nothing has been
//              sent; nothing has been created.
//   Working    the canonical MatterRequestExecution attempt is genuinely in
//              flight - context assembly, Papinian Guard where external,
//              the provider/local-model call, response restoration and
//              source-reference validation all happen inside this single
//              uninterruptible call, so Singularity deliberately reports one
//              honest state for it rather than four fictional ones.
//   WaitingForAnswer
//              the model put a question only the lawyer can answer and the
//              attempt waits, inside this process, for the answer; the same
//              attempt then continues with it. The attempt keeps its place
//              before it waits (a durable pause, see
//              paused_decision_awaits_resume()), so a question left open when
//              Singularity closes is still there to answer after the next launch.
//   Stopping   Stop was requested while an attempt was already in flight.
//              A provider HTTPS request in flight is cancelled on its
//              connection at once (Platform/HttpClient/HttpClient.hpp's
//              HttpCancelToken), and so is a local generation, whose
//              connection to the runtime closes (LocalInference/
//              LocalInferenceService). Either way cancellation then
//              takes effect at the next safe backend boundary.
//   Completed  a successful canonical MatterTask exists.
//   Failed     a real failure. Either the request could not be attempted at
//              all (no model assigned, a refused source policy) or the
//              attempt produced an error MatterTask.
//   Stopped    cancellation took effect. Before the attempt started, no
//              MatterTask exists at all; after it started, the canonical
//              MatterTask is durably marked stopped_by_user and is never
//              presented as successful current work.
enum class ChatOperationState {
	Preparing, Working, WaitingForApproval, WaitingForAnswer, Stopping, Completed, Failed, Stopped, Interrupted, Conflict
};

[[nodiscard]] const char *to_string(ChatOperationState state) noexcept;

// One operation's lifecycle projected from its canonical MatterTask, with
// the process registry used only to add live cancellation/approval facts.
struct ChatOperationStatus {
  // False only when neither the live cache nor durable Project state knows
  // this operation identity.
  bool is_known = false;
  ChatOperationState state = ChatOperationState::Completed;
  // True only while the operation can still change state on its own.
  bool is_active = false;
  // True from the moment Stop was accepted, whatever state followed.
  bool is_stop_requested = false;
  // Set once a canonical MatterTask exists for this operation.
  std::optional<std::string> task_id;
  // Plain language, present only for Failed/Stopped.
  std::string message;
  std::optional<tools::ToolApprovalRequest> approval;
	std::uint64_t revision = 0;
	std::uint64_t turn_contract_revision = 0;
	std::string stage;
	// While a proposed change waits for the lawyer: the Project sources it was
	// prepared from, exactly as this operation recorded them, so the decision is
	// made with its provenance in view. Empty otherwise.
	std::vector<ChatCitation> citations;
	// Live progress of an operation this process is running right now, from
	// the real execution event stream (Routing/Observability). Empty key and
	// negative times when no live progress exists (finished, or a restarted
	// process reading durable state only).
	std::string progress_stage_key;
	std::string progress_stage_text;
	long long elapsed_ms = -1;
	long long stage_elapsed_ms = -1;
	// What this operation is using, known once its request is prepared.
	long long since_last_event_ms = -1;
	std::optional<int> candidate_document_count;
	std::optional<int> supplied_document_count;
	// Every stage this operation has been through, in order, with the moment
	// (ms since start) it began - the stage list a lawyer watches.
	std::vector<std::pair<std::string, long long>> stage_history;
	std::string model_provider_id;
	std::string model_id;
	int document_count = 0;
	bool is_external_destination = false;
	bool resume_allowed = false;
	// The answer as far as the model has written it, while this process runs
	// the operation: shown in its place until the finished answer arrives.
	// Empty otherwise. Never persisted.
	std::string live_answer;
	std::vector<matters::AgentOperationEvent> events;
	// The question this operation is waiting on the lawyer to answer, exactly
	// as they are shown it - {id, state "asked", title, questions} - while its
	// attempt waits in this process for the answer. Empty otherwise.
	std::optional<nlohmann::json> asking;
	// Every question the operation put to the lawyer and what became of it. One
	// that was never answered reads "unanswered" once the operation is no
	// longer waiting.
	std::vector<matters::LawyerQuestionRecord> questions;
	// Another computer's Singularity runs it (a shared Matter): read here, never
	// called interrupted or resumed here while it is unfinished.
	bool runs_elsewhere = false;
};

// Runs ONE real Chat operation, end to end, through the accepted stack and
// nothing else:
//
//   composition
//   -> chat::prepare_chat_request()          Phase 21 sources + Phase 22 setup
//   -> PreparedChatRequest::request_params   the canonical handoff
//   -> routing::matter_request_execution::run_task()
//                                            PromptAssembler/Project Context,
//                                            Papinian Guard where external,
//                                            the real provider/local-model
//                                            call, response restoration,
//                                            SourceMarkerRegistry validation
//                                            and canonical MatterTask creation
//
// Nothing about the provider, model, legal task, primary result role, AI
// Setup inheritance, source selection or protection configuration is
// rebuilt here: every one of those is already decided by the boundary above
// that owns it, and this function only carries the result across. There is
// deliberately no Chat-specific execution pipeline, no second request
// assembler and no Chat-owned result store.
//
// `operation_id` is an opaque caller-supplied correlation handle so Stop can
// name this operation while it is still running (the send itself does not
// return until it is finished). It is never domain identity - the canonical
// MatterTask id is - and an id belonging to an operation that is still
// active is rejected rather than silently reused.
//
// Throws only for a request that could not be attempted at all (Phase 21
// refusing the source policy, Phase 22 having no model for the primary
// result role, a Project/Chat that no longer exists). A provider/local-model
// failure is not thrown: it becomes a real error MatterTask, exactly as the
// canonical execution path already guarantees.
struct ChatExecutionOutcome {
  std::string operation_id;
  ChatOperationState state = ChatOperationState::Completed;
  // Empty only when cancellation took effect before an attempt was ever
  // started, so no operation exists to record.
  std::string task_id;
  std::string message;
};

ChatExecutionOutcome run_chat_request(const std::string &operation_id,
                                     const ChatRequestComposition &composition);

// QUICK CHAT TITLES
//
// A new chat is named within moments of its first message, so its place in the
// lists and in the row above it never reads "New chat" while the answer is
// still being written. A local model on this Mac writes the name from the first
// message alone, on a thread of its own, beside the request - never in its way.
//
// It is housekeeping, not part of the operation: it emits no operation event,
// writes no usage entry, and is never reported as a stage. It needs a local
// generative model that is installed (one that is not loaded yet is loaded
// through the same governed path every local request takes); with none, or when
// the chat itself runs on the local model, nothing happens and the answer's own
// title - or, failing that, the message's opening words - names the chat.
//
// Off until the application turns it on (NativeBridge, with its other
// background services), so nothing that drives ChatExecution directly - a test,
// the console - ever starts a model for a title. The application supplies the
// writer: given the Project, the chat and its first message it returns a local
// model's reply, or "" when there is no local model to ask (this module owns
// the thread and how the chat is named - never which model is asked or how;
// what is kept of the reply is chat_title_from_reply's, in ChatActions, the
// same for every way a model names a chat).
using QuickChatTitleWriter = std::function<std::string(
	const std::string &project_id, const std::string &chat_id, const std::string &first_message)>;
void enable_quick_chat_titles(QuickChatTitleWriter writer);
void stop_quick_chat_titles() noexcept;

ChatExecutionOutcome resume_chat_operation(const std::string &project_id,
	const std::string &operation_id);

// Whether a request's work stays on this computer and what it really runs on
// there (Beta 1 #44): the same decision the send makes, said without sending.
// Nothing when it may run anywhere. The application installs it as the
// composer's kept-here resolver (ChatOutboundPreview).
std::optional<KeptHereModel> kept_here_model(const ChatRequestComposition &composition);

// Whether an agent works only on this computer (Agent/ owns its policy): a
// request in its thread, or one naming it, is its work and stays here. The
// Native Bridge installs it; unset, only an agent's own runs - and everything
// that carries one of them on - are kept here.
void set_agent_works_only_here(std::function<bool(const std::string &agent_id)> works_only_here);

// Retry, Run with another model and Edit and continue for a Chat task: a new
// canonical Chat operation (its own operation id, tool context, approvals,
// Stop and durable lifecycle) that joins the task's retry chain. Unset fields
// reuse what the task ran with. A pure retry (no prompt or source change, no
// other model) is refused when it cannot plausibly change the outcome, and
// another model must differ from the task's own.
struct ChatRerunRequest {
	std::optional<std::string> prompt;
	std::optional<SourcePolicyMode> source_policy_mode;
	std::vector<std::string> selected_document_ids;
	std::optional<routing::ModelTarget> model_target;
};

ChatExecutionOutcome rerun_chat_task(const std::string &project_id, const std::string &task_id,
	const ChatRerunRequest &request);

// Requests cancellation of one operation of this Project. Backend-owned: it
// records the request against the real operation record above, cancels the
// operation's provider HTTPS request or local generation in flight, and the
// execution honours it at its next safe boundary.
// An unknown or already-terminal operation is reported exactly as it is.
ChatOperationStatus request_stop(const std::string &project_id, const std::string &operation_id);

// Singularity is closing: every operation still running in this process is
// invalidated (which also wakes anything waiting for an approval that will
// never come) and recorded durably as interrupted, so a later launch finds the
// truth instead of an operation that looks like it is still working. A
// proposal paused for the lawyer's decision is not running and is left as it
// is: its change waits on its own checkpoint to be approved or rejected after
// the next launch. Returns how many operations were interrupted. Never throws.
std::size_t interrupt_live_operations() noexcept;

// Test-only: forgets this process's live coordination of operations - what a
// fresh process starts with - so a test can read operations the way Singularity
// reads them after a real restart: from their records alone.
void reset_operation_registry_for_testing();

// Test-only: called on the run's own thread once its durable record is written
// and before its request is sent, so a test can land a Stop exactly in that
// window. Empty in the application.
void set_after_operation_record_hook_for_testing(std::function<void(const std::string &operation_id)> hook);

// Every operation of this Project that has not finished, as this Node can
// truthfully report it after any restart. An operation left non-terminal by a
// process that is gone (a crash, a kill, a power loss - anything that never
// reached interrupt_live_operations()) is reconciled to a durable Interrupted
// here rather than being reported as if it were still working; an operation
// genuinely executing in this process is reported as it is. A proposal still
// waiting for the lawyer's decision stays waiting, and interrupted work stays
// listed until it is resumed or dismissed. Resume is reported only where it
// can work: a saved checkpoint, no decision still owed. This is what makes
// unfinished work findable at all: without it a lawyer who relaunches Singularity
// has no way to reach an operation whose Chat they cannot remember.
struct ChatOperationSummary {
	std::string operation_id;
	std::string task_id;
	std::optional<std::string> chat_id;
	std::string prompt;
	ChatOperationState state = ChatOperationState::Working;
	std::string stage;
	std::string activity;
	std::string updated_at;
	bool resume_allowed = false;
	bool waiting_for_approval = false;
	// True when this operation's own change is already durable on disk: the
	// document was changed, and only the turn that records it is unfinished.
	bool change_committed = false;
	// Another computer's Singularity runs it (a shared Matter): it is neither
	// interrupted nor resumable here.
	bool runs_elsewhere = false;
};

std::vector<ChatOperationSummary> list_operations(const std::string &project_id);

ChatOperationStatus operation_status(const std::string &project_id, const std::string &operation_id);

// Records a human decision on the approval an operation is waiting for and
// returns the operation's resulting status. An operation executing in this
// process receives it through its in-process wait; a paused operation records
// it durably against the exact approval its checkpoint names. Throws
// tools::ApprovalRefused for another change's approval ("approval_mismatch"),
// a stopped operation ("operation_stopped"), an operation not waiting
// ("operation_not_waiting"), a different second answer
// ("approval_already_decided") or an unknown approval ("approval_not_found");
// repeating the same answer changes nothing. A denial ends the operation at
// once with nothing changed.
//
// `for_request` is "Allow for this request": a web search or page approved so
// is approved, and the rest of this operation's web research runs without
// another card (ToolBroker's operation grant). It is recorded on the
// operation's events. A change to a Project document or a question is never
// allowed this way ("approval_scope").
ChatOperationStatus record_approval_response(const std::string &project_id, const std::string &operation_id,
	const std::string &approval_id, bool allowed, bool for_request = false);

// Gives the lawyer's answer to the question an operation is waiting on and
// returns the operation's resulting status; the waiting attempt continues with
// it at once - or, when Singularity closed while it waited, the answer is recorded
// on the place it kept and the run resumes from there. `answers` is
// {questionId: {optionIds, text}}, or nullopt for "Decide these yourself" -
// never a refusal: only Stop ends a question without an answer. Throws
// tools::ApprovalRefused for a stopped operation ("operation_stopped"), one no
// longer waiting - its place was not kept, or the question was already
// answered ("operation_not_waiting"),
// another question's id ("question_mismatch") or an answer the question cannot
// take ("answer_invalid"); a refused answer leaves the question open.
ChatOperationStatus record_question_answer(const std::string &project_id, const std::string &operation_id,
	const std::string &question_id, const std::optional<nlohmann::json> &answers);

// DURABLE PAUSE (ExternalResearch/ResearchToolLoop/ResearchPause.hpp). A run
// that waits on the lawyer - a question, or a web search to approve - keeps its
// place in the Project's `.lm` before it waits. When Singularity closes meanwhile,
// the question or the search is shown again after the next launch
// (WaitingForAnswer / WaitingForApproval), and answering it records the
// decision on that place: record_question_answer() and
// record_approval_response() take it there instead of refusing. The run then
// resumes (resume_chat_operation()) with no model call it already made.
//
// True when such a decision is on record and no run is carrying it on yet: the
// caller starts resume_chat_operation() for it, off the window's thread.
bool paused_decision_awaits_resume(const std::string &project_id, const std::string &operation_id);

} // namespace singularity::chat
