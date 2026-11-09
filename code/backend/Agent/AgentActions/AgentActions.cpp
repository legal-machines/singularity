#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentRoles/AgentRoles.hpp"

#include "Agent/AgentCard/AgentCard.hpp"
#include "Agent/AgentScheduler/AgentScheduler.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Agent/Types/AgentIdentity.hpp"
#include "Organisation/Types/FirmOffice.hpp"
#include "Platform/TextFold/TextFold.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <tuple>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>

namespace singularity::agents {
namespace {

constexpr std::size_t kMostNameCharacters = 80;
constexpr std::size_t kMostGoalCharacters = 2000;

RunnerDependencies &installed() {
	static RunnerDependencies dependencies;
	return dependencies;
}

std::string trimmed(const std::string &text) {
	const std::size_t start = text.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) return "";
	const std::size_t end = text.find_last_not_of(" \t\r\n");
	return text.substr(start, end - start + 1);
}

std::size_t characters_in(const std::string &text) {
	std::size_t count = 0;
	for (const char byte : text)
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++count;
	return count;
}

bool is_inside(const std::filesystem::path &child, const std::filesystem::path &parent) {
	auto childPart = child.begin();
	for (auto parentPart = parent.begin(); parentPart != parent.end(); ++parentPart, ++childPart) {
		if (parentPart->empty() == true) continue;
		if (childPart == child.end() || *childPart != *parentPart) return false;
	}
	return true;
}

// Loads an agent for a command, under the store's lock; refuses one that is
// gone.
Agent require_agent(const std::string &agent_id) {
	const std::optional<Agent> agent = load_agent(agent_id);
	if (agent.has_value() == false) throw AgentRefused("This agent no longer exists.");
	return *agent;
}

std::optional<AgentRun> latest_run_of(const Agent &agent) {
	if (agent.last_run_id.has_value() == false) return std::nullopt;
	try {
		return load_run(agent.id, *agent.last_run_id);
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

AgentView view_of(const Agent &agent) {
	AgentView view;
	view.agent = agent;
	view.latest_run = latest_run_of(agent);
	// One run at a time on this computer: an agent waiting its turn behind
	// another's reads as running - its work is begun - but is not under way, so
	// its character does not act out work it is not doing, and Stop is not
	// offered for a run that has not begun.
	view.is_running = AgentScheduler::shared().is_executing(agent.id);
	view.state = agent_state(agent, view.latest_run, AgentScheduler::shared().is_checking(agent.id));
	view.presence = agent_presence(view.state, view.is_running, view.latest_run.has_value() == true ? view.latest_run->stage : std::string());
	return view;
}

void save_changed(Agent &agent) {
	agent.revision += 1;
	agent.updated_at = agent_timestamp_now();
	save_agent(agent);
}

void schedule_first(Agent &agent) {
	if (agent.trigger == AgentTrigger::Daily)
		agent.next_check_at = next_daily_check(agent.daily_minute, std::chrono::system_clock::now());
	else if (agent.trigger == AgentTrigger::Weekly)
		agent.next_check_at = next_weekly_check(agent.weekly_day, agent.daily_minute, std::chrono::system_clock::now());
	else
		agent.next_check_at.reset();
}

// The roles a colleague can be begun in: the one catalogue (Agent/AgentRoles).
bool is_known_kind(const std::string &kind) {
	return agent_role(kind) != nullptr;
}

// A character of its own when the lawyer chose none: from its id, so two
// agents rarely look alike.
std::string default_character(const std::string &agent_id) {
	std::size_t hash = 0;
	for (const char character : agent_id) hash = hash * 131 + static_cast<unsigned char>(character);
	const std::size_t count = sizeof(kAgentCharacters) / sizeof(kAgentCharacters[0]);
	return kAgentCharacters[hash % count];
}

bool is_known_character(const std::string &value) {
	for (const char *known : kAgentCharacters)
		if (value == known) return true;
	for (const char *known : kAgentSpecies)
		if (value == known) return true;
	return false;
}

void apply_settings(Agent &agent, const AgentSettings &checked) {
	agent.matter_id = checked.matter_id;
	agent.name = checked.name;
	if (checked.character.empty() == false) agent.character = checked.character;
	if (agent.character.empty() == true && agent.id.empty() == false) agent.character = default_character(agent.id);
	agent.goal = checked.goal;
	agent.manner = checked.manner;
	agent.source_root = checked.source_root;
	agent.trigger = checked.trigger;
	agent.daily_minute = checked.daily_minute;
	agent.weekly_day = checked.weekly_day;
	agent.model_policy = checked.model_policy;
	agent.kind = checked.kind;
	agent.mode = checked.mode;
	agent.follows_agent_id = checked.trigger == AgentTrigger::AfterAgent ? checked.follows_agent_id : std::string();
	// Another Playbook is another review: the next run makes its own table.
	if (agent.playbook_id != checked.playbook_id) agent.review_table_id.clear();
	agent.playbook_id = checked.playbook_id;
}

// The lawyer's name as their own Settings or account give it, or nothing.
std::string person_name_now() {
	const RunnerDependencies &dependencies = installed();
	if (!dependencies.person_name) return "";
	try {
		return dependencies.person_name();
	} catch (const std::exception &) {
		return "";
	}
}

// The colleagues whose chat could not open at their last try - no Matters
// folder yet, as onboarding hires its first colleague before the folder is
// chosen - for the scheduler to try again at each wake until it opens
// (its member was told it could not be written to from this computer yet).
std::mutex &chatless_mutex() {
	static std::mutex mutex;
	return mutex;
}
std::set<std::string> &chatless() {
	static std::set<std::string> waiting;
	return waiting;
}

// The agent's thread: made and introduced for a new agent, its brief brought
// up to date for a changed one (RunnerDependencies::open_thread). Never with
// a lock held: it writes the Matter, which may be a slow share.
void open_thread_for(const std::string &agent_id) {
	const RunnerDependencies &dependencies = installed();
	if (!dependencies.open_thread) return;
	bool isWaiting = false;
	try {
		std::optional<Agent> agent;
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			agent = load_agent(agent_id);
		}
		// A colleague of the firm opens its own chat on the member's desk
		// (RunnerDependencies::desk_project).
		if (agent.has_value() == true && agent->status != AgentStatus::Draft && agent->status != AgentStatus::Stopped)
			isWaiting = dependencies.open_thread(*agent).empty();
	} catch (const std::exception &) {
	}
	const std::lock_guard<std::mutex> lock(chatless_mutex());
	if (isWaiting == true) chatless().insert(agent_id);
	else chatless().erase(agent_id);
}

// Who approved it is answerable for it, under the name their own Settings or
// account give.
void make_responsible(Agent &agent, const std::string &actor_id) {
	agent.responsible_actor_id = actor_id;
	agent.responsible_name = person_name_now();
}

// Work an agent was given and had not begun, taken back - when it stops.
void withdraw_waiting_work(Agent &agent, const std::string &why) {
	for (const std::string &id : agent.waiting_work) {
		try {
			std::optional<AgentAssignment> work = load_assignment(agent.id, id);
			if (work.has_value() == false || work->state != "waiting") continue;
			work->state = "withdrawn";
			work->finished_at = agent_timestamp_now();
			work->message = why;
			save_assignment(*work);
		} catch (const std::exception &) {
		}
	}
	agent.waiting_work.clear();
}

// An office in the firm's words, checked: its lengths, and a position of the
// firm's one scale (Organisation/Types/FirmOffice), as a person holds one.
Agent::Office checked_office(const Agent::Office &office) {
	Agent::Office checked;
	checked.title = trimmed(office.title);
	checked.practice = trimmed(office.practice);
	checked.area = trimmed(office.area);
	// No member it "answers to": an AI colleague is set over no one's
	// shoulder, so none is kept, whatever was asked.
	checked.answers_to.clear();
	if (characters_in(checked.title) > kMostNameCharacters) throw AgentRefused("The office's name is longer than 80 characters.");
	if (characters_in(checked.practice) > kMostNameCharacters * 2)
		throw AgentRefused("The practice's name is longer than 160 characters.");
	if (characters_in(checked.area) > kMostNameCharacters * 10) throw AgentRefused("The area is longer than 800 characters.");
	// It ranks with a position of the firm's one scale, as a person holds one.
	const std::string ranks = trimmed(office.ranks_with);
	if (ranks.empty() == false) {
		const int rank = organisation::position_rank(ranks);
		if (rank < 0) throw AgentRefused("Choose the position it ranks with from the firm's positions.");
		checked.ranks_with = organisation::firm_positions()[static_cast<std::size_t>(rank)];
	}
	return checked;
}

} // namespace

// Installed once by the window, and again by tests while the scheduler's
// threads read them.
std::mutex &installed_mutex() {
	static std::mutex mutex;
	return mutex;
}

void install_runner_dependencies(RunnerDependencies dependencies) {
	const std::lock_guard<std::mutex> lock(installed_mutex());
	installed() = std::move(dependencies);
}

std::function<void()> runner_take_up_mentions() {
	const std::lock_guard<std::mutex> lock(installed_mutex());
	return installed().take_up_mentions;
}

const RunnerDependencies &runner_dependencies() {
	return installed();
}

std::string agent_state(const Agent &agent, const std::optional<AgentRun> &latest_run, bool is_running) {
	if (agent.status == AgentStatus::Draft) return "waiting_for_approval";
	if (agent.status == AgentStatus::Stopped) return "stopped";
	if (is_running == true) return "running";
	// An agent that paused itself after runs that could not finish reads as
	// paused, with its reason, not as one more failure.
	if (agent.status == AgentStatus::Paused && agent.paused_reason.empty() == false) return "paused";
	if (latest_run.has_value() == true) {
		if (latest_run->status == "access_needed") return "access_needed";
		if (latest_run->status == "failed") return "failed";
		if (latest_run->status == "completed" && run_needs_review(*latest_run) == true
				&& latest_run->reviewed_at.has_value() == false)
			return "needs_review";
	}
	if (agent.status == AgentStatus::Paused) return "paused";
	return "watching";
}

std::string agent_presence(const std::string &state, bool is_running, const std::string &run_stage) {
	if (state == "waiting_for_approval") return "greet";
	if (state == "running") {
		if (is_running == false) return "wait";
		// Answering a message is no work to act out: its conversation says it is
		// typing.
		if (run_stage == "answering") return "rest";
		// Its answer is out, or its run has ended and only its last record is
		// being kept: nothing to act out any more.
		if (run_stage == "answered" || run_stage == "done") return "rest";
		if (run_stage == "analysing") return "think";
		return "work";
	}
	if (state == "needs_review") return "explain";
	if (state == "access_needed" || state == "failed") return "listen";
	return "rest";
}

// The words and times of an agent's settings, checked without its folder.
AgentSettings checked_words(const AgentSettings &settings) {
	AgentSettings checked = settings;
	checked.name = trimmed(settings.name);
	checked.goal = trimmed(settings.goal);
	checked.manner = trimmed(settings.manner);
	if (checked.matter_id.empty() == true) throw AgentRefused("Choose the Matter this agent works for.");
	if (checked.name.empty() == true) throw AgentRefused("Give the agent a name.");
	if (characters_in(checked.name) > kMostNameCharacters)
		throw AgentRefused("The agent's name is longer than 80 characters.");
	if (characters_in(checked.goal) > kMostGoalCharacters)
		throw AgentRefused("What the agent looks for is longer than 2,000 characters.");
	if (characters_in(checked.manner) > kMostNameCharacters * 5)
		throw AgentRefused("Say how the colleague works in a sentence or two: at most 400 characters.");
	if (checked.daily_minute < 0 || checked.daily_minute >= 24 * 60)
		throw AgentRefused("Choose a time of day for the daily check.");
	if (checked.weekly_day < 0 || checked.weekly_day > 6) throw AgentRefused("Choose a day of the week for the weekly run.");
	if (is_known_kind(checked.kind) == false) throw AgentRefused("Choose what the agent does.");
	if (checked.character.empty() == false && is_known_character(checked.character) == false)
		throw AgentRefused("Choose one of the agent characters.");
	if (checked.kind == "playbook_review" && checked.playbook_id.empty() == true)
		throw AgentRefused("Choose the Playbook the agent checks against.");
	if (checked.playbook_id.empty() == false) {
		if (checked.mode != AgentMode::Task)
			throw AgentRefused("Only an agent with a task of its own can check documents against a Playbook.");
		// Its task, unless the lawyer worded it themselves: the Playbook's.
		if (checked.goal.empty() == true)
			checked.goal = "Check this Matter's contracts against the firm's Playbook, and prepare the tracked changes that "
				"bring each departing contract to the firm's position.";
	}
	if (checked.mode == AgentMode::Task && checked.goal.empty() == true)
		throw AgentRefused("Write the task the agent carries out, in your own words.");
	if (checked.trigger == AgentTrigger::AfterAgent) {
		if (checked.mode != AgentMode::Task) throw AgentRefused("Only an agent with a task of its own can work after another agent reports.");
		std::optional<Agent> leader;
		try {
			leader = load_agent(checked.follows_agent_id);
		} catch (const std::exception &) {
		}
		if (leader.has_value() == false || leader->matter_id != checked.matter_id || leader->status == AgentStatus::Draft)
			throw AgentRefused("Choose an agent of this Matter whose reports it should follow.");
	}
	return checked;
}

AgentSettings checked_settings(const AgentSettings &settings) {
	AgentSettings checked = checked_words(settings);
	if (trimmed(settings.source_root).empty() == true) throw AgentRefused("Choose the folder the agent watches.");

	const RunnerDependencies &dependencies = runner_dependencies();
	if (!dependencies.matter_folder) throw std::runtime_error("Agents are not available in this part of Singularity.");
	const std::optional<MatterFolder> matter = dependencies.matter_folder(checked.matter_id);
	if (matter.has_value() == false) throw AgentRefused("This Matter is not open in Singularity on this computer.");
	std::error_code error;
	const std::filesystem::path folder = std::filesystem::canonical(std::filesystem::path(trimmed(settings.source_root)), error);
	if (error || std::filesystem::is_directory(folder, error) == false)
		throw AgentRefused("The chosen folder cannot be found. Choose it again.");
	const std::filesystem::path matterFolder = std::filesystem::canonical(matter->folder, error);
	if (error) throw AgentRefused("This Matter's folder cannot be found on this computer.");
	if (is_inside(folder, matterFolder) == false)
		throw AgentRefused("Choose this Matter's folder or a folder inside it. To watch a data room kept elsewhere, "
			"copy or move it into the Matter's folder first.");
	checked.source_root = folder.string();
	return checked;
}

namespace {

constexpr const char *kOnlyTheAdministratorHires = "Only the firm's administrator hires AI colleagues. Ask them for the "
	"colleague you need; the firm's colleagues work with every member.";
constexpr const char *kOnlyTheAdministratorChoosesModels = "Only the firm's administrator chooses a colleague's model.";

// Only the firm's administrator hires its AI colleagues and chooses their
// models: a member's computer is refused, in words that say who does it,
// whatever asked - the window, another app or a script.
void require_administrator(const char *refusal) {
	std::function<bool()> may;
	{
		const std::lock_guard<std::mutex> lock(installed_mutex());
		may = installed().may_manage_colleagues;
	}
	if (may && may() == false) throw AgentRefused(refusal);
}

} // namespace

AgentView create_agent(const AgentSettings &settings, const std::string &actor_id) {
	require_administrator(kOnlyTheAdministratorHires);
	const AgentSettings checked = checked_settings(settings);
	Agent agent;
	std::tie(agent.id, agent.id_nonce) = new_bound_agent_id(installed().device_key ? installed().device_key() : std::string());
	// Its own timed runs wait until it asks for its first one below, once its
	// thread is open: that run is made once, never twice.
	AgentScheduler::shared().hold_own_runs(agent.id);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		apply_settings(agent, checked);
		agent.status = AgentStatus::Active;
		agent.created_at = agent_timestamp_now();
		agent.updated_at = agent.created_at;
		agent.approved_by_actor_id = actor_id;
		agent.approved_at = agent.created_at;
		make_responsible(agent, actor_id);
		schedule_first(agent);
		save_agent(agent);
	}
	// It introduces itself in its thread before its first run.
	open_thread_for(agent.id);
	publish_card(agent.id);
	// The first check records the room as it is; later checks compare with it. A
	// colleague that does tasks when asked has no room to record: it waits for
	// the work it is given.
	AgentScheduler::shared().agents_changed();
	if (works_only_when_asked(agent) == true) AgentScheduler::shared().release_own_runs(agent.id);
	else AgentScheduler::shared().request_check(agent.id, "first");
	return agent_view(agent.id);
}

