#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "Platform/Trace/Trace.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"

#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/Events/Events.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatContinuity/ChatContinuity.hpp"
#include "Chat/OperationOrigin/OperationOrigin.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Routing/Observability/ExecutionProgress.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include "ExternalResearch/ResearchToolLoop/ResearchPause.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/LiveAnswer/LiveAnswer.hpp"
#include "Routing/MatterRequestExecution/MatterRequestExecution.hpp"
#include "Provider/Types/GenerationStream.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Tool/LawyerQuestion/LawyerQuestion.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <set>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::chat {
namespace {

using Json = nlohmann::json;

// This computer's device id: the operations list and status are polled every
// second, so it is read once per profile.
std::string this_device_id() {
	return platform::current_device_id();
}

struct OperationRecord {
  std::string project_id;
  ChatOperationState state = ChatOperationState::Preparing;
  bool stop_requested = false;
  // Singularity closed while this operation was running: the work stops at its
  // next boundary exactly like a Stop, but the record says what really
  // happened - never "you stopped this request".
  bool interrupted_by_shutdown = false;
  // The run returned its thread and waits for the lawyer at the place it kept
  // (Beta 1 #14): its pause is kept, never discarded as a finished run's is.
  bool kept_for_lawyer = false;
  std::optional<std::string> task_id;
  std::string message;
	std::optional<std::string> persisted_approval_id;
	// True only for the exact span run_chat_request() is genuinely inside
	// run_task() for this operation - see ActiveExecutionGuard's own comment
	// for why this, not `state`, is the real "still running" signal
	// begin_operation()/resume_chat_operation() must check.
	bool actively_executing = false;
  // Monotonic within this process - only used to prune the oldest terminal
  // records below.
  unsigned long long sequence = 0;
	// The live progress projection of this operation while run_chat_operation
	// is on the stack. Owned there; set and cleared under the registry mutex
	// (LiveProgressAttachment), and only read under that same mutex, so a
	// status read can never touch it after it is destroyed.
	routing::observability::ExecutionProgressTracker *progress = nullptr;
	// The progress as it stood when the run returned still waiting on the
	// lawyer - a proposal to approve - and when. The steps the lawyer watched
	// stay on the card, with their times, until the approved work goes on;
	// never a shorter list rebuilt from saved events.
	std::optional<routing::observability::ExecutionProgressSnapshot> frozen_progress;
	std::vector<routing::observability::ExecutionProgressTransition> frozen_transitions;
	std::chrono::steady_clock::time_point frozen_at;
	std::string model_provider_id;
	std::string model_id;
	int document_count = 0;
	bool is_external_destination = false;
	// The questions this operation has put to the lawyer, as last read from its
	// durable record. A status poll that lands while a save holds that record
	// answers from this copy, so a question already answered never drops out
	// of the chat for the length of someone else's save.
	std::vector<matters::LawyerQuestionRecord> questions;
	// Stops this operation's provider requests on the connection itself: a Stop
	// or Singularity closing cancels what is in flight instead of waiting minutes
	// for a long generation to answer.
	std::shared_ptr<platform::HttpCancelToken> http_cancel;
	// Set by mark_resuming(): the lawyer decided on a kept pause and the run
	// that carries the decision on has not started yet. Cleared when it does
	// (begin_operation() writes a fresh record).
	bool resuming_after_decision = false;
	// The answer as far as the model has written it, while run_chat_operation
	// runs (Routing/LiveAnswer): in memory only, and cleared when the run
	// returns - the finished answer takes its place.
	std::string live_answer;
};

// Live coordination only. Durable lifecycle truth is the MatterTask carrying
// the same agent_operation_id; this cache exists solely for prompt Stop and
// approval coordination while the process is alive.
struct OperationRegistry {
  std::mutex mutex;
  std::unordered_map<std::string, OperationRecord> records;
  unsigned long long next_sequence = 1;
};

OperationRegistry &registry() {
  static OperationRegistry instance;
  return instance;
}

// The durable pause of a run that waited on the lawyer (defined below).
bool has_decided_pause(const matters::MatterTask &task, const std::optional<std::string> &directory);

bool is_terminal(ChatOperationState state) {
  return state == ChatOperationState::Completed || state == ChatOperationState::Failed ||
         state == ChatOperationState::Stopped || state == ChatOperationState::Interrupted ||
		 state == ChatOperationState::Conflict;
}

// Real product bug (caught live, this session's own restart-durability
// acceptance): the reported ChatOperationState alone cannot tell
// begin_operation()/resume_chat_operation() whether a call is genuinely
// executing right now versus merely left at a non-terminal state by an
// earlier call that has already fully returned - record_approval_response()
// durably records a decision and marks the record Working so a live status
// poll shows progress, but nothing is actually parked in a thread waiting for
// it - every approval design here is non-blocking (AdvancedWorkflow's own
// synchronous wait was removed with EXEC-2, for the same reason: an answer
// held in one process is lost the moment that process ends) - and the
// following resume_operation() bridge call was being wrongly refused as a
// "still running" double-send. `actively_executing` is the real, precise
// signal: true only for the exact span run_chat_request() is inside
// run_task(), false at every other time regardless of what state string is
// showing.
struct ActiveExecutionGuard {
	OperationRegistry &state;
	std::string operation_id;
	// begin_operation() claims `actively_executing` under the same lock that
	// checks it, so two concurrent sends or resumes of one operation can never
	// both pass; the guard only releases the claim on every exit path.
	ActiveExecutionGuard(OperationRegistry &registryState, std::string opId)
			: state(registryState), operation_id(std::move(opId)) {}
	~ActiveExecutionGuard() {
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end()) found->second.actively_executing = false;
	}
};

// Clears the answer shown while the model wrote it, on every exit of the run.
struct LiveAnswerRelease {
	OperationRegistry &state;
	std::string operation_id;
	~LiveAnswerRelease() {
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end()) found->second.live_answer.clear();
	}
};

// Attaches the operation's live progress tracker to its registry record for
// exactly the tracker's lifetime. Declared after the tracker, so it detaches
// before the tracker is destroyed, on the same executing thread.
struct LiveProgressAttachment {
	OperationRegistry &state;
	std::string operation_id;
	LiveProgressAttachment(OperationRegistry &registryState, std::string opId,
			routing::observability::ExecutionProgressTracker *tracker)
			: state(registryState), operation_id(std::move(opId)) {
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found == state.records.end()) return;
		found->second.progress = tracker;
		found->second.frozen_progress.reset();
		found->second.frozen_transitions.clear();
	}
	~LiveProgressAttachment() {
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found == state.records.end()) return;
		// A run that returns with its operation still open waits on the lawyer:
		// what it did so far is kept for the card. A finished one never reads it.
		if (found->second.progress != nullptr && is_terminal(found->second.state) == false) {
			found->second.frozen_progress = found->second.progress->snapshot();
			found->second.frozen_transitions = found->second.progress->transitions();
			found->second.frozen_at = std::chrono::steady_clock::now();
		}
		found->second.progress = nullptr;
	}
	LiveProgressAttachment(const LiveProgressAttachment &) = delete;
	LiveProgressAttachment &operator=(const LiveProgressAttachment &) = delete;
};

// Keeps a small tail of finished operations so a composer that polls just
// after a send returns still reads the real terminal state instead of
// "unknown". Bounded so a long session cannot accumulate records.
constexpr std::size_t kRetainedTerminalRecords = 32;

void prune_terminal_records_locked(OperationRegistry &state) {
  std::vector<std::pair<unsigned long long, std::string>> finished;
  for (const auto &entry : state.records) {
    if (is_terminal(entry.second.state)) finished.emplace_back(entry.second.sequence, entry.first);
  }
  if (finished.size() <= kRetainedTerminalRecords) return;
  std::sort(finished.begin(), finished.end());
  const std::size_t excess = finished.size() - kRetainedTerminalRecords;
  for (std::size_t index = 0; index < excess; ++index) state.records.erase(finished[index].second);
}

// Registers this operation, or refuses an id that already names an operation
// still running - two concurrent sends must never share one Stop handle.
matters::MatterTask *find_operation_task(matters::Matter &project, const std::string &operation_id) {
	const auto found = std::find_if(project.tasks.begin(), project.tasks.end(),
		[&](const matters::MatterTask &task) { return task.agent_operation_id == operation_id; });
	return found == project.tasks.end() ? nullptr : &*found;
}

ChatOperationState operation_state_from_string(const std::string &value) {
	if (value == "preparing") return ChatOperationState::Preparing;
	if (value == "working") return ChatOperationState::Working;
	if (value == "waiting_for_approval") return ChatOperationState::WaitingForApproval;
	if (value == "waiting_for_answer") return ChatOperationState::WaitingForAnswer;
	if (value == "stopping") return ChatOperationState::Stopping;
	if (value == "failed") return ChatOperationState::Failed;
	if (value == "stopped") return ChatOperationState::Stopped;
	if (value == "interrupted") return ChatOperationState::Interrupted;
	if (value == "conflict") return ChatOperationState::Conflict;
	return ChatOperationState::Completed;
}

void append_operation_event(matters::MatterTask &task, const std::string &type,
		const std::string &message, const std::optional<std::string> &subject_id = std::nullopt,
		const std::optional<std::string> &outcome = std::nullopt) {
	matters::AgentOperationEvent event;
	event.id = matters::generate_event_id();
	event.sequence = task.operation_events.empty() ? 1 : task.operation_events.back().sequence + 1;
	event.type = type;
	event.created_at = workspaces::current_timestamp();
	event.operation_revision = task.operation_revision;
	event.message = message;
	event.subject_id = subject_id;
	event.outcome = outcome;
	task.operation_events.push_back(std::move(event));
}

// The approval as the broker holds it, with what only the saved one carries:
// the answer the change was proposed with and the concern of a review that did
// not clear it (MatterRequestExecution attaches both when the change is put to
// the lawyer). The same approval saved or shown again keeps them; another
// approval never takes them.
std::optional<tools::ToolApprovalRequest> with_saved_answer(std::optional<tools::ToolApprovalRequest> approval,
		const std::optional<tools::ToolApprovalRequest> &saved) {
	if (!approval || !saved || saved->approval_id != approval->approval_id) return approval;
	const Json kept = Json::parse(saved->change_detail_json, nullptr, false);
	Json detail = approval->change_detail_json.empty() ? Json::object()
		: Json::parse(approval->change_detail_json, nullptr, false);
	if (!kept.is_object() || !detail.is_object()) return approval;
	bool isChanged = false;
	for (const char *key : {"answer", "reviewNote"}) {
		if (!kept.contains(key) || detail.contains(key)) continue;
		detail[key] = kept.at(key);
		isChanged = true;
	}
	if (isChanged) approval->change_detail_json = detail.dump();
	return approval;
}

void persist_operation_transition(const std::string &project_id, const std::string &operation_id,
		ChatOperationState state, const std::string &stage, const std::string &message,
		const std::optional<std::string> &task_id = std::nullopt,
		const std::optional<bool> &cancel_requested = std::nullopt,
		const std::optional<bool> &resume_allowed = std::nullopt,
		const std::optional<tools::ToolApprovalRequest> &approval = std::nullopt,
		const std::optional<providers::TaskErrorCode> &error_code = std::nullopt) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace_path, project_id);
	if (!project) return;
	matters::MatterTask *task = find_operation_task(*project, operation_id);
	if (task == nullptr && task_id) {
		const auto found = std::find_if(project->tasks.begin(), project->tasks.end(),
			[&](const matters::MatterTask &entry) { return entry.id == *task_id; });
		if (found != project->tasks.end()) task = &*found;
	}
	if (task == nullptr) return;
	// A step still in progress never goes on record over an operation that
	// already finished, or over a Stop the lawyer made: a save from the window
	// that lost the race with the run itself must not reopen it. A resumed run
	// starts again from "preparing" (persist_new_operation), never through here.
	if (!is_terminal(state)
			&& (is_terminal(operation_state_from_string(task->operation_execution_state))
				|| (task->operation_cancel_requested && state != ChatOperationState::Stopping)))
		return;
	++task->operation_revision;
	task->operation_execution_state = to_string(state);
	task->operation_stage = stage;
	task->operation_updated_at = workspaces::current_timestamp();
	task->operation_activity = message;
	if (state == ChatOperationState::Failed || state == ChatOperationState::Conflict) {
		task->status = matters::TaskStatus::Error;
		task->error = message;
		if (error_code) task->error_code = *error_code;
	}
	if (state == ChatOperationState::Stopped) task->stopped_by_user = true;
	if (cancel_requested) task->operation_cancel_requested = *cancel_requested;
	if (resume_allowed) task->operation_resume_allowed = *resume_allowed;
	task->operation_approval = with_saved_answer(approval, task->operation_approval);
	std::string event_message = message;
	if (state == ChatOperationState::Failed) event_message = "The operation failed.";
	else if (state == ChatOperationState::Conflict)
		event_message = "The operation stopped because the document changed.";
	append_operation_event(*task, "operation." + std::string(to_string(state)), event_message,
		task_id, is_terminal(state) ? std::optional<std::string>(to_string(state)) : std::nullopt);
	// A transition happens at one moment: its step and the record carry the
	// same time (restore_kept_wait dates a kept wait from its last step).
	task->operation_events.back().created_at = task->operation_updated_at;
	project->updated_at = task->operation_updated_at;
	matters::save_matter(workspace_path, *project);
}

std::string persist_new_operation(const std::string &operation_id,
		const ChatRequestComposition &composition, bool resuming) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace_path, composition.project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	if (matters::MatterTask *existing = find_operation_task(*project, operation_id)) {
		// A new send never silently continues an existing operation under a new
		// request; only an explicit resume does.
		if (!resuming) throw std::runtime_error("This request reference is already in use.");
		if (!existing->document_operation_checkpoint && !has_decided_pause(*existing, project->directory_path))
			throw std::runtime_error("This request cannot be resumed safely.");
		const ChatOperationState saved = operation_state_from_string(existing->operation_execution_state);
		// Another computer's unfinished run is that computer's to resume; once it
		// said the run was interrupted, any computer may take it on.
		if (!existing->operation_device_id.empty() && existing->operation_device_id != this_device_id()
				&& !is_terminal(saved) && saved != ChatOperationState::Interrupted)
			throw std::runtime_error("Singularity on another computer is running this request. It can be resumed there.");
		if ((saved == ChatOperationState::Completed || saved == ChatOperationState::Stopped
				|| saved == ChatOperationState::Conflict) && !existing->operation_resume_allowed)
			throw std::runtime_error("This request is not waiting to be resumed.");
		++existing->operation_revision;
		existing->operation_execution_state = "preparing";
		existing->operation_stage = "preparing";
		existing->operation_updated_at = workspaces::current_timestamp();
		existing->operation_activity = "Resuming from the last durable checkpoint.";
		existing->operation_cancel_requested = false;
		existing->operation_resume_allowed = false;
		existing->operation_device_id = this_device_id();
		existing->operation_approval.reset();
		existing->status = matters::TaskStatus::Success;
		existing->error.reset();
		existing->error_code.reset();
		existing->stopped_by_user = false;
		append_operation_event(*existing, "operation.resumed", existing->operation_activity);
		project->updated_at = existing->operation_updated_at;
		const std::string task_id = existing->id;
		matters::save_matter(workspace_path, *project);
		return task_id;
	}
	matters::MatterTask task;
	task.id = matters::generate_task_id();
	task.created_at = workspaces::current_timestamp();
	task.agent_operation_id = operation_id;
	task.operation_execution_state = "preparing";
	task.operation_revision = 1;
	task.operation_stage = "preparing";
	task.operation_started_at = task.created_at;
	task.operation_updated_at = task.created_at;
	task.operation_activity = "Preparing the request.";
	task.operation_device_id = this_device_id();
	task.chat_id = composition.chat_id;
	task.prompt = composition.prompt;
	task.selected_document_ids = composition.selected_document_ids;
	// Kept on this computer: marked in the Matter as well, so it stays here if
	// this computer's record is lost or another computer carries it on.
	task.local_only = composition.local_only;
	append_operation_event(task, "operation.created", task.operation_activity);
	// Who began this operation and the most it may ever do without asking,
	// recorded on this computer before anything runs (Tool/OperationAuthority).
	// An AI colleague's answer - its own run, a thread where it answers, a
	// request carrying on one of its operations - is the colleague's, and may do
	// what the Matter's own mode lets any member do: never less because it is an
	// AI (the owner, 2026-10-30; D-158).
	tools::OperationAuthority authority;
	authority.actor_id = platform::resolve_actor_identity().actor_id;
	authority.local_only = composition.local_only;
	const auto threadOf = !composition.chat_id ? project->chats.end()
		: std::find_if(project->chats.begin(), project->chats.end(),
			[&](const matters::MatterChat &entry) { return entry.id == *composition.chat_id; });
	const std::string threadAgent = threadOf != project->chats.end() ? threadOf->agent_id : std::string();
	if (composition.agent_run || composition.agent_initiated || !composition.agent_id.empty() || !threadAgent.empty()) {
		authority.origin = tools::OperationOrigin::Agent;
		authority.agent_id = !composition.agent_id.empty() ? composition.agent_id : threadAgent;
	}
	// A request carrying on another - Retry, Run with another model, Edit and
	// continue, Send again - never has more authority than that one had here; one
	// this computer did not begin, or one it cannot name, runs ask-first.
	if (!composition.continues_operation_id.empty() || composition.retry_of_task_id) {
		const std::optional<tools::OperationAuthority> continued = composition.continues_operation_id.empty()
			? std::nullopt : tools::operation_authority(composition.project_id, composition.continues_operation_id);
		if (!continued) {
			authority.ceiling = tools::ProjectActionMode::AskForApproval;
		} else {
			if (continued->origin == tools::OperationOrigin::Agent) {
				authority.origin = tools::OperationOrigin::Agent;
				if (authority.agent_id.empty()) authority.agent_id = continued->agent_id;
			}
			authority.ceiling = tools::lower_of(authority.ceiling, continued->ceiling);
		}
	}
	try {
		tools::begin_operation_authority(composition.project_id, operation_id, authority);
	} catch (const std::exception &) {
		// Never run on without the record of what the request may do.
		throw std::runtime_error("Singularity could not record on this computer what this request may do, so it did not "
			"run. Check that the disk has room and that Singularity can write its settings, then send it again.");
	}
	// The same, signed into the Matter, so another computer carrying it on keeps
	// to it (Chat/OperationOrigin).
	sign_operation_origin(composition.project_id, task, authority);
	// A Chat's scope is what its last request ran with. Chat::selected_document_
	// ids is the default every later request of this Chat inherits, and until
	// now only creating a Chat ever set it: a person who narrowed a long
	// diligence conversation to five documents got those five for that one
	// request, and the next turn - or the same conversation reopened tomorrow -
	// silently went back to reading the whole data room. Last-write-wins and no
	// revision bump, for draft_text's own reason: sending is not the kind of
	// structural change rename and AI Setup's optimistic lock protects.
	if (composition.chat_id && !composition.chat_id->empty()) {
		const auto chat = std::find_if(project->chats.begin(), project->chats.end(),
			[&](const matters::MatterChat &entry) { return entry.id == *composition.chat_id; });
		if (chat != project->chats.end()) {
			if (chat->selected_document_ids != composition.selected_document_ids)
				chat->selected_document_ids = composition.selected_document_ids;
			// A request sent is the Chat's latest activity: it rises to the top of
			// Recent and is what the Project's last activity says.
			chat->updated_at = task.created_at;
		}
	}
	const std::string task_id = task.id;
	project->tasks.push_back(std::move(task));
	project->updated_at = workspaces::current_timestamp();
	matters::save_matter(workspace_path, *project);
	return task_id;
}

