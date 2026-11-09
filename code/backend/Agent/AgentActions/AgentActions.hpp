#pragma once

#include "Agent/AgentRunner/AgentRunner.hpp"
#include "Agent/Types/Agent.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::agents {

// The commands a lawyer gives an agent, each checked here against the
// agent's state - a button that should not be pressed is refused by the
// backend, not only hidden by the page.
//
//   draft  --approve-->  active  <--pause/resume-->  paused
//     |                    |                           |
//   decline              stop                        stop
//     v                    v                           v
//   stopped  <-------------+---------------------------+
//
// A run belongs to an active agent, or to a paused one the lawyer runs by
// hand. A stopped agent never runs again; its runs and reports stay.

struct AgentSettings {
	std::string matter_id;
	std::string name;
	std::string goal;
	std::string source_root;
	AgentTrigger trigger = AgentTrigger::OnChange;
	int daily_minute = 9 * 60;
	int weekly_day = 1;
	AgentModelPolicy model_policy = AgentModelPolicy::MatterSetup;
	// The template it was begun from, and how it works (Agent/Types/Agent.hpp).
	std::string kind = kAgentKindDataRoomWatch;
	AgentMode mode = AgentMode::Changes;
	// The agent whose reports it follows, for AgentTrigger::AfterAgent.
	std::string follows_agent_id;
	// The character it is drawn as (kAgentCharacters, or a species of the Agent
	// Family, kAgentSpecies); empty keeps its own.
	std::string character;
	// The firm's Playbook it checks the documents against, when it does.
	std::string playbook_id;
	// How it works, in a sentence (Agent/ColleagueBrief). Last, so settings
	// written out in order stay as they were.
	std::string manner;
};

// What a lawyer reads of an agent: its record, its latest run and the one
// state that sums them.
struct AgentView {
	Agent agent;
	std::optional<AgentRun> latest_run;
	// waiting_for_approval | running | needs_review | access_needed | failed |
	// paused | stopped | watching. "running" also while it waits its turn behind
	// another agent's run: one runs at a time on this computer.
	std::string state;
	// Its run is under way now; false while it waits its turn.
	bool is_running = false;
	// What its character acts out now (agent_presence).
	std::string presence;
};

// The application installs what agents need from the rest of Singularity -
// its Matters, its document reading, its model path - once, at launch.
void install_runner_dependencies(RunnerDependencies dependencies);
const RunnerDependencies &runner_dependencies();
// What takes up the requests members wrote (RunnerDependencies::
// take_up_mentions), copied safely for the scheduler's listener.
std::function<void()> runner_take_up_mentions();

AgentView create_agent(const AgentSettings &settings, const std::string &actor_id);
// A colleague of the firm: hired with its name, its face, its office and how
// it works, and belonging to no Matter. It takes part in a Matter only when
// it is given work there; until then it reaches no Matter's files or threads
// and runs nothing of its own.
struct FirmColleagueHire {
	std::string name;
	std::string character;
	Agent::Office office;
	std::string manner;
	// Its standing task in the member's words, which may be empty. A colleague
	// is always the firm's: no Matter is chosen when it is hired.
	std::string goal;
};
AgentView hire_firm_colleague(const FirmColleagueHire &hire, const std::string &actor_id);
// The firm's administrator changes a colleague: its name, face, office,
// manner and standing work, checked as at its hiring; one that is
// archived is restored first. On the computer that runs it only - a copy
// follows the firm's record.
AgentView edit_firm_colleague(const std::string &agent_id, const FirmColleagueHire &edit);
// The firm's administrator changes a colleague from any of their computers:
// one this computer runs is changed at once; one another computer runs is
// asked of it through the firm's record, signed with this computer's key,
// and changes once that computer takes it. Its view as it is now.
AgentView request_colleague_edit(const std::string &agent_id, const FirmColleagueHire &edit);
// On the computer that runs them: each colleague takes the newest change an
// administrator's computer asked for that it has not taken yet - from one of
// the firm's computers checked here, of a seat Legal Machines lists as the
// administrator's, its signature verified, made no later than five minutes
// ahead of this computer's clock - checked as the administrator's own change
// here is. Never throws.
void take_colleague_edits();
// Archived, it takes no more work and stands apart from the team, its chats
// and its record kept; restored, it is at work again. The administrator's,
// on the computer that runs it.
AgentView archive_colleague(const std::string &agent_id, bool archived);
// Let go: it is told to the Matters it worked in and to the firm's record,
// and its own record here goes; its words stay where it wrote them.
void delete_colleague(const std::string &agent_id);

