#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Business days, counted on the lawyer's computer - under the rule that
// governs them.
//
// A contract that gives "ten (10) Business Days" usually says what a Business
// Day is. That definition governs, even where it differs from a generic
// calendar. Singularity reads any such definition - English or Russian - into a
// STRUCTURED rule, never into a guessed calendar:
//
//   - the weekdays it excludes ("Saturday, Sunday", «суббота, воскресенье»);
//   - the closure authorities it names: banks, a central bank, the law's
//     public holidays, government offices, an exchange, a payment system - each
//     with the place the words give ("commercial banks in New York City or
//     London", «банки в г. Москве»);
//   - the days it names itself ("December 24", «31 декабря», "Good Friday");
//   - a definition it incorporates by reference instead ("has the meaning given
//     in the Credit Agreement").
//
// Each authority is then resolved against a REGISTRY of calendars held as data
// (CalendarRegistry.inc): each calendar with its source, the years it covers,
// the days it closes for certain, the days its words only MAY close (a day New
// York banks are "authorized" to close), and whether unscheduled closures are
// possible. An authority the registry does not know resolves to nothing: its
// weekdays are open, and every count under it is a bound, never exact.
//
// The rule that governs is found in this order (owner correction, 26 Sep 2026):
//
//   1. the operative document's own definition - the document that counts in
//      business days (an executed agreement's over its draft's; a definition
//      found only in a superseded draft does not govern; operative documents
//      that count them differently are a conflict). Another document's
//      definition is no source of its own - nor is its governing law;
//   2. a definition the operative document incorporates, when that document
//      is in the Project's material;
//   3. the matter rule: a rule the lawyer gave in this chat or a Project record
//      states. It settles whatever the material leaves open - a conflict, an
//      incorporated definition that is missing, a definition whose authority
//      Singularity knows no calendar of - and outranks everything automatic below;
//   4. a jurisdiction's calendar, only where the operative document's own
//      governing law makes it the rule (Russian law: the production calendar,
//      and the roll of Civil Code art. 193) - never because of the language a
//      document is written in, and never because another document names a law;
//   5. otherwise weekdays, with the holidays NOT known - and then no exact
//      count or deadline is given, only what holds whatever the holidays are.
//
// Every result carries its rule, the span that defines it and its assumptions
// (closed words: "scheduled closures only; unscheduled closures not known").
// It is exact only when the rule resolves to known calendars for every year it
// touches and no day it touches is one the words only may close; otherwise it
// is a bound, or UNKNOWN.
//
// A day is a count of days since 1970-01-01 (TemporalValue::first_day).
// Nothing here reads files, keeps state beyond a per-thread cache of computed
// years, or talks to anyone.
namespace singularity::context::business_calendar {

// ---------------------------------------------------------------------------
// The calendar registry (data: CalendarRegistry.inc).

// Who closes: what a definition's words name, and what a calendar is of.
enum class Authority { Banks, CentralBank, PublicHolidays, GovernmentOffices, Exchange, PaymentSystem, Unknown };

enum class DayRuleKind {
	Fixed,             // month/day
	NthWeekday,        // the nth `weekday` of month (nth 0: the last)
	WeekdayOnOrAfter,  // the first `weekday` on or after month/day
	EasterOffset,      // Western Easter Sunday + offset days
};

// How a holiday falling on a weekend is observed.
enum class Observance {
	None,                          // the day itself only
	SaturdayToFridaySundayToMonday,// US federal (5 U.S.C. 6103; E.O. 11582)
	SundayToMonday,                // a Saturday one is not moved (Federal Reserve; NYSE's New Year)
	SubstituteNextWeekday,         // the next weekday not already a holiday (England and Wales)
};

struct DayRule {
	const char *name;
	DayRuleKind kind;
	unsigned month;
	unsigned day;
	int weekday;          // Monday 0 ... Sunday 6
	int nth;
	int easter_offset;
	Observance observance;
	int from_year;        // 0: always
	int to_year;          // 0: always
};

struct DatedDay {
	int year;
	unsigned month;
	unsigned day;
};

struct CalendarEntry {
	const char *id;                  // closed word: "us_federal_reserve"
	const char *name;                // closed words: "Federal Reserve holidays"
	const char *source;              // the law or publication it follows
	// The authorities and places it is the calendar of: {Banks, "new york"}.
	std::vector<std::pair<Authority, std::string>> serves;
	int first_year;                  // the years it is known for
	int last_year;
	bool unscheduled_closures;       // whether a closure can be declared at short notice
	std::vector<std::string> includes;       // other calendars whose certain days it closes too
	std::vector<DayRule> closed;             // certain closures, by rule
	std::vector<DatedDay> closed_dates;      // certain closures, dated (decrees, proclamations, orders)
	std::vector<DatedDay> not_closed_dates;  // a rule's day a proclamation moved away
	std::vector<DatedDay> working_dates;     // weekend days made working days (Russian transfers)
	std::vector<DayRule> possible;           // days its words only MAY close (discretionary)
	std::vector<DatedDay> possible_dates;
};

[[nodiscard]] const std::vector<CalendarEntry> &registry();
[[nodiscard]] const CalendarEntry *find_calendar(std::string_view id);

// Whether a registry calendar knows the days of `year`.
[[nodiscard]] bool covers(std::string_view calendar, int year);
// Whether `day` is a certain closure of `calendar` (as observed), a weekend day
// it makes a working day, or a day its words only may close. False outside the
// years it covers.
[[nodiscard]] bool closed_on(std::string_view calendar, long long day);
[[nodiscard]] bool working_on(std::string_view calendar, long long day);
[[nodiscard]] bool possibly_closed_on(std::string_view calendar, long long day);
// Western Easter Sunday of `year`, as a day.
[[nodiscard]] long long easter_sunday(long long year);

// Monday 0 ... Sunday 6.
[[nodiscard]] int weekday(long long day);
[[nodiscard]] long long days_from_civil(long long year, unsigned month, unsigned day);
struct CivilDate {
	long long year = 1970;
	unsigned month = 1;
	unsigned day = 1;
};
[[nodiscard]] CivilDate civil_from_days(long long day);

// ---------------------------------------------------------------------------
// A rule.

enum class RuleSource { DocumentDefinition, IncorporatedDefinition, MatterRule, JurisdictionCalendar, WeekdayFallback };
enum class DayClass { BusinessDay, Weekend, Holiday };

// A closure authority a definition names, and what the registry makes of it.
struct NamedAuthority {
	Authority kind = Authority::Unknown;
	std::string place;               // canonical place ("new york", "london", "moscow", "russia"), or ""
	bool authorized = false;         // "authorized ... to close": the days the words only may close count as uncertain
	// A limb that applies only in some case ("(in relation to a payment in euro)
	// any TARGET Day"): whether it applies is not known here, so it closes no day
	// for certain and the rule's holidays are not known.
	bool conditional = false;
	std::string calendar;            // the registry calendar it resolved to; "" when none
};

// What a business day is under the rule that governs, and where that rule
// comes from. `holidays_known` false means the rule names closures Singularity
// knows no calendar of (or words it cannot read as days): its weekdays are
// then open, and every count under it is a bound, never exact.
struct BusinessDayRule {
	RuleSource source = RuleSource::WeekdayFallback;
	std::set<int> weekend{5, 6};
	// The rule's own words name the weekdays it excludes ("other than a Saturday
	// or Sunday", «кроме субботы»): a weekend day a calendar makes a working day
	// (a Russian transfer) is then no certain business day. Otherwise the weekend
	// is the calendars' own, and a working day they ALL make is a business day.
	bool weekend_explicit = false;
	// Registry calendars whose certain closures are not business days, and
	// those whose discretionary days may not be.
	std::vector<std::string> calendars;
	std::vector<std::string> possible_calendars;
	// Days the rule names itself, every year (month/day rules, Easter days).
	std::vector<DayRule> named_days;
	// The authorities its words name (resolved or not).
	std::vector<NamedAuthority> authorities;
	bool holidays_known = false;
	// Where the defining words stand: the passage (its SHA-256 and index among
	// the passages read) and the bytes in it. Empty when no text defines it.
	std::string defining_passage_sha256;
	std::size_t defining_passage = static_cast<std::size_t>(-1);
	std::size_t defining_offset = 0;
	std::size_t defining_length = 0;
	// What happens to a period ending on a day that is not a business day:
	// "" - not known (no clause found, no law that settles it);
	// "none", "next_business_day", "preceding_business_day",
	// "modified_following". `roll_source`: "contract" (the operative document's
	// clause), "law:ru_civil_code_193", "matter_rule", or "".
	// `roll_scope`: "" - the roll reaches every period; "payments" - the clause
	// moves only a payment's due day ("if any payment is due on a day that is
	// not a Business Day"): any other period's end is then not settled by it.
	std::string roll;
	std::string roll_source;
	std::string roll_scope;
	std::string roll_passage_sha256;
	std::size_t roll_offset = 0;
	std::size_t roll_length = 0;
};

// Two rules that count the same days: the same weekend, calendars, named days,
// knowledge and roll.
[[nodiscard]] bool same_days(const BusinessDayRule &a, const BusinessDayRule &b);

// What a day is under a rule: certainly a business day, certainly not one
// (weekend, holiday), a day the rule's words only may close (Possible), or a
// weekday whose status is not known here (Open: a closure authority without a
// calendar, a year the calendar does not cover). A weekend day that EVERY
// calendar of the rule makes a working day (a Russian transfer) is a business
// day - unless the rule's own words exclude that weekday by name, when it is
// Possible.
enum class DayStatus { Business, Weekend, Holiday, Possible, Open };
[[nodiscard]] DayStatus status(const BusinessDayRule &rule, long long day);

// The class of a day as far as it is certain: an uncertain day reads as a
// business day (it is one unless a closure Singularity does not know takes it).
[[nodiscard]] DayClass classify(const BusinessDayRule &rule, long long day);

// Whether every day of [from, to] is certain under the rule.
[[nodiscard]] bool exact(const BusinessDayRule &rule, long long from, long long to);

// What holds whatever the uncertain days turn out to be. Under an exact rule
// `exact` is the answer. Otherwise a count is AT MOST `at_most` (the uncertain
// days counted as business days) and AT LEAST `at_least` (they are not); a
// deadline is NOT BEFORE the earliest day and NOT AFTER the latest it can be -
// the latest only where it exists (a closure authority without a calendar
// leaves every weekday open: no latest day). `assumptions` are closed words the
// result rests on, whether exact or not.
struct CountBound {
	std::optional<long long> exact;
	long long at_most = 0;          // signed like the count: negative when `to` is before `from`
	long long at_least = 0;
	std::vector<std::string> assumptions;
};
struct DayBound {
	std::optional<long long> exact;
	std::optional<long long> not_before;
	std::optional<long long> not_after;
	std::vector<std::string> assumptions;
};
[[nodiscard]] CountBound count_bound(const BusinessDayRule &rule, long long from, long long to);
// The day `n` business days after `from` (before it when negative), counting
// from the day after.
[[nodiscard]] DayBound deadline_bound(const BusinessDayRule &rule, long long from, long long n);
// `day` itself when it is a business day, else the first after it.
[[nodiscard]] DayBound next_business_day_bound(const BusinessDayRule &rule, long long day);
// The last day of a CALENDAR-day period that arithmetic puts on `day`, under
// the rule's roll. Exact when `day` is certainly a business day, when the roll
// is "none", or when the roll is known and the day it rolls to is certain.
// When `day` is not certainly a business day - a weekend or holiday, a day the
// words only may close, or a weekday whose status is open (holidays not known)
// - and the roll is not known or its target is not certain: a bound - on `day`,
// or not after the day a roll would give. `rule` null: the weekday fallback.
// `counted_back`: a period counted back from its anchor ("ten days before the
// Closing"): a day that is not a business day then moves, if at all, to the
// business day before it - unless the roll is known to be a preceding one, a
// bound between that day and the day itself.
// `payment`: the period is a payment's (a roll clause scoped to payments
// reaches it); otherwise such a clause settles nothing.
[[nodiscard]] DayBound calendar_period_end(const BusinessDayRule *rule, long long day, bool counted_back = false,
	bool payment = false);

// Exact results only (nullopt otherwise).
[[nodiscard]] std::optional<long long> count(const BusinessDayRule &rule, long long from, long long to);
[[nodiscard]] std::optional<long long> deadline(const BusinessDayRule &rule, long long from, long long n);
[[nodiscard]] std::optional<long long> next_business_day(const BusinessDayRule &rule, long long day);
[[nodiscard]] std::optional<long long> rolled(const BusinessDayRule &rule, long long day);

// The rule as a closed word for records: "document_definition",
// "incorporated_definition", "matter_rule", "jurisdiction:<calendar id>"
// ("jurisdiction:ru_production"), "weekday_fallback".
[[nodiscard]] std::string rule_name(const BusinessDayRule &rule);
// The rule as a relation's words say it - its source and the calendars it
// resolved to, never its text: "as the agreement defines Business Day",
// "Russian production calendar, governing law", "weekdays counted; holidays
// not known".
[[nodiscard]] std::string describe(const BusinessDayRule &rule);
// describe() and the assumptions, as one parenthesis's words.
[[nodiscard]] std::string explain(const BusinessDayRule &rule, const std::vector<std::string> &assumptions);
// The rule as the calendars block of a protected request lists it beside its
// handle (DESIGN4 §7.3): where it comes from, the days it closes, whether its
// holidays are known, how a period ending on a closed day moves, and the
// documents it governs - closed words and document handles only, never the
// defining text, a year or a date:
//   "business days as DOC_2 defines them: Saturdays, Sundays and New York bank
//    holidays closed; holidays known; governs DOC_2"
//   "Russian production calendar (governing law, no definition): Saturdays,
//    Sundays and its holidays closed; holidays known for some years only; a
//    period ending on a closed day ends on the next business day (Russian
//    Civil Code rule); governs DOC_4"
// `defined_in`: the handle of the document whose words define it ("" when
// none); `governs`: the documents it governs. `this_year`: the year the request
// is made in - holidays are "known" when every calendar of the rule covers it
// and the next, "known for some years only" when not.
[[nodiscard]] std::string calendar_line(const BusinessDayRule &rule, const std::string &defined_in,
	const std::vector<std::string> &governs, int this_year);
// A rule a lawyer or a model names in a word: a registry id ("us_federal",
// "us_federal_reserve", "gb_england_wales", "ru_production", "target2" ...) or
// "federal", "nyc_banks", "new_york_banks", "russian", "weekdays",
// "weekdays_only". nullopt for anything else.
[[nodiscard]] std::optional<BusinessDayRule> named_rule(std::string_view name);

// ---------------------------------------------------------------------------
// Finding the governing rule in the material (raw, local text).

enum class Standing { Unknown, Executed, Draft };

struct RulePassage {
	std::string text;               // raw local text
	std::string sha256;             // its digest, for provenance
	std::string document;           // which document it is of ("DOC_2"); passages of one document share it
	std::string title;              // its title or file name, when known ("Lease (executed).docx")
	// A Project record the lawyer keeps (PROJECT_CONTEXT), not a document: a rule
	// it states is the matter rule, and it never makes a document operative.
	bool matter_record = false;
};

// Executed ("executed", "signed", «подписан») or a draft ("draft", "redline",
// "superseded", «проект»), from a title and the text's own first lines - whole
// words only: "Overdraft Facility Agreement", "Assignment Agreement" and
// «Договор на выполнение проектных работ» are neither.
[[nodiscard]] Standing standing_of(std::string_view title, std::string_view text);

// The structured reading of a definition's words.
struct ParsedDefinition {
	std::set<int> weekend{5, 6};
	bool weekdays_named = false;     // the words name the weekdays (Saturday, Monday to Friday, weekend ...)
	bool weekdays_explicit = false;  // ... by name ("Saturday", «суббота», "Monday to Friday"), not as "weekend" / «выходные»
	std::vector<NamedAuthority> authorities;
	std::vector<DayRule> named_days;
	bool no_holidays = false;        // the words name only weekdays - no holiday of any kind
	bool understood = true;          // false: defined in words Singularity cannot read as a set of days
};
[[nodiscard]] ParsedDefinition parse_definition(std::string_view words);
// A parsed definition resolved against the registry.
[[nodiscard]] BusinessDayRule resolve(const ParsedDefinition &parsed, RuleSource source);

// A definition of Business Day (Banking Day, Working Day, «рабочий день»,
// «банковский день») a passage writes, as a rule; `incorporates` names the
// document it takes the meaning from instead ("the credit agreement"), folded.
struct Definition {
	BusinessDayRule rule;
	std::size_t passage = 0;
	std::string incorporates;
	bool understood = true;
};
[[nodiscard]] std::vector<Definition> definitions_in(const std::vector<RulePassage> &passages);

// A clause of a passage that rolls a period's end ("if the last day is not a
// Business Day, the period ends on the next succeeding Business Day",
// «переносится на ближайший следующий рабочий день»), as a roll word with its
// span; nullopt when none.
struct RollClause {
	std::string roll;
	std::string scope;              // "" (every period) or "payments"
	std::size_t passage = 0;
	std::size_t offset = 0;
	std::size_t length = 0;
};
[[nodiscard]] std::vector<RollClause> roll_clauses_in(const std::vector<RulePassage> &passages);

// A business-day rule a lawyer states in their own words. `answer` true: the
// words answer Singularity's own question about the rule, so a plain statement of
// one ("federal holidays", "weekdays only", «по производственному календарю»)
// is the rule. `answer` false (the request itself, a Project record): only a
// sentence that is about business days AND says how they are counted states one
// ("count business days as days New York banks are open"), and only with a
// calendar Singularity holds: "Count ten business days from April 28, 2026" states
// none - its date is the count's anchor, never a closed day. A day the words
// name is a closure only where they exclude it ("excluding December 24"). A
// question ("Is July 3 a federal holiday?") or a negated cue states none;
// holidays the lawyer says are business days ("business days include federal
// holidays", "should not exclude federal holidays") make weekdays the rule.
// nullopt when the words state none.
[[nodiscard]] std::optional<BusinessDayRule> lawyer_rule(std::string_view words, bool answer = true);

struct GoverningInput {
	std::vector<RulePassage> passages;
	// The passage the rule being applied stands in (its "within ten Business
	// Days" sentence), when one does: its document is the operative one.
	std::optional<std::size_t> operative;
	// A rule the lawyer gave in this chat, or a Project record states.
	std::optional<BusinessDayRule> matter_rule;
	// How the operation's own facts are written (raw, local): with no operative
	// passage given, the documents that count in business days AND write one of
	// these are the operative ones, when some are.
	std::vector<std::string> operand_writings;
};

struct GoverningRules {
	// The rule, when one governs - the weekday fallback (holidays not known)
	// when nothing does. Empty only when the rule cannot be known here.
	std::optional<BusinessDayRule> business_day;
	// Rules of equal standing that disagree.
	std::vector<BusinessDayRule> conflicting;
	// Why business_day is empty: "rule_conflict", "definition_not_found".
	std::string unknown_reason;
};
[[nodiscard]] GoverningRules governing_rule(const GoverningInput &input);

} // namespace singularity::context::business_calendar
