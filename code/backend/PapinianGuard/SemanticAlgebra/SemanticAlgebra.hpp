#pragma once

#include "Context/TemporalSemantics/TemporalSemantics.hpp"
#include "Platform/Int128/Int128.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Protected values as typed values, worked out exactly on the lawyer's computer.
//
// Papinian Guard keeps a Project's amounts, rates and dates from a provider's
// model. The model reasons over symbols; what it needs to know about them -
// which amount is larger, whether a date falls inside a period, how much
// interest a delay costs - is worked out here, from the real values, and only
// the relation (or a new symbol for a derived value) ever leaves. Nothing here
// reads files, keeps state or talks to anyone.
//
// A value is read from the text it is written in ("18 400 000 (…
// …) … 00 …", "$2.8 million", "0,1 % …
// …", "July 2026", "B12 = 0.001 (shown as a percentage)"),
// and one value written many ways is one value: value_key is the same for
// every writing of it. Arithmetic is exact (fractions, never binary floating
// point); a date is the interval of days it names, so "July 2026" is neither
// before nor after "July 25, 2026" - that is Undetermined, with the reason,
// never a guess. A day written with no year ("each Lease Year begins on July
// 1") is a day of the year: it names no interval of days, so it is before or
// after nothing, and it meets a date only as the day of the year that date
// falls on (same_day_of_year) or as its next occurrence on or after one.
namespace singularity::protected_api::papinian::semantic {

namespace temporal = singularity::context::temporal;

// GCC's and Clang's __int128 where they have it; a portable equal elsewhere (MSVC).
using Int128 = singularity::platform::Int128;

// An expression or a value Singularity cannot work out. `kind` names the failure
// ("overflow", "division_by_zero", "currency", "type", "undetermined", ...);
// what() is the reason, written for the person or model that asked.
class AlgebraError : public std::runtime_error {
public:
	AlgebraError(std::string kind, const std::string &reason) : std::runtime_error(reason), kind_(std::move(kind)) {}
	[[nodiscard]] const std::string &kind() const { return kind_; }

private:
	std::string kind_;
};

// An exact fraction, always reduced, denominator positive. Anything that would
// not fit throws AlgebraError("overflow").
class Rational {
public:
	Rational() = default;
	// A whole number is a fraction. Int128, not long long: a power of ten past
	// 10^18 (a scale of 19 decimals) must not be cut to 64 bits on the way in.
	Rational(Int128 value) : numerator_(value) {}  // NOLINT
	// So is any builtin integer, in one step: where Int128 is a class (MSVC),
	// int to Int128 to Rational would be two conversions, which C++ refuses.
	template <typename Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
	Rational(Integer value) : numerator_(value) {}  // NOLINT
	Rational(Int128 numerator, Int128 denominator);

	// "-1234.5", "0.001", "42": digits with at most one '.', at most 30 digits.
	[[nodiscard]] static Rational from_decimal(std::string_view text);
	[[nodiscard]] static std::optional<Rational> parse_decimal(std::string_view text);

	[[nodiscard]] Int128 numerator() const { return numerator_; }
	[[nodiscard]] Int128 denominator() const { return denominator_; }
	[[nodiscard]] bool is_integer() const { return denominator_ == 1; }
	[[nodiscard]] bool is_zero() const { return numerator_ == 0; }
	[[nodiscard]] int sign() const { return numerator_ < 0 ? -1 : numerator_ > 0 ? 1 : 0; }
	[[nodiscard]] Rational abs() const;

	// To `places` decimals: half up, away from zero on an exact half.
	[[nodiscard]] Rational rounded(int places = 0) const;
	[[nodiscard]] Rational floored(int places = 0) const;
	[[nodiscard]] Rational ceiled(int places = 0) const;
	// Rounded to `places`, written with exactly `places` decimals: "-1250.00".
	[[nodiscard]] std::string to_decimal(int places) const;
	// The exact decimal when it has at most `max_places` decimals ("0.001").
	[[nodiscard]] std::optional<std::string> exact_decimal(int max_places) const;
	// "42", "-7/2".
	[[nodiscard]] std::string to_string() const;

