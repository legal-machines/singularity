#pragma once

#include "PapinianGuard/Policy/Policy.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::protected_api {

// Labels for the two shapes found whenever protection is on, whatever
// categories are picked, like the names Papinian NER finds (see Policy.hpp):
// a Russian or English street address and the name written after «ИП».
inline constexpr char address_label[] = "address";
inline constexpr char person_label[] = "person";
// A financial ratio the monetary-amounts category protects ("Gross margin |
// 0.64 (shown as a percentage)", "interest at 3%"): found and governed with
// money, but typed as the rate it is, never as an amount.
inline constexpr char percentage_label[] = "percentages";
// A party's holding - its shares, units or membership interests in a company
// ("Seller | Shares" with "3,600" under it): a financial fact about that party,
// protected with money under Standard's monetary-amounts category, and typed
// as the plain quantity it is, never as an amount of money. Contract terms
// (thresholds, periods) are not holdings and stay out of it.
inline constexpr char holding_label[] = "holdings";

// A single span of `text` recognized as sensitive. `label` is a Category's
// to_string() value, address_label or person_label, or "custom_term" for a
// user-supplied term (see Policy::custom_terms) - transformer.cpp uses this
// to pick the placeholder prefix (AMOUNT_, EMAIL_, ... or TERM_).
struct Match {
  std::size_t start = 0;
  std::size_t length = 0;
  std::string label;
  std::string value;
};

// Finds every span of `text` that `policy` currently protects, with
// overlaps already resolved: custom terms always win, then an address or a
// name (the longer spans, so a number inside one never splits it), and
// within the built-in categories, all_categories()'s order wins. The result is sorted left-to-right and never contains overlapping
// spans - see detector.cpp for the resolution algorithm.
//
// Pure and side-effect free: no logging, no I/O, nothing retained after the
// call returns. Callers are responsible for not persisting `text` itself
// anywhere Protected API's guarantees would not otherwise cover.
std::vector<Match> detect(const std::string &text, const Policy &policy);

// What a number means where it stands, read from what is written around it -
// the words beside it on its own line, or in its own cell of a workbook row
// with that row's label and its column's header (the header row each passage
// of the workbook reading opens with) - and never from its length: "12" is an
// amount under "Purchase price ($m)" and a place in "Section 12". One reading,
// used by detect() to protect the numbers that mean money or an identifier
// and by Papinian Guard to judge the numbers its model tags. The cue words are
// data, in English and Russian (DeterministicDetector.cpp).
enum class NumberMeaning {
  // Nothing nearby says what it is.
  Unknown,
  // Structure or a plain scalar: a row, section or clause ("Row 10", "п. 9.4",
  // "ст. 401"), a length of time or a count of things ("12 months", "60 days",
  // "3 copies"), a list's number, a cell reference or a formula's constant, a
  // percentage under no financial label, a factor or a probability - and zero.
  Generic,
  // A date or a part of one: "15.03.2026", "10:30", the year in "15 марта 2026 г.".
  DatePart,
  // Money: a currency beside it ("$12", "USD 12", "12 рублей"), a scale ("12
  // million"), a money word before it ("purchase price of 12"), the workbook
  // reading's "(shown in $)", or a row label or column header naming money
  // ("Fees", "Стоимость услуг, ₽").
  Money,
  // A financial ratio a company keeps confidential: under a label naming a
  // margin, an ownership share or another financial measure ("Gross margin",
  // "Ownership %", "(shown as a percentage)" beside "Adjusted EBITDA margin").
  FinancialRatio,
  // A party's holding: under a label naming shares, units or membership
  // interests ("Shares", «Количество акций»), or a count of them in prose
  // ("3,600 shares of the Company").
  Holding,
  // The number of an identifier: after "№", "No." or "#" ("протокол № 7"), or
  // an account's, a passport's or a tax number's ("Account suffix 12").
  Identifier,
};

// The meaning of the number written at text[start, start + length).
NumberMeaning number_meaning(const std::string &text, std::size_t start, std::size_t length);

// Whether text[start, start + length) stands as the whole number it is: not
// the start of "20260", not the "0" of "0.64" or of "15.03.2026", not a group
// of "2 615 000", and not the number of a code or a reference ("B10", "E80",
// "СП-087", a phone's "+7"). A letter right after it is allowed: "12m" is 12.
bool stands_as_whole_number(const std::string &text, std::size_t start, std::size_t length);

// The span a number found at text[start, end) stands for: the whole number,
// and when it is written glued to letters, the code it is part of ("СП-087/26",
// "ИНН7710482917") - except a workbook's cell reference, which stays as found
// (number_meaning() reads it as structure). A detected span widened so is one
// the frozen pass finds again where it stands.
std::pair<std::size_t, std::size_t> detected_number_span(const std::string &text, std::size_t start, std::size_t end);

// The header of the column a position stands in, when it stands in a cell of a
// table as a document's reading writes one - a line per row, the cells joined
// by tabs - whose first row is a header ("Наименование<TAB>Итого, ₽"): where
// that header cell's text begins and ends. What a figure under «Итого, ₽» is
// counted in. Nothing outside such a table, and nothing for its header row.
std::optional<std::pair<std::size_t, std::size_t>> table_column_header(const std::string &text, std::size_t position);

// Whether a number's sign stands right before text[start]: "№", "#", "No.",
// "Nr.", "N" - "Договор № 15/03/2026", "Invoice No. 2026-03-15". What follows
// it is a document's number whatever its shape, never a date.
bool after_number_sign(const std::string &text, std::size_t start);

// Whether a word that places something in time stands right before
// text[start]: "от", "с", "до", "по", "не позднее", "on", "by", "dated",
// "from", "until", "as of", "effective". What makes "02.07" in «оплата от
// 02.07» a day and "3/4" in "3/4 of the price" not one.
bool after_date_cue(const std::string &text, std::size_t start);

// Whether text[start, start + length) is a day written without its year -
// "02.07" (day first), "7/2" (month first) - after a date cue: a real day and
// month, standing whole. Its month and day when it is.
std::optional<std::pair<int, int>> yearless_day(const std::string &text, std::size_t start, std::size_t length);

// Whether text[start, start + length) is a workbook's cell reference where it
// stands - "B10", "$B$3" in the workbook reading's own lines or inside a
// formula - rather than a code written the same way ("PO12345", "CV2026").
bool stands_as_cell_reference(const std::string &text, std::size_t start, std::size_t length);

// Whether text[start, start + length) names a place in a document or a length
// of time and nothing else - "Section 12", "подп. 10.3.2", "разделом 10
// Договора", "ст. 401 ГК РФ", "12 months" - every number in it structure.
bool names_only_structure(const std::string &text, std::size_t start, std::size_t length);

// The span a quantity whose number stands at text[start, end) is written
// with: its sign, a currency before or after it, a scale ("million", "млн"),
// the amount restated in words ("(восемнадцать миллионов) рублей"), and a
// percent sign or word ("3 %", "0,1 процента"). The number alone when nothing
// of that stands beside it.
std::pair<std::size_t, std::size_t> quantity_span(const std::string &text, std::size_t start, std::size_t end);

} // namespace singularity::protected_api
