#pragma once

#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace singularity::agents {

// When agents check their rooms, on this computer, while Singularity is open:
//
// - when the lawyer runs one;
// - for a watch kept "when files change": ten quiet seconds after the folder
//   last changed, and every thirty minutes besides, because a network drive
//   does not always announce what another computer wrote;
// - for a daily or weekly agent: at its time, and once at launch for a time
//   missed while Singularity was closed;
// - for a watch kept "when files change": once at launch as well, so nothing
//   that changed while it was closed waits for the next change;
// - when a lawyer gives it work (AgentAssignment): the work given first is
//   done first, before the agent's own runs, while it is not paused.
//
// An agent the lawyer runs by hand runs only then.
//
// Each colleague makes one run at a time, and the colleagues work beside one another, as a firm's members do: a
// colleague at work on a long piece never keeps another waiting, up to kMostRunsAtOnce at once on a computer.
// Nothing here runs when Singularity is closed, and nothing says it does. Folder watches are begun and ended on this
// object's own thread, never under a lock another part of Singularity waits on: a permission prompt the system shows
// for a folder stalls only this thread.
class AgentScheduler {
public:
	static AgentScheduler &shared();

	void start();
	void stop();
	bool is_started() const;

	// A check of one agent, as soon as none of its own runs. It also ends a hold
	// (hold_own_runs) on that agent. `on_a_copy`: what asks for it decided on a
	// copy of the agent read earlier - as the scheduler's own look does - so the
	// run is decided again on the agent as it is when it begins.
	void request_check(const std::string &agent_id, const std::string &trigger, bool on_a_copy = false);
	// The agent is being set up - made, approved, resumed or given another
	// folder - and asks for its run itself once it is ready (request_check).
	// Until then its own timed runs wait, so that run is never made twice, nor
	// before its thread is open. A hold no request ends lapses after half a
	// minute, and the agent's own runs go on.
	void hold_own_runs(const std::string &agent_id);
	// Ends a hold when the setting up asks for no run after all.
	void release_own_runs(const std::string &agent_id);
	// Asks the running check of an agent to stop; false when none runs.
	bool cancel_check(const std::string &agent_id);
	// Its run is under way, or it waits its turn behind another agent's.
	bool is_checking(const std::string &agent_id) const;
	// Its run is under way now - not waiting its turn.
	bool is_executing(const std::string &agent_id) const;
	// The agents changed: watches and schedules are brought in line.
	void agents_changed();

	// Work done on the scheduler's thread each time it wakes, for another
	// part of the application (the AI apps' requests, Agent/McpConnections).
	void set_periodic_work(std::function<void()> work);

	~AgentScheduler();

private:
	// One run asked for: the colleague, what for, and whether it was decided on a
	// copy of the agent (request_check). The scheduler's look reads the agents
	// once and a run may change one before what the look decided begins.
	struct Due {
		std::string agent_id;
		std::string trigger;
		bool on_a_copy = false;
	};
	AgentScheduler() = default;
	void loop();
	// Opens the own chat of each colleague that has none yet.
	void open_missing_chats();
	// Shows this computer's colleagues in each Matter it has come to carry with
	// the firm's other computers since it last looked (Agent/AgentCard).
	void show_colleagues_where_carried();
	void listen();
	void sync_watchers();
	void reconcile_interrupted_runs();
	void execute(const Due &due);
	std::deque<Due> due_checks();

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::thread thread_;
	// Takes up what members ask (RunnerDependencies::take_up_mentions) apart
	// from the runs, so a long run never leaves a request unrecorded.
	std::thread listener_;
	bool is_started_ = false;
	bool is_stopping_ = false;
	bool is_first_pass_ = true;
	// Something changed that the thread should look at before its next wake.
	bool is_dirty_ = false;
	// The same, for the listener.
	bool is_listen_due_ = false;
	std::deque<Due> requested_;
	// Agents being set up (hold_own_runs), until when their own runs wait.
	std::map<std::string, std::chrono::steady_clock::time_point> held_;
	// Agents whose model could not be asked for a cause that clears by itself -
	// the Mac locked, the network down: until when they wait, and how many
	// times in a row it was so, which doubles the wait.
	std::map<std::string, std::pair<std::chrono::steady_clock::time_point, int>> waiting_for_model_;
	std::map<std::string, std::chrono::steady_clock::time_point> noticed_;
	// The runs at work now, one at most per colleague: what each runs for,
	// and how it is told to stop.
	struct Running {
		std::string trigger;
		std::shared_ptr<std::atomic<bool>> cancelled;
	};
	std::map<std::string, Running> running_;
	// What was due for a colleague at work already, or while as many as may
	// worked: taken up as a run ends, never lost.
	std::deque<Due> later_;
	// The runs' own threads, counted: stop() waits for every one to end.
	int workers_ = 0;
	std::condition_variable workers_done_;
	// Agents whose own run gave way to work a lawyer gave them (request_check).
	std::set<std::string> yielded_;
	// When the colleagues' missing own chats were last looked for.
	std::chrono::steady_clock::time_point chats_checked_at_ {};
	// The Matters carried with other computers that the colleagues' cards were
	// put into, and when that was last looked at.
	std::set<std::string> carried_shown_;
	std::chrono::steady_clock::time_point carried_checked_at_ {};
	// Agents whose running check the lawyer stopped (cancel_check): a Stop made
	// just before Singularity closes stays a Stop.
	std::set<std::string> stopped_by_lawyer_;
	std::map<std::string, std::pair<std::string, std::unique_ptr<platform::FileSystemWatcher>>> watchers_;
	std::function<void()> periodic_work_;
	std::chrono::system_clock::time_point started_at_;
};

// When a daily check at `daily_minute` (local time) next falls due after
// `from`, as "2026-10-11T07:00:00Z".
std::string next_daily_check(int daily_minute, std::chrono::system_clock::time_point from);
// When a weekly check on `weekly_day` (0 Sunday) at `daily_minute` next falls
// due after `from`.
std::string next_weekly_check(int weekly_day, int daily_minute, std::chrono::system_clock::time_point from);

struct Agent;

// Why an agent is due without anyone asking, or nothing: its first run, the
// time it was set for ("schedule"), a time or change missed while Singularity
// was closed ("catch_up", only on opening), or the regular look
// at a watched folder ("safety"). A change a folder announces is the
// scheduler's own.
std::optional<std::string> timed_trigger(const Agent &agent, bool is_opening,
	std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point opened_at);

// A colleague that does tasks and is set to work when asked: it never runs on
// its own, not even a first time - its work is what it is given. A watch,
// even one run by hand, still records its folder first.
bool works_only_when_asked(const Agent &agent);

// Reads "2026-10-11T07:00:00Z"; nothing for anything else.
std::optional<std::chrono::system_clock::time_point> parse_agent_timestamp(const std::string &value);

// Whether a change the folder watcher reported under `root` is one an agent
// that works on change looks at: not the Matter's own Singularity data (.lm -
// the agent's card, written after every run, would wake it again all day), a
// hidden file, or a file still being written (a staged ".lm-tmp-" or ".part"
// copy, an Office lock "~$"). A report that names no path is a change.
bool change_worth_a_run(const std::string &root, const platform::FileSystemChangeReport &report);

} // namespace singularity::agents