// Whoever shows this operation reads it again, instead of polling (Beta 1
// #263): ids and the state only (Platform/Events).
void publish_operation_changed(const std::string &project_id, const std::string &operation_id,
		std::optional<ChatOperationState> state_value) {
	nlohmann::json payload{{"projectId", project_id}, {"operationId", operation_id}};
	if (state_value) payload["state"] = to_string(*state_value);
	platform::events::publish("operation.changed", payload);
}

void begin_operation(const std::string &operation_id, const ChatRequestComposition &composition) {
  if (operation_id.empty()) throw std::runtime_error("This request has no operation reference.");
  OperationRegistry &state = registry();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.records.find(operation_id);
  // See ActiveExecutionGuard's own comment: `actively_executing`, never the
  // reported state string, is what tells a genuine concurrent double-send
  // apart from a legitimate resume re-entry after approval.
  if (found != state.records.end() && found->second.actively_executing == true) {
    throw std::runtime_error("This request is already running.");
  }
  // A Stop accepted before this operation started (the send was still queued)
  // is never dropped: the operation stops at its first boundary.
  const bool stopRequested = found != state.records.end() && found->second.stop_requested
    && found->second.project_id == composition.project_id;
  // Nothing new begins once Singularity is closing (Platform/AppLifecycle): a
  // run the pool takes up while the application closes stops at its first
  // boundary, recorded as the closing it was, never as the lawyer's Stop.
  // Read under the registry lock, which interrupt_live_operations() takes after
  // closing has begun, so an operation is either refused here or interrupted
  // there.
  const bool isClosing = platform::lifecycle::is_closing();
  prune_terminal_records_locked(state);
  OperationRecord record;
  record.project_id = composition.project_id;
  record.state = ChatOperationState::Preparing;
  record.stop_requested = stopRequested || isClosing;
  record.interrupted_by_shutdown = isClosing;
  record.actively_executing = true;
  record.sequence = state.next_sequence++;
	record.http_cancel = std::make_shared<platform::HttpCancelToken>();
	if (stopRequested || isClosing) record.http_cancel->cancel();
	state.records[operation_id] = std::move(record);
	if (!stopRequested && !isClosing)
		tools::production_tool_broker().begin_operation(composition.project_id, operation_id);
	publish_operation_changed(composition.project_id, operation_id, ChatOperationState::Preparing);
}

std::shared_ptr<platform::HttpCancelToken> http_cancel_token(const std::string &operation_id) {
	OperationRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	const auto found = state.records.find(operation_id);
	return found == state.records.end() ? nullptr : found->second.http_cancel;
}

// The run is over, whatever its end: web research the lawyer allowed "for
// this request" is not allowed for the next one.
struct OperationGrantsGuard {
	std::string project_id;
	std::string operation_id;
	~OperationGrantsGuard() {
		tools::production_tool_broker().end_operation(project_id, operation_id);
	}
};

void attach_durable_task(const std::string &operation_id, const std::string &task_id) {
	OperationRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	const auto found = state.records.find(operation_id);
	if (found != state.records.end()) found->second.task_id = task_id;
}

bool is_stop_requested(const std::string &operation_id) {
  OperationRegistry &state = registry();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.records.find(operation_id);
  return found != state.records.end() && found->second.stop_requested;
}

bool is_interrupted_by_shutdown(const std::string &operation_id) {
  OperationRegistry &state = registry();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.records.find(operation_id);
  return found != state.records.end() && found->second.interrupted_by_shutdown;
}

// Moves an operation forward, but never past a Stop already accepted: once
// the user has asked to stop, Working can no longer overwrite Stopping.
void advance_operation(const std::string &operation_id, ChatOperationState state_value) {
  OperationRegistry &state = registry();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.records.find(operation_id);
  if (found == state.records.end()) return;
  if (found->second.stop_requested == true && is_terminal(state_value) == false) {
    found->second.state = ChatOperationState::Stopping;
    publish_operation_changed(found->second.project_id, operation_id, ChatOperationState::Stopping);
    return;
  }
  found->second.state = state_value;
  publish_operation_changed(found->second.project_id, operation_id, state_value);
}

void finish_operation(const std::string &operation_id, ChatOperationState state_value,
                      const std::optional<std::string> &task_id, const std::string &message) {
  OperationRegistry &state = registry();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.records.find(operation_id);
  if (found == state.records.end()) return;
  found->second.state = state_value;
  found->second.task_id = task_id;
  found->second.message = message;
  publish_operation_changed(found->second.project_id, operation_id, state_value);
}

// The one durable consequence of a Stop that arrived too late to prevent the
// request: the canonical MatterTask this operation really did produce is
// marked stopped, so a reload can never present it as ordinary successful
// current work. Nothing else about the record is touched - its response,
// usage, provenance and source references remain exactly as the operation
// produced them, because destroying real evidence would be its own lie.
void mark_task_stopped_by_user(const std::string &project_id, const std::string &task_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto project = matters::load_matter(workspace_path, project_id);
  if (!project) return;
  const auto task = std::find_if(project->tasks.begin(), project->tasks.end(),
      [&](const matters::MatterTask &entry) { return entry.id == task_id; });
  if (task == project->tasks.end() || task->stopped_by_user == true) return;
  task->stopped_by_user = true;
  matters::save_matter(workspace_path, *project);
}

// What a test runs once a run's durable record is written (see
// set_after_operation_record_hook_for_testing); empty in the application.
std::function<void(const std::string &operation_id)> &after_operation_record_hook() {
	static std::function<void(const std::string &operation_id)> hook;
	return hook;
}

void remove_unstarted_operation_task(const std::string &project_id,
		const std::string &operation_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace_path, project_id);
	if (!project) return;
	const auto task = std::find_if(project->tasks.begin(), project->tasks.end(),
		[&](const matters::MatterTask &entry) {
			return entry.agent_operation_id == operation_id
				&& entry.operation_execution_state == "preparing"
				&& !entry.document_operation_checkpoint;
		});
	if (task == project->tasks.end()) return;
	project->tasks.erase(task);
	project->updated_at = workspaces::current_timestamp();
	matters::save_matter(workspace_path, *project);
}

// DURABLE PAUSE (ExternalResearch/ResearchToolLoop/ResearchPause.hpp): where
// a run waiting on the lawyer - a question, or a web search to approve - keeps
// its place, so the wait survives Singularity closing. One file per operation,
// kept on THIS computer beside the app's settings, never in the Project's `.lm`:
// a pause is the model's turn and, once decided, the lawyer's decision - things
// anyone with a shared Matter folder could otherwise write - and only this
// computer resumes a run it paused. It holds no restoration mapping, and losing
// it costs only the resume (the request can be sent again). A Project without
// `.lm` data keeps no pause.
std::optional<std::filesystem::path> pause_path(const std::optional<std::string> &directory,
		const std::string &operation_id) {
	if (!directory || directory->empty() || operation_id.empty()) return std::nullopt;
	const std::filesystem::path data = persistence::project_data_path(std::filesystem::path(*directory));
	std::error_code error;
	if (std::filesystem::is_directory(data, error) == false) return std::nullopt;
	return platform::computer_records_directory() / "Pauses" / (platform::sha256_hex_digest(operation_id).substr(0, 40) + ".json");
}

std::optional<std::filesystem::path> pause_path(const std::string &project_id, const std::string &operation_id) {
	std::optional<std::string> directory;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
		if (!project) return std::nullopt;
		directory = project->directory_path;
	}
	return pause_path(directory, operation_id);
}

std::optional<Json> read_pause_at(const std::optional<std::filesystem::path> &path, const std::string &operation_id) {
	if (!path) return std::nullopt;
	std::ifstream file(*path, std::ios::binary);
	if (!file) return std::nullopt;
	const Json pause = Json::parse(file, nullptr, false);
	// A damaged pause, or one of another shape, is no pause: the request is
	// then offered to be sent again, like one whose place was never kept.
	if (!research::is_well_formed_pause(pause) || !pause.contains("operationId")
			|| pause.at("operationId") != Json(operation_id))
		return std::nullopt;
	return pause;
}

void write_pause_at(const std::filesystem::path &path, const Json &pause) {
	std::filesystem::create_directories(path.parent_path());
	platform::restrict_to_owner(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
		platform::write_file_durably(staged, pause.dump());
		platform::restrict_to_owner(staged);
		platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

// The name the model gave the conversation in the answer that asked the lawyer
// (its chatTitle), applied as the question or the web search is filed - so the
// chat does not go on reading "New chat" while it waits on them, however long
// that is, and after Singularity reopens. Only a name the model gave, kept the way
// any title a model writes is kept (chat_title_from_reply, against the
// lawyer's own message), and never over a chat anyone or anything has named
// already (apply_generated_title_if_default): a chat is named once. Naming is a
// convenience - nothing here ever fails the pause.
void apply_paused_chat_title(const std::string &project_id, const std::string &operation_id,
		const Json &pause) noexcept {
	try {
		if (!pause.contains("result") || !pause.at("result").is_object()) return;
		const Json &result = pause.at("result");
		if (!result.contains("text") || !result.at("text").is_string()) return;
		const Json said = Json::parse(result.at("text").get<std::string>(), nullptr, false);
		if (!said.is_object() || !said.contains("chatTitle") || !said.at("chatTitle").is_string()) return;
		std::optional<std::string> chatId;
		std::string asked;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
			const matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
			if (task != nullptr) {
				chatId = task->chat_id;
				asked = task->prompt;
			}
		}
		if (!chatId || chatId->empty()) return;
		const std::string title = chat_title_from_reply(said.at("chatTitle").get<std::string>(), asked);
		if (title.empty()) return;
		(void)apply_generated_title_if_default(project_id, *chatId, title);
	} catch (...) {
	}
}

void save_pause(const std::string &project_id, const std::string &operation_id, const Json &pause) {
	apply_paused_chat_title(project_id, operation_id, pause);
	const std::optional<std::filesystem::path> path = pause_path(project_id, operation_id);
	if (!path) return;
	Json stored = pause;
	stored["projectId"] = project_id;
	stored["operationId"] = operation_id;
	write_pause_at(*path, stored);
}

std::optional<Json> load_pause(const std::string &project_id, const std::string &operation_id) {
	return read_pause_at(pause_path(project_id, operation_id), operation_id);
}

void discard_pause(const std::string &project_id, const std::string &operation_id) {
	const std::optional<std::filesystem::path> path = pause_path(project_id, operation_id);
	if (!path) return;
	std::error_code ignored;
	std::filesystem::remove(*path, ignored);
}

void install_pause_store() {
	static std::once_flag installed;
	std::call_once(installed, [] {
		research::ResearchPauseStore &store = research::research_pause_store();
		store.save = save_pause;
		store.load = load_pause;
		store.discard = discard_pause;
	});
}

// A pause still waiting for the lawyer: the question or the web search they
// were shown before Singularity closed, not decided since - and, for a question,
// one the operation's own record still has open. Nothing otherwise.
std::optional<Json> undecided_pause(const matters::MatterTask &task, const std::optional<std::string> &directory) {
	if (!task.agent_operation_id || task.operation_cancel_requested) return std::nullopt;
	const std::optional<Json> pause = read_pause_at(pause_path(directory, *task.agent_operation_id),
		*task.agent_operation_id);
	if (!pause || research::is_decided_pause(*pause)) return std::nullopt;
	const std::string kind = pause->value("kind", std::string());
	if (kind == "lawyer_question") {
		const std::string questionId = pause->value("approvalId", std::string());
		const bool isOpen = std::any_of(task.lawyer_questions.begin(), task.lawyer_questions.end(),
			[&](const matters::LawyerQuestionRecord &record) { return record.id == questionId && record.state == "asked"; });
		if (isOpen == false) return std::nullopt;
		return pause;
	}
	if (kind == "research_approval") return pause;
	return std::nullopt;
}

// The lawyer decided while nothing waited in this process: the resumed run
// continues from the pause with the decision.
bool has_decided_pause(const matters::MatterTask &task, const std::optional<std::string> &directory) {
	if (!task.agent_operation_id || task.operation_cancel_requested) return false;
	const std::optional<Json> pause = read_pause_at(pause_path(directory, *task.agent_operation_id),
		*task.agent_operation_id);
	return pause && research::is_decided_pause(*pause);
}

// The web search or page a pause waits on, as the card showed it when it was
// asked - so the same card is shown again after Singularity reopens.
tools::ToolApprovalRequest paused_research_approval(const Json &pause, const std::string &project_id,
		const std::string &operation_id) {
	tools::ToolApprovalRequest approval;
	approval.approval_id = pause.value("approvalId", std::string());
	approval.operation_id = operation_id;
	approval.project_id = project_id;
	approval.summary = pause.value("summary", std::string());
	approval.effect = tools::tool_effect_class_from_string(pause.value("effect", std::string()))
		.value_or(tools::ToolEffectClass::ExternalNetwork);
	approval.tool_id = pause.value("toolId", std::string());
	approval.intent_digest = pause.value("intentDigest", std::string());
	if (pause.contains("change") && pause.at("change").is_object()) approval.change_detail_json = pause.at("change").dump();
	return approval;
}

// The question a pause waits on, exactly as the lawyer is shown it.
Json paused_asking(const Json &pause) {
	const Json shown = pause.value("question", Json::object());
	return Json{{"id", pause.value("approvalId", std::string())}, {"state", "asked"},
		{"title", shown.is_object() && shown.contains("title") ? shown.at("title") : Json(nullptr)},
		{"questions", shown.is_object() && shown.contains("questions") ? shown.at("questions") : Json::array()}};
}

// Singularity closes while this run waits on the lawyer, on the place it kept - a
// question or a web search they have not decided yet. That wait is not
// interrupted: after the next launch the same question or search is in front
// of them, and it goes on from where it stood. So its record says it still
// waits, and the closing adds no step to it - no "Interrupted" under "Waiting
// for your answer", and the wait still dates from when it was asked (its time
// is left as it was). Returns false, changing nothing, when no such place is
// kept: that run truly was interrupted, and is recorded so by the caller.
//
// The record of a run whose place is kept, written as the wait it is. The
// run's own save on its way out may have written it as ended; and a close
// that came while the place was still being saved found nothing kept yet and
// recorded it as interrupted. Neither happened: it waits - with no step for
// the closing, from when it began to wait. Whether the record changed.
bool restore_kept_wait(matters::MatterTask &task, const Json &pause) {
	const bool isQuestion = pause.value("kind", std::string()) == "lawyer_question";
	const std::string waiting = to_string(isQuestion ? ChatOperationState::WaitingForAnswer
		: ChatOperationState::WaitingForApproval);
	const bool hasClosingStep = !task.operation_events.empty()
		&& task.operation_events.back().type == "operation.interrupted";
	if (task.operation_execution_state == waiting && task.operation_resume_allowed == false && !hasClosingStep
			&& task.status == matters::TaskStatus::Success)
		return false;
	if (hasClosingStep) {
		task.operation_events.pop_back();
		if (!task.operation_events.empty()) task.operation_updated_at = task.operation_events.back().created_at;
	}
	++task.operation_revision;
	task.operation_execution_state = waiting;
	task.operation_stage = isQuestion ? "question" : "approval";
	task.operation_activity = isQuestion ? "Singularity asked you a question." : pause.value("summary", std::string());
	task.operation_resume_allowed = false;
	// The loop's exit on its way out of the wait is no failure of the turn: the
	// turn waits, and its card is the question or the search.
	task.status = matters::TaskStatus::Success;
	task.error.reset();
	task.error_code.reset();
	return true;
}

bool record_kept_wait(const std::string &project_id, const std::string &operation_id) noexcept {
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		if (task == nullptr) return false;
		const std::optional<Json> pause = undecided_pause(*task, project->directory_path);
		if (!pause) return false;
		if (restore_kept_wait(*task, *pause)) matters::save_matter(workspace_path, *project);
		return true;
	} catch (...) {
		return false;
	}
}

