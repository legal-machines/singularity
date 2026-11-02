#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::agents {

// A command refused because of what the agent or the lawyer's settings are,
// in the words the lawyer reads (Bridge/NativeBridge/NativeError.hpp says it
// as it is written).
class AgentRefused final : public std::invalid_argument {
public:
	explicit AgentRefused(const std::string &words) : std::invalid_argument(words) {}
};

// An agent is standing work Singularity carries out on its own, on this
// computer, for one Matter: any task the lawyer gives it in their own words,
// at the times they choose. It is device state - it runs where it was
// approved, reads a folder by this computer's path - so it is kept in this
// computer's application data, never in the Matter folder a colleague may
// share (Agent/AgentStore). What a run produces for the lawyer to keep is
// written into the Matter as a Work Product.
//
// Two ways of working, one agent:
// - Changes: each check compares the folder with the last one and gives the
//   changed words, quoted, to one bounded analysis that answers the lawyer's
//   instruction about them (a data room watch, deadlines in new documents).
// - Task: each run carries out the lawyer's instruction as a request in the
//   agent's own chat, through the same path every chat request takes, with
//   the Matter's documents as its sources (a weekly summary, a status note,
//   anything a chat can do). It may read and propose; every change it would
//   make to the Matter waits for the lawyer in that chat.
inline constexpr int kAgentFormatVersion = 1;
inline constexpr const char *kAgentKindDataRoomWatch = "data_room_watch";

enum class AgentMode {
	Changes,
	Task,
};

enum class AgentStatus {
	// Proposed by another AI app and not yet approved: it never runs.
	Draft,
	Active,
	Paused,
	// Ended by the lawyer: it never runs again; its reports stay.
	Stopped,
};

enum class AgentTrigger {
	// Only when the lawyer runs it.
	Manual,
	// When the folder changes, while Singularity is open.
	OnChange,
	// Once a day at a time of the lawyer's choosing, while Singularity is
	// open; a check missed while it was closed is made when it opens.
	Daily,
	// Once a week, on a day and at a time of the lawyer's choosing, likewise.
	Weekly,
	// When another agent of the same Matter reports something to review: a
	// task agent working on what that one found.
	AfterAgent,
};

enum class AgentModelPolicy {
	// The model the Matter's AI Setup gives its reasoning, with the protection
	// that model's destination requires.
	MatterSetup,
	// Only a model running on this computer; nothing leaves it.
	LocalOnly,
};

// Who asked for an agent through another AI app, when one did. Every field
// is what Singularity itself observed; `client_name` is only what the app
// called itself and is shown as such.
struct AgentRequester {
	std::string connection_id;
	std::string connection_label;
	std::string client_name;
	// The requesting program's verified code signature ("Anthropic PBC"),
	// empty when it could not be verified.
	std::string signer;
	bool is_signer_verified = false;
	std::string request_id;
	std::string requested_at;
	// What the app asked for, kept as it asked it: shown to the lawyer beside
	// what they approve, never applied without them.
	std::string requested_name;
	std::string requested_goal;
	std::string requested_folder;
	std::string requested_trigger;
};

