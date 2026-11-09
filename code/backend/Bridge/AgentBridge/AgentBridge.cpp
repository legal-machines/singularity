#include "Bridge/AgentBridge/AgentBridge.hpp"

#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentRoles/AgentRoles.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Agent/AiAppConnections/AiAppConnections.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/AiSetupAssignment/AiSetupAssignment.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>

namespace singularity::bridge::agent_bridge {
namespace {

using Json = nlohmann::json;
using namespace singularity::agents;

std::string required_text(const Json &params, const char *key) {
	const auto found = params.find(key);
	if (found == params.end() || found->is_string() == false) throw std::invalid_argument(std::string("Missing ") + key + ".");
	return found->get<std::string>();
}

std::optional<std::string> optional_text(const Json &params, const char *key) {
	const auto found = params.find(key);
	if (found == params.end() || found->is_string() == false) return std::nullopt;
	return found->get<std::string>();
}

std::string daily_time_of(int minute) {
	char buffer[8];
	std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minute / 60, minute % 60);
	return buffer;
}

int minute_of(const std::string &time) {
	if (time.size() != 5 || time[2] != ':') throw AgentRefused("Choose a time of day for the daily check.");
	for (const std::size_t index : {0, 1, 3, 4})
		if (time[index] < '0' || time[index] > '9') throw AgentRefused("Choose a time of day for the daily check.");
	const int hours = (time[0] - '0') * 10 + (time[1] - '0');
	const int minutes = (time[3] - '0') * 10 + (time[4] - '0');
	if (hours > 23 || minutes > 59) throw AgentRefused("Choose a time of day for the daily check.");
	return hours * 60 + minutes;
}

AgentSettings settings_from(const Json &params) {
	AgentSettings settings;
	settings.matter_id = required_text(params, "projectId");
	settings.name = required_text(params, "name");
	settings.goal = optional_text(params, "goal").value_or("");
	settings.manner = optional_text(params, "manner").value_or("");
	settings.source_root = required_text(params, "sourceRoot");
	const std::optional<AgentTrigger> trigger = agent_trigger_from_string(required_text(params, "trigger"));
	if (trigger.has_value() == false) throw AgentRefused("Choose when the agent checks.");
	settings.trigger = *trigger;
	const std::optional<std::string> dailyTime = optional_text(params, "dailyTime");
	if (dailyTime.has_value() == true && dailyTime->empty() == false) settings.daily_minute = minute_of(*dailyTime);
	const std::optional<AgentModelPolicy> policy = agent_model_policy_from_string(required_text(params, "modelPolicy"));
	if (policy.has_value() == false) throw AgentRefused("Choose where the agent's analysis runs.");
	settings.model_policy = *policy;
	settings.kind = optional_text(params, "kind").value_or(kAgentKindDataRoomWatch);
	const std::optional<AgentMode> mode = agent_mode_from_string(optional_text(params, "mode").value_or("changes"));
	if (mode.has_value() == false) throw AgentRefused("Choose what the agent does.");
	settings.mode = *mode;
	const auto weeklyDay = params.find("weeklyDay");
	if (weeklyDay != params.end() && weeklyDay->is_number_integer() == true) settings.weekly_day = weeklyDay->get<int>();
	settings.follows_agent_id = optional_text(params, "followsAgentId").value_or("");
	settings.character = optional_text(params, "character").value_or("");
	settings.playbook_id = optional_text(params, "playbookId").value_or("");
	return settings;
}

std::string actor_id() {
	return platform::resolve_actor_identity().actor_id;
}

Json analysis_json(const RunAnalysis &analysis) {
	return {
		{"status", analysis.status},
		{"reason", analysis.reason},
		{"providerId", analysis.provider_id},
		{"modelId", analysis.model_id},
		{"destination", analysis.destination},
		{"protectionMode", analysis.protection_mode},
		{"evidenceSent", analysis.evidence_sent},
		{"findingsDropped", analysis.findings_dropped},
		{"operationId", analysis.operation_id.has_value() == true ? Json(*analysis.operation_id) : Json(nullptr)},
		{"inputTokens", analysis.input_tokens},
		{"outputTokens", analysis.output_tokens},
		{"cost", analysis.cost.has_value() == true ? Json(*analysis.cost) : Json(nullptr)},
		{"currency", analysis.currency},
	};
}

Json run_summary_json(const AgentRun &run) {
	return {
		{"id", run.id},
		{"trigger", run.trigger},
		{"status", run.status},
		{"stage", run.stage},
		{"progressDone", run.progress_done},
		{"progressTotal", run.progress_total},
		{"startedAt", run.started_at},
		{"finishedAt", run.finished_at},
		{"isBaseline", run.is_baseline},
		{"counts", {
			{"files", run.counts.files},
			{"added", run.counts.added},
			{"changed", run.counts.changed},
			{"removed", run.counts.removed},
			{"moved", run.counts.moved},
			{"unreadable", run.counts.unreadable},
			{"unreviewed", run.counts.unreviewed},
		}},
		{"findingsCount", run.findings.size()},
		{"needsReview", run_needs_review(run) == true && run.reviewed_at.has_value() == false},
		{"analysis", analysis_json(run.analysis)},
		{"task", run.task.has_value() == true ? Json{
			{"chatId", run.task->chat_id},
			{"taskId", run.task->task_id},
			{"state", run.task->state},
			{"answerExcerpt", run.task->answer_excerpt},
			{"message", run.task->message},
		} : Json(nullptr)},
		{"workProductId", run.work_product_id.has_value() == true ? Json(*run.work_product_id) : Json(nullptr)},
		{"reviewedAt", run.reviewed_at.has_value() == true ? Json(*run.reviewed_at) : Json(nullptr)},
		{"errorCode", run.error_code},
		{"errorMessage", run.error_message},
	};
}

Json requester_json(const AgentRequester &requester) {
	return {
		{"connectionId", requester.connection_id},
		{"connectionLabel", requester.connection_label},
		{"clientName", requester.client_name},
		{"signer", requester.signer},
		{"isSignerVerified", requester.is_signer_verified},
		{"requestedAt", requester.requested_at},
		{"requestedName", requester.requested_name},
		{"requestedGoal", requester.requested_goal},
		{"requestedFolder", requester.requested_folder},
		{"requestedTrigger", requester.requested_trigger},
	};
}

// The local day and month, as the agent's limits and spend count them.
std::string local_now(const char *format) {
	const std::time_t value = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &value);
#else
	localtime_r(&value, &local);
#endif
	char buffer[16];
	std::strftime(buffer, sizeof(buffer), format, &local);
	return buffer;
}