// Whether the run that just returned waits for the lawyer at its kept place:
// nobody decided its question or web search in the time it may hold its thread
// (Beta 1 #14). The state it waits in and the words for it, or nothing.
std::optional<std::pair<ChatOperationState, std::string>> kept_wait_of(const std::string &project_id,
		const std::string &operation_id) noexcept {
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
		const matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		// The attempt ended on its way out of the wait; a finished one never waits.
		if (task == nullptr || task->status != matters::TaskStatus::Error) return std::nullopt;
		const std::optional<Json> pause = undecided_pause(*task, project->directory_path);
		if (!pause) return std::nullopt;
		if (pause->value("kind", std::string()) == "lawyer_question")
			return std::make_pair(ChatOperationState::WaitingForAnswer, std::string("Singularity asked you a question."));
		return std::make_pair(ChatOperationState::WaitingForApproval, pause->value("summary", std::string()));
	} catch (...) {
		return std::nullopt;
	}
}

// The run waits for the lawyer at its kept place: this process says so until
// the run has returned (after_kept_wait), and its pause stays.
void keep_for_lawyer(const std::string &operation_id, ChatOperationState state_value,
		const std::optional<std::string> &task_id, const std::string &message) {
	std::string project_id;
	{
		OperationRegistry &state = registry();
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found == state.records.end()) return;
		found->second.kept_for_lawyer = true;
		found->second.state = state_value;
		if (task_id) found->second.task_id = task_id;
		found->second.message = message;
		project_id = found->second.project_id;
	}
	publish_operation_changed(project_id, operation_id, state_value);
}

bool is_kept_for_lawyer(const std::string &operation_id) {
	OperationRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	const auto found = state.records.find(operation_id);
	return found != state.records.end() && found->second.kept_for_lawyer;
}

// The lawyer decided at the very moment the run gave its thread back: the
// decision has no thread to take it yet, and the run's own record of its wait
// is still being written. Said as a moment's wait; the card stays.
[[noreturn]] void refuse_while_keeping(bool is_question) {
	throw tools::ApprovalRefused("decision_being_kept", is_question
		? "Singularity is keeping this question for you. Answer it again in a moment."
		: "Singularity is keeping this step for you. Decide it again in a moment.");
}

// Whether the operation's kept place is the one this decision is for.
bool pause_names(const std::string &project_id, const std::string &operation_id, const std::string &approval_id) {
	const std::optional<Json> pause = load_pause(project_id, operation_id);
	return pause && pause->value("approvalId", std::string()) == approval_id && !research::is_decided_pause(*pause);
}

// The lawyer answered a pause while nothing ran it: the run that resumes it is
// on its way, and until it starts, this process says so.
void mark_resuming(const std::string &project_id, const std::string &operation_id, const std::string &message) {
	OperationRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	OperationRecord &record = state.records[operation_id];
	if (record.actively_executing) return;
	record.project_id = project_id;
	record.state = ChatOperationState::Working;
	record.stop_requested = false;
	record.interrupted_by_shutdown = false;
	record.message = message;
	record.persisted_approval_id.reset();
	record.resuming_after_decision = true;
	record.sequence = state.next_sequence++;
}

// The run that was to carry the lawyer's decision on ended before it started:
// it found the request already finished, or could not start at all (its
// Project could not be read, or its pause no longer can). The chat must never
// go on reading "Working" for it. A finished request reads as it finished
// (`finished`). One that could not start reads as interrupted, with the way
// on: Resume while the decided pause is still kept, otherwise sending the
// request again. Only a record mark_resuming() made is settled here - never
// a run that is under way.
void settle_unstarted_resume(const std::string &project_id, const std::string &operation_id,
		const std::optional<ChatExecutionOutcome> &finished) {
	{
		OperationRegistry &state = registry();
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found == state.records.end() || found->second.actively_executing
				|| found->second.resuming_after_decision == false)
			return;
		if (finished) {
			found->second.state = finished->state;
			found->second.task_id = finished->task_id;
			found->second.message = finished->message;
			found->second.resuming_after_decision = false;
			return;
		}
		// With no live record, the operation's status is read from its durable
		// record below, which knows whether the decided pause is still kept.
		state.records.erase(found);
	}
	if (finished) return;
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		if (task == nullptr) return;
		const bool isResumable = has_decided_pause(*task, project->directory_path);
		const std::string activity = isResumable
			? "Singularity could not continue this request just now. Choose Resume to try again."
			: "Singularity could not continue this request. Send the request again to continue.";
		++task->operation_revision;
		task->operation_execution_state = to_string(ChatOperationState::Interrupted);
		task->operation_resume_allowed = isResumable;
		task->operation_activity = activity;
		task->operation_updated_at = workspaces::current_timestamp();
		append_operation_event(*task, "operation.interrupted", activity);
		project->updated_at = task->operation_updated_at;
		matters::save_matter(workspace_path, *project);
	} catch (const std::exception &) {
		// Its status then reads the record as it was saved: with no run under
		// way, an unfinished operation reads as interrupted all the same.
	}
}

// Drops the pause when the run is over, whatever its end - except Singularity
// closing, which is exactly what a pause is kept for.
struct PauseReleaseGuard {
	std::string project_id;
	std::string operation_id;
	~PauseReleaseGuard() {
		research::clear_research_attempt_facts(project_id, operation_id);
		if (is_interrupted_by_shutdown(operation_id) || is_kept_for_lawyer(operation_id)) return;
		try {
			discard_pause(project_id, operation_id);
		} catch (...) {
		}
	}
};

// run_task() appends exactly one attempt to the Project it returns, so the
// operation this call just created is its last task. Matched on the Chat as
// well, so a shape change upstream fails loudly here instead of silently
// reporting some other operation's id.
const matters::MatterTask *created_task(const matters::Matter &project,
		const std::string &operation_id) {
	const auto task = std::find_if(project.tasks.begin(), project.tasks.end(),
		[&](const matters::MatterTask &entry) { return entry.agent_operation_id == operation_id; });
	return task == project.tasks.end() ? nullptr : &*task;
}

} // namespace

const char *to_string(ChatOperationState state) noexcept {
  switch (state) {
  case ChatOperationState::Preparing: return "preparing";
  case ChatOperationState::Working: return "working";
  case ChatOperationState::WaitingForApproval: return "waiting_for_approval";
	case ChatOperationState::WaitingForAnswer: return "waiting_for_answer";
  case ChatOperationState::Stopping: return "stopping";
  case ChatOperationState::Completed: return "completed";
  case ChatOperationState::Failed: return "failed";
  case ChatOperationState::Stopped: return "stopped";
	case ChatOperationState::Interrupted: return "interrupted";
	case ChatOperationState::Conflict: return "conflict";
  }
  return "completed";
}

namespace {

std::mutex &agent_policy_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::function<bool(const std::string &)> &agent_works_only_here_hook() {
	static std::function<bool(const std::string &)> hook;
	return hook;
}

bool agent_works_only_here(const std::string &agent_id) {
	std::function<bool(const std::string &)> hook;
	{
		const std::lock_guard<std::mutex> lock(agent_policy_mutex());
		hook = agent_works_only_here_hook();
	}
	return hook && hook(agent_id);
}

// Why a request is kept on this computer: it was asked to stay here, it
// carries on work another computer began that this one cannot confirm
// (Chat/OperationOrigin), or its chat holds work kept here, which every
// request of the chat carries as its history.
enum class KeptHere { No, Asked, OriginUnconfirmed, Thread };

// The request is, or carries on, work kept on this computer: the operation it
// runs or continues, the task it runs again, or the agent whose work it is -
// named, or the one whose thread it is in.
KeptHere works_only_here(const std::string &operation_id, const ChatRequestComposition &composition) {
	std::optional<matters::Matter> project;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		project = matters::load_matter(node::require_active_workspace_path(), composition.project_id);
	}
	if (!project) return KeptHere::No;
	bool unconfirmed = false;
	const auto kept = [&](const std::string &operation) {
		if (operation.empty() || !routing::matter_request_execution::operation_works_only_here(*project, operation))
			return KeptHere::No;
		return chat::origin_unconfirmed_here(*project, operation) ? KeptHere::OriginUnconfirmed : KeptHere::Asked;
	};
	for (const std::string &operation : {operation_id, composition.continues_operation_id}) {
		const KeptHere reason = kept(operation);
		if (reason == KeptHere::Asked) return reason;
		unconfirmed = unconfirmed || reason == KeptHere::OriginUnconfirmed;
	}
	if (composition.retry_of_task_id)
		for (const matters::MatterTask &task : project->tasks) {
			if (task.id != *composition.retry_of_task_id || !chat::task_works_only_here(*project, task)) continue;
			if (!task.agent_operation_id || !chat::origin_unconfirmed_here(*project, *task.agent_operation_id))
				return KeptHere::Asked;
			unconfirmed = true;
		}
	std::string agent = composition.agent_id;
	if (agent.empty() && composition.chat_id)
		for (const matters::MatterChat &chat : project->chats)
			if (chat.id == *composition.chat_id) agent = chat.agent_id;
	if (!agent.empty() && agent_works_only_here(agent)) return KeptHere::Asked;
	// The request's history is every earlier turn of its chat: an answer, or an
	// agent's note, kept on this computer would leave it as history of a request
	// sent elsewhere.
	if (composition.chat_id && chat::chat_holds_work_kept_here(*project, *composition.chat_id)) return KeptHere::Thread;
	return unconfirmed ? KeptHere::OriginUnconfirmed : KeptHere::No;
}

// Where work kept on this computer runs (Beta 1 #44), decided once for the
// send and for the composer's state alike: why it is kept here, the local model
// it runs on, whether that model must be set for this request (AI Setup would
// choose a cloud one), or the sentence it stops with.
struct KeptHereDecision {
	KeptHere kept = KeptHere::No;
	std::optional<routing::ModelTarget> model;
	bool sets_model = false;
	std::string refusal;
};

// Work that runs only on this computer stays here on every path that carries
// it on - Resume, Retry, Run with another model, Edit and continue, Send again,
// a restart, a lawyer's message in the agent's thread - and so does work
// another computer began that this one cannot confirm. Its model is a local
// one: the one it asked for or, where the Chat or the Project's AI Setup would
// choose a cloud model, the model this computer has. Never the cloud: a cloud
// model chosen for it is refused, and with no model on this computer the work
// stops and says so. It only decides: it loads no model and changes nothing.
KeptHereDecision decide_kept_here(const std::string &operation_id, const ChatRequestComposition &composition) {
	KeptHereDecision decision;
	// Why it is kept here, said as it is: a Retry carries the mark of the work
	// it runs again, and the reason comes from that work.
	decision.kept = routing::ScopedLocalOnlyWork::is_active() ? KeptHere::Asked : works_only_here(operation_id, composition);
	if (decision.kept == KeptHere::No && composition.local_only) decision.kept = KeptHere::Asked;
	if (decision.kept == KeptHere::No) return decision;
	const bool unconfirmed = decision.kept == KeptHere::OriginUnconfirmed;
	if (composition.one_time_primary_model_override) {
		if (composition.one_time_primary_model_override->provider_id == "local") {
			decision.model = composition.one_time_primary_model_override;
			return decision;
		}
		decision.refusal = unconfirmed ? routing::matter_request_execution::kOriginUnconfirmedModelRefused
			: routing::matter_request_execution::kWorksOnlyHereModelRefused;
		return decision;
	}
	OperationSetupRequest setupRequest;
	setupRequest.project_id = composition.project_id;
	setupRequest.chat_id = composition.chat_id;
	setupRequest.legal_task_preset_id = composition.legal_task_preset_id;
	setupRequest.legal_task_preset_version = composition.legal_task_preset_version;
	const OperationSetup setup = resolve_operation_setup(setupRequest);
	if (setup.primary_model && setup.primary_model->provider_id == "local") {
		decision.model = setup.primary_model;
		return decision;
	}
	const std::optional<std::string> local = routing::preferred_local_generative_model();
	if (!local && unconfirmed)
		decision.refusal = "This work began on another computer, and this computer cannot confirm who began it "
			"or how it was allowed to run, so it stays here - and no model is installed on it. Nothing was sent. "
			"Confirm that colleague's computer in Team, or install a model in Models, then run it again.";
	else if (!local && decision.kept == KeptHere::Thread)
		decision.refusal = "This chat holds work kept on this computer, so what is asked in it stays here too - "
			"and no model is installed on it. Nothing was sent. Install a model in Models, or ask in a new chat.";
	else if (!local)
		decision.refusal = "This work runs only on this computer, and no model is installed on it, so nothing was "
			"sent. Install a model in Models, then run it again.";
	else {
		decision.model = routing::ModelTarget{"local", *local};
		decision.sets_model = true;
	}
	return decision;
}

void keep_on_this_computer(const std::string &operation_id, ChatRequestComposition &composition) {
	const KeptHereDecision decision = decide_kept_here(operation_id, composition);
	composition.local_only = decision.kept != KeptHere::No;
	if (!composition.local_only) return;
	if (!decision.refusal.empty()) throw std::runtime_error(decision.refusal);
	if (decision.sets_model) composition.one_time_primary_model_override = decision.model;
}

const char *kept_here_word(KeptHere kept) {
	switch (kept) {
		case KeptHere::Asked: return "asked";
		case KeptHere::OriginUnconfirmed: return "origin_unconfirmed";
		case KeptHere::Thread: return "thread";
		case KeptHere::No: break;
	}
	return "";
}

std::string kept_here_reason(KeptHere kept) {
	if (kept == KeptHere::Thread) return "This chat holds work kept on this computer, so what is asked in it runs here.";
	if (kept == KeptHere::OriginUnconfirmed)
		return "This work began on a computer this one cannot confirm yet, so it runs here.";
	return "This work runs only on this computer.";
}

