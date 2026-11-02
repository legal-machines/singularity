#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// The arithmetic, comparison, rate and logic family (DESIGN4 §2.5, §3.3):
// add, subtract, multiply, divide, sum, average, min, max, abs, round,
// positive_part, the comparisons, between, compare, sign, apply_rate, ratio,
// percentage_of, percentage_change, and, or, not, if_then_else - each a typer
// (from declared types only, never a value) and an evaluator (exact, over the
// real values, on this computer), registered through register_family. A date
// added to or subtracted from is the temporal family's (add_duration,
// duration_between): one executor, one table of operations.
namespace singularity::protected_api::papinian::compute {

namespace {

using semantic::DurationUnit;
using semantic::RateBasis;
using semantic::Rational;
using semantic::Value;

enum class A {
	Add, Subtract, Multiply, Divide, Sum, Average, Min, Max, Abs, PositivePart, Round,
	Equal, NotEqual, Greater, Less, GreaterOrEqual, LessOrEqual, Between, Compare, Sign,
	ApplyRate, Ratio, PercentageOf, PercentageChange, And, Or, Not, IfThenElse,
};

bool is_proportion(TypeKind kind) { return kind == TypeKind::Percentage || kind == TypeKind::Rate; }
bool is_count_like(TypeKind kind) { return kind == TypeKind::Number || kind == TypeKind::Count; }
bool is_numeric(TypeKind kind) {
	return kind == TypeKind::Money || is_count_like(kind) || is_proportion(kind) || kind == TypeKind::Duration;
}
bool is_ordered(TypeKind kind) { return is_numeric(kind) || kind == TypeKind::Date; }

// ---------------------------------------------------------------------------
// Units and periods: exact conversions only (DESIGN4 §3.2).

std::optional<long long> minutes_in(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Minute: return 1;
		case DurationUnit::Hour: return 60;
		case DurationUnit::Day: return 1440;
		case DurationUnit::Week: return 10080;
		default: return std::nullopt;
	}
}

std::optional<long long> months_in(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Month: return 1;
		case DurationUnit::Quarter: return 3;
		case DurationUnit::Year: return 12;
		default: return std::nullopt;
	}
}

bool convertible(DurationUnit a, DurationUnit b) {
	if (a == b) return true;
	return (minutes_in(a) && minutes_in(b)) || (months_in(a) && months_in(b));
}

DurationUnit finer(DurationUnit a, DurationUnit b) {
	if (a == b) return a;
	if (minutes_in(a) && minutes_in(b)) return *minutes_in(a) <= *minutes_in(b) ? a : b;
	if (months_in(a) && months_in(b)) return *months_in(a) <= *months_in(b) ? a : b;
	return a;
}

Rational to_unit(const Rational &count, DurationUnit from, DurationUnit to) {
	if (from == to) return count;
	if (minutes_in(from) && minutes_in(to)) return count * Rational(*minutes_in(from)) / Rational(*minutes_in(to));
	if (months_in(from) && months_in(to)) return count * Rational(*months_in(from)) / Rational(*months_in(to));
	throw semantic::AlgebraError("type", "units do not convert");
}

// Whether a duration in `unit` counts periods of `basis` exactly.
bool spans(RateBasis basis, DurationUnit unit) {
	switch (basis) {
		case RateBasis::PerDay: return minutes_in(unit).has_value();
		case RateBasis::PerMonth:
		case RateBasis::PerYear: return months_in(unit).has_value();
		case RateBasis::None: return false;
	}
	return false;
}

// How many periods of `basis` a duration is.
Rational periods(const Value &duration, RateBasis basis) {
	// A plain count is that many of the rate's own periods ("Days overdue: 40"
	// against a rate per day).
	if (duration.kind != semantic::Kind::Duration && basis != RateBasis::None) return duration.amount;
	switch (basis) {
		case RateBasis::PerDay:
			if (const auto minutes = minutes_in(duration.unit)) return duration.amount * Rational(*minutes) / Rational(1440);
			break;
		case RateBasis::PerMonth:
			if (const auto months = months_in(duration.unit)) return duration.amount * Rational(*months);
			break;
		case RateBasis::PerYear:
			if (const auto months = months_in(duration.unit)) return duration.amount * Rational(*months) / Rational(12);
			break;
		case RateBasis::None: break;
	}
	throw semantic::AlgebraError("type", "per_period");
}

std::optional<RateBasis> basis_of_unit(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Day:
		case DurationUnit::Week: return RateBasis::PerDay;
		case DurationUnit::Month:
		case DurationUnit::Quarter: return RateBasis::PerMonth;
		case DurationUnit::Year: return RateBasis::PerYear;
		default: return std::nullopt;
	}
}

// ---------------------------------------------------------------------------
// Typing (DESIGN4 §2.5): declared types only.

std::optional<ResultType> fail(TypeError &error, Code code, std::size_t argument, const std::string &expected,
		const ArgType *got = nullptr) {
	error.code = code;
	error.argument = argument;
	error.expected = expected;
	error.got = got != nullptr ? written(*got) : std::string();
	return std::nullopt;
}

ResultType value_result(ArgType type) {
	ResultType result;
	result.type = std::move(type);
	return result;
}

ResultType relation_result(TypeKind kind) {
	ResultType result;
	result.type.kind = kind;
	result.relation = true;
	return result;
}

bool join_currency(const ArgType &a, const ArgType &b, std::string &joined) {
	const std::string left = a.currency.empty() ? std::string("?") : a.currency;
	const std::string right = b.currency.empty() ? std::string("?") : b.currency;
	if (left == "?") joined = right;
	else if (right == "?" || left == right) joined = left;
	else return false;
	return true;
}

