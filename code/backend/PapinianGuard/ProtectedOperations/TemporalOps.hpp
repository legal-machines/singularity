#pragma once

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The temporal family of the protected computation protocol (DESIGN4 §2.5,
// §7; lane C): before, after, same_time, duration_between, add_duration,
// deadline, within, outside, overlaps, start_of, end_of, days_in,
// next_business_day, next_occurrence, day_class. Each is registered through
// register_family (TemporalOps.cpp) as a typer, an evaluator, its observation
// class and its pre-charge; the executor reaches them by find_op() - the
// arithmetic family's date rows (D + U, D - D, D - U) too.
//
// What an evaluator gives back beyond a value (OpAnswer):
//   - `relation`: TRUE / FALSE for a relation; day_class's class of the day:
//     BUSINESS_DAY, WEEKEND, HOLIDAY, MAY_CLOSE (a day the definition's words
//     only may close), OPEN (a weekday whose closures are not known here).
//   - `words`: closed lower-case words, space-separated, each one of the
//     k*Word constants below - never a year, a date or a figure. The results
//     block renders them with the ComputeBounded* / ComputeUnknownReason /
//     ComputeNotGoverningCalendar templates (lane K's words).
//   - Bounded: `lo` and `hi` (either may be missing when that side has no
//     bound: a closure authority without a calendar leaves every weekday
//     open, so a deadline counted forward has no latest day - words carry
//     no_latest_day - and one counted back has no earliest day - words carry
//     no_earliest_day), `certainty` Bounded. Each end is a day, even when the
//     result type is DATE(range).
//   - Unknown: `code` the reason (precision_insufficient, rule_unknown ...).
//   - `record.business_day_rule` / `business_day_rule_source`: the rule used.
// Rule and calendar errors (unknown_calendar, rule_conflict,
// definition_not_found, rule_unknown when no rule can be known) depend on
// text, never on a value: status Error, class S.
namespace singularity::protected_api::papinian::compute {

// Why an answer is a bound, or what its calendar is.
inline constexpr const char *kWordHolidaysNotKnown = "holidays_not_known";   // the rule's closures for the period are not known here
inline constexpr const char *kWordClosuresPossible = "closures_possible";   // a day the definition's words only may close
inline constexpr const char *kWordRollNotKnown = "roll_not_known";          // the period ends on a closed day and no roll is known
inline constexpr const char *kWordPartialDate = "partial_date";             // a month, quarter or year, not one day
inline constexpr const char *kWordMonthLength = "month_length";             // not a whole number of months, quarters or years
inline constexpr const char *kWordNoLatestDay = "no_latest_day";            // only the lower end is bounded
inline constexpr const char *kWordNoEarliestDay = "no_earliest_day";        // only the upper end is bounded
inline constexpr const char *kWordNotGoverning = "not_governing";           // under the named calendar, not the governing rule
inline constexpr const char *kWordCalendarDays = "calendar_days";           // counted in calendar days, no roll
inline constexpr const char *kWordEndsOnSaturday = "ends_on_a_saturday";    // the period's last day is a Saturday (arithmetic, no calendar)
inline constexpr const char *kWordEndsOnSunday = "ends_on_a_sunday";        // the period's last day is a Sunday

// The calendars block of a protected request (DESIGN4 §7.3): every
// business-day rule the material states, each with its handle
// ([[Calendar1]] ...) and its line (business_calendar::calendar_line: closed
// words, no year, no date). A document that counts in business days gets the
// rule that governs it (the precedence of business_calendar::governing_rule:
// its own definition, an incorporated one, the matter rule, its governing
// law's calendar, else weekdays with the holidays not known); a definition
// that governs no document (a superseded draft's) is listed too, as governing
// nothing; so is the lawyer's matter rule. Nothing when no document counts in
// business days and no matter rule is given. Handles are numbered in the
// order the documents come.
[[nodiscard]] std::vector<CalendarHandle> calendar_handles(const std::vector<business_calendar::RulePassage> &passages,
	const std::optional<business_calendar::BusinessDayRule> &matter_rule, int this_year);

// Whether `op` over operands of these declared types, with these options, is
// calendar-anchored (DESIGN4 §7.2): its result is not invariant under a common
// shift of the date roots. The period's unit comes from its DURATION type or
// from options.unit; an operand that is itself calendar-anchored anchors the
// op. Decided by the op's own typer, so it agrees with the result type the
// executor records; an op the typer refuses, or no temporal op, is not.
[[nodiscard]] bool calendar_anchored_op(std::string_view op, const std::vector<ArgType> &arguments, const OpOptions &options);

// The unit an options.unit word names ("days", "business_days", "quarters" ...;
// singular or plural; "calendar_days" is days), or nothing.
[[nodiscard]] std::optional<semantic::DurationUnit> unit_named(const std::optional<std::string> &unit);

// A window from a date is one form everywhere - within(date, anchor, period)
// and a filter's within/outside test alike (owner, 26 Sep 2026: a compact,
// reliable API): the period carries its direction, "-24 months" back from the
// anchor, "+30 days" forward. A period written without its sign is refused
// with this, never read one way silently.
inline constexpr const char *kSignThePeriod = "a window from a date needs its direction: write the period with its sign, "
	"a minus to look back from the date, a plus to look forward";

// Whether a constant is written with its sign ("-24 months", "+30 days").
[[nodiscard]] bool written_signed(std::string_view written);

// The day-precise date a day-precise date moved by a period of days, weeks,
// months, quarters or years is; nothing for business days, a partial date or
// a period that is not whole.
[[nodiscard]] std::optional<semantic::Value> date_moved_by(const semantic::Value &date, const semantic::Value &period);

} // namespace singularity::protected_api::papinian::compute