ChatExecutionOutcome run_chat_operation(const std::string &operation_id,
		const ChatRequestComposition &requested, bool resuming) {
	// Work kept on this computer stays here whichever path carries it on; a
	// model it may not use is refused before anything is prepared or sent.
	ChatRequestComposition composition = requested;
	std::optional<std::string> keptHereRefusal;
	try {
		keep_on_this_computer(operation_id, composition);
	} catch (const std::exception &error) {
		keptHereRefusal = error.what();
	}
  install_pause_store();
  begin_operation(operation_id, composition);
  // Covers the WHOLE rest of this function, every exit path (normal return
  // or exception) - see ActiveExecutionGuard's own comment for why this,
  // not the reported ChatOperationState, is the real "still running" signal
  // begin_operation()/resume_chat_operation() must check to tell a genuine
  // concurrent double-send apart from a legitimate resume re-entry.
  const ActiveExecutionGuard activeExecutionGuard(registry(), operation_id);
	const OperationGrantsGuard operationGrantsGuard{composition.project_id, operation_id};
	const PauseReleaseGuard pauseReleaseGuard{composition.project_id, operation_id};
	// Every model call and tool of this run on this thread knows the work stays
	// here (the request's own records say so for every other boundary).
	const routing::ScopedLocalOnlyWork localOnlyWork(composition.local_only);
	// Every provider request this run makes on this thread answers to its Stop.
	const platform::HttpCancelScope httpCancelScope(http_cancel_token(operation_id));
	// Constructed here, on the executing worker thread, so its thread-local
	// event subscription observes exactly this operation (OperationEvent.hpp).
	// Forwards to any outer observer (the CLI trace) instead of replacing it.
	// Presentation only: it never asks production code for forensic payloads.
	routing::observability::ExecutionProgressTracker liveProgress(true, true, false);
	const LiveProgressAttachment liveProgressAttachment(registry(), operation_id, &liveProgress);
	// Each step the run takes is said to whoever shows it, at most four times a
	// second; its state changes are said as they happen (Beta 1 #263).
	auto stepSaidAt = std::make_shared<std::chrono::steady_clock::time_point>();
	const routing::observability::ScopedEventSink stepEvents(
		[projectId = composition.project_id, operation_id, stepSaidAt](const routing::observability::OperationEvent &) {
			const auto now = std::chrono::steady_clock::now();
			if (now - *stepSaidAt < std::chrono::milliseconds(250)) return;
			*stepSaidAt = now;
			publish_operation_changed(projectId, operation_id, std::nullopt);
		}, true, false);
	// The answer is shown while the model writes it. Only a call that writes the
	// lawyer's answer is shown - a review of a proposal or the Project's memory
	// is Singularity's own work - and the record keeps the last answer shown
	// until a later call writes a word of its own.
	const auto liveReader = std::make_shared<routing::live_answer::Reader>();
	const auto liveShown = std::make_shared<bool>(true);
	const routing::observability::ScopedEventSink liveTurns([liveShown](const routing::observability::OperationEvent &event) {
		if (event.kind != routing::observability::OperationEventKind::ModelTurnStart) return;
		for (const auto &[key, value] : event.details)
			if (key == "purpose")
				*liveShown = value != "professional_review" && value != "context_automation";
	}, true, false);
	auto liveStream = std::make_shared<providers::GenerationStream>();
	liveStream->begin = [liveReader] { liveReader->begin(); };
	liveStream->text = [liveReader, liveShown, operation_id](std::string_view piece) {
		if (*liveShown == false) return;
		liveReader->add(piece);
		std::string answer = liveReader->answer();
		if (answer.empty()) return;
		OperationRegistry &state = registry();
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end()) found->second.live_answer = std::move(answer);
	};
	const providers::GenerationStreamScope liveStreamScope(std::move(liveStream));
	const LiveAnswerRelease liveAnswerRelease{registry(), operation_id};
	// The diagnostic trace (Platform/Trace/Trace.hpp), when it is on, writes
	// every event of this operation with its time - the events' own safe
	// details only, never a forensic payload, and it never asks for one.
	std::optional<routing::observability::ScopedEventSink> traceSink;
	platform::trace::Span operationSpan("operation", "chat.operation", {{"operationId", operation_id}}, true);
	if (platform::trace::enabled()) {
		platform::trace::set_thread_label("chat-operation");
		traceSink.emplace([operation_id](const routing::observability::OperationEvent &event) {
			platform::trace::Fields fields;
			fields.emplace_back("operationId", operation_id);
			fields.emplace_back("message", event.message);
			for (const auto &detail : event.details) fields.emplace_back("d." + detail.first, detail.second);
			platform::trace::record("event", routing::observability::to_string(event.kind), fields);
		}, true, false);
	}
  routing::observability::emit({routing::observability::OperationEventKind::OperationStart,
      "Chat operation started", {{"operationId", operation_id}}, std::nullopt});

  ChatExecutionOutcome outcome;
  outcome.operation_id = operation_id;

  // Checkpoint 1 - before anything at all is resolved. A Stop that arrives
  // here costs the user nothing: no retrieval ran, no request was prepared,
  // no operation exists.
  // Singularity was already closing when this run was taken up: it does not
  // start. A resumed run's record stays as it stood - still waiting where the
  // lawyer's question or approval is kept, or interrupted with Resume, its
  // decided pause kept (PauseReleaseGuard) - and a new one leaves nothing.
  if (is_interrupted_by_shutdown(operation_id) == true) {
    outcome.state = ChatOperationState::Interrupted;
    outcome.message = "Singularity was closing, so this request did not start. Nothing was sent.";
    finish_operation(operation_id, outcome.state, std::nullopt, outcome.message);
    if (record_kept_wait(composition.project_id, operation_id) == false)
      persist_operation_transition(composition.project_id, operation_id, outcome.state, "interrupted",
        outcome.message, std::nullopt, std::nullopt, true);
    return outcome;
  }
  if (is_stop_requested(operation_id) == true) {
    outcome.state = ChatOperationState::Stopped;
    outcome.message = "You stopped this request before it started.";
    finish_operation(operation_id, outcome.state, std::nullopt, outcome.message);
		persist_operation_transition(composition.project_id, operation_id, outcome.state,
			"stopped", outcome.message, std::nullopt, true, false);
    return outcome;
  }

	if (keptHereRefusal) {
		finish_operation(operation_id, ChatOperationState::Failed, std::nullopt, *keptHereRefusal);
		throw std::runtime_error(*keptHereRefusal);
	}

  PreparedChatRequest prepared;
  try {
    // The accepted Phase 23 boundary, used exactly as Phase 23's preview
    // uses it. Send never trusts a preview that was prepared earlier: this
    // prepares the composer's CURRENT state again, so a source, task, model
    // or AI Setup change since the last Review governs what is actually
    // sent. It also re-runs Phase 21's fail-closed source contract, so a
    // request Phase 21 refuses can never reach execution.
    prepared = prepare_chat_request(composition);
  } catch (const std::exception &error) {
    finish_operation(operation_id, ChatOperationState::Failed, std::nullopt, error.what());
    throw;
  }
	{
		std::set<std::string> documents;
		for (const ResolvedChatSource &source : prepared.sources.sources) documents.insert(source.document_id);
		const std::lock_guard<std::mutex> lock(registry().mutex);
		const auto found = registry().records.find(operation_id);
		if (found != registry().records.end()) {
			if (prepared.setup.primary_model) {
				found->second.model_provider_id = prepared.setup.primary_model->provider_id;
				found->second.model_id = prepared.setup.primary_model->model_id;
			}
			found->second.document_count = static_cast<int>(documents.size());
			found->second.is_external_destination = prepared.is_external_destination;
		}
	}
	if (is_stop_requested(operation_id) == true) {
		outcome.state = ChatOperationState::Stopped;
		outcome.message = "You stopped this request before it was sent.";
		finish_operation(operation_id, outcome.state, std::nullopt, outcome.message);
		// A resumed run carries on a record that already exists, which keeps the
		// Stop; a new one has no record yet, and nothing is written.
		persist_operation_transition(composition.project_id, operation_id, outcome.state,
			"stopped", outcome.message, std::nullopt, true, false);
		return outcome;
	}

	// Source-policy and request-shape refusal precede durable operation
	// creation. Once preparation succeeds, the canonical MatterTask exists
	// before any inference, tool call or mutation can begin.
	try {
		attach_durable_task(operation_id, persist_new_operation(operation_id, composition, resuming));
	} catch (const std::exception &error) {
		finish_operation(operation_id, ChatOperationState::Failed, std::nullopt, error.what());
		throw;
	}
	if (after_operation_record_hook()) after_operation_record_hook()(operation_id);

  // Checkpoint 2 - the request is fully prepared but nothing has been sent.
  // This is a real cancellation window: source retrieval over a large
  // Project is genuinely the slow half of a local operation.
  if (is_stop_requested(operation_id) == true) {
    outcome.state = ChatOperationState::Stopped;
    outcome.message = "You stopped this request before it was sent.";
		// Only the record this run made is taken back. A resumed run's record holds
		// the turn that came before - its prompt, the questions asked and the
		// lawyer's answers, its usage and signed origin - and keeps the Stop.
		if (resuming == true)
			persist_operation_transition(composition.project_id, operation_id, outcome.state,
				"stopped", outcome.message, std::nullopt, true, false);
		else
			remove_unstarted_operation_task(composition.project_id, operation_id);
    finish_operation(operation_id, outcome.state, std::nullopt, outcome.message);
    return outcome;
  }

  advance_operation(operation_id, ChatOperationState::Working);
	persist_operation_transition(composition.project_id, operation_id, ChatOperationState::Working,
		"model", "Working on the request.");

  matters::Matter project;
  try {
    // The existing canonical execution path, entered with the canonical
    // params and nothing else. Everything Phase 24 needs from an operation -
    // Project Context and PromptAssembler, the Papinian Guard and final leak
    // boundary for an external target, real inference, response restoration,
    // SourceMarkerRegistry validation, the MatterTask itself and the
    // Usage/Routing evidence - is produced in here, by the boundary that has
    // always owned it.
    prepared.request_params["operationId"] = operation_id;
    project = routing::matter_request_execution::run_task(prepared.request_params);
  } catch (const std::exception &error) {
		if (is_interrupted_by_shutdown(operation_id) == true) {
			// Singularity closed while this attempt was running and it ended on the way
			// out: interrupted, never recorded as the lawyer's own Stop.
			// A run waiting on the lawyer's kept place is not interrupted: it still
			// waits (record_kept_wait).
			outcome.state = ChatOperationState::Interrupted;
			outcome.message = "Singularity closed while this request was running. Nothing further was sent.";
			finish_operation(operation_id, outcome.state, std::nullopt, outcome.message);
			if (record_kept_wait(composition.project_id, operation_id) == false)
				persist_operation_transition(composition.project_id, operation_id, outcome.state, "interrupted",
					outcome.message, std::nullopt, std::nullopt, true);
			return outcome;
		}
		if (is_stop_requested(operation_id) == true) {
			outcome.state = ChatOperationState::Stopped;
			outcome.message = "You stopped this request.";
			finish_operation(operation_id, outcome.state, std::nullopt, outcome.message);
			persist_operation_transition(composition.project_id, operation_id, outcome.state,
				"stopped", outcome.message, std::nullopt, true, false);
			return outcome;
		}
		// It ended on its way out of a wait nobody decided in time: it waits at
		// its kept place (Beta 1 #14).
		if (const auto kept = kept_wait_of(composition.project_id, operation_id)) {
			outcome.state = kept->first;
			outcome.message = kept->second;
			keep_for_lawyer(operation_id, outcome.state, std::nullopt, outcome.message);
			(void)record_kept_wait(composition.project_id, operation_id);
			return outcome;
		}
		const bool conflict = std::string(error.what()).find("conflict") != std::string::npos;
		const ChatOperationState terminal = conflict ? ChatOperationState::Conflict : ChatOperationState::Failed;
		// A typed refusal before anything was sent (e.g. a request the model's
		// known context capacity cannot hold) keeps its stable error code, and a
		// non-retryable one is never offered as resumable unchanged.
		const auto *typed = dynamic_cast<const providers::TaskError *>(&error);
		const bool resumable = !conflict && (typed == nullptr || providers::is_retryable(typed->code()));
    finish_operation(operation_id, terminal, std::nullopt, error.what());
		persist_operation_transition(composition.project_id, operation_id, terminal,
			conflict ? "conflict" : "failed", error.what(), std::nullopt, false, resumable, std::nullopt,
			typed != nullptr ? std::optional<providers::TaskErrorCode>(typed->code())
				: conflict ? std::nullopt : std::optional<providers::TaskErrorCode>(providers::TaskErrorCode::Unknown));
    throw;
  }

  const matters::MatterTask *task = created_task(project, operation_id);
  if (task == nullptr) {
    const std::string message = "Singularity could not record the result of this request.";
    finish_operation(operation_id, ChatOperationState::Failed, std::nullopt, message);
    throw std::runtime_error(message);
  }
  outcome.task_id = task->id;

  // Checkpoint 3 - the attempt is over. A Stop that arrived during it
  // cancelled the provider request in flight (HttpCancelToken); a local model's
  // generation is not interrupted and finishes first. Either way the Stop
  // takes effect here and is recorded durably on the operation it belongs to.
  if (is_interrupted_by_shutdown(operation_id) == true) {
    // Singularity closed while this attempt was in flight. The attempt it really
    // made is kept; it is recorded as interrupted, never as a person's Stop
    // and never as a current answer.
    // One waiting on the lawyer's kept place still waits (record_kept_wait).
    outcome.state = ChatOperationState::Interrupted;
    outcome.message = "Singularity closed while this request was running. Nothing further was sent.";
    finish_operation(operation_id, outcome.state, outcome.task_id, outcome.message);
    if (record_kept_wait(composition.project_id, operation_id) == false)
      persist_operation_transition(composition.project_id, operation_id, outcome.state, "interrupted",
        outcome.message, outcome.task_id, std::nullopt, true);
    return outcome;
  }
  if (is_stop_requested(operation_id) == true) {
		// A Stop that arrived after the change was already committed cannot undo
		// it: the operation is reported as completed, truthfully, never as a
		// stopped request that changed nothing.
		const bool committedChange = task->status != matters::TaskStatus::Error
			&& ((task->document_operation_checkpoint && task->document_operation_checkpoint->stage == "committed")
				|| task->artifact_change_kind.has_value() || task->direct_document_work_document_id.has_value());
		if (committedChange) {
			outcome.state = ChatOperationState::Completed;
			outcome.message = "Stop arrived after the change was applied, so the change is kept.";
			finish_operation(operation_id, outcome.state, outcome.task_id, outcome.message);
			persist_operation_transition(composition.project_id, operation_id, outcome.state,
				"completed", outcome.message, outcome.task_id, false, false);
			if (composition.chat_id) {
				(void)apply_automatic_title_if_default(composition.project_id, *composition.chat_id, composition.prompt);
			}
			return outcome;
		}
    mark_task_stopped_by_user(composition.project_id, task->id);
    outcome.state = ChatOperationState::Stopped;
		// A Stop that ended a wait for the lawyer's answer ended the request
		// there: nothing was answered after it and nothing was changed.
		const bool stoppedWhileAsking = std::any_of(task->lawyer_questions.begin(), task->lawyer_questions.end(),
			[](const matters::LawyerQuestionRecord &record) { return record.state == "asked"; });
    outcome.message = stoppedWhileAsking
        ? "You stopped this request while it was waiting for your answer. Nothing was changed."
        : task->status == matters::TaskStatus::Error
        ? "You stopped this request before it was answered. Nothing was changed."
        : "You stopped this request. It had already been sent and finished before it could be stopped, "
          "so it is recorded but is not presented as a current answer.";
    finish_operation(operation_id, outcome.state, outcome.task_id, outcome.message);
		persist_operation_transition(composition.project_id, operation_id, outcome.state,
			"stopped", outcome.message, outcome.task_id, true, false);
    return outcome;
  }

	// Nobody decided this run's question or web search in the time it may hold
	// its thread: it waits at the place it kept, as Singularity closing would
	// leave it, and the decision resumes it with no model call made twice
	// (Beta 1 #14).
	if (const auto kept = kept_wait_of(composition.project_id, operation_id)) {
		outcome.state = kept->first;
		outcome.message = kept->second;
		keep_for_lawyer(operation_id, outcome.state, outcome.task_id, outcome.message);
		(void)record_kept_wait(composition.project_id, operation_id);
		return outcome;
	}

	if (task->status == matters::TaskStatus::Error && task->operation_execution_state == "conflict") {
		outcome.state = ChatOperationState::Conflict;
		outcome.message = task->error.value_or("The document changed, so nothing was changed.");
		finish_operation(operation_id, outcome.state, outcome.task_id, outcome.message);
		persist_operation_transition(composition.project_id, operation_id, outcome.state,
			"conflict", outcome.message, outcome.task_id, false, false, std::nullopt, task->error_code);
		return outcome;
	}
  if (task->status == matters::TaskStatus::Error) {
    routing::observability::emit({routing::observability::OperationEventKind::OperationFailed,
        "Chat operation failed; the attempt remains recorded", {{"taskId", task->id}}, std::nullopt});
    outcome.state = ChatOperationState::Failed;
    outcome.message = task->error.value_or("This request could not be completed.");
    finish_operation(operation_id, outcome.state, outcome.task_id, outcome.message);
		persist_operation_transition(composition.project_id, operation_id, outcome.state,
			"failed", outcome.message, outcome.task_id, false,
			!task->error_code || providers::is_retryable(*task->error_code), std::nullopt, task->error_code);
    return outcome;
  }

	// Real product bug (caught live, this session's own test_lm acceptance):
	// task->status only ever distinguishes Success (a generation attempt ran
	// without throwing) from Error - it says nothing about whether a pending
	// mutating tool call inside that attempt still needs a human decision.
	// task->operation_execution_state is the finer-grained signal
	// MatterRequestExecution.cpp's own tool_execution_context.approval_requested
	// callback already sets to "waiting_for_approval" (and already durably
	// persists via persist_task_checkpoint() at the exact moment ToolBroker
	// returns ApprovalRequired - see that callback's own comment). Falling
	// through to the generic Completed path below without checking this
	// field re-persisted "completed" over that already-correct state one
	// revision later, producing a real, reproduced false success: EXIT 0,
	// state=completed, zero document mutation, zero DirectDocumentWork
	// lineage, zero WorkProduct, and the raw provider JSON left as the
	// user-visible response (DirectDocumentWork's own describe_direct_
	// document_work_result() is only ever reached AFTER a committed
	// mutation). This is generic to every mutating capability that requests
	// approval through the same tool_execution_context contract (Connected
	// Document Action included), not a DirectDocumentWork-only case - fixed
	// once, here, at the one place every attempt's outcome is decided.
	if (task->operation_execution_state == "waiting_for_approval") {
		outcome.state = ChatOperationState::WaitingForApproval;
		outcome.message = task->operation_activity;
		// The callback above already persisted this exact state durably -
		// re-invoking persist_operation_transition() here would just append a
		// second, redundant lifecycle event over the correct one. The live
		// in-memory registry still needs to agree, though, since operation_
		// status()'s own "found_live" branch reads it directly.
		advance_operation(operation_id, outcome.state);
		return outcome;
	}

  routing::observability::emit({routing::observability::OperationEventKind::OperationAccepted,
      "Chat operation completed", {{"taskId", task->id}}, std::nullopt});
  outcome.state = ChatOperationState::Completed;
  finish_operation(operation_id, outcome.state, outcome.task_id, outcome.message);
	persist_operation_transition(composition.project_id, operation_id, outcome.state,
		"completed", "Request completed.", outcome.task_id, false, false);
	if (composition.chat_id) {
		(void)apply_automatic_title_if_default(composition.project_id, *composition.chat_id, composition.prompt);
	}
	return outcome;
}