// Each colleague of the firm at work here whose own chat on the member's desk
// is not open yet has it opened: one hired before this computer had a desk -
// in onboarding, before the workspace was set - would otherwise stay out of
// Messages until it first worked. Never with a lock held.
void open_missing_desk_threads();
// The firm's colleagues another computer runs, held here from the firm's
// record: each one this computer does not run is kept here as a copy that
// answers the member here in its own chat with them, its settings following the
// record, and stops when the record no longer holds it active; one this
// computer runs is never touched. Its chat on the desk opens as a hired one's
// does. Never with a lock held; never throws.
void adopt_firm_colleagues();
// An agent another AI app proposes (Agent/AiAppConnections): a draft that
// does nothing - no folder, no schedule, no run - until the lawyer approves
// it with their own settings. Its words are checked as the lawyer's are.
AgentView draft_agent_for_app(const AgentSettings &settings, const AgentRequester &requester);
// A draft another AI app proposed becomes an agent only with the lawyer's own
// settings: the folder, schedule and model they chose here, never the app's.
AgentView approve_draft(const std::string &agent_id, const AgentSettings &settings, const std::string &actor_id);
AgentView decline_draft(const std::string &agent_id);
AgentView pause_agent(const std::string &agent_id);
// Sets the model the colleague thinks with: a provider's model, checked by the
// caller against what is connected, or both empty for the firm's default. Its
// work from then on runs on it.
AgentView set_agent_model(const std::string &agent_id, const std::string &provider_id, const std::string &model_id);
// Sets what gathers the colleague's memory: a provider's or this computer's
// model, checked by the caller against what is connected; both empty for the
// firm's model for context work; `off` for none, after which it keeps no new
// notes and its notes stay as they are. The firm's administrator chooses it,
// as the colleague's model.
AgentView set_agent_memory_model(const std::string &agent_id, const std::string &provider_id, const std::string &model_id, bool off);
AgentView resume_agent(const std::string &agent_id);
AgentView stop_agent(const std::string &agent_id);
AgentView update_agent(const std::string &agent_id, long long expected_revision, const AgentSettings &settings);
// The colleague's office in the firm: the office it holds, the position it
// ranks with - one of the firm's scale (Organisation/Types/FirmOffice), or
// none - its practice, its area and whom it answers to. Its words are checked
// as its settings are; its thread's brief and its card then say who it is now.
AgentView set_agent_office(const std::string &agent_id, const Agent::Office &office);
AgentView request_agent_run(const std::string &agent_id);
AgentView cancel_agent_run(const std::string &agent_id);
AgentView mark_run_reviewed(const std::string &agent_id, const std::string &run_id, const std::string &actor_id);

// Work a lawyer gives an agent in their own words (AgentAssignment): kept,
// put in line after the work given before it, and begun once the agent is
// free. An agent at work takes work; a paused one keeps it until it is
// resumed; a stopped or unapproved one takes none.
AgentAssignment assign_work(const std::string &agent_id, const std::string &instruction, const std::string &actor_id);
// Work given by naming the agent (@) in a message of one of its Matter's
// threads: the agent answers there, after the work given before it. A message
// already taken up is never taken up twice - whichever computer reads it.
// `posted_here` only for a message the person at this computer just posted in
// this window (AgentBridge chat.post): a message read from the Matter's shared
// folder counts toward the agent's own day, whoever it says it is from.
// A message's words as the colleague it names reads them: without its own
// @name, which says only whom the message is to (the mention is kept with the
// message), and without what that leaves at the start ("@Kitsune, how are
// you?" is "how are you?"). Words that are nothing but the name stay as they
// are.
std::string words_to_colleague(const std::string &text, const std::string &colleague_name);

