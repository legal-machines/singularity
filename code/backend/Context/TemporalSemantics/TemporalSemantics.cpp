#include "Context/TemporalSemantics/TemporalSemantics.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::context::temporal {
namespace {

// Days since 1970-01-01 of a civil date (H. Hinnant's algorithm).
long long days_from_civil(long long year, unsigned month, unsigned day) {
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(year - era * 400);
	const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool leap(long long year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

unsigned days_in_month(long long year, unsigned month) {
	static const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month == 2 && leap(year) ? 29 : lengths[month - 1];
}

bool plausible_year(long long year) { return year >= 1900 && year <= 2200; }

std::optional<TemporalValue> day(long long year, unsigned month, unsigned dayOfMonth) {
	if (!plausible_year(year) || month < 1 || month > 12 || dayOfMonth < 1 || dayOfMonth > days_in_month(year, month))
		return std::nullopt;
	const long long at = days_from_civil(year, month, dayOfMonth);
	return TemporalValue{at, at, Precision::Day};
}

std::optional<TemporalValue> month_of(long long year, unsigned month) {
	if (!plausible_year(year) || month < 1 || month > 12) return std::nullopt;
	return TemporalValue{days_from_civil(year, month, 1), days_from_civil(year, month, days_in_month(year, month)),
		Precision::Month};
}

std::optional<TemporalValue> quarter_of(long long year, unsigned quarter) {
	if (!plausible_year(year) || quarter < 1 || quarter > 4) return std::nullopt;
	const unsigned first = (quarter - 1) * 3 + 1;
	return TemporalValue{days_from_civil(year, first, 1), days_from_civil(year, first + 2, days_in_month(year, first + 2)),
		Precision::Quarter};
}

std::optional<TemporalValue> year_of(long long year) {
	if (!plausible_year(year)) return std::nullopt;
	return TemporalValue{days_from_civil(year, 1, 1), days_from_civil(year, 12, 31), Precision::Year};
}

// Lower case for ASCII and Cyrillic, byte for byte the same length, so offsets
// found in the lowered text stand in the original.
std::string lowered(const std::string &text) {
	std::string result = text;
	for (std::size_t index = 0; index < result.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(result[index]);
		if (byte >= 'A' && byte <= 'Z') {
			result[index] = static_cast<char>(byte - 'A' + 'a');
		} else if (byte == 0xD0 && index + 1 < result.size()) {
			const unsigned char next = static_cast<unsigned char>(result[index + 1]);
			if (next >= 0x90 && next <= 0x9F) result[index + 1] = static_cast<char>(next + 0x20);            // …
			else if (next >= 0xA0 && next <= 0xAF) { result[index] = static_cast<char>(0xD1); result[index + 1] = static_cast<char>(next - 0x20); } // …
			else if (next == 0x81) { result[index] = static_cast<char>(0xD1); result[index + 1] = static_cast<char>(0x91); } // …
			++index;
		} else if (byte >= 0xC0) {
			++index;
		}
	}
	return result;
}

// A month's name, whole or cut short after its third letter: "march", "mar.",
// "sept". A word that only begins like one - "marketing", "decision",
// "junior" - names no month.
unsigned english_month(const std::string &name) {
	static const char *months[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september",
		"october", "november", "december"};
	std::string word = name;
	if (!word.empty() && word.back() == '.') word.pop_back();
	if (word.size() < 3) return 0;
	for (unsigned index = 0; index < 12; ++index)
		if (std::string(months[index]).compare(0, word.size(), word) == 0) return index + 1;
	return 0;
}

// Russian months in any case a date writes them in: … (of March), …
// (March), … (in March) - or cut short, as a register or a letterhead
// writes them: «…».
unsigned russian_month(const std::string &written) {
	// …: the stops a
	// month is cut at, and nothing but the month's own letters before them.
	static const std::pair<const char *, unsigned> abbreviations[] = {{"\xD1\x8F\xD0\xBD\xD0\xB2", 1},
		{"\xD1\x84\xD0\xB5\xD0\xB2", 2}, {"\xD1\x84\xD0\xB5\xD0\xB2\xD1\x80", 2}, {"\xD0\xBC\xD0\xB0\xD1\x80", 3},
		{"\xD0\xB0\xD0\xBF\xD1\x80", 4}, {"\xD0\xB8\xD1\x8E\xD0\xBD", 6}, {"\xD0\xB8\xD1\x8E\xD0\xBB", 7},
		{"\xD0\xB0\xD0\xB2\xD0\xB3", 8}, {"\xD1\x81\xD0\xB5\xD0\xBD", 9}, {"\xD1\x81\xD0\xB5\xD0\xBD\xD1\x82", 9},
		{"\xD0\xBE\xD0\xBA\xD1\x82", 10}, {"\xD0\xBD\xD0\xBE\xD1\x8F", 11}, {"\xD0\xBD\xD0\xBE\xD1\x8F\xD0\xB1", 11},
		{"\xD0\xB4\xD0\xB5\xD0\xBA", 12}};
	std::string word = written;
	const bool stop = !word.empty() && word.back() == '.';
	if (stop) word.pop_back();
	for (const auto &[abbreviation, month] : abbreviations)
		if (word == abbreviation) return month;
	// A month written whole takes no full stop of its own.
	if (stop) return 0;
	static const char *stems[] = {"\xD1\x8F\xD0\xBD\xD0\xB2\xD0\xB0\xD1\x80", "\xD1\x84\xD0\xB5\xD0\xB2\xD1\x80\xD0\xB0\xD0\xBB",
		"\xD0\xBC\xD0\xB0\xD1\x80\xD1\x82", "\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB5\xD0\xBB", "\xD0\xBC\xD0\xB0",
		"\xD0\xB8\xD1\x8E\xD0\xBD", "\xD0\xB8\xD1\x8E\xD0\xBB", "\xD0\xB0\xD0\xB2\xD0\xB3\xD1\x83\xD1\x81\xD1\x82",
		"\xD1\x81\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8F\xD0\xB1\xD1\x80", "\xD0\xBE\xD0\xBA\xD1\x82\xD1\x8F\xD0\xB1\xD1\x80",
		"\xD0\xBD\xD0\xBE\xD1\x8F\xD0\xB1\xD1\x80", "\xD0\xB4\xD0\xB5\xD0\xBA\xD0\xB0\xD0\xB1\xD1\x80"};
	// Endings a month name takes: -… (…), -…
	static const char *endings[] = {"", "\xD1\x8C", "\xD1\x8F", "\xD0\xB5", "\xD0\xB0", "\xD0\xB9"};
	for (unsigned index = 0; index < 12; ++index) {
		const std::string stem = stems[index];
		if (word.compare(0, stem.size(), stem) != 0) continue;
		const std::string rest = word.substr(stem.size());
		for (const char *ending : endings)
			if (rest == ending) {
				// "…" alone is not a month: May is …
				if (index == 4 && rest.empty()) return 0;
				return index + 1;
			}
	}
	return 0;
}

unsigned roman_or_digit_quarter(const std::string &word) {
	if (word == "i" || word == "1") return 1;
	if (word == "ii" || word == "2") return 2;
	if (word == "iii" || word == "3") return 3;
	if (word == "iv" || word == "4") return 4;
	return 0;
}

struct Pattern {
	std::regex expression;
	int kind;
};

// The shapes a date is written in, most precise first. Matched in the lowered
// text; Russian words are matched as their UTF-8 bytes.
const std::vector<Pattern> &patterns() {
	const std::string cyr = "(?:[\\xD0\\xD1][\\x80-\\xBF])+";
	static const std::vector<Pattern> all{
		{std::regex("(\\d{4})-(\\d{2})-(\\d{2})"), 0},
		{std::regex("(\\d{1,2})\\.(\\d{1,2})\\.(\\d{4})"), 1},
		{std::regex("(\\d{1,2})/(\\d{1,2})/(\\d{4})"), 2},
		// "March 3, 2026", "March 3 2026", and the hurried "March 3,2026".
		{std::regex("([a-z]{3,9}\\.?) +(\\d{1,2})(?:st|nd|rd|th)?(?:, *| +)(\\d{4})"), 3},
		// "3 March 2026", and the day an execution clause names: "the 1st day of
		// July, 2026", "the 15th of March 2026".
		{std::regex("(\\d{1,2})(?:st|nd|rd|th)? +(?:day +of +|of +)?([a-z]{3,9}\\.?),? +(\\d{4})"), 4},
		// The day in guillemets, straight or curly quotes: «03», "03", “03”, „03“;
		// or as its ordinal: «…».
		{std::regex("(?:\\xC2\\xAB|\"|\\xE2\\x80\\x9C|\\xE2\\x80\\x9E)?(\\d{1,2})(?:\\xC2\\xBB|\"|\\xE2\\x80\\x9D|\\xE2\\x80\\x9C"
			"|-\\xD0\\xB3\\xD0\\xBE|-\\xD0\\xBE\\xD0\\xB5|-\\xD0\\xB5)? +(" + cyr + "\\.?) +(\\d{4})"), 5},
		{std::regex("([a-z]{3,9}\\.?),? +(\\d{4})"), 6},
		{std::regex("(" + cyr + "\\.?) +(\\d{4})"), 7},
		{std::regex("q([1-4]) *(\\d{4})"), 8},
		{std::regex("(first|second|third|fourth|1st|2nd|3rd|4th) +quarter(?: +of)? +(\\d{4})"), 9},
		{std::regex("(iv|iii|ii|i|[1-4]) +(" + cyr + ") +(\\d{4})"), 10},
		{std::regex("(\\d{4})"), 11},
	};
	return all;
}

// The code point written at text[at], and how many bytes it takes; a broken
// sequence reads as one byte of its own.
std::pair<unsigned long, std::size_t> code_point_at(const std::string &text, std::size_t at) {
	const unsigned char lead = static_cast<unsigned char>(text[at]);
	const std::size_t size = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (size == 1 || at + size > text.size()) return {lead, 1};
	unsigned long point = lead & (0xFF >> (size + 1));
	for (std::size_t index = 1; index < size; ++index) {
		const unsigned char next = static_cast<unsigned char>(text[at + index]);
		if ((next & 0xC0) != 0x80) return {lead, 1};
		point = (point << 6) | (next & 0x3F);
	}
	return {point, size};
}

// Whether a code point goes on a word: a letter or a figure of any script.
// Spaces and marks never do - a no-break space, «guillemets», “curly quotes”,
// an en or em dash, a section sign - so "“March 3, 2026”", «15.03.2026» and
// "…" write their dates as words of their own, as a straight
// quote or a hyphen always did (the last byte of a curly quote once counted as
// a letter, and each of these went out readable).
bool continues_word(unsigned long point) {
	if (point < 0x80) return std::isalnum(static_cast<unsigned char>(point)) != 0;
	if (point <= 0xBF) return false;                                  // Latin-1 marks: no-break space, «, », §, ©
	if (point == 0xD7 || point == 0xF7) return false;                 // × ÷
	if (point >= 0x2000 && point <= 0x2BFF) return false;             // punctuation, currency signs, arrows, symbols
	if (point >= 0x3000 && point <= 0x303F) return false;             // CJK punctuation
	if (point >= 0xFE30 && point <= 0xFE4F) return false;
	if (point >= 0xFF00 && point <= 0xFF0F) return false;             // full-width marks
	return true;
}

// Whether the text goes on a word right before `offset`.
bool word_before(const std::string &text, std::size_t offset) {
	if (offset == 0) return false;
	std::size_t start = offset - 1;
	while (start > 0 && offset - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
	return continues_word(code_point_at(text, start).first);
}

// Whether the text goes on a word at `offset`.
bool word_at(const std::string &text, std::size_t offset) {
	return offset < text.size() && continues_word(code_point_at(text, offset).first);
}

// Whether a year's end at `after` is closed by what a Russian date writes glued
// to it: «…» - the year's word cut short, and nothing
// of another word after it. «…» is written apart, and «…» is no date.
bool closed_by_year_word(const std::string &text, std::size_t after) {
	if (text.compare(after, 2, "\xD0\xB3") != 0) return false;
	return !word_at(text, after + 2);
}

std::optional<TemporalValue> reading(const std::smatch &match, int kind) {
	const auto number = [&](int group) { return std::stoll(match[group].str()); };
	switch (kind) {
		case 0: return day(number(1), static_cast<unsigned>(number(2)), static_cast<unsigned>(number(3)));
		case 1: return day(number(3), static_cast<unsigned>(number(2)), static_cast<unsigned>(number(1)));
		case 2: return day(number(3), static_cast<unsigned>(number(1)), static_cast<unsigned>(number(2)));
		case 3: {
			const unsigned month = english_month(match[1].str());
			return month == 0 ? std::nullopt : day(number(3), month, static_cast<unsigned>(number(2)));
		}
		case 4: {
			const unsigned month = english_month(match[2].str());
			return month == 0 ? std::nullopt : day(number(3), month, static_cast<unsigned>(number(1)));
		}
		case 5: {
			const unsigned month = russian_month(match[2].str());
			return month == 0 ? std::nullopt : day(number(3), month, static_cast<unsigned>(number(1)));
		}
		case 6: {
			const unsigned month = english_month(match[1].str());
			return month == 0 ? std::nullopt : month_of(number(2), month);
		}
		case 7: {
			const unsigned month = russian_month(match[1].str());
			return month == 0 ? std::nullopt : month_of(number(2), month);
		}
		case 8: return quarter_of(number(2), static_cast<unsigned>(number(1)));
		case 9: {
			const std::string word = match[1].str();
			const unsigned quarter = word == "first" || word == "1st" ? 1 : word == "second" || word == "2nd" ? 2
				: word == "third" || word == "3rd" ? 3 : 4;
			return quarter_of(number(2), quarter);
		}
		case 10: {
			// "…" - the word must be a form of «…».
			const std::string word = match[2].str();
			const std::string quarterWord = "\xD0\xBA\xD0\xB2\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0\xD0\xBB";
			if (word.compare(0, quarterWord.size(), quarterWord) != 0) return std::nullopt;
			return quarter_of(number(3), roman_or_digit_quarter(match[1].str()));
		}
		case 11: return year_of(number(1));
		default: return std::nullopt;
	}
}

// Spaces as one plain space: no-break and thin spaces, tabs and line breaks.
std::string plain_spaces(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
			result += "  ";
			++index;
			continue;
		}
		result.push_back(byte == '\t' || byte == '\n' || byte == '\r' ? ' ' : static_cast<char>(byte));
	}
	return result;
}

} // namespace

SlashOrder slash_order_of(const std::string &text) {
	const std::string searchable = lowered(plain_spaces(text));
	bool monthFirst = false;
	bool dayFirst = false;
	static const std::regex slashed("(\\d{1,2})/(\\d{1,2})/(\\d{4})");
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), slashed), end; it != end; ++it) {
		const int first = std::stoi((*it)[1].str());
		const int second = std::stoi((*it)[2].str());
		if (first > 12 && first <= 31 && second >= 1 && second <= 12) dayFirst = true;
		if (second > 12 && second <= 31 && first >= 1 && first <= 12) monthFirst = true;
	}
	// A text that states its format: "(MM/DD/YYYY)", "dates are dd/mm/yyyy", «…».
	static const std::regex statedMonthFirst("\\bmm/dd/(?:yyyy|yy)\\b");
	static const std::regex statedDayFirst("\\bdd/mm/(?:yyyy|yy)\\b|\\xD0\\xB4\\xD0\\xB4/\\xD0\\xBC\\xD0\\xBC/\\xD0\\xB3\\xD0\\xB3");
	if (std::regex_search(searchable, statedMonthFirst)) monthFirst = true;
	if (std::regex_search(searchable, statedDayFirst)) dayFirst = true;
	if (monthFirst != dayFirst) return monthFirst ? SlashOrder::MonthFirst : SlashOrder::DayFirst;
	// Nothing else settles it. How a text writes its other days is no evidence:
	// a US note is dated "the 1st day of July, 2026" and schedules "03/04/2027";
	// a Russian letter «…» quotes a US invoice of "03/04/2026". Either
	// read as day first was an exact wrong date with no doubt on it.
	return SlashOrder::Unknown;
}