// Two operands of one quantity (add, subtract, sum, min, max, comparisons):
// the type they share, or the error that says why not.
std::optional<ArgType> same_quantity(const ArgType &a, const ArgType &b, std::size_t argument, TypeError &error) {
	if (a.kind == TypeKind::Money && b.kind == TypeKind::Money) {
		std::string currency;
		if (!join_currency(a, b, currency)) {
			fail(error, Code::CurrencyMismatch, argument, written(a), &b);
			return std::nullopt;
		}
		// A period not stated is the other's, as an unknown currency is: only
		// two stated periods that differ are a mismatch.
		if (a.basis != b.basis && a.basis != RateBasis::None && b.basis != RateBasis::None) {
			fail(error, Code::BasisMismatch, argument, written(a), &b);
			return std::nullopt;
		}
		ArgType joined = a;
		joined.currency = currency;
		if (joined.basis == RateBasis::None) joined.basis = b.basis;
		return joined;
	}
	if (is_proportion(a.kind) && is_proportion(b.kind)) {
		if (a.basis != b.basis && a.basis != RateBasis::None && b.basis != RateBasis::None) {
			fail(error, Code::BasisMismatch, argument, written(a), &b);
			return std::nullopt;
		}
		ArgType joined = a;
		if (joined.basis == RateBasis::None) joined.basis = b.basis;
		if (b.kind == TypeKind::Rate || joined.basis != RateBasis::None) joined.kind = TypeKind::Rate;
		return joined;
	}
	if (a.kind == TypeKind::Duration && b.kind == TypeKind::Duration) {
		const bool business = a.unit == DurationUnit::BusinessDay;
		if (business != (b.unit == DurationUnit::BusinessDay) || !convertible(a.unit, b.unit)) {
			fail(error, Code::TypeMismatch, argument, written(a), &b);
			return std::nullopt;
		}
		ArgType joined = a;
		joined.unit = finer(a.unit, b.unit);
		return joined;
	}
	if (is_count_like(a.kind) && is_count_like(b.kind)) {
		ArgType joined;
		joined.kind = a.kind == TypeKind::Count && b.kind == TypeKind::Count ? TypeKind::Count : TypeKind::Number;
		return joined;
	}
	if (a.kind == TypeKind::Date && b.kind == TypeKind::Date) return a;
	fail(error, Code::TypeMismatch, argument, written(a), &b);
	return std::nullopt;
}

OpOptions with_unit(const OpOptions &options, const char *unit) {
	OpOptions changed = options;
	if (!changed.unit) changed.unit = unit;
	return changed;
}

std::optional<ResultType> delegate_type(const char *op, const std::vector<ArgType> &arguments, const OpOptions &options,
		TypeError &error) {
	const OpEntry *entry = find_op(op);
	if (entry == nullptr || entry->typer == nullptr) return fail(error, Code::UnknownOp, 0, op);
	return entry->typer(arguments, options, error);
}

