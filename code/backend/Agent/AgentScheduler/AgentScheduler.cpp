#include "Agent/AgentScheduler/AgentScheduler.hpp"

#include "Agent/AgentActions/AgentActions.hpp"
#include "Agent/AgentRunner/AgentRunner.hpp"
#include "Agent/AgentStore/AgentStore.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"

#include <algorithm>
#include <ctime>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

namespace singularity::agents {

// The most colleagues that work at once on one computer: their runs wait on the
// model's answers, not on this computer, so several cost little.
constexpr int kMostRunsAtOnce = 4;
namespace {

constexpr auto kQuietAfterChange = std::chrono::seconds(10);
constexpr auto kSafetyCheckEvery = std::chrono::minutes(30);
constexpr auto kWakeAtLeastEvery = std::chrono::seconds(15);

std::string iso_of(std::chrono::system_clock::time_point time) {
	const std::time_t value = std::chrono::system_clock::to_time_t(time);
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &value);
#else
	gmtime_r(&value, &utc);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

// "2026-10-11" or "2026-10" in local time: the day an automatic run counts
// toward, the month a cost counts toward.
std::string local_day(std::chrono::system_clock::time_point time, const char *format) {
	const std::time_t value = std::chrono::system_clock::to_time_t(time);
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

bool is_runnable(const Agent &agent, const std::string &trigger) {
	if (agent.status == AgentStatus::Active) return true;
	// A paused agent runs only when the lawyer runs it; the work it was given
	// waits until it is resumed.
	return agent.status == AgentStatus::Paused && (trigger == "manual" || trigger == "first");
}

} // namespace

bool works_only_when_asked(const Agent &agent) {
	return agent.trigger == AgentTrigger::Manual && agent.mode == AgentMode::Task;
}

std::optional<std::string> timed_trigger(const Agent &agent, bool is_opening,
	std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point opened_at) {
	if (agent.status != AgentStatus::Active) return std::nullopt;
	// A run that did not finish is tried again half an hour after it began,
	// never at every wake, and after each one more in a row twice as long, up to
	// a day: it keeps trying on its own.
	const auto lastAttempt = agent.last_attempt_at.has_value() == true ? parse_agent_timestamp(*agent.last_attempt_at) : std::nullopt;
	const int doublings = std::min(std::max(agent.failed_in_a_row - 1, 0), 6);
	const auto retryWait = std::min<std::chrono::minutes>(kSafetyCheckEvery * (1 << doublings),
		std::chrono::hours(kAgentLongestRetryHours));
	const bool isRetryDue = lastAttempt.has_value() == false || now - *lastAttempt >= retryWait;
	if (works_only_when_asked(agent) == true) return std::nullopt;
	if (agent.last_check_at.has_value() == false) return isRetryDue == true ? std::optional<std::string>("first") : std::nullopt;
	if (agent.trigger == AgentTrigger::Daily || agent.trigger == AgentTrigger::Weekly) {
		if (agent.next_check_at.has_value() == false) return std::nullopt;
		const auto dueAt = parse_agent_timestamp(*agent.next_check_at);
		if (dueAt.has_value() == false || *dueAt > now) return std::nullopt;
		return *dueAt < opened_at ? "catch_up" : "schedule";
	}
	if (agent.trigger == AgentTrigger::OnChange) {
		if (is_opening == true) return "catch_up";
		const auto lastCheck = parse_agent_timestamp(*agent.last_check_at);
		if (lastCheck.has_value() == true && now - *lastCheck >= kSafetyCheckEvery && isRetryDue == true) return "safety";
	}
	return std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> parse_agent_timestamp(const std::string &value) {
	if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value.back() != 'Z') return std::nullopt;
	std::tm utc {};
	std::istringstream input(value.substr(0, 19));
	input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
	if (input.fail() == true) return std::nullopt;
#if defined(_WIN32)
	const std::time_t seconds = _mkgmtime(&utc);
#else
	const std::time_t seconds = timegm(&utc);
#endif
	if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
	return std::chrono::system_clock::from_time_t(seconds);
}

std::string next_daily_check(int daily_minute, std::chrono::system_clock::time_point from) {
	const std::time_t now = std::chrono::system_clock::to_time_t(from);
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	local.tm_hour = daily_minute / 60;
	local.tm_min = daily_minute % 60;
	local.tm_sec = 0;
	local.tm_isdst = -1;
	std::time_t due = std::mktime(&local);
	if (due <= now) {
		local.tm_mday += 1;
		local.tm_isdst = -1;
		due = std::mktime(&local);
	}
	return iso_of(std::chrono::system_clock::from_time_t(due));
}

std::string next_weekly_check(int weekly_day, int daily_minute, std::chrono::system_clock::time_point from) {
	const std::time_t now = std::chrono::system_clock::to_time_t(from);
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	int days = (weekly_day - local.tm_wday + 7) % 7;
	local.tm_mday += days;
	local.tm_hour = daily_minute / 60;
	local.tm_min = daily_minute % 60;
	local.tm_sec = 0;
	local.tm_isdst = -1;
	std::time_t due = std::mktime(&local);
	if (due <= now) {
		local.tm_mday += 7;
		local.tm_isdst = -1;
		due = std::mktime(&local);
	}
	return iso_of(std::chrono::system_clock::from_time_t(due));
}

AgentScheduler &AgentScheduler::shared() {
	static AgentScheduler scheduler;
	return scheduler;
}

AgentScheduler::~AgentScheduler() {
	stop();
}

void AgentScheduler::start() {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (is_started_ == true) return;
		is_started_ = true;
		is_stopping_ = false;
		is_first_pass_ = true;
		started_at_ = std::chrono::system_clock::now();
	}
	reconcile_interrupted_runs();
	thread_ = std::thread([this]() { loop(); });
	listener_ = std::thread([this]() { listen(); });
}

void AgentScheduler::stop() {
	std::map<std::string, std::pair<std::string, std::unique_ptr<platform::FileSystemWatcher>>> watchers;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (is_started_ == false) return;
		is_stopping_ = true;
		for (auto &[id, run] : running_)
			if (run.cancelled) run.cancelled->store(true);
		watchers.swap(watchers_);
	}
	wake_.notify_all();
	if (thread_.joinable() == true) thread_.join();
	if (listener_.joinable() == true) listener_.join();
	{
		// Every colleague's run ends at its next safe point; none outlives this.
		std::unique_lock<std::mutex> lock(mutex_);
		workers_done_.wait(lock, [this]() { return workers_ == 0; });
	}
	// A watch ends outside the lock its own callback takes.
	for (auto &[id, watch] : watchers)
		if (watch.second) watch.second->stop();
	watchers.clear();
	const std::lock_guard<std::mutex> lock(mutex_);
	is_started_ = false;
}