namespace {

// A pure reading of a text, kept for the next time the same words are read: a
// data room's lines are read again on every call of an operation and every
// request of its Matter - a protected answer read the same passages four times
// over, 13 s of its 41 s (11 Oct 2026). Shared by every thread, for a line's
// worth of text at a time, and bounded by what it holds.
template <typename Value>
class Remembered {
public:
	template <typename Read>
	Value get(const std::string &key, Read read) {
		if (key.size() > kLongestKey) return read();
		{
			const std::lock_guard<std::mutex> guard(mutex_);
			const auto found = values_.find(key);
			if (found != values_.end()) return found->second;
		}
		Value value = read();
		const std::lock_guard<std::mutex> guard(mutex_);
		if (bytes_ + key.size() > kMostBytes) {
			values_.clear();
			bytes_ = 0;
		}
		if (values_.emplace(key, value).second) bytes_ += key.size() + 64;
		return value;
	}

private:
	static constexpr std::size_t kLongestKey = 16 * 1024;
	static constexpr std::size_t kMostBytes = 24 * 1024 * 1024;
	std::mutex mutex_;
	std::unordered_map<std::string, Value> values_;
	std::size_t bytes_ = 0;
};

std::vector<FoundDate> find_uncached(const std::string &text, SlashOrder order);

} // namespace