AgentAssignment assign_mention(const std::string &agent_id, const std::string &matter_id, const std::string &message_text,
	const std::string &chat_id,
	const std::string &message_id, const std::string &author_actor_id, const std::string &author_name,
	bool posted_here = false);
// Work given in one of the agent's Matter's threads without a message of its
// own - the lawyer's "Try again" on work in that thread (DocumentReview's
// retry): the agent answers there, by the person at this computer.
AgentAssignment assign_in_thread(const std::string &agent_id, const std::string &matter_id, const std::string &instruction,
	const std::string &chat_id,
	const std::string &actor_id);
// A person's message in one of a Matter's threads that names agents (@), as
// the thread keeps it (MatterTask "member_message").
struct MentionMessage {
	std::string matter_id;
	std::string chat_id;
	std::string message_id;
	std::string text;
	std::string author_actor_id;
	std::string author_name;
	std::vector<std::string> mentions;
	// Posted in this window by the person at this computer - never true for a
	// message read from the Matter's shared folder (see assign_mention).
	bool posted_here = false;
};

// Every colleague of this computer the message names takes it up as work and
// answers in its thread, in whichever Matter of the firm it was written: a
// colleague is the firm's, never one Matter's; the ids of those that did. A
// message already taken up is not taken up again, so a thread read again -
// or synced from a colleague's computer - never sets an agent to work twice.
std::vector<std::string> take_up_mentions(const MentionMessage &message);

// A lead's work given back. `ended` is work another colleague of this
// computer gave in a thread as a part of its own (work.give), now done or
// failed. Once no part that colleague gave in the thread still waits or
// runs, it is given its work there once more - to read what came of the
// parts and finish what it was asked - as work it gives itself, which is
// never followed up in turn, so a round of parts ends with it. Nothing for
// work a person gave, work stopped or withdrawn, a giver that is no
// colleague here, or while the giver's work given back in the thread has not
// ended. The work given back, if any.
std::optional<AgentAssignment> give_back_after_part(const AgentAssignment &ended);

// Why `agent_id` cannot take a part another colleague gives it on this
// computer, in words, or nothing when it can (finding f-silent-refusal: a part
// it could not take was posted to it, became no one's work, and nothing said
// so). It cannot while no colleague works on this computer, when another of
// the firm's computers carries it, or while it is stopped or not yet approved.
std::optional<std::string> why_no_part(const std::string &agent_id);

// --- A colleague's memory ------------------------------------------
//
// One memory per colleague, across every conversation and every piece of work
// it has in the firm, gathered after each talk by a model of its own - the one
// its page chose, else the firm's model for context work, or none when its
// page turned it off - and read in every talk from the cache. What a note is
// about decides where it is used: the firm's everywhere, a Matter's where that
// Matter's members are, a member's when that member takes part.

// Where a colleague talks: the conversation's project and thread, the message
// it answers, the Matter it is in - none in a member's personal chat - and the
// member it talks with. In a personal chat, the Matters that member may see,
// by id and name: their notes it may use with them.
struct MemoryPlace {
	std::string project_id;
	std::string chat_id;
	std::string message_id;
	std::string matter_id;
	std::string matter_name;
	std::string person_actor_id;
	std::string person_name;
	std::map<std::string, std::string> member_matters;
};

// What it remembers that it may use here, as its talk reads it: its notes
// numbered M1, M2..., and which note each number is.
struct MemoryDigest {
	std::string text;
	std::vector<std::string> note_ids;
};
MemoryDigest memory_digest(const std::string &agent_id, const MemoryPlace &place);