std::optional<ResultType> type_arith(A op, const std::vector<ArgType> &args, const OpOptions &options, TypeError &error) {
	const auto arity = [&](std::size_t low, std::size_t high) {
		return args.size() >= low && args.size() <= high;
	};
	const bool reveal = !options.reveal || *options.reveal;
	switch (op) {
		case A::Add:
		case A::Subtract: {
			// Quantities take any number of operands (subtract: the first less
			// all the rest, as a ledger's "billed - paid - offset"); a date takes
			// one period or one date.
			if (args.size() < 2) return fail(error, Code::Arity, 0, "2 or more operands");
			const bool dated = std::any_of(args.begin(), args.end(), [](const ArgType &arg) { return arg.kind == TypeKind::Date; });
			if (dated && args.size() != 2) return fail(error, Code::Arity, 0, "2 operands with a date");
			if (args[0].kind == TypeKind::Date && args[1].kind == TypeKind::Duration)
				return delegate_type("add_duration", args, options, error);
			if (op == A::Subtract && args[0].kind == TypeKind::Date && args[1].kind == TypeKind::Date)
				return delegate_type("duration_between", {args[1], args[0]}, with_unit(options, "days"), error);
			if (dated) return fail(error, Code::TypeMismatch, 2, "DURATION", &args[1]);
			if (!is_numeric(args[0].kind)) return fail(error, Code::TypeMismatch, 1, "a quantity", &args[0]);
			ArgType joined = args[0];
			for (std::size_t index = 1; index < args.size(); ++index) {
				const auto next = same_quantity(joined, args[index], index + 1, error);
				if (!next) return std::nullopt;
				joined = *next;
			}
			return value_result(joined);
		}
		case A::Multiply: {
			if (!arity(2, 2)) return fail(error, Code::Arity, 0, "2 operands");
			for (int turn = 0; turn < 2; ++turn) {
				const ArgType &x = args[turn == 0 ? 0 : 1];
				const ArgType &y = args[turn == 0 ? 1 : 0];
				if (x.kind == TypeKind::Money) {
					if (is_count_like(y.kind)) {
						ArgType result = x;
						result.basis = RateBasis::None;
						return value_result(result);
					}
					if (is_proportion(y.kind)) {
						if (y.basis == RateBasis::None) return value_result(x);
						if (x.basis != RateBasis::None) return fail(error, Code::BasisMismatch, turn == 0 ? 2 : 1, "PERCENTAGE", &y);
						ArgType result = x;
						result.basis = y.basis;
						return value_result(result);
					}
					if (y.kind == TypeKind::Duration) {
						if (x.basis == RateBasis::None || !spans(x.basis, y.unit))
							return fail(error, Code::PerPeriod, turn == 0 ? 2 : 1, "a period the amount is owed for", &y);
						ArgType result = x;
						result.basis = RateBasis::None;
						return value_result(result);
					}
					if (y.kind == TypeKind::Money) return fail(error, Code::TypeMismatch, turn == 0 ? 2 : 1, "NUMBER", &y);
				}
				if (is_proportion(x.kind) && y.kind == TypeKind::Duration) {
					if (x.basis == RateBasis::None || !spans(x.basis, y.unit))
						return fail(error, Code::PerPeriod, turn == 0 ? 2 : 1, "a period the rate is for", &y);
					ArgType result;
					result.kind = TypeKind::Percentage;
					return value_result(result);
				}
			}
			const ArgType &a = args[0];
			const ArgType &b = args[1];
			if (a.kind == TypeKind::Date || b.kind == TypeKind::Date) return fail(error, Code::TypeMismatch, 1, "a quantity", &a);
			if (is_count_like(a.kind) && is_count_like(b.kind)) {
				ArgType result;
				result.kind = a.kind == TypeKind::Count && b.kind == TypeKind::Count ? TypeKind::Count : TypeKind::Number;
				return value_result(result);
			}
			if (is_proportion(a.kind) && is_count_like(b.kind)) return value_result(a);
			if (is_count_like(a.kind) && is_proportion(b.kind)) return value_result(b);
			if (is_proportion(a.kind) && is_proportion(b.kind)) {
				ArgType result;
				result.kind = TypeKind::Percentage;
				return value_result(result);
			}
			if (a.kind == TypeKind::Duration && is_count_like(b.kind)) return value_result(a);
			if (is_count_like(a.kind) && b.kind == TypeKind::Duration) return value_result(b);
			return fail(error, Code::TypeMismatch, 2, "a quantity this one scales", &b);
		}
		case A::Divide: {
			if (!arity(2, 2)) return fail(error, Code::Arity, 0, "2 operands");
			const ArgType &a = args[0];
			const ArgType &b = args[1];
			if (a.kind == TypeKind::Money) {
				if (b.kind == TypeKind::Money) {
					std::string currency;
					if (!join_currency(a, b, currency)) return fail(error, Code::CurrencyMismatch, 2, written(a), &b);
					if (a.basis != b.basis) return fail(error, Code::BasisMismatch, 2, written(a), &b);
					ArgType result;
					result.kind = TypeKind::Rate;
					return value_result(result);
				}
				if (is_count_like(b.kind)) return value_result(a);
				if (b.kind == TypeKind::Duration) {
					const auto basis = basis_of_unit(b.unit);
					if (a.basis != RateBasis::None || !basis) return fail(error, Code::PerPeriod, 2, "days, weeks, months, quarters or years", &b);
					ArgType result = a;
					result.basis = *basis;
					return value_result(result);
				}
				return fail(error, Code::TypeMismatch, 2, "MONEY, NUMBER, COUNT or DURATION", &b);
			}
			if (a.kind == TypeKind::Duration && b.kind == TypeKind::Duration) {
				if ((a.unit == DurationUnit::BusinessDay) != (b.unit == DurationUnit::BusinessDay) || !convertible(a.unit, b.unit))
					return fail(error, Code::TypeMismatch, 2, written(a), &b);
				ArgType result;
				result.kind = TypeKind::Number;
				return value_result(result);
			}
			if (a.kind == TypeKind::Count && b.kind == TypeKind::Count) {
				ArgType result;
				result.kind = TypeKind::Rate;
				return value_result(result);
			}
			if (is_count_like(a.kind) && is_count_like(b.kind)) {
				ArgType result;
				result.kind = TypeKind::Number;
				return value_result(result);
			}
			if (is_proportion(a.kind) && is_proportion(b.kind)) {
				ArgType result;
				result.kind = TypeKind::Number;
				return value_result(result);
			}
			if ((is_proportion(a.kind) || a.kind == TypeKind::Duration) && is_count_like(b.kind)) return value_result(a);
			return fail(error, Code::TypeMismatch, 2, "a quantity this one divides by", &b);
		}
		case A::Sum:
		case A::Average:
		case A::Min:
		case A::Max: {
			if (args.size() < 2) return fail(error, Code::Arity, 0, "2 or more operands");
			const bool ordering = op == A::Min || op == A::Max;
			if (!(ordering ? is_ordered(args[0].kind) : is_numeric(args[0].kind)))
				return fail(error, Code::TypeMismatch, 1, "a quantity", &args[0]);
			ArgType joined = args[0];
			for (std::size_t index = 1; index < args.size(); ++index) {
				const auto next = same_quantity(joined, args[index], index + 1, error);
				if (!next) return std::nullopt;
				joined = *next;
			}
			if (op == A::Average && joined.kind == TypeKind::Count) joined.kind = TypeKind::Number;
			return value_result(joined);
		}
		case A::Abs:
		case A::PositivePart:
		case A::Round: {
			if (!arity(1, 1)) return fail(error, Code::Arity, 0, "1 operand");
			if (!is_numeric(args[0].kind)) return fail(error, Code::TypeMismatch, 1, "a quantity", &args[0]);
			if (op == A::Round && options.places && (*options.places < 0 || *options.places > kMaxPlaces))
				return fail(error, Code::TypeMismatch, 0, "places 0-9");
			return value_result(args[0]);
		}
		case A::Equal:
		case A::NotEqual:
		case A::Greater:
		case A::Less:
		case A::GreaterOrEqual:
		case A::LessOrEqual:
		case A::Compare: {
			if (!arity(2, 2)) return fail(error, Code::Arity, 0, "2 operands");
			const bool equality = op == A::Equal || op == A::NotEqual;
			if (equality && args[0].kind == TypeKind::Entity && args[1].kind == TypeKind::Entity)
				return relation_result(TypeKind::Boolean);
			if (!(equality ? (is_ordered(args[0].kind) || args[0].kind == TypeKind::Boolean) : is_ordered(args[0].kind)))
				return fail(error, Code::TypeMismatch, 1, "a quantity or a date", &args[0]);
			// A day of the year ("each July 1") has no year: it meets a date through
			// the day it next falls on (a fake-provider run on E1: "not evaluable").
			if (args[0].kind == TypeKind::Date && args[1].kind == TypeKind::Date
					&& (args[0].precision == "day of year") != (args[1].precision == "day of year")) {
				const std::size_t which = args[0].precision == "day of year" ? 1 : 2;
				fail(error, Code::TypeMismatch, which, "DATE(day)", &args[which - 1]);
				error.hint = "a day of the year has no year: compare the day it next falls on - next_occurrence(that day, "
					"a date) - with the date";
				return std::nullopt;
			}
			if (args[0].kind == TypeKind::Boolean) {
				if (args[1].kind != TypeKind::Boolean) return fail(error, Code::TypeMismatch, 2, "BOOLEAN", &args[1]);
			} else if (!same_quantity(args[0], args[1], 2, error)) {
				return std::nullopt;
			}
			if (op == A::Compare) return relation_result(TypeKind::Order);
			if (!reveal) {
				ArgType flag;
				flag.kind = TypeKind::Boolean;
				return value_result(flag);
			}
			return relation_result(TypeKind::Boolean);
		}
		case A::Between: {
			if (!arity(3, 3)) return fail(error, Code::Arity, 0, "3 operands");
			if (!is_ordered(args[0].kind)) return fail(error, Code::TypeMismatch, 1, "a quantity or a date", &args[0]);
			if (!same_quantity(args[0], args[1], 2, error) || !same_quantity(args[0], args[2], 3, error)) return std::nullopt;
			if (!reveal) {
				ArgType flag;
				flag.kind = TypeKind::Boolean;
				return value_result(flag);
			}
			return relation_result(TypeKind::Boolean);
		}
		case A::Sign: {
			if (!arity(1, 1)) return fail(error, Code::Arity, 0, "1 operand");
			if (!is_numeric(args[0].kind)) return fail(error, Code::TypeMismatch, 1, "a quantity", &args[0]);
			return relation_result(TypeKind::Sign);
		}
		case A::ApplyRate: {
			if (!arity(2, 3)) return fail(error, Code::Arity, 0, "2 or 3 operands");
			if (args[0].kind != TypeKind::Money) return fail(error, Code::TypeMismatch, 1, "MONEY", &args[0]);
			if (!is_proportion(args[1].kind)) return fail(error, Code::TypeMismatch, 2, "RATE or PERCENTAGE", &args[1]);
			if (options.mode && *options.mode != "share" && *options.mode != "increase" && *options.mode != "decrease")
				return fail(error, Code::TypeMismatch, 0, "mode share, increase or decrease");
			ArgType result = args[0];
			const RateBasis rate = args[1].basis;
			if (args.size() == 3) {
				const bool count = args[2].kind == TypeKind::Count || args[2].kind == TypeKind::Number;
				if (args[2].kind != TypeKind::Duration && !count)
					return fail(error, Code::TypeMismatch, 3, "DURATION or a COUNT of the rate's periods", &args[2]);
				if (rate == RateBasis::None || (!count && !spans(rate, args[2].unit)))
					return fail(error, Code::PerPeriod, 3, "a period the rate is for", &args[2]);
				if (options.mode && *options.mode != "share") return fail(error, Code::TypeMismatch, 0, "mode share over a period");
				return value_result(result);
			}
			if (rate != RateBasis::None) {
				if (args[0].basis != RateBasis::None) return fail(error, Code::BasisMismatch, 2, "PERCENTAGE", &args[1]);
				if (options.mode && *options.mode != "share") return fail(error, Code::TypeMismatch, 0, "mode share for a periodic rate");
				result.basis = rate;
			}
			return value_result(result);
		}
		case A::Ratio: {
			if (!arity(2, 2)) return fail(error, Code::Arity, 0, "2 operands");
			const TypeKind kind = args[0].kind;
			if (!(kind == TypeKind::Money || is_count_like(kind) || kind == TypeKind::Duration))
				return fail(error, Code::TypeMismatch, 1, "MONEY, NUMBER, COUNT or DURATION", &args[0]);
			if (!same_quantity(args[0], args[1], 2, error)) return std::nullopt;
			ArgType result;
			result.kind = TypeKind::Rate;
			return value_result(result);
		}
		case A::PercentageOf: {
			if (!arity(2, 2)) return fail(error, Code::Arity, 0, "2 operands");
			if (!is_proportion(args[0].kind) || args[0].basis != RateBasis::None)
				return fail(error, Code::TypeMismatch, 1, "PERCENTAGE", &args[0]);
			if (args[1].kind == TypeKind::Money) return value_result(args[1]);
			if (is_count_like(args[1].kind)) {
				ArgType result;
				result.kind = TypeKind::Number;
				return value_result(result);
			}
			return fail(error, Code::TypeMismatch, 2, "MONEY, NUMBER or COUNT", &args[1]);
		}
		case A::PercentageChange: {
			if (!arity(2, 2)) return fail(error, Code::Arity, 0, "2 operands");
			if (!(args[0].kind == TypeKind::Money || is_count_like(args[0].kind) || is_proportion(args[0].kind)))
				return fail(error, Code::TypeMismatch, 1, "MONEY, NUMBER or PERCENTAGE", &args[0]);
			if (!same_quantity(args[0], args[1], 2, error)) return std::nullopt;
			ArgType result;
			result.kind = TypeKind::Rate;
			return value_result(result);
		}
		case A::And:
		case A::Or:
		case A::Not: {
			if (op == A::Not ? !arity(1, 1) : args.size() < 2) return fail(error, Code::Arity, 0, op == A::Not ? "1 operand" : "2 or more operands");
			for (std::size_t index = 0; index < args.size(); ++index)
				if (args[index].kind != TypeKind::Boolean) return fail(error, Code::TypeMismatch, index + 1, "BOOLEAN", &args[index]);
			if (options.reveal && *options.reveal) return relation_result(TypeKind::Boolean);
			ArgType flag;
			flag.kind = TypeKind::Boolean;
			return value_result(flag);
		}
		case A::IfThenElse: {
			if (!arity(3, 3)) return fail(error, Code::Arity, 0, "3 operands");
			if (args[0].kind != TypeKind::Boolean) return fail(error, Code::TypeMismatch, 1, "BOOLEAN", &args[0]);
			if (!is_ordered(args[1].kind) && args[1].kind != TypeKind::Boolean)
				return fail(error, Code::TypeMismatch, 2, "a quantity or a date", &args[1]);
			if (args[1].kind == TypeKind::Boolean) {
				if (args[2].kind != TypeKind::Boolean) return fail(error, Code::TypeMismatch, 3, "BOOLEAN", &args[2]);
				return value_result(args[1]);
			}
			const auto joined = same_quantity(args[1], args[2], 3, error);
			if (!joined) return std::nullopt;
			return value_result(*joined);
		}
	}
	return fail(error, Code::UnknownOp, 0, "");
}