std::vector<FoundDate> find(const std::string &text) { return find(text, slash_order_of(text)); }

std::vector<FoundDate> find(const std::string &text, SlashOrder order) {
	static Remembered<std::vector<FoundDate>> read;
	return read.get(std::string(1, static_cast<char>('0' + static_cast<int>(order))) + text,
		[&] { return find_uncached(text, order); });
}

namespace {

std::vector<FoundDate> find_uncached(const std::string &text, SlashOrder order) {
	const std::string searchable = lowered(plain_spaces(text));
	std::vector<FoundDate> candidates;
	for (const Pattern &pattern : patterns()) {
		for (std::sregex_iterator it(searchable.begin(), searchable.end(), pattern.expression), end; it != end; ++it) {
			const std::smatch &match = *it;
			const std::size_t offset = static_cast<std::size_t>(match.position(0));
			const std::size_t length = static_cast<std::size_t>(match.length(0));
			// A date stands as its own words: "2026" inside "…" or
			// "20260" is not a year of its own.
			// A day in quotes opens the date itself: «03» … 2026 … glued to the
			// line before it (a heading's city) is still a date.
			const bool quoted = searchable.compare(offset, 2, "\xC2\xAB") == 0 || searchable[offset] == '"'
				|| searchable.compare(offset, 3, "\xE2\x80\x9C") == 0 || searchable.compare(offset, 3, "\xE2\x80\x9E") == 0;
			const bool openBefore = offset == 0 || quoted || !word_before(searchable, offset);
			const std::size_t after = offset + length;
			// A year closes at a word's end, at «…» glued to it, and a day in
			// figures at the "T" an ISO timestamp writes its time after:
			// "2026-07-01T10:30:00Z".
			const bool isoTime = pattern.kind == 0 && after + 1 < searchable.size() && searchable[after] == 't'
				&& std::isdigit(static_cast<unsigned char>(searchable[after + 1])) != 0;
			const bool closedAfter = !word_at(searchable, after) || closed_by_year_word(searchable, after) || isoTime;
			if (pattern.kind == 11) {
				const bool slashOrDash = offset > 0 && (searchable[offset - 1] == '/' || searchable[offset - 1] == '-'
					|| searchable[offset - 1] == '.');
				if (!openBefore || !closedAfter || slashOrDash) continue;
			} else if (!openBefore || !closedAfter) {
				continue;
			}
			if (pattern.kind == 2) {
				// A slashed day: the order only one reading allows, else the text's.
				const unsigned first = static_cast<unsigned>(std::stoul(match[1].str()));
				const unsigned second = static_cast<unsigned>(std::stoul(match[2].str()));
				const long long year = std::stoll(match[3].str());
				const std::optional<TemporalValue> monthFirst = day(year, first, second);
				const std::optional<TemporalValue> dayFirst = day(year, second, first);
				if (monthFirst && dayFirst && first != second) {
					const bool ambiguous = order == SlashOrder::Unknown;
					candidates.push_back({offset, length, order == SlashOrder::DayFirst ? *dayFirst : *monthFirst, ambiguous});
				} else if (monthFirst || dayFirst) {
					candidates.push_back({offset, length, monthFirst ? *monthFirst : *dayFirst, false});
				}
				continue;
			}
			if (auto value = reading(match, pattern.kind)) candidates.push_back({offset, length, *value, false});
		}
	}
	// The most precise reading of each place: a longer match wins where two overlap.
	std::sort(candidates.begin(), candidates.end(), [](const FoundDate &left, const FoundDate &right) {
		if (left.offset != right.offset) return left.offset < right.offset;
		return left.length > right.length;
	});
	std::vector<FoundDate> found;
	for (const FoundDate &candidate : candidates) {
		bool overlaps = false;
		for (FoundDate &kept : found) {
			if (candidate.offset < kept.offset + kept.length && kept.offset < candidate.offset + candidate.length) {
				if (candidate.length > kept.length) kept = candidate;
				overlaps = true;
				break;
			}
		}
		if (!overlaps) found.push_back(candidate);
	}
	std::sort(found.begin(), found.end(), [](const FoundDate &left, const FoundDate &right) { return left.offset < right.offset; });
	return found;
}

} // namespace