struct Agent {
	int format_version = kAgentFormatVersion;
	std::string id;
	// The nonce its id was bound with to the key of the computer that made it
	// (Agent/Types/AgentIdentity, D-128); empty for an older agent, whose id
	// is random.
	std::string id_nonce;
	std::string matter_id;
	// The template the agent was begun from (data_room_watch, deadlines,
	// new_documents, matter_summary, status_update, playbook_review, custom):
	// its sign and its first words only; what it does is its mode, its Playbook
	// and the lawyer's instruction.
	std::string kind = kAgentKindDataRoomWatch;
	AgentMode mode = AgentMode::Changes;
	std::string name;
	// The character the agent is drawn as, wherever it appears - a living mark
	// of its own, as a person has a face (kAgentCharacters). Chosen by the
	// lawyer, or given from its id so no two agents look alike by default.
	std::string character;
	// The lawyer's instruction, in their words: what to look for, or the task.
	std::string goal;
	// Its professional habits, in a sentence: what makes it this colleague and
	// not another (Agent/ColleagueBrief, D-101). Empty: none of its own.
	std::string manner;
	// Its office in the firm, as a person's place in the firm is given (ORG-1,
	// D-071's second step): the office it holds ("Practice Group Leader"), the
	// position of the firm's scale it ranks with ("Partner",
	// Organisation/Types/FirmOffice), its practice, its area, and the member it
	// answers to. Of the whole firm, never of one Matter (the owner,
	// 2026-10-26). Empty for a colleague not given one: it is then known by its
	// standing work.
	struct Office {
		std::string title;
		std::string ranks_with;
		std::string practice;
		std::string area;
		std::string answers_to;
	} office;
	// The watched folder: the Matter's folder or one inside it, as an absolute
	// path on this computer. Empty for a draft not yet given one.
	std::string source_root;
	AgentTrigger trigger = AgentTrigger::OnChange;
	// Minutes after local midnight for a daily or weekly check.
	int daily_minute = 9 * 60;
	// The day of a weekly check: 0 Sunday to 6 Saturday.
	int weekly_day = 1;
	// The agent's own chat, where a task agent's runs are answered; made at
	// its first task run.
	std::optional<std::string> chat_id;
	// The agent whose reports it follows (AgentTrigger::AfterAgent).
	std::string follows_agent_id;
	// The firm's Playbook it checks the Matter's documents against
	// (WorkProduct/Playbook): its task is then a Review Table of the documents
	// against the Playbook's positions, and the tracked changes that bring the
	// departing ones to the firm's position, each waiting for the lawyer. The
	// table it keeps doing that in, once made.
	std::string playbook_id;
	std::string review_table_id;
	// The work lawyers gave it that it has not begun, in the order given
	// (AgentAssignment ids). What each piece is, and what became of it, is
	// kept beside its runs.
	std::vector<std::string> waiting_work;
	// How many pieces of work it was ever given: the next one's number.
	int work_given = 0;
	AgentModelPolicy model_policy = AgentModelPolicy::MatterSetup;
	// The model it thinks with, as its page sets it (D-185): a connected
	// provider's model, or both empty for the firm's default (the Matter's AI
	// Setup).
	std::string model_provider_id;
	std::string model_id;
	// What gathers its memory (D-206): a model chosen on its page, a connected
	// provider's or one on this computer; both empty for the firm's model for
	// context work (AI Setup's Context role), an efficient one. Off, it keeps no
	// new notes, and its notes stay as they are.
	std::string memory_provider_id;
	std::string memory_model_id;
	bool is_memory_off = false;
	AgentStatus status = AgentStatus::Active;
	// A colleague of the firm another computer runs, held here from the firm's
	// record (D-213): it answers the member here in its own chat with them on
	// this computer, and takes up no Matter's work - the computer that runs it
	// does. What it is follows the record; nothing of it is written back.
	bool is_firm_copy = false;
	long long revision = 1;
	std::string created_at;
	std::string updated_at;
	// The lawyer who approved it on this computer: an actor id, never a name
	// taken from anywhere else.
	std::string approved_by_actor_id;
	std::string approved_at;
	// The lawyer answerable for its work, as their own Settings or account
	// name them: who approved it, unless they hand it on. Its team sees this
	// name beside the agent (Agent/AgentCard).
	std::string responsible_actor_id;
	std::string responsible_name;
	std::optional<AgentRequester> requested_by;
	std::optional<std::string> last_run_id;
	// When the folder was last compared in full.
	std::optional<std::string> last_check_at;
	// When the agent last began a run, whatever became of it: a run that did
	// not finish is tried again only after a while, never at every wake.
	std::optional<std::string> last_attempt_at;
	// When the next scheduled check is due, so one missed while Singularity
	// was closed is made when it opens, once.
	std::optional<std::string> next_check_at;
	// When a change in the folder was noticed and not yet checked.
	std::optional<std::string> change_noticed_at;
	std::size_t most_files = 5000;
	// The most characters of evidence one run may send for analysis.
	std::size_t most_evidence_characters = 48000;
	// Its own runs that could not finish, one after another: how long it waits
	// before trying again (kAgentLongestRetryHours); and the runs it made on its
	// own today, counted for its record, never a limit (D-158).
	int failed_in_a_row = 0;
	// Why the agent paused itself; empty when the lawyer paused it, or it runs.
	std::string paused_reason;
	std::string automatic_runs_day;
	int automatic_runs = 0;
	// What its analyses cost this month ("2026-10"), as the providers billed
	// them; a task run's cost is its chat request's, counted in Usage.
	std::string spend_month;
	double spend = 0.0;
	std::string spend_currency;
};

