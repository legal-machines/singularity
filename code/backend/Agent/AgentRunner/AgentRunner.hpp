#pragma once

#include "Agent/TextDiff/TextDiff.hpp"
#include "Agent/Types/Agent.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::agents {

// The Matter an agent works for, as the runner needs it: where its folder is
// on this computer, and which of its documents Singularity already knows, by
// path relative to that folder, so a report can open them.
// A version of one of the Matter's documents that Singularity wrote in an
// operation an agent ran: a change the agent prepared, approved by the lawyer
// and written. Its path is relative to the Matter's folder.
struct AgentWrittenVersion {
	std::string relative_path;
	std::string fingerprint;
	std::string agent_id;
};

struct MatterFolder {
	std::string name;
	std::filesystem::path folder;
	std::map<std::string, std::string> document_ids;
	// What agents' own approved changes wrote: a task agent's own work, done,
	// never wakes it as if it were new.
	std::vector<AgentWrittenVersion> written_by_agents;
};

// The words of one version of one file, paragraph by paragraph, or why there
// are none.
struct ExtractedText {
	bool is_readable = false;
	std::string reason;
	std::vector<TextBlock> blocks;
};

// One bounded analysis of the evidence a run computed, on the model the
// agent's policy allows, under the protection that model's destination
// requires. The model is given no tools: it can only answer.
struct AnalysisInput {
	std::string agent_id;
	std::string run_id;
	std::string matter_id;
	std::string agent_name;
	AgentModelPolicy policy = AgentModelPolicy::MatterSetup;
	std::string prompt;
	nlohmann::json contract;
	// Set when the lawyer stops the check: the analysis abandons its call.
	const std::atomic<bool> *cancelled = nullptr;
};

struct AnalysisOutput {
	// done | withheld | failed | cancelled
	std::string status = "failed";
	std::string reason;
	// For an analysis withheld or failed: whether the next check analyses the
	// same changes again. True for a cause that passes or that the lawyer can
	// put right (no model yet, protection not ready, the provider unreachable,
	// busy or refusing the key or the model); false when the same words would
	// meet the same refusal (protection could not hide them, or they are more
	// than the model reads at once), so they are listed for the lawyer instead.
	bool analyses_again = true;
	std::string text;
	std::string provider_id;
	std::string model_id;
	std::string model_label;
	// local | protected | unprotected
	std::string destination;
	std::string protection_mode;
	std::optional<std::string> operation_id;
	long long input_tokens = 0;
	long long output_tokens = 0;
	std::optional<double> cost;
	std::string currency;
};

// One run of a task agent: the lawyer's instruction carried out as a request
// in the agent's own chat. `change_note` says which documents changed since
// the last run, by name only - their words reach the model as the request's
// sources, never as instructions.
struct TaskRunInput {
	std::string agent_id;
	std::string run_id;
	std::string matter_id;
	std::string agent_name;
	std::string instruction;
	AgentModelPolicy policy = AgentModelPolicy::MatterSetup;
	std::optional<std::string> chat_id;
	std::string change_note;
	std::vector<std::string> changed_document_ids;
	// The files of its folder this run found new, changed or moved, whether or
	// not the Matter knows them yet: a task that reads the Matter's documents
	// brings these up to date first.
	std::vector<std::filesystem::path> changed_files;
	bool is_first_run = false;
	// Why it runs now, when another agent's report woke it: that agent's name.
	std::string woken_by_agent;
	// Work a lawyer gave the agent once (AgentAssignment), when this run
	// carries it out: `instruction` is then that work, and `role` the agent's
	// standing instruction, the role it does the work in.
	bool is_assignment = false;
	std::string role;
	// A thread where someone named the agent (@): it answers there, not in its
	// own thread; and who named it.
	std::string reply_chat_id;
	std::string given_by;
	std::string given_by_actor_id;
	std::string reply_to_message_id;
	const std::atomic<bool> *cancelled = nullptr;
	// Who the colleague is in the firm, as its talk carries it
	// (Agent/ColleagueBrief colleague_identity), and the member who wrote to it:
	// their office and where they stand to it ("who ranks below you"); each
	// empty where the firm does not say.
	std::string identity;
	std::string asker_office;
	std::string asker_standing;
	// Told when the colleague takes work on after answering in words, or goes
	// straight to work: the run is at work from then on, no longer answering.
	std::function<void()> work_begins;
	// Told the moment its answer in words is posted: it is neither typing nor at
	// work any more while its run ends.
	std::function<void()> answer_given;
};

struct TaskRunOutput {
	// completed | failed | cancelled
	std::string status = "failed";
	std::string reason;
	RunTask task;
	std::optional<std::string> work_product_id;
	// The agent's chat, when this run made it.
	std::optional<std::string> made_chat_id;
};