template <A op>
std::optional<ResultType> typer(const std::vector<ArgType> &args, const OpOptions &options, TypeError &error) {
	return type_arith(op, args, options, error);
}

// ---------------------------------------------------------------------------
// Evaluation: exact, over the real values (local only).

OpAnswer failed(Code code, std::size_t argument = 0) {
	OpAnswer answer;
	answer.status = OpStatus::Error;
	answer.code = code;
	answer.error.code = code;
	answer.error.argument = argument;
	return answer;
}

OpAnswer unknown(Code reason, const ResultType &result) {
	OpAnswer answer;
	answer.status = OpStatus::Unknown;
	answer.code = reason;
	answer.result = result;
	answer.certainty.kind = Certainty::Kind::Unknown;
	answer.certainty.reason = reason;
	return answer;
}

// The zero of a value's own kind: "0" meets any quantity (DESIGN4 §2.5).
Value zero_like(const Value &like) {
	Value zero = like;
	zero.amount = Rational(0);
	return zero;
}

Value shaped(Value value, const ArgType &type) {
	switch (type.kind) {
		case TypeKind::Money:
			value.kind = semantic::Kind::Money;
			if (type.currency != "?" && !type.currency.empty()) value.currency = type.currency;
			value.basis = type.basis;
			break;
		case TypeKind::Number: value.kind = semantic::Kind::Number; value.basis = RateBasis::None; break;
		case TypeKind::Count: value.kind = semantic::Kind::Count; value.basis = RateBasis::None; break;
		case TypeKind::Percentage:
		case TypeKind::Rate: value.kind = semantic::Kind::Percentage; value.basis = type.basis; break;
		case TypeKind::Duration: value.kind = semantic::Kind::Duration; value.unit = type.unit; break;
		case TypeKind::Boolean: value.kind = semantic::Kind::Boolean; break;
		default: break;
	}
	return value;
}