std::optional<TemporalValue> parse(const std::string &text) { return parse(text, SlashOrder::Unknown); }

std::optional<TemporalValue> parse(const std::string &text, SlashOrder order) {
	std::string trimmed = plain_spaces(text);
	while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
	while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.')) trimmed.pop_back();
	// "…" after a Russian date belong to it, and so does «…» glued to its year.
	for (const std::string &tail : {std::string(" \xD0\xB3"), std::string(" \xD0\xB3\xD0\xBE\xD0\xB4\xD0\xB0"),
			std::string("\xD0\xB3")})
		if (trimmed.size() > tail.size() && trimmed.compare(trimmed.size() - tail.size(), tail.size(), tail) == 0
				&& (tail.front() == ' ' || std::isdigit(static_cast<unsigned char>(trimmed[trimmed.size() - tail.size() - 1])) != 0)) {
			trimmed.erase(trimmed.size() - tail.size());
			break;
		}
	const std::vector<FoundDate> dates = find(trimmed, order == SlashOrder::Unknown ? slash_order_of(trimmed) : order);
	if (dates.size() != 1 || dates.front().offset != 0 || dates.front().length != trimmed.size()) return std::nullopt;
	if (dates.front().ambiguous) return std::nullopt;
	return dates.front().value;
}

namespace {

// The shapes a day of the year is written in without its year: 0 "July 1",
// "Jul. 1st"; 1 "1 July", "1st of July"; 2 «…».
const std::vector<Pattern> &day_of_year_patterns() {
	const std::string cyr = "(?:[\\xD0\\xD1][\\x80-\\xBF])+";
	static const std::vector<Pattern> all{
		{std::regex("([a-z]{3,9}\\.?) +(\\d{1,2})(?:st|nd|rd|th)?"), 0},
		{std::regex("(\\d{1,2})(?:st|nd|rd|th)? +(?:day +of +|of +)?([a-z]{3,9}\\.?)"), 1},
		{std::regex("(?:\\xC2\\xAB|\"|\\xE2\\x80\\x9C|\\xE2\\x80\\x9E)?(\\d{1,2})(?:\\xC2\\xBB|\"|\\xE2\\x80\\x9D|\\xE2\\x80\\x9C"
			"|-\\xD0\\xB3\\xD0\\xBE|-\\xD0\\xBE\\xD0\\xB5|-\\xD0\\xB5)? +(" + cyr + ")"), 2},
	};
	return all;
}

// February has its 29th in a leap year, and a day of the year may be that one.
unsigned longest_month(unsigned month) {
	static const unsigned lengths[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month >= 1 && month <= 12 ? lengths[month - 1] : 0;
}

// Whether the word right before `offset` (spaces between) names a part of a
// document by its number: "Section 5", "Article 12", "clause 3", "§ 4".
bool after_reference_word(const std::string &text, std::size_t offset) {
	std::size_t end = offset;
	while (end > 0 && text[end - 1] == ' ') --end;
	if (end == offset) return false;
	if (end >= 2 && text.compare(end - 2, 2, "\xC2\xA7") == 0) return true;
	std::size_t start = end;
	while (start > 0 && (std::isalpha(static_cast<unsigned char>(text[start - 1])) != 0 || text[start - 1] == '.')) --start;
	std::string word = text.substr(start, end - start);
	while (!word.empty() && word.back() == '.') word.pop_back();
	if (!word.empty() && word.back() == 's') word.pop_back();
	static const char *references[] = {"section", "sec", "article", "art", "clause", "paragraph", "para", "schedule",
		"exhibit", "annex", "appendix", "item", "part", "chapter", "rule", "note", "no", "page", "table", "step", "tier"};
	return std::any_of(std::begin(references), std::end(references), [&](const char *reference) { return word == reference; });
}

// Whether the word after `end` is one the verb "may" takes: "May be", "may not",
// "May have".
bool modal_after(const std::string &text, std::size_t end) {
	if (end >= text.size() || text[end] != ' ') return false;
	std::size_t start = end;
	while (start < text.size() && text[start] == ' ') ++start;
	std::size_t stop = start;
	while (stop < text.size() && std::isalpha(static_cast<unsigned char>(text[stop])) != 0) ++stop;
	const std::string word = text.substr(start, stop - start);
	for (const char *verb : {"be", "not", "have", "also", "only", "elect", "request", "terminate", "require"})
		if (word == verb) return true;
	return false;
}

std::vector<FoundDayOfYear> days_of_year_in(const std::string &text, bool capital) {
	const std::string spaced = plain_spaces(text);
	const std::string searchable = lowered(spaced);
	// A day written with its year is a date, never a day of the year inside it.
	const std::vector<FoundDate> dated = find(text);
	std::vector<FoundDayOfYear> found;
	for (const Pattern &pattern : day_of_year_patterns()) {
		for (std::sregex_iterator it(searchable.begin(), searchable.end(), pattern.expression), end; it != end; ++it) {
			const std::smatch &match = *it;
			const std::size_t offset = static_cast<std::size_t>(match.position(0));
			std::size_t length = static_cast<std::size_t>(match.length(0));
			const std::size_t after = offset + length;
			if (word_before(searchable, offset) || word_at(searchable, after)) continue;
			// "July 1.5", "1/2 July": figures that go on are another number.
			const auto joiner = [](char character) {
				return character == '.' || character == ',' || character == '/' || character == ':' || character == '-';
			};
			if (after + 1 < searchable.size() && joiner(searchable[after]) && std::isdigit(static_cast<unsigned char>(searchable[after + 1])))
				continue;
			if (offset >= 2 && joiner(searchable[offset - 1]) && std::isdigit(static_cast<unsigned char>(searchable[offset - 2])))
				continue;
			unsigned month = 0;
			unsigned day = 0;
			std::size_t monthAt = 0;
			if (pattern.kind == 0) {
				month = english_month(match[1].str());
				day = static_cast<unsigned>(std::stoul(match[2].str()));
				monthAt = static_cast<std::size_t>(match.position(1));
			} else if (pattern.kind == 1) {
				month = english_month(match[2].str());
				day = static_cast<unsigned>(std::stoul(match[1].str()));
				monthAt = static_cast<std::size_t>(match.position(2));
			} else {
				month = russian_month(match[2].str());
				day = static_cast<unsigned>(std::stoul(match[1].str()));
			}
			if (month == 0 || day < 1 || day > longest_month(month)) continue;
			// A number after a reference's word is the reference's: "Section 5 May
			// be amended" names Section 5 and a verb, never the fifth of May; and
			// "May" before "be", "not" or "have" is the verb.
			if (pattern.kind == 1 && (after_reference_word(searchable, offset)
					|| (month == 5 && modal_after(searchable, static_cast<std::size_t>(match.position(2) + match.length(2))))))
				continue;
			// An English month is a name, written with its capital.
			if (pattern.kind != 2 && capital && !(spaced[monthAt] >= 'A' && spaced[monthAt] <= 'Z')) continue;
			// "1 July." ends a sentence: a full stop after a month written whole is
			// the sentence's, and only an abbreviation's own ("1 Sept.") is the date's.
			if (pattern.kind == 1 && searchable[after - 1] == '.') {
				static const char *whole[] = {"january", "february", "march", "april", "may", "june", "july", "august",
					"september", "october", "november", "december"};
				const std::string word = match[2].str().substr(0, match[2].str().size() - 1);
				if (std::any_of(std::begin(whole), std::end(whole), [&](const char *name) { return word == name; })) --length;
			}
			const bool insideDate = std::any_of(dated.begin(), dated.end(), [&](const FoundDate &date) {
				return date.value.precision != Precision::Year && offset < date.offset + date.length && date.offset < offset + length;
			});
			if (insideDate) continue;
			found.push_back({offset, length, DayOfYear{month, day}});
		}
	}
	std::sort(found.begin(), found.end(), [](const FoundDayOfYear &left, const FoundDayOfYear &right) {
		if (left.offset != right.offset) return left.offset < right.offset;
		return left.length > right.length;
	});
	std::vector<FoundDayOfYear> kept;
	for (const FoundDayOfYear &candidate : found)
		if (kept.empty() || candidate.offset >= kept.back().offset + kept.back().length) kept.push_back(candidate);
	return kept;
}

} // namespace

std::vector<FoundDayOfYear> find_days_of_year(const std::string &text) {
	static Remembered<std::vector<FoundDayOfYear>> read;
	return read.get(text, [&] { return days_of_year_in(text, true); });
}

std::optional<DayOfYear> parse_day_of_year(const std::string &text) {
	// A pure reading of its words, asked again and again for the same few
	// writings (each protected value's own, once for every line a legend
	// checks): remembered per thread, a bounded number at a time.
	thread_local std::unordered_map<std::string, std::optional<DayOfYear>> read;
	const auto known = read.find(text);
	if (known != read.end()) return known->second;
	if (read.size() >= 8192) read.clear();
	std::optional<DayOfYear> value = parse_day_of_year_uncached(text);
	read.emplace(text, value);
	return value;
}

std::optional<DayOfYear> parse_day_of_year_uncached(const std::string &text) {
	std::string trimmed = plain_spaces(text);
	while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
	while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
	const std::vector<FoundDayOfYear> days = days_of_year_in(trimmed, false);
	if (days.size() != 1 || days.front().offset != 0) return std::nullopt;
	std::size_t end = days.front().length;
	if (end < trimmed.size() && trimmed[end] == '.' && end + 1 == trimmed.size()) ++end;
	if (end != trimmed.size()) return std::nullopt;
	return days.front().value;
}

TemporalOrder compare(const TemporalValue &a, const TemporalValue &b) {
	if (a.last_day < b.first_day) return TemporalOrder::Before;
	if (a.first_day > b.last_day) return TemporalOrder::After;
	if (a.precision == Precision::Day && b.precision == Precision::Day && a.first_day == b.first_day) return TemporalOrder::Same;
	return TemporalOrder::Unknown;
}

} // namespace singularity::context::temporal