bool AgentScheduler::is_started() const {
	const std::lock_guard<std::mutex> lock(mutex_);
	return is_started_ == true && is_stopping_ == false;
}

void AgentScheduler::hold_own_runs(const std::string &agent_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	held_[agent_id] = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

void AgentScheduler::release_own_runs(const std::string &agent_id) {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		held_.erase(agent_id);
	}
	wake_.notify_all();
}

void AgentScheduler::request_check(const std::string &agent_id, const std::string &trigger, bool on_a_copy) {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		held_.erase(agent_id);
		// Work a lawyer gives comes before the agent's own run in progress - its
		// opening run, a timed or noticed check - which stops at its next safe
		// point instead of holding the model while the lawyer waits. A run the
		// lawyer asked for themselves finishes.
		const auto running = running_.find(agent_id);
		if (trigger == "assignment" && running != running_.end() && running->second.trigger != "assignment"
				&& running->second.trigger != "manual" && running->second.cancelled) {
			running->second.cancelled->store(true);
			yielded_.insert(agent_id);
		}
		for (const Due &queued : requested_)
			if (queued.agent_id == agent_id) return;
		// The run asked for is the one running now: it is not made twice.
		if (running != running_.end() && running->second.trigger == trigger) return;
		requested_.push_back({agent_id, trigger, on_a_copy});
	}
	wake_.notify_all();
}

bool AgentScheduler::cancel_check(const std::string &agent_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	requested_.erase(std::remove_if(requested_.begin(), requested_.end(),
		[&](const Due &item) { return item.agent_id == agent_id; }), requested_.end());
	const auto running = running_.find(agent_id);
	if (running == running_.end() || !running->second.cancelled) return false;
	stopped_by_lawyer_.insert(agent_id);
	running->second.cancelled->store(true);
	return true;
}