namespace {

constexpr const char *kOnlyTheAdministratorChanges = "Only the firm's administrator changes, archives or lets go of an AI "
	"colleague.";

std::string folded_name(std::string text) {
	for (char &character : text)
		if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
	return text;
}

// The colleague as the firm keeps it, on the computer that runs it: a copy
// says where it is changed.
Agent own_colleague(const std::string &agent_id) {
	Agent agent = require_agent(agent_id);
	if (agent.is_firm_copy == true)
		throw AgentRefused(agent.name + " is changed on the computer that runs it, and every other computer follows.");
	if (agent.status == AgentStatus::Draft) throw AgentRefused("A colleague not yet approved is approved or refused first.");
	return agent;
}

// Whether another colleague at work is called so.
void require_free_name(const std::string &agent_id, const std::string &name) {
	for (const Agent &other : load_agents())
		if (other.id != agent_id && other.status != AgentStatus::Stopped && other.status != AgentStatus::Draft
				&& folded_name(trimmed(other.name)) == folded_name(name))
			throw AgentRefused("There is already a colleague named " + trimmed(other.name) + ". Give this one another name.");
}

} // namespace

namespace {

// A change of a colleague's words, checked as at its hiring: trimmed, its name
// given and short enough, its face one of the faces, its office one the firm
// keeps.
FirmColleagueHire checked_edit(const FirmColleagueHire &edit) {
	FirmColleagueHire checked;
	checked.name = trimmed(edit.name);
	checked.character = trimmed(edit.character);
	checked.manner = trimmed(edit.manner);
	checked.goal = trimmed(edit.goal);
	if (checked.name.empty() == true) throw AgentRefused("Give the colleague a name.");
	if (characters_in(checked.name) > kMostNameCharacters) throw AgentRefused("The colleague's name is longer than 80 characters.");
	if (checked.character.empty() == false && is_known_character(checked.character) == false)
		throw AgentRefused("Choose one of the colleagues' faces.");
	if (characters_in(checked.manner) > kMostNameCharacters * 5)
		throw AgentRefused("Say how the colleague works in a sentence or two: at most 400 characters.");
	if (characters_in(checked.goal) > kMostGoalCharacters) throw AgentRefused("What the colleague does is longer than 2,000 characters.");
	checked.office = checked_office(edit.office);
	return checked;
}

// The checked change, on the computer that runs the colleague; every other
// computer follows the firm's record.
void apply_edit(const std::string &agent_id, const FirmColleagueHire &checked) {
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		Agent agent = own_colleague(agent_id);
		if (agent.status == AgentStatus::Stopped) throw AgentRefused("Restore " + agent.name + " first, then change it.");
		require_free_name(agent_id, checked.name);
		agent.name = checked.name;
		if (checked.character.empty() == false) agent.character = checked.character;
		agent.manner = checked.manner;
		agent.goal = checked.goal;
		agent.office = checked.office;
		save_changed(agent);
	}
	// Its thread's brief says who it is now.
	open_thread_for(agent_id);
	publish_card(agent_id);
	AgentScheduler::shared().agents_changed();
}

// How far ahead of this computer's clock an edit may say it was made: a
// computer whose clock runs fast never pins its edit over later ones.
constexpr std::chrono::minutes kMostClockLead{5};

std::string timestamp_after(std::chrono::minutes lead) {
	const std::time_t then = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + lead);
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &then);
#else
	gmtime_r(&then, &utc);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

} // namespace

AgentView edit_firm_colleague(const std::string &agent_id, const FirmColleagueHire &edit) {
	require_administrator(kOnlyTheAdministratorChanges);
	apply_edit(agent_id, checked_edit(edit));
	return agent_view(agent_id);
}

AgentView request_colleague_edit(const std::string &agent_id, const FirmColleagueHire &edit) {
	require_administrator(kOnlyTheAdministratorChanges);
	const FirmColleagueHire checked = checked_edit(edit);
	bool isOwn = false;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		const Agent agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Draft) throw AgentRefused("A colleague not yet approved is approved or refused first.");
		isOwn = agent.is_firm_copy == false;
		if (isOwn == false) {
			if (agent.status == AgentStatus::Stopped) throw AgentRefused("Restore " + agent.name + " first, then change it.");
			require_free_name(agent_id, checked.name);
		}
	}
	// One this computer runs is changed here and now.
	if (isOwn == true) {
		apply_edit(agent_id, checked);
		return agent_view(agent_id);
	}
	std::function<std::string(const std::string &)> sign;
	ColleagueEdit request;
	{
		const std::lock_guard<std::mutex> lock(installed_mutex());
		sign = installed().sign;
		request.written_by = installed().device_id ? installed().device_id() : std::string();
		request.seat_id = installed().seat_id ? installed().seat_id() : std::string();
	}
	if (!sign || request.written_by.empty() == true)
		throw AgentRefused("This computer can't sign the change, so the colleague's computer would not take it. Make it there.");
	request.agent_id = agent_id;
	request.name = checked.name;
	request.character = checked.character;
	request.goal = checked.goal;
	request.manner = checked.manner;
	request.office = checked.office;
	request.at = agent_timestamp_now();
	request.signature = sign(colleague_edit_text(request));
	if (request.signature.empty() == true)
		throw AgentRefused("This computer can't sign the change, so the colleague's computer would not take it. Make it there.");
	try {
		write_colleague_edit(request);
	} catch (const std::runtime_error &) {
		throw AgentRefused("The change can't go to the firm's record from this computer yet. Make it on the colleague's computer.");
	}
	return agent_view(agent_id);
}