// A note a talk kept: its kind, what it is about ("firm", "matter", "person")
// and its words.
struct KeptMemory {
	std::string kind;
	std::string about;
	std::string text;
};
// Keeps what a talk settled and lets go the notes it forgot (by id), as this
// computer's own notes; past a budget the oldest of the least lasting kind go.
void keep_memory(const std::string &agent_id, const MemoryPlace &place, const std::vector<KeptMemory> &kept,
	const std::vector<std::string> &forgotten_note_ids);
// Every note it holds now, oldest first.
std::vector<MemoryNote> memory_of(const std::string &agent_id);
// A member lets one of its notes go.
void forget_memory(const std::string &agent_id, const std::string &note_id);

// Opens a colleague's own chat - in the Matter it was made for, or on the
// member's desk - where it has none yet, and brings its brief up to date where
// it has one. Never throws; never call it with an agent lock held.
void open_own_chat(const std::string &agent_id);
// The colleagues whose own chat could not open at their last try - no Matters
// folder yet, as for onboarding's first colleague - which the scheduler tries
// again at each wake until it opens.
std::vector<std::string> colleagues_awaiting_chat();

// Takes back work not yet begun. Work begun is ended as a run is, by stopping
// the run.
AgentAssignment withdraw_assignment(const std::string &agent_id, const std::string &assignment_id);
// What the agent was given: the work it is doing, then what waits in its
// order, then the rest, newest first - at most `most_finished` of those.
std::vector<AgentAssignment> list_assignments(const std::string &agent_id, std::size_t most_finished);

// Whether work given to the agent in this thread waits in its line, not yet
// begun. Begun, it takes up the agent's work there that waits on the lawyer,
// with every answer given by then. False for an agent not on this computer.
[[nodiscard]] bool has_work_in_line(const std::string &agent_id, const std::string &chat_id);

// A member's Stop in a conversation: every colleague on this computer ends
// the work it was given there - the piece in hand stopped where it stands,
// what waits withdrawn - and stays at work, waiting for what it is given
// next. Its work from anywhere else goes on. How many pieces ended.
std::size_t stop_work_in_chat(const std::string &chat_id);
// A thread deleted (Chat/ChatActions set_deleted_listener): the work given in
// it ends as a Stop ends it, the waiting work saying its thread was deleted.
std::size_t withdraw_work_in_deleted_chat(const std::string &chat_id);

// The conversations a colleague has work from now - the piece in hand, then
// the pieces waiting - each once, so the conversation's Stop stands while it
// works on what was given there.
std::vector<std::string> chats_with_work(const Agent &agent, const std::optional<AgentRun> &latest_run, bool is_running);

std::vector<AgentView> list_agent_views(const std::optional<std::string> &matter_id);
AgentView agent_view(const std::string &agent_id);

// Writes the agent's card into its Matter for the rest of its team
// (Agent/AgentCard), as the agent is now. Never throws.
void publish_card(const std::string &agent_id);

// The state that sums an agent and its latest run (AgentView::state).
std::string agent_state(const Agent &agent, const std::optional<AgentRun> &latest_run, bool is_running);

// What an agent's character acts out, derived here and nowhere else from its
// state (agent_state), whether its run is under way now and that run's stage,
// so a page never stages work that is not happening.
// - greet: a draft waits for a lawyer's approval;
// - rest, while it answers a message (stage answering): its conversation says
//   it is typing, and its face acts out nothing;
// - think: a model works on its run (stage analysing);
// - work: it lists, reads, compares or writes on this computer;
// - wait: its work is begun, and another agent's run goes first;
// - explain: there is something of its to review;
// - listen: it cannot go on (access needed, or its run failed);
// - rest: otherwise.
// An agent another computer runs is known here only by its state: running
// there reads as work.
std::string agent_presence(const std::string &state, bool is_running, const std::string &run_stage);

// The settings a lawyer gave, checked: words within their limits, a daily
// time within the day, and a folder that exists inside the Matter's own
// folder - returned as its real path. Throws std::invalid_argument with the
// sentence the lawyer reads.
AgentSettings checked_settings(const AgentSettings &settings);

} // namespace singularity::agents
