#include "Context/BusinessCalendar/BusinessCalendar.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace singularity::context::business_calendar {
namespace {

// Constructors the registry's data reads with.
constexpr DayRule fixed(const char *name, unsigned month, unsigned day, Observance observance = Observance::None,
		int from = 0, int to = 0) {
	return DayRule{name, DayRuleKind::Fixed, month, day, 0, 0, 0, observance, from, to};
}
constexpr DayRule nth(const char *name, unsigned month, int weekday, int n, int from = 0, int to = 0) {
	return DayRule{name, DayRuleKind::NthWeekday, month, 1, weekday, n, 0, Observance::None, from, to};
}
constexpr DayRule on_or_after(const char *name, unsigned month, unsigned day, int weekday) {
	return DayRule{name, DayRuleKind::WeekdayOnOrAfter, month, day, weekday, 0, 0, Observance::None, 0, 0};
}
constexpr DayRule easter(const char *name, int offset) {
	return DayRule{name, DayRuleKind::EasterOffset, 1, 1, 0, 0, offset, Observance::None, 0, 0};
}

} // namespace

const std::vector<CalendarEntry> &registry() {
	static const std::vector<CalendarEntry> entries{
#include "Context/BusinessCalendar/CalendarRegistry.inc"
	};
	return entries;
}

const CalendarEntry *find_calendar(std::string_view id) {
	for (const CalendarEntry &entry : registry())
		if (id == entry.id) return &entry;
	return nullptr;
}

long long days_from_civil(long long year, unsigned month, unsigned day) {
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
	const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return era * 146097 + static_cast<long long>(dayOfEra) - 719468;
}

CivilDate civil_from_days(long long day) {
	day += 719468;
	const long long era = (day >= 0 ? day : day - 146096) / 146097;
	const unsigned dayOfEra = static_cast<unsigned>(day - era * 146097);
	const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
	const long long year = static_cast<long long>(yearOfEra) + era * 400;
	const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
	const unsigned shifted = (5 * dayOfYear + 2) / 153;
	const unsigned dayOfMonth = dayOfYear - (153 * shifted + 2) / 5 + 1;
	const unsigned month = shifted < 10 ? shifted + 3 : shifted - 9;
	return CivilDate{year + (month <= 2 ? 1 : 0), month, dayOfMonth};
}

int weekday(long long day) {
	// 1970-01-01 was a Thursday (3).
	return static_cast<int>(((day % 7) + 7 + 3) % 7);
}

long long easter_sunday(long long year) {
	// The anonymous Gregorian algorithm (Meeus/Jones/Butcher).
	const long long a = year % 19;
	const long long b = year / 100;
	const long long c = year % 100;
	const long long d = b / 4;
	const long long e = b % 4;
	const long long f = (b + 8) / 25;
	const long long g = (b - f + 1) / 3;
	const long long h = (19 * a + b - d - g + 15) % 30;
	const long long i = c / 4;
	const long long k = c % 4;
	const long long l = (32 + 2 * e + 2 * i - h - k) % 7;
	const long long m = (a + 11 * h + 22 * l) / 451;
	const long long month = (h + l - 7 * m + 114) / 31;
	const long long day = ((h + l - 7 * m + 114) % 31) + 1;
	return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
}

namespace {

// ---------------------------------------------------------------------------
// Reading the registry's data.

// The day of `month` in `year` that is the `nth` `wanted` weekday (Monday 0),
// or the last one when `nth` is 0.
long long nth_weekday(long long year, unsigned month, int wanted, int nth) {
	if (nth > 0) {
		const long long first = days_from_civil(year, month, 1);
		const int shift = (wanted - weekday(first) + 7) % 7;
		return first + shift + 7LL * (nth - 1);
	}
	const unsigned nextMonth = month == 12 ? 1 : month + 1;
	const long long nextYear = month == 12 ? year + 1 : year;
	const long long last = days_from_civil(nextYear, nextMonth, 1) - 1;
	const int back = (weekday(last) - wanted + 7) % 7;
	return last - back;
}

bool applies(const DayRule &rule, long long year) {
	return (rule.from_year == 0 || year >= rule.from_year) && (rule.to_year == 0 || year <= rule.to_year);
}

long long rule_day(const DayRule &rule, long long year) {
	switch (rule.kind) {
		case DayRuleKind::Fixed: return days_from_civil(year, rule.month, rule.day);
		case DayRuleKind::NthWeekday: return nth_weekday(year, rule.month, rule.weekday, rule.nth);
		case DayRuleKind::WeekdayOnOrAfter: {
			const long long from = days_from_civil(year, rule.month, rule.day);
			return from + (rule.weekday - weekday(from) + 7) % 7;
		}
		case DayRuleKind::EasterOffset: return easter_sunday(year) + rule.easter_offset;
	}
	return days_from_civil(year, rule.month, rule.day);
}

// The days `rules` close in their holiday-year `year`, as observed.
std::set<long long> rule_days(const std::vector<DayRule> &rules, long long year) {
	std::set<long long> days;
	std::vector<long long> substitutes;
	for (const DayRule &rule : rules) {
		if (!applies(rule, year)) continue;
		const long long day = rule_day(rule, year);
		const int of = weekday(day);
		days.insert(day);
		switch (rule.observance) {
			case Observance::None: break;
			case Observance::SaturdayToFridaySundayToMonday:
				if (of == 5) days.insert(day - 1);
				if (of == 6) days.insert(day + 1);
				break;
			case Observance::SundayToMonday:
				if (of == 6) days.insert(day + 1);
				break;
			case Observance::SubstituteNextWeekday:
				if (of >= 5) substitutes.push_back(day);
				break;
		}
	}
	std::sort(substitutes.begin(), substitutes.end());
	for (const long long day : substitutes) {
		long long next = day + 1;
		while (weekday(next) >= 5 || days.count(next) != 0) ++next;
		days.insert(next);
	}
	return days;
}

struct YearDays {
	std::set<long long> closed;
	std::set<long long> working;
	std::set<long long> possible;
};

bool in_year(long long day, long long year) { return civil_from_days(day).year == year; }

const YearDays &year_days(const CalendarEntry &entry, long long year, int depth = 0) {
	thread_local std::map<std::pair<const CalendarEntry *, long long>, YearDays> cache;
	const auto key = std::make_pair(&entry, year);
	const auto found = cache.find(key);
	if (found != cache.end()) return found->second;
	YearDays days;
	for (const long long holidayYear : {year - 1, year, year + 1}) {
		for (const long long day : rule_days(entry.closed, holidayYear))
			if (in_year(day, year)) days.closed.insert(day);
		for (const long long day : rule_days(entry.possible, holidayYear))
			if (in_year(day, year)) days.possible.insert(day);
	}
	const auto dated = [&](const std::vector<DatedDay> &list, std::set<long long> &into, bool add) {
		for (const DatedDay &date : list) {
			if (date.year != year) continue;
			const long long day = days_from_civil(date.year, date.month, date.day);
			if (add) into.insert(day);
			else into.erase(day);
		}
	};
	dated(entry.closed_dates, days.closed, true);
	dated(entry.not_closed_dates, days.closed, false);
	dated(entry.working_dates, days.working, true);
	dated(entry.possible_dates, days.possible, true);
	if (depth < 4)
		for (const std::string &id : entry.includes)
			if (const CalendarEntry *included = find_calendar(id)) {
				const YearDays &more = year_days(*included, year, depth + 1);
				days.closed.insert(more.closed.begin(), more.closed.end());
				days.working.insert(more.working.begin(), more.working.end());
			}
	for (const long long day : days.closed) days.possible.erase(day);
	return cache.emplace(key, std::move(days)).first->second;
}

bool entry_covers(const CalendarEntry &entry, long long year) {
	return year >= entry.first_year && year <= entry.last_year;
}

bool named_on(const DayRule &rule, long long day) {
	const long long year = civil_from_days(day).year;
	return applies(rule, year) && rule_day(rule, year) == day;
}

} // namespace

bool covers(std::string_view calendar, int year) {
	const CalendarEntry *entry = find_calendar(calendar);
	return entry != nullptr && entry_covers(*entry, year);
}

bool closed_on(std::string_view calendar, long long day) {
	const CalendarEntry *entry = find_calendar(calendar);
	const long long year = civil_from_days(day).year;
	return entry != nullptr && entry_covers(*entry, year) && year_days(*entry, year).closed.count(day) != 0;
}

bool working_on(std::string_view calendar, long long day) {
	const CalendarEntry *entry = find_calendar(calendar);
	const long long year = civil_from_days(day).year;
	return entry != nullptr && entry_covers(*entry, year) && year_days(*entry, year).working.count(day) != 0;
}

bool possibly_closed_on(std::string_view calendar, long long day) {
	const CalendarEntry *entry = find_calendar(calendar);
	const long long year = civil_from_days(day).year;
	return entry != nullptr && entry_covers(*entry, year) && year_days(*entry, year).possible.count(day) != 0;
}

// ---------------------------------------------------------------------------
// Days under a rule.

bool same_days(const BusinessDayRule &a, const BusinessDayRule &b) {
	const auto names = [](const std::vector<DayRule> &days) {
		std::set<std::string> result;
		for (const DayRule &day : days)
			result.insert(std::to_string(static_cast<int>(day.kind)) + "|" + std::to_string(day.month) + "|" + std::to_string(day.day) + "|"
				+ std::to_string(day.weekday) + "|" + std::to_string(day.nth) + "|" + std::to_string(day.easter_offset) + "|"
				+ std::to_string(day.from_year));
		return result;
	};
	const std::set<std::string> ac(a.calendars.begin(), a.calendars.end());
	const std::set<std::string> bc(b.calendars.begin(), b.calendars.end());
	const std::set<std::string> ap(a.possible_calendars.begin(), a.possible_calendars.end());
	const std::set<std::string> bp(b.possible_calendars.begin(), b.possible_calendars.end());
	return a.weekend == b.weekend && a.weekend_explicit == b.weekend_explicit && ac == bc && ap == bp
		&& names(a.named_days) == names(b.named_days) && a.holidays_known == b.holidays_known && a.roll == b.roll
		&& a.roll_scope == b.roll_scope;
}

DayStatus status(const BusinessDayRule &rule, long long day) {
	const long long year = civil_from_days(day).year;
	bool uncovered = false;
	bool closed = false;
	// A weekend day is a working day only when EVERY calendar of the rule makes
	// it one: a Russian transferred Saturday is no business day under a rule
	// that also needs New York's banks, which are closed on it.
	std::size_t working = 0;
	for (const std::string &id : rule.calendars) {
		const CalendarEntry *entry = find_calendar(id);
		if (entry == nullptr || !entry_covers(*entry, year)) {
			uncovered = true;
			continue;
		}
		const YearDays &days = year_days(*entry, year);
		if (days.closed.count(day) != 0) closed = true;
		if (days.working.count(day) != 0) ++working;
	}
	for (const DayRule &named : rule.named_days)
		if (named_on(named, day)) closed = true;
	if (closed) return DayStatus::Holiday;
	if (rule.weekend.count(weekday(day)) != 0) {
		if (working == 0 || working != rule.calendars.size()) return DayStatus::Weekend;
		// The calendars make it a working day, but the rule's own words exclude
		// that weekday by name ("other than a Saturday"): which reading governs is
		// not certain.
		if (rule.weekend_explicit) return DayStatus::Possible;
	}
	if (uncovered || !rule.holidays_known) return DayStatus::Open;
	for (const std::string &id : rule.possible_calendars) {
		const CalendarEntry *entry = find_calendar(id);
		if (entry == nullptr || !entry_covers(*entry, year)) return DayStatus::Open;
		if (year_days(*entry, year).possible.count(day) != 0) return DayStatus::Possible;
	}
	return DayStatus::Business;
}

DayClass classify(const BusinessDayRule &rule, long long day) {
	switch (status(rule, day)) {
		case DayStatus::Weekend: return DayClass::Weekend;
		case DayStatus::Holiday: return DayClass::Holiday;
		default: return DayClass::BusinessDay;
	}
}

namespace {

bool certain(DayStatus status) { return status == DayStatus::Business || status == DayStatus::Weekend || status == DayStatus::Holiday; }
bool may_be_business(DayStatus status) { return status == DayStatus::Business || status == DayStatus::Possible || status == DayStatus::Open; }

// The closed words a result over [low, high] rests on.
std::vector<std::string> assumptions_over(const BusinessDayRule &rule, long long low, long long high) {
	std::vector<std::string> said;
	const auto say = [&](const std::string &words) {
		if (std::find(said.begin(), said.end(), words) == said.end()) said.push_back(words);
	};
	if (!rule.holidays_known && rule.source != RuleSource::WeekdayFallback)
		say("the definition names closures Singularity keeps no calendar of");
	bool unscheduled = false;
	for (const std::string &id : rule.calendars) {
		const CalendarEntry *entry = find_calendar(id);
		if (entry == nullptr) continue;
		if (entry->unscheduled_closures) unscheduled = true;
		for (long long year = civil_from_days(low).year; year <= civil_from_days(high).year; ++year)
			if (!entry_covers(*entry, year)) say(std::string(entry->name) + " not known for a year the period reaches");
	}
	if (unscheduled) say("scheduled closures only; unscheduled closures not known");
	for (long long day = low; day <= high && high - low < 4000; ++day)
		if (status(rule, day) == DayStatus::Possible) {
			say("a day in the period may or may not be closed under the definition's words");
			break;
		}
	return said;
}

} // namespace

bool exact(const BusinessDayRule &rule, long long from, long long to) {
	const long long low = std::min(from, to);
	const long long high = std::max(from, to);
	if (high - low > 40000) return false;
	for (long long day = low; day <= high; ++day)
		if (!certain(status(rule, day))) return false;
	return true;
}

CountBound count_bound(const BusinessDayRule &rule, long long from, long long to) {
	CountBound bound;
	const bool forward = to >= from;
	const long long low = forward ? from : to;
	const long long high = forward ? to : from;
	long long most = 0;
	long long least = 0;
	bool sure = true;
	for (long long day = low + 1; day <= high; ++day) {
		const DayStatus of = status(rule, day);
		if (of == DayStatus::Business) { ++most; ++least; }
		else if (!certain(of)) { ++most; sure = false; }
	}
	bound.at_most = forward ? most : -most;
	bound.at_least = forward ? least : -least;
	if (sure) bound.exact = bound.at_most;
	bound.assumptions = assumptions_over(rule, low + 1, std::max(low + 1, high));
	return bound;
}

std::optional<long long> count(const BusinessDayRule &rule, long long from, long long to) {
	return count_bound(rule, from, to).exact;
}

