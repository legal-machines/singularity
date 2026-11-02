#pragma once

#include "Matter/Types/Matter.hpp"
#include "Routing/Types/ModelTarget.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::review_table {

/*
 * Review Tables: the same questions asked of many documents of a Matter.
 *
 * A run reads each document whole and asks the model one structured question
 * per document - every column at once - on the Matter's model or a local one,
 * through the same execution as every request (Papinian Guard protecting what
 * leaves this computer, LocalOnly keeping work here). For every answer the
 * model copies the words it rests on; Singularity then finds those words in
 * the document itself, and an answer whose words it cannot find is shown as
 * unconfirmed, never as checked.
 *
 * A run writes each document's row as soon as it is answered. Stopped, cut off
 * by a quit, or failing on one document, it keeps every row it finished; the
 * next run answers only the rows still missing, the ones that failed, and
 * those whose document changed since it was answered. Usage - tokens and the
 * cost the provider reported - is kept per row and in the Matter's Usage.
 */

struct NewColumn {
	std::string heading;
	std::string question;
	std::string answer_kind = "text";
	std::string playbook_id;
	std::string position_id;
	std::string position;
	std::string law;
};

struct CreateReviewTable {
	std::string project_id;
	std::string title;
	std::vector<std::string> document_ids;
	std::vector<NewColumn> columns;
	std::string playbook_id;
	int playbook_version = 0;
	// Every model call of the table stays on this computer.
	bool local_only = false;
};

matters::ReviewTable create(const CreateReviewTable &command);

struct UpdateReviewTable {
	std::string project_id;
	std::string table_id;
	std::optional<std::string> title;
	// The table's documents and questions, whole, when they change. A column
	// keeps its id (and its answers) while its question stays the same; a
	// changed question is a new column, answered again.
	std::optional<std::vector<std::string>> document_ids;
	std::optional<std::vector<matters::ReviewColumn>> columns;
	std::optional<bool> local_only;
};

matters::ReviewTable update(const UpdateReviewTable &command);

bool remove(const std::string &project_id, const std::string &table_id);

// What a table shows now: the table, its rows, and per document whether its
// row is current, stale (the document changed since), missing or failed.
struct RowStatus {
	std::string document_id;
	// "current", "stale", "missing", "failed", "unavailable".
	std::string status;
	// Current, and the document has changed since the row read it only by the
	// change prepared from the row itself, approved and written: the row's
	// departures are what that change was made for.
	bool change_written = false;
};

struct ReviewTableView {
	matters::ReviewTable table;
	std::vector<matters::ReviewRow> rows;
	std::vector<RowStatus> statuses;
	// Rows the next run would answer.
	std::size_t to_answer = 0;
	// What the table's rows cost: tokens, and the provider-reported cost when
	// every billed row reported one.
	std::uint64_t input_tokens = 0;
	std::uint64_t output_tokens = 0;
	std::optional<std::int64_t> cost_micros;
	std::string cost_currency;
};

std::vector<matters::ReviewTable> list(const std::string &project_id);
ReviewTableView get(const std::string &project_id, const std::string &table_id);

struct RunOptions {
	// The model to ask, in place of the Matter's AI Setup.
	std::optional<routing::ModelTarget> model;
	// Called after each row is written, with what the table shows then.
	std::function<void(const ReviewTableView &)> progress;
};

// Runs the table on this thread until every row that needs an answer has one,
// the lawyer stops it, or it cannot go on. Throws only when it cannot start.
ReviewTableView run(const std::string &project_id, const std::string &table_id, const RunOptions &options = {});

// Asks a run of the table on this computer to stop: the document in flight is
// abandoned and nothing more is sent. Recorded in the table either way.
void request_stop(const std::string &project_id, const std::string &table_id);

// Records that the change with `operation_id` is prepared from the answers of
// the document's row (WorkProduct/PlaybookChanges). When the lawyer approves it
// and Singularity writes it, the version it writes is that change, not a new
// document: the row stays current, and nobody is asked to answer it again.
// Nothing happens when the row is gone.
void note_change(const std::string &project_id, const std::string &table_id, const std::string &document_id,
	const std::string &operation_id);

// The table as a real Excel workbook - the answers, and every quote with
// where it stands - or a Word document holding the same. Bytes of the file.
std::string export_xlsx(const std::string &project_id, const std::string &table_id);
std::string export_docx(const std::string &project_id, const std::string &table_id);

// The prompt and output contract one row is asked with (tests and the CLI).
std::string row_prompt(const matters::ReviewTable &table, const std::string &document_name,
	const std::vector<std::string> &passages);
nlohmann::json row_contract(const matters::ReviewTable &table);

} // namespace singularity::review_table
