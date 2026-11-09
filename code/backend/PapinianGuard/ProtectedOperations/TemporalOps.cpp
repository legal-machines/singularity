#include "PapinianGuard/ProtectedOperations/TemporalOps.hpp"

#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

// The temporal family (DESIGN4 §2.5, §7): each op a typer (declared types
// only, never a value), an evaluator (over the real values, on this computer)
// and a pre-charge (what an answer that can depend on a value costs, decided
// before it is worked out). Dates are intervals of days: an answer that
// depends on where in an interval a day falls is UNKNOWN precision_insufficient
// or a BOUND, never a guess; a business day is counted under the rule that
// governs it, and an uncertain calendar never gives an exact day (C3 §2).
//
// Link rule (SEAM §0.1): this file calls only the protocol's own sources,
// SemanticAlgebra and BusinessCalendar; it reaches the operation's table only
// through OpContext and the table's fields.
namespace singularity::protected_api::papinian::compute {

namespace {

namespace calendar = singularity::context::business_calendar;
namespace temporal = singularity::context::temporal;
using semantic::DurationUnit;
using semantic::Value;

// ---------------------------------------------------------------------------
// Static types.

int precision_rank(const std::string &precision) {
	if (precision == "day") return 0;
	if (precision == "month") return 1;
	if (precision == "quarter") return 2;
	if (precision == "year") return 3;
	return 4;  // range: no period of its own
}

bool is_date(const ArgType &type) { return type.kind == TypeKind::Date && type.precision != "day of year"; }
bool is_day(const ArgType &type) { return type.kind == TypeKind::Date && (type.precision == "day" || type.precision.empty()); }
bool is_day_of_year(const ArgType &type) { return type.kind == TypeKind::Date && type.precision == "day of year"; }

// A day of the year ("each July 1") has no year: it meets a date through the
// day it next falls on (a fake-provider run on E1 was told only "not evaluable").
constexpr const char *kDayOfYearHint = "a day of the year has no year: compare the day it next falls on - "
	"next_occurrence(that day, a date) - with the date";
bool is_calendar(const ArgType &type) { return type.kind == TypeKind::Calendar; }
// A period: a duration, or a plain count whose unit options.unit gives.
bool is_period(const ArgType &type, const OpOptions &options) {
	return type.kind == TypeKind::Duration
		|| ((type.kind == TypeKind::Number || type.kind == TypeKind::Count) && unit_named(options.unit).has_value());
}

DurationUnit period_unit(const ArgType &type, const OpOptions &options) {
	if (type.kind == TypeKind::Duration) return type.unit;
	return unit_named(options.unit).value_or(DurationUnit::Day);
}

bool month_class(DurationUnit unit) {
	return unit == DurationUnit::Month || unit == DurationUnit::Quarter || unit == DurationUnit::Year;
}

bool any_anchored(const std::vector<ArgType> &arguments) {
	return std::any_of(arguments.begin(), arguments.end(), [](const ArgType &type) { return type.calendar_anchored; });
}

ArgType date_type(const std::string &precision, bool anchored) {
	ArgType type;
	type.kind = TypeKind::Date;
	type.precision = precision;
	type.calendar_anchored = anchored;
	return type;
}

ArgType duration_type(DurationUnit unit, bool anchored) {
	ArgType type;
	type.kind = TypeKind::Duration;
	type.unit = unit;
	type.calendar_anchored = anchored;
	return type;
}

ResultType relation_result(TypeKind kind, bool anchored) {
	ResultType result;
	result.type.kind = kind;
	result.type.calendar_anchored = anchored;
	result.relation = true;
	return result;
}

ResultType value_result(ArgType type) {
	ResultType result;
	result.type = std::move(type);
	return result;
}

std::optional<ResultType> mismatch(TypeError &error, std::size_t argument, const std::string &expected, const ArgType &got) {
	error.code = Code::TypeMismatch;
	error.argument = argument;
	error.expected = expected;
	error.got = written(got);
	return std::nullopt;
}

std::optional<ResultType> arity(TypeError &error) {
	error.code = Code::Arity;
	error.argument = 0;
	return std::nullopt;
}

std::optional<ResultType> needs_unit(TypeError &error) {
	error.code = Code::UnitRequired;
	error.argument = 0;
	return std::nullopt;
}

// The optional trailing calendar operand at `at`, when there is one.
bool trailing_calendar(const std::vector<ArgType> &arguments, std::size_t at, TypeError &error, bool &failed) {
	if (arguments.size() <= at) return false;
	if (!is_calendar(arguments[at])) {
		failed = true;
		mismatch(error, at + 1, "CALENDAR", arguments[at]);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Typers (DESIGN4 §2.5, temporal rows).

std::optional<ResultType> type_order(const std::vector<ArgType> &arguments, const OpOptions &, TypeError &error) {
	if (arguments.size() != 2) return arity(error);
	if (is_day_of_year(arguments[0]) != is_day_of_year(arguments[1])) {
		const std::size_t which = is_day_of_year(arguments[0]) ? 1 : 2;
		mismatch(error, which, "DATE(day)", arguments[which - 1]);
		error.hint = kDayOfYearHint;
		return std::nullopt;
	}
	for (std::size_t index = 0; index < 2; ++index)
		if (!is_date(arguments[index])) return mismatch(error, index + 1, "DATE", arguments[index]);
	return relation_result(TypeKind::Boolean, any_anchored(arguments));
}

std::optional<ResultType> type_same_time(const std::vector<ArgType> &arguments, const OpOptions &, TypeError &error) {
	if (arguments.size() != 2) return arity(error);
	for (std::size_t index = 0; index < 2; ++index)
		if (!is_day(arguments[index])) return mismatch(error, index + 1, "DATE(day)", arguments[index]);
	return relation_result(TypeKind::Boolean, any_anchored(arguments));
}

// within(d, period) · within(d, start, end) · within(d, anchor, duration[, calendar]); outside alike.
std::optional<ResultType> type_within(const std::vector<ArgType> &arguments, const OpOptions &options, TypeError &error) {
	if (arguments.size() < 2 || arguments.size() > 4) return arity(error);
	if (!is_date(arguments[0])) return mismatch(error, 1, "DATE", arguments[0]);
	if (!is_date(arguments[1])) return mismatch(error, 2, "DATE", arguments[1]);
	bool anchored = any_anchored(arguments);
	if (arguments.size() >= 3) {
		if (is_period(arguments[2], options)) {
			const DurationUnit unit = period_unit(arguments[2], options);
			if (unit == DurationUnit::Hour || unit == DurationUnit::Minute)
				return mismatch(error, 3, "DURATION(days, weeks, months, quarters, years or business days)", arguments[2]);
			if (unit == DurationUnit::BusinessDay && !is_day(arguments[1])) return mismatch(error, 2, "DATE(day)", arguments[1]);
			anchored = anchored || month_class(unit) || unit == DurationUnit::BusinessDay;
			bool failed = false;
			trailing_calendar(arguments, 3, error, failed);
			if (failed) return std::nullopt;
		} else if (is_date(arguments[2])) {
			if (arguments.size() == 4) return arity(error);
		} else {
			return mismatch(error, 3, "DATE or DURATION", arguments[2]);
		}
	}
	return relation_result(TypeKind::Boolean, anchored);
}

std::optional<ResultType> type_duration_between(const std::vector<ArgType> &arguments, const OpOptions &options, TypeError &error) {
	if (arguments.size() < 2 || arguments.size() > 3) return arity(error);
	for (std::size_t index = 0; index < 2; ++index)
		if (!is_date(arguments[index])) return mismatch(error, index + 1, "DATE", arguments[index]);
	bool failed = false;
	trailing_calendar(arguments, 2, error, failed);
	if (failed) return std::nullopt;
	const auto unit = unit_named(options.unit);
	if (!unit) return needs_unit(error);
	if (*unit == DurationUnit::BusinessDay)
		for (std::size_t index = 0; index < 2; ++index)
			if (!is_day(arguments[index])) return mismatch(error, index + 1, "DATE(day)", arguments[index]);
	const bool anchored = any_anchored(arguments) || month_class(*unit) || *unit == DurationUnit::BusinessDay;
	return value_result(duration_type(*unit, anchored));
}

// The precision of a date moved by a period (DESIGN4 §7.1): a day stays a
// day; a month moved by months stays a month, a quarter by quarters, a year
// by years; anything else is the shifted interval, a range.
std::string shifted_precision(const std::string &precision, DurationUnit unit) {
	if (precision == "day" || precision.empty()) return "day";
	if (precision == "month" && month_class(unit)) return "month";
	if (precision == "quarter" && (unit == DurationUnit::Quarter || unit == DurationUnit::Year)) return "quarter";
	if (precision == "year" && unit == DurationUnit::Year) return "year";
	return "range";
}

std::optional<ResultType> type_add_duration(const std::vector<ArgType> &arguments, const OpOptions &options, TypeError &error) {
	if (arguments.size() < 2 || arguments.size() > 3) return arity(error);
	if (!is_date(arguments[0])) return mismatch(error, 1, "DATE", arguments[0]);
	if (!is_period(arguments[1], options)) {
		if (arguments[1].kind == TypeKind::Number || arguments[1].kind == TypeKind::Count) return needs_unit(error);
		return mismatch(error, 2, "DURATION", arguments[1]);
	}
	const DurationUnit unit = period_unit(arguments[1], options);
	if (unit == DurationUnit::Hour || unit == DurationUnit::Minute)
		return mismatch(error, 2, "DURATION(days, weeks, months, quarters, years or business days)", arguments[1]);
	if (unit == DurationUnit::BusinessDay && !is_day(arguments[0])) return mismatch(error, 1, "DATE(day)", arguments[0]);
	bool failed = false;
	trailing_calendar(arguments, 2, error, failed);
	if (failed) return std::nullopt;
	const bool anchored = any_anchored(arguments) || month_class(unit) || unit == DurationUnit::BusinessDay;
	return value_result(date_type(shifted_precision(arguments[0].precision, unit), anchored));
}

std::optional<ResultType> type_deadline(const std::vector<ArgType> &arguments, const OpOptions &options, TypeError &error) {
	if (arguments.size() < 2 || arguments.size() > 3) return arity(error);
	if (!is_day(arguments[0])) return mismatch(error, 1, "DATE(day)", arguments[0]);
	if (!is_period(arguments[1], options)) {
		if (arguments[1].kind == TypeKind::Number || arguments[1].kind == TypeKind::Count) return needs_unit(error);
		return mismatch(error, 2, "DURATION", arguments[1]);
	}
	const DurationUnit unit = period_unit(arguments[1], options);
	if (unit == DurationUnit::Hour || unit == DurationUnit::Minute)
		return mismatch(error, 2, "DURATION(days, weeks, months, quarters, years or business days)", arguments[1]);
	bool failed = false;
	const bool given = trailing_calendar(arguments, 2, error, failed);
	if (failed) return std::nullopt;
	if (!given && !options.calendar) {
		error.code = Code::CalendarRequired;
		error.argument = 3;
		error.expected = "CALENDAR";
		return std::nullopt;
	}
	return value_result(date_type("day", true));
}

std::optional<ResultType> type_period_end(const std::vector<ArgType> &arguments, const OpOptions &options, TypeError &error) {
	if (arguments.size() != 1) return arity(error);
	if (!is_date(arguments[0])) return mismatch(error, 1, "DATE", arguments[0]);
	const auto unit = unit_named(options.unit);
	if (!unit) return needs_unit(error);
	const int rank = *unit == DurationUnit::Month ? 1 : *unit == DurationUnit::Quarter ? 2 : *unit == DurationUnit::Year ? 3 : -1;
	if (rank < 0) {
		error.code = Code::TypeMismatch;
		error.argument = 0;
		error.expected = "unit month, quarter or year";
		error.got = options.unit.value_or("");
		return std::nullopt;
	}
	// A period holds its date only when the date is no coarser than it; an
	// anchored period can hold a coarser date only by where it falls.
	if (!options.anchor && precision_rank(arguments[0].precision) > rank)
		return mismatch(error, 1, "DATE(" + options.unit.value_or("") + " or finer)", arguments[0]);
	return value_result(date_type("day", true));
}

std::optional<ResultType> type_days_in(const std::vector<ArgType> &arguments, const OpOptions &, TypeError &error) {
	if (arguments.size() != 1) return arity(error);
	if (!is_date(arguments[0])) return mismatch(error, 1, "DATE", arguments[0]);
	return value_result(duration_type(DurationUnit::Day, true));
}

std::optional<ResultType> type_next_business_day(const std::vector<ArgType> &arguments, const OpOptions &, TypeError &error) {
	if (arguments.empty() || arguments.size() > 2) return arity(error);
	if (!is_day(arguments[0])) return mismatch(error, 1, "DATE(day)", arguments[0]);
	bool failed = false;
	trailing_calendar(arguments, 1, error, failed);
	if (failed) return std::nullopt;
	return value_result(date_type("day", true));
}

std::optional<ResultType> type_day_class(const std::vector<ArgType> &arguments, const OpOptions &, TypeError &error) {
	if (arguments.empty() || arguments.size() > 2) return arity(error);
	if (!is_day(arguments[0])) return mismatch(error, 1, "DATE(day)", arguments[0]);
	bool failed = false;
	trailing_calendar(arguments, 1, error, failed);
	if (failed) return std::nullopt;
	return relation_result(TypeKind::Word, true);
}

std::optional<ResultType> type_next_occurrence(const std::vector<ArgType> &arguments, const OpOptions &, TypeError &error) {
	if (arguments.size() != 2) return arity(error);
	if (!is_day_of_year(arguments[0]) && !is_day(arguments[0])) return mismatch(error, 1, "DATE(day of year)", arguments[0]);
	if (!is_date(arguments[1])) return mismatch(error, 2, "DATE", arguments[1]);
	return value_result(date_type("day", true));
}

// ---------------------------------------------------------------------------
// Evaluation helpers.

std::vector<ArgType> types_of(const std::vector<Operand> &operands) {
	std::vector<ArgType> types;
	types.reserve(operands.size());
	for (const Operand &operand : operands) types.push_back(operand.type);
	return types;
}

OpAnswer failed(Code code, std::size_t argument = 0, const std::string &expected = {}, const std::string &got = {}) {
	OpAnswer answer;
	answer.status = OpStatus::Error;
	answer.code = code;
	answer.error.code = code;
	answer.error.argument = argument;
	answer.error.expected = expected;
	answer.error.got = got;
	return answer;
}

OpAnswer unknown(Code reason, const OpAnswer *from = nullptr) {
	OpAnswer answer;
	if (from != nullptr) {
		answer.result = from->result;
		answer.roots = from->roots;
		answer.record = from->record;
		answer.words = from->words;
	}
	answer.status = OpStatus::Unknown;
	answer.code = reason;
	answer.certainty.kind = Certainty::Kind::Unknown;
	answer.certainty.reason = reason;
	return answer;
}

void add_words(OpAnswer &answer, const std::string &word) {
	if (word.empty()) return;
	for (std::size_t at = 0; at < answer.words.size();) {
		const std::size_t end = std::min(answer.words.find(' ', at), answer.words.size());
		if (answer.words.compare(at, end - at, word) == 0) return;
		at = end + 1;
	}
	answer.words += (answer.words.empty() ? "" : " ") + word;
}

// Every operand's roots, and the type the typer gives.
bool prepare(OpAnswer &answer, const std::vector<Operand> &operands, const OpOptions &options,
		std::optional<ResultType> (*typer)(const std::vector<ArgType> &, const OpOptions &, TypeError &)) {
	TypeError error;
	const auto result = typer(types_of(operands), options, error);
	if (!result) {
		answer = failed(error.code, error.argument, error.expected, error.got);
		answer.error = error;
		return false;
	}
	answer.result = *result;
	for (const Operand &operand : operands) answer.roots.insert(operand.roots.begin(), operand.roots.end());
	return true;
}

// The value a date operand stands for, or nothing.
const Value *value_of(const Operand &operand) {
	return operand.value ? &*operand.value : nullptr;
}

Value day_value(long long day, const Value &like) {
	return semantic::make_date(temporal::TemporalValue{day, day, temporal::Precision::Day},
		like.date_style.empty() ? std::string("english") : like.date_style);
}

OpAnswer exact_value(OpAnswer answer, Value value) {
	answer.status = OpStatus::Ok;
	answer.value = std::move(value);
	answer.certainty.kind = Certainty::Kind::Exact;
	return answer;
}

OpAnswer bounded(OpAnswer answer, std::optional<Value> lo, std::optional<Value> hi, const std::string &why) {
	answer.status = OpStatus::Bounded;
	answer.lo = std::move(lo);
	answer.hi = std::move(hi);
	answer.certainty.kind = Certainty::Kind::Bounded;
	add_words(answer, why);
	if (!answer.hi) add_words(answer, kWordNoLatestDay);
	if (!answer.lo) add_words(answer, kWordNoEarliestDay);
	return answer;
}

OpAnswer truth(OpAnswer answer, semantic::Truth holds) {
	if (holds == semantic::Truth::Undetermined) {
		OpAnswer unsure = unknown(Code::PrecisionInsufficient);
		unsure.result = answer.result;
		unsure.roots = answer.roots;
		return unsure;
	}
	answer.status = OpStatus::Ok;
	answer.relation = holds == semantic::Truth::True ? "TRUE" : "FALSE";
	answer.certainty.kind = Certainty::Kind::Exact;
	return answer;
}

semantic::Truth negation(semantic::Truth holds) {
	return holds == semantic::Truth::True ? semantic::Truth::False
		: holds == semantic::Truth::False ? semantic::Truth::True : semantic::Truth::Undetermined;
}

// The period operand as a duration: a duration, or a whole count in options.unit.
std::optional<Value> period_of(const Operand &operand, const OpOptions &options) {
	if (!operand.value) return std::nullopt;
	if (operand.value->kind == semantic::Kind::Duration) return *operand.value;
	const auto unit = unit_named(options.unit);
	if (!unit || (operand.value->kind != semantic::Kind::Number && operand.value->kind != semantic::Kind::Count)) return std::nullopt;
	return semantic::make_duration(operand.value->amount, *unit);
}

// A whole count of a period, or nothing when the count is not whole.
std::optional<long long> whole(const semantic::Rational &count) {
	if (!count.is_integer()) return std::nullopt;
	const semantic::Int128 number = count.numerator();
	if (number > 1000000 || number < -1000000) return std::nullopt;
	return static_cast<long long>(number);
}

// `day` moved by `months`, the day of the month clamped to the month's end
// (January 31 + 1 month is February 28 or 29).
long long add_months_to_day(long long day, long long months) {
	const calendar::CivilDate civil = calendar::civil_from_days(day);
	const long long index = civil.year * 12 + static_cast<long long>(civil.month) - 1 + months;
	const long long year = index >= 0 ? index / 12 : (index - 11) / 12;
	const unsigned month = static_cast<unsigned>(index - year * 12 + 1);
	const unsigned length = calendar::civil_from_days(calendar::days_from_civil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - 1).day;
	return calendar::days_from_civil(year, month, std::min(civil.day, length));
}

// Whole months from `from` to `to` by the anniversary rule (the anniversary
// day clamped to its month's end), and whether `to` is exactly that anniversary.
long long whole_months(long long from, long long to, bool &exact) {
	if (to < from) {
		const long long back = whole_months(to, from, exact);
		return -back;
	}
	const calendar::CivilDate a = calendar::civil_from_days(from);
	const calendar::CivilDate b = calendar::civil_from_days(to);
	long long months = (b.year - a.year) * 12 + (static_cast<long long>(b.month) - static_cast<long long>(a.month));
	if (add_months_to_day(from, months) > to) --months;
	exact = add_months_to_day(from, months) == to;
	return months;
}

long long months_per(DurationUnit unit) {
	return unit == DurationUnit::Year ? 12 : unit == DurationUnit::Quarter ? 3 : 1;
}

semantic::Rational days_per(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Week: return semantic::Rational(7);
		case DurationUnit::Hour: return semantic::Rational(1, 24);
		case DurationUnit::Minute: return semantic::Rational(1, 1440);
		default: return semantic::Rational(1);
	}
}

// ---------------------------------------------------------------------------
// Calendars (DESIGN4 §7.3): a handle, "governing" or "calendar_days".

struct UsedCalendar {
	std::optional<calendar::BusinessDayRule> rule;
	bool calendar_days = false;
	bool not_governing = false;
	Code error = Code::None;
};

Code code_of_reason(const std::string &reason) {
	if (reason == "rule_conflict") return Code::RuleConflict;
	if (reason == "definition_not_found") return Code::DefinitionNotFound;
	return Code::RuleUnknown;
}

calendar::GoverningRules governing_for(const std::vector<Operand> &operands, const OpContext &context) {
	if (!context.rules_for) return context.rules;
	std::vector<std::string> writings;
	for (const Operand &operand : operands)
		if (operand.row != nullptr && !operand.row->original.empty()) writings.push_back(operand.row->original);
	return context.rules_for(writings);
}

std::string handle_of(std::string written) {
	if (written.size() > 4 && written.compare(0, 2, "[[") == 0 && written.compare(written.size() - 2, 2, "]]") == 0)
		written = written.substr(2, written.size() - 4);
	return written;
}

// The calendar an op names - its trailing CALENDAR operand, else
// options.calendar, else the governing rule.
UsedCalendar calendar_for(const std::vector<Operand> &operands, std::size_t calendarAt, const OpOptions &options,
		const OpContext &context) {
	UsedCalendar used;
	const Operand *operand = operands.size() > calendarAt ? &operands[calendarAt] : nullptr;
	std::string named = operand != nullptr ? (operand->arg.symbol.empty() ? operand->arg.written : operand->arg.symbol)
		: options.calendar.value_or("governing");
	named = handle_of(named);
	if (named == "calendar_days") {
		used.calendar_days = true;
		return used;
	}
	const calendar::GoverningRules governing = governing_for(operands, context);
	if (named == "governing" || named.empty()) {
		if (!governing.business_day) {
			used.error = code_of_reason(governing.unknown_reason);
			return used;
		}
		used.rule = governing.business_day;
		return used;
	}
	const CalendarHandle *handle = operand != nullptr ? operand->calendar : nullptr;
	if (handle == nullptr && context.table != nullptr)
		for (const CalendarHandle &each : context.table->calendars)
			if (each.handle == named) handle = &each;
	if (handle == nullptr) {
		used.error = Code::UnknownCalendar;
		return used;
	}
	if (!handle->rule) {
		used.error = Code::RuleUnknown;
		return used;
	}
	used.rule = handle->rule;
	// Another rule than the one that governs these facts: allowed (an
	// alternative scenario), and said so.
	used.not_governing = !governing.business_day || !calendar::same_days(*governing.business_day, *handle->rule)
		|| governing.business_day->source != handle->rule->source;
	return used;
}

void note_rule(OpAnswer &answer, const UsedCalendar &used) {
	if (used.calendar_days) {
		add_words(answer, kWordCalendarDays);
		return;
	}
	if (!used.rule) return;
	answer.record.business_day_rule = calendar::rule_name(*used.rule);
	if (!used.rule->defining_passage_sha256.empty()) {
		answer.record.business_day_rule_source.origin = "source";
		answer.record.business_day_rule_source.passage_sha256 = used.rule->defining_passage_sha256;
		answer.record.business_day_rule_source.offset = used.rule->defining_offset;
		answer.record.business_day_rule_source.length = used.rule->defining_length;
	}
	if (used.not_governing) add_words(answer, kWordNotGoverning);
}

// Why a count or a day under `rule` is only a bound.
const char *bound_reason(const calendar::BusinessDayRule &rule) {
	return rule.holidays_known && !rule.possible_calendars.empty() ? kWordClosuresPossible : kWordHolidaysNotKnown;
}

// The operand options.anchor names (a DATE symbol or @label), as a value.
struct Anchor {
	std::optional<Value> value;
	std::set<std::string> roots;
	bool named = false;
	bool found = false;
};

Anchor anchor_of(const OpOptions &options, const OpContext &context) {
	Anchor anchor;
	if (!options.anchor || options.anchor->empty()) return anchor;
	anchor.named = true;
	const std::string &name = *options.anchor;
	if (context.table == nullptr) return anchor;
	std::string symbol = name;
	if (name[0] == '@') {
		const auto label = context.table->compute_labels.find(name.substr(1));
		if (label == context.table->compute_labels.end()) return anchor;
		symbol = label->second;
	}
	const auto derived = context.table->derived_facts.find(symbol);
	if (derived != context.table->derived_facts.end()) {
		anchor.value = derived->second.value;
		anchor.roots = derived->second.roots;
		anchor.found = derived->second.certainty.kind == Certainty::Kind::Exact;
		return anchor;
	}
	const RequestMappingEntry *row = context.row_of ? context.row_of(symbol) : nullptr;
	if (row != nullptr && row->value) {
		anchor.value = row->value;
		if (!row->fact_key.empty()) anchor.roots.insert(row->fact_key);
		anchor.found = true;
	}
	return anchor;
}

// ---------------------------------------------------------------------------
// Evaluators.

enum class Order { Before, After, SameTime, Overlaps };

OpAnswer evaluate_order(Order order, const std::vector<Operand> &operands, const OpOptions &options) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, order == Order::SameTime ? type_same_time : type_order)) return answer;
	const Value *a = value_of(operands[0]);
	const Value *b = value_of(operands[1]);
	if (a == nullptr || b == nullptr) return failed(Code::DependencyFailed, a == nullptr ? 1 : 2);
	semantic::Relation relation;
	switch (order) {
		case Order::Before: relation = semantic::before(*a, *b); break;
		case Order::After: relation = semantic::after(*a, *b); break;
		case Order::SameTime: relation = semantic::same_day(*a, *b); break;
		case Order::Overlaps: relation = semantic::overlaps(*a, *b); break;
	}
	return truth(std::move(answer), relation.truth);
}