// The characters an agent can be drawn as. The page owns how each looks and
// moves; the backend keeps which one an agent is.
inline constexpr const char *kAgentCharacters[] = {"ember", "moss", "slate", "tide", "dune", "plum", "ink", "sun"};
// The Agent Family's species (D-063), which the new client draws an AI
// colleague as and a firm hires one as: kept as its character too.
inline constexpr const char *kAgentSpecies[] = {"kitsune", "unicorn", "phoenix", "griffin", "pegasus", "dragon"};

// A colleague's own run that could not finish is tried again after a wait that
// doubles with each one in a row - half an hour, an hour, two - up to a day. It
// never stops of its own accord and no person has to start it again: a member
// is not paused for a cause outside them, and neither is an AI colleague (the
// owner, 2026-10-30; D-158).
inline constexpr int kAgentLongestRetryHours = 24;

// One quoted paragraph and where it stands.
struct EvidenceQuote {
	std::string text;
	std::string location;
};

// What a run shows as evidence: the words of one change in one document,
// computed on this computer. Analysis may only cite these, by id.
struct RunEvidence {
	std::string id;
	std::string relative_path;
	// "changed": `before` left and `after` came; "added": `after` is the new
	// document's opening.
	std::string kind;
	std::vector<EvidenceQuote> context;
	std::vector<EvidenceQuote> before;
	std::vector<EvidenceQuote> after;
	std::size_t omitted_before = 0;
	std::size_t omitted_after = 0;
};

// One file of the data room that a run reports on.
struct RunDocument {
	std::string relative_path;
	// added | changed | removed | moved | unreadable
	std::string kind;
	std::string previous_path;
	std::string previous_sha256;
	std::string current_sha256;
	std::optional<std::string> document_id;
	// compared | read | unreviewed | not_applicable
	std::string review = "not_applicable";
	// Why a document was not reviewed, in words.
	std::string note;
	std::vector<std::string> evidence_ids;
};

// One point the analysis raised, kept only when every piece of evidence it
// cites was given to it.
struct RunFinding {
	std::string id;
	std::string title;
	// What the cited evidence shows.
	std::string observation;
	// What it may mean for the deal: analysis, not a conclusion.
	std::string analysis;
	std::string uncertainty;
	bool needs_review = true;
	std::vector<std::string> evidence_ids;
};

struct RunAnalysis {
	// done | not_needed | withheld | failed | cancelled
	std::string status = "not_needed";
	// Why it was not done, in words a lawyer reads.
	std::string reason;
	std::string provider_id;
	std::string model_id;
	std::string model_label;
	// local | protected | unprotected
	std::string destination;
	std::string protection_mode;
	std::size_t evidence_sent = 0;
	std::size_t findings_dropped = 0;
	std::optional<std::string> operation_id;
	long long input_tokens = 0;
	long long output_tokens = 0;
	std::optional<double> cost;
	std::string currency;
};

// A task run: the request it made in the agent's chat and what became of it.
struct RunTask {
	std::string chat_id;
	std::string task_id;
	std::string operation_id;
	// completed | waiting_for_approval | failed | stopped | skipped
	std::string state;
	// The answer's opening words, as the run shows them.
	std::string answer_excerpt;
	std::string message;
	// What its request cost, as the provider billed it; none when it did not
	// say.
	std::optional<double> cost;
	std::string currency;
};

struct RunCounts {
	std::size_t files = 0;
	std::size_t added = 0;
	std::size_t changed = 0;
	std::size_t removed = 0;
	std::size_t moved = 0;
	std::size_t unreadable = 0;
	std::size_t unreviewed = 0;
};

struct AgentRun {
	std::string id;
	std::string agent_id;
	std::string matter_id;
	// manual | change | schedule | catch_up | first | safety | after_agent |
	// assignment
	std::string trigger;
	// The work a lawyer gave the agent that this run carries out
	// (AgentAssignment), when it is one.
	std::string assignment_id;
	// running | completed | failed | cancelled | access_needed | interrupted
	std::string status = "running";
	// listing | reading | comparing | answering | analysing | writing | answered
	// | done.
	// answering: it writes its answer to a message (its talk, D-129), in the
	// chat `answering_chat_id` names; it is at work only once it takes work on.
	// answered: its answer is out and its run is ending - neither typing nor at
	// work (D-130).
	std::string stage = "listing";
	std::string answering_chat_id;
	// Measured while documents are read; zero when not measured.
	std::size_t progress_done = 0;
	std::size_t progress_total = 0;
	std::string started_at;
	std::string finished_at;
	// The first check: it records the room and compares nothing.
	bool is_baseline = false;
	RunCounts counts;
	std::vector<RunDocument> documents;
	std::vector<RunEvidence> evidence;
	std::vector<RunFinding> findings;
	RunAnalysis analysis;
	std::optional<RunTask> task;
	std::optional<std::string> work_product_id;
	std::optional<std::string> reviewed_at;
	std::optional<std::string> reviewed_by_actor_id;
	std::string error_code;
	std::string error_message;
};