// A quantity's amount in the unit `type` asks for (durations convert exactly).
Rational amount_in(const Value &value, const ArgType &type) {
	if (value.kind == semantic::Kind::Duration && type.kind == TypeKind::Duration) return to_unit(value.amount, value.unit, type.unit);
	return value.amount;
}

std::string sign_word(int sign) { return sign < 0 ? "NEGATIVE" : sign > 0 ? "POSITIVE" : "ZERO"; }

Value add_values(const Value &a, const Value &b, const ArgType &type, bool subtract) {
	const Rational left = amount_in(a, type);
	const Rational right = amount_in(b, type);
	Value result = shaped(a, type);
	result.amount = subtract ? left - right : left + right;
	result.source_decimals = std::max(a.source_decimals, b.source_decimals);
	return result;
}

const Value *value_of(const Operand &operand) { return operand.value ? &*operand.value : nullptr; }

// A worked-out rate or number set against a figure the material states to fewer
// places ("5.48%", a ratio that is 5.4849...%): equal as written when the
// worked-out value, rounded to the places the figure is written with, is it.
// Never for money, where a cent is a cent.
bool equal_as_written(const std::vector<Operand> &operands) {
	if (operands.size() != 2 || !operands[0].value || !operands[1].value) return false;
	for (int stated = 0; stated < 2; ++stated) {
		const Operand &written = operands[stated];
		const Operand &worked = operands[1 - stated];
		if (worked.derived == nullptr || written.derived != nullptr) continue;
		const Value &a = *worked.value;
		const Value &b = *written.value;
		if (a.kind != b.kind || (a.kind != semantic::Kind::Percentage && a.kind != semantic::Kind::Number)) continue;
		const std::string text = written.row != nullptr ? written.row->original : written.arg.constant;
		if (text.empty()) continue;
		// The places of the figure as written: digits after its decimal mark.
		std::size_t first = text.find_first_of("0123456789");
		if (first == std::string::npos) continue;
		std::size_t end = first;
		while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) != 0 || text[end] == '.' || text[end] == ','
				|| text[end] == ' '))
			++end;
		const std::string number = text.substr(first, end - first);
		const std::size_t mark = number.find_last_of(".,");
		int places = 0;
		if (mark != std::string::npos) {
			const std::size_t after = number.size() - mark - 1;
			// "1,000" is a thousands mark, not a decimal one.
			if (after != 3 || number[mark] == '.') places = static_cast<int>(after);
		}
		if (places > 6) continue;
		const bool percent = a.kind == semantic::Kind::Percentage && text.find('%') != std::string::npos;
		const Rational scale(percent ? 100 : 1);
		try {
			if ((a.amount * scale).rounded(places) == (b.amount * scale).rounded(places)) return true;
		} catch (const semantic::AlgebraError &) {
		}
	}
	return false;
}

bool bounded_operand(const Operand &operand) {
	return operand.derived != nullptr && operand.derived->certainty.kind != Certainty::Kind::Exact;
}

OpAnswer delegate(const char *op, const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	const OpEntry *entry = find_op(op);
	if (entry == nullptr || entry->evaluator == nullptr) return failed(Code::UnknownOp);
	return entry->evaluator(operands, options, context);
}

semantic::Relation relate(const Value &a, semantic::Comparator comparator, const Value &b, OpContext &context) {
	return semantic::compare(a, comparator, b, context.language);
}