void take_colleague_edits() {
	std::function<std::optional<std::string>(const std::string &)> key_of;
	std::function<bool(const std::string &, const std::string &, const std::string &)> verify;
	{
		const std::lock_guard<std::mutex> lock(installed_mutex());
		key_of = installed().administrator_computer_key;
		verify = installed().verify;
	}
	// Without a way to tell the administrator's computers, nothing is taken.
	if (!key_of || !verify) return;
	std::vector<std::string> own;
	try {
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (const Agent &agent : load_agents())
			if (agent.is_firm_copy == false && agent.matter_id.empty() == true && agent.status != AgentStatus::Draft)
				own.push_back(agent.id);
	} catch (const std::exception &) {
		return;
	}
	const std::string latest = timestamp_after(kMostClockLead);
	// Each computer is asked about once a round, whatever it wrote.
	std::map<std::string, std::optional<std::string>> keys;
	for (const std::string &agent_id : own) {
		std::vector<ColleagueEdit> edits;
		std::map<std::string, std::string> taken;
		try {
			edits = colleague_edits(agent_id);
			if (edits.empty() == true) continue;
			taken = taken_colleague_edits(agent_id);
		} catch (const std::exception &) {
			continue;
		}
		std::optional<ColleagueEdit> newest;
		bool isMarked = false;
		for (const ColleagueEdit &edit : edits) {
			const auto was = taken.find(edit.written_by);
			if (was != taken.end() && edit.at <= was->second) continue;
			if (edit.at > latest) continue;
			if (keys.count(edit.written_by) == 0) keys[edit.written_by] = key_of(edit.written_by);
			const std::optional<std::string> &key = keys[edit.written_by];
			if (key.has_value() == false || verify(*key, colleague_edit_text(edit), edit.signature) == false) continue;
			taken[edit.written_by] = edit.at;
			isMarked = true;
			if (newest.has_value() == false || edit.at > newest->at) newest = edit;
		}
		if (isMarked == false) continue;
		try {
			save_taken_colleague_edits(agent_id, taken);
		} catch (const std::exception &) {
			// Not marked, it is not taken either: the next round tries again.
			continue;
		}
		FirmColleagueHire change;
		change.name = newest->name;
		change.character = newest->character;
		change.goal = newest->goal;
		change.manner = newest->manner;
		change.office = newest->office;
		try {
			apply_edit(agent_id, checked_edit(change));
		} catch (const std::exception &) {
			// Refused as the administrator's own would be: the record's words stand.
		}
	}
}

AgentView archive_colleague(const std::string &agent_id, bool archived) {
	require_administrator(kOnlyTheAdministratorChanges);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		Agent agent = own_colleague(agent_id);
		if (archived == true) {
			if (agent.status == AgentStatus::Stopped) return view_of(agent);
			agent.status = AgentStatus::Stopped;
			agent.next_check_at.reset();
			withdraw_waiting_work(agent, "The colleague was archived before it began this.");
		} else {
			if (agent.status != AgentStatus::Stopped) return view_of(agent);
			require_free_name(agent_id, trimmed(agent.name));
			agent.status = AgentStatus::Active;
			agent.paused_reason.clear();
			agent.failed_in_a_row = 0;
			schedule_first(agent);
		}
		save_changed(agent);
	}
	publish_card(agent_id);
	if (archived == true) AgentScheduler::shared().cancel_check(agent_id);
	else open_thread_for(agent_id);
	AgentScheduler::shared().agents_changed();
	return agent_view(agent_id);
}

void delete_colleague(const std::string &agent_id) {
	require_administrator(kOnlyTheAdministratorChanges);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		Agent agent = own_colleague(agent_id);
		// The Matters it worked in are told it is gone, as an archived one is.
		if (agent.status != AgentStatus::Stopped) {
			agent.status = AgentStatus::Stopped;
			agent.next_check_at.reset();
			withdraw_waiting_work(agent, "The colleague was let go before it began this.");
			save_changed(agent);
		}
	}
	publish_card(agent_id);
	AgentScheduler::shared().cancel_check(agent_id);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		forget_agent(agent_id, true);
	}
	AgentScheduler::shared().agents_changed();
}

AgentView hire_firm_colleague(const FirmColleagueHire &hire, const std::string &actor_id) {
	require_administrator(kOnlyTheAdministratorHires);
	AgentSettings settings;
	settings.name = trimmed(hire.name);
	settings.character = trimmed(hire.character);
	settings.manner = trimmed(hire.manner);
	settings.goal = trimmed(hire.goal);
	settings.kind = "custom";
	settings.mode = AgentMode::Task;
	settings.trigger = AgentTrigger::Manual;
	if (settings.name.empty() == true) throw AgentRefused("Give the colleague a name.");
	if (characters_in(settings.name) > kMostNameCharacters) throw AgentRefused("The colleague's name is longer than 80 characters.");
	if (settings.character.empty() == false && is_known_character(settings.character) == false)
		throw AgentRefused("Choose one of the colleagues' faces.");
	if (characters_in(settings.manner) > kMostNameCharacters * 5)
		throw AgentRefused("Say how the colleague works in a sentence or two: at most 400 characters.");
	if (characters_in(settings.goal) > kMostGoalCharacters)
		throw AgentRefused("What the colleague does is longer than 2,000 characters.");
	const Agent::Office office = checked_office(hire.office);
	// A colleague's name is how the members name it (@Kestrel) and how the window knows it:
	// two at work under one name would be one to everyone.
	{
		const auto folded = [](std::string text) {
			for (char &character : text)
				if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
			return text;
		};
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (const Agent &other : load_agents())
			if (other.status != AgentStatus::Stopped && other.status != AgentStatus::Draft
					&& folded(trimmed(other.name)) == folded(settings.name))
				throw AgentRefused("There is already a colleague named " + trimmed(other.name) + ". Give this one another name.");
	}
	Agent agent;
	std::tie(agent.id, agent.id_nonce) = new_bound_agent_id(installed().device_key ? installed().device_key() : std::string());
	AgentScheduler::shared().hold_own_runs(agent.id);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		apply_settings(agent, settings);
		agent.office = office;
		agent.status = AgentStatus::Active;
		agent.created_at = agent_timestamp_now();
		agent.updated_at = agent.created_at;
		agent.approved_by_actor_id = actor_id;
		agent.approved_at = agent.created_at;
		make_responsible(agent, actor_id);
		save_agent(agent);
	}
	// Its own chat with the member opens on the desk, so a message reaches it at
	// once. It works when asked, in any Matter: nothing of its own is scheduled.
	open_thread_for(agent.id);
	AgentScheduler::shared().agents_changed();
	AgentScheduler::shared().release_own_runs(agent.id);
	return agent_view(agent.id);
}

namespace {

// What a copy holds of the firm's record of its colleague; whether anything
// it holds changed.
bool take_record(Agent &agent, const FirmColleague &colleague) {
	// At work, paused or stopped as the record says; anything else waits.
	AgentStatus status = agent_status_from_string(colleague.status).value_or(AgentStatus::Paused);
	if (status == AgentStatus::Draft) status = AgentStatus::Paused;
	const bool isSame = agent.name == colleague.name && agent.character == colleague.character && agent.goal == colleague.goal
		&& agent.manner == colleague.manner && agent.office.title == colleague.office.title
		&& agent.office.ranks_with == colleague.office.ranks_with && agent.office.practice == colleague.office.practice
		&& agent.office.area == colleague.office.area && agent.office.answers_to == colleague.office.answers_to
		&& agent.model_provider_id == colleague.model_provider_id && agent.model_id == colleague.model_id
		&& agent.memory_provider_id == colleague.memory_provider_id && agent.memory_model_id == colleague.memory_model_id
		&& agent.is_memory_off == colleague.is_memory_off && agent.status == status
		&& agent.responsible_actor_id == colleague.responsible_actor_id && agent.responsible_name == colleague.responsible_name;
	if (isSame == true) return false;
	agent.name = colleague.name;
	agent.character = colleague.character;
	agent.goal = colleague.goal;
	agent.manner = colleague.manner;
	agent.office.title = colleague.office.title;
	agent.office.ranks_with = colleague.office.ranks_with;
	agent.office.practice = colleague.office.practice;
	agent.office.area = colleague.office.area;
	agent.office.answers_to = colleague.office.answers_to;
	agent.model_provider_id = colleague.model_provider_id;
	agent.model_id = colleague.model_id;
	agent.memory_provider_id = colleague.memory_provider_id;
	agent.memory_model_id = colleague.memory_model_id;
	agent.is_memory_off = colleague.is_memory_off;
	agent.status = status;
	agent.responsible_actor_id = colleague.responsible_actor_id;
	agent.responsible_name = colleague.responsible_name;
	agent.approved_by_actor_id = colleague.responsible_actor_id;
	return true;
}

} // namespace

void adopt_firm_colleagues() {
	std::vector<FirmColleague> firm;
	try {
		firm = firm_colleagues();
	} catch (const std::exception &) {
		return;
	}
	// No record here, or none read: the copies stay as they are.
	if (firm.empty() == true) return;
	std::vector<std::string> toOpen;
	bool isChanged = false;
	try {
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		std::set<std::string> held;
		for (const FirmColleague &colleague : firm) {
			if (colleague.agent_id.rfind("agent_", 0) != 0) continue;
			std::optional<Agent> kept = load_agent(colleague.agent_id);
			// One this computer runs is its own: the record is written from it.
			if (kept.has_value() == true && kept->is_firm_copy == false) continue;
			// Let go by the firm: its copy goes, its chat with it too.
			if (colleague.status == "deleted") {
				if (kept.has_value() == true) {
					forget_agent(colleague.agent_id, false);
					isChanged = true;
				}
				continue;
			}
			held.insert(colleague.agent_id);
			Agent agent = kept.value_or(Agent{});
			if (kept.has_value() == false) {
				// Not run here yet: the record's colleague, as a hired one is made,
				// with no work of its own and nothing scheduled.
				if (agent_status_from_string(colleague.status).value_or(AgentStatus::Paused) != AgentStatus::Active) continue;
				agent.id = colleague.agent_id;
				agent.is_firm_copy = true;
				agent.kind = "custom";
				agent.mode = AgentMode::Task;
				agent.trigger = AgentTrigger::Manual;
				agent.created_at = colleague.created_at.empty() == false ? colleague.created_at : agent_timestamp_now();
				agent.approved_at = agent.created_at;
			}
			if (take_record(agent, colleague) == false) continue;
			agent.updated_at = agent_timestamp_now();
			save_agent(agent);
			isChanged = true;
			if (agent.status == AgentStatus::Active && (agent.chat_id.has_value() == false || agent.chat_id->empty() == true))
				toOpen.push_back(agent.id);
		}
		// A copy whose colleague the record no longer holds is paused, its chat
		// kept.
		for (Agent &agent : load_agents()) {
			if (agent.is_firm_copy == false || held.count(agent.id) > 0 || agent.status != AgentStatus::Active) continue;
			agent.status = AgentStatus::Paused;
			agent.updated_at = agent_timestamp_now();
			save_agent(agent);
			isChanged = true;
		}
	} catch (const std::exception &) {
		return;
	}
	for (const std::string &id : toOpen) open_thread_for(id);
	if (isChanged == true) AgentScheduler::shared().agents_changed();
}

