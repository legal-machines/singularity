#pragma once

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "Document/Types/SheetRenderTypes.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTableTypes.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// The local table model (DESIGN4 §6): every tabular document a prompt draws
// from, read whole from the workbook itself - all rows, hidden ones included,
// formulas and the values they were saved with - and held on this computer
// for the protected computation protocol. The model addresses it by handle,
// column letter and row; it never reconstructs a sum from masked row text.
//
// Papinian never reads files: the Document side hands the tables over
// (Routing/ProtectedTableSource), and this module builds the model from them.
// Pure: nothing here calls the guard. A cell is linked to the registry row
// its reading produced by reading the operation's own table (fact_by_cell);
// a cell no prompt carried keeps a root key of its own, and gets a symbol only
// when an operation must name it (OpContext::issue_cell).
namespace singularity::protected_api::papinian::tables {

// A Word or Markdown table as the Document side reads it, cell text by row
// (the first row is its header when it has one).
struct WordTable {
	std::string table_id;
	std::vector<std::vector<std::string>> rows;
};

// What the Document side hands over for one tabular document.
struct TableInput {
	std::string document_id;
	// How the prompt names it ("DOC_3"), when the caller knows it.
	std::string document_handle;
	std::string title;
	std::optional<singularity::documents::SheetWorkbookRender> workbook;
	std::vector<WordTable> word_tables;
	std::vector<std::vector<std::string>> csv;
};

// The table models of `inputs`, handles numbered after the tables `symbols`
// already holds, each cell linked to the registry row the workbook reading's
// occurrence produced. `evidence` is the raw SOURCE_MATERIAL: the sheets and
// columns it touches are the ones described (DESIGN4 §6.2, D14); every row of
// every table is available for execution all the same.
[[nodiscard]] std::vector<ProtectedTable> build_tables(const std::vector<TableInput> &inputs,
	const OperationProtectedSymbolTable &symbols, const std::vector<std::string> &evidence = {});

// The seam's spelling (lane S): the guard is not needed to build - kept so a
// caller holding it compiles unchanged.
[[nodiscard]] inline std::vector<ProtectedTable> build_tables(const std::vector<TableInput> &inputs,
		OperationProtectedSymbolTable &table, const PapinianGuard &) {
	return build_tables(inputs, static_cast<const OperationProtectedSymbolTable &>(table));
}

// Marks described the sheets and columns the raw passages' workbook readings
// touch ("Sheet \"Ledger\"", "Row 16: A16 = ... | B16 = ...").
void mark_evidence(std::vector<ProtectedTable> &tables, const std::vector<std::string> &evidence);

// The table models a prompt's own passages hold when no document could be read
// (a pasted table): the cycle-3 reading of workbook lines, moved here.
[[nodiscard]] std::vector<ProtectedTable> tables_in_passages(const std::vector<std::string> &raw_passages);

// Band schedules enumerated in contract prose (DESIGN4 §6.6): "from X to Y -
// r %", "above X: r %", «от X до Y — r %», «свыше X» ..., one band per line or
// per semicolon clause. Each threshold and band value is the symbol of the
// fact it is, or a figure the material writes openly.
[[nodiscard]] std::vector<ProtectedSchedule> schedules_in(
	const std::vector<singularity::context::business_calendar::RulePassage> &raw_passages,
	const OperationProtectedSymbolTable &table);

// Links the cells of the operation's tables that no fact had when they were
// built to the registry rows protection has issued for them since (a
// workbook reading the first call carried): one cell, one fact key, so an
// observation of it is charged once, under the fact it is.
void relink_tables(OperationProtectedSymbolTable &symbols);

// A table by its handle ("Table1" or "[[Table1]]"), or nullptr.
[[nodiscard]] const ProtectedTable *find_table(const OperationProtectedSymbolTable &table, const std::string &handle);
[[nodiscard]] ProtectedTable *find_table(OperationProtectedSymbolTable &table, const std::string &handle);
[[nodiscard]] const ProtectedSchedule *find_schedule(const OperationProtectedSymbolTable &table, const std::string &handle);

// ---------------------------------------------------------------------------
// Reading the model.

[[nodiscard]] const TableColumn *column_of(const ProtectedTable &table, const std::string &letter);
[[nodiscard]] const TableRow *row_of(const ProtectedTable &table, std::size_t number);
[[nodiscard]] const TableCell *cell_of(const ProtectedTable &table, const std::string &ref);
[[nodiscard]] const TableCell *cell_of(const ProtectedTable &table, std::size_t row, const std::string &letter);
[[nodiscard]] const DerivedColumn *derived_column(const ProtectedTable &table, const std::string &label);

// A table's own key: a digest of its document and sheet, never their words.
[[nodiscard]] std::string place_key(const ProtectedTable &table);
// The fact key a cell is charged and traced as: its registry row's, or, for a
// cell no prompt carried, one of its own that names where it stands (by the
// table's digest and the cell's reference).
[[nodiscard]] std::string cell_root(const ProtectedTable &table, const TableCell &cell);
// A column's public grouping key (DESIGN4 §8.3): (document, sheet, letter).
[[nodiscard]] std::string group_key(const ProtectedTable &table, const std::string &letter);

// A type as §2.4 writes it ("MONEY(USD/month)", "DATE(month)") read back.
[[nodiscard]] std::optional<compute::ArgType> parse_type(const std::string &written);

// The rows of `rows` with their labels, in row order, at most `limit`: what
// `list_rows` gives and what [[Rows1]] restores to. Labels are the material's
// words (a data span when sent).
[[nodiscard]] std::vector<std::pair<std::size_t, std::string>> row_labels(const ProtectedTable &table,
	const std::vector<std::size_t> &rows, std::size_t limit);

// The tables block's parts for one table (DESIGN4 §6.4): each line as pieces
// that are Singularity's structure (control text) or the material's words (a data
// span: title, sheet, headers - full detection when sent). Only described
// columns are listed.
struct BlockPart {
	bool data = false;
	std::string text;
};
using BlockLine = std::vector<BlockPart>;
[[nodiscard]] std::vector<BlockLine> describe(const ProtectedTable &table);
// "DOC_3 has 3 more sheets; describe_tables(DOC_3) describes them" as a count
// per document of tables not described.
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> undescribed_sheets(
	const std::vector<ProtectedTable> &tables);

// ---------------------------------------------------------------------------
// The protocol's operands over tables (DESIGN4 §2.2): what the executor's
// resolver calls for a table handle, a column, a row, a cell, a row set, a
// group set, a derived column, a schedule or a document. Returns false when
// `arg` is none of these (the executor resolves it); true with `error.code`
// set when it is one and cannot be used.
[[nodiscard]] bool resolve_arg(const OperationProtectedSymbolTable &symbols, const compute::Arg &arg,
	compute::Operand &operand, compute::TypeError &error);

// Keeps what a table op returned under the symbol the executor issued for it
// ("[[Rows1]]") and the model's label: a row set, a group set or a derived
// column, so a later op can name it.
void remember(OperationProtectedSymbolTable &symbols, const std::string &symbol, const std::string &label,
	const compute::OpAnswer &answer);

// ---------------------------------------------------------------------------
// Workbook lines in a passage (the cycle-3 grammar, moved from
// ProtectedRelations): "Sheet \"Ledger\", rows 3-19" opens a sheet, "Row 16
// (hidden): A16 = ... | B16 = 42500 (shown in $) {=B14+B15}" is a row.

struct PassageCell {
	std::string ref;
	std::string column;
	unsigned row = 0;
	// The value as written, without its notes or formula.
	std::string raw;
	// "(shown in $)" and similar, without the leading space.
	std::string note;
	// Without its braces: "=B14+B15".
	std::string formula;
};

struct PassageRow {
	unsigned number = 0;
	bool hidden = false;
	std::vector<PassageCell> cells;
};

struct PassageSheet {
	std::size_t passage = 0;
	// From the passage's "Sheet \"X\"" line, when it has one.
	std::string name;
	bool sheet_hidden = false;
	std::vector<PassageRow> rows;
};

// One "B16 = value (note) {=formula}" piece, or nothing when it is not one.
[[nodiscard]] std::optional<PassageCell> parse_passage_cell(const std::string &piece);
// Every sheet of at least `min_rows` rows the passages hold, at most
// `max_tables`, each at most `max_rows` rows, rows in the order read.
[[nodiscard]] std::vector<PassageSheet> passage_sheets(const std::vector<std::string> &passages,
	std::size_t max_tables, std::size_t max_rows, std::size_t min_rows = 2);

} // namespace singularity::protected_api::papinian::tables