bool AgentScheduler::is_checking(const std::string &agent_id) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	if (running_.count(agent_id) > 0) return true;
	for (const Due &queued : requested_)
		if (queued.agent_id == agent_id) return true;
	return false;
}

bool AgentScheduler::is_executing(const std::string &agent_id) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	return running_.count(agent_id) > 0;
}

void AgentScheduler::agents_changed() {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		is_dirty_ = true;
		is_listen_due_ = true;
	}
	wake_.notify_all();
}

void AgentScheduler::set_periodic_work(std::function<void()> work) {
	const std::lock_guard<std::mutex> lock(mutex_);
	periodic_work_ = std::move(work);
}

// A check Singularity was making when it closed is said to be interrupted;
// the agent checks again on its own schedule.
void AgentScheduler::reconcile_interrupted_runs() {
	const std::lock_guard<std::mutex> lock(agent_store_mutex());
	for (const Agent &agent : load_agents()) {
		if (agent.last_run_id.has_value() == false) continue;
		std::optional<AgentRun> run;
		try {
			run = load_run(agent.id, *agent.last_run_id);
		} catch (const std::exception &) {
		}
		if (run.has_value() == false || run->status != "running") continue;
		run->status = "interrupted";
		run->stage = "done";
		run->finished_at = agent_timestamp_now();
		run->error_code = "agent.interrupted";
		run->error_message = "Singularity closed while this check ran. Nothing it found was lost; the next check looks again.";
		try {
			save_run(*run);
		} catch (const std::exception &) {
		}
	}
	// Work an agent was doing when Singularity closed is begun again, once,
	// before the rest of what it was given; closed on a second time, it says so.
	for (Agent agent : load_agents()) {
		std::vector<AgentAssignment> assignments;
		try {
			assignments = load_assignments(agent.id);
		} catch (const std::exception &) {
			continue;
		}
		std::vector<std::string> again;
		bool isChanged = false;
		for (AgentAssignment &work : assignments) {
			if (work.state != "working") continue;
			if (work.attempts < 2 && agent.status != AgentStatus::Stopped) {
				work.state = "waiting";
				work.message = "Singularity closed while the agent worked on this, so it begins it again.";
				again.push_back(work.id);
			} else {
				work.state = "failed";
				work.finished_at = agent_timestamp_now();
				work.message = "Singularity closed while the agent worked on this. Give it the work again to start over.";
			}
			try {
				save_assignment(work);
				isChanged = true;
			} catch (const std::exception &) {
			}
		}
		if (isChanged == false) continue;
		agent.waiting_work.insert(agent.waiting_work.begin(), again.begin(), again.end());
		try {
			save_agent(agent);
		} catch (const std::exception &) {
		}
	}
}

void AgentScheduler::sync_watchers() {
	std::vector<Agent> agents;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agents = load_agents();
	}
	std::map<std::string, std::string> wanted;
	for (const Agent &agent : agents)
		if (agent.status == AgentStatus::Active && agent.trigger == AgentTrigger::OnChange && agent.source_root.empty() == false)
			wanted[agent.id] = agent.source_root;

	std::vector<std::unique_ptr<platform::FileSystemWatcher>> ended;
	std::vector<std::pair<std::string, std::string>> toStart;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		for (auto watch = watchers_.begin(); watch != watchers_.end();) {
			const auto found = wanted.find(watch->first);
			if (found == wanted.end() || found->second != watch->second.first) {
				ended.push_back(std::move(watch->second.second));
				watch = watchers_.erase(watch);
				continue;
			}
			++watch;
		}
		for (const auto &[id, root] : wanted)
			if (watchers_.count(id) == 0) toStart.push_back({id, root});
	}
	for (std::unique_ptr<platform::FileSystemWatcher> &watch : ended)
		if (watch) watch->stop();
	// Begun with no lock held: the system may ask the lawyer about the folder,
	// and only this thread waits for the answer.
	for (const auto &[id, root] : toStart) {
		const std::string agentId = id;
		std::unique_ptr<platform::FileSystemWatcher> watch = platform::start_watching(std::filesystem::path(root),
			[this, agentId, root](const platform::FileSystemChangeReport &report) {
				if (change_worth_a_run(root, report) == false) return;
				{
					const std::lock_guard<std::mutex> lock(mutex_);
					noticed_[agentId] = std::chrono::steady_clock::now();
				}
				wake_.notify_all();
			});
		const std::lock_guard<std::mutex> lock(mutex_);
		if (is_stopping_ == true) {
			if (watch) ended.push_back(std::move(watch));
			continue;
		}
		watchers_[id] = {root, std::move(watch)};
	}
	for (std::unique_ptr<platform::FileSystemWatcher> &watch : ended)
		if (watch) watch->stop();
}

