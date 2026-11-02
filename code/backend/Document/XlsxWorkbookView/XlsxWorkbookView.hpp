#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/SheetRenderTypes.hpp"

namespace singularity::documents {

// Reads an .xlsx package into the workbook a lawyer LOOKS at
// (SheetRenderTypes.hpp): every sheet in workbook order with its tab state,
// frozen panes, gridlines, default and declared column widths, row heights,
// hidden rows and columns and merged ranges; every cell with its raw value,
// the text its number format draws (XlsxNumberFormat, the one formatter), its
// formula - a shared formula translated to the cell, as Excel shows it - and
// its own look, with theme colours and tints resolved.
//
// The same package posture as XlsxExtractor: the archive is read in memory,
// DTDs and entities are refused, and its safety limits hold (24 MiB per XML
// part, 4096 archive entries, 200,000 cells per sheet). A sheet past a limit
// is shown as far as it fits and `limitation` says where it stops - the rest of
// the workbook is never thrown away with it. Formulas are never calculated:
// a cell shows the value Excel last saved with it, or nothing when the file
// carries none. This reader is read-only and changes nothing the model reads
// (XlsxExtractor stays the extraction path); what a formula holds after
// Singularity writes a cell is worked out apart from it, below.

// An array formula written once on its first cell and calculated into every
// cell of its range ("C2:C3"); the other cells hold only the values it last
// spilled into them. A what-if data table ({=TABLE(,A1)}) is kept the same way:
// Excel fills its whole range from the formulas beside it, so any change may
// move every value in it. 0-based and inclusive.
struct XlsxArrayFormula {
	std::string sheet;
	std::size_t first_row = 0;
	std::size_t first_column = 0;
	std::size_t last_row = 0;
	std::size_t last_column = 0;
	bool is_data_table = false;
};

struct XlsxWorkbookViewResult {
	// ready when the workbook was read, empty when it holds no cells at all,
	// unreadable when the package itself could not be read.
	ExtractionStatus status = ExtractionStatus::unreadable;
	SheetWorkbookRender workbook;
	// What is not shown, in the words a lawyer is shown; empty when the reading
	// is complete.
	std::string limitation;
	std::vector<ExtractionDiagnostic> diagnostics;
	// Every array formula whose range is more than its first cell, and every
	// data table.
	std::vector<XlsxArrayFormula> array_formulas;
};

[[nodiscard]] XlsxWorkbookViewResult read_xlsx_workbook_view(const DocumentExtractionRequest& request);

// A cell by its sheet's name ("" for the workbook's first sheet) and reference.
struct XlsxCellAddress {
	std::string sheet;
	std::string ref;
};

// A formula that reads a changed cell, directly or through another formula,
// and what it holds once the changed cells hold their new values.
struct XlsxFormulaResult {
	std::string sheet;
	std::string ref;
	// The value calculated again, as a workbook stores it ("3250000"). Nothing
	// where Singularity does not calculate the formula itself - anything beyond
	// numbers, cells, + - * / ^ %, brackets and SUM - so the value it was saved
	// with no longer holds and Excel calculates it when the workbook is opened.
	std::optional<std::string> stored;
};

// The formulas of `workbook` - read after `changed` were written, and whole,
// with nothing left out at a safety limit - whose saved values the change
// leaves behind, each calculated again where Singularity can. A formula whose
// references cannot all be read (a defined name, INDIRECT, OFFSET, another
// workbook, a table's columns) may read the change, so it is among them.
// An array formula of `arrays` that reads the change is never calculated
// here, and neither is any cell of its range: each is named, with no value,
// and so is every formula that reads one of them. A data table of `arrays`
// is read as reading everything, so any change names its whole range.
// The search and the calculation cost about the size of the formulas that
// read the change, not the square of the sheet: a ledger of thousands of
// running balances is settled well within a second.
// Nothing here writes: the workbook's writer puts the values in.
// A changed cell that itself holds a formula - one Singularity has just written -
// is calculated with them where Singularity can, and named with no value where it
// cannot.
[[nodiscard]] std::vector<XlsxFormulaResult> formulas_reading_changed_cells(const SheetWorkbookRender& workbook,
	const std::vector<XlsxCellAddress>& changed, const std::vector<XlsxArrayFormula>& arrays = {});

// The same search in a workbook too large for its reading to be whole (a sheet
// past the reading's safety limits): every formula of every sheet, read from
// the package itself - a sheet part of up to 64 MiB - that reads a changed
// cell, directly or through another formula, and every cell of an array
// formula's range it reaches. None is calculated here: each is named with no
// value, so its writer leaves it without the total it was saved with and Excel
// calculates it when the workbook is opened. Nothing when a sheet cannot be
// read that far - then no total of the workbook can be kept true.
[[nodiscard]] std::optional<std::vector<XlsxFormulaResult>> package_formulas_reading_changed_cells(
	const std::filesystem::path& path, const std::vector<XlsxCellAddress>& changed);

// A shared formula's text as one cell of its range holds it: the master's text
// (as the package stores it, without "=") with every relative reference moved
// by that cell's distance from the master - the reading every cell of the
// range is shown with.
[[nodiscard]] std::string xlsx_shared_formula_text(const std::string& master, long long rowOffset,
	long long columnOffset);

} // namespace singularity::documents