std::string month_of_this_computer() {
	return local_now("%Y-%m");
}

// The name of an agent this one follows, or nothing.
std::string agent_name_of(const std::string &agent_id) {
	if (agent_id.empty() == true) return "";
	try {
		const std::optional<Agent> agent = load_agent(agent_id);
		return agent.has_value() == true ? agent->name : "";
	} catch (const std::exception &) {
		return "";
	}
}

Json view_json(const AgentView &view) {
	const Agent &agent = view.agent;
	return {
		{"id", agent.id},
		{"projectId", agent.matter_id},
		{"kind", agent.kind},
		{"mode", to_string(agent.mode)},
		{"name", agent.name},
		{"character", agent.character},
		{"goal", agent.goal},
		{"manner", agent.manner},
		{"sourceRoot", agent.source_root},
		{"folderName", agent.source_root.empty() == true ? std::string() : std::filesystem::path(agent.source_root).filename().string()},
		{"trigger", to_string(agent.trigger)},
		{"dailyTime", daily_time_of(agent.daily_minute)},
		{"weeklyDay", agent.weekly_day},
		{"chatId", agent.chat_id.has_value() == true ? Json(*agent.chat_id) : Json(nullptr)},
		// Held here from the firm's record: changed on the computer that runs it.
		{"firmCopy", agent.is_firm_copy},
		// Where its own chat with the member is: the Matter it was made for, in the
		// zero state, or the member's desk for a colleague of the firm.
		{"chatProjectId", agent.chat_id.has_value() == true ? Json(home_project_of(agent, runner_dependencies())) : Json(nullptr)},
		{"modelPolicy", to_string(agent.model_policy)},
		// The model it thinks with, as its page set it; null for the firm's
		// default.
		{"model", agent.model_id.empty() == true ? Json(nullptr)
			: Json{{"providerId", agent.model_provider_id}, {"modelId", agent.model_id}}},
		// What gathers its memory: the model chosen for it, null for the firm's
		// model for context work; off, none.
		{"memoryModel", agent.memory_model_id.empty() == true ? Json(nullptr)
			: Json{{"providerId", agent.memory_provider_id}, {"modelId", agent.memory_model_id}}},
		{"memoryOff", agent.is_memory_off},
		{"playbookId", agent.playbook_id},
		{"reviewTableId", agent.review_table_id},
		{"status", to_string(agent.status)},
		{"state", view.state},
		{"isRunning", view.is_running},
		{"presence", view.presence},
		// The chat it writes an answer in now, as a messenger says "typing...";
		// null while it works, waits or rests.
		{"answeringChatId", view.is_running == true && view.latest_run.has_value() == true && view.latest_run->stage == "answering"
			&& view.latest_run->answering_chat_id.empty() == false ? Json(view.latest_run->answering_chat_id) : Json(nullptr)},
		{"office", {
			{"title", agent.office.title},
			{"ranksWith", agent.office.ranks_with},
			{"practice", agent.office.practice},
			{"area", agent.office.area},
			{"answersTo", agent.office.answers_to},
		}},
		{"revision", agent.revision},
		{"createdAt", agent.created_at},
		{"approvedAt", agent.approved_at},
		{"approvedByActorId", agent.approved_by_actor_id},
		{"requestedBy", agent.requested_by.has_value() == true ? requester_json(*agent.requested_by) : Json(nullptr)},
		{"lastCheckAt", agent.last_check_at.has_value() == true ? Json(*agent.last_check_at) : Json(nullptr)},
		{"nextCheckAt", agent.next_check_at.has_value() == true ? Json(*agent.next_check_at) : Json(nullptr)},
		{"latestRun", view.latest_run.has_value() == true ? run_summary_json(*view.latest_run) : Json(nullptr)},
		{"followsAgentId", agent.follows_agent_id},
		{"followsAgentName", agent_name_of(agent.follows_agent_id)},
		{"responsibleName", agent.responsible_name},
		{"failedInARow", agent.failed_in_a_row},
		{"pausedReason", agent.paused_reason},
		// A colleague never rests for a day's limit any more; kept for the pages
		// that read it.
		{"isRestingToday", false},
		{"spendThisMonth", agent.spend_month == month_of_this_computer() && agent.spend > 0.0 ? Json(agent.spend) : Json(nullptr)},
		{"spendCurrency", agent.spend_currency},
		{"assignmentsWaiting", agent.waiting_work.size()},
		// The conversations it has work from now, so a conversation's Stop
		// stands while it works on what was given there.
		{"workChatIds", chats_with_work(agent, view.latest_run, view.is_running)},
	};
}