OpAnswer evaluate_before(const std::vector<Operand> &operands, const OpOptions &options, OpContext &) {
	return evaluate_order(Order::Before, operands, options);
}
OpAnswer evaluate_after(const std::vector<Operand> &operands, const OpOptions &options, OpContext &) {
	return evaluate_order(Order::After, operands, options);
}
OpAnswer evaluate_same_time(const std::vector<Operand> &operands, const OpOptions &options, OpContext &) {
	return evaluate_order(Order::SameTime, operands, options);
}
OpAnswer evaluate_overlaps(const std::vector<Operand> &operands, const OpOptions &options, OpContext &) {
	return evaluate_order(Order::Overlaps, operands, options);
}

// The day a date moved by a period falls on, as an interval: exact for a
// day, the moved interval for a month, quarter or year (a range where it no
// longer is one period). Business days are the caller's.
struct Moved {
	temporal::TemporalValue interval;
	bool range = false;
};

std::optional<Moved> moved(const Value &date, const Value &period) {
	const DurationUnit unit = period.unit;
	const temporal::TemporalValue &from = date.date;
	const bool dayClass = unit == DurationUnit::Day || unit == DurationUnit::Week;
	semantic::Rational count = period.amount;
	if (dayClass) count = count * days_per(unit);
	const auto steps = whole(count);
	if (!steps) return std::nullopt;
	Moved result;
	if (dayClass) {
		result.interval = {from.first_day + *steps, from.last_day + *steps, from.precision};
		result.range = from.precision != temporal::Precision::Day;
		return result;
	}
	const long long months = *steps * months_per(unit);
	const long long first = add_months_to_day(from.first_day, months);
	switch (from.precision) {
		case temporal::Precision::Day:
			result.interval = {first, first, temporal::Precision::Day};
			return result;
		case temporal::Precision::Month:
			result.interval = {first, add_months_to_day(first, 1) - 1, temporal::Precision::Month};
			return result;
		case temporal::Precision::Quarter:
			result.interval = {first, add_months_to_day(first, 3) - 1, temporal::Precision::Quarter};
			result.range = unit == DurationUnit::Month;
			return result;
		case temporal::Precision::Year:
			result.interval = {first, add_months_to_day(first, 12) - 1, temporal::Precision::Year};
			result.range = unit != DurationUnit::Year;
			return result;
	}
	return std::nullopt;
}