	friend Rational operator+(const Rational &a, const Rational &b);
	friend Rational operator-(const Rational &a, const Rational &b);
	friend Rational operator*(const Rational &a, const Rational &b);
	friend Rational operator/(const Rational &a, const Rational &b);
	Rational operator-() const;
	friend bool operator==(const Rational &a, const Rational &b) { return a.numerator_ == b.numerator_ && a.denominator_ == b.denominator_; }
	friend bool operator!=(const Rational &a, const Rational &b) { return !(a == b); }
	friend bool operator<(const Rational &a, const Rational &b);
	friend bool operator>(const Rational &a, const Rational &b) { return b < a; }
	friend bool operator<=(const Rational &a, const Rational &b) { return !(b < a); }
	friend bool operator>=(const Rational &a, const Rational &b) { return !(a < b); }

private:
	Int128 numerator_ = 0;
	Int128 denominator_ = 1;
};

// Count: a whole non-negative count of rows, items or events, worked out by
// the protected computation protocol (DESIGN4 §2.4); nothing reads one from
// text.
enum class Kind { Money, Number, Percentage, Duration, Date, Boolean, DayOfYear, Count };
// Business days are read and counted, but never turned into calendar days or
// added to a date: that needs a holiday calendar Singularity does not have.
// Quarter, Hour and Minute are the protected computation protocol's own units
// (DESIGN4 §2.4); minutes, hours, days and weeks convert exactly.
enum class DurationUnit { Day, Week, Month, Year, BusinessDay, Quarter, Hour, Minute };
enum class RateBasis { None, PerDay, PerMonth, PerYear };
enum class Language { English, Russian };

struct Value {
	Kind kind = Kind::Number;
	// Money and Number: the amount. Percentage: the fraction (0.1 % is 1/1000).
	// Duration: the count of `unit`.
	Rational amount;
	std::string currency;            // Money only: USD EUR GBP JPY RUB ...; "" when unknown
	DurationUnit unit = DurationUnit::Day;
	// Percentage: the period the rate is for (0.1 % per day). Money: the period
	// an amount is owed for, as a daily rate makes it ($1,000 * 0.1 % per day
	// is $1 per day). Over a period the basis is spent: $1 per day for 45 days
	// is $45.
	RateBasis basis = RateBasis::None;
	temporal::TemporalValue date;    // Date: the interval of days and its precision
	temporal::DayOfYear day_of_year; // DayOfYear: the month and the day, no year
	bool truth = false;              // Boolean
	int source_decimals = 0;         // display hint: 2 when the source wrote cents
	std::string date_style;          // display hint: "english" "russian" "dotted" "iso" "slashed"
};

[[nodiscard]] Value make_money(Rational amount, std::string currency, int source_decimals = 0);
[[nodiscard]] Value make_number(Rational amount);
[[nodiscard]] Value make_percentage(Rational fraction, RateBasis basis = RateBasis::None);
[[nodiscard]] Value make_duration(Rational count, DurationUnit unit);
[[nodiscard]] Value make_date(const temporal::TemporalValue &date, std::string style = "english");
[[nodiscard]] Value make_boolean(bool truth);
[[nodiscard]] Value make_day_of_year(unsigned month, unsigned day, std::string style = "english");

// The value's identity, the same for every writing of it: "MONEY:RUB:18400000",
// "MONEY:?:1500.5" (currency unknown), "PCT:1/1000" (the basis is not part of
// it: 0,1 % in a clause and 0.001 in a cell are one rate), "NUM:42",
// "DUR:Day:45", "DATE:D:20260210", "DATE:M:202607", "DATE:Q:2026Q3",
// "DATE:Y:2026", "BOOL:true", "DOY:0701" (July 1, no year: never the key of
// any July 1 that has one).
[[nodiscard]] std::string value_key(const Value &value);

// What a caller knows about a bare cell: "1183452.05" is rubles when the
// column is, 0.001 is a rate when the cell is shown as a percentage.
struct ParseHints {
	std::optional<Kind> expect;
	std::string currency;
	bool percent_display = false;
};

// The one value a protected span writes, or nothing when the text is not
// wholly one value. "July 1" and «…» are a day of the year.
[[nodiscard]] std::optional<Value> parse_value(std::string_view text, const ParseHints &hints = {});

// The period a rate written at text[start, end) is for, as its own clause
// says - after it to the clause's end ("5% interest per annum", "8% annual
// interest", "0.1% of the unpaid amount for each day of delay", «0,1 % …
// …», «…»), else before
// it ("an annual rate of 8%"). One vocabulary with parse_value(). A word that
// may name a year of something else ("annual", "a day", «…») counts only
// beside the rate; another rate written in figures ends the search. None when
// the clause names no period.
[[nodiscard]] RateBasis rate_basis_of_clause(std::string_view text, std::size_t start, std::size_t end);

// The currency a label names, as its code: "Amount ($)" USD, «…» RUB,
// «…» RUB, "Fee (EUR)" EUR, "Price, US dollars" USD; "" when it names
// none. What a table's column header says its figures are counted in.
[[nodiscard]] std::string currency_named(std::string_view label);

// The measure a label names for the bare figure beside or under it: «…
// …», "Days overdue", «…» a count of days (business days
// with «…» / "business"); months, hours and minutes likewise; «…»,
// "Number of", "Quantity" a count. Strict, so money keeps its label: only the
// plural of a unit ("per day", «…» are a rate's), never a label with a
// figure ("Late fee (after 30 days)") or one naming a currency («…
// …, ₽»). Nothing when it names none.
struct NamedMeasure {
	Kind kind = Kind::Duration;              // Duration or Count
	DurationUnit unit = DurationUnit::Day;   // a Duration's unit
};
[[nodiscard]] std::optional<NamedMeasure> measure_named(std::string_view label);

// A whole number written in English or Russian words, in any case:
// "one hundred and five", "twenty-one thousand", "…
[[nodiscard]] std::optional<Rational> parse_number_words(std::string_view text);

enum class Truth { True, False, Undetermined };
enum class Comparator { Less, LessOrEqual, Greater, GreaterOrEqual, Equal, NotEqual };

// A relation and, when it is Undetermined, why - naming the values as the
// caller wrote them (`written_a`, `written_b`; the values' own display when empty).
struct Relation {
	Truth truth = Truth::Undetermined;
	std::string reason;
};

// Quantities of one kind (Money in one currency; a Percentage against a Number
// as its fraction; rates for different periods as rates for a year, a year
// being 365 or 366 days: 0.1 % per day is more than 18 % per annum;
// Durations in convertible units - days and weeks, months and years - or
// bounded: 45 days is more than a month, 30 days is Undetermined against it),
// dates as intervals, Booleans for equality. Throws AlgebraError
// ("type" or "currency") for values that do not compare.
[[nodiscard]] Relation compare(const Value &a, Comparator comparator, const Value &b, Language language = Language::English,
	std::string_view written_a = {}, std::string_view written_b = {});

// Dates as intervals of days: before iff every day of `a` precedes every day
// of `b`; same_day only for one day; within/overlaps as periods.
[[nodiscard]] Relation before(const Value &a, const Value &b, Language language = Language::English,
	std::string_view written_a = {}, std::string_view written_b = {});
[[nodiscard]] Relation after(const Value &a, const Value &b, Language language = Language::English,
	std::string_view written_a = {}, std::string_view written_b = {});
[[nodiscard]] Relation same_day(const Value &a, const Value &b, Language language = Language::English,
	std::string_view written_a = {}, std::string_view written_b = {});
// `date` inside `period` (every day of it), or inside [start of `from` .. end of `to`].
[[nodiscard]] Relation within(const Value &date, const Value &period, Language language = Language::English,
	std::string_view written_date = {}, std::string_view written_period = {});
[[nodiscard]] Relation within(const Value &date, const Value &from, const Value &to, Language language = Language::English,
	std::string_view written_date = {});
[[nodiscard]] Relation overlaps(const Value &a, const Value &b);

// Whether two values fall on the same day of the year - a day of the year, or a
// date whose day that is ("July 1" and "July 1, 2025"; a month holding July 1
// leaves it Undetermined). Throws AlgebraError("type") for anything else.
[[nodiscard]] Relation same_day_of_year(const Value &a, const Value &b, Language language = Language::English,
	std::string_view written_a = {}, std::string_view written_b = {});
// The first day on or after `on_or_after` that is `day_of_year` (a day of the
// year, or a date whose day of the year is taken): July 1 on or after
// "September 8, 2026" is July 1, 2027, and a February 29 waits for a leap year.
// Throws AlgebraError("undetermined") when the date is a period across which
// the answer changes, ("type") for values of another kind.
[[nodiscard]] Value next_occurrence(const Value &day_of_year, const Value &on_or_after, Language language = Language::English,
	std::string_view written_day = {}, std::string_view written_date = {});

// The first or the last day of the period of `unit` - Month, Quarter or Year
// - that holds `date` (DESIGN4 §7.1): September 2026 starts on September 1,
// 2026; Q3 2024 ends on September 30, 2024. With an `anchor` (a date, or a day
// of the year such as "July 1") the periods are counted from its day: a lease
// year that begins on July 1 starts, for March 3, 2027, on July 1, 2026; a
// month counted from the 15th ends on the 14th. A day of the month the period's
// month does not have is its last day. Throws AlgebraError("undetermined")
// when the date's days fall in more than one such period (2026, asked for its
// month) or the anchor names no day; ("type") for another kind or unit.
[[nodiscard]] Value start_of(const Value &date, DurationUnit unit, const std::optional<Value> &anchor = std::nullopt);
[[nodiscard]] Value end_of(const Value &date, DurationUnit unit, const std::optional<Value> &anchor = std::nullopt);
// How many days a date's interval holds, as days: 30 for September 2026, 92
// for Q3, 1 for a day. Throws AlgebraError("type") for another kind.
[[nodiscard]] Value days_in(const Value &date);

// How two dates stand as intervals, for a timeline of a request's dates.
enum class DateRelation { Before, After, SameDay, SamePeriod, Contains, Within, Overlaps };
[[nodiscard]] DateRelation relate_dates(const temporal::TemporalValue &a, const temporal::TemporalValue &b);

// How a result is written, from a statement's `as ...`: money, $, €, £, ¥, ₽,
// …, USD, EUR, GBP, JPY, RUB; percent; number; integer; days,
// weeks, months, years; date; yes/no; words (alone or with a currency).
enum class FormatStyle { Default, Money, Percent, Number, Integer, Days, Weeks, Months, Years, Date, YesNo };
struct Format {
	FormatStyle style = FormatStyle::Default;
	std::string currency;  // asked currency code, "" for the value's own
	std::string mark;      // how it is written when asked: "$", "…", "USD"
	bool words = false;
};
[[nodiscard]] std::optional<Format> parse_format(std::string_view text);

// The value as the lawyer reads it: EN "$21,350.00", "0.1%", "July 31, 2026",
// "45 days"; RU "828 000 ₽", "1 183 452,05 ₽" (no-break grouping, decimal
// comma), "0,1 %", "…"; a dotted source date stays
// dotted. Throws AlgebraError when the format does not fit the value.
[[nodiscard]] std::string display(const Value &value, const Format &format = {}, Language language = Language::English);

// How many places an amount in `currency` is paid to: 2 (cents, kopecks,
// pence), 0 for a currency with no minor unit (JPY); 2 when it is unknown.
[[nodiscard]] int minor_unit_places(std::string_view currency);

// A whole number in words: "twenty-one thousand three hundred fifty",
// "…" (`feminine` for a feminine noun: …).
[[nodiscard]] std::string number_in_words(Int128 number, Language language, bool feminine = false);

// The Russian noun form a count takes: 1 …, 2 …, 5 …, 21 …, 111 …
[[nodiscard]] std::string russian_plural(Int128 count, std::string_view one, std::string_view few, std::string_view many);

// Russian when Cyrillic letters outnumber Latin ones.
[[nodiscard]] Language language_of(std::string_view text);

// A rate times a period counts the period in the rate's units (0.1 % per day
// for 2 weeks is 1.4 %; per annum for 6 months is half) and refuses what is
// not exact (per day for 2 months; per annum for 45 days, where the contract
// says how many days make the year). A rate scaled by a plain number keeps its
// period - 0.1 % per day * 2 is 0.2 % per day, never a lump rate that months
// could multiply - unless the number counts its periods in another: 18 % per
// annum / 365 is 0.05 % per day, / 12 is per month; and a plain count in a
// product divided by a year ("rate * 45 / 365") is the days, a lump rate.
// Chains of any length are fine; nesting deeper than 32 levels is refused.
//
// An expression over typed values (DESIGN §1.6): + - * / ( ), %, comparisons,
// and / or / not (short-circuit), numbers with a dot, periods ("30 days",
// "12 months"), ⟦operand⟧ (a restored protected value), and min, max, sum,
// avg, abs, round, floor, ceil, if (lazy), days_between, months_between,
// add_days, add_months, add_years, start, end, within, before, after,
// same_day, overlaps, clamp, same_day_of_year, next_occurrence.
struct EvaluationError {
	std::string kind;
	std::string reason;  // names the operands as the expression writes them
};
struct Evaluation {
	std::optional<Value> value;
	std::optional<EvaluationError> error;
};
[[nodiscard]] Evaluation evaluate(const std::string &expression, Language language = Language::English);

// ---------------------------------------------------------------------------
// Exact numbers of any size, for the protected computation protocol (DESIGN4
// §3.2): nothing it works out can overflow, so whether an operation overflows
// never depends on a hidden value. Rational (Int128) stays for every existing
// caller. Declared by the seam; worked out in SemanticAlgebra.cpp (lane C).

// An integer of any size: base 2^32 limbs, least significant first, no
// leading zero limb; zero has none.
class BigInt {
public:
	BigInt() = default;
	BigInt(long long value);  // NOLINT
	[[nodiscard]] static BigInt from_int128(Int128 value);
	// Decimal digits with an optional leading '-'.
	[[nodiscard]] static std::optional<BigInt> parse_decimal(std::string_view text);