// One piece of work given, as the page reads it: whose it is, where it stands
// in the agent's line, and what became of it - the run that did it and the
// answer in the agent's chat.
Json assignment_json(const AgentAssignment &work, const Agent &agent, const std::string &my_actor_id) {
	Json position = nullptr;
	for (std::size_t index = 0; index < agent.waiting_work.size(); ++index)
		if (agent.waiting_work[index] == work.id && work.state == "waiting") position = index + 1;
	Json result = nullptr;
	if (work.run_id.empty() == false && work.state != "waiting") {
		std::optional<AgentRun> run;
		try {
			run = load_run(agent.id, work.run_id);
		} catch (const std::exception &) {
		}
		if (run.has_value() == true) {
			result = {
				{"runId", run->id},
				{"status", run->status},
				{"chatId", run->task.has_value() == true ? Json(run->task->chat_id) : Json(nullptr)},
				{"taskState", run->task.has_value() == true ? Json(run->task->state) : Json(nullptr)},
				{"answerExcerpt", run->task.has_value() == true ? run->task->answer_excerpt : std::string()},
				{"workProductId", run->work_product_id.has_value() == true ? Json(*run->work_product_id) : Json(nullptr)},
				{"needsReview", run_needs_review(*run) == true && run->reviewed_at.has_value() == false},
			};
		}
	}
	return {
		{"id", work.id},
		{"instruction", work.instruction},
		{"givenByName", work.given_by_name},
		{"isGivenByYou", work.given_by_actor_id == my_actor_id},
		{"givenAt", work.given_at},
		{"state", work.state},
		{"position", position},
		{"chatId", work.chat_id},
		{"replyToMessageId", work.reply_to_message_id},
		{"startedAt", work.started_at},
		{"finishedAt", work.finished_at},
		{"message", work.message},
		{"result", result},
	};
}