std::deque<AgentScheduler::Due> AgentScheduler::due_checks() {
	std::deque<Due> due;
	std::vector<Agent> agents;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agents = load_agents();
	}
	const auto now = std::chrono::system_clock::now();
	const auto steadyNow = std::chrono::steady_clock::now();
	const std::lock_guard<std::mutex> lock(mutex_);
	due.swap(requested_);
	const auto isQueued = [&](const std::string &id) {
		for (const Due &queued : due)
			if (queued.agent_id == id) return true;
		return false;
	};
	for (const Agent &agent : agents) {
		if (isQueued(agent.id) == true) continue;
		// Being set up: the action that set it up asks for its run itself.
		const auto held = held_.find(agent.id);
		if (held != held_.end()) {
			if (steadyNow < held->second) continue;
			held_.erase(held);
		}
		// Its model could not be asked a moment ago: it asks again once its
		// wait is over, not every pass.
		const auto waiting = waiting_for_model_.find(agent.id);
		if (waiting != waiting_for_model_.end() && steadyNow < waiting->second.first) continue;
		std::optional<std::string> own;
		bool isNoticedChange = false;
		if (agent.status == AgentStatus::Active) {
			if (agent.trigger == AgentTrigger::OnChange && agent.last_check_at.has_value() == true) {
				const auto noticed = noticed_.find(agent.id);
				if (noticed != noticed_.end() && steadyNow - noticed->second >= kQuietAfterChange) {
					own = "change";
					isNoticedChange = true;
				}
			}
			if (own.has_value() == false) own = timed_trigger(agent, is_first_pass_, now, started_at_);
		}
		// Work a lawyer gave comes before the agent's own runs - all but the
		// look it takes on opening, which notes what changed while Singularity
		// was closed.
		const bool hasWork = agent.status == AgentStatus::Active && agent.waiting_work.empty() == false;
		if (hasWork == true && (own.has_value() == false || is_first_pass_ == false)) {
			due.push_back({agent.id, "assignment", false});
			continue;
		}
		if (own.has_value() == false) continue;
		if (isNoticedChange == true) noticed_.erase(agent.id);
		// Decided on the copy read above: decided again as it begins (execute).
		due.push_back({agent.id, *own, true});
	}
	is_first_pass_ = false;
	return due;
}