	[[nodiscard]] bool is_zero() const { return limbs_.empty(); }
	[[nodiscard]] int sign() const { return limbs_.empty() ? 0 : negative_ ? -1 : 1; }
	[[nodiscard]] std::size_t bit_length() const;
	[[nodiscard]] std::optional<Int128> to_int128() const;
	[[nodiscard]] std::string to_string() const;
	[[nodiscard]] BigInt abs() const;
	BigInt operator-() const;

	friend BigInt operator+(const BigInt &a, const BigInt &b);
	friend BigInt operator-(const BigInt &a, const BigInt &b);
	friend BigInt operator*(const BigInt &a, const BigInt &b);
	// Truncating division; throws AlgebraError("division_by_zero").
	static void divide(const BigInt &numerator, const BigInt &denominator, BigInt &quotient, BigInt &remainder);
	friend BigInt operator/(const BigInt &a, const BigInt &b);
	friend BigInt operator%(const BigInt &a, const BigInt &b);
	[[nodiscard]] static BigInt gcd(const BigInt &a, const BigInt &b);
	// -1, 0 or 1.
	[[nodiscard]] static int compare(const BigInt &a, const BigInt &b);
	friend bool operator==(const BigInt &a, const BigInt &b) { return compare(a, b) == 0; }
	friend bool operator!=(const BigInt &a, const BigInt &b) { return compare(a, b) != 0; }
	friend bool operator<(const BigInt &a, const BigInt &b) { return compare(a, b) < 0; }
	friend bool operator>(const BigInt &a, const BigInt &b) { return compare(a, b) > 0; }
	friend bool operator<=(const BigInt &a, const BigInt &b) { return compare(a, b) <= 0; }
	friend bool operator>=(const BigInt &a, const BigInt &b) { return compare(a, b) >= 0; }

private:
	bool negative_ = false;
	std::vector<std::uint32_t> limbs_;
};

// An exact fraction of any size, always reduced, denominator positive.
class ExactRational {
public:
	enum class Rounding { HalfUp, HalfEven, Down, Up };