OpAnswer eval_arith(A op, const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	std::vector<ArgType> types;
	for (const Operand &operand : operands) types.push_back(operand.type);
	TypeError typing;
	const auto result = type_arith(op, types, options, typing);
	if (!result) {
		OpAnswer answer = failed(typing.code, typing.argument);
		answer.error = typing;
		return answer;
	}
	// A date meets a period or another date in the temporal family.
	if ((op == A::Add || op == A::Subtract) && !operands.empty() && operands[0].type.kind == TypeKind::Date) {
		if (operands.size() == 2 && operands[1].type.kind == TypeKind::Duration) {
			std::vector<Operand> moved = operands;
			if (op == A::Subtract && moved[1].value) moved[1].value->amount = -moved[1].value->amount;
			return delegate("add_duration", moved, options, context);
		}
		if (op == A::Subtract && operands.size() == 2)
			return delegate("duration_between", {operands[1], operands[0]}, with_unit(options, "days"), context);
	}
	OpAnswer answer;
	answer.result = *result;
	for (const Operand &operand : operands) answer.roots.insert(operand.roots.begin(), operand.roots.end());
	for (std::size_t index = 0; index < operands.size(); ++index) {
		if (operands[index].type.kind == TypeKind::Entity) continue;
		if (value_of(operands[index]) == nullptr) return failed(Code::DependencyFailed, index + 1);
		// A bounded value is not worked with further here: the answer is honest.
		if (bounded_operand(operands[index])) return unknown(Code::BoundedOperand, *result);
	}
	const bool reveal = !options.reveal || *options.reveal;
	const auto ok_value = [&](Value value) {
		answer.status = OpStatus::Ok;
		answer.value = shaped(std::move(value), answer.result.type);
		answer.certainty.kind = Certainty::Kind::Exact;
		return answer;
	};
	const auto truth_answer = [&](semantic::Truth truth) {
		if (truth == semantic::Truth::Undetermined) return unknown(Code::PrecisionInsufficient, answer.result);
		if (!answer.result.relation) return ok_value(semantic::make_boolean(truth == semantic::Truth::True));
		answer.status = OpStatus::Ok;
		answer.relation = truth == semantic::Truth::True ? "TRUE" : "FALSE";
		answer.certainty.kind = Certainty::Kind::Exact;
		return answer;
	};
	try {
		switch (op) {
			case A::Add:
			case A::Subtract: {
				Value difference = add_values(*operands[0].value, *operands[1].value, answer.result.type, op == A::Subtract);
				for (std::size_t index = 2; index < operands.size(); ++index)
					difference = add_values(difference, *operands[index].value, answer.result.type, op == A::Subtract);
				if (op == A::Subtract && reveal) answer.sign = sign_word(difference.amount.sign());
				return ok_value(std::move(difference));
			}
			case A::Sum:
			case A::Average: {
				Value total = shaped(*operands[0].value, answer.result.type);
				total.amount = amount_in(*operands[0].value, answer.result.type);
				for (std::size_t index = 1; index < operands.size(); ++index)
					total = add_values(total, *operands[index].value, answer.result.type, false);
				if (op == A::Average) total.amount = total.amount / Rational(static_cast<semantic::Int128>(operands.size()));
				return ok_value(std::move(total));
			}
			case A::Min:
			case A::Max: {
				std::size_t pick = 0;
				for (std::size_t index = 1; index < operands.size(); ++index) {
					const semantic::Relation less = relate(*operands[index].value, semantic::Comparator::Less, *operands[pick].value, context);
					const semantic::Relation greater = relate(*operands[index].value, semantic::Comparator::Greater, *operands[pick].value, context);
					if (less.truth == semantic::Truth::Undetermined || greater.truth == semantic::Truth::Undetermined)
						return unknown(Code::PrecisionInsufficient, answer.result);
					if ((op == A::Min && less.truth == semantic::Truth::True) || (op == A::Max && greater.truth == semantic::Truth::True))
						pick = index;
				}
				if (answer.result.type.kind == TypeKind::Date) {
					answer.status = OpStatus::Ok;
					answer.value = *operands[pick].value;
					answer.certainty.kind = Certainty::Kind::Exact;
					return answer;
				}
				Value chosen = shaped(*operands[pick].value, answer.result.type);
				chosen.amount = amount_in(*operands[pick].value, answer.result.type);
				return ok_value(std::move(chosen));
			}
			case A::Abs: {
				Value value = *operands[0].value;
				value.amount = value.amount.abs();
				return ok_value(std::move(value));
			}
			case A::PositivePart: {
				Value value = *operands[0].value;
				if (value.amount.sign() < 0) value.amount = Rational(0);
				return ok_value(std::move(value));
			}
			case A::Round: {
				Value value = *operands[0].value;
				int places = options.places ? *options.places
					: value.kind == semantic::Kind::Money ? semantic::minor_unit_places(value.currency) : 0;
				semantic::ExactRational::Rounding mode = semantic::ExactRational::Rounding::HalfUp;
				if (options.rounding) {
					if (*options.rounding == "half_even") mode = semantic::ExactRational::Rounding::HalfEven;
					else if (*options.rounding == "down") mode = semantic::ExactRational::Rounding::Down;
					else if (*options.rounding == "up") mode = semantic::ExactRational::Rounding::Up;
				}
				const auto rounded = semantic::ExactRational::from(value.amount).rounded(places, mode).to_rational();
				if (!rounded) return unknown(Code::NotEvaluable, answer.result);
				value.amount = *rounded;
				return ok_value(std::move(value));
			}
			case A::Multiply: {
				const Value &a = *operands[0].value;
				const Value &b = *operands[1].value;
				// A per-period quantity over a duration spends the period.
				const auto spent = [&](const Value &per, const Value &duration) {
					return per.amount * periods(duration, per.basis);
				};
				Value product = shaped(a, answer.result.type);
				if ((a.kind == semantic::Kind::Money || a.kind == semantic::Kind::Percentage) && b.kind == semantic::Kind::Duration)
					product.amount = spent(a, b);
				else if ((b.kind == semantic::Kind::Money || b.kind == semantic::Kind::Percentage) && a.kind == semantic::Kind::Duration)
					product.amount = spent(b, a);
				else
					product.amount = a.amount * b.amount;
				if (a.kind == semantic::Kind::Money) product.currency = a.currency;
				else if (b.kind == semantic::Kind::Money) product.currency = b.currency;
				if (answer.result.type.kind == TypeKind::Duration)
					product.unit = a.kind == semantic::Kind::Duration ? a.unit : b.unit;
				return ok_value(std::move(product));
			}
			case A::Divide: {
				const Value &a = *operands[0].value;
				const Value &b = *operands[1].value;
				Rational divisor = b.amount;
				Rational dividend = a.amount;
				if (a.kind == semantic::Kind::Duration && b.kind == semantic::Kind::Duration) {
					const DurationUnit unit = finer(a.unit, b.unit);
					dividend = to_unit(a.amount, a.unit, unit);
					divisor = to_unit(b.amount, b.unit, unit);
				} else if (a.kind == semantic::Kind::Money && b.kind == semantic::Kind::Duration) {
					divisor = periods(b, answer.result.type.basis);
				}
				if (divisor.is_zero()) return unknown(Code::DivisionByZero, answer.result);
				Value quotient = shaped(a, answer.result.type);
				quotient.amount = dividend / divisor;
				return ok_value(std::move(quotient));
			}
			case A::Equal:
			case A::NotEqual:
			case A::Greater:
			case A::Less:
			case A::GreaterOrEqual:
			case A::LessOrEqual: {
				if (operands[0].type.kind == TypeKind::Entity) {
					// Symbol identity: static, never a value.
					const bool same = operands[0].fact_key == operands[1].fact_key && !operands[0].fact_key.empty();
					answer.status = OpStatus::Ok;
					answer.relation = (op == A::Equal) == same ? "TRUE" : "FALSE";
					return answer;
				}
				const semantic::Comparator comparator = op == A::Equal ? semantic::Comparator::Equal
					: op == A::NotEqual ? semantic::Comparator::NotEqual
					: op == A::Greater ? semantic::Comparator::Greater
					: op == A::Less ? semantic::Comparator::Less
					: op == A::GreaterOrEqual ? semantic::Comparator::GreaterOrEqual : semantic::Comparator::LessOrEqual;
				semantic::Truth truth = relate(*operands[0].value, comparator, *operands[1].value, context).truth;
				// The relation is the exact one; that the two are the same figure to
				// the places one is written with is said beside it, never instead:
				// a worked-out 4.75% is not a contract's "5%" (live EN1, 27 Sep 2026).
				OpAnswer decided = truth_answer(truth);
				if ((op == A::Equal || op == A::NotEqual) && truth != semantic::Truth::Undetermined
						&& (truth == semantic::Truth::True) != (op == A::Equal) && equal_as_written(operands))
					decided.words = "as_written";
				return decided;
			}
			case A::Between: {
				const semantic::Relation low = relate(*operands[0].value, semantic::Comparator::GreaterOrEqual, *operands[1].value, context);
				const semantic::Relation high = relate(*operands[0].value, semantic::Comparator::LessOrEqual, *operands[2].value, context);
				if (low.truth == semantic::Truth::False || high.truth == semantic::Truth::False) return truth_answer(semantic::Truth::False);
				if (low.truth == semantic::Truth::True && high.truth == semantic::Truth::True) return truth_answer(semantic::Truth::True);
				return truth_answer(semantic::Truth::Undetermined);
			}
			case A::Compare: {
				const semantic::Relation less = relate(*operands[0].value, semantic::Comparator::Less, *operands[1].value, context);
				const semantic::Relation equal = relate(*operands[0].value, semantic::Comparator::Equal, *operands[1].value, context);
				const semantic::Relation greater = relate(*operands[0].value, semantic::Comparator::Greater, *operands[1].value, context);
				answer.status = OpStatus::Ok;
				answer.certainty.kind = Certainty::Kind::Exact;
				if (less.truth == semantic::Truth::True) answer.relation = "LESS";
				else if (equal.truth == semantic::Truth::True) answer.relation = "EQUAL";
				else if (greater.truth == semantic::Truth::True) answer.relation = "GREATER";
				else return unknown(Code::PrecisionInsufficient, answer.result);
				if (equal.truth != semantic::Truth::True && equal_as_written(operands)) answer.words = "as_written";
				return answer;
			}
			case A::Sign: {
				answer.status = OpStatus::Ok;
				answer.certainty.kind = Certainty::Kind::Exact;
				answer.relation = sign_word(operands[0].value->amount.sign());
				return answer;
			}
			case A::ApplyRate: {
				const Value &amount = *operands[0].value;
				const Value &rate = *operands[1].value;
				Rational fraction = rate.amount;
				if (operands.size() == 3) fraction = fraction * periods(*operands[2].value, rate.basis);
				const std::string mode = options.mode.value_or("share");
				Value applied = shaped(amount, answer.result.type);
				applied.amount = mode == "increase" ? amount.amount * (Rational(1) + fraction)
					: mode == "decrease" ? amount.amount * (Rational(1) - fraction) : amount.amount * fraction;
				return ok_value(std::move(applied));
			}
			case A::Ratio: {
				const Value &a = *operands[0].value;
				const Value &b = *operands[1].value;
				Rational divisor = b.amount;
				Rational dividend = a.amount;
				if (a.kind == semantic::Kind::Duration) {
					const DurationUnit unit = finer(a.unit, b.unit);
					dividend = to_unit(a.amount, a.unit, unit);
					divisor = to_unit(b.amount, b.unit, unit);
				}
				if (divisor.is_zero()) return unknown(Code::DivisionByZero, answer.result);
				return ok_value(semantic::make_percentage(dividend / divisor));
			}
			case A::PercentageOf: {
				Value part = shaped(*operands[1].value, answer.result.type);
				part.amount = operands[0].value->amount * operands[1].value->amount;
				return ok_value(std::move(part));
			}
			case A::PercentageChange: {
				const Rational before = operands[0].value->amount;
				if (before.is_zero()) return unknown(Code::DivisionByZero, answer.result);
				return ok_value(semantic::make_percentage((operands[1].value->amount - before) / before));
			}
			case A::And:
			case A::Or:
			case A::Not: {
				bool truth = op != A::Or;
				if (op == A::Not) truth = !operands[0].value->truth;
				else
					for (const Operand &operand : operands) truth = op == A::And ? truth && operand.value->truth : truth || operand.value->truth;
				return truth_answer(truth ? semantic::Truth::True : semantic::Truth::False);
			}
			case A::IfThenElse: {
				const Operand &chosen = operands[0].value->truth ? operands[1] : operands[2];
				if (answer.result.type.kind == TypeKind::Date) {
					answer.status = OpStatus::Ok;
					answer.value = *chosen.value;
					answer.certainty.kind = Certainty::Kind::Exact;
					return answer;
				}
				Value value = shaped(*chosen.value, answer.result.type);
				value.amount = amount_in(*chosen.value, answer.result.type);
				return ok_value(std::move(value));
			}
		}
	} catch (const semantic::AlgebraError &) {
		return failed(Code::NotEvaluable);
	}
	return failed(Code::UnknownOp);
}

