#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// A workbook as its author laid it out, for a lawyer to LOOK at - the sheet
// counterpart of DocumentRenderModel.
//
// XlsxExtractor reads a workbook so a model can reason about it: every cell
// flattened to "A1<tab>value". That is a transcript, not a spreadsheet. This
// is the reading that keeps what Excel shows: each sheet in workbook order with
// its tab state, frozen panes, column widths, hidden rows and columns and
// merged ranges; each cell with its raw value, the text its number format
// draws, its formula and its own look.
//
// It is a READING and nothing else - nothing here is written back, and a cell
// change is still validated against the model's reading, never this one.
// Nothing is invented: a style the workbook does not declare is simply absent,
// and a formula Singularity cannot calculate keeps the value Excel last saved with
// it (or none, when the file carries none).
//
// This header deliberately depends on nothing else in Document/: the document
// reading (DocumentRenderTypes.hpp) is the one that carries a workbook.

// What kind of value a cell holds. A date is a number that its number format
// draws as a date; its value stays Excel's serial number.
enum class SheetCellKind { Empty, Number, Text, Boolean, Date, Error };

// Whether this cell is what it was in the version Singularity compared against.
// Unknown is what every cell carries when nothing was compared.
enum class SheetCellChangeMark { Unknown, Unchanged, Added, Changed };

// A sheet tab's own state in the workbook. A very hidden sheet is one Excel's
// own Unhide list does not offer.
enum class SheetVisibility { Visible, Hidden, VeryHidden };

// A "*x" repeat fill in the number format: what stands before the fill hugs the
// cell's left edge and what stands after it hugs the right edge - the "$" of an
// accounting format on the left, the amount on the right.
struct SheetCellFill {
	std::string before;
	std::string after;
};

// Each edge's line style as the workbook names it ("thin", "medium", "thick",
// "double", "dashed", "dotted", "hair", ...), "" for no line.
struct SheetCellBorders {
	std::string top;
	std::string right;
	std::string bottom;
	std::string left;
};

struct SheetCellStyle {
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strike = false;
	// "RRGGBB" with theme colours and tints already resolved. "" where the text
	// is the workbook's own default colour (the reader's ink applies), "auto",
	// or a colour the workbook does not resolve.
	std::string font_color_hex;
	// "RRGGBB" of a solid fill, "" for none (or a pattern other than solid).
	std::string fill_color_hex;
	// Excel's own words: "general", "left", "center", "right", "fill",
	// "justify", "centerContinuous", "distributed".
	std::string horizontal = "general";
	// "top", "center" or "bottom" (Excel's default).
	std::string vertical = "bottom";
	bool wrap = false;
	unsigned int indent = 0;
	// Points where the cell's font is not the workbook's default size;
	// nothing where it is.
	std::optional<double> font_size_points;
	SheetCellBorders borders;
};

struct SheetRenderCell {
	// 0-based column, and the reference a lawyer reads ("B4").
	std::size_t column = 0;
	std::string ref;
	SheetCellKind kind = SheetCellKind::Empty;
	// The raw value, by kind: `number` for Number and Date (a date stays its
	// serial number), `raw_text` for Text and Error, `boolean` for Boolean.
	// Empty carries none of them.
	double number = 0.0;
	std::string raw_text;
	bool boolean = false;
	// A number exactly as the workbook stores it ("0.14883720930232559"), for a
	// Number or Date cell read from a stored number; empty otherwise. What the
	// model reads of a number (XlsxExtractor) is this, never a rounded display.
	std::string stored_value;
	// What Excel displays at unlimited width, drawn by the one number formatter
	// (XlsxNumberFormat) from the cell's own format.
	std::string text;
	std::optional<SheetCellFill> fill;
	// "RRGGBB" from a [Red]/[Blue]/[ColorN] section colour, "" otherwise.
	std::string color_hex;
	// With its leading "=". A shared formula is already translated to this
	// cell; an array formula stands as written on its anchor cell.
	std::optional<std::string> formula;
	// The format code the text was drawn with ("General", "0.00%", ...).
	std::string number_format = "General";
	SheetCellStyle style;
	SheetCellChangeMark change = SheetCellChangeMark::Unknown;
	std::string previous_text;
};

struct SheetRenderRow {
	// 0-based.
	std::size_t index = 0;
	// Points, where the workbook stores this row's height.
	std::optional<double> height_points;
	bool hidden = false;
	// Sorted by column; only cells the workbook actually has.
	std::vector<SheetRenderCell> cells;
};

// One <cols> entry: a 0-based inclusive column range.
struct SheetRenderColumn {
	std::size_t first = 0;
	std::size_t last = 0;
	// Characters, as Excel's Column Width dialog shows them (8.43 is Excel's
	// standard column); nothing where the range declares no width.
	std::optional<double> width_characters;
	bool hidden = false;
};

// A merged range, 0-based inclusive.
struct SheetRenderMerge {
	std::size_t first_row = 0;
	std::size_t first_column = 0;
	std::size_t last_row = 0;
	std::size_t last_column = 0;
};

// A cell the compared version had and this one does not.
struct SheetRemovedCell {
	std::string ref;
	std::string previous_text;
};

struct SheetRender {
	std::string name;
	SheetVisibility visibility = SheetVisibility::Visible;
	// The used extent: the last used row/column index + 1, 0 for an empty sheet.
	std::size_t row_count = 0;
	std::size_t column_count = 0;
	// Rows above and columns left of a frozen pane; 0 where nothing is frozen.
	std::size_t frozen_rows = 0;
	std::size_t frozen_columns = 0;
	bool show_grid_lines = true;
	// Characters (see SheetRenderColumn) and points.
	double default_column_width = 8.43;
	double default_row_height = 15.0;
	std::vector<SheetRenderColumn> columns;
	// Sorted by index; only rows the workbook actually has.
	std::vector<SheetRenderRow> rows;
	std::vector<SheetRenderMerge> merges;
	std::vector<SheetRemovedCell> removed_cells;
};

struct SheetWorkbookRender {
	// The 1904 date system: serial 0 is 1 January 1904 rather than Excel's
	// fictional 0 January 1900.
	bool date1904 = false;
	// The tab the workbook was saved on, as an index into `sheets`.
	std::size_t active_sheet = 0;
	// Every sheet in workbook order, hidden ones included.
	std::vector<SheetRender> sheets;
};

// The wire vocabulary, spelled once.

inline const char* sheet_cell_kind_name(SheetCellKind kind) {
	switch (kind) {
		case SheetCellKind::Number: return "number";
		case SheetCellKind::Text: return "text";
		case SheetCellKind::Boolean: return "boolean";
		case SheetCellKind::Date: return "date";
		case SheetCellKind::Error: return "error";
		case SheetCellKind::Empty: break;
	}
	return "empty";
}

inline const char* sheet_cell_change_name(SheetCellChangeMark change) {
	switch (change) {
		case SheetCellChangeMark::Unchanged: return "unchanged";
		case SheetCellChangeMark::Added: return "added";
		case SheetCellChangeMark::Changed: return "changed";
		case SheetCellChangeMark::Unknown: break;
	}
	return "unknown";
}

inline const char* sheet_visibility_name(SheetVisibility visibility) {
	switch (visibility) {
		case SheetVisibility::Hidden: return "hidden";
		case SheetVisibility::VeryHidden: return "veryHidden";
		case SheetVisibility::Visible: break;
	}
	return "visible";
}

} // namespace singularity::documents