struct RunnerDependencies {
	std::function<std::optional<MatterFolder>(const std::string &matter_id)> matter_folder;
	std::function<ExtractedText(const std::filesystem::path &path)> extract;
	std::function<AnalysisOutput(const AnalysisInput &input)> analyse;
	std::function<TaskRunOutput(const TaskRunInput &input)> run_task;
	// Keeps the report in the Matter as Work Product; its id, or nothing when
	// it could not be kept (the run keeps the report either way).
	std::function<std::optional<std::string>(const Agent &agent, const AgentRun &run)> write_report;
	// Who the lawyer is on this computer, for nothing but attribution.
	std::function<std::string()> actor_id;
	// Their name as their own Settings or account give it: what their team
	// reads beside the agents they are answerable for.
	std::function<std::string()> person_name;
	// This computer's device id: the computer that runs the agents it holds.
	std::function<std::string()> device_id;
	// This computer's device key (Ed25519, base64, Platform/DeviceKey) and its
	// signature: a new agent's id is bound to the key, and its card is signed
	// with it, so no other computer can speak for it (D-128).
	std::function<std::string()> device_key;
	std::function<std::string(const std::string &message)> sign;
	// The agent's thread (MatterChat::agent_id), the one conversation the
	// lawyer has with it: made, given the agent's brief and introduced by the
	// agent when it has none; its brief brought up to date when it has one.
	// Its chat id, or "" when the Matter is not open here.
	std::function<std::string(const Agent &agent)> open_thread;
	// After a run has ended, the agent tells its thread, in its own words,
	// what the run found; a task's answer is in the thread already.
	std::function<void(const Agent &agent, const AgentRun &run)> report_run;
	// Messages in the Matters of this computer's agents that name one of them
	// (@) - written here or on a colleague's computer and synced through the
	// Matter's folder - taken up as work, once each. Called each time the
	// scheduler wakes; it looks only where a Matter's operations changed.
	std::function<void()> take_up_mentions;
	// A colleague's word in its thread that work just given waits behind work
	// it is doing: posted at once, a reply to the request, so the member who
	// asked is never left without an answer while it waits (2026-10-29: a
	// request to Unicorn sat unanswered through a deck run of half an hour).
	// The message's id, or "" when it could not be posted.
	std::function<std::string(const Agent &agent, const AgentAssignment &work, const std::string &words)> say_waiting;
	// The member's desk on this computer: the project, never listed or shared
	// as a Matter, that keeps their personal chats with the firm's colleagues
	// (D-042, D-157). Made the first time it is asked for; its id, or "" when it
	// cannot be made. A colleague of the firm keeps its own chat there.
	std::function<std::string()> desk_project;
	// The Matters this computer carries with the firm's other computers
	// (Matter/MatterCoordination). A colleague of the firm may be named in any
	// of them (D-157), so its card goes into each, and every computer that
	// carries one lists it beside the people (PHIL-10).
	std::function<std::vector<std::string>()> carried_matters;
	// Whether the member at this computer may hire the firm's AI colleagues and
	// choose their models: the firm's administrator (D-184), as the application
	// installs it from the role its account flow recorded (Organisation/
	// FirmRole). With none installed, anyone may: a test, a tool with no firm.
	std::function<bool()> may_manage_colleagues;
	// An administrator's change of a colleague made on another of the firm's
	// computers (D-219), checked before the computer that runs the colleague
	// takes it: the device key of `device_id` when it is one of the firm's
	// computers checked here and its seat is the administrator's by Legal
	// Machines' own list of the firm's seats - never by what a file in the
	// firm's record says of itself; none otherwise.
	std::function<std::optional<std::string>(const std::string &device_id)> administrator_computer_key;
	std::function<bool(const std::string &key, const std::string &message, const std::string &signature)> verify;
	// The seat signed in here, or "".
	std::function<std::string()> seat_id;
};

// Where a colleague's own chat is kept: the Matter it was made for, in the zero
// state, or the member's desk for a colleague of the firm (D-157). Never call it
// with an agent lock held: the desk may have to be made.
inline std::string home_project_of(const Agent &agent, const RunnerDependencies &dependencies) {
	if (agent.matter_id.empty() == false) return agent.matter_id;
	return dependencies.desk_project ? dependencies.desk_project() : std::string();
}

// The analysis contract: findings that cite evidence by id, nothing else.
nlohmann::json analysis_contract();

// Which documents changed since the last run, by name only.
std::string change_note(const AgentRun &run);

// The prompt a run sends: the lawyer's request, the evidence by id and the
// rules for answering. Document text is data: its own delimiters are
// neutralised so a document cannot close its evidence block and speak.
std::string analysis_prompt(const Agent &agent, const std::vector<RunEvidence> &evidence);

// The findings an answer gives, kept only where every piece of evidence a
// finding cites was given; `dropped` counts the rest.
std::vector<RunFinding> findings_from_answer(const std::string &text, const std::vector<RunEvidence> &evidence,
	std::size_t &dropped);

// Paragraphs of extracted text, each with where a person finds it.
std::vector<TextBlock> blocks_from_text(const std::string &text, const std::vector<std::pair<std::size_t, std::size_t>> &page_offsets);

// The report a run keeps in the Matter, as Markdown.
std::string report_markdown(const Agent &agent, const AgentRun &run);
std::string report_title(const AgentRun &run);

// One check of one agent, from listing the folder to keeping the report. It
// persists the run as it goes, so its state can be read while it works, and
// ends every expected failure - a folder gone or forbidden, a document that
// cannot be read, a model that cannot be reached - in a state the lawyer
// reads, never an exception. Never writes into the watched folder.
AgentRun run_agent_check(const Agent &agent, const std::string &run_id, const std::string &trigger,
	const RunnerDependencies &dependencies, const std::atomic<bool> *cancelled);

// One piece of work a lawyer gave the agent (AgentAssignment), carried out in
// its own chat as a task run is: the work is the request, the agent's
// standing instruction the role it works in. It reads and records no folder,
// so the agent's own checks compare as they did before. Ends every expected
// failure in a state the lawyer reads, never an exception.
AgentRun run_agent_assignment(const Agent &agent, const AgentAssignment &assignment, const std::string &run_id,
	const RunnerDependencies &dependencies, const std::atomic<bool> *cancelled);

// Whether `child` is `parent` or lies inside it, folder by folder and with
// links resolved: "Contracts Archive" is not inside "Contracts".
bool is_inside_folder(const std::filesystem::path &child, const std::filesystem::path &parent);

// Whether a run leaves something for the lawyer to look at: anything changed
// in the room, or anything that could not be read or analysed.
bool run_needs_review(const AgentRun &run);

} // namespace singularity::agents
