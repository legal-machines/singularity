#include "WorkProduct/PlaybookChanges/PlaybookChanges.hpp"

#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"
#include "WorkProduct/Playbook/Playbook.hpp"
#include "WorkProduct/ReviewTable/ReviewTable.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace singularity::playbook_changes {
namespace {

// Batches of changes being prepared on this computer, live in this process,
// by Matter and table: the lawyer's Stop reaches one through its table
// (request_stop). Never destroyed: a batch may still be ending as the process
// exits.
struct LiveBatch {
	std::atomic<bool> stop{false};
};

std::mutex &batch_mutex() {
	static auto *mutex = new std::mutex();
	return *mutex;
}

std::map<std::string, std::shared_ptr<LiveBatch>> &live_batches() {
	static auto *batches = new std::map<std::string, std::shared_ptr<LiveBatch>>();
	return *batches;
}

std::string batch_key(const std::string &project_id, const std::string &table_id) {
	return project_id + "\n" + table_id;
}

// While changes are prepared, a stop - the caller's (an AI colleague's run
// stopped) or the lawyer's own on the table (request_stop) - reaches the
// request running at that moment, as Stop in its chat does. Its thread asks
// for that stop again until the request has taken it - a stop that arrives
// while a request is still beginning is not lost - and then no more. Ended,
// and joined, when it goes out of scope, however preparation ends.
class StopWatch {
public:
	StopWatch(const std::string &project_id, const std::atomic<bool> *cancelled, const std::atomic<bool> *own)
		: project_id_(project_id)
		, cancelled_(cancelled)
		, own_(own)
	{
		thread_ = std::thread([this]() { watch(); });
	}

	~StopWatch() {
		is_finished_.store(true);
		if (thread_.joinable() == true) thread_.join();
	}

	StopWatch(const StopWatch &) = delete;
	StopWatch &operator=(const StopWatch &) = delete;

	// The request now running, or none.
	void set_running(const std::string &operation_id) {
		const std::lock_guard<std::mutex> lock(mutex_);
		running_ = operation_id;
	}