void open_missing_desk_threads() {
	std::vector<std::string> missing;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (const Agent &agent : load_agents()) {
			if (agent.matter_id.empty() == false || agent.status != AgentStatus::Active) continue;
			if (agent.chat_id.has_value() == false || agent.chat_id->empty() == true) missing.push_back(agent.id);
		}
	}
	for (const std::string &id : missing) open_thread_for(id);
}

AgentView draft_agent_for_app(const AgentSettings &settings, const AgentRequester &requester) {
	AgentSettings words = settings;
	words.source_root.clear();
	const AgentSettings checked = checked_words(words);
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		std::tie(agent.id, agent.id_nonce) = new_bound_agent_id(installed().device_key ? installed().device_key() : std::string());
		apply_settings(agent, checked);
		// Inert until the lawyer decides: no folder, no schedule, no run.
		agent.status = AgentStatus::Draft;
		agent.created_at = agent_timestamp_now();
		agent.updated_at = agent.created_at;
		agent.requested_by = requester;
		agent.requested_by->requested_at = agent.created_at;
		agent.requested_by->requested_name = checked.name;
		agent.requested_by->requested_goal = checked.goal;
		agent.requested_by->requested_folder = "";
		agent.requested_by->requested_trigger = to_string(checked.trigger);
		save_agent(agent);
	}
	AgentScheduler::shared().agents_changed();
	return view_of(agent);
}

AgentView approve_draft(const std::string &agent_id, const AgentSettings &settings, const std::string &actor_id) {
	require_administrator(kOnlyTheAdministratorHires);
	const AgentSettings checked = checked_settings(settings);
	Agent agent;
	// Its own timed runs wait until it asks for its first one below.
	AgentScheduler::shared().hold_own_runs(agent_id);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status != AgentStatus::Draft) throw AgentRefused("This agent was already decided.");
		// What the lawyer approves is what runs: their Matter, folder, words,
		// schedule and model. The app's request stays beside it as it asked.
		apply_settings(agent, checked);
		agent.status = AgentStatus::Active;
		agent.approved_by_actor_id = actor_id;
		agent.approved_at = agent_timestamp_now();
		make_responsible(agent, actor_id);
		schedule_first(agent);
		save_changed(agent);
	}
	open_thread_for(agent.id);
	publish_card(agent.id);
	AgentScheduler::shared().agents_changed();
	if (works_only_when_asked(agent) == true) AgentScheduler::shared().release_own_runs(agent.id);
	else AgentScheduler::shared().request_check(agent.id, "first");
	return agent_view(agent.id);
}

AgentView decline_draft(const std::string &agent_id) {
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	Agent agent = require_agent(agent_id);
	if (agent.status != AgentStatus::Draft) throw AgentRefused("This agent was already decided.");
	agent.status = AgentStatus::Stopped;
	save_changed(agent);
	return view_of(agent);
}

AgentView set_agent_model(const std::string &agent_id, const std::string &provider_id, const std::string &model_id) {
	require_administrator(kOnlyTheAdministratorChoosesModels);
	const std::string provider = trimmed(provider_id);
	const std::string model = trimmed(model_id);
	if (provider.empty() != model.empty()) throw AgentRefused("Choose a model, or the firm's default.");
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Stopped) throw AgentRefused("A colleague who left the firm takes no model.");
		if (agent.model_provider_id == provider && agent.model_id == model) return view_of(agent);
		agent.model_provider_id = provider;
		agent.model_id = model;
		save_changed(agent);
	}
	AgentScheduler::shared().agents_changed();
	return view_of(agent);
}

AgentView set_agent_memory_model(const std::string &agent_id, const std::string &provider_id, const std::string &model_id, bool off) {
	require_administrator(kOnlyTheAdministratorChoosesModels);
	const std::string provider = off == true ? std::string() : trimmed(provider_id);
	const std::string model = off == true ? std::string() : trimmed(model_id);
	if (provider.empty() != model.empty()) throw AgentRefused("Choose a model, the firm's own for context, or none.");
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Stopped) throw AgentRefused("A colleague who left the firm keeps no memory.");
		if (agent.memory_provider_id == provider && agent.memory_model_id == model && agent.is_memory_off == off) return view_of(agent);
		agent.memory_provider_id = provider;
		agent.memory_model_id = model;
		agent.is_memory_off = off;
		save_changed(agent);
	}
	AgentScheduler::shared().agents_changed();
	return view_of(agent);
}

AgentView pause_agent(const std::string &agent_id) {
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Paused) return view_of(agent);
		if (agent.status != AgentStatus::Active) throw AgentRefused("Only an agent at work can be paused.");
		agent.status = AgentStatus::Paused;
		agent.paused_reason.clear();
		save_changed(agent);
	}
	publish_card(agent_id);
	AgentScheduler::shared().cancel_check(agent_id);
	AgentScheduler::shared().agents_changed();
	return view_of(agent);
}

AgentView resume_agent(const std::string &agent_id) {
	Agent agent;
	// Its own timed runs wait until it asks for its run below.
	AgentScheduler::shared().hold_own_runs(agent_id);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Active) return view_of(agent);
		if (agent.status != AgentStatus::Paused) throw AgentRefused("Only a paused agent can be resumed.");
		agent.status = AgentStatus::Active;
		agent.paused_reason.clear();
		agent.failed_in_a_row = 0;
		schedule_first(agent);
		save_changed(agent);
	}
	publish_card(agent_id);
	AgentScheduler::shared().agents_changed();
	// What changed while it was paused is checked now, not at the next change;
	// one set to work when asked has no runs of its own to hold, so the work
	// given it while it was paused begins at once - it waited half a minute
	// for the hold to lapse (finding f-resumed-part-waits).
	if (works_only_when_asked(agent) == false)
		AgentScheduler::shared().request_check(agent_id, agent.last_check_at.has_value() == true ? "catch_up" : "first");
	else
		AgentScheduler::shared().release_own_runs(agent_id);
	return view_of(agent);
}

AgentView stop_agent(const std::string &agent_id) {
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Stopped) return view_of(agent);
		agent.status = AgentStatus::Stopped;
		agent.next_check_at.reset();
		withdraw_waiting_work(agent, "The agent was stopped before it began this.");
		save_changed(agent);
	}
	publish_card(agent_id);
	AgentScheduler::shared().cancel_check(agent_id);
	AgentScheduler::shared().agents_changed();
	return view_of(agent);
}

AgentView update_agent(const std::string &agent_id, long long expected_revision, const AgentSettings &settings) {
	const AgentSettings checked = checked_settings(settings);
	Agent agent;
	bool isFolderChanged = false;
	// Another folder asks for a first run below: until then its own timed runs
	// wait. Without one the hold ends there.
	AgentScheduler::shared().hold_own_runs(agent_id);
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.revision != expected_revision)
			throw AgentRefused("This agent was changed elsewhere since you opened it. Look at it again, then make your change.");
		if (agent.status == AgentStatus::Draft || agent.status == AgentStatus::Stopped)
			throw AgentRefused("A stopped or unapproved agent cannot be changed.");
		if (checked.matter_id != agent.matter_id) throw AgentRefused("An agent stays with the Matter it was made for.");
		// Agents never wake each other in a circle.
		if (checked.trigger == AgentTrigger::AfterAgent) {
			std::string leader = checked.follows_agent_id;
			for (int step = 0; step < 32 && leader.empty() == false; ++step) {
				if (leader == agent_id) throw AgentRefused("That agent already follows this one, directly or through others.");
				const std::optional<Agent> next = load_agent(leader);
				leader = next.has_value() == true && next->trigger == AgentTrigger::AfterAgent ? next->follows_agent_id : std::string();
			}
		}
		isFolderChanged = checked.source_root != agent.source_root;
		apply_settings(agent, checked);
		// Another folder is another room: it is recorded afresh, never compared
		// with the old one.
		if (isFolderChanged == true) agent.last_check_at.reset();
		schedule_first(agent);
		save_changed(agent);
	}
	// Its thread's brief says who it is now.
	open_thread_for(agent_id);
	publish_card(agent_id);
	AgentScheduler::shared().agents_changed();
	if (isFolderChanged == true && agent.status == AgentStatus::Active && works_only_when_asked(agent) == false)
		AgentScheduler::shared().request_check(agent_id, "first");
	else
		AgentScheduler::shared().release_own_runs(agent_id);
	return agent_view(agent_id);
}

AgentView set_agent_office(const std::string &agent_id, const Agent::Office &office) {
	const Agent::Office checked = checked_office(office);
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		if (agent.status == AgentStatus::Draft || agent.status == AgentStatus::Stopped)
			throw AgentRefused("A stopped or unapproved agent cannot be changed.");
		agent.office = checked;
		save_changed(agent);
	}
	// Its thread's brief says who it is now.
	open_thread_for(agent_id);
	publish_card(agent_id);
	return agent_view(agent_id);
}

AgentView request_agent_run(const std::string &agent_id) {
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
	}
	if (agent.status != AgentStatus::Active && agent.status != AgentStatus::Paused)
		throw AgentRefused("A stopped or unapproved agent does not run.");
	// A colleague of the firm watches no folder of one Matter: run now, it takes
	// up its standing work in its own chat, where it reaches every Matter.
	if (agent.matter_id.empty() == true) {
		(void)assign_work(agent_id, agent.goal.empty() == false ? agent.goal
			: std::string("Look over your field of work across the firm and say what needs doing now."), installed().actor_id
			? installed().actor_id() : std::string());
		return agent_view(agent_id);
	}
	if (AgentScheduler::shared().is_started() == false)
		throw std::runtime_error("Agents run only in the Singularity window.");
	AgentScheduler::shared().request_check(agent_id, agent.last_check_at.has_value() == true ? "manual" : "first");
	return view_of(agent);
}