// Work a lawyer gives an agent in their own words, once, as they would hand a
// colleague a piece of work. The agent carries out what it is given in the
// order it was given, one piece at a time, in its own chat, as a task run
// does: the Matter's documents are its sources, the Matter's protection
// applies, and every change it would make waits for the lawyer there. Its
// standing instruction is the role it works in. Kept beside its runs, on the
// computer that runs it.
struct AgentAssignment {
	int format_version = 1;
	std::string id;
	std::string agent_id;
	std::string matter_id;
	// The agent's first piece of work is 1, its next 2: the order work was
	// given in, to the moment.
	int number = 0;
	// Where it answers: a thread of the Matter where someone named the agent
	// (@), or its own thread when empty; and the message it answers.
	std::string chat_id;
	std::string reply_to_message_id;
	std::string instruction;
	// Who gave it, as Singularity knows them on this computer.
	std::string given_by_actor_id;
	std::string given_by_name;
	std::string given_at;
	// It reached this computer through the Matter's shared folder - not from a
	// message posted in this window. It is taken up only as its author's, signed
	// by a computer the person here confirmed (Chat/MessageProvenance), and is
	// work like any other (D-158).
	bool from_shared_folder = false;
	// waiting | working | done | failed | stopped | withdrawn
	std::string state = "waiting";
	// The run that carries it out, once one began.
	std::string run_id;
	std::string started_at;
	std::string finished_at;
	// Runs begun for it: work Singularity closed on is begun again once.
	int attempts = 0;
	// Why it was not done, in words.
	std::string message;
};

// The most characters of one piece of work: as many as any member's message
// holds (Chat/ChatActions). A colleague takes all the work it is given, in
// the order given, as a person does (D-158).
inline constexpr std::size_t kAgentMostAssignmentCharacters = 20000;

// What an AI colleague remembers (D-159): its own short notes, one memory
// across every conversation and every piece of work it has in the firm - its
// personal chats and every Matter's threads alike. A note says what kind of
// thing it is, what it is about - the whole firm, one Matter, or one member -
// and where it was learned, so the walls between clients and a member's
// privacy hold by structure (ORG-6): a Matter's notes are used only where that
// Matter's members are, a member's only when that member takes part.
struct MemoryNote {
	std::string id;
	// rule | preference | fact | decision | commitment
	std::string kind = "fact";
	// firm | matter | person
	std::string scope = "firm";
	std::string matter_id;
	std::string matter_name;
	std::string person_actor_id;
	std::string person_name;
	std::string text;
	// Where it was learned: the project and thread, the message it answered,
	// and who wrote that message.
	std::string project_id;
	std::string chat_id;
	std::string message_id;
	std::string said_by_actor_id;
	std::string said_by_name;
	std::string at;
	// The computer that wrote it: each computer writes only its own notes
	// (Agent/AgentStore), so a colleague's notes from every computer merge with
	// no conflict.
	std::string device_id;
	// Set on a note that lets another go: the id of the note it retires.
	std::string retires;
};

// How much a colleague's memory holds, in characters of its notes: of the
// whole firm, of each Matter, of each member. Past a budget the oldest notes
// of the least lasting kind go first - a fact before a commitment, a
// preference, a decision, a rule - so memory stays short enough to be read in
// every conversation, from the cache (D-159).
inline constexpr std::size_t kMemoryFirmCharacters = 2200;
inline constexpr std::size_t kMemoryMatterCharacters = 1500;
inline constexpr std::size_t kMemoryPersonCharacters = 800;
inline constexpr std::size_t kMemoryNoteCharacters = 300;

const char *to_string(AgentMode mode);
std::optional<AgentMode> agent_mode_from_string(const std::string &value);
const char *to_string(AgentStatus status);
const char *to_string(AgentTrigger trigger);
const char *to_string(AgentModelPolicy policy);
std::optional<AgentStatus> agent_status_from_string(const std::string &value);
std::optional<AgentTrigger> agent_trigger_from_string(const std::string &value);
std::optional<AgentModelPolicy> agent_model_policy_from_string(const std::string &value);

} // namespace singularity::agents