// A date moved by a period: exact, a range (a bound on both ends), or - for
// business days - under the calendar.
OpAnswer shift(OpAnswer answer, const Value &date, const Value &period, const std::vector<Operand> &operands, std::size_t calendarAt,
		const OpOptions &options, OpContext &context) {
	if (period.unit == DurationUnit::BusinessDay) {
		const UsedCalendar used = calendar_for(operands, calendarAt, options, context);
		if (used.error != Code::None) return failed(used.error, calendarAt + 1);
		note_rule(answer, used);
		const auto count = whole(period.amount);
		if (!count) return unknown(Code::PrecisionInsufficient, &answer);
		if (used.calendar_days) {
			// Business days counted as calendar days is no business-day count.
			return failed(Code::CalendarRequired, calendarAt + 1, "CALENDAR");
		}
		const calendar::DayBound bound = calendar::deadline_bound(*used.rule, date.date.first_day, *count);
		if (bound.exact) return exact_value(std::move(answer), day_value(*bound.exact, date));
		if (!bound.not_before && !bound.not_after) return unknown(Code::RuleUnknown, &answer);
		std::optional<Value> lo;
		std::optional<Value> hi;
		if (bound.not_before) lo = day_value(*bound.not_before, date);
		if (bound.not_after) hi = day_value(*bound.not_after, date);
		return bounded(std::move(answer), lo, hi, bound_reason(*used.rule));
	}
	const auto result = moved(date, period);
	if (!result) return unknown(Code::PrecisionInsufficient, &answer);
	if (result->range)
		return bounded(std::move(answer), day_value(result->interval.first_day, date), day_value(result->interval.last_day, date),
			kWordPartialDate);
	Value value = date;
	value.date = result->interval;
	return exact_value(std::move(answer), value);
}