void AgentScheduler::execute(const Due &due) {
	const std::string &agent_id = due.agent_id;
	const std::string &trigger = due.trigger;
	// Nothing new begins once Singularity is closing (Platform/AppLifecycle).
	if (platform::lifecycle::is_closing()) return;
	const bool isAssignment = trigger == "assignment";
	Agent agent;
	std::optional<AgentAssignment> work;
	const std::string runId = new_run_id();
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		const std::optional<Agent> loaded = load_agent(agent_id);
		if (loaded.has_value() == false || is_runnable(*loaded, trigger) == false) return;
		// A first run is made once: asked for twice - by its creation and by
		// the scheduler's own look - the second finds the first made.
		if (trigger == "first" && loaded->last_check_at.has_value() == true) return;
		// Decided on a copy of the agent read before now - the look's, which a
		// run may have changed since, as a first run that could not finish does
		// in stamping its attempt - it is decided again on the agent as it is: a
		// run that is no longer due is not made (a first run made twice after one
		// that failed, seen under load, 2026-11-08).
		if (due.on_a_copy == true && trigger != "change" && trigger != "assignment"
				&& timed_trigger(*loaded, trigger == "catch_up", std::chrono::system_clock::now(), started_at_).has_value() == false) return;
		agent = *loaded;
		if (isAssignment == true) {
			// The work given first is done first. Work withdrawn, or whose
			// record cannot be read, leaves the line.
			while (agent.waiting_work.empty() == false && work.has_value() == false) {
				const std::string next = agent.waiting_work.front();
				agent.waiting_work.erase(agent.waiting_work.begin());
				try {
					const std::optional<AgentAssignment> found = load_assignment(agent.id, next);
					if (found.has_value() == true && found->state == "waiting") work = found;
				} catch (const std::exception &) {
				}
			}
			if (work.has_value() == false) {
				save_agent(agent);
				return;
			}
			work->state = "working";
			work->run_id = runId;
			work->started_at = agent_timestamp_now();
			work->attempts += 1;
			work->message.clear();
			save_assignment(*work);
		}
		// A run the colleague makes on its own counts toward its day's record,
		// never toward a limit: it works as much as its work needs.
		if (trigger != "manual" && trigger != "first" && isAssignment == false) {
			const std::string today = local_day(std::chrono::system_clock::now(), "%Y-%m-%d");
			if (agent.automatic_runs_day != today) {
				agent.automatic_runs_day = today;
				agent.automatic_runs = 0;
			}
			agent.automatic_runs += 1;
		}
		agent.last_run_id = runId;
		// Work a lawyer gave is no attempt at the agent's own run: it never
		// holds back the next one.
		if (isAssignment == false) agent.last_attempt_at = agent_timestamp_now();
		save_agent(agent);
	}
	// Its stop: the one its run was given as it was dispatched, so a Stop said
	// meanwhile holds.
	std::shared_ptr<std::atomic<bool>> cancelled;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (is_stopping_ == true) return;
		Running &running = running_[agent_id];
		running.trigger = trigger;
		if (!running.cancelled) running.cancelled = std::make_shared<std::atomic<bool>>(false);
		cancelled = running.cancelled;
	}
	// The team sees it at work from the moment it begins, on every computer
	// that opens the Matter: its card says so.
	publish_card(agent_id);
	AgentRun run = isAssignment == true
		? run_agent_assignment(agent, *work, runId, runner_dependencies(), cancelled.get())
		: run_agent_check(agent, runId, trigger, runner_dependencies(), cancelled.get());
	bool gaveWay = false;
	bool closedOn = false;
	bool isClosing = false;
	bool isStoppedAsInterrupted = false;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		gaveWay = yielded_.erase(agent_id) > 0;
		const bool stoppedByLawyer = stopped_by_lawyer_.erase(agent_id) > 0;
		// A run the lawyer stopped is stopped, however it ended: a model request
		// the Stop cut short can end as interrupted, and its work went back to
		// the head of the line.
		if (stoppedByLawyer == true && run.status == "interrupted") {
			run.status = "cancelled";
			run.error_code = "agent.cancelled";
			run.error_message = "The work was stopped before it was done.";
			isStoppedAsInterrupted = true;
		}
		// Singularity closed on this run: stop says it is stopping before it
		// cancels the run, under this lock, so a run it cut short is told apart
		// from the lawyer's Stop and from a run that gave way.
		isClosing = is_stopping_ || platform::lifecycle::is_closing();
		closedOn = run.status == "interrupted"
			|| (isClosing && run.status != "completed" && !stoppedByLawyer && !gaveWay);
		// A run that reached its model ends any wait for it.
		if (run.status != "interrupted") waiting_for_model_.erase(agent_id);
	}
	if (isStoppedAsInterrupted == true) {
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		try {
			save_run(run);
		} catch (const std::exception &) {
		}
	}
	// Interrupted while Singularity stays open: its model could not be asked for
	// a cause that clears by itself. The work it was given goes back to the head
	// of its line, as work a closing cut short does at the next launch, and it
	// asks again after a wait that doubles each time - half a minute, then a
	// minute, up to ten - so a locked Mac or a dropped network costs a lawyer
	// nothing but the wait.
	if (closedOn == true && isClosing == false && isAssignment == true && work.has_value() == true) {
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			try {
				std::optional<AgentAssignment> current = load_assignment(agent_id, work->id);
				std::optional<Agent> holder = load_agent(agent_id);
				if (current.has_value() == true && holder.has_value() == true && current->state == "working") {
					current->state = "waiting";
					current->message = run.error_message;
					save_assignment(*current);
					if (std::find(holder->waiting_work.begin(), holder->waiting_work.end(), current->id) == holder->waiting_work.end())
						holder->waiting_work.insert(holder->waiting_work.begin(), current->id);
					save_agent(*holder);
				}
			} catch (const std::exception &) {
			}
		}
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			auto &waiting = waiting_for_model_[agent_id];
			const int doublings = std::min(waiting.second, 5);
			waiting.second += 1;
			waiting.first = std::chrono::steady_clock::now() + std::min<std::chrono::seconds>(std::chrono::seconds(30) * (1 << doublings),
				std::chrono::minutes(10));
		}
	}
	if (closedOn) {
		// Neither a result nor a failure: the run says it was interrupted, work
		// given stays in work, and the next launch begins it again, first
		// (reconcile_interrupted_runs). No failure is counted, no time is moved,
		// and nothing is reported.
		AgentRun interrupted = run;
		interrupted.status = "interrupted";
		interrupted.stage = "done";
		interrupted.error_code = "agent.interrupted";
		interrupted.error_message = isClosing == false && run.error_message.empty() == false ? run.error_message
			: isAssignment ? "Singularity closed while the agent worked on this, so it begins it again."
			: "Singularity closed while this check ran. Nothing it found was lost; the next check looks again.";
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			try {
				save_run(interrupted);
			} catch (const std::exception &) {
			}
		}
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			running_.erase(agent_id);
		}
		publish_card(agent_id);
		return;
	}
	bool hasMoreWork = false;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		if (work.has_value() == true) {
			work->state = run.status == "completed" ? "done" : (run.status == "cancelled" ? "stopped" : "failed");
			work->finished_at = run.finished_at;
			work->message = run.status == "completed" ? std::string() : run.error_message;
			try {
				save_assignment(*work);
			} catch (const std::exception &) {
			}
		}
		std::optional<Agent> current = load_agent(agent_id);
		if (current.has_value() == true) {
			// Work given is not a check of the folder, and what became of it
			// says nothing of the agent's own runs.
			if (run.status == "completed" && isAssignment == false) current->last_check_at = run.finished_at;
			// An own run that gave way to the lawyer's work is not made again:
			// what the lawyer gave supersedes it, and the agent's next own run
			// compares from here.
			if (gaveWay && run.status == "cancelled" && isAssignment == false) current->last_check_at = run.finished_at;
			// A cause that does not go away never costs a run every half hour:
			// each own run in a row that could not finish makes it wait longer
			// before the next (timed_trigger). It never pauses itself: a member
			// is not stopped for a cause outside them. Work it was given says
			// nothing of its own runs.
			if (run.status == "failed" && isAssignment == false) {
				current->failed_in_a_row += 1;
			} else if (run.status == "completed" && isAssignment == false) {
				current->failed_in_a_row = 0;
			}
			// What its analysis or its task's request cost counts toward its month.
			std::optional<double> cost = run.analysis.cost;
			std::string currency = run.analysis.currency;
			if (cost.has_value() == false && run.task.has_value() == true) {
				cost = run.task->cost;
				currency = run.task->currency;
			}
			if (cost.has_value() == true && *cost > 0.0) {
				const std::string month = local_day(std::chrono::system_clock::now(), "%Y-%m");
				if (current->spend_month != month) {
					current->spend_month = month;
					current->spend = 0.0;
				}
				current->spend += *cost;
				if (currency.empty() == false) current->spend_currency = currency;
			}
			// Its own chat is kept the moment it is made (AgentProduction
			// open_thread_of), never taken from a run: a run answers where it was
			// named, and a Matter's thread taken for its own left that Matter's
			// threads. Its next time is set by its own runs; work given in
			// between never moves it.
			if (isAssignment == false && current->trigger == AgentTrigger::Daily && current->status == AgentStatus::Active)
				current->next_check_at = next_daily_check(current->daily_minute, std::chrono::system_clock::now());
			if (isAssignment == false && current->trigger == AgentTrigger::Weekly && current->status == AgentStatus::Active)
				current->next_check_at = next_weekly_check(current->weekly_day, current->daily_minute, std::chrono::system_clock::now());
			hasMoreWork = current->waiting_work.empty() == false && current->status == AgentStatus::Active;
			try {
				save_agent(*current);
			} catch (const std::exception &) {
			}
		}
	}
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		running_.erase(agent_id);
	}
	// The team sees how it went (Agent/AgentCard), and the agent tells its
	// thread what it found.
	publish_card(agent_id);
	{
		const RunnerDependencies &dependencies = runner_dependencies();
		if (dependencies.report_run) {
			try {
				std::optional<Agent> current;
				{
					const std::lock_guard<std::mutex> lock(agent_store_mutex());
					current = load_agent(agent_id);
				}
				if (current.has_value() == true) dependencies.report_run(*current, run);
			} catch (const std::exception &) {
			}
		}
	}
	// Agents that follow this one take up what it reported.
	if (run.status == "completed" && run_needs_review(run) == true) {
		std::vector<Agent> followers;
		{
			const std::lock_guard<std::mutex> lock(agent_store_mutex());
			for (const Agent &candidate : load_agents())
				if (candidate.status == AgentStatus::Active && candidate.trigger == AgentTrigger::AfterAgent
						&& candidate.follows_agent_id == agent_id && candidate.matter_id == agent.matter_id && candidate.id != agent_id)
					followers.push_back(candidate);
		}
		for (const Agent &follower : followers) request_check(follower.id, "after_agent");
	}
	// Work a colleague gave as a part of its own: once its parts have ended,
	// that colleague is given its work back, to finish what it was asked
	// (finding f-lead-not-woken).
	if (work.has_value() == true) {
		try {
			(void)give_back_after_part(*work);
		} catch (const std::exception &) {
		}
	}
	// The next piece of work it was given, at once.
	if (hasMoreWork == true) request_check(agent_id, "assignment");
	agents_changed();
}