template <A op>
OpAnswer evaluator(const std::vector<Operand> &operands, const OpOptions &options, OpContext &context) {
	return eval_arith(op, operands, options, context);
}

// ---------------------------------------------------------------------------
// Pre-charges (DESIGN4 §8.3): from the AST and declared types only. The
// executor keys each charge by its canonical form and keeps only the hidden
// roots.

ObservationCharge roots_charge(const std::vector<Operand> &operands) {
	ObservationCharge charge;
	std::set<std::string> roots;
	for (const Operand &operand : operands) roots.insert(operand.roots.begin(), operand.roots.end());
	charge.roots.assign(roots.begin(), roots.end());
	return charge;
}

// A relation word is an observation; a Flag (reveal:false) is not.
std::optional<ObservationCharge> charge_relation(const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &) {
	if (options.reveal && !*options.reveal) return std::nullopt;
	for (const Operand &operand : operands)
		if (operand.type.kind == TypeKind::Entity) return std::nullopt;
	return roots_charge(operands);
}

std::optional<ObservationCharge> charge_always(const std::vector<Operand> &operands, const OpOptions &, const OpContext &) {
	return roots_charge(operands);
}

// Logic stays a Flag unless the model asks to see it.
std::optional<ObservationCharge> charge_logic(const std::vector<Operand> &operands, const OpOptions &options, const OpContext &) {
	if (!options.reveal || !*options.reveal) return std::nullopt;
	return roots_charge(operands);
}