AgentView cancel_agent_run(const std::string &agent_id) {
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
	}
	AgentScheduler::shared().cancel_check(agent_id);
	return view_of(agent);
}

AgentView mark_run_reviewed(const std::string &agent_id, const std::string &run_id, const std::string &actor_id) {
	AgentView view;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		const Agent agent = require_agent(agent_id);
		std::optional<AgentRun> run = load_run(agent_id, run_id);
		if (run.has_value() == false) throw AgentRefused("This check no longer exists.");
		if (run->status != "completed") throw AgentRefused("Only a finished check can be marked reviewed.");
		if (run->reviewed_at.has_value() == false) {
			run->reviewed_at = agent_timestamp_now();
			run->reviewed_by_actor_id = actor_id;
			save_run(*run);
		}
		view = view_of(agent);
	}
	publish_card(agent_id);
	return view;
}

void publish_card(const std::string &agent_id) {
	// One card is written at a time, each with the agent as it is when its
	// turn comes, so a slow earlier write never lands over a later one.
	static std::mutex publishing;
	const std::lock_guard<std::mutex> turn(publishing);
	try {
		const RunnerDependencies &dependencies = installed();
		if (!dependencies.matter_folder) return;
		AgentView view;
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			const std::optional<Agent> agent = load_agent(agent_id);
			// A draft is not the team's until a lawyer approves it; a copy of the
			// firm's colleague is the card of the computer that runs it.
			if (agent.has_value() == false || agent->status == AgentStatus::Draft || agent->is_firm_copy == true) return;
			view = view_of(*agent);
		}
		// Every Matter it works in shows it to its team: the one it was made for,
		// in the zero state, and each where it was given work - never the
		// member's desk, which is no Matter.
		std::set<std::string> matterIds;
		if (view.agent.matter_id.empty() == false) matterIds.insert(view.agent.matter_id);
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			for (const AgentAssignment &work : load_assignments(agent_id))
				if (work.matter_id.empty() == false) matterIds.insert(work.matter_id);
		}
		// And each Matter this computer carries with the firm's other computers:
		// their lawyers may name it there too.
		if (dependencies.carried_matters)
			for (const std::string &matterId : dependencies.carried_matters()) matterIds.insert(matterId);
		const std::string desk = dependencies.desk_project ? dependencies.desk_project() : std::string();
		const std::string device = dependencies.device_id ? dependencies.device_id() : std::string();
		AgentCard card = card_of(view.agent, view.latest_run, view.state, device, agent_timestamp_now());
		// Signed by this computer, the one that runs it.
		if (dependencies.device_key && dependencies.sign) {
			card.host_device_key = dependencies.device_key();
			card.counter = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			card.signature = dependencies.sign(agent_card_signed_text(card.agent_id, card.host_device_id, card.host_device_key,
				card.id_nonce, card.responsible_actor_id, card.counter));
		}
		for (const std::string &matterId : matterIds) {
			if (matterId == desk) continue;
			// A Matter's folder may be a slow share: it is written with no lock held.
			const std::optional<MatterFolder> matter = dependencies.matter_folder(matterId);
			if (matter.has_value() == true) publish_agent_card(matter->folder, card);
		}
	} catch (const std::exception &) {
	}
}

namespace {

AgentAssignment give_work(const std::string &agent_id, const std::string &matter_id, const std::string &instruction,
		const std::string &actor_id, const std::string &given_by, const std::string &chat_id, const std::string &message_id,
		bool from_shared_folder);

} // namespace

AgentAssignment assign_work(const std::string &agent_id, const std::string &instruction, const std::string &actor_id) {
	return give_work(agent_id, "", instruction, actor_id, person_name_now(), "", "", false);
}

std::string words_to_colleague(const std::string &text, const std::string &colleague_name) {
	const std::string name = trimmed(colleague_name);
	if (name.empty() == true) return text;
	const auto lower = [](std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
		return value;
	};
	const std::string handle = "@" + lower(name);
	const std::string folded = lower(text);
	const auto isWordCharacter = [](char character) {
		return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-';
	};
	std::string words;
	std::size_t at = 0;
	while (at < text.size()) {
		const bool starts = at == 0 || isWordCharacter(text[at - 1]) == false;
		const std::size_t end = at + handle.size();
		if (starts == true && folded.compare(at, handle.size(), handle) == 0
				&& (end >= text.size() || isWordCharacter(text[end]) == false)) {
			at = end;
			continue;
		}
		words.push_back(text[at]);
		++at;
	}
	// What the name leaves behind: a comma or colon after it, spaces doubled.
	std::string tidy;
	for (const char character : words) {
		if (character == ' ' && (tidy.empty() == true || tidy.back() == ' ' || tidy.back() == '\n')) continue;
		tidy.push_back(character);
	}
	std::size_t first = 0;
	while (first < tidy.size() && (tidy[first] == ',' || tidy[first] == ':' || tidy[first] == ';' || tidy[first] == ' '
			|| tidy[first] == '\n'))
		++first;
	std::string result = trimmed(tidy.substr(first));
	for (std::size_t index = 0; index + 1 < result.size(); ++index) {
		if (result[index] == ' ' && (result[index + 1] == ',' || result[index + 1] == '.' || result[index + 1] == '?'
				|| result[index + 1] == '!'))
			result.erase(index, 1);
	}
	return result.empty() == true ? trimmed(text) : result;
}

AgentAssignment assign_mention(const std::string &agent_id, const std::string &matter_id, const std::string &message_text,
	const std::string &chat_id, const std::string &message_id, const std::string &author_actor_id,
	const std::string &author_name, bool posted_here) {
	if (matter_id.empty() == true || chat_id.empty() == true || message_id.empty() == true)
		throw AgentRefused("Name the colleague in a message of a thread.");
	return give_work(agent_id, matter_id, message_text, author_actor_id, author_name, chat_id, message_id, posted_here == false);
}

AgentAssignment assign_in_thread(const std::string &agent_id, const std::string &matter_id, const std::string &instruction,
	const std::string &chat_id, const std::string &actor_id) {
	if (matter_id.empty() == true || chat_id.empty() == true) throw AgentRefused("Give the work in a thread.");
	return give_work(agent_id, matter_id, instruction, actor_id, person_name_now(), chat_id, "", false);
}

namespace {

// What a colleague says when work it is given cannot begin at once - it is
// paused, or at work on other work, or other work was given before it - in
// its own words; "" when the work begins at once. `current_here` is the start
// of the work it is at, when that work came in this very thread: it names it,
// so the member knows what it is on; work of any other thread or Matter is
// never named here. `is_joined`: the message joined the member's earlier work
// in line in this thread.
std::string waiting_words(bool is_paused, bool is_at_work, std::size_t waiting_ahead, const std::string &current_here,
		bool is_joined) {
	if (is_paused == true) return "I'm paused at the moment. I'll start on this as soon as I'm resumed.";
	const std::string onNow = current_here.empty() == true ? std::string() : "I'm still on \u201c" + current_here + "\u201d. ";
	if (is_joined == true) {
		if (is_at_work == false) return "I've added this to what you sent before. I'll read them together as soon as I come to it.";
		return "I've added this to what you sent before. " + onNow + "I'll read them together as soon as I've finished "
			+ (current_here.empty() == true ? "the work I'm on." : "it.");
	}
	const std::size_t ahead = waiting_ahead + (is_at_work == true ? 1 : 0);
	if (ahead == 0) return std::string();
	if (ahead == 1 && is_at_work == true) {
		if (current_here.empty() == false) return onNow + "I'll start on this as soon as I've finished it.";
		return "I'll start on this as soon as I've finished what I'm working on.";
	}
	static const char *const counted[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
	const std::string count = ahead < 10 ? std::string(counted[ahead]) : std::to_string(ahead);
	return onNow + "I have " + count + (ahead == 1 ? " piece" : " pieces") + " of work ahead of this one. I'll start on it as "
		"soon as I've finished " + (ahead == 1 ? "it." : "them.");
}

// The start of a piece of work, in one line, as the colleague names it back to
// the member who gave it: at most about sixty letters, cut at a word.
std::string start_of(const std::string &instruction) {
	std::string line;
	for (const char character : instruction) {
		if (character == '\n' || character == '\r' || character == '\t') {
			if (line.empty() == false && line.back() != ' ') line += ' ';
			continue;
		}
		line += character;
	}
	line = trimmed(line);
	std::size_t letters = 0;
	for (std::size_t index = 0; index < line.size(); ++index) {
		if ((static_cast<unsigned char>(line[index]) & 0xC0) == 0x80) continue;
		if (++letters <= 60) continue;
		std::size_t cut = line.rfind(' ', index);
		if (cut == std::string::npos || cut < index / 2) cut = index;
		return trimmed(line.substr(0, cut)) + "\u2026";
	}
	return line;
}

AgentAssignment give_work(const std::string &agent_id, const std::string &matter_id, const std::string &instruction,
		const std::string &actor_id, const std::string &given_by, const std::string &chat_id, const std::string &message_id,
		bool from_shared_folder) {
	const std::string words = trimmed(instruction);
	if (words.empty() == true) throw AgentRefused("Write the work, in your own words.");
	if (characters_in(words) > kAgentMostAssignmentCharacters)
		throw AgentRefused("The work is longer than 20,000 characters. Put the detail in a document and name it here.");
	if (AgentScheduler::shared().is_started() == false) throw std::runtime_error("Agents work only in the Singularity window.");
	// Where it was given: the thread's Matter, or the colleague's own chat - the
	// Matter it was made for, or the member's desk - when no thread is named.
	// Found with no lock held: the desk may have to be made.
	std::string where = trimmed(matter_id);
	if (where.empty() == true) {
		std::optional<Agent> found;
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			found = load_agent(agent_id);
		}
		if (found.has_value() == false) throw AgentRefused("This agent no longer exists.");
		where = home_project_of(*found, installed());
		if (where.empty() == true) throw AgentRefused("Its own chat cannot be opened on this computer now. Give the work in a thread.");
	}
	AgentAssignment work;
	// What the work waits behind as it is given: the colleague paused, or the
	// work already given to it and not yet begun.
	std::optional<Agent> given_to;
	bool is_paused = false;
	std::size_t ahead = 0;
	// The work the colleague is at, named back when it came in this thread; and
	// whether this message joined the member's earlier work in line here.
	std::string current_here;
	bool is_joined = false;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		Agent agent = require_agent(agent_id);
		if (agent.status != AgentStatus::Active && agent.status != AgentStatus::Paused)
			throw AgentRefused("A stopped or unapproved agent takes no work.");
		// A message is taken up once, whichever computer reads it first here.
		if (message_id.empty() == false)
			for (const AgentAssignment &given : load_assignments(agent_id))
				if (given.reply_to_message_id == message_id
						|| std::find(given.folded_message_ids.begin(), given.folded_message_ids.end(), message_id)
							!= given.folded_message_ids.end())
					return given;
		is_paused = agent.status == AgentStatus::Paused;
		const std::vector<AgentAssignment> given = load_assignments(agent_id);
		for (const AgentAssignment &each : given)
			if (each.state == "working" && chat_id.empty() == false && each.chat_id == chat_id) current_here = start_of(each.instruction);
		// A later message of the same member in the same thread, while their
		// earlier work there still waits, joins that work. The colleague reads
		// them together as it takes the work up, as a person reads what came
		// while they were busy, and its line grows by no message of one
		// conversation.
		if (message_id.empty() == false && chat_id.empty() == false) {
			for (const AgentAssignment &earlier : given) {
				if (earlier.state != "waiting" || earlier.chat_id != chat_id || earlier.given_by_actor_id != actor_id
						|| earlier.from_shared_folder != from_shared_folder || earlier.reply_to_message_id.empty() == true)
					continue;
				if (std::find(agent.waiting_work.begin(), agent.waiting_work.end(), earlier.id) == agent.waiting_work.end()) continue;
				const std::string more = words_to_colleague(words, agent.name);
				if (characters_in(earlier.instruction) + characters_in(more) + 2 > kAgentMostAssignmentCharacters) break;
				work = earlier;
				work.instruction += "\n\n" + more;
				work.folded_message_ids.push_back(message_id);
				save_assignment(work);
				is_joined = true;
				given_to = agent;
				break;
			}
		}
		if (is_joined == false) {
			// Work that reached this computer through a shared folder is taken up only
			// as its author's (Chat/MessageProvenance) and is work like any other: a
			// colleague takes all it is given, in the order given.
			work.id = new_assignment_id();
			work.agent_id = agent.id;
			work.matter_id = where;
			agent.work_given += 1;
			work.number = agent.work_given;
			// Work given by naming the agent reads as said to it, without its name.
			work.instruction = message_id.empty() == true ? words : words_to_colleague(words, agent.name);
			work.chat_id = chat_id;
			work.reply_to_message_id = message_id;
			work.given_by_actor_id = actor_id;
			work.given_by_name = given_by;
			work.given_at = agent_timestamp_now();
			work.from_shared_folder = from_shared_folder;
			work.state = "waiting";
			save_assignment(work);
			// Only work still waiting: the work it is doing may still be in its line.
			for (const AgentAssignment &earlier : given) {
				if (earlier.id == work.id || earlier.state != "waiting") continue;
				if (std::find(agent.waiting_work.begin(), agent.waiting_work.end(), earlier.id) != agent.waiting_work.end()) ++ahead;
			}
			// Its line is not one of its settings: an open edit of the agent stays
			// current.
			agent.waiting_work.push_back(work.id);
			save_agent(agent);
			given_to = agent;
		}
	}
	// Work that cannot begin at once - the colleague at work on another, other
	// work given before it, the colleague paused - is answered at once in its
	// thread, before the scheduler is told of it; work that begins at once is
	// answered as the colleague takes it on.
	const std::string waiting = waiting_words(is_paused, AgentScheduler::shared().is_executing(agent_id), ahead, current_here, is_joined);
	if (waiting.empty() == false && given_to.has_value() == true) {
		std::function<std::string(const Agent &, const AgentAssignment &, const std::string &)> say;
		{
			const std::lock_guard<std::mutex> lock(installed_mutex());
			say = installed().say_waiting;
		}
		if (say) {
			try {
				(void)say(*given_to, work, waiting);
			} catch (const std::exception &) {
			}
		}
	}
	AgentScheduler::shared().request_check(agent_id, "assignment");
	// Its team in that Matter sees it from its first piece of work there.
	publish_card(agent_id);
	return work;
}

} // namespace