	bool is_cancelled() const {
		return (cancelled_ != nullptr && cancelled_->load() == true) || (own_ != nullptr && own_->load() == true);
	}

private:
	void watch() {
		while (is_finished_.load() == false) {
			if (is_cancelled() == true) {
				std::string running;
				{
					const std::lock_guard<std::mutex> lock(mutex_);
					running = running_;
				}
				if (running.empty() == false && running != stopped_) {
					try {
						if (chat::request_stop(project_id_, running).is_stop_requested == true) stopped_ = running;
					} catch (const std::exception &) {
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	const std::string project_id_;
	const std::atomic<bool> *cancelled_ = nullptr;
	const std::atomic<bool> *own_ = nullptr;
	std::atomic<bool> is_finished_{false};
	std::mutex mutex_;
	std::string running_;
	// The request whose stop was taken; the watch's own thread alone reads it.
	std::string stopped_;
	std::thread thread_;
};

// The question a change's request put to the lawyer, in a line: its title,
// or else its first prompt.
std::string asked_question(const std::string &project_id, const std::string &operation_id) {
	try {
		const chat::ChatOperationStatus status = chat::operation_status(project_id, operation_id);
		if (!status.asking) return {};
		const nlohmann::json &asking = *status.asking;
		if (asking.contains("title") && asking.at("title").is_string() && !asking.at("title").get<std::string>().empty())
			return asking.at("title").get<std::string>();
		if (asking.contains("questions") && asking.at("questions").is_array() && !asking.at("questions").empty()) {
			const nlohmann::json &first = asking.at("questions").front();
			if (first.contains("prompt") && first.at("prompt").is_string()) return first.at("prompt").get<std::string>();
		}
	} catch (const std::exception &) {
	}
	return {};
}

// The cells of `row` that depart from the firm's position: a term that
// deviates, quoted from the document and found there, or one the document
// lacks.
std::vector<const matters::ReviewCell *> departures(const matters::ReviewTable &table, const matters::ReviewRow &row) {
	std::vector<const matters::ReviewCell *> found;
	for (const matters::ReviewColumn &column : table.columns) {
		if (column.position.empty()) continue;
		for (const matters::ReviewCell &cell : row.cells) {
			if (cell.column_id != column.id) continue;
			const bool deviates = cell.assessment == "deviates" && !cell.evidence.empty() && cell.evidence.front().verified;
			const bool missing = cell.assessment == "missing" || (cell.state == "not_found" && cell.assessment != "conforms");
			if (deviates || missing) found.push_back(&cell);
		}
	}
	return found;
}

const matters::ReviewColumn *column_of(const matters::ReviewTable &table, const std::string &column_id) {
	for (const matters::ReviewColumn &column : table.columns)
		if (column.id == column_id) return &column;
	return nullptr;
}

// The Playbook position a column was made from, as the table's version of the
// Playbook states it, when this computer holds it.
std::optional<playbooks::Position> position_of(const matters::ReviewTable &table, const matters::ReviewColumn &column) {
	if (table.playbook_id.empty() || column.position_id.empty()) return std::nullopt;
	const std::optional<playbooks::Playbook> playbook = playbooks::get(table.playbook_id,
		table.playbook_version > 0 ? std::optional<int>(table.playbook_version) : std::nullopt);
	if (!playbook) return std::nullopt;
	for (const playbooks::Position &position : playbook->positions)
		if (position.id == column.position_id) return position;
	return std::nullopt;
}

} // namespace

std::string change_instruction(const matters::ReviewTable &table, const matters::ReviewRow &row,
		const std::string &document_name) {
	std::string said = "Revise " + document_name + " so it meets the firm's position on each point below. Make every "
		"change in the document itself, as tracked changes, and change nothing else. Use the document's own defined "
		"terms, party names and section numbering.\n";
	int number = 0;
	for (const matters::ReviewCell *cell : departures(table, row)) {
		const matters::ReviewColumn *column = column_of(table, cell->column_id);
		if (column == nullptr) continue;
		const std::optional<playbooks::Position> position = position_of(table, *column);
		said += "\n" + std::to_string(++number) + ". " + column->heading + ". ";
		const bool missing = cell->assessment == "missing" || cell->state == "not_found";
		if (missing)
			said += "The document has no such term.";
		else
			said += "The document now says: \"" + cell->evidence.front().quote + "\".";
		said += "\nThe firm's position:\n" + column->position;
		std::string clause;
		if (position) {
			clause = position->standard_clause;
			if (clause.empty())
				for (const playbooks::Fallback &fallback : position->fallbacks)
					if (!fallback.clause.empty()) {
						clause = fallback.clause;
						break;
					}
		}
		if (!clause.empty()) said += "\nThe firm's clause: \"" + clause + "\"";
		if (missing)
			said += "\nAdd the firm's clause as a new section after the section it most closely belongs with, "
				"numbered to fit the document.";
		else
			said += "\nReplace the quoted words with language that meets the firm's position - the firm's clause where "
				"it fits, else its first acceptable alternative that fits.";
		said += "\n";
	}
	// The change rests on the document and the firm's own positions, so it is
	// made without web search: nothing would be added from it, and a search
	// would wait for an approval nobody looking at this chat is there to give.
	said += "\nThe positions above are the firm's policy, not law: add no legal citation of your own, and work "
		"without web search.";
	// The change is decided here, by whoever prepares it, within the firm's
	// positions: no point is left open for someone else to settle.
	said += "\nDecide every point yourself and do not stop to ask anyone. Where the firm's position offers more than one "
		"acceptable option, choose the one closest to its preferred position that this document can carry. Where the "
		"document is unclear, settle it from its own words and the firm's positions. Say in the change's summary which "
		"option you applied on each point and why, so whoever approves it can check your choice at a glance. Only a "
		"point that truly cannot be changed in this document as it stands is left unchanged, and you say why.";
	return said;
}

std::vector<PreparedChange> prepare(const std::string &project_id, const std::string &table_id,
		const std::vector<std::string> &document_ids, const PreparedBy &by, const std::atomic<bool> *cancelled) {
	const review_table::ReviewTableView view = review_table::get(project_id, table_id);
	if (view.table.playbook_id.empty() && std::none_of(view.table.columns.begin(), view.table.columns.end(),
			[](const matters::ReviewColumn &column) { return !column.position.empty(); }))
		throw std::runtime_error("This table checks no firm position, so there is no change to prepare. Make the table "
			"from a Playbook.");
	if (view.table.run_state == "running")
		throw std::runtime_error("The table is still being answered. Prepare the changes once it finishes.");
	// Nothing new begins once Singularity is closing (Platform/AppLifecycle).
	if (platform::lifecycle::is_closing())
		throw std::runtime_error("Singularity is closing, so no change is prepared now. Prepare the changes again once "
			"Singularity is open.");
	const std::string key = batch_key(project_id, table_id);
	const auto batch = std::make_shared<LiveBatch>();
	{
		const std::lock_guard<std::mutex> lock(batch_mutex());
		if (live_batches().count(key) > 0)
			throw std::runtime_error("The changes for this table are already being prepared.");
		live_batches()[key] = batch;
	}
	struct Unregister {
		std::string key;
		~Unregister() {
			const std::lock_guard<std::mutex> lock(batch_mutex());
			live_batches().erase(key);
		}
	} unregister{key};
	std::vector<PreparedChange> prepared;
	StopWatch stopWatch(project_id, cancelled, &batch->stop);
	for (const matters::ReviewDocument &document : view.table.documents) {
		// Stopped, or Singularity is closing: no other document is begun. The
		// request running when it closed is interrupted on its own (ChatExecution).
		if (stopWatch.is_cancelled() == true || platform::lifecycle::is_closing()) break;
		if (!document_ids.empty()
				&& std::find(document_ids.begin(), document_ids.end(), document.document_id) == document_ids.end())
			continue;
		const auto row = std::find_if(view.rows.begin(), view.rows.end(),
			[&](const matters::ReviewRow &entry) { return entry.document_id == document.document_id; });
		const auto status = std::find_if(view.statuses.begin(), view.statuses.end(),
			[&](const review_table::RowStatus &entry) { return entry.document_id == document.document_id; });
		if (row == view.rows.end() || status == view.statuses.end() || status->status != "current") continue;
		// The change made from this row is approved and written: the document
		// is that change, and another would only repeat it.
		if (status->change_written) continue;
		const std::vector<const matters::ReviewCell *> found = departures(view.table, *row);
		if (found.empty()) continue;
		PreparedChange change;
		change.document_id = document.document_id;
		change.document_name = document.name;
		for (const matters::ReviewCell *cell : found) change.column_ids.push_back(cell->column_id);
		// A Chat of its own, named for what it does, so the lawyer finds the
		// change - and its approval - beside the request that made it.
		matters::MatterChat chat = chat::create(project_id);
		chat = chat::rename(project_id, chat.id, "Playbook changes: " + document.name, chat.revision,
			matters::ChatTitleAuthority::User);
		change.chat_id = chat.id;
		chat::ChatRequestComposition composition;
		composition.project_id = project_id;
		composition.chat_id = chat.id;
		composition.prompt = change_instruction(view.table, *row, document.name);
		composition.source_policy_mode = chat::SourcePolicyMode::Selected;
		composition.selected_document_ids = {document.document_id};
		composition.target_document_id = document.document_id;
		composition.local_only = view.table.local_only;
		if (!by.agent_id.empty()) {
			composition.agent_run = true;
			composition.agent_initiated = true;
			composition.agent_name = by.agent_name;
			// The request is the agent's: its thread, Team and the change's own
			// record name the agent that prepared it.
			composition.agent_id = by.agent_id;
			// Nobody watches a colleague's request as it runs: a wait on the lawyer
			// would keep its place and return at once. It decides each point itself
			// and does not stop to ask; its change still waits for approval.
			composition.unattended = true;
		}
		// The change is asked of the Matter's model, as the table's own runs are
		// (review_table::model_for) - never the model an earlier run asked, which
		// may since have been retired or moved away from; a table that works only
		// here keeps its change here too (local_only above).
		change.operation_id = matters::generate_chat_operation_id();
		// The row names the change made from it, before the change exists: the
		// version it writes, once approved, is then never taken for a new
		// document that needs answering, and changing, again.
		try {
			review_table::note_change(project_id, table_id, document.document_id, change.operation_id);
		} catch (const std::exception &) {
		}
		// Each change is made as the Matter's own mode lets any member make it:
		// in a Matter that asks first it waits for the lawyer.
		stopWatch.set_running(change.operation_id);
		try {
			const chat::ChatExecutionOutcome outcome = chat::run_chat_request(change.operation_id, composition);
			change.state = chat::to_string(outcome.state);
			change.message = outcome.message;
			if (outcome.state == chat::ChatOperationState::WaitingForAnswer)
				change.question = asked_question(project_id, change.operation_id);
		} catch (const std::exception &error) {
			change.state = "failed";
			change.message = error.what();
		}
		stopWatch.set_running("");
		const bool closedOn = change.state == chat::to_string(chat::ChatOperationState::Interrupted);
		prepared.push_back(std::move(change));
		// Singularity closed on this request: nothing more is begun.
		if (closedOn) break;
	}
	return prepared;
}

bool request_stop(const std::string &project_id, const std::string &table_id) {
	std::shared_ptr<LiveBatch> batch;
	{
		const std::lock_guard<std::mutex> lock(batch_mutex());
		const auto found = live_batches().find(batch_key(project_id, table_id));
		if (found != live_batches().end()) batch = found->second;
	}
	if (!batch) return false;
	batch->stop.store(true);
	return true;
}

} // namespace singularity::playbook_changes