OpAnswer evaluate_add_duration(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_add_duration)) return answer;
	const Value *date = value_of(operands[0]);
	const auto period = period_of(operands[1], options);
	if (date == nullptr || !period) return failed(Code::DependencyFailed, date == nullptr ? 1 : 2);
	return shift(std::move(answer), *date, *period, operands, 2, options, context);
}

OpAnswer evaluate_deadline(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_deadline)) return answer;
	const Value *date = value_of(operands[0]);
	const auto period = period_of(operands[1], options);
	if (date == nullptr || !period) return failed(Code::DependencyFailed, date == nullptr ? 1 : 2);
	if (period->unit == DurationUnit::BusinessDay) return shift(std::move(answer), *date, *period, operands, 2, options, context);
	// A calendar period: its last day, then rolled per the calendar's rule
	// (C3 §2; … 193, a following-business-day clause). A period
	// counted back ("thirteen days before") never rolls forward past the
	// count: its closed last day moves, if at all, to the business day before.
	const bool countedBack = period->amount.sign() < 0;
	const auto end = moved(*date, *period);
	if (!end) return unknown(Code::PrecisionInsufficient, &answer);
	const UsedCalendar used = calendar_for(operands, 2, options, context);
	if (used.error != Code::None) return failed(used.error, 3);
	note_rule(answer, used);
	// A last day on a weekend is said as such - weekday arithmetic, no calendar
	// (owner, 26 Sep 2026: no holiday calendars; the model reasons about a roll
	// the law may set, and asks the lawyer where it matters).
	// Said only where the result leaves the weekend to the reader: counted in
	// calendar days, or a bound; a day a known rule settled needs no word.
	const long long weekday = ((end->interval.first_day % 7) + 7 + 4) % 7;  // 1970-01-01 was a Thursday; 0 is Sunday
	const char *weekend = weekday == 6 ? kWordEndsOnSaturday : weekday == 0 ? kWordEndsOnSunday : nullptr;
	if (used.calendar_days) {
		if (weekend != nullptr) add_words(answer, weekend);
		return exact_value(std::move(answer), day_value(end->interval.first_day, *date));
	}
	const calendar::DayBound bound = calendar::calendar_period_end(&*used.rule, end->interval.first_day, countedBack);
	if (bound.exact) return exact_value(std::move(answer), day_value(*bound.exact, *date));
	if (weekend != nullptr) add_words(answer, weekend);
	if (!bound.not_before && !bound.not_after) return unknown(Code::RuleUnknown, &answer);
	std::optional<Value> lo;
	std::optional<Value> hi;
	if (bound.not_before) lo = day_value(*bound.not_before, *date);
	if (bound.not_after) hi = day_value(*bound.not_after, *date);
	// Why a bound: the roll that would settle it is not known (none at all, one
	// scoped to payments, or - counted back - one that is not a preceding
	// roll), and/or the day's own closure is not known.
	const bool rollApplies = used.rule->roll_scope != "payments";
	const bool rollSettled = rollApplies
		&& (countedBack ? used.rule->roll == "preceding_business_day" : !used.rule->roll.empty());
	if (!rollSettled) add_words(answer, kWordRollNotKnown);
	const calendar::DayStatus endStatus = calendar::status(*used.rule, end->interval.first_day);
	const bool dayUncertain = endStatus == calendar::DayStatus::Open || endStatus == calendar::DayStatus::Possible;
	return bounded(std::move(answer), lo, hi, dayUncertain || rollSettled ? bound_reason(*used.rule) : std::string());
}