// The durable decision a checkpoint records for its approval: "pending",
// "approved", "rejected" or "withdrawn", or empty when it records none.
// The document already carries this operation's change, but its turn was never
// finished: Resume records it without writing again, and nothing may treat the
// operation as one that changed nothing.
bool committed_but_unfinished(const matters::MatterTask &task) {
	return task.document_operation_checkpoint && task.document_operation_checkpoint->stage == "committed"
		&& (task.status != matters::TaskStatus::Success || !task.response_text);
}

std::string checkpoint_approval_decision(const matters::MatterTask &task) {
	if (!task.document_operation_checkpoint) return {};
	const Json &draft = task.document_operation_checkpoint->draft;
	if (draft.contains("approval") && draft.at("approval").is_object())
		return draft.at("approval").value("decision", std::string("pending"));
	return draft.value("decision", std::string());
}

// True when this operation's own durable checkpoint is what is waiting on
// exactly this approval, and nothing has been answered on it yet. The document
// change is answered on a record that survives a quit, so an answer that
// arrives while the executing thread is still finishing must still be written
// there - never absorbed by an in-process session nobody is waiting on.
bool checkpoint_awaits_approval(const std::string &project_id, const std::string &operation_id,
		const std::string &approval_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace_path, project_id);
	if (!project) return false;
	const matters::MatterTask *task = find_operation_task(*project, operation_id);
	if (task == nullptr || !task->document_operation_checkpoint) return false;
	const Json &draft = task->document_operation_checkpoint->draft;
	if (!draft.contains("approval") || !draft.at("approval").is_object()) return false;
	if (draft.at("approval").value("approvalId", std::string()) != approval_id) return false;
	const std::string decision = checkpoint_approval_decision(*task);
	return decision.empty() || decision == "pending";
}

// A stop ends a paused operation's pending approval: it can never be granted
// later.
// An operation that paused for the lawyer's decision is logged when it
// resumes. One that ends at the decision instead - declined, or stopped while
// it waited - still spent what preparing the change cost, and Usage counts it
// like any other run. Logged once: an operation already in the Routing Log is
// not logged again.
void log_unresumed_run(const std::filesystem::path &workspace_path, const std::string &project_id,
		const matters::MatterTask &task, std::optional<providers::TaskErrorCode> code) {
	if (task.usage.total_tokens.value_or(0) <= 0 && !task.usage.cost_micros) return;
	try {
		for (const routing::RoutingLogEntry &logged : routing::load_routing_log(workspace_path))
			if (logged.operation_id == task.id) return;
		routing::RoutingLogEntry entry;
		entry.matter_id = project_id;
		entry.operation_id = task.id;
		entry.task_id = task.root_task_id.value_or(task.id);
		entry.attempt = task.attempt;
		entry.time = task.created_at;
		entry.provider_id = task.provider_id;
		entry.model_id = task.model_id;
		entry.execution_kind = task.execution_kind;
		entry.legal_task_preset_id = task.legal_task_preset_id;
		entry.legal_task_preset_version = task.legal_task_preset_version;
		entry.processing.local_operation_units = task.protected_api_applied ? 1 : 0;
		entry.processing.external_operation_units = task.provider_id ? 1 : 0;
		entry.privacy_metadata = task.privacy_metadata;
		entry.success = false;
		entry.latency_ms = worked_milliseconds(task).value_or(0);
		entry.usage = task.usage;
		entry.error_code = code;
		routing::append_routing_log_entry(workspace_path, entry);
	} catch (const std::exception &) {
		// Operational telemetry: it never blocks or undoes the lawyer's decision.
	}
}

// The lawyer's decision on a change waiting on its checkpoint, recorded on this
// computer - an approval only of a change this computer prepared, exactly as it
// prepared it: its action, document, version and content
// (Tool/OperationAuthority). The checkpoint is in the Project's .lm, which a
// shared folder lets others write: a change prepared elsewhere, or altered after
// it was shown, is never approved here, and nothing is written.
void decide_as_prepared_here(const std::string &project_id, const std::string &operation_id, const Json &draft,
		bool allowed) {
	const bool recorded = tools::record_decision_here(project_id, operation_id,
		tools::approval_binding_of_checkpoint(draft), allowed, platform::resolve_actor_identity().actor_id);
	// A rejection only ever stops a change: it stands wherever it was prepared.
	if (allowed && !recorded)
		throw tools::ApprovalRefused("approval_not_prepared_here",
			"This change was not prepared on this computer, or it changed after it was shown, so nothing was written. "
			"Send the request again to prepare it here.");
}

void withdraw_pending_approval(matters::MatterTask &task) {
	if (!task.document_operation_checkpoint) return;
	Json &draft = task.document_operation_checkpoint->draft;
	if (draft.contains("approval") && draft.at("approval").is_object()
			&& draft.at("approval").value("decision", std::string("pending")) == "pending")
		draft["approval"]["decision"] = "withdrawn";
}

} // namespace

namespace {

struct QuickTitleJob {
	std::string project_id;
	std::string chat_id;
	std::string prompt;
};

struct QuickTitleWorker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<QuickTitleJob> pending;
	std::thread thread;
	QuickChatTitleWriter writer;
	bool stopping = false;
};

// Deliberately leaked, like PrivacyActions' worker: it must outlive static
// teardown, because its thread may still be finishing a reply.
QuickTitleWorker &quick_title_worker() {
	static QuickTitleWorker *worker = new QuickTitleWorker();
	return *worker;
}

void write_quick_title(const QuickTitleJob &job, const QuickChatTitleWriter &writer) {
	const std::string title = chat_title_from_reply(writer(job.project_id, job.chat_id, job.prompt), job.prompt);
	if (title.empty()) return;
	(void)apply_generated_title_if_default(job.project_id, job.chat_id, title);
}

void run_quick_titles() {
	QuickTitleWorker &worker = quick_title_worker();
	for (;;) {
		QuickTitleJob job;
		QuickChatTitleWriter writer;
		{
			std::unique_lock<std::mutex> lock(worker.mutex);
			worker.wake.wait(lock, [&] { return worker.stopping || !worker.pending.empty(); });
			if (worker.stopping) return;
			job = std::move(worker.pending.front());
			worker.pending.pop_front();
			writer = worker.writer;
		}
		if (!writer) continue;
		try {
			write_quick_title(job, writer);
		} catch (...) {
			// No local model, no room for it, a reply that would not do: the chat
			// is simply named another way. Never anyone's error.
		}
	}
}

void request_quick_title(const ChatRequestComposition &composition) {
	if (!composition.chat_id || composition.prompt.empty()) return;
	QuickTitleWorker &worker = quick_title_worker();
	const std::lock_guard<std::mutex> lock(worker.mutex);
	if (!worker.writer || worker.stopping) return;
	worker.pending.push_back({composition.project_id, *composition.chat_id, composition.prompt});
	if (!worker.thread.joinable()) worker.thread = std::thread(run_quick_titles);
	worker.wake.notify_one();
}

// Whether this message opens the chat: it has no turn yet and nobody named it.
bool opens_chat(const ChatRequestComposition &composition) {
	if (!composition.chat_id) return false;
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		const auto project = matters::load_matter(workspace_path, composition.project_id);
		if (!project) return false;
		const auto chat = std::find_if(project->chats.begin(), project->chats.end(),
			[&](const matters::MatterChat &entry) { return entry.id == *composition.chat_id; });
		if (chat == project->chats.end() || chat->title_authority != matters::ChatTitleAuthority::Default) return false;
		return std::none_of(project->tasks.begin(), project->tasks.end(),
			[&](const matters::MatterTask &task) { return task.chat_id && *task.chat_id == *composition.chat_id; });
	} catch (...) {
		return false;
	}
}

} // namespace

void enable_quick_chat_titles(QuickChatTitleWriter writer) {
	QuickTitleWorker &worker = quick_title_worker();
	const std::lock_guard<std::mutex> lock(worker.mutex);
	worker.writer = std::move(writer);
}

void stop_quick_chat_titles() noexcept {
	QuickTitleWorker &worker = quick_title_worker();
	std::thread thread;
	{
		const std::lock_guard<std::mutex> lock(worker.mutex);
		worker.writer = nullptr;
		worker.stopping = true;
		worker.pending.clear();
		thread = std::move(worker.thread);
	}
	worker.wake.notify_all();
	// A reply already being written is not waited for at exit: the thread is
	// left to finish on its own (the worker is never destroyed).
	if (thread.joinable()) thread.detach();
}

std::optional<KeptHereModel> kept_here_model(const ChatRequestComposition &composition) {
	const KeptHereDecision decision = decide_kept_here(std::string(), composition);
	if (decision.kept == KeptHere::No) return std::nullopt;
	KeptHereModel kept;
	kept.reason = kept_here_word(decision.kept);
	kept.model = decision.model;
	kept.message = decision.refusal.empty() ? kept_here_reason(decision.kept) : decision.refusal;
	return kept;
}

void set_agent_works_only_here(std::function<bool(const std::string &agent_id)> works_only_here) {
	const std::lock_guard<std::mutex> lock(agent_policy_mutex());
	agent_works_only_here_hook() = std::move(works_only_here);
}

// A run that returned at its kept place leaves no live record: its status, its
// answer and its Stop read the operation's record and the place, as they do
// after a restart (Beta 1 #14). A decision that came meanwhile has marked it
// resuming, and the run that carries it on is under way.
ChatExecutionOutcome after_kept_wait(const std::string &operation_id, ChatExecutionOutcome outcome) {
	OperationRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	const auto found = state.records.find(operation_id);
	if (found != state.records.end() && found->second.kept_for_lawyer && !found->second.actively_executing
			&& !found->second.resuming_after_decision)
		state.records.erase(found);
	return outcome;
}

ChatExecutionOutcome run_chat_request(const std::string &operation_id,
                                     const ChatRequestComposition &composition) {
	// Housekeeping beside the request, never part of it: see the header.
	if (opens_chat(composition)) request_quick_title(composition);
	return after_kept_wait(operation_id, run_chat_operation(operation_id, composition, false));
}

ChatExecutionOutcome resume_chat_operation(const std::string &project_id,
		const std::string &operation_id) {
	{
		OperationRegistry &live = registry();
		const std::lock_guard<std::mutex> guard(live.mutex);
		const auto found = live.records.find(operation_id);
		// See ActiveExecutionGuard's own comment: `actively_executing`, never
		// the reported state string, is what tells a genuine concurrent
		// double-resume apart from the correct next step after approval.
		if (found != live.records.end() && found->second.actively_executing == true)
			throw std::runtime_error("This request is still running.");
	}
	ChatRequestComposition composition;
	// Everything before the run starts: a resume that carries the lawyer's
	// decision on and ends here is settled, never left reading "Working".
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		if (!project) throw std::runtime_error("This Project no longer exists.");
		matters::MatterTask *task = find_operation_task(*project, operation_id);
		// A run that waited on the lawyer when Singularity closed resumes from its
		// pause once they have decided (ResearchPause.hpp).
		const bool pausedDecision = task != nullptr && has_decided_pause(*task, project->directory_path);
		if (task == nullptr || (!task->document_operation_checkpoint && !pausedDecision))
			throw std::runtime_error("This request has no durable checkpoint to resume.");
		const ChatOperationState saved = operation_state_from_string(task->operation_execution_state);
		const std::string decision = checkpoint_approval_decision(*task);
		// A finished decision is reported, never executed again: a stopped or
		// denied request stays so, and a committed change is never applied twice.
		ChatExecutionOutcome finished;
		finished.operation_id = operation_id;
		finished.task_id = task->id;
		if (saved == ChatOperationState::Stopped || task->operation_cancel_requested || decision == "withdrawn") {
			finished.state = ChatOperationState::Stopped;
			finished.message = task->operation_activity;
			settle_unstarted_resume(project_id, operation_id, finished);
			return finished;
		}
		// IDEM-2: a change whose commit is already durable is never applied a
		// second time - but "the file was written" is not the same fact as "the
		// turn was finished". An operation interrupted between its commit and
		// its own completion has a changed document and no result: no answer in
		// the Chat, no Work Product, no lineage. Reporting that as Completed
		// leaves the lawyer with a silently edited document and nothing saying
		// so. It is finished here instead, from its own record, with zero model
		// calls (execute_task_attempt()'s commitAlreadyRecovered path).
		const bool turnFinished = saved == ChatOperationState::Completed
			&& task->status == matters::TaskStatus::Success && task->response_text.has_value();
		if (turnFinished) {
			finished.state = ChatOperationState::Completed;
			finished.message = task->operation_activity;
			settle_unstarted_resume(project_id, operation_id, finished);
			return finished;
		}
		if (decision == "rejected") {
			finished.state = ChatOperationState::Failed;
			finished.message = task->error.value_or("You rejected this change, so nothing was written.");
			settle_unstarted_resume(project_id, operation_id, finished);
			return finished;
		}
		if (saved == ChatOperationState::Conflict) {
			finished.state = ChatOperationState::Conflict;
			finished.message = task->error.value_or(task->operation_activity);
			settle_unstarted_resume(project_id, operation_id, finished);
			return finished;
		}
		if (decision == "pending")
			throw tools::ApprovalRefused("approval_required", "This change is still waiting for your approval.");
		if (is_terminal(saved) && saved != ChatOperationState::Interrupted
				&& saved != ChatOperationState::Failed && !task->operation_resume_allowed)
			throw std::runtime_error("This request is not waiting to be resumed.");
		composition.project_id = project_id;
		composition.chat_id = task->chat_id;
		composition.prompt = task->prompt;
		composition.selected_document_ids = task->selected_document_ids;
		composition.target_document_id = task->professional_target_document_id;
		composition.source_policy_mode = task->selected_document_ids.empty()
			? SourcePolicyMode::Relevant : SourcePolicyMode::Selected;
		composition.legal_task_preset_id = task->legal_task_preset_id;
		composition.legal_task_preset_version = task->legal_task_preset_version;
		if (task->requested_provider_id && task->requested_model_id)
			composition.one_time_primary_model_override = routing::ModelTarget{
				*task->requested_provider_id, *task->requested_model_id};
		// Resume carries the operation on as it began: its agent, and whoever
		// asked, stay as its record says. What it may do is this computer's own
		// record of it, which the request reads itself (Tool/OperationAuthority).
		composition.agent_id = task->agent_id;
		composition.agent_initiated = task->agent_initiated;
		if (task->agent_initiated) composition.agent_name = task->author_name;
		else if (!task->author_actor_id.empty()) {
			composition.author_actor_id = task->author_actor_id;
			composition.author_name = task->author_name;
		}
		composition.answers_message_id = task->answers_message_id;
		// A run that paused on the lawyer was a Chat request that could ask them:
		// it goes on able to, or the question it continues would not be read.
		if (pausedDecision) composition.can_ask_lawyer = true;
		// Work an AI colleague began itself goes on as nobody watches it: a next
		// wait keeps its place and returns again (Beta 1 #14).
		composition.unattended = task->agent_initiated;
	} catch (...) {
		settle_unstarted_resume(project_id, operation_id, std::nullopt);
		throw;
	}
	return after_kept_wait(operation_id, run_chat_operation(operation_id, composition, true));
}