	ExactRational() = default;
	ExactRational(const BigInt &integer);  // NOLINT
	// Throws AlgebraError("division_by_zero") for a zero denominator.
	ExactRational(const BigInt &numerator, const BigInt &denominator);
	[[nodiscard]] static ExactRational from(const Rational &value);
	[[nodiscard]] static std::optional<ExactRational> parse_decimal(std::string_view text);

	[[nodiscard]] const BigInt &numerator() const { return numerator_; }
	[[nodiscard]] const BigInt &denominator() const { return denominator_; }
	[[nodiscard]] bool is_zero() const { return numerator_.is_zero(); }
	[[nodiscard]] bool is_integer() const;
	[[nodiscard]] int sign() const { return numerator_.sign(); }
	// Numerator and denominator bit lengths together (DESIGN4 §3.2).
	[[nodiscard]] std::size_t bits() const;
	[[nodiscard]] ExactRational abs() const;
	[[nodiscard]] ExactRational rounded(int places, Rounding rounding = Rounding::HalfUp) const;
	// As a Rational when it fits; nothing otherwise.
	[[nodiscard]] std::optional<Rational> to_rational() const;
	[[nodiscard]] std::optional<std::string> exact_decimal(int max_places) const;
	[[nodiscard]] std::string to_decimal(int places, Rounding rounding = Rounding::HalfUp) const;
	// "42", "-7/2".
	[[nodiscard]] std::string to_string() const;