std::string guard_mode_word() {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const protected_api::Mode mode = platform::load_app_settings().local_node_setup.protected_api_policy.mode;
	return mode == protected_api::Mode::Off ? "off" : "on";
}

} // namespace

Json list(const Json &params) {
	const std::optional<std::string> matterId = optional_text(params, "projectId");
	// The firm's colleagues, each with its own chat on the member's desk: those
	// another computer runs held here from the firm's record first.
	if (matterId.has_value() == false) {
		take_colleague_edits();
		adopt_firm_colleagues();
		open_missing_desk_threads();
	}
	Json agents = Json::array();
	for (const AgentView &view : list_agent_views(matterId)) agents.push_back(view_json(view));
	return {{"agents", agents}};
}

Json roles(const Json &) {
	Json list = Json::array();
	for (const AgentRole &role : agent_roles())
		list.push_back(Json{{"id", role.id}, {"mode", to_string(role.mode)}, {"label", role.label},
			{"description", role.description}, {"duty", role.duty}, {"name", role.name}, {"goal", role.goal},
			{"trigger", to_string(role.trigger)}, {"weeklyDay", role.weekly_day}, {"dailyTime", daily_time_of(role.daily_minute)},
			{"usesPlaybook", role.uses_playbook}});
	return {{"roles", list}};
}

Json get(const Json &params) {
	const AgentView view = agent_view(required_text(params, "agentId"));
	Json runs = Json::array();
	for (const AgentRun &run : load_runs(view.agent.id, 30)) runs.push_back(run_summary_json(run));
	Json assignments = Json::array();
	const std::string me = actor_id();
	for (const AgentAssignment &work : list_assignments(view.agent.id, 30)) assignments.push_back(assignment_json(work, view.agent, me));
	return {{"agent", view_json(view)}, {"runs", runs}, {"assignments", assignments}};
}

Json get_run(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	const std::optional<AgentRun> run = load_run(agentId, required_text(params, "runId"));
	if (run.has_value() == false) throw AgentRefused("This check no longer exists.");
	Json value = run_to_json(*run);
	value["summary"] = run_summary_json(*run);
	return value;
}

Json create(const Json &params) {
	return view_json(create_agent(settings_from(params), actor_id()));
}

// A colleague of the firm: its name, face, office, manner and standing task.
// It is never hired into a Matter, and nothing of its own is scheduled.
Json hire(const Json &params) {
	FirmColleagueHire hire;
	hire.name = required_text(params, "name");
	hire.character = optional_text(params, "character").value_or("");
	hire.manner = optional_text(params, "manner").value_or("");
	hire.goal = optional_text(params, "goal").value_or("");
	hire.office.title = optional_text(params, "title").value_or("");
	hire.office.ranks_with = optional_text(params, "ranksWith").value_or("");
	hire.office.practice = optional_text(params, "practice").value_or("");
	hire.office.area = optional_text(params, "area").value_or("");
	hire.office.answers_to = optional_text(params, "answersTo").value_or("");
	return view_json(hire_firm_colleague(hire, actor_id()));
}

Json edit(const Json &params) {
	FirmColleagueHire changed;
	changed.name = required_text(params, "name");
	changed.character = optional_text(params, "character").value_or("");
	changed.manner = optional_text(params, "manner").value_or("");
	changed.goal = optional_text(params, "goal").value_or("");
	changed.office.title = optional_text(params, "title").value_or("");
	changed.office.ranks_with = optional_text(params, "ranksWith").value_or("");
	changed.office.practice = optional_text(params, "practice").value_or("");
	changed.office.area = optional_text(params, "area").value_or("");
	changed.office.answers_to = optional_text(params, "answersTo").value_or("");
	return view_json(edit_firm_colleague(required_text(params, "agentId"), changed));
}

// The administrator's change of a colleague from any of their
// computers: the same words as agent.edit.
Json request_edit(const Json &params) {
	FirmColleagueHire changed;
	changed.name = required_text(params, "name");
	changed.character = optional_text(params, "character").value_or("");
	changed.manner = optional_text(params, "manner").value_or("");
	changed.goal = optional_text(params, "goal").value_or("");
	changed.office.title = optional_text(params, "title").value_or("");
	changed.office.ranks_with = optional_text(params, "ranksWith").value_or("");
	changed.office.practice = optional_text(params, "practice").value_or("");
	changed.office.area = optional_text(params, "area").value_or("");
	changed.office.answers_to = optional_text(params, "answersTo").value_or("");
	return view_json(request_colleague_edit(required_text(params, "agentId"), changed));
}