DayBound deadline_bound(const BusinessDayRule &rule, long long from, long long n) {
	DayBound bound;
	if (n == 0) {
		bound.exact = from;
		return bound;
	}
	const long long step = n > 0 ? 1 : -1;
	const long long wanted = n > 0 ? n : -n;
	const long long guard = 7 * wanted + 400;
	// Nearest: every uncertain day counted as a business day. Farthest: only the
	// certain ones - where the certain days reach it at all.
	std::optional<long long> nearest;
	std::optional<long long> farthest;
	long long loose = 0;
	long long strict = 0;
	for (long long walked = 1, day = from + step; walked <= guard && (!nearest || !farthest); ++walked, day += step) {
		const DayStatus of = status(rule, day);
		if (!nearest && may_be_business(of) && ++loose == wanted) nearest = day;
		if (!farthest && of == DayStatus::Business && ++strict == wanted) farthest = day;
	}
	if (!nearest) return bound;
	if (farthest && *farthest == *nearest) {
		bound.exact = *nearest;
	} else if (n > 0) {
		bound.not_before = *nearest;
		if (farthest) bound.not_after = *farthest;
	} else {
		bound.not_after = *nearest;
		if (farthest) bound.not_before = *farthest;
	}
	const long long reach = farthest ? *farthest : *nearest;
	bound.assumptions = assumptions_over(rule, std::min(from, reach), std::max(from, reach));
	return bound;
}

std::optional<long long> deadline(const BusinessDayRule &rule, long long from, long long n) {
	return deadline_bound(rule, from, n).exact;
}

namespace {

DayBound first_business(const BusinessDayRule &rule, long long day, long long step) {
	DayBound bound;
	std::optional<long long> nearest;
	std::optional<long long> farthest;
	for (long long walked = 0, at = day; walked < 400 && (!nearest || !farthest); ++walked, at += step) {
		const DayStatus of = status(rule, at);
		if (!nearest && may_be_business(of)) nearest = at;
		if (!farthest && of == DayStatus::Business) farthest = at;
	}
	if (!nearest) return bound;
	if (farthest && *farthest == *nearest) bound.exact = *nearest;
	else if (step > 0) {
		bound.not_before = *nearest;
		if (farthest) bound.not_after = *farthest;
	} else {
		bound.not_after = *nearest;
		if (farthest) bound.not_before = *farthest;
	}
	const long long reach = farthest ? *farthest : *nearest;
	bound.assumptions = assumptions_over(rule, std::min(day, reach), std::max(day, reach));
	return bound;
}

} // namespace

DayBound next_business_day_bound(const BusinessDayRule &rule, long long day) {
	return first_business(rule, day, 1);
}

std::optional<long long> next_business_day(const BusinessDayRule &rule, long long day) {
	return next_business_day_bound(rule, day).exact;
}

DayBound calendar_period_end(const BusinessDayRule *given, long long day, bool counted_back, bool payment) {
	BusinessDayRule rule = given != nullptr ? *given : BusinessDayRule{};
	// A clause that moves only a payment's due day settles no other period.
	if (rule.roll_scope == "payments" && !payment) rule.roll = rule.roll_source = "";
	DayBound bound;
	const DayStatus of = status(rule, day);
	const auto add = [&](const std::string &words) { bound.assumptions.push_back(words); };
	if (of == DayStatus::Business || rule.roll == "none") {
		bound.exact = day;
		return bound;
	}
	// Not certainly a business day: a weekend or holiday, a day the words only
	// may close, or a weekday whose holidays are not known (Open). An Open day
	// is never passed off as the period's exact end - if it is a holiday, a roll
	// the contract or the law sets may move it.
	if (counted_back && rule.roll != "preceding_business_day") {
		// Counted back, not a business day (or maybe not): on the day itself, or
		// the business day before it if a roll applies.
		const DayBound previous = first_business(rule, day - 1, -1);
		bound.not_after = day;
		if (previous.exact) bound.not_before = previous.exact;
		else if (previous.not_before) bound.not_before = previous.not_before;
		if (of == DayStatus::Open) add("whether that day is a holiday under the rule that governs is not known");
		add("whether a period ending on a day that is not a business day moves to the one before is not settled");
		for (const std::string &words : previous.assumptions)
			if (std::find(bound.assumptions.begin(), bound.assumptions.end(), words) == bound.assumptions.end()) add(words);
		return bound;
	}
	const bool uncertainDay = of == DayStatus::Possible || of == DayStatus::Open;
	if (rule.roll.empty()) {
		// Not a business day (or maybe not), and whether the period's end moves is
		// not settled: on the day itself, or the next business day if a roll applies.
		const DayBound next = next_business_day_bound(rule, day + 1);
		bound.not_before = day;
		if (next.exact) bound.not_after = next.exact;
		else if (next.not_after) bound.not_after = next.not_after;
		if (of == DayStatus::Open) add("whether that day is a holiday under the rule that governs is not known");
		add("whether a period ending on a day that is not a business day moves to the next one is not settled");
		for (const std::string &words : next.assumptions)
			if (std::find(bound.assumptions.begin(), bound.assumptions.end(), words) == bound.assumptions.end()) add(words);
		return bound;
	}
	const bool preceding = rule.roll == "preceding_business_day";
	DayBound moved = first_business(rule, day, preceding ? -1 : 1);
	if (rule.roll == "modified_following") {
		const long long reach = moved.exact ? *moved.exact : moved.not_after ? *moved.not_after : day;
		if (civil_from_days(reach).month != civil_from_days(day).month) moved = first_business(rule, day, -1);
	}
	bound.assumptions = moved.assumptions;
	if (uncertainDay) {
		// The day may stand, or move.
		if (preceding) {
			bound.not_after = day;
			if (moved.exact) bound.not_before = moved.exact;
			else if (moved.not_before) bound.not_before = moved.not_before;
		} else {
			bound.not_before = day;
			if (moved.exact) bound.not_after = moved.exact;
			else if (moved.not_after) bound.not_after = moved.not_after;
		}
		return bound;
	}
	bound.exact = moved.exact;
	bound.not_before = moved.not_before;
	bound.not_after = moved.not_after;
	return bound;
}

std::optional<long long> rolled(const BusinessDayRule &rule, long long day) {
	return calendar_period_end(&rule, day).exact;
}

// ---------------------------------------------------------------------------
// Words.

std::string rule_name(const BusinessDayRule &rule) {
	switch (rule.source) {
		case RuleSource::DocumentDefinition: return "document_definition";
		case RuleSource::IncorporatedDefinition: return "incorporated_definition";
		case RuleSource::MatterRule: return "matter_rule";
		case RuleSource::JurisdictionCalendar:
			return "jurisdiction:" + (rule.calendars.empty() ? std::string("weekdays") : rule.calendars.front());
		case RuleSource::WeekdayFallback: return "weekday_fallback";
	}
	return "weekday_fallback";
}

std::string describe(const BusinessDayRule &rule) {
	std::string words;
	switch (rule.source) {
		case RuleSource::DocumentDefinition: words = "as the agreement defines Business Day"; break;
		case RuleSource::IncorporatedDefinition: words = "as the definition the agreement incorporates sets it"; break;
		case RuleSource::MatterRule: words = "under the business-day rule the lawyer gave"; break;
		case RuleSource::JurisdictionCalendar: {
			const CalendarEntry *entry = rule.calendars.empty() ? nullptr : find_calendar(rule.calendars.front());
			words = entry != nullptr ? std::string(entry->name) + ", governing law" : std::string("weekdays, governing law");
			break;
		}
		case RuleSource::WeekdayFallback: return "weekdays counted; holidays not known";
	}
	if (rule.source != RuleSource::JurisdictionCalendar && !rule.calendars.empty()) {
		std::string names;
		for (const std::string &id : rule.calendars)
			if (const CalendarEntry *entry = find_calendar(id)) names += (names.empty() ? "" : " and ") + std::string(entry->name);
		if (!names.empty()) words += ": " + names;
	}
	if (!rule.holidays_known) words += "; its holidays not known";
	return words;
}

std::string explain(const BusinessDayRule &rule, const std::vector<std::string> &assumptions) {
	std::string words = describe(rule);
	for (const std::string &assumption : assumptions) words += "; " + assumption;
	return words;
}

namespace {

// "a", "a and b", "a, b and c".
std::string listed(const std::vector<std::string> &items) {
	std::string words;
	for (std::size_t index = 0; index < items.size(); ++index) {
		if (index > 0) words += index + 1 == items.size() ? " and " : ", ";
		words += items[index];
	}
	return words;
}

} // namespace

std::string calendar_line(const BusinessDayRule &rule, const std::string &defined_in, const std::vector<std::string> &governs,
		int this_year) {
	std::string words;
	const std::string definer = defined_in.empty() ? std::string("the agreement") : defined_in;
	switch (rule.source) {
		case RuleSource::DocumentDefinition: words = "business days as " + definer + " defines them"; break;
		case RuleSource::IncorporatedDefinition: words = "business days as the definition " + definer + " incorporates sets them"; break;
		case RuleSource::MatterRule: words = "business days under the rule the lawyer gave"; break;
		case RuleSource::JurisdictionCalendar: {
			const CalendarEntry *entry = rule.calendars.empty() ? nullptr : find_calendar(rule.calendars.front());
			words = (entry != nullptr ? std::string(entry->name) : std::string("weekdays")) + " (governing law, no definition)";
			break;
		}
		case RuleSource::WeekdayFallback: words = "weekdays only; holidays not known (no rule found)"; break;
	}
	if (rule.source != RuleSource::WeekdayFallback) {
		static const char *const days[] = {"Mondays", "Tuesdays", "Wednesdays", "Thursdays", "Fridays", "Saturdays", "Sundays"};
		std::vector<std::string> closed;
		for (const int day : rule.weekend)
			if (day >= 0 && day < 7) closed.emplace_back(days[day]);
		if (rule.source == RuleSource::JurisdictionCalendar) {
			if (!rule.calendars.empty()) closed.emplace_back("its holidays");
		} else {
			for (const std::string &id : rule.calendars)
				if (const CalendarEntry *entry = find_calendar(id)) closed.emplace_back(entry->name);
		}
		if (!rule.named_days.empty()) closed.emplace_back("the days the definition names");
		if (!closed.empty()) words += ": " + listed(closed) + " closed";
		std::vector<std::string> possible;
		for (const std::string &id : rule.possible_calendars)
			if (const CalendarEntry *entry = find_calendar(id)) possible.emplace_back(entry->name);
		if (!possible.empty()) words += "; some other days may close (" + listed(possible) + ")";
		bool covered = rule.holidays_known;
		for (const std::string &id : rule.calendars) covered = covered && covers(id, this_year) && covers(id, this_year + 1);
		words += !rule.holidays_known ? "; holidays not known" : covered ? "; holidays known" : "; holidays known for some years only";
	}
	const std::string law = rule.roll_source == "law:ru_civil_code_193" ? " (Russian Civil Code rule)" : std::string();
	const std::string subject = rule.roll_scope == "payments" ? "a payment due on a closed day is due" : "a period ending on a closed day ends";
	if (rule.roll == "next_business_day") words += "; " + subject + " on the next business day" + law;
	else if (rule.roll == "preceding_business_day") words += "; " + subject + " on the business day before" + law;
	else if (rule.roll == "modified_following")
		words += "; " + subject + " on the next business day, or the one before when that is in the next month" + law;
	else if (rule.roll == "none") words += "; a period ending on a closed day does not move";
	words += governs.empty() ? std::string("; governs no document here") : "; governs " + listed(governs);
	return words;
}