std::optional<ObservationCharge> delegate_charge(const char *op, const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &context) {
	const OpEntry *entry = find_op(op);
	if (entry == nullptr || entry->pre_charge == nullptr) return std::nullopt;
	return entry->pre_charge(operands, options, context);
}

// A difference returns its sign unless reveal:false; a date meets the
// temporal family's own charge.
std::optional<ObservationCharge> charge_subtract(const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &context) {
	if (operands.size() == 2 && operands[0].type.kind == TypeKind::Date) {
		if (operands[1].type.kind == TypeKind::Duration) return delegate_charge("add_duration", operands, options, context);
		return delegate_charge("duration_between", {operands[1], operands[0]}, with_unit(options, "days"), context);
	}
	if (options.reveal && !*options.reveal) return std::nullopt;
	return roots_charge(operands);
}

std::optional<ObservationCharge> charge_add(const std::vector<Operand> &operands, const OpOptions &options,
		const OpContext &context) {
	if (operands.size() == 2 && operands[0].type.kind == TypeKind::Date && operands[1].type.kind == TypeKind::Duration)
		return delegate_charge("add_duration", operands, options, context);
	return std::nullopt;
}

// Whether a divisor is zero is one observation per canonical divisor for the
// life of the matter (its zero allowance), never part of the root's budget.
std::optional<ObservationCharge> zero_charge(const Operand &divisor) {
	if (divisor.roots.empty()) return std::nullopt;
	ObservationCharge charge;
	charge.key = "zero:" + (divisor.fact_key.empty() ? divisor.arg.written : divisor.fact_key);
	charge.zero_divisors.push_back(charge.key);
	return charge;
}

std::optional<ObservationCharge> charge_divisor(const std::vector<Operand> &operands, const OpOptions &, const OpContext &) {
	if (operands.size() < 2) return std::nullopt;
	return zero_charge(operands[1]);
}

std::optional<ObservationCharge> charge_old(const std::vector<Operand> &operands, const OpOptions &, const OpContext &) {
	if (operands.empty()) return std::nullopt;
	return zero_charge(operands[0]);
}

} // namespace

void register_arithmetic_family() {
	register_family("arithmetic", {
		{"add", typer<A::Add>, evaluator<A::Add>, ObservationClass::Conditional, charge_add},
		{"subtract", typer<A::Subtract>, evaluator<A::Subtract>, ObservationClass::Conditional, charge_subtract},
		{"multiply", typer<A::Multiply>, evaluator<A::Multiply>, ObservationClass::Never, nullptr},
		{"divide", typer<A::Divide>, evaluator<A::Divide>, ObservationClass::Conditional, charge_divisor},
		{"sum", typer<A::Sum>, evaluator<A::Sum>, ObservationClass::Never, nullptr},
		{"average", typer<A::Average>, evaluator<A::Average>, ObservationClass::Never, nullptr},
		{"min", typer<A::Min>, evaluator<A::Min>, ObservationClass::Never, nullptr},
		{"max", typer<A::Max>, evaluator<A::Max>, ObservationClass::Never, nullptr},
		{"abs", typer<A::Abs>, evaluator<A::Abs>, ObservationClass::Never, nullptr},
		{"positive_part", typer<A::PositivePart>, evaluator<A::PositivePart>, ObservationClass::Never, nullptr},
		{"round", typer<A::Round>, evaluator<A::Round>, ObservationClass::Never, nullptr},
		{"equal", typer<A::Equal>, evaluator<A::Equal>, ObservationClass::Conditional, charge_relation},
		{"not_equal", typer<A::NotEqual>, evaluator<A::NotEqual>, ObservationClass::Conditional, charge_relation},
		{"greater_than", typer<A::Greater>, evaluator<A::Greater>, ObservationClass::Conditional, charge_relation},
		{"less_than", typer<A::Less>, evaluator<A::Less>, ObservationClass::Conditional, charge_relation},
		{"greater_or_equal", typer<A::GreaterOrEqual>, evaluator<A::GreaterOrEqual>, ObservationClass::Conditional, charge_relation},
		{"less_or_equal", typer<A::LessOrEqual>, evaluator<A::LessOrEqual>, ObservationClass::Conditional, charge_relation},
		{"between", typer<A::Between>, evaluator<A::Between>, ObservationClass::Conditional, charge_relation},
		{"compare", typer<A::Compare>, evaluator<A::Compare>, ObservationClass::Always, charge_always},
		{"sign", typer<A::Sign>, evaluator<A::Sign>, ObservationClass::Always, charge_always},
		{"apply_rate", typer<A::ApplyRate>, evaluator<A::ApplyRate>, ObservationClass::Never, nullptr},
		{"ratio", typer<A::Ratio>, evaluator<A::Ratio>, ObservationClass::Conditional, charge_divisor},
		{"percentage_of", typer<A::PercentageOf>, evaluator<A::PercentageOf>, ObservationClass::Never, nullptr},
		{"percentage_change", typer<A::PercentageChange>, evaluator<A::PercentageChange>, ObservationClass::Conditional, charge_old},
		{"and", typer<A::And>, evaluator<A::And>, ObservationClass::Conditional, charge_logic},
		{"or", typer<A::Or>, evaluator<A::Or>, ObservationClass::Conditional, charge_logic},
		{"not", typer<A::Not>, evaluator<A::Not>, ObservationClass::Conditional, charge_logic},
		{"if_then_else", typer<A::IfThenElse>, evaluator<A::IfThenElse>, ObservationClass::Never, nullptr},
	});
}

} // namespace singularity::protected_api::papinian::compute