Json archive(const Json &params) {
	const auto archived = params.find("archived");
	if (archived == params.end() || archived->is_boolean() == false) throw std::invalid_argument("archived must be true or false.");
	return view_json(archive_colleague(required_text(params, "agentId"), archived->get<bool>()));
}

Json remove(const Json &params) {
	delete_colleague(required_text(params, "agentId"));
	return Json{{"deleted", true}};
}

// What a colleague remembers, each note with what it is about and who said
// it: a member sees what it keeps, and lets go of what it should not.
Json memory_json(const std::string &agent_id) {
	Json notes = Json::array();
	for (const MemoryNote &note : memory_of(agent_id)) {
		notes.push_back({
			{"id", note.id},
			{"kind", note.kind},
			{"scope", note.scope},
			{"matterId", note.matter_id},
			{"matterName", note.matter_name},
			{"personName", note.person_name},
			{"personActorId", note.person_actor_id},
			{"text", note.text},
			{"at", note.at},
			{"saidByName", note.said_by_name},
			{"saidByActorId", note.said_by_actor_id},
		});
	}
	return Json{{"agentId", agent_id}, {"notes", notes}};
}

Json firm_colleagues(const Json &) {
	std::set<std::string> here;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (const Agent &agent : load_agents()) here.insert(agent.id);
	}
	Json list = Json::array();
	for (const FirmColleague &colleague : agents::firm_colleagues()) {
		if (here.count(colleague.agent_id) > 0 || colleague.status == "stopped" || colleague.status == "deleted") continue;
		list.push_back({
			{"agentId", colleague.agent_id},
			{"name", colleague.name},
			{"character", colleague.character},
			{"goal", colleague.goal},
			{"manner", colleague.manner},
			{"office", {{"title", colleague.office.title}, {"ranksWith", colleague.office.ranks_with},
				{"practice", colleague.office.practice}, {"area", colleague.office.area}, {"answersTo", colleague.office.answers_to}}},
			{"modelProviderId", colleague.model_provider_id},
			{"modelId", colleague.model_id},
			{"memoryProviderId", colleague.memory_provider_id},
			{"memoryModelId", colleague.memory_model_id},
			{"memoryOff", colleague.is_memory_off},
			{"status", colleague.status},
			{"responsibleActorId", colleague.responsible_actor_id},
			{"responsibleName", colleague.responsible_name},
			{"createdAt", colleague.created_at},
			{"definedAt", colleague.defined_at},
		});
	}
	return Json{{"colleagues", std::move(list)}};
}

Json memory(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	(void)agent_view(agentId);
	return memory_json(agentId);
}

Json forget_memory(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	agents::forget_memory(agentId, required_text(params, "noteId"));
	return memory_json(agentId);
}

Json approve(const Json &params) {
	return view_json(approve_draft(required_text(params, "agentId"), settings_from(params), actor_id()));
}

Json decline(const Json &params) {
	return view_json(decline_draft(required_text(params, "agentId")));
}

Json pause(const Json &params) {
	return view_json(pause_agent(required_text(params, "agentId")));
}

// The model a colleague thinks with: one a connected account gives for the
// firm's reasoning, checked as AI Setup checks it, or the firm's default when
// none is named.
Json set_model(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	const std::string providerId = optional_text(params, "providerId").value_or("");
	const std::string modelId = optional_text(params, "modelId").value_or("");
	if (providerId.empty() == false && modelId.empty() == false)
		routing::validate_role_model_assignment(routing::AiSetupRole::Reasoning, routing::ModelTarget{providerId, modelId});
	return view_json(set_agent_model(agentId, providerId, modelId));
}

// What gathers a colleague's memory: a model a connected account gives for the
// firm's context work, checked as AI Setup checks it; the firm's own for
// context when none is named; none when `off`.
Json set_memory_model(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	const std::string providerId = optional_text(params, "providerId").value_or("");
	const std::string modelId = optional_text(params, "modelId").value_or("");
	const auto off = params.find("off");
	const bool isOff = off != params.end() && off->is_boolean() == true && off->get<bool>() == true;
	if (isOff == false && providerId.empty() == false && modelId.empty() == false)
		routing::validate_role_model_assignment(routing::AiSetupRole::Context, routing::ModelTarget{providerId, modelId});
	return view_json(set_agent_memory_model(agentId, providerId, modelId, isOff));
}