namespace {

// ASCII and Cyrillic letters in lower case, … as …; every other byte as it
// is. Byte for byte the same length, so an offset in the folded text is the
// same offset in the text.
std::string fold(std::string_view text) {
	std::string result(text);
	for (std::size_t index = 0; index < result.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(result[index]);
		if (byte < 0x80) {
			result[index] = static_cast<char>(std::tolower(byte));
			continue;
		}
		if (index + 1 >= result.size()) continue;
		const unsigned char next = static_cast<unsigned char>(result[index + 1]);
		if (byte == 0xD0 && next >= 0x90 && next <= 0x9F) {           // …
			result[index + 1] = static_cast<char>(next + 0x20);
			++index;
		} else if (byte == 0xD0 && next >= 0xA0 && next <= 0xAF) {    // …
			result[index] = static_cast<char>(0xD1);
			result[index + 1] = static_cast<char>(next - 0x20);
			++index;
		} else if ((byte == 0xD0 && next == 0x81) || (byte == 0xD1 && next == 0x91)) {  // … -> …
			result[index] = static_cast<char>(0xD0);
			result[index + 1] = static_cast<char>(0xB5);
			++index;
		}
	}
	return result;
}

bool has(const std::string &folded, std::initializer_list<const char *> words) {
	return std::any_of(words.begin(), words.end(), [&](const char *word) { return folded.find(word) != std::string::npos; });
}

// Whether a letter (ASCII alphanumeric, or a Cyrillic letter's bytes) stands at
// `at` / right before `at`.
bool letter_at(const std::string &text, std::size_t at) {
	if (at >= text.size()) return false;
	const unsigned char byte = static_cast<unsigned char>(text[at]);
	if (byte < 0x80) return std::isalnum(byte) != 0;
	return byte == 0xD0 || byte == 0xD1;
}
bool letter_before(const std::string &text, std::size_t at) {
	if (at == 0) return false;
	const unsigned char byte = static_cast<unsigned char>(text[at - 1]);
	if (byte < 0x80) return std::isalnum(byte) != 0;
	// A continuation byte: a Cyrillic letter when its lead is D0/D1.
	return at >= 2 && (static_cast<unsigned char>(text[at - 2]) == 0xD0 || static_cast<unsigned char>(text[at - 2]) == 0xD1);
}

// A byte of a word: an ASCII letter or digit, or a Cyrillic letter's bytes.
bool word_byte(const std::string &text, std::size_t at) {
	if (letter_at(text, at)) return true;
	const unsigned char byte = static_cast<unsigned char>(text[at]);
	return byte >= 0x80 && byte <= 0xBF && at > 0
		&& (static_cast<unsigned char>(text[at - 1]) == 0xD0 || static_cast<unsigned char>(text[at - 1]) == 0xD1);
}

// `word` in `text` from `from`, starting a word; ending one unless `prefix`.
std::size_t find_word(const std::string &text, const std::string &word, std::size_t from, bool prefix) {
	for (std::size_t at = text.find(word, from); at != std::string::npos; at = text.find(word, at + 1)) {
		if (letter_before(text, at)) continue;
		if (!prefix && letter_at(text, at + word.size())) continue;
		return at;
	}
	return std::string::npos;
}

bool has_word(const std::string &text, std::initializer_list<const char *> words, bool prefix = false) {
	return std::any_of(words.begin(), words.end(), [&](const char *word) { return find_word(text, word, 0, prefix) != std::string::npos; });
}

void blank(std::string &text, std::size_t at, std::size_t length) {
	for (std::size_t index = at; index < at + length && index < text.size(); ++index) text[index] = ' ';
}

// Where the sentence that begins at `from` ends: a full stop before a space or
// a line's end (not one inside "U.S.C." or "N.Y."), a line's end, or 480 bytes on.
std::size_t sentence_end(const std::string &text, std::size_t from) {
	const std::size_t limit = std::min(text.size(), from + 480);
	for (std::size_t at = from; at < limit; ++at) {
		if (text[at] == '\n') return at;
		if (text[at] != '.') continue;
		if (at + 1 < text.size() && text[at + 1] != ' ' && text[at + 1] != '\n' && text[at + 1] != '\r') continue;
		std::size_t word = at;
		while (word > from && text[word - 1] != ' ' && text[word - 1] != '\n') --word;
		const std::string token = text.substr(word, at - word);
		// "U.S.C.", "N.Y.", one letter ("…" of «…»): an abbreviation.
		const bool cyrillicLetter = token.size() == 2 && (static_cast<unsigned char>(token[0]) == 0xD0 || static_cast<unsigned char>(token[0]) == 0xD1);
		if (token.find('.') != std::string::npos || token.size() <= 1 || cyrillicLetter) continue;
		return at + 1;
	}
	return limit;
}

// Where the sentence holding `at` begins: after a full stop that ends one (not
// the one of an abbreviation), or a line's start, at most 480 bytes back.
std::size_t sentence_start(const std::string &text, std::size_t at) {
	std::size_t start = at;
	while (start > 0 && at - start < 480) {
		if (text[start - 1] == '\n') break;
		if (text[start - 1] == ' ' && start >= 2 && text[start - 2] == '.') {
			std::size_t word = start - 2;
			while (word > 0 && text[word - 1] != ' ' && text[word - 1] != '\n') --word;
			const std::string token = text.substr(word, start - 2 - word);
			const bool cyrillicLetter = token.size() == 2 && (static_cast<unsigned char>(token[0]) == 0xD0 || static_cast<unsigned char>(token[0]) == 0xD1);
			if (token.find('.') == std::string::npos && token.size() > 1 && !cyrillicLetter) break;
		}
		--start;
	}
	return start;
}

bool quote_byte_at(const std::string &text, std::size_t at, std::size_t &length) {
	if (at >= text.size()) return false;
	if (text[at] == '"' || text[at] == '\'') { length = 1; return true; }
	// “ ” ‘ ’ (E2 80 9C/9D/98/99), « » (C2 AB/BB)
	if (at + 2 < text.size() && static_cast<unsigned char>(text[at]) == 0xE2 && static_cast<unsigned char>(text[at + 1]) == 0x80) {
		const unsigned char third = static_cast<unsigned char>(text[at + 2]);
		if (third == 0x9C || third == 0x9D || third == 0x98 || third == 0x99) { length = 3; return true; }
	}
	if (at + 1 < text.size() && static_cast<unsigned char>(text[at]) == 0xC2) {
		const unsigned char second = static_cast<unsigned char>(text[at + 1]);
		if (second == 0xAB || second == 0xBB) { length = 2; return true; }
	}
	return false;
}

// A quote mark that ends right before `at` (its start), or npos.
std::size_t quote_before(const std::string &text, std::size_t at) {
	std::size_t length = 0;
	for (std::size_t back = 1; back <= 3 && back <= at; ++back)
		if (quote_byte_at(text, at - back, length) && at - back + length == at) return at - back;
	return std::string::npos;
}

// A dash: "-", "–" (E2 80 93), "—" (E2 80 94).
std::size_t dash_at(const std::string &text, std::size_t at) {
	if (at < text.size() && text[at] == '-') return 1;
	if (at + 2 < text.size() && static_cast<unsigned char>(text[at]) == 0xE2 && static_cast<unsigned char>(text[at + 1]) == 0x80
			&& (static_cast<unsigned char>(text[at + 2]) == 0x93 || static_cast<unsigned char>(text[at + 2]) == 0x94))
		return 3;
	return 0;
}

// The document a definition takes its meaning from: "the credit agreement" of
// "has the meaning given to it in the Credit Agreement".
std::string incorporated_name(const std::string &definition) {
	for (const char *cue : {"meaning given", "meaning set forth", "meaning assigned", "meaning ascribed", "has the meaning",
			"shall have the meaning", "as defined in", "\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xB5\xD1\x82 \xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",  // …
			"\xD0\xB2 \xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB8"}) {  // …
		const std::size_t found = definition.find(cue);
		if (found == std::string::npos) continue;
		std::size_t in = definition.rfind(" in ");
		std::size_t skip = 4;
		const std::size_t russianIn = definition.rfind(" \xD0\xB2 ");  // "…"
		if ((in == std::string::npos || in < found) && russianIn != std::string::npos && russianIn > found) {
			in = russianIn;
			skip = 4;
		}
		if (in == std::string::npos || in < found) return "(unnamed)";
		std::string name = definition.substr(in + skip);
		while (!name.empty() && (name.back() == '.' || name.back() == ' ' || name.back() == ';' || name.back() == ',')) name.pop_back();
		if (name.rfind("the ", 0) == 0) name.erase(0, 4);
		if (name.rfind("that certain ", 0) == 0) name.erase(0, 13);
		return name.empty() ? "(unnamed)" : name;
	}
	return {};
}

// ---------------------------------------------------------------------------
// Reading a definition's words into a structured rule.

struct PlaceAlias {
	const char *alias;
	const char *place;
	bool prefix;
};

// Places a definition may name, folded, longest first where one holds another.
const std::vector<PlaceAlias> &place_aliases() {
	static const std::vector<PlaceAlias> aliases{
		{"new york stock exchange", "nyse", false}, {"nyse", "nyse", false},
		{"new york city", "new york", false}, {"city of new york", "new york", false}, {"state of new york", "new york", false},
		{"new york state", "new york", false}, {"new york", "new york", false}, {"nyc", "new york", false}, {"ny", "new york", false},
		{"manhattan", "new york", false},
		{"england and wales", "england", false}, {"england", "england", false}, {"london", "london", false},
		{"united states of america", "united states", false}, {"united states", "united states", false},
		{"u.s.", "united states", false}, {"usa", "united states", false},
		{"russian federation", "russia", false}, {"russia", "russia", false}, {"moscow", "moscow", false},
		{"target2", "target2", false},
		{"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9 \xD1\x84\xD0\xB5\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD0\xB8", "russia", false},  // …
		{"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xB0\xD1\x8F \xD1\x84\xD0\xB5\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD1\x8F", "russia", false},  // …
		{"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8", "russia", false},  // …
		{"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F", "russia", false},  // …
		{"\xD1\x80\xD1\x84", "russia", false},  // …
		{"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA", "russia", true},  // …(…)
		// Saint Petersburg keeps the federal production calendar (no regional holidays).
		{"\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBA\xD1\x82-\xD0\xBF\xD0\xB5\xD1\x82\xD0\xB5\xD1\x80\xD0\xB1\xD1\x83\xD1\x80\xD0\xB3", "russia", true},  // …
		{"english", "england", false},
		{"\xD1\x82\xD1\x80\xD1\x83\xD0\xB4\xD0\xBE\xD0\xB2", "russia", true},  // …(…) - the Russian Labour Code
		{"\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA", "moscow", true},  // …
		{"\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2", "moscow", true},  // …
		{"\xD0\xBB\xD0\xBE\xD0\xBD\xD0\xB4\xD0\xBE\xD0\xBD", "london", true},  // …
		{"\xD0\xBD\xD1\x8C\xD1\x8E-\xD0\xB9\xD0\xBE\xD1\x80\xD0\xBA", "new york", true},  // …
	};
	return aliases;
}

struct PlaceScan {
	std::vector<std::string> places;
	bool other = false;
};

// The first word of a list item that opens a new clause instead of naming one
// more place: ", provided that", ", which", ", in each case", «…».
const std::vector<std::string> &clauseOpeners() {
	static const std::vector<std::string> words{"provided", "except", "excluding", "including", "other", "unless", "subject",
		"as", "if", "when", "where", "which", "that", "each", "such", "being", "respectively", "in", "on", "for", "to", "with",
		"without", "save", "but", "any", "a", "an", "the", "whether", "so", "then", "by", "at", "from", "after", "before",
		"\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB9", "\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB5",  // …
		"\xD0\xBA\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB5", "\xD0\xB5\xD1\x81\xD0\xBB\xD0\xB8", "\xD0\xBF\xD1\x80\xD0\xB8",  // …
		"\xD0\xB2", "\xD0\xBD\xD0\xB0", "\xD0\xB0", "\xD0\xBD\xD0\xBE", "\xD1\x81", "\xD0\xBF\xD0\xBE", "\xD0\xB7\xD0\xB0"};  // …
	return words;
}

// The places a window of a definition names; `other` when a place follows "in"
// / «…» that Singularity does not know ("banks in Boston, Massachusetts").
PlaceScan scan_places(std::string window) {
	PlaceScan scan;
	for (const PlaceAlias &alias : place_aliases()) {
		const std::string word(alias.alias);
		for (std::size_t at = find_word(window, word, 0, alias.prefix); at != std::string::npos;
				at = find_word(window, word, at + 1, alias.prefix)) {
			if (std::find(scan.places.begin(), scan.places.end(), alias.place) == scan.places.end()) scan.places.push_back(alias.place);
			// A stem blanks its whole word.
			std::size_t end = at + word.size();
			if (alias.prefix) while (end < window.size() && word_byte(window, end)) ++end;
			blank(window, at, end - at);
		}
	}
	// What follows a locative word, up to where the place's words end. A comma
	// does not end it: "in London, New York and Tokyo" is one list, and every
	// place of it is read - a place Singularity keeps no calendar of is never
	// silently dropped. A list item that opens a new clause (", provided",
	// ", which", ", in each case") ends it.
	static const std::vector<std::string> stops{";", ".", ":", "(", ")", " are ", " is ", " shall ", " generally", " for ",
		" to ", " on ", " under ", " authori", " required", " open", " closed", " close", " in ", " which", " where ",
		" \xD0\xBD\xD0\xB5 ", " \xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x82", " \xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82",  // …
		" \xD0\xB7\xD0\xB0\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82", " \xD0\xBE\xD1\x81\xD1\x83\xD1\x89\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2"};  // …
	static const std::vector<std::string> notPlaces{"the", "city", "state", "of", "or", "and", "county", "borough",
		"which", "accordance", "ordinary", "general", "respect", "relation", "connection", "addition", "lieu", "order", "each", "both",
		"either",
		"any", "such", "that", "its", "their", "this", "full", "writing", "question", "case", "a", "an", "course", "business",
		"\xD0\xB3",  // …
		"\xD0\xB8", "\xD0\xB8\xD0\xBB\xD0\xB8",  // …
		"\xD0\xB3\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB4\xD0\xB5",  // …
		"\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB9", "\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB5",  // …
		"\xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",  // …
		"\xD1\x81\xD0\xBE\xD0\xBE\xD1\x82\xD0\xB2\xD0\xB5\xD1\x82\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8\xD0\xB8",  // …
		"\xD1\x81\xD0\xBB\xD1\x83\xD1\x87\xD0\xB0\xD0\xB5", "\xD0\xBF\xD0\xBE\xD1\x80\xD1\x8F\xD0\xB4\xD0\xBA\xD0\xB5"};  // …
	// A locative whose first word is a function word ("in which", "in
	// accordance", «…») names no place.
	static const std::vector<std::string> functionFirst{"which", "accordance", "respect", "relation", "connection", "addition",
		"lieu", "order", "writing", "full", "each", "any", "such", "case", "whole", "part", "general", "ordinary", "the ordinary",
		"\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB9", "\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB5",  // …
		"\xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",  // …
		"\xD1\x81\xD0\xBE\xD0\xBE\xD1\x82\xD0\xB2\xD0\xB5\xD1\x82\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8\xD0\xB8",  // …
		"\xD1\x81\xD0\xBB\xD1\x83\xD1\x87\xD0\xB0\xD0\xB5", "\xD0\xBF\xD0\xBE\xD1\x80\xD1\x8F\xD0\xB4\xD0\xBA\xD0\xB5",  // …
		"\xD1\x81\xD0\xB2\xD1\x8F\xD0\xB7\xD0\xB8"};  // …
	for (const std::string &locative : {std::string(" in "), std::string(" \xD0\xB2 ")}) {
		for (std::size_t at = window.find(locative); at != std::string::npos; at = window.find(locative, at + 1)) {
			const std::size_t from = at + locative.size();
			std::size_t end = window.size();
			for (const std::string &stop : stops) {
				const std::size_t found = window.find(stop, from);
				if (found != std::string::npos && found < end) end = found;
			}
			// The words, item by item (a comma opens the next item).
			std::vector<std::vector<std::string>> items(1);
			std::string word;
			for (std::size_t index = from; index <= end; ++index) {
				if (index < end && word_byte(window, index)) {
					word += window[index];
					continue;
				}
				if (!word.empty()) items.back().push_back(word);
				word.clear();
				if (index < end && window[index] == ',') items.emplace_back();
			}
			const auto first_word = [](const std::vector<std::string> &item) -> std::string {
				for (const std::string &one : item)
					if (one != "and" && one != "or" && one != "\xD0\xB8" && one != "\xD0\xB8\xD0\xBB\xD0\xB8") return one;  // …
				return {};
			};
			// A locative whose first word is a function word names no place ("in
			// which", "in accordance"); one whose words are all known places (blanked
			// above) leaves only empty items.
			if (!items.front().empty() && std::find(functionFirst.begin(), functionFirst.end(), items.front().front()) != functionFirst.end())
				continue;
			for (std::size_t item = 0; item < items.size(); ++item) {
				if (item > 0) {
					const std::string opening = first_word(items[item]);
					if (!opening.empty() && std::find(clauseOpeners().begin(), clauseOpeners().end(), opening) != clauseOpeners().end()) break;
				}
				for (const std::string &one : items[item])
					if (std::find(notPlaces.begin(), notPlaces.end(), one) == notPlaces.end()) scan.other = true;
			}
		}
	}
	return scan;
}

// Places written BEFORE the authority's noun ("Frankfurt and New York banks",
// "Delaware banks"): the capitalised English words right before `at` (in
// `original`, the unfolded text at the same offsets), back to a word that ends
// such a list. Adjectives of the noun ("commercial", "clearing") and "and" /
// "or" are passed over; a lower-case word ends the list ("open for trading and
// commercial banks"). `other` when a word is left that is no place Singularity knows.
PlaceScan places_before(const std::string &text, const std::string &original, std::size_t at) {
	static const std::set<std::string> ends{"which", "on", "whom", "where", "when", "that", "the", "a", "an", "any", "other",
		"than", "day", "days", "of", "in", "if", "for", "by", "its", "their", "are", "is", "be", "to", "at", "with", "all", "each",
		"such", "no", "not", "nor", "means", "mean", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
		"mondays", "tuesdays", "wednesdays", "thursdays", "fridays", "saturdays", "sundays", "weekend", "weekends", "holiday",
		"holidays", "close", "closed", "open", "shall", "will", "may", "including", "excluding", "except", "under", "as", "use",
		"using", "count", "counting", "apply", "applying", "follow", "following", "observe", "treat", "exclude", "include", "plus",
		"per", "both", "either", "only", "also"};
	static const std::set<std::string> adjectives{"commercial", "clearing", "major", "principal", "main", "leading", "national",
		"federal", "state", "savings", "member", "retail", "foreign", "domestic", "international", "local", "licensed", "chartered",
		"relevant", "designated", "applicable", "participating", "depository", "general", "public", "legal", "statutory",
		"central", "reserve", "investment", "banking", "bank", "and", "or", "&"};
	std::vector<std::string> words;
	std::size_t cursor = at;
	while (cursor > 0 && words.size() < 8) {
		while (cursor > 0 && (text[cursor - 1] == ' ' || text[cursor - 1] == ',' || text[cursor - 1] == '\'' || text[cursor - 1] == '.'))
			--cursor;
		const std::size_t end = cursor;
		while (cursor > 0 && word_byte(text, cursor - 1) && static_cast<unsigned char>(text[cursor - 1]) < 0x80) --cursor;
		if (cursor == end) break;  // not an English word
		const std::string word = text.substr(cursor, end - cursor);
		if (ends.count(word) != 0) break;
		const bool capital = cursor < original.size() && std::isupper(static_cast<unsigned char>(original[cursor])) != 0;
		if (!capital && adjectives.count(word) == 0) break;
		if (word.size() > 1) words.push_back(word);  // "U.S.", "N.Y.": letters of an abbreviation
	}
	PlaceScan scan;
	if (words.empty()) return scan;
	std::string list;
	for (auto word = words.rbegin(); word != words.rend(); ++word) list += " " + *word;
	PlaceScan read = scan_places(" in" + list + " ");
	scan.places = read.places;
	// What is left once the known places are blanked: an adjective of the noun,
	// or a place Singularity does not know.
	std::string rest = " " + list + " ";
	for (const PlaceAlias &alias : place_aliases()) {
		const std::string word(alias.alias);
		for (std::size_t found = find_word(rest, word, 0, alias.prefix); found != std::string::npos;
				found = find_word(rest, word, found + 1, alias.prefix))
			blank(rest, found, word.size());
	}
	std::string one;
	for (std::size_t index = 0; index <= rest.size(); ++index) {
		if (index < rest.size() && rest[index] != ' ') {
			one += rest[index];
			continue;
		}
		if (!one.empty() && adjectives.count(one) == 0) scan.other = true;
		one.clear();
	}
	return scan;
}

struct Cue {
	const char *words;
	Authority kind;
	const char *place;     // fixed place, or nullptr: the window's places
	bool prefix;
	bool possible;         // the calendar's discretionary days count as uncertain under these words
};

// The closure authorities a definition may name, most specific first.
const std::vector<Cue> &cues() {
	static const std::vector<Cue> list{
		// Exchanges and payment systems before banks and places ("New York Stock Exchange").
		{"new york stock exchange", Authority::Exchange, "nyse", false, false},
		{"nyse", Authority::Exchange, "nyse", false, false},
		{"stock exchange", Authority::Exchange, nullptr, false, false},
		{"securities exchange", Authority::Exchange, nullptr, false, false},
		{"exchange is open", Authority::Exchange, nullptr, false, false},
		{"exchanges are open", Authority::Exchange, nullptr, false, false},
		{"nasdaq", Authority::Exchange, "nasdaq", false, false},
		{"trading day", Authority::Exchange, nullptr, false, false},
		{"securities market", Authority::Exchange, nullptr, false, false},
		{"bond market", Authority::Exchange, nullptr, false, false},
		{"sifma", Authority::Exchange, "sifma", false, false},
		{"\xD0\xB1\xD0\xB8\xD1\x80\xD0\xB6", Authority::Exchange, nullptr, true, false},  // …
		{"trans-european automated real-time gross settlement", Authority::PaymentSystem, "target2", false, false},
		{"target2", Authority::PaymentSystem, "target2", false, false},
		{"target 2", Authority::PaymentSystem, "target2", false, false},
		{"target system", Authority::PaymentSystem, "target2", false, false},
		{"target day", Authority::PaymentSystem, "target2", false, false},
		{"t2 system", Authority::PaymentSystem, "target2", false, false},
		{"fedwire", Authority::PaymentSystem, "fedwire", false, false},
		{"chaps", Authority::PaymentSystem, "chaps", false, false},
		{"sepa", Authority::PaymentSystem, "sepa", false, false},
		{"payment system", Authority::PaymentSystem, nullptr, false, false},
		{"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB6\xD0\xBD", Authority::PaymentSystem, nullptr, true, false},  // …
		// Central banks.
		{"federal reserve bank of new york", Authority::CentralBank, "united states", false, false},
		{"federal reserve bank", Authority::CentralBank, "united states", false, false},
		{"federal reserve system", Authority::CentralBank, "united states", false, false},
		{"federal reserve", Authority::CentralBank, "united states", false, false},
		{"\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA \xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8", Authority::CentralBank, "russia", false, false},  // …
		{"\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xB0 \xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8", Authority::CentralBank, "russia", false, false},  // …
		{"\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82\xD1\x80\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD", Authority::CentralBank, "russia", true, false},  // …(…)
		// Government offices.
		{"federal government", Authority::GovernmentOffices, "united states", false, false},
		{"united states government", Authority::GovernmentOffices, "united states", false, false},
		{"u.s. government", Authority::GovernmentOffices, "united states", false, false},
		{"government of the united states", Authority::GovernmentOffices, "united states", false, false},
		{"federal offices", Authority::GovernmentOffices, "united states", false, false},
		// The law's public holidays.
		{"legal public holiday", Authority::PublicHolidays, "united states", true, false},
		{"6103", Authority::PublicHolidays, "united states", false, false},
		{"federal holiday", Authority::PublicHolidays, "united states", true, true},
		{"\xD0\xBD\xD0\xB5\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD1\x85 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD", Authority::PublicHolidays, "russia", true, false},  // …
		{"\xD0\xBD\xD0\xB5\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD", Authority::PublicHolidays, "russia", true, false},  // …
		{"\xD0\xBD\xD0\xB5\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD0\xBC\xD0\xB8 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD", Authority::PublicHolidays, "russia", true, false},  // …
		{"\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB8\xD0\xB7\xD0\xB2\xD0\xBE\xD0\xB4\xD1\x81\xD1\x82\xD0\xB2\xD0\xB5\xD0\xBD\xD0\xBD", Authority::PublicHolidays, "russia", true, false},  // …(…)
		{"bank holiday", Authority::PublicHolidays, nullptr, true, false},
		{"public holiday", Authority::PublicHolidays, nullptr, true, false},
		{"legal holiday", Authority::PublicHolidays, nullptr, true, false},
		{"statutory holiday", Authority::PublicHolidays, nullptr, true, false},
		{"national holiday", Authority::PublicHolidays, nullptr, true, false},
		{"state holiday", Authority::PublicHolidays, nullptr, true, false},
		{"holiday", Authority::PublicHolidays, nullptr, true, false},
		{"\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD", Authority::PublicHolidays, nullptr, true, false},  // …
		// Banks.
		{"principal financial cent", Authority::Banks, nullptr, true, false},
		{"banking institution", Authority::Banks, nullptr, true, false},
		{"interbank market", Authority::Banks, nullptr, false, false},
		{"commercial bank", Authority::Banks, nullptr, true, false},
		{"bank", Authority::Banks, nullptr, true, false},
		{"\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA", Authority::Banks, nullptr, true, false},  // …
		{"\xD0\xBA\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD0\xBD", Authority::Banks, nullptr, true, false},  // …(…)
	};
	return list;
}

// Where a cue's clause ends: the next item of the definition's list.
std::size_t clause_end(const std::string &text, std::size_t from) {
	std::size_t end = text.size();
	for (const char *stop : {";", ", or ", " or a ", " or any ", " or on ", " or the ", " and a day", " and, in", ", and ", " nor ",
			"(", ")", " \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xB4", ", \xD0\xB0 \xD1\x82\xD0\xB0\xD0\xBA\xD0\xB6\xD0\xB5",  // …, , …
			" \xD0\xBB\xD0\xB8\xD0\xB1\xD0\xBE "}) {  // …
		const std::size_t found = text.find(stop, from);
		if (found != std::string::npos && found < end) end = found;
	}
	return end;
}

// Where the clause holding `at` begins.
std::size_t clause_start(const std::string &text, std::size_t at) {
	std::size_t start = at >= 40 ? at - 40 : 0;
	for (const char *stop : {",", ";", "(", " or ", " and ", " other than ", " \xD0\xB8 ", " \xD0\xB8\xD0\xBB\xD0\xB8 "}) {
		const std::size_t found = text.rfind(stop, at);
		if (found != std::string::npos && found >= start && found < at) start = found + std::string(stop).size();
	}
	return start;
}

bool is_bankrupt(const std::string &text, std::size_t at) {
	return text.compare(at, 8, "bankrupt") == 0;
}

// Month names, English and Russian genitive, for the days a definition names.
const char *const kEnglishMonths[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september",
	"october", "november", "december"};
const char *const kRussianMonths[] = {
	"\xD1\x8F\xD0\xBD\xD0\xB2\xD0\xB0\xD1\x80\xD1\x8F", "\xD1\x84\xD0\xB5\xD0\xB2\xD1\x80\xD0\xB0\xD0\xBB\xD1\x8F",
	"\xD0\xBC\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0", "\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB5\xD0\xBB\xD1\x8F", "\xD0\xBC\xD0\xB0\xD1\x8F",
	"\xD0\xB8\xD1\x8E\xD0\xBD\xD1\x8F", "\xD0\xB8\xD1\x8E\xD0\xBB\xD1\x8F", "\xD0\xB0\xD0\xB2\xD0\xB3\xD1\x83\xD1\x81\xD1\x82\xD0\xB0",
	"\xD1\x81\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8F\xD0\xB1\xD1\x80\xD1\x8F", "\xD0\xBE\xD0\xBA\xD1\x82\xD1\x8F\xD0\xB1\xD1\x80\xD1\x8F",
	"\xD0\xBD\xD0\xBE\xD1\x8F\xD0\xB1\xD1\x80\xD1\x8F", "\xD0\xB4\xD0\xB5\xD0\xBA\xD0\xB0\xD0\xB1\xD1\x80\xD1\x8F"};

// The days a definition names itself: "December 24", "24 December", «31
// …», "Good Friday", "Christmas Eve" - each blanked once read.
void named_days_in(std::string &text, std::vector<DayRule> &days) {
	static const std::vector<std::pair<const char *, DayRule>> names{
		// The day after the fourth Thursday of November: the Friday on or after
		// November 23.
		{"friday after thanksgiving", on_or_after("Day after Thanksgiving", 11, 23, 4)},
		{"friday following thanksgiving", on_or_after("Day after Thanksgiving", 11, 23, 4)},
		{"friday immediately following thanksgiving", on_or_after("Day after Thanksgiving", 11, 23, 4)},
		{"day after thanksgiving", on_or_after("Day after Thanksgiving", 11, 23, 4)},
		{"day following thanksgiving", on_or_after("Day after Thanksgiving", 11, 23, 4)},
		{"black friday", on_or_after("Day after Thanksgiving", 11, 23, 4)},
		{"good friday", easter("Good Friday", -2)},
		{"easter monday", easter("Easter Monday", 1)},
		{"christmas eve", fixed("Christmas Eve", 12, 24)},
		{"new year's eve", fixed("New Year's Eve", 12, 31)},
		{"new year\xE2\x80\x99s eve", fixed("New Year's Eve", 12, 31)},
		{"boxing day", fixed("Boxing Day", 12, 26)},
		{"christmas day", fixed("Christmas Day", 12, 25)},
		{"new year's day", fixed("New Year's Day", 1, 1)},
		{"new year\xE2\x80\x99s day", fixed("New Year's Day", 1, 1)},
	};
	for (const auto &[words, rule] : names) {
		const std::string word(words);
		for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
			days.push_back(rule);
			blank(text, at, word.size());
		}
	}
	const auto dated = [&](unsigned month, unsigned day, std::size_t at, std::size_t length) {
		if (day == 0 || day > 31) return;
		DayRule rule = fixed("a day the definition names", month, day);
		// "December 24, 2026": that year only.
		static const std::regex year("^,? ?(\\d{4})");
		std::smatch found;
		const std::string after = text.substr(at + length, 8);
		std::size_t used = 0;
		if (std::regex_search(after, found, year)) {
			rule.from_year = rule.to_year = std::stoi(found[1].str());
			used = static_cast<std::size_t>(found.length(0));
		}
		days.push_back(rule);
		blank(text, at, length + used);
	};
	for (unsigned month = 1; month <= 12; ++month) {
		const std::string english(kEnglishMonths[month - 1]);
		const std::string russian(kRussianMonths[month - 1]);
		for (const std::string &name : {english, russian}) {
			for (std::size_t at = find_word(text, name, 0, false); at != std::string::npos; at = find_word(text, name, at + 1, false)) {
				// "<month> <day>" (English) or "<day> <month>" (both).
				std::size_t digits = at + name.size() + 1;
				std::size_t end = digits;
				while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0 && end - digits < 2) ++end;
				if (end > digits && name == english && text[at + name.size()] == ' ' && !letter_at(text, end + (text.compare(end, 2, "st") == 0
						|| text.compare(end, 2, "nd") == 0 || text.compare(end, 2, "rd") == 0 || text.compare(end, 2, "th") == 0 ? 2 : 0))) {
					dated(month, static_cast<unsigned>(std::stoi(text.substr(digits, end - digits))), at, end - at);
					continue;
				}
				std::size_t back = at;
				while (back > 0 && text[back - 1] == ' ') --back;
				// "24th of December", "24 December", «…»
				if (back >= 3 && text.compare(back - 3, 3, " of") == 0) back -= 3;
				while (back > 0 && text[back - 1] == ' ') --back;
				if (back >= 2 && (text.compare(back - 2, 2, "st") == 0 || text.compare(back - 2, 2, "nd") == 0
						|| text.compare(back - 2, 2, "rd") == 0 || text.compare(back - 2, 2, "th") == 0))
					back -= 2;
				std::size_t start = back;
				while (start > 0 && std::isdigit(static_cast<unsigned char>(text[start - 1])) != 0 && back - start < 2) --start;
				if (start < back && !letter_before(text, start))
					dated(month, static_cast<unsigned>(std::stoi(text.substr(start, back - start))), start, at + name.size() - start);
			}
		}
	}
}