void AgentScheduler::open_missing_chats() {
	// A colleague whose chat could not open at its last try is tried again at
	// every wake until it opens: onboarding hires the first colleague before
	// the Matters folder is chosen, and with no desk yet its chat waited five
	// minutes or a restart, so its member could not write to it (it said it
	// could not be written to from this computer yet). With no folder a
	// try makes nothing.
	for (const std::string &agentId : colleagues_awaiting_chat()) open_own_chat(agentId);
	// At the first wake, then every five minutes at most: every colleague with
	// no chat of its own.
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto now = std::chrono::steady_clock::now();
		if (chats_checked_at_ != std::chrono::steady_clock::time_point {} && now - chats_checked_at_ < std::chrono::minutes(5)) return;
		chats_checked_at_ = now;
	}
	std::vector<std::string> missing;
	std::vector<Agent> agents;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		agents = load_agents();
		for (const Agent &agent : agents)
			if ((agent.status == AgentStatus::Active || agent.status == AgentStatus::Paused) && agent.chat_id.has_value() == false)
				missing.push_back(agent.id);
	}
	for (const std::string &agentId : missing) open_own_chat(agentId);
	// What every computer of the firm holds of its colleagues, written where it
	// changed since: a colleague hired before the firm's record was kept
	// reaches it too.
	publish_firm_colleagues(agents);
}