ChatExecutionOutcome rerun_chat_task(const std::string &project_id, const std::string &task_id,
		const ChatRerunRequest &request) {
	ChatRequestComposition composition;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		if (!project) throw std::runtime_error("This Project no longer exists.");
		const auto original = std::find_if(project->tasks.begin(), project->tasks.end(),
			[&](const matters::MatterTask &task) { return task.id == task_id; });
		if (original == project->tasks.end()) throw std::runtime_error("This task no longer exists.");
		if (!original->chat_id) throw std::runtime_error("Only a Chat message can be retried or edited here.");
		if (original->consilium_group_id || original->workflow_run_id)
			throw std::runtime_error("This step is part of a Consilium or workflow and can't be edited here.");
		const bool revision = request.prompt.has_value() || request.source_policy_mode.has_value();
		// Work kept on this computer is never run again on a model outside it.
		const bool keptHere = chat::task_works_only_here(*project, *original);
		if (keptHere && request.model_target && request.model_target->provider_id != "local")
			throw std::runtime_error(original->agent_operation_id
				? routing::matter_request_execution::works_only_here_refusal(*project, *original->agent_operation_id)
				: routing::matter_request_execution::kWorksOnlyHereModelRefused);
		composition.local_only = keptHere;
		// Asking the same model again would write a written change twice. (Run
		// with another model redoes the same piece of work in place - SLR-3.)
		if (!revision && !request.model_target && matters::has_committed_document_change(*original))
			throw std::runtime_error("Its change is already in the document, so running it again would write it twice. Write a follow-up instead.");
		if (request.model_target) {
			if (matters::is_same_model_target(*original, request.model_target->provider_id, request.model_target->model_id))
				throw std::runtime_error(
					"Choose a different connected model - this is the same model the original task already used.");
		} else if (!revision && !matters::is_retry_eligible(*original)) {
			if (original->status == matters::TaskStatus::Success)
				throw std::runtime_error(
					"This task already completed. Choose Run with another model to compare a different model's output.");
			if (original->error_code == providers::TaskErrorCode::ChangeRejected)
				throw std::runtime_error(
					"You rejected this change, so nothing was written. Edit the instruction to ask for a different change.");
			throw std::runtime_error(
				"Retrying with the same settings won't fix this. Change the request or choose another model, then send again.");
		}
		if (request.prompt && request.prompt->find_first_not_of(" \t\r\n") == std::string::npos)
			throw std::runtime_error("Enter a message before continuing.");
		composition.project_id = project_id;
		composition.chat_id = original->chat_id;
		composition.prompt = request.prompt.value_or(original->prompt);
		const SourcePolicyMode mode = request.source_policy_mode.value_or(
			original->selected_document_ids.empty() ? SourcePolicyMode::Relevant : SourcePolicyMode::Selected);
		composition.source_policy_mode = mode;
		composition.selected_document_ids = request.source_policy_mode ? request.selected_document_ids
			: original->selected_document_ids;
		if (mode == SourcePolicyMode::Selected && composition.selected_document_ids.empty())
			throw std::runtime_error("Choose at least one document, or use Project default sources.");
		composition.target_document_id = original->professional_target_document_id;
		composition.legal_task_preset_id = original->legal_task_preset_id;
		composition.legal_task_preset_version = original->legal_task_preset_version;
		// Work kept here takes the model it ran on only when that model is this
		// computer's; otherwise this computer's own model runs it
		// (keep_on_this_computer) - a cloud model it once used is never asked
		// again on its behalf.
		const auto inherits = [&](const std::optional<std::string> &provider) {
			return provider && (!keptHere || *provider == "local");
		};
		if (request.model_target) composition.one_time_primary_model_override = request.model_target;
		else if (inherits(original->provider_id) && original->model_id)
			composition.one_time_primary_model_override = routing::ModelTarget{*original->provider_id, *original->model_id};
		// A failed attempt never recorded an answering model, only the one it
		// asked: a Retry asks that same model again, never whichever model the
		// Chat has been switched to since. (Edit and continue follows the Chat.)
		else if (!revision && inherits(original->requested_provider_id) && original->requested_model_id)
			composition.one_time_primary_model_override =
				routing::ModelTarget{*original->requested_provider_id, *original->requested_model_id};
		composition.retry_of_task_id = original->id;
		// It runs with no more authority than the operation it runs again had on
		// this computer; an agent's turn stays the agent's.
		composition.continues_operation_id = original->agent_operation_id.value_or(std::string());
		if (!revision) composition.agent_id = original->agent_id;
	}
	return run_chat_request("op_rerun_" + matters::generate_task_id(), composition);
}

std::size_t interrupt_live_operations() noexcept {
	struct LiveOperation {
		std::string project_id;
		std::string operation_id;
		std::optional<std::string> task_id;
	};
	std::vector<LiveOperation> live;
	{
		OperationRegistry &state = registry();
		const std::lock_guard<std::mutex> lock(state.mutex);
		for (auto &entry : state.records) {
			OperationRecord &record = entry.second;
			if (is_terminal(record.state)) continue;
			// A proposal paused for the lawyer's decision is not running: it is
			// already durable on its own checkpoint, nothing is interrupted, and it
			// is still there to approve or reject after Singularity opens again.
			if (record.state == ChatOperationState::WaitingForApproval) continue;
			live.push_back({record.project_id, entry.first, record.task_id});
			record.state = ChatOperationState::Interrupted;
			record.stop_requested = true;
			record.interrupted_by_shutdown = true;
			if (record.http_cancel != nullptr) record.http_cancel->cancel();
			record.actively_executing = false;
			record.message = "Singularity closed while this request was running.";
		}
	}
	for (const LiveOperation &operation : live) {
		// A run waiting on the lawyer's kept place is not interrupted: after the
		// next launch it waits for them where it stood (record_kept_wait).
		const bool isKeptWait = record_kept_wait(operation.project_id, operation.operation_id);
		try {
			// Waking an approval wait is what actually lets a worker return.
			tools::production_tool_broker().invalidate_operation(operation.project_id, operation.operation_id);
		} catch (const std::exception &) {
		}
		if (isKeptWait) continue;
		try {
			persist_operation_transition(operation.project_id, operation.operation_id,
				ChatOperationState::Interrupted, "interrupted",
				"Singularity closed while this request was running. Nothing further was sent.", operation.task_id,
				std::nullopt, true);
		} catch (const std::exception &) {
		}
	}
	return live.size();
}

void reset_operation_registry_for_testing() {
	OperationRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.records.clear();
}

void set_after_operation_record_hook_for_testing(std::function<void(const std::string &operation_id)> hook) {
	after_operation_record_hook() = std::move(hook);
}

std::vector<ChatOperationSummary> list_operations(const std::string &project_id) {
	// Which operations this process is genuinely executing right now. Everything
	// else that is non-terminal on disk was left there by a process that no
	// longer exists.
	std::set<std::string> live;
	{
		OperationRegistry &state = registry();
		const std::lock_guard<std::mutex> lock(state.mutex);
		for (const auto &entry : state.records)
			if (entry.second.project_id == project_id && !is_terminal(entry.second.state))
				live.insert(entry.first);
	}
	std::vector<ChatOperationSummary> summaries;
	const std::string thisDevice = this_device_id();
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		if (!project) throw std::runtime_error("This Project no longer exists.");
		bool changed = false;
		for (matters::MatterTask &task : project->tasks) {
			if (!task.agent_operation_id) continue;
			const ChatOperationState saved = operation_state_from_string(task.operation_execution_state);
			// In a Matter shared between computers, another computer's run is that
			// computer's: it is never called interrupted here, nor resumed here.
			const bool runsElsewhere = !task.operation_device_id.empty() && task.operation_device_id != thisDevice
				&& !is_terminal(saved);
			const bool committedButUnfinished = committed_but_unfinished(task);
			// Interrupted work stays listed until it is resumed or dismissed: it is
			// unfinished, and its instruction must not leave the Chat.
			if (is_terminal(saved) && saved != ChatOperationState::Interrupted && !committedButUnfinished) continue;
			const bool executing = live.count(*task.agent_operation_id) != 0;
			// A proposal waiting for the lawyer's decision is not interrupted by a
			// restart: its decision lives on its own checkpoint.
			const bool awaitingDecision = saved == ChatOperationState::WaitingForApproval && task.operation_approval
				&& checkpoint_approval_decision(task) == "pending";
			// Nor is a question or a web search waiting on the place its run kept
			// (record_kept_wait): it still waits for the lawyer, from when it was asked.
			const std::optional<Json> keptWait = executing ? std::nullopt
				: undecided_pause(task, project->directory_path);
			// Singularity may have closed before the run could put its record right:
			// it still waits, and its record says so (restore_kept_wait).
			if (keptWait && restore_kept_wait(task, *keptWait)) changed = true;
			if (!executing && !is_terminal(saved) && !awaitingDecision && !keptWait && !runsElsewhere) {
				// STAT-2: an operation nothing is executing any more is interrupted,
				// durably and once - never left reading as if it were still working.
				// A pending approval stays answerable: its decision lives on this
				// operation's own checkpoint, not in the process that asked for it.
				// When the work really stopped is its last recorded step, not this
				// relaunch: the hours Singularity was closed are not work.
				const std::string stoppedAt = task.operation_updated_at;
				task.operation_execution_state = to_string(ChatOperationState::Interrupted);
				task.operation_updated_at = workspaces::current_timestamp();
				task.operation_resume_allowed = true;
				if (task.operation_activity.empty() || saved != ChatOperationState::WaitingForApproval)
					task.operation_activity = "Singularity closed while this request was running.";
				append_operation_event(task, "operation.interrupted",
					"This request was interrupted before it finished.");
				if (!stoppedAt.empty()) task.operation_events.back().created_at = stoppedAt;
				changed = true;
			}
			ChatOperationSummary summary;
			summary.operation_id = *task.agent_operation_id;
			summary.task_id = task.id;
			summary.chat_id = task.chat_id;
			summary.prompt = task.prompt;
			summary.state = operation_state_from_string(task.operation_execution_state);
			summary.stage = task.operation_stage;
			summary.activity = task.operation_activity;
			summary.updated_at = task.operation_updated_at;
			summary.waiting_for_approval = checkpoint_approval_decision(task) == "pending";
			summary.change_committed = committedButUnfinished;
			summary.runs_elsewhere = runsElsewhere;
			// Resume is offered only where it can work: a saved checkpoint to
			// continue from, no decision still owed, and no Stop already accepted.
			summary.resume_allowed = !executing && task.document_operation_checkpoint.has_value()
				&& checkpoint_approval_decision(task) != "pending" && !task.operation_cancel_requested;
			// A run that waited on the lawyer when Singularity closed still waits on
			// them: its question or its web search is theirs to answer, and once
			// answered it goes on from where it stood (ResearchPause.hpp).
			if (runsElsewhere) summary.resume_allowed = false;
			if (!executing && !runsElsewhere) {
				if (const std::optional<Json> &paused = keptWait) {
					const bool isQuestion = paused->value("kind", std::string()) == "lawyer_question";
					summary.state = isQuestion ? ChatOperationState::WaitingForAnswer : ChatOperationState::WaitingForApproval;
					summary.waiting_for_approval = !isQuestion;
					summary.resume_allowed = false;
				} else if (has_decided_pause(task, project->directory_path)) {
					summary.resume_allowed = true;
				}
			}
			summaries.push_back(std::move(summary));
		}
		if (changed) {
			project->updated_at = workspaces::current_timestamp();
			matters::save_matter(workspace_path, *project);
		}
	}
	std::stable_sort(summaries.begin(), summaries.end(),
		[](const ChatOperationSummary &left, const ChatOperationSummary &right) {
			return left.updated_at > right.updated_at;
		});
	return summaries;
}

ChatOperationStatus request_stop(const std::string &project_id, const std::string &operation_id) {
  OperationRegistry &state = registry();
  ChatOperationStatus status;
	bool newly_requested = false;
	bool found_live = false;
	bool paused = false;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end() && found->second.project_id == project_id) {
			found_live = true;
			OperationRecord &record = found->second;
			// A Stop asked again changes nothing more: the first one cancelled the
			// request, ended its approvals and went on record. Each one written
			// again would be a whole save of the Project, and a new operation file
			// in its shared folder.
			if (is_terminal(record.state) == false && record.stop_requested == false) {
				record.stop_requested = true;
				// The provider request in flight, if any, is cancelled now.
				if (record.http_cancel != nullptr) record.http_cancel->cancel();
				tools::production_tool_broker().invalidate_operation(project_id, operation_id);
				newly_requested = true;
				// Nothing executes for an operation paused for approval: the Stop
				// takes effect now, not at a boundary no thread will reach.
				paused = !record.actively_executing;
				record.state = paused ? ChatOperationState::Stopped : ChatOperationState::Stopping;
				if (paused) record.message = "You stopped this request. Nothing was changed.";
			}
			status.is_known = true;
			status.state = record.state;
			status.is_active = is_terminal(record.state) == false;
			status.is_stop_requested = record.stop_requested;
			status.task_id = record.task_id;
			status.message = record.message;
		}
	}
	if (!found_live) {
		ChatOperationStatus durable = operation_status(project_id, operation_id);
		if (durable.is_known && (durable.state == ChatOperationState::Interrupted
				|| durable.state == ChatOperationState::WaitingForApproval
				|| durable.state == ChatOperationState::WaitingForAnswer)) {
			// A question or a web search kept waiting over a restart ends here:
			// nothing will continue it.
			const bool wasPausedForLawyer = durable.state == ChatOperationState::WaitingForAnswer
				|| (durable.state == ChatOperationState::WaitingForApproval && durable.approval
					&& durable.approval->effect == tools::ToolEffectClass::ExternalNetwork);
			try {
				discard_pause(project_id, operation_id);
			} catch (...) {
			}
			bool changeInDocument = false;
			{
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				const auto workspace_path = node::require_active_workspace_path();
				auto project = matters::load_matter(workspace_path, project_id);
				if (project) {
					if (matters::MatterTask *task = find_operation_task(*project, operation_id)) {
						changeInDocument = committed_but_unfinished(*task);
						if (!changeInDocument) {
							withdraw_pending_approval(*task);
							matters::save_matter(workspace_path, *project);
							log_unresumed_run(workspace_path, project_id, *task, std::nullopt);
						}
					}
				}
			}
			// The change is already in the document: a Stop cannot take it back and
			// must never record it as a request that changed nothing. Resume
			// records it in the Chat without writing anything again.
			if (changeInDocument) {
				durable.message = "This change is already in the document. Resume records it in this Chat.";
				return durable;
			}
			persist_operation_transition(project_id, operation_id, ChatOperationState::Stopped,
				"stopped", durable.state == ChatOperationState::WaitingForApproval || wasPausedForLawyer
					? "You stopped this request. Nothing was changed." : "You stopped this interrupted operation.",
				durable.task_id, true, false);
			return operation_status(project_id, operation_id);
		}
		if (!durable.is_known) {
			// The send may still be queued: remember the Stop, so the operation
			// stops at its first boundary if it starts. Nothing is claimed about
			// an operation Singularity has no record of.
			const std::lock_guard<std::mutex> lock(state.mutex);
			prune_terminal_records_locked(state);
			OperationRecord tombstone;
			tombstone.project_id = project_id;
			tombstone.state = ChatOperationState::Stopped;
			tombstone.stop_requested = true;
			tombstone.sequence = state.next_sequence++;
			state.records[operation_id] = std::move(tombstone);
		}
		return durable;
	}
	if (newly_requested && paused) {
		try {
			discard_pause(project_id, operation_id);
		} catch (...) {
		}
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			const auto workspace_path = node::require_active_workspace_path();
			auto project = matters::load_matter(workspace_path, project_id);
			if (project) {
				if (matters::MatterTask *task = find_operation_task(*project, operation_id)) {
					withdraw_pending_approval(*task);
					matters::save_matter(workspace_path, *project);
					log_unresumed_run(workspace_path, project_id, *task, std::nullopt);
				}
			}
		}
		persist_operation_transition(project_id, operation_id, ChatOperationState::Stopped,
			"stopped", "You stopped this request. Nothing was changed.", status.task_id, true, false);
	} else if (newly_requested) {
		persist_operation_transition(project_id, operation_id, ChatOperationState::Stopping,
			"stopping", "Stopping at the next safe boundary.", status.task_id, true, false);
	}
  return status;
}