std::vector<std::string> take_up_mentions(const MentionMessage &message) {
	std::vector<std::string> taken;
	// A colleague's personal chat with a member is theirs alone: a name written
	// there calls no other colleague into it.
	std::string chatOwner;
	if (message.chat_id.empty() == false) {
		try {
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			for (const Agent &each : load_agents())
				if (each.chat_id.value_or(std::string()) == message.chat_id) chatOwner = each.id;
		} catch (const std::exception &) {
		}
	}
	for (const std::string &mention : message.mentions) {
		if (mention.rfind("agent_", 0) != 0) continue;
		if (chatOwner.empty() == false && mention != chatOwner) continue;
		std::optional<Agent> agent;
		try {
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			agent = load_agent(mention);
		} catch (const std::exception &) {
		}
		// A colleague another computer runs takes it up there; one of the firm
		// takes it up in whichever Matter it was named.
		if (agent.has_value() == false) continue;
		if (agent->status != AgentStatus::Active && agent->status != AgentStatus::Paused) continue;
		// A copy of the firm's colleague answers only the member here, in its own
		// chat with them: a Matter's threads are the computer's that runs it,
		// which would otherwise answer too.
		if (agent->is_firm_copy == true && (agent->status != AgentStatus::Active || message.posted_here == false
				|| agent->chat_id.value_or(std::string()) != message.chat_id)) {
			continue;
		}
		try {
			(void)assign_mention(agent->id, message.matter_id, message.text, message.chat_id, message.message_id,
				message.author_actor_id, message.author_name, message.posted_here);
			taken.push_back(agent->id);
		} catch (const std::exception &) {
		}
	}
	return taken;
}

std::optional<AgentAssignment> give_back_after_part(const AgentAssignment &ended) {
	if (ended.state != "done" && ended.state != "failed") return std::nullopt;
	if (ended.matter_id.empty() == true || ended.chat_id.empty() == true) return std::nullopt;
	const std::string &lead = ended.given_by_actor_id;
	// Only a colleague gives a part; work a colleague gave itself ends its round.
	if (lead.rfind("agent_", 0) != 0 || lead == ended.agent_id) return std::nullopt;
	// Two parts ending at once give back one piece of work between them.
	static std::mutex giving;
	const std::lock_guard<std::mutex> once(giving);
	std::optional<Agent> giver;
	// The parts of this round - given in the thread since the giver's work
	// there was last given back - in the order given.
	std::vector<std::pair<AgentAssignment, std::string>> round;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		giver = load_agent(lead);
		if (giver.has_value() == false || giver->is_firm_copy == true
				|| (giver->status != AgentStatus::Active && giver->status != AgentStatus::Paused))
			return std::nullopt;
		std::string lastGivenBack;
		for (const AgentAssignment &own : load_assignments(lead)) {
			if (own.given_by_actor_id != lead || own.chat_id != ended.chat_id) continue;
			if (own.state == "waiting" || own.state == "working") return std::nullopt;
			if (own.given_at > lastGivenBack) lastGivenBack = own.given_at;
		}
		for (const Agent &each : load_agents()) {
			if (each.id == lead) continue;
			for (const AgentAssignment &part : load_assignments(each.id)) {
				if (part.given_by_actor_id != lead || part.chat_id != ended.chat_id) continue;
				if (part.state == "waiting" || part.state == "working") return std::nullopt;
				if (part.given_at > lastGivenBack) round.emplace_back(part, each.name);
			}
		}
	}
	std::sort(round.begin(), round.end(), [](const auto &left, const auto &right) { return left.first.given_at < right.first.given_at; });
	std::string ends;
	for (const auto &[part, name] : round) {
		const std::string what = "“" + start_of(part.instruction) + "”";
		const std::string why = trimmed(part.message);
		ends += (ends.empty() == true ? "" : "; ") + name + (part.state == "done" ? " finished " + what
			: part.state == "failed" ? " could not finish " + what + (why.empty() == true ? std::string() : " (" + why + ")")
			: " was stopped before finishing " + what);
	}
	if (ends.empty() == true) ends = "its answer is in the thread";
	// A full stop after them, unless the last one's quoted words end a sentence.
	bool isEnded = false;
	const std::string close = "\u201d";
	if (ends.size() > close.size() && ends.compare(ends.size() - close.size(), close.size(), close) == 0) {
		const std::string inner = ends.substr(0, ends.size() - close.size());
		for (const std::string mark : {".", "?", "!", "\u2026"})
			if (inner.size() >= mark.size() && inner.compare(inner.size() - mark.size(), mark.size(), mark) == 0) isEnded = true;
	}
	ends += isEnded == true ? " " : ". ";
	const std::string words = round.size() > 1
		? "The parts you gave in this thread have ended: " + ends + "Read what came of them above, bring them together with your "
			"own work here, and finish what you were asked."
		: "The part you gave in this thread has ended: " + ends
			+ "Read what came of it above, bring it together with your own work here, and finish what you were asked.";
	return give_work(lead, ended.matter_id, words, lead, giver->name, ended.chat_id, "", false);
}

std::optional<std::string> why_no_part(const std::string &agent_id) {
	if (AgentScheduler::shared().is_started() == false) return std::string("no AI colleague works on this computer now");
	std::optional<Agent> agent;
	try {
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = load_agent(agent_id);
	} catch (const std::exception &) {
	}
	// A copy of a colleague another computer carries answers only its own chat
	// here (take_up_mentions).
	if (agent.has_value() == false || agent->is_firm_copy == true) return std::string("another of the firm's computers carries it");
	if (agent->status == AgentStatus::Active || agent->status == AgentStatus::Paused) return std::nullopt;
	return std::string(agent->status == AgentStatus::Stopped ? "it is stopped" : "it is not approved yet");
}

AgentAssignment withdraw_assignment(const std::string &agent_id, const std::string &assignment_id) {
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	Agent agent = require_agent(agent_id);
	std::optional<AgentAssignment> work = load_assignment(agent_id, assignment_id);
	if (work.has_value() == false) throw AgentRefused("This work no longer exists.");
	if (work->state == "working") throw AgentRefused("The agent is doing this work now. Stop the run to end it.");
	if (work->state != "waiting") throw AgentRefused("This work is no longer waiting.");
	work->state = "withdrawn";
	work->finished_at = agent_timestamp_now();
	work->message.clear();
	save_assignment(*work);
	agent.waiting_work.erase(std::remove(agent.waiting_work.begin(), agent.waiting_work.end(), assignment_id), agent.waiting_work.end());
	save_agent(agent);
	return *work;
}

std::vector<AgentAssignment> list_assignments(const std::string &agent_id, std::size_t most_finished) {
	std::vector<AgentAssignment> all;
	Agent agent;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agent = require_agent(agent_id);
		all = load_assignments(agent_id);
	}
	std::vector<AgentAssignment> listed;
	for (const AgentAssignment &work : all)
		if (work.state == "working") listed.push_back(work);
	for (const std::string &id : agent.waiting_work)
		for (const AgentAssignment &work : all)
			if (work.id == id && work.state == "waiting") listed.push_back(work);
	std::vector<AgentAssignment> finished;
	for (const AgentAssignment &work : all)
		if (work.state != "working" && work.state != "waiting") finished.push_back(work);
	// The work given last comes first.
	std::reverse(finished.begin(), finished.end());
	if (finished.size() > most_finished) finished.resize(most_finished);
	listed.insert(listed.end(), finished.begin(), finished.end());
	return listed;
}

