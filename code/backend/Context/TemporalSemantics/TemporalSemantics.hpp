#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Dates as a lawyer writes them, read on the lawyer's computer.
//
// Papinian Guard keeps every date of a Project from a provider's model, so the
// model cannot tell which of two documents came first. What it needs - "the
// draft precedes the signed agreement" - is worked out here, from the dates,
// and only the relation is ever sent (Context/TemporalContext). Nothing here
// leaves the device; it reads dates, and it says only what they prove.
//
// A date is an interval of days: "3 March 2026" is one day, "March 2026" its
// month, "Q3 2026" its quarter, "2026" its year. Two dates are in order only
// when their intervals do not overlap - "March 2026" is neither before nor after
// "3 March 2026" - so precision never becomes a guess.
namespace singularity::context::temporal {

enum class Precision { Day, Month, Quarter, Year };

struct TemporalValue {
	// Days since 1970-01-01, inclusive.
	long long first_day = 0;
	long long last_day = 0;
	Precision precision = Precision::Day;
};

enum class TemporalOrder { Before, After, Same, Unknown };

// How a text writes a day with slashes: "05/06/2026" is May 6 in New York and
// 5 June in London or Moscow. Unknown until the text itself says.
enum class SlashOrder { Unknown, MonthFirst, DayFirst };

// One date as written: English ("March 3, 2026", "Mar. 3, 2026", "3 March 2026",
// "03/03/2026", "2026-03-03", "February 2026", "Q3 2026", "2026") or Russian
// ("03.03.2026", "3 марта 2026 г.", "«03» марта 2026 г.", "март 2026 г.",
// "III квартал 2026 г."). Nothing for text that is not wholly a date.
// A slashed day both orders read differently is no date here unless the text
// or `order` settles it: "05/06/2026" alone is nothing, "06/15/2026" is June 15.
[[nodiscard]] std::optional<TemporalValue> parse(const std::string &text);
[[nodiscard]] std::optional<TemporalValue> parse(const std::string &text, SlashOrder order);

// Where `a` stands against `b`: Before or After only when they cannot overlap,
// Same only for the same day.
[[nodiscard]] TemporalOrder compare(const TemporalValue &a, const TemporalValue &b);

// What a text's own slashes say of their order: a slashed day that one order
// alone can read ("06/15/2026", "15/06/2026"), or a format the text states
// ("MM/DD/YYYY", "dd/mm/yyyy"). How it writes other days is no evidence - a US
// note dated "the 1st day of July, 2026" writes its schedule month first.
// Evidence both ways, or none, is Unknown. Pass one document's text: two
// documents of one prompt may write their slashes each its own way.
[[nodiscard]] SlashOrder slash_order_of(const std::string &text);

// A date found in running text: where it stands and what it is.
struct FoundDate {
	std::size_t offset = 0;
	std::size_t length = 0;
	TemporalValue value;
	// A slashed day both orders read, differently, in a text that does not say
	// which it uses: `value` is only its month-first reading, and no order,
	// deadline or duration may rest on it.
	bool ambiguous = false;
};

// Every date written in `text`, left to right, the most precise reading of each
// place (a day before its month, a month before its year). Slashed days are
// read in the order the text's own dates show (slash_order_of), or the one
// given; where neither settles it, they are found and marked ambiguous.
[[nodiscard]] std::vector<FoundDate> find(const std::string &text);
[[nodiscard]] std::vector<FoundDate> find(const std::string &text, SlashOrder order);

// A day of the year with no year: "July 1", "1 July", "July 1st", "1st of
// July", «1 июля», «"1" июля» - the day a yearly event falls on ("each
// twelve-month period beginning on July 1"). No interval of days: which July 1
// it is depends on the year, so it is before or after nothing by itself. "The
// first day of each month" names no day of the year.
struct DayOfYear {
	unsigned month = 0;
	unsigned day = 0;
};

// One day of the year as written, and nothing for text that is not wholly one
// (a date with its year is not: that is parse()'s).
[[nodiscard]] std::optional<DayOfYear> parse_day_of_year(const std::string &text);
// The same reading, never remembered (parse_day_of_year remembers it).
[[nodiscard]] std::optional<DayOfYear> parse_day_of_year_uncached(const std::string &text);

struct FoundDayOfYear {
	std::size_t offset = 0;
	std::size_t length = 0;
	DayOfYear value;
};

// Every day of the year `text` writes without its year, left to right: never a
// part of a date written with its year ("July 1, 2026"), and an English month
// only as a name is written, with its capital ("may 5" is a verb and a count).
[[nodiscard]] std::vector<FoundDayOfYear> find_days_of_year(const std::string &text);

} // namespace singularity::context::temporal