ChatOperationStatus operation_status(const std::string &project_id, const std::string &operation_id) {
  OperationRegistry &state = registry();
  ChatOperationStatus status;
	bool found_live = false;
	std::optional<tools::ToolApprovalRequest> newly_pending_approval;
	bool newly_asked_question = false;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end() && found->second.project_id == project_id) {
			found_live = true;
			status.is_known = true;
			status.state = found->second.state;
			status.is_active = is_terminal(found->second.state) == false;
			status.is_stop_requested = found->second.stop_requested;
			status.task_id = found->second.task_id;
			status.message = found->second.message;
			status.model_provider_id = found->second.model_provider_id;
			status.model_id = found->second.model_id;
			status.document_count = found->second.document_count;
			status.is_external_destination = found->second.is_external_destination;
			status.questions = found->second.questions;
			status.live_answer = found->second.live_answer;
			const auto showProgress = [&](const routing::observability::ExecutionProgressSnapshot &snapshot,
					const std::vector<routing::observability::ExecutionProgressTransition> &transitions,
					long long waitedMs) {
				status.progress_stage_key = snapshot.stage_key;
				status.progress_stage_text = snapshot.stage_text;
				status.elapsed_ms = snapshot.elapsed.count() + waitedMs;
				status.stage_elapsed_ms = snapshot.stage_elapsed.count() + waitedMs;
				status.since_last_event_ms = snapshot.since_last_event.count() + waitedMs;
				status.candidate_document_count = snapshot.candidate_document_count;
				status.supplied_document_count = snapshot.supplied_document_count;
				for (const auto &transition : transitions)
					status.stage_history.emplace_back(transition.stage_key, transition.elapsed.count());
			};
			if (status.is_active == true && found->second.progress != nullptr) {
				showProgress(found->second.progress->snapshot(), found->second.progress->transitions(), 0);
			} else if (status.is_active == true && found->second.frozen_progress.has_value()) {
				// Still waiting where the run left it, as a live tracker parked in
				// that stage would read: its clock goes on, its steps stay.
				const long long waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - found->second.frozen_at).count();
				showProgress(*found->second.frozen_progress, found->second.frozen_transitions, waitedMs);
			}
			if (status.is_active == true) {
				const auto approval = tools::production_tool_broker().pending_approval(project_id, operation_id);
				if (approval.has_value() && approval->tool_id == tools::lawyer_question::kToolId) {
					// A question to the lawyer is not a change to approve: it is shown as
					// the question, and only its answer lets the attempt go on.
					status.state = ChatOperationState::WaitingForAnswer;
					const Json shown = Json::parse(approval->question_json, nullptr, false);
					status.asking = Json{{"id", approval->approval_id}, {"state", "asked"},
						{"title", shown.is_object() && shown.contains("title") ? shown.at("title") : Json(nullptr)},
						{"questions", shown.is_object() && shown.contains("questions") ? shown.at("questions") : Json::array()}};
					if (found->second.persisted_approval_id != approval->approval_id) {
						found->second.persisted_approval_id = approval->approval_id;
						newly_asked_question = true;
					}
				} else if (approval.has_value()) {
					status.state = ChatOperationState::WaitingForApproval;
					status.approval = approval;
					if (found->second.persisted_approval_id != approval->approval_id) {
						found->second.persisted_approval_id = approval->approval_id;
						newly_pending_approval = approval;
					}
				}
			}
		}
	}
	if (newly_pending_approval) {
		persist_operation_transition(project_id, operation_id, ChatOperationState::WaitingForApproval,
			"approval", newly_pending_approval->summary, status.task_id, false, true,
			newly_pending_approval);
	}
	// Saved once, like an approval: a question cannot be resumed after a
	// restart, so nothing resumable is recorded with it.
	if (newly_asked_question) {
		persist_operation_transition(project_id, operation_id, ChatOperationState::WaitingForAnswer,
			"question", "Singularity asked you a question.", status.task_id, false, false);
	}

	// Work this process is running now is answered from its live record without
	// ever waiting on a save: this is polled every second from the window, and
	// the worker holds the record's lock while it writes. Its durable part
	// (stage, events) is added only when the record is free this instant.
	std::unique_lock<std::recursive_mutex> freeNow(persistence::persistence_mutex(), std::defer_lock);
	if (found_live && status.is_active && !freeNow.try_lock()) return status;

	// Reconnect/process-restart fallback: read the canonical task instead of
	// fabricating an unknown state just because the live coordination cache is gone.
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace_path, project_id);
	if (!project) return status;
	matters::MatterTask *task = find_operation_task(*project, operation_id);
	if (task == nullptr) return status;
	status.is_known = true;
	const bool runsElsewhere = !found_live && !task->operation_device_id.empty() && task->operation_device_id != this_device_id()
		&& !is_terminal(operation_state_from_string(task->operation_execution_state));
	if (runsElsewhere) {
		// Another computer's Singularity runs it: what that computer last saved
		// is all this one knows, and it says so.
		status.state = operation_state_from_string(task->operation_execution_state);
		status.is_active = false;
		status.is_stop_requested = task->operation_cancel_requested;
		status.runs_elsewhere = true;
	} else if (!found_live) {
		status.state = operation_state_from_string(task->operation_execution_state);
		// With no process running it, a non-terminal operation is stopped if a
		// Stop was accepted, still waiting when its approval is still pending,
		// and otherwise interrupted. A web search that was waiting keeps no
		// checkpoint of its own: it waits again only on the place its run kept
		// (below) - without one nothing could carry its approval on, and the card
		// would take a decision it can only refuse.
		if (!is_terminal(status.state)) {
			const bool isWebResearch = task->operation_approval
				&& task->operation_approval->effect == tools::ToolEffectClass::ExternalNetwork
				&& !task->document_operation_checkpoint;
			if (task->operation_cancel_requested) status.state = ChatOperationState::Stopped;
			else if (status.state == ChatOperationState::WaitingForApproval && task->operation_approval && !isWebResearch
					&& (checkpoint_approval_decision(*task).empty() || checkpoint_approval_decision(*task) == "pending"))
				status.state = ChatOperationState::WaitingForApproval;
			else status.state = ChatOperationState::Interrupted;
		}
		status.is_active = false;
		status.is_stop_requested = task->operation_cancel_requested;
	}
	// A run that waited on the lawyer when Singularity closed: its question or web
	// search is in front of them again, exactly as they were shown it.
	std::optional<Json> paused;
	bool pausedDecided = false;
	if (!found_live && status.state != ChatOperationState::Stopped && status.state != ChatOperationState::Completed
			&& status.state != ChatOperationState::Failed && status.state != ChatOperationState::Conflict) {
		paused = undecided_pause(*task, project->directory_path);
		pausedDecided = !paused && has_decided_pause(*task, project->directory_path);
		if (paused && paused->value("kind", std::string()) == "lawyer_question") {
			status.state = ChatOperationState::WaitingForAnswer;
			status.asking = paused_asking(*paused);
		} else if (paused) {
			status.state = ChatOperationState::WaitingForApproval;
			status.approval = paused_research_approval(*paused, project_id, operation_id);
		}
	}
	status.task_id = task->id;
	if (status.message.empty()) status.message = task->operation_activity;
	if (paused) status.message = paused->value("kind", std::string()) == "lawyer_question"
		? "Singularity asked you a question." : task->operation_activity;
	// While this process owns the operation, the broker is authoritative for
	// whether an approval is still actionable. A durable approval snapshot is
	// only a reconnect aid; it must not resurrect a just-consumed live session.
	if (!status.approval && !found_live && status.state == ChatOperationState::WaitingForApproval)
		status.approval = task->operation_approval;
	status.revision = task->operation_revision;
	status.turn_contract_revision = task->turn_contract_revision;
	status.stage = task->operation_stage;
	status.resume_allowed = (task->document_operation_checkpoint.has_value() || pausedDecided)
		&& checkpoint_approval_decision(*task) != "pending"
		&& (task->operation_resume_allowed || (!found_live && status.state == ChatOperationState::Interrupted));
	if (paused) status.resume_allowed = false;
	if (status.state == ChatOperationState::Stopped || status.state == ChatOperationState::Completed)
		status.resume_allowed = false;
	if (runsElsewhere) status.resume_allowed = false;
	status.events = task->operation_events;
	status.questions = task->lawyer_questions;
	if (found_live) {
		const std::lock_guard<std::mutex> registryLock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end()) found->second.questions = status.questions;
	}
	// A question nothing waits on any more - the request finished, failed, was
	// stopped or Singularity closed without keeping its place - was never answered,
	// and says so. One its pause keeps waiting is still open.
	if (!status.is_active && !paused)
		for (matters::LawyerQuestionRecord &record : status.questions)
			if (record.state == "asked") record.state = "unanswered";
	// The answer a change was proposed with is kept on the saved approval
	// (MatterRequestExecution's attach_proposal_answer); the live approval the
	// broker holds is the same approval, and shows it as well.
	if (!paused) status.approval = with_saved_answer(status.approval, task->operation_approval);
	if (status.state == ChatOperationState::WaitingForApproval) {
		for (const context::ResolvedSourceReference &reference : task->source_references) {
			ChatCitation citation;
			citation.document_id = reference.document_id;
			citation.chunk_id = reference.chunk_id;
			citation.document_name = reference.document_name;
			citation.page = reference.page;
			citation.heading = reference.heading;
			citation.label = context::format_reference_label(reference);
			status.citations.push_back(std::move(citation));
		}
	}
	return status;
}

// The name of the Project document a waiting change would be written into,
// when another program holds it open for editing - nothing otherwise.
std::optional<std::string> approval_target_open_elsewhere(const std::string &project_id,
		const std::string &operation_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
	if (!project || !project->directory_path || project->directory_path->empty()) return std::nullopt;
	const matters::MatterTask *task = find_operation_task(*project, operation_id);
	if (task == nullptr) return std::nullopt;
	std::optional<std::string> target = task->operation_approval ? task->operation_approval->target_document_id : std::nullopt;
	if (!target && task->document_operation_checkpoint) {
		const Json &draft = task->document_operation_checkpoint->draft;
		if (draft.contains("approval") && draft.at("approval").is_object()
				&& draft.at("approval").contains("targetDocumentId") && draft.at("approval").at("targetDocumentId").is_string())
			target = draft.at("approval").at("targetDocumentId").get<std::string>();
	}
	if (!target) target = task->professional_target_document_id;
	if (!target) return std::nullopt;
	const auto document = std::find_if(project->documents.begin(), project->documents.end(),
		[&](const matters::MatterDocument &entry) { return entry.id == *target; });
	if (document == project->documents.end()) return std::nullopt;
	const std::filesystem::path path = std::filesystem::path(*project->directory_path) / document->relative_path;
	if (!platform::office_document_is_open(path)) return std::nullopt;
	return document->name;
}

namespace {

// Records the lawyer's decision on a pause - the question or the web search a
// run was waiting on when Singularity closed - bound to exactly that question or
// search, once, and only while nothing else has ended the operation. The run
// that resumes it is started by the caller (NativeBridge), once the decision
// is on record.
ChatOperationStatus record_paused_decision(const std::string &project_id, const std::string &operation_id,
		const std::string &approval_id, bool allowed, const std::optional<Json> &answers, bool is_question,
		bool for_request = false) {
	std::optional<std::filesystem::path> path;
	Json pause;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
		const matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		if (task == nullptr)
			throw tools::ApprovalRefused("approval_not_found", "This request is no longer in the Project.");
		const ChatOperationState saved = operation_state_from_string(task->operation_execution_state);
		if (task->operation_cancel_requested || saved == ChatOperationState::Stopped)
			throw tools::ApprovalRefused("operation_stopped", is_question
				? "This request was stopped, so its question can no longer be answered."
				: "This request was stopped, so its web search can no longer be approved.");
		path = pause_path(project->directory_path, operation_id);
		const std::optional<Json> kept = read_pause_at(path, operation_id);
		if (!kept)
			throw tools::ApprovalRefused("operation_not_waiting", is_question
				? "Singularity closed while this question was open and could not keep its place. Send the request again to "
					"continue."
				: "This web search is no longer waiting for your decision.");
		pause = *kept;
		if (pause.value("approvalId", std::string()) != approval_id
				|| pause.value("kind", std::string()) != (is_question ? "lawyer_question" : "research_approval"))
			throw tools::ApprovalRefused(is_question ? "question_mismatch" : "approval_mismatch", is_question
				? "This answer does not match the question Singularity is waiting on."
				: "This approval does not match the web search waiting for your decision.");
		if (research::is_decided_pause(pause))
			throw tools::ApprovalRefused("approval_already_decided", is_question
				? "This question was already answered." : "You already answered this approval.");
		if (is_question && undecided_pause(*task, project->directory_path) == std::nullopt)
			throw tools::ApprovalRefused("operation_not_waiting", "This question was already answered.");
	}
	Json decision{{"allowed", allowed}, {"decidedAt", workspaces::current_timestamp()}};
	// "Allow for this request": the resumed run's further web research is not
	// put to the lawyer again (ResearchToolLoop hands it to ToolBroker).
	if (is_question == false && allowed == true && for_request == true) decision["forRequest"] = true;
	if (is_question) {
		// The answer is checked against the question exactly as the lawyer was
		// shown it; one it cannot take leaves the question open.
		const Json question = pause.value("question", Json::object());
		const Json given = answers ? *answers : Json(nullptr);
		if (const std::optional<std::string> issue = tools::lawyer_question::answers_issue(question, given))
			throw tools::ApprovalRefused("answer_invalid", tools::lawyer_question::answer_issue_message(*issue));
		decision["answers"] = answers ? tools::lawyer_question::normalized_answers(question, *answers) : Json(nullptr);
	}
	pause["decision"] = decision;
	write_pause_at(*path, pause);
	const std::string activity = is_question ? "Continuing with your answer."
		: !allowed ? "Web search declined; continuing the request."
		: for_request ? "Web research allowed for the rest of this request; continuing the request."
		: "Web search approved; continuing the request.";
	// The operation reads as going on from this moment - in the list of
	// unfinished work too - and the run that resumes it takes it from here. A
	// web search's decision goes on its record now, as a live one does; a
	// question's answer is recorded by the resumed run itself, beside the
	// question (MatterRequestExecution's lawyer_question_answered).
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		if (matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id)) {
			++task->operation_revision;
			task->operation_execution_state = to_string(ChatOperationState::Working);
			task->operation_stage = "resuming";
			task->operation_activity = activity;
			task->operation_updated_at = workspaces::current_timestamp();
			if (is_question == false)
				append_operation_event(*task, allowed ? "approval.granted" : "approval.denied", activity, approval_id,
					std::string(allowed ? "approved" : "denied"));
			project->updated_at = task->operation_updated_at;
			matters::save_matter(workspace_path, *project);
		}
	}
	mark_resuming(project_id, operation_id, activity);
	return operation_status(project_id, operation_id);
}

} // namespace

