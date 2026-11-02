#pragma once

#include <optional>
#include <string>

namespace singularity::documents {

// The ONE number formatter: what Excel (en-US) displays for a cell value
// under a number format code, at unlimited column width.
//
// Every surface that shows a workbook cell shows the text drawn here - the
// viewer never formats numbers a second time. It covers Excel's built-in
// formats (0-49 with their en-US meanings), custom codes with up to four
// sections (positive; negative; zero; text) and [conditions], section colours,
// quoted and escaped literals, "_x" spaces, the "*x" repeat fill, digit
// placeholders with thousands separators and trailing-comma scaling,
// percentages, scientific notation, simple fractions, dates and times
// (including elapsed [h]/[mm]/[ss], AM/PM and fractional seconds), currency
// and locale brackets, and Excel's "General".
//
// Excel's own arithmetic is reproduced where it shows: a value is first seen
// as its 15 significant digits and then rounded half away from zero (so 1.005
// under "0.00" is "1.01", as in Excel); the 1900 date system keeps Excel's
// fictional 29 February 1900 (serial 60) and the 1904 system starts at
// 1 January 1904. Nothing here throws: a code Excel would refuse draws the
// value as General.
struct FormattedCellValue {
	// The whole text: `fill_before` + `fill_after` when the format has a fill.
	std::string text;
	// Set only when the format has a "*x" repeat fill: what hugs the left edge
	// and what hugs the right edge of the cell.
	std::string fill_before;
	std::string fill_after;
	bool has_fill = false;
	// "RRGGBB" from the section's [Red]/[Blue]/[ColorN], "" otherwise.
	std::string color_hex;
	// The section that drew the value is a date or time section.
	bool is_date = false;
};

// A number under a format code. `date1904` is the workbook's date system.
[[nodiscard]] FormattedCellValue format_cell_value(double value, const std::string& format_code, bool date1904);

// Text under a format code: the code's text section ("@" and its literals) when
// it has one, the text itself otherwise.
[[nodiscard]] FormattedCellValue format_cell_text(const std::string& text, const std::string& format_code);

// The code of a built-in format id with its en-US meaning ("General" for an id
// Excel does not define). Locale-reserved ids map to the format Excel shows for
// them in en-US.
[[nodiscard]] std::string builtin_number_format(unsigned int format_id);

// Excel's standard 64-colour indexed palette ("RRGGBB"; "" past index 63, where
// 64 and 65 mean the system's own text and window colours). [ColorN] in a
// format code is entry N + 7; a workbook's indexed colours start from it.
[[nodiscard]] std::string excel_indexed_color(unsigned int index);

// The serial Excel stores for a calendar date and a time of day (seconds since
// midnight) under the workbook's date system - the inverse of how dates are
// drawn above, 1900's fictional 29 February included - or nothing for a date
// the system cannot hold.
[[nodiscard]] std::optional<double> excel_date_serial(int year, int month, int day, double seconds_of_day,
	bool date1904);

// Whether a code draws numbers as dates or times (judged by its first section).
[[nodiscard]] bool is_date_number_format(const std::string& format_code);

// ---- Typing, read the way Excel reads it -----------------------------------
//
// The inverse of the formatter, for what a person types into a cell: Excel
// (en-US) stores "7%" as 0.07, "$1,250,000" as 1250000 and "December 15, 2025"
// as the date's serial, and gives a General cell the format the typing was
// written in. A plain number ("0.07", "1250000") is not typing of this kind -
// it is stored as written - and text that is none of these stays text.

enum class TypedCellKind { Grouped, Percent, Currency, Date };

struct TypedCellValue {
	TypedCellKind kind = TypedCellKind::Grouped;
	// The value exactly as a workbook stores it: "0.075", "1250000", "46006".
	std::string stored;
	double number = 0.0;
	// The format Excel gives a General cell this is typed into, in the style it
	// was typed in: "0%", "0.0%", "\"$\"#,##0.00", "#,##0", "mmmm d, yyyy".
	std::string format_code;
	// The currency an amount was typed with, as it was written ("$", "€",
	// "USD"); "" for everything else.
	std::string currency;
};

// Grouped digits ("1,250,000"), a percentage ("7.5%"), an amount with its
// currency ("$1,250,000.00", "(£2,000)", "USD 250,000") or a whole calendar date
// ("12/15/2025", "Dec 15, 2025", "15 December 2025", "2025-12-15",
// "15.12.2025") - or nothing. A slash date is read month first, as Excel (en-US)
// reads it: "13/12/2025" is no date and stays text. Never more than Excel's 15
// significant digits.
[[nodiscard]] std::optional<TypedCellValue> read_typed_cell_value(const std::string& text, bool date1904);

// What a cell whose format is `format_code` makes of such typing: it shows it
// as what it is (a date under a date format, a percentage under a percentage
// format, an amount under any other number format), it has no format of its
// own yet (General) and takes the typing's, or it would show something else -
// a date format given an amount, a text format given anything. A format that
// shows only a time ("h:mm") would show a date as 0:00, so a date takes its
// own format there.
enum class TypedValueFit { Fits, TakesTypedFormat, Conflicts };

[[nodiscard]] TypedValueFit typed_value_fit(TypedCellKind kind, const std::string& format_code);

// The same for the typing itself, its currency included: an amount typed in
// one currency is never shown in another. "€1,250,000" in a cell that shows
// dollars - or that shows no currency at all - takes the format it was typed
// in, so the euros stay euros; "USD 250,000" in a dollar cell fits it.
[[nodiscard]] TypedValueFit typed_value_fit(const TypedCellValue& value, const std::string& format_code);

// Whether a code shows whatever is typed as text ("@"): Excel keeps typing in
// such a cell as the words typed, a date or an amount included.
[[nodiscard]] bool is_text_number_format(const std::string& format_code);

} // namespace singularity::documents
