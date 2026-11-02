#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::documents {

// Rows inserted into a workbook the way Excel's Insert Row puts them in: every
// row from the insertion down moves down, and so does everything in the
// workbook that reaches those rows - the formulas of every sheet, defined
// names, merged cells, conditional formats, data validations, tables and their
// filters, comments, pictures and charts - while the hidden flag, the height
// and the look of each row travel with the row itself. A new row takes the
// look of the row above it (of the row below where the one above is the
// sheet's heading), and a row that joins a table takes the formula of every
// column the table calculates. Nothing is ever written into a new row here:
// its values are cell writes of their own.
//
// What Singularity cannot move with certainty is refused, never approximated: a
// row inside an array formula, a what-if data table, a pivot table, a formula
// that reads a run of sheets at once, a format rule whose rows would compare
// with other rows once the new row is in, a sheet protected against inserted
// rows, or content of a kind Singularity does not know.

inline constexpr std::size_t kXlsxMaximumRows = 1048576;

// One new row: the sheet by its name ("" names the workbook's first sheet), and
// the number (1-based) the row has once it is in. Several new rows of one
// sheet are numbered as the sheet stands with all of them in.
struct XlsxRowInsertion {
	std::string sheet;
	std::size_t row = 0;
};

// Why the rows cannot be inserted, in the words a lawyer reads.
class XlsxRowInsertionRefused final : public std::runtime_error {
public:
	explicit XlsxRowInsertionRefused(const std::string &reason)
		: std::runtime_error(reason)
	{
	}
};

// A formula an insertion put into a new row: a calculated column of a table
// the row joined. `formula` carries its leading "=".
struct XlsxFilledFormula {
	std::string sheet;
	std::string ref;
	std::string formula;
};

// Inserts `rows` into the package at `path`, in place. Returns the formulas
// the new rows took from the tables they joined. Throws XlsxRowInsertionRefused
// when the rows cannot be inserted exactly, and std::runtime_error when the
// package cannot be read or written; the package is then left as it was.
std::vector<XlsxFilledFormula> insert_xlsx_rows(const std::filesystem::path &path,
	const std::vector<XlsxRowInsertion> &rows);

// The number a row of the sheet as it stands now has once the rows numbered
// `inserted` (as the sheet will stand, in any order) are in.
[[nodiscard]] std::size_t xlsx_row_once_inserted(std::size_t row, std::vector<std::size_t> inserted);

// `formula` (with or without its "=") of a cell on `formulaSheet`, as it reads
// once `count` rows are inserted at row `at` of `insertedSheet`; nothing when
// it names a run of sheets, or rows of a sheet it does not name.
[[nodiscard]] std::optional<std::string> xlsx_formula_with_rows_inserted(const std::string &formula,
	const std::string &formulaSheet, const std::string &insertedSheet, std::size_t at, std::size_t count);

// Proves that the workbook at `after` is the one at `before` with `rows`
// inserted and nothing else: every cell with what it held, where it now stands;
// every formula reading what it read; every hidden row still hidden and every
// merge still merged; the new rows empty but for the formulas `filled` names.
// Nothing when it is; otherwise what differs.
[[nodiscard]] std::optional<std::string> xlsx_rows_inserted_exactly(const std::filesystem::path &before,
	const std::filesystem::path &after, const std::vector<XlsxRowInsertion> &rows,
	const std::vector<XlsxFilledFormula> &filled);

// ---- Formulas Singularity writes ----------------------------------------------------

// Whether a value given to a cell is a formula: it begins with "=" and has
// something after it, the way typing into Excel makes one.
[[nodiscard]] bool is_xlsx_formula(const std::string &value);

// Why the formula `typed` cannot be written into `cellRef` of `sheet` in a
// workbook of `sheetNames`; nothing when it can. A formula that reads another
// workbook, a run of sheets, a sheet the workbook does not have or its own
// cell, one that fills other cells than its own, or one that is not whole, is
// never written.
[[nodiscard]] std::optional<std::string> xlsx_formula_refusal(const std::string &typed, const std::string &sheet,
	const std::string &cellRef, const std::vector<std::string> &sheetNames);

// The formula as a workbook stores it: no "=", function names and cell
// references in capitals, and a function newer than Excel 2007 under the name
// the file format gives it ("_xlfn.XLOOKUP").
[[nodiscard]] std::string xlsx_formula_stored(const std::string &typed);

// The formula as Excel shows it in its formula bar: "=" and the stored text,
// without the file format's prefixes.
[[nodiscard]] std::string xlsx_formula_shown(const std::string &typed);

// ---- Rows deleted ------------------------------------------------------------------
//
// Rows deleted the way Excel's Delete Row takes them out: every row below moves
// up, and so does everything in the workbook that reaches those rows - the
// formulas of every sheet, defined names, merged cells, conditional formats,
// data validations, tables and their filters, comments, pictures and charts. A
// range that reaches into the deleted rows shrinks by them; a reference that
// reads only deleted rows becomes #REF!, exactly as Excel writes it, and is
// reported so the lawyer reads which formula it is before approving. A formula
// whose range shrank, or that lost what it read, keeps no value it was saved
// with: its value is worked out again, or left to Excel when it opens the file.
//
// What Singularity cannot take out with certainty is refused, never approximated:
// part of an array formula, a what-if data table, a pivot table, a table's
// heading or totals row, every row of a table, a note, picture or chart that
// stands on a deleted row, a filter's heading row, a format rule whose rows
// would compare with other rows once the rows are gone, a sheet protected
// against deleted rows, or content of a kind Singularity does not know.

// One row to delete: the sheet by its name ("" names the workbook's first
// sheet) and the row's number (1-based) as the sheet stands now, before any
// row of the change is deleted.
struct XlsxRowDeletion {
	std::string sheet;
	std::size_t row = 0;
};

