#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matters {

/*
 * A Review Table: the same questions asked of many documents, answered as one
 * table - a row per document, a column per question. Every answer names the
 * document it comes from and the exact words it rests on, checked against
 * that document by Singularity, never by the model.
 *
 * Kept in the Matter as two collections (Persistence/ProjectStore): the table
 * itself - its questions and the state of its run - and one record per
 * document row, written as each document is answered, so a run that stops
 * keeps every row it finished and a large table never rewrites itself whole.
 */

// One question asked of every document: a column.
struct ReviewColumn {
	std::string id;
	std::string heading;
	std::string question;
	// The kind of answer the column holds: "text", "date", "amount", "yes_no",
	// "party", "duration" or "clause".
	std::string answer_kind = "text";
	// The firm's position this column checks the document against, when the
	// column comes from a Playbook (WorkProduct/Playbook): the position as the
	// Playbook stated it when the table was made - firm policy, never law or
	// a fact of the document.
	std::string playbook_id;
	std::string position_id;
	std::string position;
	// The law the firm cites for that position, as the Playbook states it -
	// kept apart from the position, and never supplied by a model.
	std::string law;
};

// A document the table reads: a row.
struct ReviewDocument {
	std::string document_id;
	std::string name;
};

struct ReviewTable {
	std::string id;
	std::string title;
	std::string created_at;
	std::string updated_at;
	std::string created_by_actor_id;
	std::vector<ReviewDocument> documents;
	std::vector<ReviewColumn> columns;
	// The Playbook the table checks the documents against, as it stood when
	// the table was made (its version), when there is one.
	std::string playbook_id;
	int playbook_version = 0;
	// The run: "idle", "running", "stopped", "completed", "interrupted" or
	// "failed"; the computer running it, and the model its last run asked -
	// what the table says answered it. Each run asks the Matter's model anew
	// (review_table::model_for); this is never a choice that holds.
	std::string run_state = "idle";
	std::string run_id;
	std::string run_device_id;
	std::string run_started_at;
	std::string run_finished_at;
	bool stop_requested = false;
	std::string provider_id;
	std::string model_id;
	// Every model call of this table stays on this computer.
	bool local_only = false;
	std::string run_message;
	// The Work Product the table is kept as (canonical.review-table).
	std::string work_product_id;
	std::uint64_t revision = 1;
};

// The words an answer rests on, as Singularity found them in the document.
struct ReviewEvidence {
	std::string quote;
	std::string chunk_id;
	// Where the quote stands in the document's text, and whether Singularity
	// found it there word for word (whitespace, quote marks and case aside).
	std::size_t start = 0;
	std::size_t length = 0;
	std::optional<int> page;
	std::string heading;
	bool verified = false;
	// The model's quote differed from the document's words - a name written
	// out in full where the document uses its short form, a figure written
	// another way - and Singularity found the passage it meant: the quote is
	// the document's own words, as they stand.
	bool aligned = false;
};

// One answer: a cell.
struct ReviewCell {
	std::string column_id;
	// "answered", "not_found" (the document does not address it) or "failed".
	std::string state = "answered";
	// The answer as the lawyer reads it, and the same in a normal form for its
	// kind: an ISO date, "USD 2000000", "yes"/"no", a number of months.
	std::string answer;
	std::string value;
	// How sure the answer is ("high", "medium", "low"), and whether the quote
	// states it ("stated") or the model concluded it from the quote
	// ("inferred").
	std::string confidence;
	std::string basis;
	std::string reasoning;
	// Against a Playbook position: "conforms", "deviates" or "missing", and
	// why - the model's assessment, never the firm's.
	std::string assessment;
	std::string assessment_reason;
	std::vector<ReviewEvidence> evidence;
};

// One document's row, as the run answered it.
struct ReviewRow {
	// "<table id>:<document id>".
	std::string id;
	std::string table_id;
	std::string document_id;
	// "answered", "failed" or "skipped" (the document could not be read).
	std::string state;
	std::string message;
	// The document as the row read it: an answer read from an earlier version
	// is shown as such, and the next run answers it again.
	std::string document_fingerprint;
	int document_version = 0;
	std::string provider_id;
	std::string model_id;
	// "local", "protected" or "unprotected".
	std::string destination;
	std::uint64_t input_tokens = 0;
	std::uint64_t output_tokens = 0;
	std::optional<std::int64_t> cost_micros;
	std::string cost_currency;
	std::int64_t latency_ms = 0;
	std::string answered_at;
	std::string run_id;
	std::vector<ReviewCell> cells;
	// The change prepared from these answers to bring the document to the
	// firm's position (WorkProduct/PlaybookChanges): its operation. Once the
	// lawyer approves it and it is written, the document is that change, and
	// the row still stands for it: it is not answered again for it.
	std::string change_operation_id;
};

} // namespace singularity::matters