void AgentScheduler::show_colleagues_where_carried() {
	// At the first wake, then every half minute at most: one small file read.
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto now = std::chrono::steady_clock::now();
		if (carried_checked_at_ != std::chrono::steady_clock::time_point {} && now - carried_checked_at_ < std::chrono::seconds(30)) return;
		carried_checked_at_ = now;
	}
	const RunnerDependencies &dependencies = runner_dependencies();
	if (!dependencies.carried_matters) return;
	std::set<std::string> carried;
	try {
		for (const std::string &matterId : dependencies.carried_matters()) carried.insert(matterId);
	} catch (const std::exception &) {
		return;
	}
	bool isNew = false;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		isNew = std::any_of(carried.begin(), carried.end(), [this](const std::string &id) { return carried_shown_.count(id) == 0; });
		carried_shown_ = carried;
	}
	if (isNew == false) return;
	// A Matter shared since, or brought here: each colleague at work here shows
	// in it from now on, as one given work there does.
	std::vector<std::string> shown;
	{
		const std::lock_guard<std::mutex> lock(agent_store_mutex());
		for (const Agent &agent : load_agents())
			if (agent.status == AgentStatus::Active || agent.status == AgentStatus::Paused) shown.push_back(agent.id);
	}
	for (const std::string &agentId : shown) publish_card(agentId);
}