// The words of a folded text: ASCII letters and digits, Cyrillic letters.
std::vector<std::string> words_in(const std::string &folded) {
	std::vector<std::string> words;
	std::string word;
	for (std::size_t index = 0; index <= folded.size(); ++index) {
		if (index < folded.size() && word_byte(folded, index)) {
			word += folded[index];
			continue;
		}
		if (!word.empty()) words.push_back(word);
		word.clear();
	}
	return words;
}

// Whether a closing condition of a definition stands in a clause no cue read:
// each "closed" / "open" / «…» is looked at in its clause
// (up to the definition's next "; or", ", or", "or the", parenthesis ...); a
// clause that holds no cue and names more than generic words and known places
// ("the Landlord's management office", "the Secretary of State of the State of
// Delaware") is a closure Singularity cannot read.
bool unread_closure(const std::string &text, const std::vector<std::size_t> &cueAt) {
	static const std::vector<std::string> delimiters{";", ", or ", " or a ", " or an ", " or any ", " or on ", " or the ", " or if ",
		" or when ", " or where ", " nor ", ", and ", " and a day", " and any day", " and on ", "(", ")", ":",
		" \xD0\xBB\xD0\xB8\xD0\xB1\xD0\xBE ", " \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xB4", " \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xB2 ",  // …
		", \xD0\xB0 \xD1\x82\xD0\xB0\xD0\xBA\xD0\xB6\xD0\xB5", ", \xD0\xB8\xD0\xBB\xD0\xB8 "};  // , …
	static const std::vector<std::string> verbs{" closed", " close ", " close.", " close,", " closes", " open ", " open.", " open,",
		" opened", " shut", "\xD0\xB7\xD0\xB0\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82", "\xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82",  // …
		"\xD0\xBD\xD0\xB5 \xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x82", "\xD0\xBD\xD0\xB5 \xD0\xBE\xD1\x81\xD1\x83\xD1\x89\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2"};  // …
	static const std::set<std::string> generic{"a", "an", "the", "any", "day", "days", "on", "which", "is", "are", "be", "been",
		"being", "to", "by", "or", "and", "of", "in", "for", "law", "laws", "required", "authorized", "authorised", "permitted",
		"obligated", "closed", "close", "closes", "open", "opened", "shut", "not", "generally", "general", "business", "other",
		"than", "such", "that", "its", "their", "it", "they", "executive", "order", "orders", "regulation", "regulations",
		"governmental", "action", "remain", "remains", "customarily", "ordinarily", "usually", "dealings", "transact",
		"transacting", "conduct", "conducting", "carry", "carrying", "public", "as", "result", "at", "all", "times", "city",
		"state", "place", "places", "with", "under", "pursuant", "applicable", "relevant", "located", "situated", "where", "when",
		"whom", "there", "said", "same", "respective", "respectively", "transactions", "payments", "payment", "settlement",
		"settlements", "deposits", "foreign", "currency", "trading", "make", "making", "receive", "receiving", "normal",
		"ordinary", "hours", "institutions", "customers", "public", "effect", "effecting", "otherwise", "also", "either",
		"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday", "weekend", "herein", "hereunder",
		"means", "mean", "shall", "will", "may", "must", "do", "does", "wholly", "partially", "fully", "entire", "whole"};
	// Russian words, by stem.
	static const std::vector<std::string> genericStems{"\xD0\xB4\xD0\xB5\xD0\xBD", "\xD0\xB4\xD0\xBD", "\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80",  // …
		"\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xB4\xD0\xB0", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x82", "\xD0\xB7\xD0\xB0\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82",  // …
		"\xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82", "\xD0\xB4\xD0\xBB\xD1\x8F", "\xD0\xB8\xD0\xBB\xD0\xB8",  // …
		"\xD0\xBE\xD1\x81\xD1\x83\xD1\x89\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2", "\xD0\xBE\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86",  // …
		"\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xB4", "\xD1\x81\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD1\x88", "\xD0\xBE\xD0\xB1\xD1\x89",  // …
		"\xD0\xB4\xD0\xB5\xD1\x8F\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C", "\xD1\x81\xD0\xBE\xD0\xBE\xD1\x82\xD0\xB2\xD0\xB5\xD1\x82\xD1\x81\xD1\x82\xD0\xB2",  // …
		"\xD0\xB7\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD", "\xD0\xB3\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB4", "\xD0\xBA\xD0\xBB\xD0\xB8\xD0\xB5\xD0\xBD\xD1\x82",  // …
		"\xD0\xBE\xD0\xB1\xD1\x81\xD0\xBB\xD1\x83\xD0\xB6", "\xD1\x80\xD0\xB0\xD1\x81\xD1\x87\xD0\xB5\xD1\x82", "\xD0\xB2\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBD",  // …
		"\xD0\xBD\xD0\xB5\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87", "\xD1\x8F\xD0\xB2\xD0\xBB\xD1\x8F", "\xD1\x81\xD1\x83\xD0\xB1\xD0\xB1\xD0\xBE\xD1\x82",  // …
		"\xD0\xB2\xD0\xBE\xD1\x81\xD0\xBA\xD1\x80\xD0\xB5\xD1\x81", "\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB6", "\xD0\xBE\xD0\xB1\xD1\x8B\xD1\x87\xD0\xBD",  // …
		"\xD1\x83\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB", "\xD1\x80\xD0\xB5\xD0\xB6\xD0\xB8\xD0\xBC", "\xD1\x81\xD0\xB2\xD0\xBE"};  // …
	static const std::set<std::string> genericShort{"\xD0\xB2", "\xD0\xB8", "\xD1\x81", "\xD0\xB3", "\xD0\xBF\xD0\xBE", "\xD0\xBD\xD0\xB0",  // …
		"\xD0\xBD\xD0\xB5", "\xD0\xB0", "\xD0\xBA", "\xD0\xBE", "\xD0\xBE\xD0\xB1", "\xD1\x83", "\xD0\xB8\xD1\x85", "\xD0\xB5\xD0\xB5"};  // …
	for (const std::string &verb : verbs) {
		for (std::size_t at = text.find(verb); at != std::string::npos; at = text.find(verb, at + 1)) {
			std::size_t from = 0;
			std::size_t to = text.size();
			for (const std::string &delimiter : delimiters) {
				const std::size_t before = at == 0 ? std::string::npos : text.rfind(delimiter, at);
				if (before != std::string::npos && before + delimiter.size() <= at) from = std::max(from, before + delimiter.size());
				const std::size_t after = text.find(delimiter, at + 1);
				if (after != std::string::npos) to = std::min(to, after);
			}
			if (std::any_of(cueAt.begin(), cueAt.end(), [&](std::size_t cue) { return cue >= from && cue < to; })) continue;
			std::string clause = " " + text.substr(from, to - from) + " ";
			for (const PlaceAlias &alias : place_aliases()) {
				const std::string word(alias.alias);
				for (std::size_t found = find_word(clause, word, 0, alias.prefix); found != std::string::npos;
						found = find_word(clause, word, found + 1, alias.prefix)) {
					std::size_t end = found + word.size();
					if (alias.prefix) while (end < clause.size() && word_byte(clause, end)) ++end;
					blank(clause, found, end - found);
				}
			}
			for (const std::string &word : words_in(clause)) {
				const bool cyrillic = static_cast<unsigned char>(word[0]) >= 0x80;
				if (!cyrillic) {
					if (word.size() > 1 && generic.count(word) == 0 && !std::all_of(word.begin(), word.end(), [](char c) {
							return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
						return true;
					continue;
				}
				if (genericShort.count(word) != 0) continue;
				if (std::none_of(genericStems.begin(), genericStems.end(), [&](const std::string &stem) { return word.rfind(stem, 0) == 0; }))
					return true;
			}
		}
	}
	return false;
}

} // namespace

ParsedDefinition parse_definition(std::string_view words) {
	ParsedDefinition parsed;
	const std::string original = " " + std::string(words) + " ";
	std::string text = " " + fold(words) + " ";
	// «…» of «…» is a town's abbreviation.
	for (std::size_t hit = text.find("\xD0\xB3."); hit != std::string::npos; hit = text.find("\xD0\xB3.", hit + 3))
		if (!letter_before(text, hit)) text[hit + 2] = ' ';
	// The defined term itself ("banking day" holds "bank"), wherever it stands.
	for (const char *term : {"business day", "banking day", "working day"}) {
		const std::string word(term);
		for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) blank(text, at, word.size());
	}
	for (const char *stem : {"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87", "\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA"}) {
		const std::string word(stem);  // …(…) …(…) …
		for (std::size_t at = find_word(text, word, 0, true); at != std::string::npos; at = find_word(text, word, at + 1, true)) {
			const std::size_t space = text.find(' ', at);
			if (space == std::string::npos || space - at > 24 || text.compare(space + 1, 2, "\xD0\xB4") != 0) continue;
			std::size_t end = space + 1;
			while (end < text.size() && word_byte(text, end)) ++end;
			blank(text, at, end - at);
		}
	}
	// The days it names itself.
	named_days_in(text, parsed.named_days);
	// The weekdays it excludes - by name, or as "weekend" / «…». A
	// weekday named as ONE day ("the Friday after Thanksgiving" read above; "the
	// Monday following Easter") is no weekend: one Singularity cannot read makes the
	// definition not understood, never a weekday closed every week.
	bool unreadDay = false;
	if (has(text, {"monday through friday", "monday to friday", "monday-friday", "mondays through fridays",
			"\xD1\x81 \xD0\xBF\xD0\xBE\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xB0 \xD0\xBF\xD0\xBE \xD0\xBF\xD1\x8F\xD1\x82\xD0\xBD\xD0\xB8\xD1\x86\xD1\x83"})) {  // …
		parsed.weekend = {5, 6};
		parsed.weekdays_named = true;
		parsed.weekdays_explicit = true;
	} else {
		static const std::vector<std::pair<const char *, int>> days{
			{"monday", 0}, {"tuesday", 1}, {"wednesday", 2}, {"thursday", 3}, {"friday", 4}, {"saturday", 5}, {"sunday", 6},
			{"\xD0\xBF\xD0\xBE\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD", 0}, {"\xD0\xB2\xD1\x82\xD0\xBE\xD1\x80\xD0\xBD\xD0\xB8\xD0\xBA", 1},
			{"\xD1\x87\xD0\xB5\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB3", 3}, {"\xD0\xBF\xD1\x8F\xD1\x82\xD0\xBD\xD0\xB8\xD1\x86", 4},
			{"\xD1\x81\xD1\x83\xD0\xB1\xD0\xB1\xD0\xBE\xD1\x82", 5}, {"\xD0\xB2\xD0\xBE\xD1\x81\xD0\xBA\xD1\x80\xD0\xB5\xD1\x81\xD0\xB5\xD0\xBD", 6}};
		static const std::vector<std::string> oneDay{"after", "before", "following", "preceding", "immediately", "next", "prior",
			"of", "\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5", "\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB4",  // …
			"\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4\xD1\x83\xD1\x8E\xD1\x89"};  // …
		std::set<int> named;
		for (const auto &[stem, day] : days) {
			const std::string word(stem);
			for (std::size_t at = find_word(text, word, 0, true); at != std::string::npos; at = find_word(text, word, at + 1, true)) {
				std::size_t end = at + word.size();
				while (end < text.size() && word_byte(text, end)) ++end;
				while (end < text.size() && text[end] == ' ') ++end;
				std::size_t next = end;
				while (next < text.size() && word_byte(text, next)) ++next;
				const std::string following = text.substr(end, next - end);
				if (std::any_of(oneDay.begin(), oneDay.end(), [&](const std::string &cue) { return following.rfind(cue, 0) == 0; })) {
					unreadDay = true;
					continue;
				}
				named.insert(day);
				parsed.weekdays_explicit = true;
			}
		}
		if (has_word(text, {"weekend", "week-end", "\xD0\xB2\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBD"}, true)) named.insert({5, 6});  // …
		if (!named.empty()) {
			parsed.weekend = named;
			parsed.weekdays_named = true;
		}
	}
	// A limb that applies only in some case - LMA's "(a) (in relation to a
	// currency other than euro) the principal financial centre of the country of
	// that currency; or (b) (in relation to euro) any TARGET Day" - from where it
	// opens: every authority it names is conditional.
	std::size_t conditionalFrom = std::string::npos;
	for (const char *cue : {"in relation to", "in respect of", "principal financial cent", "if the currency", "where the currency",
			"for the purposes of", "if applicable", "where applicable", "in the case of", "denominated in",
			"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xBE \xD0\xBA",  // …
			"\xD0\xB2 \xD0\xBE\xD1\x82\xD0\xBD\xD0\xBE\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB8"}) {  // …
		const std::size_t found = text.find(cue);
		if (found != std::string::npos && found < conditionalFrom) conditionalFrom = found;
	}
	if (conditionalFrom != std::string::npos) {
		const std::size_t low = conditionalFrom >= 48 ? conditionalFrom - 48 : 0;
		for (const char opener : {'(', ':', ';'}) {
			const std::size_t found = text.rfind(opener, conditionalFrom);
			if (found != std::string::npos && found >= low && found < conditionalFrom) {
				conditionalFrom = found;
				break;
			}
		}
	}
	// The closure authorities, each with the places its clause names.
	const bool authorized = has(text, {"authorized", "authorised", "permitted", "\xD0\xB2\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xB5",  // …
		"\xD1\x80\xD0\xB0\xD0\xB7\xD1\x80\xD0\xB5\xD1\x88"});  // …
	const auto add = [&](Authority kind, const std::string &place, bool possible, bool conditional) {
		for (NamedAuthority &known : parsed.authorities)
			if (known.kind == kind && known.place == place) {
				known.authorized = known.authorized || possible;
				known.conditional = known.conditional && conditional;
				return;
			}
		NamedAuthority authority;
		authority.kind = kind;
		authority.place = place;
		authority.authorized = possible;
		authority.conditional = conditional;
		parsed.authorities.push_back(std::move(authority));
	};
	std::vector<std::size_t> cueAt;
	for (const Cue &cue : cues()) {
		const std::string word(cue.words);
		for (std::size_t at = find_word(text, word, 0, cue.prefix); at != std::string::npos; at = find_word(text, word, at + 1, cue.prefix)) {
			if (cue.kind == Authority::Banks && is_bankrupt(text, at)) continue;
			std::size_t cueEnd = at + word.size();
			if (cue.prefix) while (cueEnd < text.size() && word_byte(text, cueEnd)) ++cueEnd;
			const bool possible = cue.possible || (authorized && cue.kind == Authority::Banks);
			const bool conditional = conditionalFrom != std::string::npos && at >= conditionalFrom;
			cueAt.push_back(at);
			if (cue.place != nullptr) {
				add(cue.kind, cue.place, possible, conditional);
			} else {
				std::size_t end = clause_end(text, cueEnd);
				// A place list the clause's end cut ("in London, New York, and
				// Tokyo"): its last item is read too.
				for (const char *joint : {", and ", ", or "}) {
					const std::string glue(joint);
					if (text.compare(end, glue.size(), glue) != 0) continue;
					std::size_t itemEnd = end + glue.size();
					while (itemEnd < text.size() && std::string(",;.():").find(text[itemEnd]) == std::string::npos) ++itemEnd;
					const std::vector<std::string> item = words_in(text.substr(end + glue.size(), itemEnd - end - glue.size()));
					if (!item.empty() && item.size() <= 4
							&& std::find(clauseOpeners().begin(), clauseOpeners().end(), item.front()) == clauseOpeners().end()
							&& std::none_of(item.begin(), item.end(), [](const std::string &one) {
								return one == "are" || one == "is" || one == "shall" || one == "day" || one == "days"; }))
						end = itemEnd;
				}
				const std::size_t start = clause_start(text, at);
				std::string window = text.substr(start, at - start) + " " + text.substr(cueEnd, end - cueEnd);
				PlaceScan scan = scan_places(" " + window + " ");
				// Places written before the noun ("Frankfurt and New York banks").
				const PlaceScan before = places_before(text, original, at);
				for (const std::string &place : before.places)
					if (std::find(scan.places.begin(), scan.places.end(), place) == scan.places.end()) scan.places.push_back(place);
				scan.other = scan.other || before.other;
				std::vector<std::string> places;
				for (const std::string &place : scan.places)
					if (place != "nyse" && place != "target2") places.push_back(place);
				for (const std::string &place : places) add(cue.kind, place, possible, conditional);
				if (scan.other || places.empty()) add(cue.kind, "", possible, conditional);
			}
			blank(text, at, cueEnd - at);
		}
	}
	// A closing condition no cue reads ("or a day on which the Landlord's
	// management office is closed", "or the Secretary of State of the State of
	// Delaware is closed"): the definition then names closures Singularity keeps no
	// calendar of - whatever else it names, its holidays are not known.
	if (unread_closure(text, cueAt)) add(Authority::Unknown, "", false, false);
	parsed.no_holidays = parsed.weekdays_named && parsed.authorities.empty() && parsed.named_days.empty();
	parsed.understood = (parsed.weekdays_named || !parsed.authorities.empty() || !parsed.named_days.empty()) && !unreadDay;
	return parsed;
}

BusinessDayRule resolve(const ParsedDefinition &parsed, RuleSource source) {
	BusinessDayRule rule;
	rule.source = source;
	rule.weekend = parsed.weekend;
	rule.weekend_explicit = parsed.weekdays_explicit;
	rule.named_days = parsed.named_days;
	bool resolved = parsed.understood;
	for (NamedAuthority authority : parsed.authorities) {
		// A conditional limb closes no day for certain: whether it applies is not
		// known here.
		if (authority.conditional) {
			resolved = false;
			rule.authorities.push_back(std::move(authority));
			continue;
		}
		for (const CalendarEntry &entry : registry()) {
			const bool serves = std::any_of(entry.serves.begin(), entry.serves.end(), [&](const auto &served) {
				return served.first == authority.kind && !authority.place.empty() && served.second == authority.place;
			});
			if (!serves) continue;
			authority.calendar = entry.id;
			if (std::find(rule.calendars.begin(), rule.calendars.end(), entry.id) == rule.calendars.end())
				rule.calendars.push_back(entry.id);
			if (authority.authorized && (!entry.possible.empty() || !entry.possible_dates.empty())
					&& std::find(rule.possible_calendars.begin(), rule.possible_calendars.end(), entry.id) == rule.possible_calendars.end())
				rule.possible_calendars.push_back(entry.id);
			break;
		}
		if (authority.calendar.empty()) resolved = false;
		rule.authorities.push_back(std::move(authority));
	}
	rule.holidays_known = resolved;
	return rule;
}

std::optional<BusinessDayRule> named_rule(std::string_view name) {
	const std::string folded = fold(name);
	BusinessDayRule rule;
	rule.source = RuleSource::MatterRule;
	rule.holidays_known = true;
	std::string id = folded;
	if (folded == "federal" || folded == "us federal holidays" || folded == "us_federal_holidays") id = "us_federal";
	else if (folded == "nyc_banks" || folded == "new_york_banks" || folded == "ny_banks") id = "us_ny_banks";
	else if (folded == "russian" || folded == "russian_production" || folded == "ru") id = "ru_production";
	else if (folded == "london_banks" || folded == "uk_banks" || folded == "england") id = "gb_england_wales";
	else if (folded == "weekdays" || folded == "weekdays_only" || folded == "monday_to_friday") return rule;
	if (find_calendar(id) == nullptr) return std::nullopt;
	rule.calendars.push_back(id);
	return rule;
}

// ---------------------------------------------------------------------------
// Standing, definitions and clauses in the material.

namespace {

std::vector<std::string> words_of(const std::string &folded) {
	std::vector<std::string> words;
	std::string word;
	for (std::size_t index = 0; index < folded.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(folded[index]);
		if (std::isalnum(byte) != 0 || byte == 0xD0 || byte == 0xD1 || (byte >= 0x80 && byte <= 0xBF && !word.empty()))
			word += folded[index];
		else if (!word.empty()) {
			words.push_back(word);
			word.clear();
		}
	}
	if (!word.empty()) words.push_back(word);
	return words;
}

} // namespace

Standing standing_of(std::string_view title, std::string_view text) {
	std::string head(title);
	const std::size_t line = text.find('\n', text.size() > 0 && text[0] == '[' ? text.find('\n') + 1 : 0);
	head += "\n" + std::string(text.substr(0, std::min<std::size_t>(line == std::string_view::npos ? text.size() : line, 200)));
	const std::string folded = fold(head);
	if (has(folded, {"not signed", "not executed", "\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD"}))  // …
		return Standing::Draft;
	const std::vector<std::string> words = words_of(folded);
	const auto word = [&](std::initializer_list<const char *> wanted) {
		return std::any_of(words.begin(), words.end(), [&](const std::string &token) {
			return std::any_of(wanted.begin(), wanted.end(), [&](const char *one) { return token == one; });
		});
	};
	// «…» only as its own word: «…» are not drafts.
	if (word({"draft", "redline", "redlined", "superseded", "unsigned", "unexecuted", "\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82"}))
		return Standing::Draft;
	if (word({"executed", "signed", "countersigned", "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD",  // …
			"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB9",  // …
			"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBE"}))  // …
		return Standing::Executed;
	return Standing::Unknown;
}

namespace {

struct Found {
	std::size_t start = 0;
	std::size_t end = 0;
};

bool russian_definition_verb(const std::string &folded, std::size_t from, std::size_t to) {
	const std::string window = folded.substr(from, to > from ? to - from : 0);
	return has(window, {"\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB0",  // …
		"\xD0\xBF\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xBC\xD0\xB0",                   // …
		"\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xB5\xD1\x82",           // …
		"\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD1\x8E\xD1\x82",           // …
		"\xD1\x8F\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82",                   // …
		"\xD1\x8F\xD0\xB2\xD0\xBB\xD1\x8F\xD1\x8E\xD1\x82",                   // …
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB7\xD0\xBD\xD0\xB0"});                 // …
}

// Right after a quoted term: a dash or a colon ("…",
// "\"Business Day\": ...").
bool dash_or_colon_after(const std::string &folded, std::size_t after) {
	while (after < folded.size() && folded[after] == ' ') ++after;
	return (after < folded.size() && folded[after] == ':') || dash_at(folded, after) != 0;
}

// Right after a quoted term: its closing parenthesis ("(a \"Business Day\")").
bool closes_parenthesis(const std::string &folded, std::size_t after) {
	while (after < folded.size() && folded[after] == ' ') ++after;
	return after < folded.size() && folded[after] == ')';
}

// A defined term's parenthesis opens within 48 bytes before `at`: its index.
std::size_t opening_parenthesis(const std::string &folded, std::size_t at) {
	const std::size_t open = folded.rfind('(', at);
	if (open == std::string::npos || at - open > 48 || folded.find(')', open) < at) return std::string::npos;
	return open;
}

// Every definition of Business Day a text writes: where it begins and ends.
std::vector<Found> definition_spans(const std::string &folded) {
	std::vector<Found> found;
	const auto take = [&](std::size_t start, std::size_t end) {
		for (const Found &known : found)
			if (start < known.end && end > known.start) return;
		found.push_back({start, end});
	};
	// A trailing definition: "... any day other than a Saturday, Sunday or legal
	// holiday (a \"Business Day\")", «…»)».
	const auto trailing = [&](std::size_t quoteStart, std::size_t after) {
		if (quoteStart == std::string::npos || !closes_parenthesis(folded, after)) return false;
		const std::size_t open = opening_parenthesis(folded, quoteStart);
		if (open == std::string::npos) return false;
		const std::size_t start = sentence_start(folded, open);
		if (open <= start + 8) return false;
		const std::size_t close = folded.find(')', after);
		take(start, close == std::string::npos ? open : close + 1);
		return true;
	};
	for (const char *term : {"business day", "banking day", "working day"}) {
		const std::string word(term);
		for (std::size_t at = folded.find(word); at != std::string::npos; at = folded.find(word, at + word.size())) {
			std::size_t after = at + word.size();
			if (after < folded.size() && folded[after] == 's') ++after;
			std::size_t quote = 0;
			bool quotedAfter = false;
			while (quote_byte_at(folded, after, quote)) { after += quote; quotedAfter = true; }
			const std::size_t quoteStart = quote_before(folded, at);
			const bool quotedBefore = quoteStart != std::string::npos;
			const bool lineStart = at == 0 || folded[at - 1] == '\n' || (quotedBefore && (quoteStart == 0 || folded[quoteStart - 1] == '\n'));
			if (quotedBefore && quotedAfter && trailing(quoteStart, after)) continue;
			std::size_t rest = after;
			while (rest < folded.size() && folded[rest] == ' ') ++rest;
			std::string next = folded.substr(rest, 80);
			// ", as used herein, means"
			if (next.rfind(", as used", 0) == 0 || next.rfind("as used", 0) == 0 || next.rfind(", when used", 0) == 0) {
				const std::size_t means = next.find("mean");
				if (means == std::string::npos || means > 60) continue;
				next = next.substr(means);
			}
			const auto starts = [&](std::initializer_list<const char *> verbs) {
				return std::any_of(verbs.begin(), verbs.end(), [&](const char *verb) { return next.rfind(verb, 0) == 0; });
			};
			const bool strong = starts({"means", "mean ", "shall mean", "will mean", "shall be deemed to mean", "is deemed to mean",
				"refers to", "shall refer to", "has the meaning", "have the meaning", "shall have the meaning", "is defined", "are defined",
				"shall be construed", "is construed"});
			const bool marked = (quotedBefore && quotedAfter) || lineStart;
			const bool punctuated = marked && dash_or_colon_after(folded, after);
			const bool weak = marked && starts({"shall be ", "is ", "are ", "includes", "shall include"})
				&& next.substr(0, 60).find("day") != std::string::npos;
			if (!strong && !punctuated && !weak) continue;
			take(quotedBefore ? quoteStart : at, sentence_end(folded, at));
		}
	}
	for (const char *stem : {"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87", "\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA"}) {
		const std::string word(stem);
		for (std::size_t at = find_word(folded, word, 0, true); at != std::string::npos; at = find_word(folded, word, at + word.size(), true)) {
			// The word after the adjective is …
			const std::size_t space = folded.find(' ', at);
			if (space == std::string::npos || space - at > 24) continue;  // «…» is 20 bytes
			if (folded.compare(space + 1, 2, "\xD0\xB4") != 0) continue;  // …
			std::size_t nounEnd = space + 1;
			while (nounEnd < folded.size() && word_byte(folded, nounEnd)) ++nounEnd;
			const std::string noun = folded.substr(space + 1, nounEnd - space - 1);
			if (noun.rfind("\xD0\xB4\xD0\xB5\xD0\xBD", 0) != 0 && noun.rfind("\xD0\xB4\xD0\xBD", 0) != 0) continue;  // …
			std::size_t after = nounEnd;
			std::size_t quote = 0;
			bool quotedAfter = false;
			while (quote_byte_at(folded, after, quote)) { after += quote; quotedAfter = true; }
			const std::size_t quoteStart = quote_before(folded, at);
			const bool quotedBefore = quoteStart != std::string::npos;
			if (quotedBefore && quotedAfter) {
				if (trailing(quoteStart, after)) continue;
				if (dash_or_colon_after(folded, after)) {
					take(quoteStart, sentence_end(folded, at));
					continue;
				}
			}
			// Not "…": a defining verb stands right after,
			// or "…" right before and the verb after.
			const std::size_t before = at >= 8 ? at - 8 : 0;
			const bool under = folded.substr(before, at - before).find("\xD0\xBF\xD0\xBE\xD0\xB4 ") != std::string::npos;  // …
			if (!russian_definition_verb(folded, nounEnd, std::min(folded.size(), nounEnd + (under ? 60 : 40)))) continue;
			std::size_t start = under ? folded.rfind("\xD0\xBF\xD0\xBE\xD0\xB4 ", at) : at;
			if (const std::size_t opening = quote_before(folded, start); opening != std::string::npos) start = opening;
			take(start, sentence_end(folded, at));
		}
	}
	std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.start < b.start; });
	return found;
}

} // namespace

std::vector<Definition> definitions_in(const std::vector<RulePassage> &passages) {
	std::vector<Definition> definitions;
	for (std::size_t index = 0; index < passages.size(); ++index) {
		const std::string folded = fold(passages[index].text);
		for (const Found &span : definition_spans(folded)) {
			Definition definition;
			const std::string words = folded.substr(span.start, span.end - span.start);
			definition.incorporates = incorporated_name(words);
			const ParsedDefinition parsed = parse_definition(passages[index].text.substr(span.start, span.end - span.start));
			definition.understood = parsed.understood;
			definition.rule = resolve(parsed, definition.incorporates.empty() ? RuleSource::DocumentDefinition
				: RuleSource::IncorporatedDefinition);
			definition.rule.defining_passage_sha256 = passages[index].sha256;
			definition.rule.defining_passage = index;
			definition.rule.defining_offset = span.start;
			definition.rule.defining_length = span.end - span.start;
			definition.passage = index;
			definitions.push_back(std::move(definition));
		}
	}
	return definitions;
}

std::vector<RollClause> roll_clauses_in(const std::vector<RulePassage> &passages) {
	std::vector<RollClause> clauses;
	for (std::size_t index = 0; index < passages.size(); ++index) {
		const std::string folded = fold(passages[index].text);
		for (std::size_t from = 0; from < folded.size();) {
			const std::size_t end = sentence_end(folded, from);
			const std::string sentence = folded.substr(from, end - from);
			const bool english = has(sentence, {"business day", "banking day", "working day"});
			// A day that is not a business day ...
			const bool notBusiness = has(sentence, {"not a business day", "not a banking day", "not a working day", "is not a business",
				"other than a business day", "saturday", "sunday", "holiday",
				"\xD0\xBD\xD0\xB5\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87",  // …
				"\xD0\xB2\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBD",  // …
				"\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD\xD0\xB8\xD1\x87"});  // …(…)
			const bool conditional = has(sentence, {"if ", "where ", "in the event", "should ", "falls on", "fall on", "would otherwise",
				"\xD0\xB5\xD1\x81\xD0\xBB\xD0\xB8", "\xD0\xBF\xD1\x80\xD0\xB8\xD1\x85\xD0\xBE\xD0\xB4\xD0\xB8\xD1\x82\xD1\x81\xD1\x8F",  // …
				"\xD0\xBF\xD1\x80\xD0\xB8\xD1\x85\xD0\xBE\xD0\xB4\xD0\xB8\xD1\x82\xD1\x81\xD1\x8F", "\xD0\xB2\xD1\x8B\xD0\xBF\xD0\xB0\xD0\xB4\xD0\xB0"});  // …
			std::string roll;
			if (english) {
				const bool following = has(sentence, {"next succeeding business day", "next following business day", "immediately succeeding business day",
					"immediately following business day", "following business day", "next business day", "succeeding business day",
					"next banking day", "next working day", "following banking day", "following working day"});
				const bool preceding = has(sentence, {"preceding business day", "previous business day", "preceding banking day",
					"preceding working day"});
				if (following && preceding && has(sentence, {"month"})) roll = "modified_following";
				else if (following) roll = "next_business_day";
				else if (preceding) roll = "preceding_business_day";
			} else {
				const bool working = has(sentence, {"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87", "\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA"});  // …
				const bool following = has(sentence, {"\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4\xD1\x83\xD1\x8E\xD1\x89"});  // …
				const bool preceding = has(sentence, {"\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD1\x88\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2",  // …
					"\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD1\x8B\xD0\xB4\xD1\x83\xD1\x89"});  // …
				const bool moves = has(sentence, {"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD\xD0\xBE\xD1\x81",  // …
					"\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F",  // …
					"\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xBC \xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x87\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8F"});  // …
				if (working && moves && following) roll = "next_business_day";
				else if (working && moves && preceding) roll = "preceding_business_day";
			}
			// What the clause moves: every period ("the last day of any period",
			// «…»), or a payment's due day alone ("if any payment
			// is due on a day that is not a Business Day, such payment shall be
			// made ..."), which settles no notice or cure period.
			std::string scope;
			const bool general = has(sentence, {"period", "action", "notice", "deadline", "obligation", "performance", "any date",
				"date on which", "time for", "any act", "\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA",  // …
				"\xD0\xB4\xD0\xB5\xD0\xB9\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8", "\xD0\xBE\xD0\xB1\xD1\x8F\xD0\xB7\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2",  // …
				"\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB"});  // …
			const bool payment = has(sentence, {"payment", "paid", "payable", " pay ", "amount", " sum", "interest", "installment",
				"instalment", "\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB6", "\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82",  // …
				"\xD1\x83\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82", "\xD1\x81\xD1\x83\xD0\xBC\xD0\xBC"});  // …
			if (payment && !general) scope = "payments";
			if (!roll.empty() && notBusiness && conditional) clauses.push_back({roll, scope, index, from, end - from});
			from = end > from ? end : from + 1;
			while (from < folded.size() && (folded[from] == ' ' || folded[from] == '\n')) ++from;
		}
	}
	return clauses;
}

// ---------------------------------------------------------------------------
// The lawyer's words.

std::optional<BusinessDayRule> lawyer_rule(std::string_view words, bool answer) {
	const std::string text(words);
	std::size_t from = 0;
	while (from < text.size()) {
		std::size_t end = text.find_first_of(".!?;\n", from);
		const bool question = end != std::string::npos && text[end] == '?';
		// "U.S." and "N.Y." are no sentence ends.
		while (end != std::string::npos && text[end] == '.' && end + 1 < text.size() && text[end + 1] != ' ' && text[end + 1] != '\n')
			end = text.find_first_of(".!?;\n", end + 1);
		const std::string sentence = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
		from = end == std::string::npos ? text.size() : end + 1;
		if (question) continue;
		const std::string folded = " " + fold(sentence) + " ";
		const bool aboutDays = has(folded, {"business day", "banking day", "working day", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8",  // …
			"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB5\xD0\xB3", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD0\xBC",  // …
			"\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA"});  // …
		if (!answer && !aboutDays) continue;
		// A cue the lawyer rejects states no rule of theirs.
		if (has(folded, {" not the ", " not use", "n't use", " rather than ", " instead of ", " ignore the ", " whether ",
				" \xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE ", " \xD0\xBD\xD0\xB5 \xD0\xB8\xD1\x81\xD0\xBF\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xB7",  // …
				" \xD0\xBB\xD0\xB8 "}))  // … (a question)
			continue;
		// Whether the words EXCLUDE something from business days - and where.
		std::size_t excluding = std::string::npos;
		bool positiveExclusion = false;
		for (const char *cue : {" exclud", " except", " other than", " not a business day", " not business days", " not count",
				" closed", " off ", " \xD0\xBA\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB5", " \xD0\xB8\xD1\x81\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87",  // …
				" \xD0\xB7\xD0\xB0 \xD0\xB8\xD1\x81\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5\xD0\xBC",  // …
				" \xD0\xBD\xD0\xB5\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87", " \xD0\xB2\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4\xD0\xBD"}) {  // …
			for (std::size_t at = folded.find(cue); at != std::string::npos; at = folded.find(cue, at + 1)) {
				// "should not exclude", "do not count", «…»: the
				// opposite of an exclusion.
				const std::size_t back = at >= 6 ? at - 6 : 0;
				const std::string before = folded.substr(back, at - back);
				if (before.find(" not") != std::string::npos || before.find("n't") != std::string::npos
						|| before.find("\xD0\xBD\xD0\xB5") != std::string::npos)  // …
					continue;
				positiveExclusion = true;
				excluding = std::min(excluding, at);
			}
		}
		// Holidays the lawyer says ARE business days: "business days include
		// federal holidays", "should not exclude federal holidays", «…
		// …». Weekdays alone are then the rule - unless the
		// words also exclude something, which is not a rule Singularity can read.
		const bool including = has(folded, {" include", " not exclude", "n't exclude", " not be excluded", " not excluded",
			" count holidays", " holidays count", " holidays are business days", " holidays as business days", " holidays are working days",
			" \xD0\xBD\xD0\xB5 \xD0\xB8\xD1\x81\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87", " \xD0\xB2\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB0\xD1\x8F",  // …
			" \xD0\xB2\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB0\xD1\x8E\xD1\x82", " \xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F \xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87"});  // …
		BusinessDayRule rule;
		rule.source = RuleSource::MatterRule;
		if (has(folded, {"weekdays only", "only weekdays", "weekdays alone", "holidays do not count", "holidays are not excluded",
				"ignore holidays", "no holidays", "\xD1\x82\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xBA\xD0\xBE \xD0\xB1\xD1\x83\xD0\xB4\xD0\xBD",  // …
				"\xD0\xB1\xD0\xB5\xD0\xB7 \xD1\x83\xD1\x87\xD0\xB5\xD1\x82\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD"})  // …
				&& !positiveExclusion) {
			rule.holidays_known = true;
			return rule;
		}
		ParsedDefinition parsed = parse_definition(sentence);
		if (including) {
			if (positiveExclusion || has(folded, {" but ", " \xD0\xBD\xD0\xBE "})) continue;  // …
			if (parsed.authorities.empty() && parsed.named_days.empty() && !has(folded, {"holiday", "\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD"}))
				continue;
			rule.holidays_known = true;
			return rule;
		}
		// A day the words name is a closure only where they exclude it: "from
		// April 28, 2026" is the count's anchor, never a closed day.
		if (!positiveExclusion) parsed.named_days.clear();
		else parsed.named_days = parse_definition(sentence.substr(std::min(excluding, sentence.size()))).named_days;
		// "the production calendar" said alone.
		if (parsed.authorities.empty() && has(folded, {"production calendar"})) {
			NamedAuthority russian;
			russian.kind = Authority::PublicHolidays;
			russian.place = "russia";
			parsed.authorities.push_back(russian);
			parsed.understood = true;
		}
		// The request itself or a Project record states a rule only when it says
		// how business days are counted, and only with a calendar Singularity holds.
		if (!answer) {
			const bool counting = has(folded, {" under ", " per ", " according to", " using ", " use ", " as days", " are days",
				" means ", " treat", " calendar", " exclud", " other than", " except", " by the ",
				" \xD0\xBF\xD0\xBE ", " \xD0\xBA\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB5", " \xD0\xB8\xD1\x81\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87",  // …
				"\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xB5\xD0\xBC", "\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C",  // …
				"\xD0\xBA\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB4\xD0\xB0\xD1\x80"});  // …
			if (!counting) continue;
		}
		if (!parsed.authorities.empty() || !parsed.named_days.empty()) {
			BusinessDayRule stated = resolve(parsed, RuleSource::MatterRule);
			// A request's words that name a closure Singularity keeps no calendar of are
			// no rule to count by: the material's own rule stands.
			if (!answer && !stated.holidays_known) continue;
			return stated;
		}
		if (parsed.weekdays_named && aboutDays) {
			rule = resolve(parsed, RuleSource::MatterRule);
			rule.holidays_known = parsed.no_holidays && !has(folded, {"holiday", "\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xBD"});
			if (!answer && !rule.holidays_known) continue;
			return rule;
		}
	}
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// The rule that governs.

namespace {

// Which agreement a document is, whatever its status: its title (or first
// line) without its extension, its status words and what stands in brackets -
// "Supply Agreement - DRAFT v2.docx" and "Supply Agreement (executed).pdf" are
// one agreement.
std::string family_of(const RulePassage &passage) {
	std::string name = passage.title;
	if (name.empty()) {
		std::size_t from = passage.text.size() > 0 && passage.text[0] == '[' ? passage.text.find('\n') : std::string::npos;
		from = from == std::string::npos ? 0 : from + 1;
		const std::size_t end = passage.text.find('\n', from);
		name = passage.text.substr(from, std::min<std::size_t>(end == std::string::npos ? passage.text.size() - from : end - from, 120));
	}
	std::string folded = fold(name);
	for (const char *extension : {".docx", ".doc", ".pdf", ".xlsx", ".txt", ".md", ".rtf"}) {
		const std::string suffix(extension);
		if (folded.size() > suffix.size() && folded.compare(folded.size() - suffix.size(), suffix.size(), suffix) == 0)
			folded.erase(folded.size() - suffix.size());
	}
	std::string plain;
	int depth = 0;
	for (const char character : folded) {
		if (character == '(' || character == '[') { ++depth; continue; }
		if (character == ')' || character == ']') { depth = std::max(0, depth - 1); continue; }
		if (depth == 0) plain += character;
	}
	static const std::set<std::string> status{"draft", "executed", "signed", "unsigned", "redline", "clean", "final",
		"superseded", "copy", "version", "execution", "form", "rev", "revised",
		"\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82",  // …
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB9",  // …
		"\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB0\xD0\xBA\xD1\x86\xD0\xB8\xD1\x8F"};  // …
	std::string family;
	for (const std::string &word : words_of(plain)) {
		const bool numbered = word.size() >= 2 && word[0] == 'v' && std::all_of(word.begin() + 1, word.end(), [](char c) {
			return std::isdigit(static_cast<unsigned char>(c)) != 0; });
		if (status.count(word) == 0 && !numbered) family += (family.empty() ? "" : " ") + word;
	}
	return family;
}

// Whether a passage counts in business days outside a definition of them:
// "within ten (10) Business Days", «…».
bool counts_business_days(const RulePassage &passage) {
	const std::string folded = fold(passage.text);
	std::vector<Found> spans = definition_spans(folded);
	const auto defining = [&](std::size_t at) {
		return std::any_of(spans.begin(), spans.end(), [&](const Found &span) { return at >= span.start && at < span.end; });
	};
	for (const char *term : {"business day", "banking day", "working day", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD1\x85 \xD0\xB4\xD0\xBD",  // …
			"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD0\xB9 \xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C",  // …
			"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xB4\xD0\xBD",  // …
			"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD0\xB5 \xD0\xB4\xD0\xBD",  // …
			"\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA\xD0\xB8\xD1\x85 \xD0\xB4\xD0\xBD"}) {  // …
		const std::string word(term);
		for (std::size_t at = folded.find(word); at != std::string::npos; at = folded.find(word, at + word.size()))
			if (!defining(at)) return true;
	}
	return false;
}

// The law a governing-law clause names: "ru" (Russian law), "other" (any other
// law), "mixed" (clauses of one document that name both), with the span of the
// first. Only a sentence that makes a law govern THE AGREEMENT counts - "This
// Agreement is governed by ...", «…», a
// "Governing Law:" heading - never a sentence that only mentions a law
// ("Payments comply with Russian currency regulation", "Sales by the
// Subsidiary are subject to the laws of the Russian Federation", a dispute
// clause naming the Federal Rules of Civil Procedure). The law is the first
// one the sentence names after its governing verb.
struct LawClause {
	std::string law;
	std::size_t passage = 0;
	std::size_t offset = 0;
	std::size_t length = 0;
};

std::optional<LawClause> law_clause(const std::vector<RulePassage> &passages, std::size_t index) {
	const std::string folded = fold(passages[index].text);
	static const std::vector<const char *> governs{"governed by", "governed in accordance", "governed under", "construed in accordance",
		"construed under", "interpreted in accordance", "interpreted under", "shall govern", "will govern", "governs this",
		"subject to the law", "subject to the laws", "enforced in accordance",
		"\xD1\x80\xD0\xB5\xD0\xB3\xD1\x83\xD0\xBB\xD0\xB8\xD1\x80\xD1\x83\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F",  // …
		"\xD1\x80\xD0\xB5\xD0\xB3\xD1\x83\xD0\xBB\xD0\xB8\xD1\x80\xD1\x83\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F",  // …
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBB\xD0\xB5\xD0\xB6\xD0\xB8\xD1\x82 \xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8E",  // …
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8F\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F",  // …
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xBC",  // …(…)
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD1\x87\xD0\xB8\xD0\xBD\xD1\x8F\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F",  // …
		"\xD1\x80\xD1\x83\xD0\xBA\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB4\xD1\x81\xD1\x82\xD0\xB2\xD1\x83\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F",  // …
		"\xD1\x82\xD0\xBE\xD0\xBB\xD0\xBA\xD1\x83\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F"};  // …
	// What a governing sentence governs: the agreement itself.
	static const std::vector<const char *> subjects{"this ", "these ", "hereunder", "hereof", "herein", "the agreement", "the contract",
		"the lease", "the note", "the parties", "\xD0\xB4\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80",  // …
		"\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8", "\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82\xD1\x80\xD0\xB0\xD0\xBA\xD1\x82",  // …
		"\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD1\x89"};  // …
	static const std::vector<const char *> headings{"governing law", "applicable law", "choice of law",
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xBC\xD0\xBE\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE"};  // …
	static const std::vector<const char *> russian{"laws of the russian federation", "law of the russian federation", "russian law",
		"russian federation law", "laws of russia", "law of russia",
		"\xD0\xB7\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE\xD0\xBC \xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9 \xD1\x84\xD0\xB5\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD0\xB8",  // …
		"\xD0\xB7\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE \xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9 \xD1\x84\xD0\xB5\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD0\xB8",  // …
		"\xD0\xB7\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE\xD0\xBC \xD1\x80\xD1\x84",  // …
		"\xD0\xB7\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE \xD1\x80\xD1\x84",  // …
		"\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE \xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9 \xD1\x84\xD0\xB5\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD0\xB8",  // …
		"\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE\xD0\xBC \xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9 \xD1\x84\xD0\xB5\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD0\xB8",  // …
		"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xB8\xD0\xBC \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE\xD0\xBC",  // …
		"\xD1\x80\xD0\xBE\xD1\x81\xD1\x81\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE"};  // …
	static const std::vector<const char *> anyLaw{"laws of", "law of", " law", "\xD0\xB7\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2",  // …
		"\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE\xD0\xBC", "\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBE "};  // …
	const auto first_of = [](const std::string &text, const std::vector<const char *> &words, std::size_t from) {
		std::size_t best = std::string::npos;
		for (const char *word : words) {
			const std::size_t found = text.find(word, from);
			if (found != std::string::npos && found < best) best = found;
		}
		return best;
	};
	std::optional<LawClause> result;
	for (std::size_t from = 0; from < folded.size();) {
		const std::size_t end = sentence_end(folded, from);
		const std::string sentence = folded.substr(from, end - from);
		std::size_t verb = first_of(sentence, governs, 0);
		std::size_t lead = 0;
		while (lead < sentence.size() && (sentence[lead] == ' ' || std::isdigit(static_cast<unsigned char>(sentence[lead])) != 0
				|| sentence[lead] == '.' || sentence[lead] == '[' || sentence[lead] == ']'))
			++lead;
		std::size_t headingEnd = 0;
		for (const char *word : headings)
			if (sentence.compare(lead, std::string(word).size(), word) == 0) headingEnd = lead + std::string(word).size();
		const bool heading = headingEnd != 0;
		// Where the agreement must stand: before a passive verb ("This Agreement
		// is governed by"); anywhere for an active one ("the laws of X shall
		// govern this Agreement", «…
		// …»); at the very start for a loose one ("This Agreement is
		// subject to the laws of ..." - not "Payments under this Agreement are
		// subject to ...").
		bool agreement = false;
		std::size_t lawFrom = verb;
		if (verb != std::string::npos) {
			const auto at_verb = [&](std::initializer_list<const char *> words) {
				return std::any_of(words.begin(), words.end(), [&](const char *word) { return sentence.compare(verb, std::string(word).size(), word) == 0; });
			};
			const std::size_t subject = first_of(sentence, subjects, lead);
			if (at_verb({"shall govern", "will govern", "governs this",
					"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xBC",  // …
					"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8F\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F"})) {  // …
				agreement = subject != std::string::npos;
				lawFrom = lead;
			} else if (at_verb({"subject to the law", "\xD0\xBF\xD0\xBE\xD0\xB4\xD1\x87\xD0\xB8\xD0\xBD\xD1\x8F\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F"})) {  // …
				agreement = subject == lead;
			} else {
				agreement = subject != std::string::npos && subject < verb;
			}
		}
		if (heading && !agreement) lawFrom = verb = headingEnd;
		if (agreement || heading) {
			const std::size_t lawAt = first_of(sentence, anyLaw, lawFrom);
			const std::size_t russianAt = first_of(sentence, russian, lawFrom);
			if (lawAt != std::string::npos || russianAt != std::string::npos) {
				// The first law named after the verb: Russian when its name stands
				// at (or right before) the first law word.
				const bool isRussian = russianAt != std::string::npos && (lawAt == std::string::npos || russianAt <= lawAt + 16);
				const std::string law = isRussian ? "ru" : "other";
				if (!result) result = LawClause{law, index, from, end - from};
				else if (result->law != law) result->law = "mixed";
			}
		}
		from = end > from ? end : from + 1;
		while (from < folded.size() && (folded[from] == ' ' || folded[from] == '\n')) ++from;
	}
	return result;
}

// The document an incorporated definition names, among the passages: one whose
// title or first line holds its name.
std::optional<BusinessDayRule> incorporated_rule(const Definition &definition, const std::vector<RulePassage> &passages,
		const std::vector<Definition> &all) {
	if (definition.incorporates == "(unnamed)") return std::nullopt;
	for (const Definition &other : all) {
		if (!other.incorporates.empty()) continue;
		const RulePassage &passage = passages[other.passage];
		const std::size_t line = passage.text.find('\n', passage.text.size() > 0 && passage.text[0] == '[' ? passage.text.find('\n') + 1 : 0);
		const std::string heading = fold(passage.title + "\n" + passage.text.substr(0, std::min<std::size_t>(line, 240)));
		if (heading.find(definition.incorporates) == std::string::npos) continue;
		BusinessDayRule rule = other.rule;
		rule.source = RuleSource::IncorporatedDefinition;
		return rule;
	}
	return std::nullopt;
}

bool same_counting(const BusinessDayRule &a, const BusinessDayRule &b) {
	BusinessDayRule left = a;
	BusinessDayRule right = b;
	left.roll = right.roll = "";
	return same_days(left, right);
}

} // namespace

GoverningRules governing_rule(const GoverningInput &input) {
	GoverningRules rules;
	const std::vector<RulePassage> &passages = input.passages;
	// A Project record is no document: its words are the matter rule's.
	std::vector<RulePassage> documentsOnly;
	std::vector<std::size_t> original;
	for (std::size_t index = 0; index < passages.size(); ++index) {
		if (passages[index].matter_record) continue;
		documentsOnly.push_back(passages[index]);
		original.push_back(index);
	}
	std::vector<Definition> all = definitions_in(documentsOnly);
	for (Definition &definition : all) {
		definition.passage = original[definition.passage];
		definition.rule.defining_passage = definition.passage;
	}
	const auto document_of = [&](std::size_t index) {
		return passages[index].document.empty() ? "#" + std::to_string(index) : passages[index].document;
	};
	// Each document's first passage: its heading, standing and agreement.
	std::map<std::string, std::size_t> first;
	for (const std::size_t index : original) first.emplace(document_of(index), index);
	const auto standing = [&](const std::string &document) {
		const RulePassage &passage = passages[first.at(document)];
		return standing_of(passage.title, passage.text);
	};
	// The operative documents: the one the rule's own sentence stands in, when
	// the caller knows it; else every document that counts in business days -
	// narrowed to those the operation's own facts are written in, when some are;
	// else, when none counts in them, every document that defines them; else the
	// material's only document. Another document's definition - or governing
	// law - is never a source of its own.
	std::vector<std::string> documents;
	const auto add = [&](const std::string &document) {
		if (std::find(documents.begin(), documents.end(), document) == documents.end()) documents.push_back(document);
	};
	if (input.operative && *input.operative < passages.size() && !passages[*input.operative].matter_record) {
		add(document_of(*input.operative));
	} else {
		for (const std::size_t index : original)
			if (counts_business_days(passages[index])) add(document_of(index));
		if (documents.size() > 1 && !input.operand_writings.empty()) {
			std::vector<std::string> writing;
			for (const std::string &document : documents)
				for (const std::size_t index : original) {
					if (document_of(index) != document) continue;
					const bool holds = std::any_of(input.operand_writings.begin(), input.operand_writings.end(),
						[&](const std::string &written) { return written.size() >= 4 && passages[index].text.find(written) != std::string::npos; });
					if (holds && std::find(writing.begin(), writing.end(), document) == writing.end()) writing.push_back(document);
				}
			if (!writing.empty()) documents = writing;
		}
		if (documents.empty())
			for (const Definition &definition : all) add(document_of(definition.passage));
		if (documents.empty() && first.size() == 1) add(first.begin()->first);
	}
	// A draft gives way to its agreement's executed version; beside documents
	// of any other standing, a draft is no operative document at all.
	for (std::string &document : documents) {
		if (standing(document) != Standing::Draft) continue;
		const std::string family = family_of(passages[first.at(document)]);
		if (family.empty()) continue;
		for (const auto &[other, index] : first)
			if (other != document && standing(other) == Standing::Executed && family_of(passages[index]) == family) {
				document = other;
				break;
			}
	}
	std::sort(documents.begin(), documents.end());
	documents.erase(std::unique(documents.begin(), documents.end()), documents.end());
	if (std::any_of(documents.begin(), documents.end(), [&](const std::string &document) { return standing(document) != Standing::Draft; }))
		documents.erase(std::remove_if(documents.begin(), documents.end(),
			[&](const std::string &document) { return standing(document) == Standing::Draft; }), documents.end());

	// Each operative document's governing law and roll clause - its own only.
	std::map<std::string, LawClause> laws;
	std::map<std::string, RollClause> rolls;
	const std::vector<RollClause> clauses = roll_clauses_in(documentsOnly);
	for (const std::string &document : documents) {
		for (const std::size_t index : original) {
			if (document_of(index) != document) continue;
			if (std::optional<LawClause> law = law_clause(passages, index)) {
				const auto known = laws.find(document);
				if (known == laws.end()) laws.emplace(document, *law);
				else if (known->second.law != law->law) known->second.law = "mixed";
			}
		}
		for (const RollClause &clause : clauses) {
			const std::size_t index = original[clause.passage];
			if (document_of(index) != document) continue;
			RollClause found = clause;
			found.passage = index;
			// A clause for every period is preferred over one for payments alone.
			const auto known = rolls.find(document);
			if (known == rolls.end()) rolls.emplace(document, found);
			else if (known->second.scope == "payments" && found.scope.empty()) known->second = found;
		}
	}
	// A period's end rolls under the operative document's own clause, else under
	// its governing law where the law sets it (Russian Civil Code art. 193).
	const auto roll_of = [&](const std::string &document, BusinessDayRule &rule) {
		const auto clause = rolls.find(document);
		const auto law = laws.find(document);
		const bool russianLaw = law != laws.end() && law->second.law == "ru";
		// A clause for payments alone that moves them as art. 193 moves every
		// period adds nothing: the law's roll reaches every period.
		const bool lawCovers = russianLaw && clause != rolls.end() && clause->second.scope == "payments"
			&& clause->second.roll == "next_business_day";
		if (clause != rolls.end() && !lawCovers) {
			rule.roll = clause->second.roll;
			rule.roll_source = "contract";
			rule.roll_scope = clause->second.scope;
			rule.roll_passage_sha256 = passages[clause->second.passage].sha256;
			rule.roll_offset = clause->second.offset;
			rule.roll_length = clause->second.length;
		} else if (russianLaw) {
			rule.roll = "next_business_day";
			rule.roll_source = "law:ru_civil_code_193";
			rule.roll_passage_sha256 = passages[law->second.passage].sha256;
			rule.roll_offset = law->second.offset;
			rule.roll_length = law->second.length;
		}
	};
	// The roll the operative documents agree on; none known when they differ.
	const auto common_roll = [&](BusinessDayRule &rule) {
		if (!rule.roll.empty() && rule.roll_source == "matter_rule") return;
		std::optional<BusinessDayRule> agreed;
		bool differ = false;
		for (const std::string &document : documents) {
			BusinessDayRule own;
			roll_of(document, own);
			if (!agreed) agreed = own;
			else if (agreed->roll != own.roll || agreed->roll_scope != own.roll_scope) differ = true;
		}
		if (!agreed || differ) return;
		rule.roll = agreed->roll;
		rule.roll_source = agreed->roll_source;
		rule.roll_scope = agreed->roll_scope;
		rule.roll_passage_sha256 = agreed->roll_passage_sha256;
		rule.roll_offset = agreed->roll_offset;
		rule.roll_length = agreed->roll_length;
	};
	// What the material leaves open, the lawyer settles.
	const auto settled = [&]() {
		rules.business_day = input.matter_rule;
		rules.business_day->source = RuleSource::MatterRule;
		common_roll(*rules.business_day);
		return rules;
	};
	// Each operative document's own rule: its definition, or the one it
	// incorporates; none when it defines nothing.
	std::vector<BusinessDayRule> resolved;
	bool undefined = false;
	bool missing = false;
	for (const std::string &document : documents) {
		std::size_t own = 0;
		for (const Definition &definition : all) {
			if (document_of(definition.passage) != document) continue;
			++own;
			BusinessDayRule rule = definition.rule;
			if (!definition.incorporates.empty()) {
				std::optional<BusinessDayRule> found = incorporated_rule(definition, passages, all);
				if (!found) {
					missing = true;
					continue;
				}
				rule = *found;
				rule.defining_passage_sha256 = definition.rule.defining_passage_sha256;
				rule.defining_passage = definition.rule.defining_passage;
				rule.defining_offset = definition.rule.defining_offset;
				rule.defining_length = definition.rule.defining_length;
			}
			roll_of(document, rule);
			resolved.push_back(rule);
		}
		if (own == 0) undefined = true;
	}
	if (!resolved.empty() || missing) {
		if (resolved.empty()) {
			if (input.matter_rule) return settled();
			rules.unknown_reason = "definition_not_found";
			return rules;
		}
		// Operative documents that count differently - two definitions that
		// disagree, or one that defines Business Day beside one that does not:
		// which governs is the lawyer's to say.
		bool agree = !undefined;
		for (const BusinessDayRule &rule : resolved)
			if (!same_counting(rule, resolved.front())) agree = false;
		if (!agree) {
			rules.conflicting = resolved;
			if (input.matter_rule) return settled();
			rules.unknown_reason = "rule_conflict";
			return rules;
		}
		BusinessDayRule rule = resolved.front();
		for (const BusinessDayRule &other : resolved)
			if (other.roll != rule.roll || other.roll_scope != rule.roll_scope) rule.roll = rule.roll_source = rule.roll_scope = "";
		// A definition whose closures Singularity knows no calendar of: the lawyer's
		// rule reads it.
		if (!rule.holidays_known && input.matter_rule) return settled();
		rules.business_day = rule;
		return rules;
	}
	if (input.matter_rule) return settled();
	// The operative documents' own governing law, when it makes a calendar the
	// rule: all of them Russian, or a conflict when one names another law.
	bool russian = false;
	bool other = false;
	bool silent = false;
	std::string russianDocument;
	for (const std::string &document : documents) {
		const auto law = laws.find(document);
		if (law == laws.end()) silent = true;
		else if (law->second.law == "ru" || law->second.law == "mixed") {
			// A document whose clauses name Russian law beside another: a conflict.
			russian = true;
			if (russianDocument.empty()) russianDocument = document;
			if (law->second.law == "mixed") other = true;
		} else other = true;
	}
	BusinessDayRule fallback;
	fallback.source = RuleSource::WeekdayFallback;
	fallback.holidays_known = false;
	common_roll(fallback);
	if (russian) {
		BusinessDayRule law;
		law.source = RuleSource::JurisdictionCalendar;
		law.calendars = {"ru_production"};
		law.holidays_known = true;
		const LawClause &clause = laws.at(russianDocument);
		law.defining_passage_sha256 = passages[clause.passage].sha256;
		law.defining_passage = clause.passage;
		law.defining_offset = clause.offset;
		law.defining_length = clause.length;
		roll_of(russianDocument, law);
		if (other) {
			rules.conflicting = {law, fallback};
			rules.unknown_reason = "rule_conflict";
			return rules;
		}
		if (!silent) {
			rules.business_day = law;
			return rules;
		}
	}
	rules.business_day = fallback;
	return rules;
}

} // namespace singularity::context::business_calendar