OpAnswer evaluate_duration_between(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_duration_between)) return answer;
	const Value *a = value_of(operands[0]);
	const Value *b = value_of(operands[1]);
	if (a == nullptr || b == nullptr) return failed(Code::DependencyFailed, a == nullptr ? 1 : 2);
	const DurationUnit unit = *unit_named(options.unit);
	const bool days = a->date.precision == temporal::Precision::Day && b->date.precision == temporal::Precision::Day;
	if (unit == DurationUnit::BusinessDay) {
		const UsedCalendar used = calendar_for(operands, 2, options, context);
		if (used.error != Code::None) return failed(used.error, 3);
		note_rule(answer, used);
		if (used.calendar_days) return failed(Code::CalendarRequired, 3, "CALENDAR");
		const calendar::CountBound bound = calendar::count_bound(*used.rule, a->date.first_day, b->date.first_day);
		if (bound.exact) return exact_value(std::move(answer), semantic::make_duration(semantic::Rational(*bound.exact), unit));
		const long long low = std::min(bound.at_least, bound.at_most);
		const long long high = std::max(bound.at_least, bound.at_most);
		return bounded(std::move(answer), semantic::make_duration(semantic::Rational(low), unit),
			semantic::make_duration(semantic::Rational(high), unit), bound_reason(*used.rule));
	}
	if (!month_class(unit)) {
		// Days after: exclusive start, inclusive end; in the unit asked.
		const auto in_unit = [&](long long count) { return semantic::make_duration(semantic::Rational(count) / days_per(unit), unit); };
		if (days) return exact_value(std::move(answer), in_unit(b->date.first_day - a->date.first_day));
		return bounded(std::move(answer), in_unit(b->date.first_day - a->date.last_day), in_unit(b->date.last_day - a->date.first_day),
			kWordPartialDate);
	}
	const long long per = months_per(unit);
	// The whole units at or below the true count (its floor), and whether the
	// count is exactly that. whole_months truncates toward zero: backwards
	// (`to` before `from`) a part-month makes the true count lower still.
	const auto units = [&](long long from, long long to, bool &exact) {
		bool anniversary = false;
		const long long months = whole_months(from, to, anniversary);
		const long long floorMonths = months < 0 && !anniversary ? months - 1 : months;
		exact = anniversary && months % per == 0;
		long long count = floorMonths / per;
		if (floorMonths % per != 0 && floorMonths < 0) --count;
		return count;
	};
	if (days) {
		bool exact = false;
		const long long count = units(a->date.first_day, b->date.first_day, exact);
		if (exact) return exact_value(std::move(answer), semantic::make_duration(semantic::Rational(count), unit));
		return bounded(std::move(answer), semantic::make_duration(semantic::Rational(count), unit),
			semantic::make_duration(semantic::Rational(count + 1), unit), kWordMonthLength);
	}
	bool lowExact = false;
	bool highExact = false;
	const long long low = units(a->date.last_day, b->date.first_day, lowExact);
	const long long high = units(a->date.first_day, b->date.last_day, highExact);
	return bounded(std::move(answer), semantic::make_duration(semantic::Rational(low), unit),
		semantic::make_duration(semantic::Rational(highExact ? high : high + 1), unit), kWordPartialDate);
}