Json resume(const Json &params) {
	return view_json(resume_agent(required_text(params, "agentId")));
}

Json stop(const Json &params) {
	return view_json(stop_agent(required_text(params, "agentId")));
}

Json update(const Json &params) {
	const auto revision = params.find("expectedRevision");
	if (revision == params.end() || revision->is_number_integer() == false) throw std::invalid_argument("Missing expectedRevision.");
	return view_json(update_agent(required_text(params, "agentId"), revision->get<long long>(), settings_from(params)));
}

Json set_office(const Json &params) {
	Agent::Office office;
	office.title = optional_text(params, "title").value_or("");
	office.ranks_with = optional_text(params, "ranksWith").value_or("");
	office.practice = optional_text(params, "practice").value_or("");
	office.area = optional_text(params, "area").value_or("");
	office.answers_to = optional_text(params, "answersTo").value_or("");
	return view_json(set_agent_office(required_text(params, "agentId"), office));
}

Json run(const Json &params) {
	return view_json(request_agent_run(required_text(params, "agentId")));
}

Json cancel_run(const Json &params) {
	return view_json(cancel_agent_run(required_text(params, "agentId")));
}

Json mark_reviewed(const Json &params) {
	return view_json(mark_run_reviewed(required_text(params, "agentId"), required_text(params, "runId"), actor_id()));
}

Json post_message(const Json &params) {
  std::vector<std::string> mentions;
  if (params.contains("mentions")) {
    if (!params.at("mentions").is_array()) throw std::invalid_argument("mentions must be a list.");
    for (const Json &mention : params.at("mentions")) {
      if (!mention.is_string()) throw std::invalid_argument("mentions must be a list of ids.");
      mentions.push_back(mention.get<std::string>());
    }
  }
  const std::string projectId = params.at("projectId").get<std::string>();
  const std::string chatId = params.at("chatId").get<std::string>();
  // What it carries: the message it replies to, the documents it hands over
  // with what was not checked, the answers it offers, or the answer it gives,
  // and the place in a document it is about.
  chat::MessageExtras extras;
  if (params.contains("quoteMessageId") && params.at("quoteMessageId").is_string())
    extras.quote_message_id = params.at("quoteMessageId").get<std::string>();
  if (params.contains("notChecked") && params.at("notChecked").is_string())
    extras.not_checked = params.at("notChecked").get<std::string>();
  if (params.contains("answerOption") && params.at("answerOption").is_string())
    extras.answer_option = params.at("answerOption").get<std::string>();
  if (params.contains("askOptions")) {
    if (!params.at("askOptions").is_array()) throw std::invalid_argument("askOptions must be a list.");
    for (const Json &option : params.at("askOptions")) {
      if (!option.is_string()) throw std::invalid_argument("askOptions must be a list of answers.");
      extras.ask_options.push_back(option.get<std::string>());
    }
  }
  if (params.contains("attachmentDocumentIds")) {
    if (!params.at("attachmentDocumentIds").is_array()) throw std::invalid_argument("attachmentDocumentIds must be a list.");
    for (const Json &id : params.at("attachmentDocumentIds")) {
      if (!id.is_string()) throw std::invalid_argument("attachmentDocumentIds must be a list of ids.");
      extras.attachment_document_ids.push_back(id.get<std::string>());
    }
  }
  if (params.contains("place") && !params.at("place").is_null()) {
    const Json &place = params.at("place");
    if (!place.is_object()) throw std::invalid_argument("place must be an object.");
    const auto text_of = [&place](const char *key) {
      if (!place.contains(key) || place.at(key).is_null()) return std::string();
      if (!place.at(key).is_string()) throw std::invalid_argument(std::string("place.") + key + " must be text.");
      return place.at(key).get<std::string>();
    };
    extras.place.document_id = text_of("documentId");
    if (extras.place.document_id.empty()) throw std::invalid_argument("place.documentId names the document.");
    extras.place.section = text_of("section");
    extras.place.heading = text_of("heading");
    extras.place.quote = text_of("quote");
  }
  const matters::MatterTask message = chat::post_member_message(projectId, chatId, params.at("text").get<std::string>(),
      mentions, extras);
  agents::MentionMessage mentioned;
  mentioned.matter_id = projectId;
  mentioned.chat_id = chatId;
  mentioned.message_id = message.id;
  mentioned.text = chat::words_for_colleagues(projectId, message);
  mentioned.author_actor_id = message.author_actor_id;
  mentioned.author_name = message.author_name;
  // Those it names, and the colleague whose message it replies to.
  mentioned.mentions = chat::addressed_colleagues(projectId, message);
  // Just posted here by the person at this computer.
  mentioned.posted_here = true;
  return Json{{"messageId", message.id}, {"createdAt", message.created_at},
      {"agentsTakingItUp", agents::take_up_mentions(mentioned)}};
}