namespace {

// A change prepared on another computer - or a pending approval written into a
// shared Matter's .lm - is never approved from its record (Tool/OperationAuthority).
// This computer prepares the step again from itself, with no model call, and
// the lawyer's approval stands only when what it prepares is exactly what they
// were shown: the same action, document, version, name and content, and the
// same description of the change. Otherwise the change is put to them again, as
// prepared here. nullopt when the change waiting is one this computer prepared:
// the ordinary approval answers it.
std::optional<ChatOperationStatus> approve_prepared_elsewhere(const std::string &project_id,
		const std::string &operation_id, const std::string &approval_id, bool for_request) {
	tools::ApprovalBinding shown;
	std::string shownDetail;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		if (task == nullptr || !task->document_operation_checkpoint || task->operation_cancel_requested)
			return std::nullopt;
		Json &draft = task->document_operation_checkpoint->draft;
		if (!draft.contains("approval") || !draft.at("approval").is_object()
				|| draft.at("approval").value("approvalId", std::string()) != approval_id
				|| checkpoint_approval_decision(*task) != "pending")
			return std::nullopt;
		shown = tools::approval_binding_of_checkpoint(draft);
		if (tools::prepared_here(project_id, operation_id, shown)) return std::nullopt;
		shownDetail = task->operation_approval ? task->operation_approval->change_detail_json : std::string();
		// The step is carried on here, from itself.
		draft.erase("approval");
		draft.erase("decision");
		task->operation_approval.reset();
		task->operation_execution_state = to_string(ChatOperationState::Interrupted);
		task->operation_resume_allowed = true;
		++task->operation_revision;
		task->operation_updated_at = workspaces::current_timestamp();
		append_operation_event(*task, "approval.prepared_again", "Prepared again on this computer for your approval.");
		project->updated_at = task->operation_updated_at;
		matters::save_matter(workspace_path, *project);
	}
	const ChatExecutionOutcome prepared = resume_chat_operation(project_id, operation_id);
	if (prepared.state != ChatOperationState::WaitingForApproval) return operation_status(project_id, operation_id);
	tools::ApprovalBinding again;
	std::string againDetail;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
		const matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		if (task != nullptr && task->document_operation_checkpoint) {
			again = tools::approval_binding_of_checkpoint(task->document_operation_checkpoint->draft);
			againDetail = task->operation_approval ? task->operation_approval->change_detail_json : std::string();
		}
	}
	const bool sameChange = !again.approval_id.empty() && again.tool_id == shown.tool_id
		&& again.proposal_digest == shown.proposal_digest && again.target_document_id == shown.target_document_id
		&& again.target_version == shown.target_version && again.target_fingerprint == shown.target_fingerprint
		&& again.new_document == shown.new_document && againDetail == shownDetail;
	if (!sameChange)
		throw tools::ApprovalRefused("approval_prepared_again",
			"This change was prepared on another computer, so Singularity prepared it again here. Nothing was written "
			"yet: review it as it stands now and approve it to write it.");
	return record_approval_response(project_id, operation_id, again.approval_id, true, for_request);
}

} // namespace

bool paused_decision_awaits_resume(const std::string &project_id, const std::string &operation_id) {
	{
		OperationRegistry &state = registry();
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end() && found->second.actively_executing) return false;
	}
	const std::optional<Json> pause = load_pause(project_id, operation_id);
	if (pause && research::is_decided_pause(*pause)) return true;
	// A change to a Project document the lawyer approved while nothing ran it
	// is written by carrying its operation on. That is part of the decision
	// itself, so no client has to ask for it, and one that closes before asking
	// never leaves an approved change unwritten (Beta 1 #262). A rejected change
	// needs nothing more: its operation ended with the decision.
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
	const matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
	return task != nullptr && task->document_operation_checkpoint
		&& task->document_operation_checkpoint->stage != "committed"
		&& checkpoint_approval_decision(*task) == "approved"
		&& task->operation_execution_state == to_string(ChatOperationState::Working)
		&& task->operation_stage == "approved";
}

ChatOperationStatus record_approval_response(const std::string &project_id, const std::string &operation_id,
		const std::string &approval_id, bool allowed, bool for_request) {
	// A question to the lawyer is answered, never approved or declined: its
	// answer comes through record_question_answer().
	const std::optional<tools::ToolApprovalRequest> waiting =
		tools::production_tool_broker().pending_approval(project_id, operation_id);
	if (waiting && waiting->approval_id == approval_id && waiting->tool_id == tools::lawyer_question::kToolId)
		throw tools::ApprovalRefused("approval_mismatch", "This is a question for you to answer, not a change to approve.");
	// Only web research is allowed for a whole request: every change to a
	// Project document is approved exactly, on its own.
	const bool isPausedResearch = [&] {
		if (waiting) return false;
		const std::optional<Json> pause = load_pause(project_id, operation_id);
		return pause && pause->value("kind", std::string()) == "research_approval"
			&& pause->value("approvalId", std::string()) == approval_id
			&& pause->value("effect", std::string()) == tools::to_string(tools::ToolEffectClass::ExternalNetwork);
	}();
	if (for_request && (!allowed || (!isPausedResearch && (!waiting || waiting->approval_id != approval_id
			|| waiting->effect != tools::ToolEffectClass::ExternalNetwork))))
		throw tools::ApprovalRefused("approval_scope",
			"Only web research can be allowed for the whole request. Approve this step on its own.");
	// A change is never written under a document Word holds open: Word would
	// save its own copy over it. The approval is refused and stays waiting,
	// so closing the document and approving again writes it.
	if (allowed) {
		if (const std::optional<std::string> open = approval_target_open_elsewhere(project_id, operation_id))
			throw tools::ApprovalRefused("document_open_elsewhere",
				platform::office_document_open_message(*open) + " The change is still waiting: close it, then choose "
				"Approve and write again.");
	}
	OperationRegistry &state = registry();
	bool live_wait = false;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end() && found->second.project_id == project_id) {
			if (found->second.stop_requested)
				throw tools::ApprovalRefused("operation_stopped",
					"This request was stopped, so its change can no longer be approved.");
			live_wait = found->second.actively_executing;
		}
	}
	// A decision nobody is waiting for, given while the run still executes, has
	// nowhere to land: no thread takes it, and the run's own last save goes
	// over it. A change to a Project document is never waited on by a thread -
	// it waits on the operation's checkpoint once the run returns - so a quick
	// approval of one was lost, and the turn read as completed with nothing
	// written (Beta 1 #125, LIFE-1). Such a decision is refused as a moment's
	// wait, and the change stays in front of the lawyer, to decide again. A web
	// search the research loop waits on is taken at once, as it always was.
	if (live_wait && waiting && waiting->approval_id == approval_id
			&& !tools::production_tool_broker().is_awaited(approval_id))
		throw tools::ApprovalRefused("change_still_preparing",
			"This change is still being prepared, so nothing was written. Decide it again in a moment.");
	// An operation still executing waits in-process for this exact approval -
	// the research loop's own boundaries genuinely park a thread. A Project
	// document change never does: it is parked on this operation's own
	// checkpoint, so an answer that arrives in the moment between the request
	// being surfaced and the executing thread returning still belongs on the
	// record, not only in this process.
	const bool durable_decision = live_wait
		&& checkpoint_awaits_approval(project_id, operation_id, approval_id);
	if (durable_decision) {
		std::optional<Json> draft;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
			const matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
			if (task != nullptr && task->document_operation_checkpoint) draft = task->document_operation_checkpoint->draft;
		}
		decide_as_prepared_here(project_id, operation_id, draft.value_or(Json::object()), allowed);
	}
	if (live_wait) {
		if (!waiting && !durable_decision && pause_names(project_id, operation_id, approval_id))
			refuse_while_keeping(false);
		if (!tools::production_tool_broker().respond_to_approval(project_id, operation_id, approval_id, allowed,
					nullptr, for_request && !durable_decision)
				&& !durable_decision)
			throw tools::ApprovalRefused("approval_mismatch",
				"This approval does not match the change waiting for your decision.");
	}
	if (live_wait && !durable_decision) {
		bool stillRunning = false;
		{
			const std::lock_guard<std::mutex> lock(state.mutex);
			const auto found = state.records.find(operation_id);
			if (found != state.records.end()) {
				found->second.persisted_approval_id.reset();
				// The worker was woken above and may already have finished: a
				// finished operation is never put back to Working.
				stillRunning = !found->second.stop_requested && !is_terminal(found->second.state);
				if (stillRunning) found->second.state = ChatOperationState::Working;
			}
		}
		if (stillRunning)
			persist_operation_transition(project_id, operation_id, ChatOperationState::Working,
				allowed ? "tool" : "approval",
				!allowed ? "The requested action was denied."
					: for_request ? "Web research allowed for the rest of this request; continuing the operation."
					: "Approval granted; continuing the operation.",
				std::nullopt, std::nullopt, false);
		return operation_status(project_id, operation_id);
	}
	// A web search that was waiting when Singularity closed is decided on the place
	// its run kept (ResearchPause.hpp); the resumed run goes on from there.
	if (live_wait == false) {
		const std::optional<Json> pause = load_pause(project_id, operation_id);
		if (pause && pause->value("kind", std::string()) == "research_approval"
				&& pause->value("approvalId", std::string()) == approval_id)
			return record_paused_decision(project_id, operation_id, approval_id, allowed, std::nullopt, false,
				for_request);
	}
	// A change this computer did not prepare is prepared again here before it
	// can be approved (approve_prepared_elsewhere).
	if (allowed && live_wait == false)
		if (const std::optional<ChatOperationStatus> carried = approve_prepared_elsewhere(project_id, operation_id,
				approval_id, for_request))
			return *carried;
	// A paused operation keeps its approval durably: the decision is bound to
	// that exact approval, recorded once, and only while the operation is
	// really waiting for it - never after a Stop, never a second, different
	// answer, never for another change.
	bool unchanged = false;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto project = matters::load_matter(workspace_path, project_id);
		matters::MatterTask *task = !project ? nullptr : find_operation_task(*project, operation_id);
		if (task == nullptr)
			throw tools::ApprovalRefused("approval_not_found", "This approval is no longer available.");
		const ChatOperationState saved = operation_state_from_string(task->operation_execution_state);
		if (task->operation_cancel_requested || saved == ChatOperationState::Stopped
				|| saved == ChatOperationState::Stopping)
			throw tools::ApprovalRefused("operation_stopped",
				"This request was stopped, so its change can no longer be approved.");
		// A web search with no place kept: its run ended when Singularity closed, and
		// nothing can carry an approval on. Said with what to do next.
		if (!task->document_operation_checkpoint)
			throw tools::ApprovalRefused("operation_not_waiting", task->operation_approval
					&& task->operation_approval->effect == tools::ToolEffectClass::ExternalNetwork
				? "Singularity closed while this web search waited and could not keep its place. Send the request again "
					"to continue."
				: "This request is no longer waiting for approval. Send the request again to continue.");
		Json &draft = task->document_operation_checkpoint->draft;
		const bool bound = draft.contains("approval") && draft.at("approval").is_object();
		const std::string boundApprovalId = bound ? draft.at("approval").value("approvalId", std::string())
			: task->operation_approval ? task->operation_approval->approval_id : std::string();
		if (boundApprovalId.empty())
			throw tools::ApprovalRefused("approval_not_found", "This approval is no longer available.");
		if (boundApprovalId != approval_id)
			throw tools::ApprovalRefused("approval_mismatch",
				"This approval does not match the change waiting for your decision.");
		const std::string decision = allowed ? "approved" : "rejected";
		const std::string recorded = checkpoint_approval_decision(*task);
		if (recorded == decision) {
			unchanged = true;
		} else {
			if (!recorded.empty() && recorded != "pending")
				throw tools::ApprovalRefused("approval_already_decided", "You already answered this approval.");
			if (saved != ChatOperationState::WaitingForApproval && saved != ChatOperationState::Interrupted
					&& saved != ChatOperationState::Working)
				throw tools::ApprovalRefused("operation_not_waiting", "This request is not waiting for approval.");
			decide_as_prepared_here(project_id, operation_id, draft, allowed);
			if (bound) {
				draft["approval"]["decision"] = decision;
				draft["approval"]["decidedAt"] = workspaces::current_timestamp();
				// The name a new document was shown under goes with the decision, so
				// it is written under exactly that name - an approval asked before
				// the binding carried it included.
				if (!draft["approval"].contains("newDocument") && task->operation_approval) {
					const Json shown = Json::parse(task->operation_approval->change_detail_json, nullptr, false);
					if (shown.is_object() && shown.contains("newDocument") && shown.at("newDocument").is_string())
						draft["approval"]["newDocument"] = shown.at("newDocument");
				}
			}
			draft["decision"] = decision;
			++task->operation_revision;
			task->operation_updated_at = workspaces::current_timestamp();
			if (allowed) {
				task->operation_execution_state = to_string(ChatOperationState::Working);
				task->operation_stage = "approved";
				task->operation_activity = "Approval granted; continuing the operation.";
				task->operation_resume_allowed = true;
				append_operation_event(*task, "approval.granted", task->operation_activity, approval_id, "approved");
			} else {
				task->operation_execution_state = to_string(ChatOperationState::Failed);
				task->operation_stage = "rejected";
				task->operation_activity = "You rejected this change, so nothing was written.";
				task->status = matters::TaskStatus::Error;
				task->error = task->operation_activity;
				task->error_code = providers::TaskErrorCode::ChangeRejected;
				task->operation_resume_allowed = false;
				task->rejected_approval = task->operation_approval;
				append_operation_event(*task, "approval.denied", task->operation_activity, approval_id, "rejected");
			}
			task->operation_approval.reset();
			project->updated_at = task->operation_updated_at;
			matters::save_matter(workspace_path, *project);
			if (!allowed) log_unresumed_run(workspace_path, project_id, *task, providers::TaskErrorCode::ChangeRejected);
		}
	}
	if (!unchanged) {
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end() && found->second.project_id == project_id) {
			found->second.persisted_approval_id.reset();
			found->second.state = allowed ? ChatOperationState::Working : ChatOperationState::Failed;
			if (!allowed) found->second.message = "You rejected this change, so nothing was written.";
		}
	}
	// The run that asked for this change filed it with the broker and ended
	// without waiting on it: the decision lives on the checkpoint. Its filing is
	// answered too, or every read of the resumed run would find it still open
	// and put the same approval card back in front of the lawyer who has just
	// decided it. Nothing waits on it, so the answer only closes it.
	(void)tools::production_tool_broker().respond_to_approval(project_id, operation_id, approval_id, allowed);
	return operation_status(project_id, operation_id);
}

ChatOperationStatus record_question_answer(const std::string &project_id, const std::string &operation_id,
		const std::string &question_id, const std::optional<Json> &answers) {
	// A question is answered while the attempt that asked it waits in this
	// process - or, when Singularity closed while it waited, on the place the
	// attempt kept (ResearchPause.hpp), and the resumed run goes on from there.
	OperationRegistry &state = registry();
	bool isLive = false;
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end() && found->second.project_id == project_id && found->second.stop_requested)
			throw tools::ApprovalRefused("operation_stopped",
				"This request was stopped, so its question can no longer be answered.");
		isLive = found != state.records.end() && found->second.project_id == project_id
			&& found->second.actively_executing;
	}
	if (isLive == false) return record_paused_decision(project_id, operation_id, question_id, true, answers, true);
	const std::optional<tools::ToolApprovalRequest> waiting =
		tools::production_tool_broker().pending_approval(project_id, operation_id);
	if (!waiting && pause_names(project_id, operation_id, question_id)) refuse_while_keeping(true);
	if (!waiting || waiting->tool_id != tools::lawyer_question::kToolId || waiting->approval_id != question_id)
		throw tools::ApprovalRefused("question_mismatch",
			"This answer does not match the question Singularity is waiting on.");
	// The answer is checked against the question exactly as the lawyer was
	// shown it. One it cannot take is refused and the question stays open, so
	// the lawyer can answer again; "Decide these yourself" is always taken.
	const Json question = Json::parse(waiting->question_json, nullptr, false);
	const Json given = answers ? *answers : Json(nullptr);
	if (const std::optional<std::string> issue = tools::lawyer_question::answers_issue(question, given))
		throw tools::ApprovalRefused("answer_invalid", tools::lawyer_question::answer_issue_message(*issue));
	// Given, never refused: the attempt goes on either way, with the answer or
	// with the decision left to it. Only Stop or Singularity closing ends a question
	// unanswered, so a refusal from the broker can only mean it was answered
	// already.
	const Json reply = answers ? tools::lawyer_question::normalized_answers(question, *answers) : Json(nullptr);
	if (!tools::production_tool_broker().respond_to_approval(project_id, operation_id, question_id, true, reply))
		throw tools::ApprovalRefused("operation_not_waiting", "This question was already answered.");
	// The waiting attempt records the answer on its own task as it continues
	// (the operation's lawyer_question_answered): only its thread writes the
	// question's record, so nothing written here can be overwritten by it.
	{
		const std::lock_guard<std::mutex> lock(state.mutex);
		const auto found = state.records.find(operation_id);
		if (found != state.records.end()) {
			found->second.persisted_approval_id.reset();
			// The attempt was woken above and may already have finished: a
			// finished operation is never put back to Working.
			if (!found->second.stop_requested && !is_terminal(found->second.state))
				found->second.state = ChatOperationState::Working;
		}
	}
	return operation_status(project_id, operation_id);
}

} // namespace singularity::chat