bool has_work_in_line(const std::string &agent_id, const std::string &chat_id) {
	if (agent_id.empty() == true || chat_id.empty() == true) return false;
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	std::optional<Agent> agent;
	try {
		agent = load_agent(agent_id);
	} catch (const std::exception &) {
		return false;
	}
	if (agent.has_value() == false) return false;
	for (const AgentAssignment &work : load_assignments(agent_id)) {
		if (work.state != "waiting" || work.chat_id != chat_id) continue;
		if (std::find(agent->waiting_work.begin(), agent->waiting_work.end(), work.id) != agent->waiting_work.end()) return true;
	}
	return false;
}

namespace {

std::size_t end_work_in_chat(const std::string &chat_id, const std::string &why) {
	if (chat_id.empty() == true) return 0;
	std::vector<std::string> inHand;
	std::vector<std::string> changed;
	std::size_t ended = 0;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (Agent agent : load_agents()) {
			bool isChanged = false;
			for (AgentAssignment work : load_assignments(agent.id)) {
				if (work.chat_id != chat_id) continue;
				if (work.state == "waiting") {
					work.state = "withdrawn";
					work.finished_at = agent_timestamp_now();
					work.message = why;
					save_assignment(work);
					agent.waiting_work.erase(std::remove(agent.waiting_work.begin(), agent.waiting_work.end(), work.id),
						agent.waiting_work.end());
					isChanged = true;
					++ended;
				} else if (work.state == "working") {
					inHand.push_back(agent.id);
					++ended;
				}
			}
			if (isChanged == true) {
				save_changed(agent);
				changed.push_back(agent.id);
			}
		}
	}
	// The piece in hand stops as a Stop on its run stops it; the colleague then
	// takes up its next piece from elsewhere, if any waits.
	for (const std::string &id : inHand) (void)AgentScheduler::shared().cancel_check(id);
	for (const std::string &id : changed) publish_card(id);
	if (inHand.empty() == false || changed.empty() == false) AgentScheduler::shared().agents_changed();
	return ended;
}

} // namespace

std::size_t stop_work_in_chat(const std::string &chat_id) {
	return end_work_in_chat(chat_id, "Stopped in the conversation before it began.");
}

std::size_t withdraw_work_in_deleted_chat(const std::string &chat_id) {
	return end_work_in_chat(chat_id, "Its thread was deleted before the work began.");
}

std::vector<std::string> chats_with_work(const Agent &agent, const std::optional<AgentRun> &latest_run, bool is_running) {
	std::vector<std::string> chats;
	const auto keep = [&](const std::string &chat) {
		if (chat.empty() == false && std::find(chats.begin(), chats.end(), chat) == chats.end()) chats.push_back(chat);
	};
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	if (is_running == true && latest_run.has_value() == true && latest_run->assignment_id.empty() == false) {
		try {
			if (const std::optional<AgentAssignment> work = load_assignment(agent.id, latest_run->assignment_id)) keep(work->chat_id);
		} catch (const std::exception &) {
		}
	}
	for (const std::string &id : agent.waiting_work) {
		try {
			if (const std::optional<AgentAssignment> work = load_assignment(agent.id, id); work.has_value() == true && work->state == "waiting")
				keep(work->chat_id);
		} catch (const std::exception &) {
		}
	}
	return chats;
}

// --- Memory -------------------------------------------------------

namespace {

// The notes that still count: every note no other note retires.
std::vector<MemoryNote> live_notes(const std::vector<MemoryNote> &all) {
	std::set<std::string> retired;
	for (const MemoryNote &note : all)
		if (note.retires.empty() == false) retired.insert(note.retires);
	std::vector<MemoryNote> live;
	for (const MemoryNote &note : all)
		if (note.retires.empty() == true && retired.count(note.id) == 0) live.push_back(note);
	return live;
}

// Which notes go first past a budget: the least lasting kind, the oldest.
int how_lasting(const std::string &kind) {
	if (kind == "fact") return 0;
	if (kind == "commitment") return 1;
	if (kind == "preference") return 2;
	if (kind == "decision") return 3;
	return 4;
}

std::string scope_key(const MemoryNote &note) {
	if (note.scope == "matter") return "matter:" + note.matter_id;
	if (note.scope == "person") return "person:" + note.person_actor_id;
	return "firm";
}

std::size_t budget_of(const std::string &key) {
	if (key.rfind("matter:", 0) == 0) return kMemoryMatterCharacters;
	if (key.rfind("person:", 0) == 0) return kMemoryPersonCharacters;
	return kMemoryFirmCharacters;
}

std::string memory_device() {
	const RunnerDependencies &dependencies = installed();
	const std::string device = dependencies.device_id ? dependencies.device_id() : std::string();
	return device.empty() == true ? std::string("device") : device;
}

MemoryNote retirement_of(const MemoryNote &note, const std::string &device) {
	MemoryNote retire;
	retire.id = new_memory_note_id();
	retire.kind = note.kind;
	retire.scope = note.scope;
	retire.matter_id = note.matter_id;
	retire.person_actor_id = note.person_actor_id;
	retire.retires = note.id;
	retire.at = agent_timestamp_now();
	retire.device_id = device;
	return retire;
}

std::string folded(const std::string &text) {
	std::string plain;
	for (const unsigned char character : text) {
		if (std::isspace(character) != 0) {
			if (plain.empty() == false && plain.back() != ' ') plain.push_back(' ');
			continue;
		}
		plain.push_back(static_cast<char>(std::tolower(character)));
	}
	while (plain.empty() == false && (plain.back() == ' ' || plain.back() == '.')) plain.pop_back();
	return plain;
}

// What this computer wrote and let go itself goes from its file together, so
// its file holds only what still counts; what it let go of another computer's
// stays, so that one's note stays gone.
void prune_own(std::vector<MemoryNote> &own) {
	std::set<std::string> ownIds;
	for (const MemoryNote &note : own)
		if (note.retires.empty() == true) ownIds.insert(note.id);
	std::set<std::string> letGo;
	for (const MemoryNote &note : own)
		if (note.retires.empty() == false && ownIds.count(note.retires) > 0) letGo.insert(note.retires);
	own.erase(std::remove_if(own.begin(), own.end(), [&](const MemoryNote &note) {
		return note.retires.empty() == true ? letGo.count(note.id) > 0 : letGo.count(note.retires) > 0;
	}), own.end());
}

std::string kind_words(const MemoryNote &note) {
	if (note.kind == "decision") return "Decided: ";
	if (note.kind == "commitment") return "You committed: ";
	return std::string();
}

std::string grouped(std::size_t number) {
	std::string digits = std::to_string(number);
	for (std::size_t at = digits.size(); at > 3; at -= 3) digits.insert(at - 3, ",");
	return digits;
}

// --- The walls between clients ---------------------------------------

// The names that identify each Matter's client, as the application knows them,
// folded once (Platform/TextFold) for a note to be read against.
struct ClientNames {
	std::vector<MatterIdentities> matters;
};

ClientNames client_names() {
	ClientNames names;
	const RunnerDependencies &dependencies = installed();
	if (!dependencies.matter_identities) return names;
	try {
		names.matters = dependencies.matter_identities();
	} catch (const std::exception &) {
		return names;
	}
	for (MatterIdentities &matter : names.matters) {
		std::vector<std::string> folded;
		for (const std::string &name : matter.names) {
			const std::string one = platform::folded_text(name);
			if (one.empty() == false) folded.push_back(one);
		}
		matter.names = std::move(folded);
	}
	return names;
}

// A letter or a digit in any script: what a name may not run into.
bool word_point(utf8proc_int32_t point) {
	const utf8proc_category_t category = utf8proc_category(point);
	return category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_NO;
}

// Whether folded words carry a folded name whole, with no letter or digit beside it.
bool carries_name(const std::string &words, const std::string &name) {
	for (std::size_t at = words.find(name); at != std::string::npos; at = words.find(name, at + 1)) {
		bool joined = false;
		if (at > 0) {
			std::size_t start = at - 1;
			while (start > 0 && (static_cast<unsigned char>(words[start]) & 0xC0) == 0x80) --start;
			utf8proc_int32_t point = -1;
			utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(words.data() + start),
				static_cast<utf8proc_ssize_t>(at - start), &point);
			joined = point >= 0 && word_point(point);
		}
		const std::size_t end = at + name.size();
		if (joined == false && end < words.size()) {
			utf8proc_int32_t point = -1;
			utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(words.data() + end),
				static_cast<utf8proc_ssize_t>(words.size() - end), &point);
			joined = point >= 0 && word_point(point);
		}
		if (joined == false) return true;
	}
	return false;
}

// The Matters whose client a note's words name.
std::set<std::string> matters_named(const std::string &text, const ClientNames &names) {
	std::set<std::string> named;
	if (names.matters.empty() == true) return named;
	const std::string words = platform::folded_text(text);
	for (const MatterIdentities &matter : names.matters) {
		for (const std::string &name : matter.names) {
			if (carries_name(words, name) == true) {
				named.insert(matter.matter_id);
				break;
			}
		}
	}
	return named;
}

// Where a note is filed: never wider than where it was learned. In a Matter's
// thread a fact, a decision or a commitment is that Matter's, whatever its
// label; a rule or a preference may be the firm's or the member's only while
// it names no client. In a personal chat a note that names exactly one of the
// member's Matters is that Matter's; any other note about a Matter, or naming
// a client, is the member's, never the firm's. A note with no place it may be
// filed is not kept.
struct Filing {
	std::string scope;
	std::string matter_id;
	std::string matter_name;
};

std::optional<Filing> filing_of(const KeptMemory &given, const MemoryPlace &place, const std::set<std::string> &named) {
	const bool lasting = given.kind == "rule" || given.kind == "preference";
	const bool hasMember = place.person_actor_id.empty() == false;
	if (place.matter_id.empty() == false) {
		const Filing matter{"matter", place.matter_id, place.matter_name};
		if (lasting == false || given.about == "matter" || named.empty() == false) return matter;
		if (given.about == "person") return hasMember == true ? Filing{"person", "", ""} : matter;
		return Filing{"firm", "", ""};
	}
	std::vector<std::string> theirs;
	for (const std::string &id : named)
		if (place.member_matters.count(id) > 0) theirs.push_back(id);
	if (theirs.size() == 1) return Filing{"matter", theirs.front(), place.member_matters.at(theirs.front())};
	if (named.empty() == true && given.about == "firm") return Filing{"firm", "", ""};
	if (hasMember == true) return Filing{"person", "", ""};
	return std::nullopt;
}