Json assign(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	const std::string me = actor_id();
	const AgentAssignment work = assign_work(agentId, required_text(params, "instruction"), me);
	return assignment_json(work, agent_view(agentId).agent, me);
}

Json withdraw_assignment(const Json &params) {
	const std::string agentId = required_text(params, "agentId");
	const AgentAssignment work = agents::withdraw_assignment(agentId, required_text(params, "assignmentId"));
	return assignment_json(work, agent_view(agentId).agent, actor_id());
}

// Where a new agent's analysis would run, said before the lawyer approves it:
// the model, whether it leaves this computer, whether protection applies, and
// what stands in the way now.
Json preview_setup(const Json &params) {
	const std::string projectId = required_text(params, "projectId");
	const std::optional<AgentModelPolicy> policy = agent_model_policy_from_string(required_text(params, "modelPolicy"));
	if (policy.has_value() == false) throw AgentRefused("Choose where the agent's analysis runs.");
	std::optional<routing::ModelTarget> model;
	if (*policy == AgentModelPolicy::LocalOnly) {
		const std::optional<std::string> local = routing::preferred_local_generative_model();
		if (local.has_value() == true) model = routing::ModelTarget{"local", *local};
	} else {
		chat::OperationSetupRequest request;
		request.project_id = projectId;
		model = chat::resolve_operation_setup(request).primary_model;
	}
	Json preview = {
		{"modelPolicy", to_string(*policy)},
		{"providerId", nullptr},
		{"modelId", nullptr},
		{"destination", "none"},
		{"protection", "not_applicable"},
		{"isReady", false},
		{"issue", ""},
	};
	if (model.has_value() == false) {
		preview["issue"] = *policy == AgentModelPolicy::LocalOnly
			? "No local model is installed. Install one in Models, or let the agent use this Matter's model."
			: "No AI model is set for this colleague yet. The firm's administrator chooses it on the colleague's page.";
		return preview;
	}
	const bool isLocal = model->provider_id == "local";
	preview["providerId"] = model->provider_id;
	preview["modelId"] = model->model_id;
	preview["destination"] = isLocal == true ? "local" : "online";
	preview["protection"] = isLocal == true ? "not_applicable" : guard_mode_word();
	routing::InferenceRequest request;
	request.model_target = model;
	try {
		routing::require_execution_ready(request);
		preview["isReady"] = true;
	} catch (const providers::TaskError &error) {
		preview["issue"] = error.code() == providers::TaskErrorCode::PrivacyProtectionUnavailable
			? "Protection is not ready yet. The agent will list changes; it analyses them once protection is ready."
			: "This model cannot be reached right now. The agent will list changes and analyse them when it can.";
	} catch (const std::exception &) {
		preview["issue"] = "This model cannot be reached right now. The agent will list changes and analyse them when it can.";
	}
	return preview;
}

std::optional<std::string> matter_folder_for(const Json &params) {
	const RunnerDependencies &dependencies = runner_dependencies();
	if (!dependencies.matter_folder) return std::nullopt;
	const std::optional<MatterFolder> matter = dependencies.matter_folder(required_text(params, "projectId"));
	if (matter.has_value() == false) return std::nullopt;
	return matter->folder.string();
}

Json chosen_folder(const Json &params, const std::optional<std::string> &folder) {
	if (folder.has_value() == false) return nullptr;
	AgentSettings probe;
	probe.matter_id = required_text(params, "projectId");
	probe.name = "Folder check";
	probe.source_root = *folder;
	// The same check an agent's creation makes, so the page says at once why
	// a folder cannot be watched.
	const AgentSettings checked = checked_settings(probe);
	return {{"path", checked.source_root}, {"name", std::filesystem::path(checked.source_root).filename().string()}};
}