// Why the rows cannot be deleted, in the words a lawyer reads.
class XlsxRowDeletionRefused final : public std::runtime_error {
public:
	explicit XlsxRowDeletionRefused(const std::string &reason)
		: std::runtime_error(reason)
	{
	}
};

// Deleted rows of one sheet, numbered as the sheet stands now.
struct XlsxLostRows {
	std::string sheet;
	std::vector<std::size_t> rows;
};

// A formula that will no longer find what it reads once the rows are gone:
// Excel shows #REF! for it. Named as the workbook stands now.
struct XlsxLostReference {
	// "cell", "name", "conditional format", "data validation", "table column",
	// "chart", "sparkline".
	std::string kind;
	// The sheet the formula stands on; "" for a name of the whole workbook or
	// a chart.
	std::string sheet;
	// The cell it stands in ("D40"), the name ("Fees"), or the range a rule
	// covers ("C2:C5"); "" for a chart.
	std::string place;
	// With its leading "=", as Excel's formula bar shows it now and once the
	// rows are gone.
	std::string formula;
	std::string formula_after;
	// The deleted rows it read, and their sheet - the first sheet it lost rows
	// of; `more_rows` the rows of every other sheet it lost rows of.
	std::string rows_sheet;
	std::vector<std::size_t> rows;
	std::vector<XlsxLostRows> more_rows;
};

// A formula cell whose value no longer holds once the rows are gone - it read
// a deleted row, directly, through a whole column, a defined name or a table
// that loses rows, or it counts rows by where they stand (ROW) or decides at
// run time what it reads (INDIRECT, OFFSET) - named as the sheet stands
// without them. It keeps no value it was saved with.
struct XlsxRecalculatedCell {
	std::string sheet;
	std::string ref;
};

struct XlsxRowDeletionResult {
	std::vector<XlsxLostReference> lost;
	std::vector<XlsxRecalculatedCell> recalculated;
};

// Deletes `rows` from the package at `path`, in place. Throws
// XlsxRowDeletionRefused when the rows cannot be deleted exactly, and
// std::runtime_error when the package cannot be read or written; the package is
// then left as it was.
XlsxRowDeletionResult delete_xlsx_rows(const std::filesystem::path &path, const std::vector<XlsxRowDeletion> &rows);

// What delete_xlsx_rows would report, found without changing anything.
[[nodiscard]] std::vector<XlsxLostReference> xlsx_references_lost(const std::filesystem::path &path,
	const std::vector<XlsxRowDeletion> &rows);

// The number a row of the sheet as it stands now has once the rows numbered
// `deleted` (as the sheet stands now) are gone; 0 when it is one of them.
[[nodiscard]] std::size_t xlsx_row_once_deleted(std::size_t row, std::vector<std::size_t> deleted);

// `formula` (with or without its "=") of a cell on `formulaSheet`, as it reads
// once `count` rows from row `at` of `deletedSheet` are deleted; nothing when
// it names a run of sheets, or rows of a sheet it does not name.
[[nodiscard]] std::optional<std::string> xlsx_formula_with_rows_deleted(const std::string &formula,
	const std::string &formulaSheet, const std::string &deletedSheet, std::size_t at, std::size_t count);

// Proves that the workbook at `after` is the one at `before` with `rows`
// deleted and nothing else: every cell that stays with what it held, where it
// now stands; every formula reading what Excel's Delete Row makes it read, and
// every formula whose value no longer holds (XlsxRecalculatedCell) keeping no
// value; every hidden row still hidden and every merge where it moved. Nothing when it
// is; otherwise what differs.
[[nodiscard]] std::optional<std::string> xlsx_rows_deleted_exactly(const std::filesystem::path &before,
	const std::filesystem::path &after, const std::vector<XlsxRowDeletion> &rows);

// ---- Sheets added ------------------------------------------------------------------
//
// A new, empty worksheet after the workbook's last tab, as Excel's New Sheet
// makes one: its own part, its entry among the workbook's sheets with a sheet
// id and relationship of its own, its content type, and its name among the
// workbook's titles (docProps/app.xml) where the workbook lists them. Its cells
// take the workbook's default look; its values are cell writes of their own.

class XlsxSheetAdditionRefused final : public std::runtime_error {
public:
	explicit XlsxSheetAdditionRefused(const std::string &reason)
		: std::runtime_error(reason)
	{
	}
};

// A sheet name's letters folded for comparison: two names Excel holds to be the
// same fold to the same text. Without one, ASCII letters are folded alone.
using XlsxNameFold = std::string (*)(const std::string &name);

// Why a new sheet cannot be named `name` beside `existing`, in the words a
// lawyer reads; nothing when it can. Excel's rules: 1 to 31 characters, none of
// : \ / ? * [ ], not beginning or ending with an apostrophe, not "History", and
// no other sheet of the same name in any letter case.
[[nodiscard]] std::optional<std::string> xlsx_sheet_name_refusal(const std::string &name,
	const std::vector<std::string> &existing, XlsxNameFold fold = nullptr);

// Adds a new, empty sheet named each of `names`, in that order, after the
// workbook's last sheet. Throws XlsxSheetAdditionRefused when a name cannot be
// given, and std::runtime_error when the package cannot be read or written;
// the package is then left as it was.
void add_xlsx_sheets(const std::filesystem::path &path, const std::vector<std::string> &names,
	XlsxNameFold fold = nullptr);

// Proves that the workbook at `after` is the one at `before` with an empty
// sheet named each of `names` after its last one, and nothing else.
[[nodiscard]] std::optional<std::string> xlsx_sheets_added_exactly(const std::filesystem::path &before,
	const std::filesystem::path &after, const std::vector<std::string> &names);

} // namespace singularity::documents