// Whether every client a note names may be spoken of here: in a Matter's
// thread only that Matter's, in a personal chat the member's own Matters'.
bool clients_here(const std::set<std::string> &named, const MemoryPlace &place) {
	for (const std::string &id : named) {
		const bool here = place.matter_id.empty() == false ? id == place.matter_id : place.member_matters.count(id) > 0;
		if (here == false) return false;
	}
	return true;
}

} // namespace

std::vector<MemoryNote> memory_of(const std::string &agent_id) {
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	return live_notes(load_memory_notes(agent_id));
}

MemoryDigest memory_digest(const std::string &agent_id, const MemoryPlace &place) {
	const ClientNames names = client_names();
	const std::vector<MemoryNote> live = memory_of(agent_id);
	MemoryDigest digest;
	std::vector<const MemoryNote *> rules;
	std::vector<const MemoryNote *> firm;
	std::vector<const MemoryNote *> person;
	std::map<std::string, std::vector<const MemoryNote *>> matters;
	std::size_t firmCharacters = 0;
	for (const MemoryNote &note : live) {
		bool usable = false;
		if (note.scope == "firm") firmCharacters += note.text.size();
		if (note.scope == "matter") {
			// A Matter's notes where its members are: in its threads, or with a
			// member of it in their personal chat.
			usable = place.matter_id.empty() == false ? note.matter_id == place.matter_id
				: place.member_matters.count(note.matter_id) > 0;
		} else if (note.scope == "firm" || note.scope == "person") {
			// A note wider than a Matter that names a client - kept before the
			// walls, or whose client its protection came to know since - is used
			// only where that client may be spoken of.
			const bool clientsHere = clients_here(matters_named(note.text, names), place);
			if (note.scope == "firm") {
				usable = clientsHere;
			} else {
				// A member's notes only with that member; in a Matter's thread, which
				// every member of it reads, only their rules and preferences.
				usable = clientsHere && place.person_actor_id.empty() == false && note.person_actor_id == place.person_actor_id
					&& (place.matter_id.empty() == true || note.kind == "rule" || note.kind == "preference");
			}
		}
		if (usable == false) continue;
		if (note.kind == "rule") rules.push_back(&note);
		else if (note.scope == "firm") firm.push_back(&note);
		else if (note.scope == "person") person.push_back(&note);
		else matters[note.matter_id].push_back(&note);
	}
	if (rules.empty() == true && firm.empty() == true && person.empty() == true && matters.empty() == true) return digest;
	const auto line = [&](const MemoryNote &note) {
		digest.note_ids.push_back(note.id);
		return "M" + std::to_string(digest.note_ids.size()) + ". " + kind_words(note) + note.text + "\n";
	};
	std::string text = "Your memory - your own notes from your work across the firm:\n";
	// Rules first, wherever they were learned: a newer note never pushes one out.
	if (rules.empty() == false) {
		text += "Rules you keep:\n";
		for (const MemoryNote *note : rules) text += line(*note);
	}
	if (firm.empty() == false) {
		text += "About the firm:\n";
		for (const MemoryNote *note : firm) text += line(*note);
	}
	// In a personal chat, the Matters' notes together stay within one Matter's
	// budget, the newest first.
	std::size_t matterCharacters = 0;
	for (auto &[matterId, notes] : matters) {
		std::string name = notes.front()->matter_name;
		if (place.matter_id == matterId && place.matter_name.empty() == false) name = place.matter_name;
		const auto member = place.member_matters.find(matterId);
		if (member != place.member_matters.end() && member->second.empty() == false) name = member->second;
		std::vector<const MemoryNote *> shown;
		for (auto note = notes.rbegin(); note != notes.rend(); ++note) {
			if (place.matter_id.empty() == true && matterCharacters + (*note)->text.size() > kMemoryMatterCharacters) break;
			matterCharacters += (*note)->text.size();
			shown.insert(shown.begin(), *note);
		}
		if (shown.empty() == true) continue;
		text += "About the Matter " + (name.empty() == true ? std::string("you work on") : name) + ":\n";
		for (const MemoryNote *note : shown) text += line(*note);
	}
	if (person.empty() == false) {
		text += "About " + (place.person_name.empty() == true ? std::string("the member you talk with") : place.person_name)
			+ ", whom you talk with:\n";
		for (const MemoryNote *note : person) text += line(*note);
	}
	text += "Firm notes: " + grouped(firmCharacters) + " of " + grouped(kMemoryFirmCharacters) + " characters.";
	digest.text = text;
	return digest;
}

void keep_memory(const std::string &agent_id, const MemoryPlace &place, const std::vector<KeptMemory> &kept,
	const std::vector<std::string> &forgotten_note_ids) {
	if (kept.empty() == true && forgotten_note_ids.empty() == true) return;
	const std::string device = memory_device();
	const ClientNames names = client_names();
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	if (load_agent(agent_id).has_value() == false) return;
	std::vector<MemoryNote> own = load_own_memory_notes(agent_id, device);
	std::vector<MemoryNote> live = live_notes(load_memory_notes(agent_id));
	std::set<std::string> letGo;
	for (const std::string &id : forgotten_note_ids) {
		const auto found = std::find_if(live.begin(), live.end(), [&](const MemoryNote &note) { return note.id == id; });
		if (found == live.end() || letGo.insert(id).second == false) continue;
		own.push_back(retirement_of(*found, device));
	}
	// What this computer filed wider than its words allow, before the walls: a
	// note of the firm's or a member's that names exactly one Matter's client is
	// filed again under that Matter and the wide one let go, so the client's
	// words leave the firm's record. One naming several stays, used only where
	// all of them may be spoken of.
	std::vector<MemoryNote> refiled;
	for (const MemoryNote &note : live) {
		if (note.scope == "matter" || note.device_id != device || letGo.count(note.id) > 0) continue;
		const std::set<std::string> named = matters_named(note.text, names);
		if (named.size() != 1) continue;
		const auto matter = std::find_if(names.matters.begin(), names.matters.end(),
			[&](const MatterIdentities &one) { return one.matter_id == *named.begin(); });
		MemoryNote moved = note;
		moved.id = new_memory_note_id();
		moved.scope = "matter";
		moved.matter_id = *named.begin();
		moved.matter_name = matter != names.matters.end() ? matter->matter_name : std::string();
		moved.person_actor_id.clear();
		moved.person_name.clear();
		moved.at = agent_timestamp_now();
		own.push_back(retirement_of(note, device));
		letGo.insert(note.id);
		refiled.push_back(moved);
	}
	live.erase(std::remove_if(live.begin(), live.end(), [&](const MemoryNote &note) { return letGo.count(note.id) > 0; }),
		live.end());
	for (const MemoryNote &moved : refiled) {
		own.push_back(moved);
		live.push_back(moved);
	}
	for (const KeptMemory &given : kept) {
		const std::optional<Filing> filing = filing_of(given, place, matters_named(given.text, names));
		if (filing.has_value() == false) continue;
		MemoryNote note;
		note.kind = given.kind;
		note.scope = filing->scope;
		if (note.scope == "matter") {
			note.matter_id = filing->matter_id;
			note.matter_name = filing->matter_name;
		}
		if (note.scope == "person") {
			note.person_actor_id = place.person_actor_id;
			note.person_name = place.person_name;
		}
		note.text = trimmed(given.text);
		if (note.text.empty() == true) continue;
		if (note.text.size() > kMemoryNoteCharacters) {
			std::size_t at = kMemoryNoteCharacters;
			while (at > 0 && (static_cast<unsigned char>(note.text[at]) & 0xC0) == 0x80) --at;
			note.text = note.text.substr(0, at);
		}
		// What it remembers already is not kept twice.
		const std::string key = scope_key(note);
		const std::string words = folded(note.text);
		const bool isKnown = std::any_of(live.begin(), live.end(), [&](const MemoryNote &known) {
			return scope_key(known) == key && folded(known.text) == words;
		});
		if (isKnown == true) continue;
		note.id = new_memory_note_id();
		note.project_id = place.project_id;
		note.chat_id = place.chat_id;
		note.message_id = place.message_id;
		note.said_by_actor_id = place.person_actor_id;
		note.said_by_name = place.person_name;
		note.at = agent_timestamp_now();
		note.device_id = device;
		own.push_back(note);
		live.push_back(note);
	}
	// Past a budget, the oldest of the least lasting kind go first.
	std::map<std::string, std::vector<const MemoryNote *>> byScope;
	for (const MemoryNote &note : live) byScope[scope_key(note)].push_back(&note);
	for (auto &[key, notes] : byScope) {
		std::size_t total = 0;
		for (const MemoryNote *note : notes) total += note->text.size();
		while (total > budget_of(key) && notes.size() > 1) {
			const auto first = std::min_element(notes.begin(), notes.end(), [](const MemoryNote *one, const MemoryNote *other) {
				const int oneLasts = how_lasting(one->kind);
				const int otherLasts = how_lasting(other->kind);
				return oneLasts != otherLasts ? oneLasts < otherLasts : one->at < other->at;
			});
			total -= (*first)->text.size();
			own.push_back(retirement_of(**first, device));
			notes.erase(first);
		}
	}
	prune_own(own);
	save_own_memory_notes(agent_id, device, own);
}

void forget_memory(const std::string &agent_id, const std::string &note_id) {
	const std::string device = memory_device();
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	require_agent(agent_id);
	const std::vector<MemoryNote> live = live_notes(load_memory_notes(agent_id));
	const auto found = std::find_if(live.begin(), live.end(), [&](const MemoryNote &note) { return note.id == note_id; });
	if (found == live.end()) throw AgentRefused("This note is gone already.");
	std::vector<MemoryNote> own = load_own_memory_notes(agent_id, device);
	own.push_back(retirement_of(*found, device));
	prune_own(own);
	save_own_memory_notes(agent_id, device, own);
}

void open_own_chat(const std::string &agent_id) {
	open_thread_for(agent_id);
}

std::vector<std::string> colleagues_awaiting_chat() {
	const std::lock_guard<std::mutex> lock(chatless_mutex());
	return std::vector<std::string>(chatless().begin(), chatless().end());
}

std::vector<AgentView> list_agent_views(const std::optional<std::string> &matter_id) {
	std::vector<Agent> agents;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agents = load_agents();
	}
	// Every colleague is the firm's, and may be named in any Matter: asked for a
	// Matter's, it is every one; a draft only where it was proposed.
	std::vector<AgentView> views;
	for (const Agent &agent : agents) {
		if (matter_id.has_value() == true && agent.status == AgentStatus::Draft && agent.matter_id != *matter_id) continue;
		views.push_back(view_of(agent));
	}
	return views;
}

AgentView agent_view(const std::string &agent_id) {
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	return view_of(require_agent(agent_id));
}

} // namespace singularity::agents