OpAnswer evaluate_containment(bool outside, const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_within)) return answer;
	const Value *date = value_of(operands[0]);
	const Value *start = value_of(operands[1]);
	if (date == nullptr || start == nullptr) return failed(Code::DependencyFailed, date == nullptr ? 1 : 2);
	semantic::Truth holds = semantic::Truth::Undetermined;
	if (operands.size() == 2) {
		holds = semantic::within(*date, *start).truth;
	} else if (operands[2].type.kind == TypeKind::Date) {
		const Value *end = value_of(operands[2]);
		if (end == nullptr) return failed(Code::DependencyFailed, 3);
		holds = semantic::within(*date, *start, *end).truth;
	} else {
		// From an anchor for a period: anchor <= date <= anchor + period; a
		// negative period looks back (anchor + period <= date <= anchor). A
		// period the model writes carries its direction (kSignThePeriod).
		if (operands[2].arg.kind == ArgKind::Constant && !written_signed(operands[2].arg.constant)) {
			OpAnswer unsigned_period = failed(Code::TypeMismatch, 3, "a signed DURATION", "an unsigned DURATION");
			unsigned_period.error.hint = kSignThePeriod;
			return unsigned_period;
		}
		const auto period = period_of(operands[2], options);
		if (!period) return failed(Code::DependencyFailed, 3);
		OpAnswer reach = shift(OpAnswer{}, *start, *period, operands, 3, options, context);
		if (reach.status == OpStatus::Error) return reach;
		answer.record.business_day_rule = reach.record.business_day_rule;
		answer.record.business_day_rule_source = reach.record.business_day_rule_source;
		if (reach.words.find(kWordNotGoverning) != std::string::npos) add_words(answer, kWordNotGoverning);
		if (reach.status != OpStatus::Ok && reach.status != OpStatus::Bounded) return unknown(Code::PrecisionInsufficient, &answer);
		const bool back = period->amount.sign() < 0;
		// The reach's end nearest the anchor (the shortest reach) and its
		// farthest (the longest), exact or as far as its bound allows. The
		// near end is never past the anchor; the far end may have no bound -
		// no latest day forward, no earliest day back.
		const std::optional<Value> nearEnd = reach.status == OpStatus::Ok ? reach.value : back ? reach.hi : reach.lo;
		const std::optional<Value> farEnd = reach.status == OpStatus::Ok ? reach.value : back ? reach.lo : reach.hi;
		const Value near = nearEnd ? *nearEnd : *start;
		const auto span = [&](const Value &end) {
			return back ? semantic::within(*date, end, *start).truth : semantic::within(*date, *start, end).truth;
		};
		const semantic::Truth shortest = span(near);
		if (farEnd) {
			const semantic::Truth longest = span(*farEnd);
			holds = shortest == longest ? shortest : semantic::Truth::Undetermined;
		} else {
			// No far end: inside when inside the shortest reach, outside when
			// on the anchor's other side, else not known.
			const semantic::Truth beyond = back ? semantic::after(*date, *start).truth : semantic::before(*date, *start).truth;
			holds = shortest == semantic::Truth::True ? shortest
				: beyond == semantic::Truth::True ? semantic::Truth::False : semantic::Truth::Undetermined;
		}
	}
	return truth(std::move(answer), outside ? negation(holds) : holds);
}

OpAnswer evaluate_within(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	return evaluate_containment(false, operands, options, context);
}
OpAnswer evaluate_outside(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	return evaluate_containment(true, operands, options, context);
}

OpAnswer evaluate_period(bool end, const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_period_end)) return answer;
	const Value *date = value_of(operands[0]);
	if (date == nullptr) return failed(Code::DependencyFailed, 1);
	const Anchor anchor = anchor_of(options, context);
	if (anchor.named && !anchor.found) return failed(Code::UnknownLabel, 0, "DATE", options.anchor.value_or(""));
	if (anchor.value && anchor.value->kind != semantic::Kind::Date && anchor.value->kind != semantic::Kind::DayOfYear)
		return failed(Code::TypeMismatch, 0, "DATE", "anchor");
	answer.roots.insert(anchor.roots.begin(), anchor.roots.end());
	const DurationUnit unit = *unit_named(options.unit);
	try {
		return exact_value(std::move(answer), end ? semantic::end_of(*date, unit, anchor.value) : semantic::start_of(*date, unit, anchor.value));
	} catch (const semantic::AlgebraError &error) {
		if (error.kind() == "type") return failed(Code::TypeMismatch, 0);
		OpAnswer unsure = unknown(Code::PrecisionInsufficient);
		unsure.result = answer.result;
		unsure.roots = answer.roots;
		return unsure;
	}
}

OpAnswer evaluate_start_of(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	return evaluate_period(false, operands, options, context);
}
OpAnswer evaluate_end_of(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	return evaluate_period(true, operands, options, context);
}

OpAnswer evaluate_days_in(const std::vector<Operand> &operands, const OpOptions &options, OpContext &) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_days_in)) return answer;
	const Value *date = value_of(operands[0]);
	if (date == nullptr) return failed(Code::DependencyFailed, 1);
	return exact_value(std::move(answer), semantic::days_in(*date));
}

OpAnswer evaluate_next_business_day(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_next_business_day)) return answer;
	const Value *date = value_of(operands[0]);
	if (date == nullptr) return failed(Code::DependencyFailed, 1);
	const UsedCalendar used = calendar_for(operands, 1, options, context);
	if (used.error != Code::None) return failed(used.error, 2);
	if (used.calendar_days) return failed(Code::CalendarRequired, 2, "CALENDAR");
	note_rule(answer, used);
	const calendar::DayBound bound = calendar::next_business_day_bound(*used.rule, date->date.first_day);
	if (bound.exact) return exact_value(std::move(answer), day_value(*bound.exact, *date));
	if (!bound.not_before) return unknown(Code::RuleUnknown, &answer);
	std::optional<Value> hi;
	if (bound.not_after) hi = day_value(*bound.not_after, *date);
	return bounded(std::move(answer), day_value(*bound.not_before, *date), hi, bound_reason(*used.rule));
}

OpAnswer evaluate_day_class(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_day_class)) return answer;
	const Value *date = value_of(operands[0]);
	if (date == nullptr) return failed(Code::DependencyFailed, 1);
	const UsedCalendar used = calendar_for(operands, 1, options, context);
	if (used.error != Code::None) return failed(used.error, 2);
	if (used.calendar_days) return failed(Code::CalendarRequired, 2, "CALENDAR");
	note_rule(answer, used);
	answer.status = OpStatus::Ok;
	answer.certainty.kind = Certainty::Kind::Exact;
	switch (calendar::status(*used.rule, date->date.first_day)) {
		case calendar::DayStatus::Business: answer.relation = "BUSINESS_DAY"; break;
		case calendar::DayStatus::Weekend: answer.relation = "WEEKEND"; break;
		case calendar::DayStatus::Holiday: answer.relation = "HOLIDAY"; break;
		case calendar::DayStatus::Possible:
			answer.relation = "MAY_CLOSE";
			add_words(answer, kWordClosuresPossible);
			break;
		case calendar::DayStatus::Open:
			answer.relation = "OPEN";
			add_words(answer, kWordHolidaysNotKnown);
			break;
	}
	return answer;
}