namespace {

Json ai_app_json(const AiAppConnection &connection) {
	Json activity = Json::array();
	for (const AiAppActivity &entry : ai_app_activity(connection.id, 20))
		activity.push_back({{"at", entry.at}, {"tool", entry.tool}, {"isRefused", entry.is_refused}, {"detail", entry.detail}});
	return {
		{"id", connection.id},
		{"label", connection.label},
		{"projectIds", connection.matter_ids},
		{"mayPauseAgents", connection.may_pause_agents},
		{"createdAt", connection.created_at},
		{"revokedAt", connection.revoked_at.has_value() == true ? Json(*connection.revoked_at) : Json(nullptr)},
		{"lastUsedAt", connection.last_used_at.has_value() == true ? Json(*connection.last_used_at) : Json(nullptr)},
		{"lastClientName", connection.last_client_name},
		{"boundSigner", connection.bound_signer.empty() == true ? "" : signer_organisation(connection.bound_signer)},
		{"draftsProposed", connection.drafts_proposed},
		{"activity", activity},
	};
}

// The helper ships beside the application's own executable.
std::string helper_path() {
#if defined(_WIN32)
	constexpr const char *kHelperName = "singularity-mcp.exe";
#else
	constexpr const char *kHelperName = "singularity-mcp";
#endif
	return (platform::executable_path().parent_path() / kHelperName).string();
}

bool is_helper_present() {
	std::error_code error;
	return std::filesystem::is_regular_file(helper_path(), error);
}

Json setup_json(const AiAppSetup &setup) {
	return {{"aiApp", ai_app_json(setup.connection)}, {"key", setup.key}, {"helperPath", helper_path()}, {"isHelperPresent", is_helper_present()}};
}

} // namespace

Json ai_apps(const Json &) {
	Json apps = Json::array();
	for (const AiAppConnection &connection : agents::list_ai_apps()) apps.push_back(ai_app_json(connection));
	return {{"aiApps", apps}, {"helperPath", helper_path()}, {"isHelperPresent", is_helper_present()}};
}

Json connect_app(const Json &params) {
	const auto projects = params.find("projectIds");
	if (projects == params.end() || projects->is_array() == false) throw std::invalid_argument("Missing projectIds.");
	std::vector<std::string> projectIds;
	for (const Json &project : *projects) {
		if (project.is_string() == false) throw std::invalid_argument("Missing projectIds.");
		projectIds.push_back(project.get<std::string>());
	}
	const auto pause = params.find("mayPauseAgents");
	const bool mayPause = pause != params.end() && pause->is_boolean() == true && pause->get<bool>() == true;
	return setup_json(agents::connect_ai_app(required_text(params, "label"), projectIds, actor_id(), mayPause));
}

Json renew_app_key(const Json &params) {
	return setup_json(agents::renew_ai_app_key(required_text(params, "aiAppId")));
}

Json disconnect_app(const Json &params) {
	return {{"aiApp", ai_app_json(agents::revoke_ai_app(required_text(params, "aiAppId")))}};
}

std::string run_file_path(const Json &params) {
	const std::optional<Agent> agent = load_agent(required_text(params, "agentId"));
	if (agent.has_value() == false) throw AgentRefused("This agent no longer exists.");
	const std::string relative = required_text(params, "relativePath");
	if (relative.empty() == true || relative.front() == '/' || relative.find("..") != std::string::npos)
		throw AgentRefused("This file is not in the watched folder.");
	std::error_code error;
	const std::filesystem::path root = std::filesystem::canonical(agent->source_root, error);
	if (error) throw AgentRefused("The watched folder cannot be found.");
	// "." is the watched folder itself.
	if (relative == ".") return root.string();
	const std::filesystem::path file = std::filesystem::weakly_canonical(root / relative, error);
	if (error) throw AgentRefused("This file cannot be found.");
	const std::string rootText = root.string() + "/";
	if (file.string().rfind(rootText, 0) != 0) throw AgentRefused("This file is not in the watched folder.");
	if (std::filesystem::exists(file, error) == false) throw AgentRefused("This file is no longer in the watched folder.");
	return file.string();
}

} // namespace singularity::bridge::agent_bridge