void AgentScheduler::loop() {
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (is_stopping_ == true) return;
			// Once Singularity is closing nothing new begins: the loop only waits
			// to be stopped (Platform/AppLifecycle).
			if (platform::lifecycle::is_closing()) {
				wake_.wait_for(lock, kWakeAtLeastEvery, [this]() { return is_stopping_ == true; });
				continue;
			}
		}
		sync_watchers();
		// A colleague's own chat opens as it is hired; one that could not open
		// then - no workspace yet, a desk not made - opens now, so its members
		// can write to it. Each once a run of the program.
		open_missing_chats();
		show_colleagues_where_carried();
		std::function<void()> periodic;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			periodic = periodic_work_;
		}
		if (periodic) {
			try {
				periodic();
			} catch (const std::exception &) {
			}
		}
		std::deque<Due> due;
		try {
			due = due_checks();
		} catch (const std::exception &) {
		}
		for (const Due &item : due) {
			const std::string &agentId = item.agent_id;
			const std::string &trigger = item.trigger;
			if (platform::lifecycle::is_closing()) break;
			{
				const std::lock_guard<std::mutex> lock(mutex_);
				if (is_stopping_ == true) return;
				// One run at a time for each colleague, and so many at once on this
				// computer: what is due for one at work, or past that, waits for a
				// run to end.
				if (running_.count(agentId) > 0 || static_cast<int>(running_.size()) >= kMostRunsAtOnce) {
					const bool isKept = std::any_of(later_.begin(), later_.end(),
						[&](const Due &kept) { return kept.agent_id == agentId && kept.trigger == trigger; });
					if (isKept == false) later_.push_back(item);
					continue;
				}
				running_[agentId] = Running{trigger, std::make_shared<std::atomic<bool>>(false)};
				++workers_;
			}
			// Each run on a thread of its own, beside the others.
			std::thread([this, item, agentId = agentId]() {
				try {
					execute(item);
				} catch (const std::exception &) {
				}
				{
					const std::lock_guard<std::mutex> lock(mutex_);
					running_.erase(agentId);
					// What waited for a run to end is due again.
					while (later_.empty() == false) {
						requested_.push_back(later_.front());
						later_.pop_front();
					}
					is_dirty_ = true;
					--workers_;
				}
				wake_.notify_all();
				workers_done_.notify_all();
			}).detach();
		}
		std::unique_lock<std::mutex> lock(mutex_);
		if (is_stopping_ == true) return;
		wake_.wait_for(lock, kWakeAtLeastEvery,
			[this]() { return is_stopping_ == true || requested_.empty() == false || is_dirty_ == true; });
		is_dirty_ = false;
	}
}

// What members ask of this computer's colleagues - messages written on other
// computers, which reach this one with the Matter - is taken up apart from the
// runs, which the loop makes one after another: a colleague at work on a long
// run still has the next request recorded, in its order, and begins it when
// the run ends.
void AgentScheduler::listen() {
	for (;;) {
		if (platform::lifecycle::is_closing() == false) {
			const std::function<void()> take_up = runner_take_up_mentions();
			if (take_up) {
				try {
					take_up();
				} catch (const std::exception &) {
				}
			}
		}
		std::unique_lock<std::mutex> lock(mutex_);
		if (is_stopping_ == true) return;
		wake_.wait_for(lock, kWakeAtLeastEvery, [this]() { return is_stopping_ == true || is_listen_due_ == true; });
		is_listen_due_ = false;
		if (is_stopping_ == true) return;
	}
}

bool change_worth_a_run(const std::string &root, const platform::FileSystemChangeReport &report) {
	if (report.affected_paths.empty() == true) return true;
	for (const std::string &affected : report.affected_paths) {
		const std::filesystem::path relative = std::filesystem::path(affected).lexically_relative(std::filesystem::path(root));
		bool ignored = relative.empty() == true || *relative.begin() == "..";
		for (const std::filesystem::path &part : relative) {
			const std::string name = part.string();
			if (name.size() > 1 && name.front() == '.' && name != "..") ignored = true;
		}
		const std::string file = relative.filename().string();
		if (file.find(".lm-tmp-") != std::string::npos || relative.extension() == ".part" || file.rfind("~$", 0) == 0)
			ignored = true;
		if (ignored == false) return true;
	}
	return false;
}

} // namespace singularity::agents