OpAnswer evaluate_next_occurrence(const std::vector<Operand> &operands, const OpOptions &options, OpContext &) {
	OpAnswer answer;
	if (!prepare(answer, operands, options, type_next_occurrence)) return answer;
	const Value *day = value_of(operands[0]);
	const Value *date = value_of(operands[1]);
	if (day == nullptr || date == nullptr) return failed(Code::DependencyFailed, day == nullptr ? 1 : 2);
	try {
		return exact_value(std::move(answer), semantic::next_occurrence(*day, *date));
	} catch (const semantic::AlgebraError &error) {
		if (error.kind() != "undetermined") return failed(Code::TypeMismatch, 0);
		OpAnswer unsure = unknown(Code::PrecisionInsufficient);
		unsure.result = answer.result;
		unsure.roots = answer.roots;
		return unsure;
	}
}

// ---------------------------------------------------------------------------
// Pre-charges (DESIGN4 §8.3): decided from the operands' declared types and
// the op's options, never from a value, and paid before evaluation. The key
// is the op over its operands' fact keys; the executor's normaliser may
// replace it with the canonical form (lane X), and adds the public grouping
// keys.

std::string charge_key(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &context) {
	std::string key = context.node != nullptr ? context.node->op : std::string("temporal");
	key += "(";
	for (std::size_t index = 0; index < operands.size(); ++index) {
		const Operand &operand = operands[index];
		key += index == 0 ? "" : ",";
		key += !operand.fact_key.empty() ? "fk:" + operand.fact_key
			: !operand.arg.symbol.empty() ? operand.arg.symbol : "c:" + operand.arg.written;
	}
	key += ";" + options.unit.value_or("") + ";" + options.calendar.value_or("") + ";" + options.anchor.value_or("") + ")";
	return key;
}

ObservationCharge charge_of(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &context, bool calendarBudget) {
	ObservationCharge charge;
	charge.key = charge_key(operands, options, context);
	std::set<std::string> roots;
	std::set<std::string> dated;
	for (const Operand &operand : operands) {
		roots.insert(operand.roots.begin(), operand.roots.end());
		if (operand.type.kind == TypeKind::Date) dated.insert(operand.roots.begin(), operand.roots.end());
	}
	const Anchor anchor = anchor_of(options, context);
	roots.insert(anchor.roots.begin(), anchor.roots.end());
	dated.insert(anchor.roots.begin(), anchor.roots.end());
	charge.roots.assign(roots.begin(), roots.end());
	if (calendarBudget) charge.calendar_roots.assign(dated.begin(), dated.end());
	return charge;
}

bool any_partial(const std::vector<Operand> &operands) {
	return std::any_of(operands.begin(), operands.end(), [](const Operand &operand) {
		return operand.type.kind == TypeKind::Date && !is_day(operand.type) && !is_day_of_year(operand.type);
	});
}

bool any_anchored(const std::vector<Operand> &operands) {
	return std::any_of(operands.begin(), operands.end(), [](const Operand &operand) { return operand.type.calendar_anchored; });
}

// Relations: always an observation; on the calendar budget too when an
// operand is partial or calendar-anchored (the answer may turn on where in a
// period a day falls) or a business-day reach is involved.
std::optional<ObservationCharge> charge_relation(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &context) {
	bool calendarBudget = any_partial(operands) || any_anchored(operands);
	for (const Operand &operand : operands)
		if (operand.type.kind != TypeKind::Date && operand.type.kind != TypeKind::Calendar) {
			const DurationUnit unit = period_unit(operand.type, options);
			calendarBudget = calendarBudget || month_class(unit) || unit == DurationUnit::BusinessDay;
		}
	return charge_of(operands, options, context, calendarBudget);
}

// A value whose exactness can depend on a date: charged to the calendar budget.
std::optional<ObservationCharge> charge_duration_between(const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &context) {
	const auto unit = unit_named(options.unit);
	if (!unit || !(month_class(*unit) || *unit == DurationUnit::BusinessDay)) return std::nullopt;
	return charge_of(operands, options, context, true);
}

// Whether a period operand's value is hidden: a protected root or a derived
// fact. Whether it is a whole count in its unit (and not beyond the day
// range) is then a property of its value - "1.5 months" parses as a root as
// well as "2 months" - and an answer's status (a date, or UNKNOWN) shows it.
// A literal the model wrote is its own.
bool hidden_period(const std::vector<Operand> &operands) {
	return operands.size() > 1 && (operands[1].derived != nullptr || operands[1].row != nullptr || !operands[1].roots.empty());
}

std::optional<ObservationCharge> charge_add_duration(const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &context) {
	if (operands.size() < 2) return std::nullopt;
	const DurationUnit unit = period_unit(operands[1].type, options);
	if (unit == DurationUnit::BusinessDay) return charge_of(operands, options, context, true);
	// Whether a hidden period is whole depends on its value.
	if (hidden_period(operands)) return charge_of(operands, options, context, false);
	return std::nullopt;
}

std::optional<ObservationCharge> charge_deadline(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &context) {
	const std::string named = operands.size() > 2 ? handle_of(operands[2].arg.symbol.empty() ? operands[2].arg.written : operands[2].arg.symbol)
		: options.calendar.value_or("governing");
	const DurationUnit unit = operands.size() > 1 ? period_unit(operands[1].type, options) : DurationUnit::Day;
	if (named == "calendar_days" && unit != DurationUnit::BusinessDay) {
		// Counted in calendar days: no calendar reached; only whether a hidden
		// period is whole depends on a value.
		if (hidden_period(operands)) return charge_of(operands, options, context, false);
		return std::nullopt;
	}
	return charge_of(operands, options, context, true);
}

std::optional<ObservationCharge> charge_calendar(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &context) {
	return charge_of(operands, options, context, true);
}

std::optional<ObservationCharge> charge_period_end(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &context) {
	if (!options.anchor) return std::nullopt;
	// Counted from an anchor, a date coarser than a day - or an anchor that is
	// one - may fall across the period's start.
	const Anchor anchor = anchor_of(options, context);
	const bool partialAnchor = anchor.value && anchor.value->kind == semantic::Kind::Date
		&& anchor.value->date.precision != temporal::Precision::Day;
	if (!any_partial(operands) && !partialAnchor) return std::nullopt;
	return charge_of(operands, options, context, true);
}

std::optional<ObservationCharge> charge_next_occurrence(const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &context) {
	if (operands.size() < 2 || is_day(operands[1].type)) return std::nullopt;
	return charge_of(operands, options, context, true);
}

std::optional<calendar::BusinessDayRule> governing_rule_of(const std::vector<calendar::RulePassage> &passages,
		std::optional<std::size_t> operative, const std::optional<calendar::BusinessDayRule> &matterRule, std::string &reason) {
	calendar::GoverningInput input;
	input.passages = passages;
	input.operative = operative;
	input.matter_rule = matterRule;
	const calendar::GoverningRules rules = calendar::governing_rule(input);
	reason = rules.unknown_reason;
	return rules.business_day;
}

bool counts_business_days(const std::string &text) {
	std::string folded;
	folded.reserve(text.size());
	for (const char character : text) folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	for (const char *cue : {"business day", "banking day", "working day", "business-day", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87"
			/* … */, "\xD0\xA0\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87" /* … */, "\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA"
			/* … */})
		if (folded.find(cue) != std::string::npos) return true;
	return false;
}