	friend ExactRational operator+(const ExactRational &a, const ExactRational &b);
	friend ExactRational operator-(const ExactRational &a, const ExactRational &b);
	friend ExactRational operator*(const ExactRational &a, const ExactRational &b);
	friend ExactRational operator/(const ExactRational &a, const ExactRational &b);
	ExactRational operator-() const;
	[[nodiscard]] static int compare(const ExactRational &a, const ExactRational &b);
	friend bool operator==(const ExactRational &a, const ExactRational &b) { return compare(a, b) == 0; }
	friend bool operator!=(const ExactRational &a, const ExactRational &b) { return compare(a, b) != 0; }
	friend bool operator<(const ExactRational &a, const ExactRational &b) { return compare(a, b) < 0; }
	friend bool operator>(const ExactRational &a, const ExactRational &b) { return compare(a, b) > 0; }
	friend bool operator<=(const ExactRational &a, const ExactRational &b) { return compare(a, b) <= 0; }
	friend bool operator>=(const ExactRational &a, const ExactRational &b) { return compare(a, b) >= 0; }

private:
	BigInt numerator_;
	BigInt denominator_{1};
};

// One figure a text writes (DESIGN4 §8.2): digits with or without separators,
// number words in English or Russian, percentages, durations with units,
// amounts with currency signs, codes or words. `structural` marks what is
// never a figure of the matter: a clause, section or article number, a list
// enumerator, a row number, a DOC id, a cell reference or function code, a
// date's own digits.
struct RecognisedFigure {
	enum class Kind { Money, Percentage, Duration, Number, Date, Words };
	std::size_t start = 0;
	std::size_t length = 0;
	Kind kind = Kind::Number;
	std::optional<Value> value;
	bool structural = false;
	// A number word under ten standing alone in prose, with no unit, currency
	// or percent ("one of the parties", «…»): still a figure, but
	// one plain prose writes as a word.
	bool incidental = false;
};

// The one recogniser both sides use: the grounding index (what the lawyer's
// material writes openly) and the answer contract's raw-figure check.
class FigureRecogniser {
public:
	[[nodiscard]] static std::vector<RecognisedFigure> find(std::string_view text, Language language = Language::English);
};

} // namespace singularity::protected_api::papinian::semantic