bool same_rule(const calendar::BusinessDayRule &a, const calendar::BusinessDayRule &b) {
	return a.source == b.source && calendar::same_days(a, b) && a.defining_passage_sha256 == b.defining_passage_sha256
		&& a.roll == b.roll && a.roll_scope == b.roll_scope;
}

} // namespace

std::optional<DurationUnit> unit_named(const std::optional<std::string> &unit) {
	if (!unit) return std::nullopt;
	static const std::map<std::string, DurationUnit> units{
		{"minutes", DurationUnit::Minute}, {"minute", DurationUnit::Minute}, {"hours", DurationUnit::Hour}, {"hour", DurationUnit::Hour},
		{"days", DurationUnit::Day}, {"day", DurationUnit::Day}, {"calendar_days", DurationUnit::Day},
		{"business_days", DurationUnit::BusinessDay}, {"business_day", DurationUnit::BusinessDay}, {"weeks", DurationUnit::Week},
		{"week", DurationUnit::Week}, {"months", DurationUnit::Month}, {"month", DurationUnit::Month},
		{"quarters", DurationUnit::Quarter}, {"quarter", DurationUnit::Quarter}, {"years", DurationUnit::Year}, {"year", DurationUnit::Year},
	};
	const auto found = units.find(*unit);
	if (found == units.end()) return std::nullopt;
	return found->second;
}

bool calendar_anchored_op(std::string_view op, const std::vector<ArgType> &arguments, const OpOptions &options) {
	using Typer = std::optional<ResultType> (*)(const std::vector<ArgType> &, const OpOptions &, TypeError &);
	static const std::map<std::string_view, Typer> typers{
		{"before", type_order}, {"after", type_order}, {"same_time", type_same_time}, {"overlaps", type_order},
		{"within", type_within}, {"outside", type_within}, {"duration_between", type_duration_between},
		{"add_duration", type_add_duration}, {"deadline", type_deadline}, {"start_of", type_period_end},
		{"end_of", type_period_end}, {"days_in", type_days_in}, {"next_business_day", type_next_business_day},
		{"day_class", type_day_class}, {"next_occurrence", type_next_occurrence},
	};
	const auto found = typers.find(op);
	if (found == typers.end()) return false;
	TypeError error;
	const auto result = found->second(arguments, options, error);
	return result && result->type.calendar_anchored;
}

std::vector<CalendarHandle> calendar_handles(const std::vector<calendar::RulePassage> &passages,
		const std::optional<calendar::BusinessDayRule> &matterRule, int thisYear) {
	struct Found {
		calendar::BusinessDayRule rule;
		std::vector<std::string> governs;
	};
	std::vector<Found> found;
	const auto place = [&found](const calendar::BusinessDayRule &rule, const std::string &document) {
		for (Found &each : found)
			if (same_rule(each.rule, rule)) {
				if (!document.empty() && std::find(each.governs.begin(), each.governs.end(), document) == each.governs.end())
					each.governs.push_back(document);
				return;
			}
		found.push_back({rule, document.empty() ? std::vector<std::string>{} : std::vector<std::string>{document}});
	};
	// Each document that counts in business days: the rule that governs it.
	std::vector<std::string> seen;
	for (std::size_t index = 0; index < passages.size(); ++index) {
		const calendar::RulePassage &passage = passages[index];
		if (passage.matter_record || passage.document.empty()) continue;
		if (std::find(seen.begin(), seen.end(), passage.document) != seen.end()) continue;
		bool counts = false;
		std::optional<std::size_t> operative;
		for (std::size_t other = index; other < passages.size(); ++other)
			if (passages[other].document == passage.document && counts_business_days(passages[other].text)) {
				counts = true;
				operative = other;
				break;
			}
		if (!counts) continue;
		seen.push_back(passage.document);
		std::string reason;
		if (const auto rule = governing_rule_of(passages, operative, matterRule, reason)) place(*rule, passage.document);
	}
	// Every definition the material writes, governing or not (a draft's).
	for (const calendar::Definition &definition : calendar::definitions_in(passages))
		if (definition.understood && definition.incorporates.empty()) place(definition.rule, std::string());
	if (matterRule) place(*matterRule, std::string());

	std::vector<CalendarHandle> handles;
	for (std::size_t index = 0; index < found.size(); ++index) {
		CalendarHandle handle;
		handle.handle = "Calendar" + std::to_string(index + 1);
		handle.rule = found[index].rule;
		handle.governing = !found[index].governs.empty();
		handle.documents = found[index].governs;
		std::string definedIn;
		const std::size_t at = found[index].rule.defining_passage;
		if (at < passages.size()) definedIn = passages[at].document;
		handle.line = "[[" + handle.handle + "]] " + calendar::calendar_line(found[index].rule, definedIn, found[index].governs, thisYear);
		handles.push_back(std::move(handle));
	}
	return handles;
}

void register_temporal_family() {
	register_family("temporal", {
		{"before", type_order, evaluate_before, ObservationClass::Always, charge_relation},
		{"after", type_order, evaluate_after, ObservationClass::Always, charge_relation},
		{"same_time", type_same_time, evaluate_same_time, ObservationClass::Always, charge_relation},
		{"overlaps", type_order, evaluate_overlaps, ObservationClass::Always, charge_relation},
		{"within", type_within, evaluate_within, ObservationClass::Always, charge_relation},
		{"outside", type_within, evaluate_outside, ObservationClass::Always, charge_relation},
		{"duration_between", type_duration_between, evaluate_duration_between, ObservationClass::Conditional, charge_duration_between},
		{"add_duration", type_add_duration, evaluate_add_duration, ObservationClass::Conditional, charge_add_duration},
		{"deadline", type_deadline, evaluate_deadline, ObservationClass::Conditional, charge_deadline},
		{"start_of", type_period_end, evaluate_start_of, ObservationClass::Conditional, charge_period_end},
		{"end_of", type_period_end, evaluate_end_of, ObservationClass::Conditional, charge_period_end},
		{"days_in", type_days_in, evaluate_days_in, ObservationClass::Never, nullptr},
		{"next_business_day", type_next_business_day, evaluate_next_business_day, ObservationClass::Conditional, charge_calendar},
		{"day_class", type_day_class, evaluate_day_class, ObservationClass::Always, charge_calendar},
		{"next_occurrence", type_next_occurrence, evaluate_next_occurrence, ObservationClass::Conditional, charge_next_occurrence},
	});
}

bool written_signed(std::string_view written) {
	const std::size_t first = written.find_first_not_of(" \t");
	if (first == std::string_view::npos) return false;
	return written[first] == '-' || written[first] == '+' || written.substr(first, 3) == "\xE2\x88\x92";
}

std::optional<semantic::Value> date_moved_by(const semantic::Value &date, const semantic::Value &period) {
	if (date.kind != semantic::Kind::Date || date.date.precision != temporal::Precision::Day
			|| period.kind != semantic::Kind::Duration || period.unit == DurationUnit::BusinessDay)
		return std::nullopt;
	const std::optional<Moved> end = moved(date, period);
	if (!end || end->range || end->interval.precision != temporal::Precision::Day) return std::nullopt;
	return semantic::make_date(end->interval);
}

} // namespace singularity::protected_api::papinian::compute
