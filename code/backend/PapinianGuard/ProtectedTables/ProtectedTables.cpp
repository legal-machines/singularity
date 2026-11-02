#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"
#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"

#include "PapinianGuard/SemanticRoles/SemanticRoles.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

namespace singularity::protected_api::papinian::tables {
namespace {

namespace documents = singularity::documents;
namespace temporal = singularity::context::temporal;
using semantic::Kind;
using semantic::Rational;
using semantic::Value;

// ---------------------------------------------------------------------------
// Words

// ASCII and Cyrillic letters in lower case, byte for byte otherwise.
std::string fold(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte < 0x80) {
			result += static_cast<char>(std::tolower(byte));
			continue;
		}
		if (index + 1 < text.size()) {
			const unsigned char next = static_cast<unsigned char>(text[index + 1]);
			if (byte == 0xD0 && next >= 0x90 && next <= 0x9F) {  // А-П
				result += static_cast<char>(0xD0);
				result += static_cast<char>(next + 0x20);
				++index;
				continue;
			}
			if (byte == 0xD0 && next >= 0xA0 && next <= 0xAF) {  // Р-Я
				result += static_cast<char>(0xD1);
				result += static_cast<char>(next - 0x20);
				++index;
				continue;
			}
			if (byte == 0xD0 && next == 0x81) {  // Ё
				result += "\xD1\x91";
				++index;
				continue;
			}
		}
		result += static_cast<char>(byte);
	}
	return result;
}

std::string trim(std::string_view text) {
	std::size_t from = 0;
	std::size_t to = text.size();
	while (from < to && (text[from] == ' ' || text[from] == '\t' || text[from] == '\n' || text[from] == '\r')) ++from;
	while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\n' || text[to - 1] == '\r')) --to;
	return std::string(text.substr(from, to - from));
}

bool letter_at(const std::string &text, std::size_t position) {
	if (position >= text.size()) return false;
	const unsigned char byte = static_cast<unsigned char>(text[position]);
	return std::isalpha(byte) != 0 || byte == 0xD0 || byte == 0xD1;
}

bool letter_before(const std::string &text, std::size_t position) {
	if (position == 0) return false;
	const unsigned char byte = static_cast<unsigned char>(text[position - 1]);
	if (byte < 0x80) return std::isalpha(byte) != 0;
	return position >= 2 && (byte & 0xC0) == 0x80
		&& (static_cast<unsigned char>(text[position - 2]) == 0xD0 || static_cast<unsigned char>(text[position - 2]) == 0xD1);
}

// `word` standing as a word in `folded`; a trailing '*' makes it a stem.
bool has_word(const std::string &folded, std::string_view word) {
	const bool stem = !word.empty() && word.back() == '*';
	const std::string_view body = stem ? word.substr(0, word.size() - 1) : word;
	if (body.empty()) return false;
	for (std::size_t at = folded.find(body); at != std::string::npos; at = folded.find(body, at + 1)) {
		if (letter_before(folded, at)) continue;
		const std::size_t end = at + body.size();
		if (stem || !letter_at(folded, end)) return true;
		if (folded.compare(end, 1, "s") == 0 && !letter_at(folded, end + 1)) return true;
	}
	return false;
}

bool any_word(const std::string &folded, std::initializer_list<std::string_view> words) {
	return std::any_of(words.begin(), words.end(), [&](std::string_view word) { return has_word(folded, word); });
}

// ---------------------------------------------------------------------------
// Cells and columns

std::string column_letters(std::size_t zeroBased) {
	std::string letters;
	std::size_t number = zeroBased + 1;
	while (number > 0) {
		const std::size_t rest = (number - 1) % 26;
		letters.insert(letters.begin(), static_cast<char>('A' + rest));
		number = (number - 1) / 26;
	}
	return letters;
}

std::size_t column_number(const std::string &letters) {
	std::size_t number = 0;
	for (const char letter : letters) number = number * 26 + static_cast<std::size_t>(letter - 'A' + 1);
	return number;
}

// "B12" -> ("B", 12); nothing for anything else.
std::optional<std::pair<std::string, std::size_t>> split_ref(const std::string &ref) {
	std::size_t at = 0;
	while (at < ref.size() && ref[at] == '$') ++at;
	const std::size_t letters = at;
	while (at < ref.size() && ref[at] >= 'A' && ref[at] <= 'Z') ++at;
	if (at == letters || at - letters > 3) return std::nullopt;
	const std::string column = ref.substr(letters, at - letters);
	while (at < ref.size() && ref[at] == '$') ++at;
	const std::size_t digits = at;
	while (at < ref.size() && ref[at] >= '0' && ref[at] <= '9') ++at;
	if (at == digits || at != ref.size() || at - digits > 7) return std::nullopt;
	return std::make_pair(column, static_cast<std::size_t>(std::strtoul(ref.c_str() + digits, nullptr, 10)));
}

// Days since 1970-01-01 of a civil date, and back (Howard Hinnant's).
long long days_from_civil(long long year, unsigned month, unsigned day) {
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(year - era * 400);
	const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civil_from_days(long long days, long long &year, unsigned &month, unsigned &day) {
	days += 719468;
	const long long era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(days - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	year = static_cast<long long>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	if (month <= 2) ++year;
}

// The interval of the month, quarter or year a day falls in.
temporal::TemporalValue period_of(long long day, temporal::Precision precision) {
	long long year = 0;
	unsigned month = 0;
	unsigned date = 0;
	civil_from_days(day, year, month, date);
	switch (precision) {
		case temporal::Precision::Day: return {day, day, precision};
		case temporal::Precision::Month: {
			const long long first = days_from_civil(year, month, 1);
			const long long next = month == 12 ? days_from_civil(year + 1, 1, 1) : days_from_civil(year, month + 1, 1);
			return {first, next - 1, precision};
		}
		case temporal::Precision::Quarter: {
			const unsigned start = (month - 1) / 3 * 3 + 1;
			const long long first = days_from_civil(year, start, 1);
			const long long next = start == 10 ? days_from_civil(year + 1, 1, 1) : days_from_civil(year, start + 3, 1);
			return {first, next - 1, precision};
		}
		case temporal::Precision::Year:
			return {days_from_civil(year, 1, 1), days_from_civil(year + 1, 1, 1) - 1, precision};
	}
	return {day, day, temporal::Precision::Day};
}

// An Excel serial day as days since 1970 (the 1900 system counts a February
// 29, 1900 that never was).
long long excel_day(double serial, bool date1904) {
	const long long whole = static_cast<long long>(serial);
	if (date1904) return whole - 24107;
	return whole >= 61 ? whole - 25569 : whole - 25568;
}

// A number exactly as a workbook stores it - "0.14883720930232559",
// "1.25E-3", "-42" - as a fraction.
std::optional<Rational> stored_decimal(const std::string &text) {
	std::string mantissa = text;
	long exponent = 0;
	const std::size_t e = text.find_first_of("eE");
	if (e != std::string::npos) {
		mantissa = text.substr(0, e);
		char *end = nullptr;
		exponent = std::strtol(text.c_str() + e + 1, &end, 10);
		if (end == text.c_str() + e + 1 || exponent > 30 || exponent < -30) return std::nullopt;
	}
	bool negative = false;
	if (!mantissa.empty() && (mantissa.front() == '-' || mantissa.front() == '+')) {
		negative = mantissa.front() == '-';
		mantissa.erase(0, 1);
	}
	std::string digits;
	long point = -1;
	for (const char character : mantissa) {
		if (character == '.') {
			if (point >= 0) return std::nullopt;
			point = static_cast<long>(digits.size());
			continue;
		}
		if (character < '0' || character > '9') return std::nullopt;
		digits += character;
	}
	if (digits.empty()) return std::nullopt;
	if (point < 0) point = static_cast<long>(digits.size());
	point += exponent;
	while (point > static_cast<long>(digits.size())) digits += '0';
	while (point < 0) {
		digits.insert(digits.begin(), '0');
		++point;
	}
	std::string decimal = digits.substr(0, static_cast<std::size_t>(point));
	if (decimal.empty()) decimal = "0";
	const std::string fraction = digits.substr(static_cast<std::size_t>(point));
	std::string written = decimal;
	if (!fraction.empty()) written += "." + fraction;
	// Leading zeros do not count toward the digits Rational reads.
	std::size_t skip = 0;
	while (skip + 1 < written.size() && written[skip] == '0' && written[skip + 1] != '.') ++skip;
	written.erase(0, skip);
	std::optional<Rational> value = Rational::parse_decimal(written);
	if (!value) return std::nullopt;
	return negative ? -*value : *value;
}

// What a number format shows: a percentage, an amount in a currency (its ISO
// code), how many decimals, and for a date the finest part it draws.
struct FormatReading {
	bool percent = false;
	std::string currency;
	int decimals = 0;
	bool day = false;
	bool month = false;
	bool year = false;
};

std::string currency_code(const std::string &symbol) {
	if (symbol == "$" || symbol == "USD" || symbol == "US$") return "USD";
	if (symbol == "\xE2\x82\xAC" || symbol == "EUR") return "EUR";
	if (symbol == "\xC2\xA3" || symbol == "GBP") return "GBP";
	if (symbol == "\xC2\xA5" || symbol == "JPY") return "JPY";
	if (symbol == "\xE2\x82\xBD" || symbol == "RUB" || symbol == "\xD1\x80\xD1\x83\xD0\xB1." || symbol == "\xD1\x80."
			|| symbol == "\xD1\x80\xD1\x83\xD0\xB1")
		return "RUB";
	return semantic::currency_named("(" + symbol + ")");
}

FormatReading read_format(const std::string &format) {
	FormatReading reading;
	bool afterPoint = false;
	for (std::size_t index = 0; index < format.size(); ++index) {
		const char character = format[index];
		if (character == ';') break;
		if (character == '"') {
			const std::size_t end = format.find('"', index + 1);
			const std::string literal = format.substr(index + 1, end == std::string::npos ? std::string::npos : end - index - 1);
			const std::string code = currency_code(trim(literal));
			if (!code.empty()) reading.currency = code;
			index = end == std::string::npos ? format.size() : end;
			continue;
		}
		if (character == '\\') {
			if (index + 1 < format.size() && format[index + 1] == '$') reading.currency = "USD";
			++index;
			continue;
		}
		if (character == '[') {
			const std::size_t end = format.find(']', index);
			const std::string inner = format.substr(index + 1, end == std::string::npos ? std::string::npos : end - index - 1);
			if (inner.size() > 1 && inner[0] == '$') {
				const std::size_t dash = inner.find('-');
				const std::string symbol = inner.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
				const std::string code = currency_code(symbol);
				if (!code.empty()) reading.currency = code;
			}
			index = end == std::string::npos ? format.size() : end;
			continue;
		}
		if (character == '_' || character == '*') {
			++index;
			continue;
		}
		if (character == '%') reading.percent = true;
		else if (character == '$') reading.currency = "USD";
		else if (format.compare(index, 3, "\xE2\x82\xAC") == 0) { reading.currency = "EUR"; index += 2; }
		else if (format.compare(index, 3, "\xE2\x82\xBD") == 0) { reading.currency = "RUB"; index += 2; }
		else if (format.compare(index, 2, "\xC2\xA3") == 0) { reading.currency = "GBP"; index += 1; }
		else if (format.compare(index, 2, "\xC2\xA5") == 0) { reading.currency = "JPY"; index += 1; }
		else if (character == '.') afterPoint = true;
		else if ((character == '0' || character == '#') && afterPoint) ++reading.decimals;
		else if (character == 'd' || character == 'D') reading.day = true;
		else if (character == 'm' || character == 'M') reading.month = true;
		else if (character == 'y' || character == 'Y') reading.year = true;
	}
	return reading;
}

// How finely a date cell's format draws it: a format with no day is a month;
// with neither day nor month, a year.
temporal::Precision date_precision(const FormatReading &format) {
	if (format.day) return temporal::Precision::Day;
	if (format.month) return temporal::Precision::Month;
	if (format.year) return temporal::Precision::Year;
	return temporal::Precision::Day;
}

std::string precision_word(temporal::Precision precision) {
	switch (precision) {
		case temporal::Precision::Day: return "day";
		case temporal::Precision::Month: return "month";
		case temporal::Precision::Quarter: return "quarter";
		case temporal::Precision::Year: return "year";
	}
	return "day";
}

std::string origin_word(FactProvenance::Origin origin) {
	switch (origin) {
		case FactProvenance::Origin::Source: return "source";
		case FactProvenance::Origin::ProjectContext: return "context";
		case FactProvenance::Origin::History: return "history";
		case FactProvenance::Origin::Request: return "request";
		case FactProvenance::Origin::Instruction: return "instruction";
		case FactProvenance::Origin::Derived: return "derived";
		case FactProvenance::Origin::Occurrence: return "occurrence";
		case FactProvenance::Origin::External: return "external";
	}
	return "source";
}

// ---------------------------------------------------------------------------
// Header words: the closed role classes a ledger's headers name, used where
// the role lexicon (SemanticRoles) names none. Legal-general words only.

std::string header_role(const std::string &header) {
	const std::string folded = fold(header);
	if (folded.empty()) return {};
	// A shift turns a role word into another: "Unpaid", «Не оплачено».
	if (any_word(folded, {"unpaid", "overdue", "outstanding", "remaining", "arrears", "balance", "owed", "owing",
			"\xD0\xB7\xD0\xB0\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB5\xD0\xBD*",   // задолжен*
			"\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD1\x82*",                           // остат*
			"\xD1\x81\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xBE",                    // сальдо
			"\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB3*",                                   // долг*
			"\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x87*"}))  // недоплач*
		return "BALANCE";
	if (any_word(folded, {"penalty", "penalties", "late fee", "late charge", "liquidated damages",
			"\xD0\xBD\xD0\xB5\xD1\x83\xD1\x81\xD1\x82\xD0\xBE\xD0\xB9\xD0\xBA*",   // неустойк*
			"\xD0\xBF\xD0\xB5\xD0\xBD\xD0\xB8",                                    // пени
			"\xD1\x88\xD1\x82\xD1\x80\xD0\xB0\xD1\x84*"}))                          // штраф*
		return "PENALTY";
	if (any_word(folded, {"interest", "\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8B"}))  // проценты
		return "INTEREST";
	if (any_word(folded, {"offset", "set-off", "credit memo", "credit note", "discount", "adjustment", "escalation",
			"\xD0\xB7\xD0\xB0\xD1\x87\xD0\xB5\xD1\x82*", "\xD0\xB7\xD0\xB0\xD1\x87\xD1\x91\xD1\x82*",  // зачет*, зачёт*
			"\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB4\xD0\xBA*",                           // скидк*
			"\xD0\xB8\xD0\xBD\xD0\xB4\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB0\xD1\x86*"}))  // индексац*
		return "ADJUSTMENT";
	if (any_word(folded, {"paid", "payment", "payments", "received", "receipt", "remitted", "collected",
			"\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x87*", "\xD1\x83\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x87*",  // оплач*, уплач*
			"\xD0\xBF\xD0\xBE\xD1\x81\xD1\x82\xD1\x83\xD0\xBF*",                   // поступ*
			"\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE"}))   // оплачено
		return "PAID";
	if (any_word(folded, {"billed", "invoiced", "charged", "accrued", "scheduled", "due", "payable", "invoice amount",
			"\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB*",                   // начисл*
			"\xD0\xB2\xD1\x8B\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB*",           // выставл*
			"\xD0\xBA \xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5"}))          // к оплате
		return "BILLED";
	if (any_word(folded, {"total", "subtotal", "\xD0\xB8\xD1\x82\xD0\xBE\xD0\xB3*", "\xD0\xB2\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE"}))
		return "TOTAL";
	if (any_word(folded, {"rent", "price", "fee", "fees", "royalty", "royalties", "consideration",
			"\xD1\x86\xD0\xB5\xD0\xBD*",                                           // цен*
			"\xD1\x81\xD1\x82\xD0\xBE\xD0\xB8\xD0\xBC\xD0\xBE\xD1\x81\xD1\x82*",   // стоимост*
			"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82*",                                   // плат*
			"\xD0\xB0\xD1\x80\xD0\xB5\xD0\xBD\xD0\xB4*",                           // аренд*
			"\xD0\xB2\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD0\xB3\xD1\x80\xD0\xB0\xD0\xB6\xD0\xB4*",  // вознагражд*
			"\xD0\xBE\xD0\xB1\xD1\x8A\xD0\xB5\xD0\xBC*", "\xD0\xBE\xD0\xB1\xD1\x8A\xD1\x91\xD0\xBC*"}))  // объем*, объём*
		return "PRICE";
	if (any_word(folded, {"due date", "deadline", "\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA*"}))  // срок*
		return "DEADLINE";
	if (any_word(folded, {"days", "months", "number of", "count", "qty", "quantity",
			"\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9", "\xD0\xB4\xD0\xBD\xD0\xB8",       // дней, дни
			"\xD0\xBA\xD0\xBE\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2*",  // количеств*
			"\xD0\xBA\xD0\xBE\xD0\xBB-\xD0\xB2\xD0\xBE"}))                         // кол-во
		return "COUNT";
	return {};
}

// A number column counted in days or months, or a count, by its header.
// Where `word` first stands as a word in `folded` (a trailing '*' makes it a
// stem), or npos.
std::size_t word_position(const std::string &folded, std::string_view word) {
	const bool stem = !word.empty() && word.back() == '*';
	const std::string_view body = stem ? word.substr(0, word.size() - 1) : word;
	if (body.empty()) return std::string::npos;
	for (std::size_t at = folded.find(body); at != std::string::npos; at = folded.find(body, at + 1)) {
		if (letter_before(folded, at)) continue;
		const std::size_t end = at + body.size();
		if (stem || !letter_at(folded, end)) return at;
		if (folded.compare(end, 1, "s") == 0 && !letter_at(folded, end + 1)) return at;
	}
	return std::string::npos;
}

std::optional<std::string> header_measure(const std::string &header) {
	const std::string folded = fold(header);
	// The unit the label counts in is the one it names first: «Минут в месяце»
	// is minutes, "Days per month" days (a fake-provider run on the real RU2
	// register: minutes typed as months, so no downtime could be divided by it).
	struct Unit { const char *type; std::vector<std::string_view> words; };
	static const std::vector<Unit> units{
		{"DURATION(days)", {"days", "day", "\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9", "\xD0\xB4\xD0\xBD\xD0\xB8", "\xD0\xB4\xD0\xBD*"}},
		{"DURATION(months)", {"months", "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86*"}},
		{"DURATION(minutes)", {"minutes", "\xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82*", "\xD0\xBC\xD0\xB8\xD0\xBD"}},  // минут*, мин
		{"DURATION(hours)", {"hours", "\xD1\x87\xD0\xB0\xD1\x81*"}}};
	std::size_t first = std::string::npos;
	const char *named = nullptr;
	for (const Unit &unit : units)
		for (const std::string_view word : unit.words) {
			const std::size_t at = word_position(folded, word);
			if (at < first) {
				first = at;
				named = unit.type;
			}
		}
	if (named != nullptr) return std::string(named);
	if (any_word(folded, {"number of", "count", "qty", "quantity",
			"\xD0\xBA\xD0\xBE\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2*",
			"\xD0\xBA\xD0\xBE\xD0\xBB-\xD0\xB2\xD0\xBE"}))
		return std::string("COUNT");
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// Building one table

// The facts the operation's registry already holds for workbook cells, and
// for writings (a Word table's cells go through prose protection).
struct Registry {
	const OperationProtectedSymbolTable *symbols = nullptr;
	// "Ledger!B16" -> rows (the registry's own unique key, and every
	// workbook-scoped key naming that cell).
	std::map<std::string, std::vector<std::size_t>> by_cell;
	// A writing, trimmed -> quantity and date rows written so.
	std::map<std::string, std::vector<std::size_t>> by_writing;
	// A name's folded writing -> the person or organisation row.
	std::map<std::string, std::size_t> by_name;
	// A row -> the documents it was read in ("d:<scope>", scope_of): its
	// workbook-scoped cell key's, and its passage's (document_of_passage). Two
	// workbooks with a "Sheet1!B2" each are two places; a row read in one is
	// never the other's cell.
	std::map<std::size_t, std::set<std::string>> row_scopes;

	explicit Registry(const OperationProtectedSymbolTable *given) : symbols(given) {
		if (symbols == nullptr) return;
		for (const auto &[key, row] : symbols->fact_by_cell) {
			if (row == static_cast<std::size_t>(-1) || row >= symbols->mapping.size()) continue;
			if (key.rfind("cell|", 0) == 0) {
				const std::size_t bar = key.find('|', 5);
				if (bar == std::string::npos) continue;
				std::vector<std::size_t> &rows = by_cell[key.substr(bar + 1)];
				if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
				const std::string scope = key.substr(5, bar - 5);
				if (scope.rfind("d:", 0) == 0 && scope.size() > 2) row_scopes[row].insert(scope);
			} else if (key.find('|') == std::string::npos && key.find(':') == std::string::npos) {
				std::vector<std::size_t> &rows = by_cell[key];
				if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
			}
		}
		for (std::size_t row = 0; row < symbols->mapping.size(); ++row) {
			const RequestMappingEntry &entry = symbols->mapping[row];
			if (!entry.provenance.passage_sha256.empty())
				if (const auto document = symbols->document_of_passage.find(entry.provenance.passage_sha256);
						document != symbols->document_of_passage.end() && !document->second.empty())
					row_scopes[row].insert("d:" + document->second);
			if (entry.derived) continue;
			if (entry.value) {
				for (const std::string &form : entry.every_form()) by_writing[trim(form)].push_back(row);
				continue;
			}
			if (entry.type && (*entry.type == EntityType::Person || *entry.type == EntityType::Organisation
					|| *entry.type == EntityType::LegalEntity))
				for (const std::string &form : entry.every_form()) by_name.emplace(fold(trim(form)), row);
		}
	}
};

// Whether a registry value is the cell's own: the same amount (a currency the
// reading could not tell is no disagreement), the same day.
bool agrees(const Value &a, const Value &b) {
	if (a.kind == Kind::Date || b.kind == Kind::Date) {
		return a.kind == b.kind && a.date.first_day == b.date.first_day && a.date.last_day == b.date.last_day;
	}
	if (a.kind == Kind::Boolean || b.kind == Kind::Boolean) return a.kind == b.kind && a.truth == b.truth;
	if (a.kind == Kind::DayOfYear || b.kind == Kind::DayOfYear) return false;
	if (a.kind == Kind::Money && b.kind == Kind::Money && !a.currency.empty() && !b.currency.empty()
			&& a.currency != b.currency)
		return false;
	return a.amount == b.amount;
}

void link_row(const Registry &registry, std::size_t row, TableCell &cell) {
	const RequestMappingEntry &entry = registry.symbols->mapping[row];
	cell.fact_key = entry.fact_key;
	cell.placeholder = entry.placeholder;
	cell.source.marker = entry.placeholder;
	cell.source.origin = origin_word(entry.provenance.origin);
	cell.source.passage_sha256 = entry.provenance.passage_sha256;
	cell.source.offset = entry.provenance.offset;
	cell.source.length = entry.provenance.length;
	if (!entry.provenance.cell.empty()) cell.source.cell = entry.provenance.cell;
}

// Where a table stands, for linking its cells: the documents it is (its own
// id, and the scopes the evidence's passages of it were filed under), whether
// the evidence named any, and how many built tables have a cell at each
// "Sheet!B2" - all decided from structure, never from a value.
struct Place {
	std::set<std::string> scopes;
	bool evidence_scoped = false;
	const std::map<std::string, std::size_t> *cell_tables = nullptr;
};

// Whether registry row `row` may be this table's cell `sheetCell`: read in
// this table's document; or, where neither side says its document, read at a
// place only this table has.
bool in_place(const Registry &registry, std::size_t row, const Place &place, const std::string &sheetCell, bool byCell) {
	if (const auto scopes = registry.row_scopes.find(row); scopes != registry.row_scopes.end() && !scopes->second.empty()) {
		for (const std::string &scope : scopes->second)
			if (place.scopes.count(scope) != 0) return true;
		if (place.evidence_scoped) return false;
	}
	if (!byCell) return true;
	if (place.cell_tables == nullptr) return true;
	const auto count = place.cell_tables->find(sheetCell);
	return count == place.cell_tables->end() || count->second <= 1;
}

// Links a cell to the one registry row its reading produced: by the cell it is
// ("Ledger!B16") in its own document, confirmed by its value; or, for a table
// read from prose, by the one quantity row written exactly as the cell is.
void link_cell(const Registry &registry, const Place &place, const std::string &sheetCell, bool byWriting, TableCell &cell) {
	if (registry.symbols == nullptr || !cell.value) return;
	std::optional<std::size_t> found;
	const auto candidates = [&](const std::vector<std::size_t> &rows, bool byCell) {
		std::set<std::size_t> agreeing;
		for (const std::size_t row : rows) {
			if (!in_place(registry, row, place, sheetCell, byCell)) continue;
			const RequestMappingEntry &entry = registry.symbols->mapping[row];
			if (entry.value && agrees(*entry.value, *cell.value)) agreeing.insert(row);
		}
		return agreeing;
	};
	if (const auto at = registry.by_cell.find(sheetCell); at != registry.by_cell.end()) {
		const std::set<std::size_t> agreeing = candidates(at->second, true);
		if (agreeing.size() == 1) found = *agreeing.begin();
	}
	if (!found && byWriting) {
		if (const auto at = registry.by_writing.find(trim(cell.text)); at != registry.by_writing.end()) {
			const std::set<std::size_t> agreeing = candidates(at->second, false);
			if (agreeing.size() == 1) found = *agreeing.begin();
		}
	}
	if (found) link_row(registry, *found, cell);
}

void link_name(const Registry &registry, TableCell &cell) {
	if (registry.symbols == nullptr || cell.value || cell.text.empty()) return;
	const auto at = registry.by_name.find(fold(trim(cell.text)));
	if (at == registry.by_name.end()) return;
	cell.placeholder = registry.symbols->mapping[at->second].placeholder;
	cell.source.marker = cell.placeholder;
}

bool non_empty(const TableCell &cell) {
	return cell.kind != CellKind::Empty || !cell.formula.empty() || !trim(cell.text).empty();
}

bool is_text(const TableCell &cell) { return cell.kind == CellKind::Text && !cell.value && !trim(cell.text).empty(); }

bool has_value_cell(const TableRow &row) {
	return std::any_of(row.cells.begin(), row.cells.end(), [](const TableCell &cell) { return cell.value.has_value(); });
}

TableCell *mutable_cell(TableRow &row, std::size_t column) {
	for (TableCell &cell : row.cells)
		if (cell.column == column) return &cell;
	return nullptr;
}

const TableCell *row_cell(const TableRow &row, std::size_t column) {
	for (const TableCell &cell : row.cells)
		if (cell.column == column) return &cell;
	return nullptr;
}

// A formula as its row writes it: a relative reference in the row's own row is
// "Bn", in the row above "B(n-1)"; an absolute one stays as written.
std::string row_pattern(const std::string &formula, std::size_t ownRow) {
	static const std::regex reference("(\\$?)([A-Z]{1,3})(\\$?)([0-9]{1,7})");
	std::string pattern;
	std::size_t last = 0;
	for (auto it = std::sregex_iterator(formula.begin(), formula.end(), reference); it != std::sregex_iterator(); ++it) {
		const std::smatch &match = *it;
		const std::size_t at = static_cast<std::size_t>(match.position(0));
		// A function's name ("SUM(") is not a reference.
		if (at > 0 && std::isalpha(static_cast<unsigned char>(formula[at - 1]))) continue;
		pattern += formula.substr(last, at - last);
		const std::size_t row = static_cast<std::size_t>(std::strtoul(match[4].str().c_str(), nullptr, 10));
		if (match[3].matched && !match[3].str().empty()) pattern += match.str(0);
		else if (row == ownRow) pattern += match[1].str() + match[2].str() + "n";
		else if (row + 1 == ownRow) pattern += match[1].str() + match[2].str() + "(n-1)";
		else pattern += match.str(0);
		last = at + static_cast<std::size_t>(match.length(0));
	}
	pattern += formula.substr(last);
	return pattern;
}

// What a column's shared formula does, from its pattern alone (language-free,
// DESIGN4 §6.3).
ColumnFormula classify_formula(const std::string &pattern, const std::string &ownColumn) {
	ColumnFormula formula;
	formula.pattern = pattern;
	formula.op = "formula";
	std::string body = pattern;
	if (!body.empty() && body.front() == '=') body.erase(0, 1);
	static const std::regex running("^([A-Z]{1,3})\\(n-1\\)((?:[+-][A-Z]{1,3}n)+)$");
	static const std::regex chain("^[A-Z]{1,3}n(?:([+-])[A-Z]{1,3}n)+$");
	static const std::regex product("^([A-Z]{1,3})n\\*([A-Z]{1,3})n$");
	static const std::regex quotient("^([A-Z]{1,3})n/([A-Z]{1,3})n$");
	static const std::regex scaled("^(?:([A-Z]{1,3})n\\*(\\$[A-Z]{1,3}\\$[0-9]{1,7})|(\\$[A-Z]{1,3}\\$[0-9]{1,7})\\*([A-Z]{1,3})n)$");
	static const std::regex term("([+-]?)([A-Z]{1,3})n");
	std::smatch match;
	if (std::regex_match(body, match, running) && match[1].str() == ownColumn) {
		formula.op = "running_balance";
		const std::string rest = match[2].str();
		for (auto it = std::sregex_iterator(rest.begin(), rest.end(), term); it != std::sregex_iterator(); ++it)
			formula.inputs.push_back((*it)[2].str());
		return formula;
	}
	if (std::regex_match(body, match, chain)) {
		bool minus = false;
		bool plus = false;
		std::size_t index = 0;
		for (auto it = std::sregex_iterator(body.begin(), body.end(), term); it != std::sregex_iterator(); ++it, ++index) {
			formula.inputs.push_back((*it)[2].str());
			if (index > 0) {
				if ((*it)[1].str() == "-") minus = true;
				else plus = true;
			}
		}
		formula.op = minus && !plus ? "subtract" : plus && !minus ? "add" : "formula";
		return formula;
	}
	if (std::regex_match(body, match, product)) {
		formula.op = "multiply";
		formula.inputs = {match[1].str(), match[2].str()};
		return formula;
	}
	if (std::regex_match(body, match, quotient)) {
		formula.op = "divide";
		formula.inputs = {match[1].str(), match[2].str()};
		return formula;
	}
	if (std::regex_match(body, match, scaled)) {
		formula.op = "scale";
		if (match[1].matched) formula.inputs = {match[1].str(), match[2].str()};
		else formula.inputs = {match[4].str(), match[3].str()};
		return formula;
	}
	return formula;
}

// "=SUM(B4:B18)", "=SUBTOTAL(109,B4:B11)": the function, its code, the rows.
struct RangeFormula {
	std::string function;
	int code = 0;
	std::size_t first = 0;
	std::size_t last = 0;
};

std::optional<RangeFormula> range_formula(const std::string &formula) {
	static const std::regex shape(
		"^=\\s*(SUM|SUBTOTAL|AGGREGATE)\\s*\\(\\s*(?:([0-9]{1,3})\\s*,\\s*(?:[0-9]{1,2}\\s*,\\s*)?)?\\$?[A-Z]{1,3}\\$?([0-9]{1,7})\\s*:\\s*\\$?[A-Z]{1,3}\\$?([0-9]{1,7})\\s*\\)\\s*$",
		std::regex::icase);
	std::smatch match;
	if (!std::regex_match(formula, match, shape)) return std::nullopt;
	RangeFormula reading;
	reading.function = match[1].str();
	std::transform(reading.function.begin(), reading.function.end(), reading.function.begin(),
		[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	if (match[2].matched) reading.code = std::atoi(match[2].str().c_str());
	const std::size_t a = static_cast<std::size_t>(std::strtoul(match[3].str().c_str(), nullptr, 10));
	const std::size_t b = static_cast<std::size_t>(std::strtoul(match[4].str().c_str(), nullptr, 10));
	reading.first = std::min(a, b);
	reading.last = std::max(a, b);
	return reading;
}

std::string rows_words(std::size_t first, std::size_t last) {
	return first == last ? "row " + std::to_string(first)
		: "rows " + std::to_string(first) + "\xE2\x80\x93" + std::to_string(last);
}

// The kind of a row below the header (DESIGN4 §6.3), and a total row's summary.
RowKind body_kind(const TableRow &row, bool tableHasValues, std::string &summary) {
	if (std::none_of(row.cells.begin(), row.cells.end(), non_empty)) return RowKind::Blank;
	const std::string label = fold(row.label);
	for (const TableCell &cell : row.cells) {
		if (cell.formula.empty()) continue;
		std::string upper = cell.formula;
		std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		if (upper.find("SUBTOTAL(") != std::string::npos || upper.find("AGGREGATE(") != std::string::npos) {
			if (const auto range = range_formula(cell.formula)) {
				summary = range->function + "(" + std::to_string(range->code) + ") of " + rows_words(range->first, range->last);
				if (range->function == "SUBTOTAL")
					summary += range->code >= 101 ? " (skips hidden rows)" : " (counts hidden rows)";
			} else {
				summary = "SUBTOTAL";
			}
			return RowKind::Subtotal;
		}
	}
	if (any_word(label, {"subtotal", "sub-total", "\xD0\xBF\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB5\xD0\xB6\xD1\x83\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD*"})) {
		summary = "subtotal";
		return RowKind::Subtotal;
	}
	for (const TableCell &cell : row.cells) {
		if (const auto range = range_formula(cell.formula); range && range->function == "SUM" && range->last > range->first) {
			summary = "SUM of " + rows_words(range->first, range->last);
			return RowKind::Total;
		}
	}
	if (any_word(label, {"total", "totals", "grand total", "\xD0\xB8\xD1\x82\xD0\xBE\xD0\xB3*",
			"\xD0\xB2\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE"})) {
		summary = "total";
		return RowKind::Total;
	}
	// A line of text alone below the rows of figures: a note.
	if (tableHasValues && !has_value_cell(row)
			&& std::count_if(row.cells.begin(), row.cells.end(), non_empty) == 1 && row.cells.size() >= 1)
		return RowKind::Note;
	return RowKind::Body;
}

std::string label_of(const TableRow &row, std::size_t keyColumn) {
	if (keyColumn != 0)
		if (const TableCell *key = row_cell(row, keyColumn); key != nullptr && !trim(key->text).empty()) return trim(key->text);
	for (const TableCell &cell : row.cells)
		if (cell.kind == CellKind::Text && !trim(cell.text).empty()) return trim(cell.text);
	return {};
}

// Everything after the cells are read: the header, the kinds of rows, the
// columns' types, roles and formulas, the key and its period.
void settle(ProtectedTable &table, std::size_t headerRow, const std::map<std::size_t, std::string> &headers,
		std::size_t columnCount, const std::set<std::size_t> &hiddenColumns) {
	table.header_row = headerRow;
	const bool anyValues = std::any_of(table.rows.begin(), table.rows.end(), has_value_cell);
	for (TableRow &row : table.rows) {
		row.label = label_of(row, 0);
		if (headerRow != 0 && row.number < headerRow) row.kind = RowKind::Note;
		else if (headerRow != 0 && row.number == headerRow) row.kind = RowKind::Header;
		else {
			std::string summary;
			row.kind = body_kind(row, anyValues, summary);
			if (row.kind == RowKind::Total || row.kind == RowKind::Subtotal) table.total_rows.emplace_back(row.number, summary);
		}
	}
	const auto body = [&](const TableRow &row) { return row.kind == RowKind::Body; };
	std::size_t bodyRows = static_cast<std::size_t>(std::count_if(table.rows.begin(), table.rows.end(), body));
	table.too_large = bodyRows > compute::kRowsPerTable;

	// Columns: every column a body or header cell stands in.
	std::set<std::size_t> used;
	for (const TableRow &row : table.rows)
		for (const TableCell &cell : row.cells)
			if (non_empty(cell)) used.insert(cell.column);
	for (const auto &[column, header] : headers) {
		static_cast<void>(header);
		used.insert(column);
	}
	static_cast<void>(columnCount);

	// The key: the first date column whose body rows are each one distinct
	// month, quarter or year.
	std::size_t keyColumn = 0;
	for (const std::size_t column : used) {
		std::set<long long> periods;
		std::optional<temporal::Precision> precision;
		bool every = bodyRows >= 2;
		for (const TableRow &row : table.rows) {
			if (!body(row)) continue;
			const TableCell *cell = row_cell(row, column);
			if (cell == nullptr || !cell->value || cell->value->kind != Kind::Date) { every = false; break; }
			const temporal::Precision own = cell->value->date.precision;
			if (own == temporal::Precision::Day || (precision && *precision != own)) { every = false; break; }
			precision = own;
			if (!periods.insert(cell->value->date.first_day).second) { every = false; break; }
		}
		if (every && precision) {
			keyColumn = column;
			table.key_column = column_letters(column - 1);
			table.period = precision_word(*precision);
			break;
		}
	}
	if (keyColumn != 0)
		for (TableRow &row : table.rows) row.label = label_of(row, keyColumn);

	for (const std::size_t column : used) {
		TableColumn described;
		described.letter = column_letters(column - 1);
		if (const auto header = headers.find(column); header != headers.end()) described.header = header->second;
		described.hidden = hiddenColumns.count(column) != 0;
		std::map<std::string, std::size_t> kinds;
		std::set<std::string> currencies;
		std::set<std::string> precisions;
		bool entities = true;
		bool anyText = false;
		std::size_t filled = 0;
		std::vector<std::pair<std::size_t, std::string>> patterns;
		for (const TableRow &row : table.rows) {
			if (!body(row)) continue;
			const TableCell *cell = row_cell(row, column);
			if (cell == nullptr || !non_empty(*cell)) {
				described.sparse = true;
				continue;
			}
			++filled;
			if (!cell->formula.empty()) patterns.emplace_back(row.number, row_pattern(cell->formula, row.number));
			if (!cell->value) {
				if (cell->kind == CellKind::Text) {
					anyText = true;
					if (cell->placeholder.empty()) entities = false;
					++kinds["text"];
				} else {
					described.sparse = true;
				}
				continue;
			}
			switch (cell->value->kind) {
				case Kind::Money:
					++kinds["money"];
					if (!cell->value->currency.empty()) currencies.insert(cell->value->currency);
					break;
				case Kind::Percentage: ++kinds["percentage"]; break;
				case Kind::Date:
					++kinds["date"];
					precisions.insert(precision_word(cell->value->date.precision));
					break;
				case Kind::Duration: ++kinds["duration"]; break;
				case Kind::Boolean: ++kinds["boolean"]; break;
				case Kind::Count: ++kinds["count"]; break;
				default: ++kinds["number"]; break;
			}
		}
		const std::string headerCurrency = semantic::currency_named(described.header);
		if (!headerCurrency.empty()) currencies.insert(headerCurrency);
		std::string kind;
		std::size_t best = 0;
		for (const auto &[name, count] : kinds)
			if (count > best) { best = count; kind = name; }
		// A money column's plain figures are amounts in its currency.
		if (kinds.count("money") != 0 && kind == "number") kind = "money";
		if (kind == "number" && !headerCurrency.empty()) kind = "money";
		if (kind == "money") {
			described.currency = currencies.size() == 1 ? *currencies.begin() : std::string();
			semantic::RateBasis basis = semantic::RateBasis::None;
			if (keyColumn != 0 && column != keyColumn) {
				if (table.period == "month") basis = semantic::RateBasis::PerMonth;
				else if (table.period == "year") basis = semantic::RateBasis::PerYear;
			}
			compute::ArgType type;
			type.kind = compute::TypeKind::Money;
			type.currency = described.currency.empty() ? "?" : described.currency;
			type.basis = basis;
			described.type = compute::written(type);
			for (TableRow &row : table.rows) {
				if (!body(row)) continue;
				TableCell *cell = mutable_cell(row, column);
				if (cell == nullptr || !cell->value) continue;
				// A key-value sheet's row says what its figure is ("Days overdue":
				// 40 days, not $40).
				if (cell->value->kind == Kind::Number && column != 1)
					if (const std::optional<std::string> measure = header_measure(row.label))
						if (const std::optional<compute::ArgType> measured = parse_type(*measure)) {
							if (measured->kind == compute::TypeKind::Duration)
								cell->value = semantic::make_duration(cell->value->amount, measured->unit);
							else if (measured->kind == compute::TypeKind::Count)
								cell->value->kind = Kind::Count;
							continue;
						}
				if (cell->value->kind == Kind::Number) {
					const std::string code = described.currency;
					const int decimals = cell->value->source_decimals;
					cell->value = semantic::make_money(cell->value->amount, code, decimals);
				}
				if (cell->value->kind == Kind::Money) {
					if (cell->value->currency.empty()) cell->value->currency = described.currency;
					cell->value->basis = basis;
				}
			}
		} else if (kind == "percentage") {
			described.type = "PERCENTAGE";
		} else if (kind == "date") {
			described.type = precisions.size() == 1 ? "DATE(" + *precisions.begin() + ")" : "DATE(range)";
		} else if (kind == "duration") {
			described.type = "DURATION(days)";
		} else if (kind == "boolean") {
			described.type = "BOOLEAN";
		} else if (kind == "count") {
			described.type = "COUNT";
		} else if (kind == "number") {
			const std::optional<std::string> measure = header_measure(described.header);
			described.type = measure ? *measure : "NUMBER";
		} else if (kind == "text" || (anyText && filled > 0)) {
			described.type = entities && anyText ? "ENTITY" : "WORD";
		} else {
			described.type = "WORD";
		}
		// A shared formula down the column.
		if (patterns.size() >= 2) {
			std::map<std::string, std::size_t> counts;
			for (const auto &[row, pattern] : patterns) {
				static_cast<void>(row);
				++counts[pattern];
			}
			std::string shared;
			std::size_t most = 0;
			for (const auto &[pattern, count] : counts)
				if (count > most) { most = count; shared = pattern; }
			ColumnFormula formula = classify_formula(shared, described.letter);
			// A running balance's first row starts it differently.
			const std::size_t allowed = formula.op == "running_balance" ? 1 : 0;
			formula.consistent = most >= 2 && patterns.size() - most <= allowed;
			described.formula = formula;
		}
		described.role_class = roles::role_class_of(described.header, semantic::language_of(described.header));
		if (described.role_class.empty()) described.role_class = header_role(described.header);
		table.columns.push_back(std::move(described));
	}
	// Roles from formulas, language-free: a column worked out as one column less
	// others, or carried from the row above, is a balance.
	for (TableColumn &column : table.columns) {
		if (!column.formula || !column.formula->consistent) continue;
		if (column.formula->op == "running_balance" || column.formula->op == "subtract")
			if (column.role_class.empty() || column.role_class == "PRICE" || column.role_class == "TOTAL") column.role_class = "BALANCE";
	}
	std::sort(table.columns.begin(), table.columns.end(), [](const TableColumn &a, const TableColumn &b) {
		return column_number(a.letter) < column_number(b.letter);
	});
}

// The row of `sheet` that names its columns: the last frozen row, or else the
// last row of two or more text cells before the first row holding a figure.
std::optional<std::size_t> header_index(const std::vector<TableRow> &rows, std::size_t frozenRows) {
	if (frozenRows > 0)
		for (std::size_t index = 0; index < rows.size(); ++index)
			if (rows[index].number == frozenRows && std::count_if(rows[index].cells.begin(), rows[index].cells.end(), is_text) >= 1)
				return index;
	std::optional<std::size_t> header;
	for (std::size_t index = 0; index < rows.size() && index < 24; ++index) {
		const TableRow &row = rows[index];
		const std::size_t texts = static_cast<std::size_t>(std::count_if(row.cells.begin(), row.cells.end(), is_text));
		const bool onlyText = std::all_of(row.cells.begin(), row.cells.end(), [](const TableCell &cell) {
			return !non_empty(cell) || is_text(cell);
		});
		if (has_value_cell(row)) break;
		if (onlyText && texts >= 2) header = index;
	}
	return header;
}

void finish_table(ProtectedTable &table, std::optional<std::size_t> headerAt, std::size_t columnCount,
		const std::set<std::size_t> &hiddenColumns, const std::vector<documents::SheetRenderMerge> &merges) {
	std::map<std::size_t, std::string> headers;
	std::size_t headerRow = 0;
	if (headerAt) {
		const TableRow &header = table.rows[*headerAt];
		headerRow = header.number;
		for (const TableCell &cell : header.cells)
			if (is_text(cell)) headers[cell.column] = trim(cell.text);
		// A column the header row leaves empty is named by the merged cell
		// over it, or by the row above (a two-row header).
		std::set<std::size_t> columns;
		for (const TableRow &row : table.rows)
			for (const TableCell &cell : row.cells) columns.insert(cell.column);
		for (const std::size_t column : columns) {
			if (headers.count(column) != 0) continue;
			for (const documents::SheetRenderMerge &merge : merges) {
				if (merge.first_column + 1 > column || merge.last_column + 1 < column) continue;
				if (merge.first_row + 1 > headerRow || merge.last_row + 1 < headerRow - (headerRow > 1 ? 1 : 0)) continue;
				for (const TableRow &row : table.rows)
					if (row.number == merge.first_row + 1)
						if (const TableCell *anchor = row_cell(row, merge.first_column + 1); anchor && is_text(*anchor))
							headers[column] = trim(anchor->text);
				if (headers.count(column) != 0) break;
			}
			if (headers.count(column) == 0 && *headerAt > 0) {
				const TableRow &above = table.rows[*headerAt - 1];
				if (above.number + 1 == headerRow)
					if (const TableCell *cell = row_cell(above, column); cell && is_text(*cell)) headers[column] = trim(cell->text);
			}
		}
		// A title: the one text cell of a row above the header.
		for (std::size_t index = 0; index < *headerAt; ++index) {
			const TableRow &row = table.rows[index];
			if (std::count_if(row.cells.begin(), row.cells.end(), non_empty) == 1 && !row.cells.empty()) {
				for (const TableCell &cell : row.cells)
					if (is_text(cell)) {
						table.title = trim(cell.text);
						break;
					}
				break;
			}
		}
	}
	settle(table, headerRow, headers, columnCount, hiddenColumns);
}

// One workbook cell, as the file stores it.
TableCell read_cell(const documents::SheetRenderCell &cell, std::size_t rowNumber, bool date1904,
		const std::string &sheet, const std::string &headerCurrency) {
	TableCell read;
	read.ref = cell.ref;
	read.row = rowNumber;
	read.column = cell.column + 1;
	read.text = cell.text;
	if (cell.formula) read.formula = *cell.formula;
	read.source.cell = sheet + "!" + cell.ref;
	read.source.origin = "source";
	const FormatReading format = read_format(cell.number_format);
	switch (cell.kind) {
		case documents::SheetCellKind::Empty: read.kind = CellKind::Empty; break;
		case documents::SheetCellKind::Number: {
			read.kind = CellKind::Number;
			const std::string stored = !cell.stored_value.empty() ? cell.stored_value : std::string();
			std::optional<Rational> amount = stored.empty() ? std::nullopt : stored_decimal(stored);
			if (!amount) {
				// A number the file gives only as a double: its shortest exact
				// writing is what the reading shows (17 significant digits).
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "%.17g", cell.number);
				amount = stored_decimal(buffer);
			}
			if (amount) {
				if (format.percent) read.value = semantic::make_percentage(*amount);
				else if (!format.currency.empty()) read.value = semantic::make_money(*amount, format.currency, format.decimals);
				else if (!headerCurrency.empty()) read.value = semantic::make_money(*amount, headerCurrency, format.decimals);
				else read.value = semantic::make_number(*amount);
			}
			if (read.formula.empty() == false) read.cached = stored.empty() ? read.text : stored;
			break;
		}
		case documents::SheetCellKind::Date: {
			read.kind = CellKind::Date;
			const long long day = excel_day(cell.number, date1904);
			read.value = semantic::make_date(period_of(day, date_precision(format)));
			if (!read.formula.empty()) read.cached = cell.stored_value.empty() ? read.text : cell.stored_value;
			break;
		}
		case documents::SheetCellKind::Text: {
			read.kind = CellKind::Text;
			if (!cell.raw_text.empty()) read.text = cell.raw_text;
			semantic::ParseHints hints;
			hints.currency = headerCurrency;
			const std::string text = trim(read.text);
			// A text cell that is wholly a date or an amount with its sign counts as
			// one; a bare number written as text stays text.
			if (const std::optional<Value> parsed = semantic::parse_value(text, hints);
					parsed && (parsed->kind == Kind::Date || parsed->kind == Kind::Money || parsed->kind == Kind::Percentage)
					&& text.find_first_not_of("0123456789.,- ") != std::string::npos)
				read.value = parsed;
			if (!read.formula.empty()) read.cached = read.text;
			break;
		}
		case documents::SheetCellKind::Boolean:
			read.kind = CellKind::Boolean;
			read.value = semantic::make_boolean(cell.boolean);
			if (!read.formula.empty()) read.cached = cell.boolean ? "TRUE" : "FALSE";
			break;
		case documents::SheetCellKind::Error:
			read.kind = CellKind::Error;
			if (!cell.raw_text.empty()) read.text = cell.raw_text;
			if (!read.formula.empty()) read.cached = read.text;
			break;
	}
	return read;
}

ProtectedTable sheet_table(const TableInput &input, const documents::SheetRender &sheet, bool date1904) {
	ProtectedTable table;
	table.document_id = input.document_id;
	table.document_handle = input.document_handle;
	table.sheet = sheet.name;
	table.sheet_hidden = sheet.visibility != documents::SheetVisibility::Visible;
	table.title = input.title;
	std::set<std::size_t> hiddenColumns;
	for (const documents::SheetRenderColumn &range : sheet.columns)
		if (range.hidden)
			for (std::size_t column = range.first; column <= range.last && column < range.first + 16384; ++column)
				hiddenColumns.insert(column + 1);
	// Header currencies first: a plain figure under "Amount ($)" is dollars.
	std::map<std::size_t, std::string> headerCurrency;
	for (const documents::SheetRenderRow &row : sheet.rows) {
		if (row.index >= 24) break;
		for (const documents::SheetRenderCell &cell : row.cells)
			if (cell.kind == documents::SheetCellKind::Text) {
				const std::string named = semantic::currency_named(cell.text);
				if (!named.empty()) headerCurrency[cell.column] = named;
			}
	}
	for (const documents::SheetRenderRow &row : sheet.rows) {
		TableRow read;
		read.number = row.index + 1;
		read.hidden = row.hidden;
		for (const documents::SheetRenderCell &cell : row.cells) {
			const auto currency = headerCurrency.find(cell.column);
			TableCell built = read_cell(cell, read.number, date1904, sheet.name,
				currency == headerCurrency.end() ? std::string() : currency->second);
			built.hidden_row = row.hidden;
			built.hidden_column = hiddenColumns.count(built.column) != 0;
			read.cells.push_back(std::move(built));
		}
		table.rows.push_back(std::move(read));
	}
	const std::optional<std::size_t> header = header_index(table.rows, sheet.frozen_rows);
	finish_table(table, header, sheet.column_count, hiddenColumns, sheet.merges);
	return table;
}

// A table given as text by row (a Word table, a CSV): letters by position,
// rows numbered from 1, values read from the text.
ProtectedTable text_table(const TableInput &input, const std::string &name, const std::vector<std::vector<std::string>> &grid) {
	ProtectedTable table;
	table.document_id = input.document_id;
	table.document_handle = input.document_handle;
	table.sheet = name;
	table.title = input.title;
	std::map<std::size_t, std::string> headerCurrency;
	if (!grid.empty())
		for (std::size_t column = 0; column < grid.front().size(); ++column) {
			const std::string named = semantic::currency_named(grid.front()[column]);
			if (!named.empty()) headerCurrency[column + 1] = named;
		}
	for (std::size_t index = 0; index < grid.size(); ++index) {
		TableRow row;
		row.number = index + 1;
		for (std::size_t column = 0; column < grid[index].size(); ++column) {
			TableCell cell;
			cell.row = row.number;
			cell.column = column + 1;
			cell.ref = column_letters(column) + std::to_string(row.number);
			cell.text = trim(grid[index][column]);
			cell.source.cell = name + "!" + cell.ref;
			cell.source.origin = "source";
			if (cell.text.empty()) {
				cell.kind = CellKind::Empty;
			} else {
				semantic::ParseHints hints;
				if (const auto currency = headerCurrency.find(cell.column); currency != headerCurrency.end())
					hints.currency = currency->second;
				cell.value = semantic::parse_value(cell.text, hints);
				if (cell.value && cell.value->kind == Kind::DayOfYear) cell.value.reset();
				cell.kind = !cell.value ? CellKind::Text
					: cell.value->kind == Kind::Date ? CellKind::Date
					: cell.value->kind == Kind::Boolean ? CellKind::Boolean : CellKind::Number;
			}
			row.cells.push_back(std::move(cell));
		}
		table.rows.push_back(std::move(row));
	}
	std::optional<std::size_t> header;
	if (!table.rows.empty()) {
		const TableRow &first = table.rows.front();
		const std::size_t texts = static_cast<std::size_t>(std::count_if(first.cells.begin(), first.cells.end(), is_text));
		if (texts >= 2 && !has_value_cell(first)) header = 0;
	}
	finish_table(table, header, grid.empty() ? 0 : grid.front().size(), {}, {});
	return table;
}

// Links each cell of a built table to its registry row (A3/A4): a workbook's by
// the cell it is, a table read from prose by its writing too.
void link_table(const Registry &registry, const Place &place, bool byWriting, ProtectedTable &table) {
	for (TableRow &row : table.rows)
		for (TableCell &cell : row.cells) {
			if (cell.value) link_cell(registry, place, table.sheet + "!" + cell.ref, byWriting, cell);
			else if (cell.kind == CellKind::Text) link_name(registry, cell);
		}
}

// Described: the columns its linked cells stand in, the key and label
// columns, and every column their formulas read.
void describe_columns(ProtectedTable &table, const std::set<std::string> &touched) {
	std::set<std::string> columns(table.described_columns.begin(), table.described_columns.end());
	columns.insert(touched.begin(), touched.end());
	if (!table.key_column.empty()) columns.insert(table.key_column);
	// The label column: the first text column.
	for (const TableColumn &column : table.columns)
		if (column.type == "WORD" || column.type == "ENTITY") {
			columns.insert(column.letter);
			break;
		}
	bool grew = true;
	while (grew) {
		grew = false;
		for (const TableColumn &column : table.columns) {
			if (columns.count(column.letter) == 0 || !column.formula) continue;
			for (const std::string &input : column.formula->inputs) {
				const auto split = split_ref(input);
				const std::string letter = split ? split->first : input;
				if (column_of(table, letter) != nullptr && columns.insert(letter).second) grew = true;
			}
		}
	}
	table.described_columns.assign(columns.begin(), columns.end());
	std::sort(table.described_columns.begin(), table.described_columns.end(),
		[](const std::string &a, const std::string &b) { return column_number(a) < column_number(b); });
}

void describe_all(ProtectedTable &table) {
	table.described = true;
	table.described_columns.clear();
	for (const TableColumn &column : table.columns) table.described_columns.push_back(column.letter);
}

std::string strip_brackets(const std::string &handle) {
	if (handle.size() > 4 && handle.compare(0, 2, "[[") == 0 && handle.compare(handle.size() - 2, 2, "]]") == 0)
		return handle.substr(2, handle.size() - 4);
	return handle;
}

// A passage of the evidence and the document its SOURCE marker names ("DOC_3"),
// empty when it carries none.
struct EvidencePassage {
	std::string document;
	std::string text;
};

// The length of "[SOURCE:DOC_<n>:CHUNK_<n>]\n" at `at` (the marker
// SourceMarkerRegistry writes), its "DOC_<n>" into `document`; 0 otherwise.
std::size_t source_marker(const std::string &text, std::size_t at, std::string &document) {
	static const std::string prefix = "[SOURCE:DOC_";
	static const std::string middle = ":CHUNK_";
	if (text.compare(at, prefix.size(), prefix) != 0) return 0;
	std::size_t cursor = at + prefix.size();
	const std::size_t first = cursor;
	while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9' && cursor - first < 7) ++cursor;
	if (cursor == first) return 0;
	const std::string handle = text.substr(at + 8, cursor - at - 8);
	if (text.compare(cursor, middle.size(), middle) != 0) return 0;
	cursor += middle.size();
	const std::size_t chunk = cursor;
	while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9' && cursor - chunk < 7) ++cursor;
	if (cursor == chunk || cursor + 1 >= text.size() || text[cursor] != ']' || text[cursor + 1] != '\n') return 0;
	document = handle;
	return cursor + 2 - at;
}

// The evidence as passages, each under the document its marker names: a
// passage given with its marker line, or a SOURCE_MATERIAL run of them, is
// split at every marker; text before any marker is a passage of no document.
std::vector<EvidencePassage> split_evidence(const std::vector<std::string> &evidence) {
	std::vector<EvidencePassage> passages;
	for (const std::string &text : evidence) {
		std::string document;
		std::size_t start = 0;
		std::size_t search = 0;
		const auto emit = [&](std::size_t end) {
			std::string body = text.substr(start, end - start);
			if (const std::size_t closing = body.find("\n[/SOURCE]"); closing != std::string::npos) body.erase(closing);
			if (body.find_first_not_of(" \t\r\n") != std::string::npos) passages.push_back({document, std::move(body)});
		};
		while (true) {
			const std::size_t found = text.find("[SOURCE:DOC_", search);
			if (found == std::string::npos) break;
			std::string handle;
			const std::size_t length = found == 0 || text[found - 1] == '\n' ? source_marker(text, found, handle) : 0;
			if (length == 0) {
				search = found + 1;
				continue;
			}
			emit(found);
			document = handle;
			start = found + length;
			search = start;
		}
		emit(text.size());
	}
	return passages;
}

bool of_document(const ProtectedTable &table, const std::string &document) {
	return !document.empty() && (table.document_handle == document || table.document_id == document);
}

} // namespace

// ---------------------------------------------------------------------------
// Building

std::vector<ProtectedTable> build_tables(const std::vector<TableInput> &inputs, const OperationProtectedSymbolTable &symbols,
		const std::vector<std::string> &evidence) {
	const Registry registry(&symbols);
	std::vector<ProtectedTable> built;
	std::vector<bool> byWriting;
	for (const TableInput &input : inputs) {
		if (input.workbook)
			for (const documents::SheetRender &sheet : input.workbook->sheets) {
				ProtectedTable table = sheet_table(input, sheet, input.workbook->date1904);
				if (table.rows.empty()) continue;
				built.push_back(std::move(table));
				byWriting.push_back(false);
			}
		for (const WordTable &word : input.word_tables) {
			if (word.rows.size() < 2) continue;
			built.push_back(text_table(input, word.table_id.empty() ? "table" : word.table_id, word.rows));
			byWriting.push_back(true);
		}
		if (input.csv.size() >= 2) {
			built.push_back(text_table(input, "csv", input.csv));
			byWriting.push_back(true);
		}
	}
	// Where each table stands: its document id, and the scopes its own
	// evidence passages were filed under (document_of_passage, by digest).
	std::map<std::string, std::size_t> cellTables;
	for (const ProtectedTable &table : built) {
		std::set<std::string> places;
		for (const TableRow &row : table.rows)
			for (const TableCell &cell : row.cells)
				if (cell.value) places.insert(table.sheet + "!" + cell.ref);
		for (const std::string &place : places) ++cellTables[place];
	}
	std::vector<Place> placeOf(built.size());
	const std::vector<EvidencePassage> passages = split_evidence(evidence);
	for (std::size_t index = 0; index < built.size(); ++index) {
		Place &place = placeOf[index];
		place.cell_tables = &cellTables;
		if (!built[index].document_id.empty()) place.scopes.insert("d:" + built[index].document_id);
		for (const EvidencePassage &passage : passages) {
			if (!of_document(built[index], passage.document)) continue;
			const auto scope = symbols.document_of_passage.find(singularity::platform::sha256_hex_digest(passage.text));
			if (scope == symbols.document_of_passage.end() || scope->second.empty()) continue;
			place.scopes.insert("d:" + scope->second);
			place.evidence_scoped = true;
		}
	}
	for (std::size_t index = 0; index < built.size(); ++index) link_table(registry, placeOf[index], byWriting[index], built[index]);
	std::size_t number = symbols.tables.size();
	for (ProtectedTable &table : built) {
		table.handle = "Table" + std::to_string(++number);
		// What the evidence touched through the registry: every cell a reading
		// of the prompt produced a fact for.
		std::set<std::string> touched;
		for (const TableRow &row : table.rows)
			for (const TableCell &cell : row.cells)
				if (!cell.fact_key.empty()) touched.insert(column_letters(cell.column - 1));
		if (!touched.empty()) {
			table.described = true;
			describe_columns(table, touched);
		}
	}
	if (!evidence.empty()) mark_evidence(built, evidence);
	return built;
}

void relink_tables(OperationProtectedSymbolTable &symbols) {
	if (symbols.tables.empty()) return;
	const Registry registry(&symbols);
	std::map<std::string, std::size_t> cellTables;
	for (const ProtectedTable &table : symbols.tables) {
		std::set<std::string> places;
		for (const TableRow &row : table.rows)
			for (const TableCell &cell : row.cells)
				if (cell.value) places.insert(table.sheet + "!" + cell.ref);
		for (const std::string &place : places) ++cellTables[place];
	}
	for (ProtectedTable &table : symbols.tables) {
		Place place;
		place.cell_tables = &cellTables;
		if (!table.document_id.empty()) place.scopes.insert("d:" + table.document_id);
		for (TableRow &row : table.rows)
			for (TableCell &cell : row.cells)
				if (cell.value && cell.fact_key.empty()) link_cell(registry, place, table.sheet + "!" + cell.ref, false, cell);
	}
}

void mark_evidence(std::vector<ProtectedTable> &tables, const std::vector<std::string> &evidence) {
	const std::vector<EvidencePassage> passages = split_evidence(evidence);
	std::vector<std::string> texts;
	texts.reserve(passages.size());
	for (const EvidencePassage &passage : passages) texts.push_back(passage.text);
	// One row read is enough to touch a sheet - of the passage's own document
	// when its marker names one; a passage that names none touches a sheet of
	// that name only when one table has it.
	for (const PassageSheet &sheet : passage_sheets(texts, 64, compute::kRowsPerTable, 1)) {
		if (sheet.name.empty()) continue;
		const std::string &document = passages[sheet.passage].document;
		std::set<std::string> touched;
		for (const PassageRow &row : sheet.rows)
			for (const PassageCell &cell : row.cells) touched.insert(cell.column);
		std::vector<ProtectedTable *> sheets;
		for (ProtectedTable &table : tables) {
			if (table.sheet != sheet.name) continue;
			if (!document.empty() && !of_document(table, document)) continue;
			sheets.push_back(&table);
		}
		if (document.empty() && sheets.size() != 1) continue;
		for (ProtectedTable *table : sheets) {
			table->described = true;
			describe_columns(*table, touched);
		}
	}
	// A Word or CSV table the prose shows: two of its cells' words stand in a
	// passage of its own document (or of none).
	for (ProtectedTable &table : tables) {
		if (table.described || (table.sheet.rfind("table", 0) != 0 && table.sheet != "csv")) continue;
		std::size_t seen = 0;
		for (const TableRow &row : table.rows) {
			for (const TableCell &cell : row.cells) {
				if (cell.text.size() < 3) continue;
				for (const EvidencePassage &passage : passages) {
					if (!passage.document.empty() && !of_document(table, passage.document)) continue;
					if (passage.text.find(cell.text) != std::string::npos) {
						++seen;
						break;
					}
				}
			}
			if (seen >= 2) break;
		}
		if (seen >= 2) describe_all(table);
	}
}

std::vector<ProtectedTable> tables_in_passages(const std::vector<std::string> &raw_passages) {
	std::vector<ProtectedTable> built;
	for (const PassageSheet &sheet : passage_sheets(raw_passages, 6, compute::kRowsPerTable)) {
		ProtectedTable table;
		table.sheet = sheet.name.empty() ? "passage " + std::to_string(sheet.passage + 1) : sheet.name;
		table.document_id = "passage:" + std::to_string(sheet.passage);
		table.sheet_hidden = sheet.sheet_hidden;
		std::vector<PassageRow> rows = sheet.rows;
		std::sort(rows.begin(), rows.end(), [](const PassageRow &a, const PassageRow &b) { return a.number < b.number; });
		for (const PassageRow &passageRow : rows) {
			TableRow row;
			row.number = passageRow.number;
			row.hidden = passageRow.hidden;
			for (const PassageCell &piece : passageRow.cells) {
				TableCell cell;
				cell.ref = piece.ref;
				cell.row = piece.row;
				cell.column = column_number(piece.column);
				cell.text = piece.raw;
				cell.formula = piece.formula;
				cell.hidden_row = passageRow.hidden;
				cell.source.cell = table.sheet + "!" + piece.ref;
				cell.source.origin = "source";
				semantic::ParseHints hints;
				hints.currency = semantic::currency_named(piece.note);
				if (!hints.currency.empty()) hints.expect = Kind::Money;
				if (piece.note.find("percent") != std::string::npos) hints.percent_display = true;
				if (!piece.raw.empty()) {
					cell.value = semantic::parse_value(piece.raw, hints);
					if (!cell.value) cell.value = semantic::parse_value(piece.raw);
					if (cell.value && cell.value->kind == Kind::DayOfYear) cell.value.reset();
				}
				cell.kind = piece.raw.empty() ? CellKind::Empty
					: !cell.value ? CellKind::Text
					: cell.value->kind == Kind::Date ? CellKind::Date : CellKind::Number;
				if (!cell.formula.empty()) cell.cached = piece.raw;
				row.cells.push_back(std::move(cell));
			}
			std::sort(row.cells.begin(), row.cells.end(), [](const TableCell &a, const TableCell &b) { return a.column < b.column; });
			table.rows.push_back(std::move(row));
		}
		const std::optional<std::size_t> header = header_index(table.rows, 0);
		finish_table(table, header, 0, {}, {});
		describe_all(table);
		built.push_back(std::move(table));
	}
	std::size_t number = 0;
	for (ProtectedTable &table : built) table.handle = "Table" + std::to_string(++number);
	return built;
}

// ---------------------------------------------------------------------------
// Reading the model

const ProtectedTable *find_table(const OperationProtectedSymbolTable &table, const std::string &handle) {
	const std::string wanted = strip_brackets(handle);
	for (const ProtectedTable &candidate : table.tables)
		if (candidate.handle == wanted) return &candidate;
	return nullptr;
}

ProtectedTable *find_table(OperationProtectedSymbolTable &table, const std::string &handle) {
	const std::string wanted = strip_brackets(handle);
	for (ProtectedTable &candidate : table.tables)
		if (candidate.handle == wanted) return &candidate;
	return nullptr;
}

const ProtectedSchedule *find_schedule(const OperationProtectedSymbolTable &table, const std::string &handle) {
	const std::string wanted = strip_brackets(handle);
	for (const ProtectedSchedule &candidate : table.schedules)
		if (candidate.handle == wanted) return &candidate;
	return nullptr;
}

const TableColumn *column_of(const ProtectedTable &table, const std::string &letter) {
	for (const TableColumn &column : table.columns)
		if (column.letter == letter) return &column;
	return nullptr;
}

const TableRow *row_of(const ProtectedTable &table, std::size_t number) {
	for (const TableRow &row : table.rows)
		if (row.number == number) return &row;
	return nullptr;
}

const TableCell *cell_of(const ProtectedTable &table, std::size_t row, const std::string &letter) {
	const TableRow *found = row_of(table, row);
	if (found == nullptr) return nullptr;
	return row_cell(*found, column_number(letter));
}

const TableCell *cell_of(const ProtectedTable &table, const std::string &ref) {
	const auto split = split_ref(ref);
	if (!split) return nullptr;
	return cell_of(table, split->second, split->first);
}

const DerivedColumn *derived_column(const ProtectedTable &table, const std::string &label) {
	const std::string wanted = !label.empty() && label.front() == '@' ? label.substr(1) : label;
	for (const DerivedColumn &column : table.derived_columns)
		if (column.label == wanted) return &column;
	return nullptr;
}

// A place's key as a digest: a sheet's name is the material's words, and keys
// are kept (the matter's observation counts) where no words belong.
std::string place_key(const ProtectedTable &table) {
	return singularity::platform::sha256_hex_digest(table.document_id + "\x1F" + table.sheet).substr(0, 16);
}

std::string cell_root(const ProtectedTable &table, const TableCell &cell) {
	if (!cell.fact_key.empty()) return cell.fact_key;
	return "cell:" + place_key(table) + ":" + cell.ref;
}

std::string group_key(const ProtectedTable &table, const std::string &letter) {
	return "column:" + place_key(table) + ":" + letter;
}

std::optional<compute::ArgType> parse_type(const std::string &written) {
	compute::ArgType type;
	const std::size_t open = written.find('(');
	const std::string head = written.substr(0, open);
	std::string inside;
	if (open != std::string::npos) {
		if (written.back() != ')') return std::nullopt;
		inside = written.substr(open + 1, written.size() - open - 2);
	}
	const auto basis_of = [](const std::string &word) {
		if (word == "day") return semantic::RateBasis::PerDay;
		if (word == "month") return semantic::RateBasis::PerMonth;
		if (word == "year") return semantic::RateBasis::PerYear;
		return semantic::RateBasis::None;
	};
	if (head == "MONEY") {
		type.kind = compute::TypeKind::Money;
		const std::size_t slash = inside.find('/');
		type.currency = inside.substr(0, slash);
		if (type.currency.empty()) type.currency = "?";
		if (slash != std::string::npos) {
			type.basis = basis_of(inside.substr(slash + 1));
			if (type.basis == semantic::RateBasis::None) return std::nullopt;
		}
		return type;
	}
	if (head == "NUMBER") { type.kind = compute::TypeKind::Number; return type; }
	if (head == "COUNT") { type.kind = compute::TypeKind::Count; return type; }
	if (head == "PERCENTAGE") { type.kind = compute::TypeKind::Percentage; return type; }
	if (head == "RATE") {
		type.kind = compute::TypeKind::Rate;
		type.basis = basis_of(inside);
		return type;
	}
	if (head == "DATE") {
		type.kind = compute::TypeKind::Date;
		type.precision = inside;
		return type;
	}
	if (head == "DURATION") {
		type.kind = compute::TypeKind::Duration;
		static const std::map<std::string, semantic::DurationUnit> units{{"days", semantic::DurationUnit::Day},
			{"weeks", semantic::DurationUnit::Week}, {"months", semantic::DurationUnit::Month},
			{"years", semantic::DurationUnit::Year}, {"business days", semantic::DurationUnit::BusinessDay},
			{"quarters", semantic::DurationUnit::Quarter}, {"hours", semantic::DurationUnit::Hour},
			{"minutes", semantic::DurationUnit::Minute}};
		const auto unit = units.find(inside);
		if (unit == units.end()) return std::nullopt;
		type.unit = unit->second;
		return type;
	}
	if (head == "BOOLEAN") { type.kind = compute::TypeKind::Boolean; return type; }
	if (head == "WORD") { type.kind = compute::TypeKind::Word; return type; }
	if (head == "ENTITY") {
		type.kind = compute::TypeKind::Entity;
		type.entity = inside;
		return type;
	}
	return std::nullopt;
}

std::vector<std::pair<std::size_t, std::string>> row_labels(const ProtectedTable &table, const std::vector<std::size_t> &rows,
		std::size_t limit) {
	std::vector<std::size_t> sorted = rows;
	std::sort(sorted.begin(), sorted.end());
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	std::vector<std::pair<std::size_t, std::string>> labels;
	for (const std::size_t number : sorted) {
		if (labels.size() >= limit) break;
		const TableRow *row = row_of(table, number);
		labels.emplace_back(number, row == nullptr ? std::string() : row->label);
	}
	return labels;
}

std::vector<BlockLine> describe(const ProtectedTable &table) {
	std::vector<BlockLine> lines;
	const auto control = [](std::string text) { return BlockPart{false, std::move(text)}; };
	const auto data = [](std::string text) { return BlockPart{true, std::move(text)}; };
	std::size_t firstBody = 0;
	std::size_t lastBody = 0;
	std::vector<std::size_t> hidden;
	for (const TableRow &row : table.rows) {
		if (row.kind != RowKind::Body) continue;
		if (firstBody == 0) firstBody = row.number;
		lastBody = row.number;
		if (row.hidden) hidden.push_back(row.number);
	}
	BlockLine head;
	head.push_back(control("[[" + table.handle + "]] "));
	if (!table.title.empty()) {
		head.push_back(control("\""));
		head.push_back(data(table.title));
		head.push_back(control("\" "));
	}
	head.push_back(control("(" + (table.document_handle.empty() ? std::string("document") : table.document_handle) + ", sheet \""));
	head.push_back(data(table.sheet));
	head.push_back(control(std::string("\"") + (table.sheet_hidden ? ", hidden sheet" : "") + "):"));
	std::string shape;
	if (table.header_row != 0) shape += " header row " + std::to_string(table.header_row) + ";";
	if (firstBody != 0) {
		shape += " body rows " + std::to_string(firstBody) + "\xE2\x80\x93" + std::to_string(lastBody);
		if (!hidden.empty()) {
			shape += " (row" + std::string(hidden.size() == 1 ? " " : "s ");
			for (std::size_t index = 0; index < hidden.size(); ++index)
				shape += (index == 0 ? "" : ", ") + std::to_string(hidden[index]);
			shape += " hidden)";
		}
	}
	for (const auto &[row, summary] : table.total_rows) {
		static_cast<void>(summary);
		shape += "; total row " + std::to_string(row);
	}
	head.push_back(control(shape));
	lines.push_back(std::move(head));
	BlockLine columns;
	for (const std::string &letter : table.described_columns) {
		const TableColumn *column = column_of(table, letter);
		if (column == nullptr) continue;
		if (!columns.empty()) columns.push_back(control(" \xC2\xB7 "));
		columns.push_back(control(column->letter + " \""));
		columns.push_back(data(column->header));
		std::string tail = "\" " + column->type;
		if (!column->role_class.empty()) tail += " " + column->role_class;
		if (column->hidden) tail += " (hidden column)";
		if (column->formula && column->formula->consistent) {
			if (column->formula->op == "subtract" && !column->formula->inputs.empty()) {
				tail += " = " + column->formula->inputs.front();
				for (std::size_t index = 1; index < column->formula->inputs.size(); ++index)
					tail += " \xE2\x88\x92 " + column->formula->inputs[index];
				tail += " (formula)";
			} else if (column->formula->op == "running_balance") {
				tail += " = previous row + formula (running balance)";
			} else {
				tail += " (formula: " + column->formula->op + ")";
			}
		}
		columns.push_back(control(tail));
	}
	if (!columns.empty()) {
		columns.insert(columns.begin(), control("  "));
		lines.push_back(std::move(columns));
	}
	for (const auto &[row, summary] : table.total_rows) {
		BlockLine total;
		total.push_back(control("  row " + std::to_string(row) + " = " + summary));
		lines.push_back(std::move(total));
	}
	BlockLine refs;
	refs.push_back(control("  cells by reference: [[" + table.handle + "]].<column><row>"));
	lines.push_back(std::move(refs));
	return lines;
}

std::vector<std::pair<std::string, std::size_t>> undescribed_sheets(const std::vector<ProtectedTable> &tables) {
	std::vector<std::pair<std::string, std::size_t>> counts;
	for (const ProtectedTable &table : tables) {
		if (table.described) continue;
		const std::string document = table.document_handle.empty() ? table.document_id : table.document_handle;
		auto found = std::find_if(counts.begin(), counts.end(), [&](const auto &entry) { return entry.first == document; });
		if (found == counts.end()) counts.emplace_back(document, 1);
		else ++found->second;
	}
	return counts;
}

// ---------------------------------------------------------------------------
// Operands

namespace {

compute::ArgType table_type(const ProtectedTable &table, compute::TypeKind kind) {
	compute::ArgType type;
	type.kind = kind;
	type.table = table.handle;
	return type;
}

bool fail(compute::TypeError &error, compute::Code code, std::string hint = {}) {
	error.code = code;
	error.hint = std::move(hint);
	return true;
}

// A row set, group set or derived column the model named, anywhere.
template <typename Table>
bool find_named(Table &tables, const std::string &key, const RowSet *&rows, const GroupSet *&groups,
		const DerivedColumn *&column, const ProtectedTable *&owner) {
	for (const ProtectedTable &table : tables) {
		if (const auto found = table.row_sets.find(key); found != table.row_sets.end()) {
			rows = &found->second;
			owner = &table;
			return true;
		}
		if (const auto found = table.group_sets.find(key); found != table.group_sets.end()) {
			groups = &found->second;
			owner = &table;
			return true;
		}
		if (!key.empty() && key.front() == '@')
			if (const DerivedColumn *derived = derived_column(table, key)) {
				column = derived;
				owner = &table;
				return true;
			}
	}
	return false;
}

bool fill_named(const OperationProtectedSymbolTable &symbols, const std::string &key, compute::Operand &operand) {
	const RowSet *rows = nullptr;
	const GroupSet *groups = nullptr;
	const DerivedColumn *column = nullptr;
	const ProtectedTable *owner = nullptr;
	if (!find_named(symbols.tables, key, rows, groups, column, owner)) return false;
	operand.table = owner;
	if (rows != nullptr) {
		operand.rows = *rows;
		operand.type = table_type(*owner, compute::TypeKind::RowSet);
		operand.roots.insert(rows->roots.begin(), rows->roots.end());
	} else if (groups != nullptr) {
		operand.groups = *groups;
		operand.type = table_type(*owner, compute::TypeKind::GroupSet);
		const TableColumn *key_column = column_of(*owner, groups->key_column);
		operand.type.key = key_column == nullptr ? std::string("WORD") : key_column->type;
	} else {
		operand.type = table_type(*owner, compute::TypeKind::Column);
		operand.type.element = column->type;
	}
	return true;
}

} // namespace

bool resolve_arg(const OperationProtectedSymbolTable &symbols, const compute::Arg &arg, compute::Operand &operand,
		compute::TypeError &error) {
	operand.arg = arg;
	const auto usable = [&](const ProtectedTable *table) -> bool {
		if (table == nullptr || !table->described) {
			fail(error, compute::Code::UnknownSymbol, "a table is named by the handle the tables block gives it");
			return false;
		}
		if (table->too_large) {
			fail(error, compute::Code::TableTooLarge);
			return false;
		}
		return true;
	};
	switch (arg.kind) {
		case compute::ArgKind::Table: {
			const ProtectedTable *table = find_table(symbols, arg.symbol);
			if (!usable(table)) return true;
			operand.table = table;
			operand.type = table_type(*table, compute::TypeKind::Table);
			return true;
		}
		case compute::ArgKind::Column: {
			const ProtectedTable *table = find_table(symbols, arg.symbol);
			if (!usable(table)) return true;
			operand.table = table;
			const TableColumn *column = column_of(*table, arg.column);
			if (column == nullptr
					|| std::find(table->described_columns.begin(), table->described_columns.end(), arg.column)
						== table->described_columns.end())
				return fail(error, compute::Code::UnknownColumn, "a column is named by a letter the tables block lists");
			operand.type = table_type(*table, compute::TypeKind::Column);
			operand.type.element = column->type;
			return true;
		}
		case compute::ArgKind::Row: {
			const ProtectedTable *table = find_table(symbols, arg.symbol);
			if (!usable(table)) return true;
			const TableRow *row = row_of(*table, arg.row);
			if (row == nullptr || row->kind == RowKind::Header || row->kind == RowKind::Note)
				return fail(error, compute::Code::UnknownSymbol, "a row is named by its number in the sheet");
			operand.table = table;
			RowSet one;
			one.table = table->handle;
			one.certain = {arg.row};
			one.structural = true;
			one.base_rows = {arg.row};
			one.base_size = 1;
			one.canonical = table->handle + ".r" + std::to_string(arg.row);
			operand.rows = one;
			operand.type = table_type(*table, compute::TypeKind::RowSet);
			return true;
		}
		case compute::ArgKind::CellRef: {
			const ProtectedTable *table = find_table(symbols, arg.symbol);
			if (!usable(table)) return true;
			const TableColumn *column = column_of(*table, arg.column);
			if (column == nullptr
					|| std::find(table->described_columns.begin(), table->described_columns.end(), arg.column)
						== table->described_columns.end())
				return fail(error, compute::Code::UnknownColumn, "a column is named by a letter the tables block lists");
			const TableCell *cell = cell_of(*table, arg.row, arg.column);
			// A formula cell saved as an error (#DIV/0!) holds no figure but can
			// still be named: recalc answers evaluation_failed for it (§6.6).
			if (cell == nullptr || (!cell->value && (cell->formula.empty() || cell->kind != CellKind::Error)))
				return fail(error, compute::Code::UnknownSymbol, "that cell holds no figure");
			operand.table = table;
			operand.value = cell->value;
			const std::optional<compute::ArgType> type = parse_type(column->type);
			operand.type = type ? *type : compute::ArgType{};
			// A key-value sheet's cell is what it holds: a rate shown as a
			// percentage, or days by its row's label, in a column of amounts.
			if (cell->value
					&& ((cell->value->kind == semantic::Kind::Percentage && operand.type.kind != compute::TypeKind::Percentage
							&& operand.type.kind != compute::TypeKind::Rate)
						|| (cell->value->kind == semantic::Kind::Duration && operand.type.kind != compute::TypeKind::Duration)
						|| (cell->value->kind == semantic::Kind::Count && operand.type.kind != compute::TypeKind::Count)))
				operand.type = operations::type_of_value(*cell->value);
			// A total row of a column owed per period holds the total.
			if (const TableRow *row = row_of(*table, arg.row);
					row != nullptr && (row->kind == RowKind::Total || row->kind == RowKind::Subtotal))
				operand.type.basis = operand.type.kind == compute::TypeKind::Money ? semantic::RateBasis::None : operand.type.basis;
			operand.fact_key = cell_root(*table, *cell);
			operand.roots = {operand.fact_key};
			if (!cell->fact_key.empty())
				if (const auto row = symbols.fact_by_key.find(cell->fact_key);
						row != symbols.fact_by_key.end() && row->second < symbols.mapping.size())
					operand.row = &symbols.mapping[row->second];
			return true;
		}
		case compute::ArgKind::Schedule: {
			const ProtectedSchedule *schedule = find_schedule(symbols, arg.symbol);
			if (schedule == nullptr) return fail(error, compute::Code::UnknownSymbol);
			operand.schedule = schedule;
			operand.type.kind = compute::TypeKind::Schedule;
			operand.type.key = schedule->key_type;
			operand.type.element = schedule->value_type;
			for (const ScheduleBand &band : schedule->bands)
				for (const std::string *fact : {&band.lower_fact, &band.upper_fact, &band.value_fact})
					if (!fact->empty()) operand.roots.insert(*fact);
			return true;
		}
		case compute::ArgKind::Document: {
			bool known = false;
			for (const ProtectedTable &table : symbols.tables)
				known = known || table.document_handle == arg.symbol || table.document_id == arg.symbol;
			if (!known) return fail(error, compute::Code::UnknownSymbol);
			operand.type.kind = compute::TypeKind::Word;
			operand.type.element = "document";
			return true;
		}
		case compute::ArgKind::Symbol: {
			const std::string inner = strip_brackets(arg.symbol);
			if (inner.rfind("Table", 0) == 0) {
				const ProtectedTable *table = find_table(symbols, inner);
				if (!usable(table)) return true;
				operand.table = table;
				operand.type = table_type(*table, compute::TypeKind::Table);
				return true;
			}
			if (inner.rfind("Schedule", 0) == 0) {
				compute::Arg schedule = arg;
				schedule.kind = compute::ArgKind::Schedule;
				return resolve_arg(symbols, schedule, operand, error);
			}
			if (inner.rfind("Rows", 0) == 0 || inner.rfind("Groups", 0) == 0) {
				if (!fill_named(symbols, inner, operand)) return fail(error, compute::Code::UnknownSymbol);
				return true;
			}
			return false;
		}
		case compute::ArgKind::Ref: {
			const std::string label = arg.label.empty() ? arg.written : "@" + arg.label;
			if (fill_named(symbols, label, operand)) return true;
			if (const auto symbol = symbols.compute_labels.find(arg.label); symbol != symbols.compute_labels.end()) {
				const std::string inner = strip_brackets(symbol->second);
				if ((inner.rfind("Rows", 0) == 0 || inner.rfind("Groups", 0) == 0) && fill_named(symbols, inner, operand))
					return true;
			}
			return false;
		}
		case compute::ArgKind::Calendar:
		case compute::ArgKind::Constant:
			return false;
	}
	return false;
}

void remember(OperationProtectedSymbolTable &symbols, const std::string &symbol, const std::string &label,
		const compute::OpAnswer &answer) {
	const std::string inner = strip_brackets(symbol);
	if (answer.rows) {
		ProtectedTable *table = find_table(symbols, answer.rows->table);
		if (table == nullptr) return;
		if (!inner.empty()) table->row_sets[inner] = *answer.rows;
		if (!label.empty()) table->row_sets["@" + label] = *answer.rows;
	}
	if (answer.groups) {
		ProtectedTable *table = find_table(symbols, answer.groups->table);
		if (table == nullptr) return;
		if (!inner.empty()) table->group_sets[inner] = *answer.groups;
		if (!label.empty()) table->group_sets["@" + label] = *answer.groups;
	}
}

// ---------------------------------------------------------------------------
// Workbook lines in passages

std::optional<PassageCell> parse_passage_cell(const std::string &piece) {
	static const std::regex shape("^\\s*\\$?([A-Z]{1,3})\\$?([0-9]{1,7}) = (.*)$");
	std::smatch match;
	if (!std::regex_match(piece, match, shape)) return std::nullopt;
	PassageCell cell;
	cell.column = match[1].str();
	cell.row = static_cast<unsigned>(std::strtoul(match[2].str().c_str(), nullptr, 10));
	cell.ref = cell.column + match[2].str();
	std::string value = match[3].str();
	const std::size_t formula = value.rfind(" {=");
	if (formula != std::string::npos && !value.empty() && value.back() == '}') {
		cell.formula = value.substr(formula + 2, value.size() - formula - 3);
		value.erase(formula);
	}
	const std::size_t shown = value.find(" (shown");
	if (shown != std::string::npos) {
		cell.note = value.substr(shown + 1);
		value.erase(shown);
	}
	for (const char *typeNote : {" (date)", " (number)", " (text)"}) {
		const std::string suffix(typeNote);
		if (value.size() > suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0)
			value.erase(value.size() - suffix.size());
	}
	while (!value.empty() && value.back() == ' ') value.pop_back();
	cell.raw = value;
	return cell;
}

std::vector<PassageSheet> passage_sheets(const std::vector<std::string> &passages, std::size_t max_tables,
		std::size_t max_rows, std::size_t min_rows) {
	static const std::regex rowPrefix("^Row ([0-9]{1,7})( \\(hidden\\))?: (.*)$");
	static const std::regex sheetLine("^Sheet \"([^\"]*)\"( \\((?:very )?hidden sheet\\))?.*$");
	std::vector<PassageSheet> sheets;
	for (std::size_t passage = 0; passage < passages.size() && sheets.size() < max_tables; ++passage) {
		std::optional<PassageSheet> current;
		std::map<unsigned, std::size_t> rowIndex;
		std::string name;
		bool hiddenSheet = false;
		const auto finish = [&]() {
			if (current && current->rows.size() >= std::max<std::size_t>(min_rows, 1)) sheets.push_back(std::move(*current));
			current.reset();
			rowIndex.clear();
		};
		std::size_t from = 0;
		const std::string &text = passages[passage];
		while (from <= text.size()) {
			std::size_t end = text.find('\n', from);
			if (end == std::string::npos) end = text.size();
			const std::string line = text.substr(from, end - from);
			from = end + 1;
			std::smatch match;
			std::string body = line;
			std::optional<unsigned> number;
			bool hidden = false;
			if (std::regex_match(line, match, rowPrefix)) {
				number = static_cast<unsigned>(std::strtoul(match[1].str().c_str(), nullptr, 10));
				hidden = match[2].matched;
				body = match[3].str();
			}
			std::vector<PassageCell> cells;
			std::size_t pieceStart = 0;
			bool every = true;
			while (pieceStart <= body.size()) {
				std::size_t pieceEnd = body.find(" | ", pieceStart);
				if (pieceEnd == std::string::npos) pieceEnd = body.size();
				const std::optional<PassageCell> cell = parse_passage_cell(body.substr(pieceStart, pieceEnd - pieceStart));
				if (!cell) { every = false; break; }
				cells.push_back(*cell);
				pieceStart = pieceEnd + 3;
			}
			if (!every || cells.empty()) {
				if (line.rfind("Sheet ", 0) == 0) {
					finish();
					std::smatch sheet;
					if (std::regex_match(line, sheet, sheetLine)) {
						name = sheet[1].str();
						hiddenSheet = sheet[2].matched;
					}
				}
				if (end == text.size()) break;
				continue;
			}
			if (!current) current = PassageSheet{passage, name, hiddenSheet, {}};
			for (PassageCell &cell : cells) {
				const unsigned rowNumber = number.value_or(cell.row);
				auto found = rowIndex.find(rowNumber);
				if (found == rowIndex.end()) {
					if (current->rows.size() >= max_rows) break;
					found = rowIndex.emplace(rowNumber, current->rows.size()).first;
					current->rows.push_back(PassageRow{rowNumber, hidden, {}});
				}
				current->rows[found->second].cells.push_back(std::move(cell));
			}
			if (end == text.size()) break;
		}
		finish();
	}
	return sheets;
}

} // namespace singularity::protected_api::papinian::tables

// ---------------------------------------------------------------------------
// Band schedules in prose (DESIGN4 §6.6)

namespace singularity::protected_api::papinian::tables {
namespace {

struct Figure {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string text;
	std::optional<semantic::Value> value;
};

bool digit_at(const std::string &text, std::size_t at) { return at < text.size() && text[at] >= '0' && text[at] <= '9'; }

// Every figure a clause writes: digits with separators and a decimal comma or
// point, and the percent sign or currency right after them.
std::vector<Figure> figures_in(const std::string &text, std::size_t from, std::size_t to) {
	std::vector<Figure> found;
	std::size_t at = from;
	while (at < to) {
		if (!digit_at(text, at) || (at > from && (std::isalpha(static_cast<unsigned char>(text[at - 1])) != 0))) {
			++at;
			continue;
		}
		const std::size_t start = at;
		std::size_t end = at;
		while (end < to) {
			if (digit_at(text, end)) { ++end; continue; }
			// A thousands space (plain or no-break) or a separator between digits.
			if ((text[end] == ',' || text[end] == '.' || text[end] == ' ') && digit_at(text, end + 1)) { end += 2; continue; }
			if (text.compare(end, 2, "\xC2\xA0") == 0 && digit_at(text, end + 2)) { end += 3; continue; }
			break;
		}
		std::size_t tail = end;
		while (tail < to && text[tail] == ' ') ++tail;
		if (tail < to && text[tail] == '%') end = tail + 1;
		else if (text.compare(tail, 2, "\xC2\xA0") == 0 && tail + 2 < to && text[tail + 2] == '%') end = tail + 3;

		// An amount's currency sign before it: "$1,000,000".
		std::size_t begin = start;
		for (const char *sign : {"$", "\xE2\x82\xAC", "\xC2\xA3"}) {
			const std::size_t length = std::char_traits<char>::length(sign);
			if (begin >= from + length && text.compare(begin - length, length, sign) == 0) {
				begin -= length;
				break;
			}
		}
		Figure figure;
		figure.start = begin;
		figure.end = end;
		figure.text = text.substr(begin, end - begin);
		figure.value = semantic::parse_value(figure.text);
		if (figure.value && figure.value->kind != semantic::Kind::Date && figure.value->kind != semantic::Kind::DayOfYear)
			found.push_back(std::move(figure));
		at = end;
	}
	return found;
}

struct Band {
	std::size_t start = 0;
	std::size_t end = 0;
	std::optional<Figure> lower;
	std::optional<Figure> upper;
	bool lower_inclusive = true;
	bool upper_inclusive = false;
	Figure value;
};

// One clause as a band, or nothing: the figures it writes and the words before
// the first of them say where the band lies; the last figure is its value.
std::optional<Band> band_in(const std::string &text, std::size_t from, std::size_t to) {
	const std::vector<Figure> figures = figures_in(text, from, to);
	if (figures.size() < 2) return std::nullopt;
	const std::string before = fold(text.substr(from, figures.front().start - from));
	const std::string between = figures.size() >= 2 ? fold(text.substr(figures[0].end, figures[1].start - figures[0].end)) : std::string();
	const auto ends_with_any = [](const std::string &folded, std::initializer_list<std::string_view> words) {
		std::string clean = folded;
		while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) clean.pop_back();
		for (const std::string_view word : words)
			if (clean.size() >= word.size() && clean.compare(clean.size() - word.size(), word.size(), word) == 0
					&& (clean.size() == word.size() || !letter_at(clean, clean.size() - word.size() - 1)))
				return true;
		return false;
	};
	Band band;
	band.start = from;
	band.end = to;
	band.value = figures.back();
	const bool rangeWords = has_word(between, "to") || has_word(between, "\xD0\xB4\xD0\xBE")  // до
		|| between.find('-') != std::string::npos || between.find("\xE2\x80\x93") != std::string::npos
		|| between.find("\xE2\x80\x94") != std::string::npos;
	if (figures.size() >= 3 && rangeWords) {
		band.lower = figures[0];
		band.upper = figures[1];
		return band;
	}
	if (ends_with_any(before, {"above", "over", "more than", "exceeding", "in excess of", ">",
			"\xD1\x81\xD0\xB2\xD1\x8B\xD1\x88\xD0\xB5",                  // свыше
			"\xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5",                  // более
			"\xD1\x81\xD0\xB2\xD0\xB5\xD1\x80\xD1\x85"})) {              // сверх
		band.lower = figures[0];
		band.lower_inclusive = false;
		return band;
	}
	if (ends_with_any(before, {"at least", "not less than", "from", "\xE2\x89\xA5", ">=",
			"\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",  // не менее
			"\xD0\xBE\xD1\x82"})) {                                      // от
		band.lower = figures[0];
		return band;
	}
	if (ends_with_any(before, {"less than", "below", "under", "<",
			"\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",                  // менее
			"\xD0\xBD\xD0\xB8\xD0\xB6\xD0\xB5"})) {                      // ниже
		band.upper = figures[0];
		return band;
	}
	if (ends_with_any(before, {"up to", "not more than", "not exceeding", "\xE2\x89\xA4", "<=",
			"\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5",  // не более
			"\xD0\xB4\xD0\xBE"})) {                                      // до
		band.upper = figures[0];
		band.upper_inclusive = true;
		return band;
	}
	// "X or more: r %", «X и более — r %».
	const std::string after = fold(text.substr(figures[0].end, figures[1].start - figures[0].end));
	if (after.find("or more") != std::string::npos || after.find("and above") != std::string::npos
			|| after.find("\xD0\xB8 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5") != std::string::npos) {  // и более
		band.lower = figures[0];
		return band;
	}
	if (after.find("or less") != std::string::npos || after.find("and below") != std::string::npos
			|| after.find("\xD0\xB8 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5") != std::string::npos) {  // и менее
		band.upper = figures[0];
		band.upper_inclusive = true;
		return band;
	}
	return std::nullopt;
}

// A clause that writes its bands rate first, joined by "and": "2% of
// Purchases up to $1,000,000 and 3.5% of Purchases above $1,000,000", «2 % при
// объёме до 1 000 000 руб. и 3,5 % свыше 1 000 000 руб.». Each part is a rate
// and the one threshold after it; nothing unless every part reads so.
std::vector<Band> rate_first_bands(const std::string &text, std::size_t from, std::size_t to) {
	std::vector<Band> bands;
	std::vector<std::pair<std::size_t, std::size_t>> parts;
	std::size_t start = from;
	const std::string lowered = fold(text.substr(from, to - from));
	for (std::size_t at = 0; at < lowered.size();) {
		std::size_t joint = std::string::npos;
		std::size_t length = 0;
		for (const char *word : {" and ", " \xD0\xB8 "}) {
			const std::size_t found = lowered.find(word, at);
			if (found != std::string::npos && found < joint) {
				joint = found;
				length = std::char_traits<char>::length(word);
			}
		}
		if (joint == std::string::npos) break;
		parts.emplace_back(start, from + joint);
		start = from + joint + length;
		at = joint + length;
	}
	parts.emplace_back(start, to);
	if (parts.size() < 2) return bands;
	for (const auto &[partStart, partEnd] : parts) {
		// From the part's rate on: a section number before it ("6.1") is not the band's.
		std::vector<Figure> figures = figures_in(text, partStart, partEnd);
		while (!figures.empty() && (!figures.front().value || figures.front().value->kind != semantic::Kind::Percentage))
			figures.erase(figures.begin());
		if (figures.size() != 2) return {};
		std::string between = fold(text.substr(figures[0].end, figures[1].start - figures[0].end));
		while (!between.empty() && between.back() == ' ') between.pop_back();
		const auto ends = [&](std::initializer_list<std::string_view> words) {
			for (const std::string_view word : words)
				if (between.size() >= word.size() && between.compare(between.size() - word.size(), word.size(), word) == 0
						&& (between.size() == word.size() || between[between.size() - word.size() - 1] == ' '))
					return true;
			return false;
		};
		Band band;
		band.start = partStart;
		band.end = partEnd;
		band.value = figures[0];
		if (ends({"up to", "not exceeding", "not more than", "\xD0\xB4\xD0\xBE", "\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5"})) {
			band.upper = figures[1];
			band.upper_inclusive = true;
		} else if (ends({"above", "over", "in excess of", "exceeding", "more than", "\xD1\x81\xD0\xB2\xD1\x8B\xD1\x88\xD0\xB5",
				"\xD1\x81\xD0\xB2\xD0\xB5\xD1\x80\xD1\x85", "\xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5"})) {
			band.lower = figures[1];
			band.lower_inclusive = false;
		} else if (ends({"from", "at least", "not less than", "\xD0\xBE\xD1\x82", "\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5"})) {
			band.lower = figures[1];
		} else if (ends({"below", "under", "less than", "\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5", "\xD0\xBD\xD0\xB8\xD0\xB6\xD0\xB5"})) {
			band.upper = figures[1];
		} else {
			return {};
		}
		bands.push_back(std::move(band));
	}
	return bands;
}

// The fact a figure of the passage is: the registry row stated at its place,
// else the one row written exactly so with that value (of the passage's own
// document, when several are); or none (a figure the material writes openly: a
// grounded constant). `ambiguous` when several rows could be it and none is
// the passage's own: the figure is a protected fact whose symbol cannot be
// told, never an open constant.
struct FactAt {
	const RequestMappingEntry *row = nullptr;
	bool ambiguous = false;
};

FactAt fact_at(const OperationProtectedSymbolTable &symbols, const std::string &sha, const Figure &figure) {
	std::vector<const RequestMappingEntry *> written;
	for (const RequestMappingEntry &entry : symbols.mapping) {
		if (entry.derived || !entry.value) continue;
		if (!sha.empty() && entry.provenance.passage_sha256 == sha && entry.provenance.offset < figure.end
				&& entry.provenance.offset + entry.provenance.length > figure.start)
			return {&entry, false};
		if (figure.value && agrees(*entry.value, *figure.value)) {
			for (const std::string &form : entry.every_form())
				if (trim(form) == figure.text || form.find(figure.text) != std::string::npos) {
					written.push_back(&entry);
					break;
				}
		}
	}
	if (written.size() == 1) return {written.front(), false};
	if (written.empty()) return {};
	const auto scope_of = [&](const std::string &digest) -> std::string {
		const auto found = symbols.document_of_passage.find(digest);
		return found == symbols.document_of_passage.end() ? std::string() : found->second;
	};
	const std::string own = sha.empty() ? std::string() : scope_of(sha);
	const RequestMappingEntry *same = nullptr;
	std::size_t sameCount = 0;
	if (!own.empty())
		for (const RequestMappingEntry *entry : written)
			if (scope_of(entry->provenance.passage_sha256) == own) {
				same = entry;
				++sameCount;
			}
	if (sameCount == 1) return {same, false};
	return {nullptr, true};
}

std::string type_of_value(const semantic::Value &value) {
	switch (value.kind) {
		case semantic::Kind::Money: return "MONEY(" + (value.currency.empty() ? std::string("?") : value.currency) + ")";
		case semantic::Kind::Percentage: return "PERCENTAGE";
		case semantic::Kind::Duration: return "DURATION(days)";
		case semantic::Kind::Count: return "COUNT";
		default: return "NUMBER";
	}
}

} // namespace

std::vector<ProtectedSchedule> schedules_in(const std::vector<singularity::context::business_calendar::RulePassage> &raw_passages,
		const OperationProtectedSymbolTable &table) {
	std::vector<ProtectedSchedule> schedules;
	std::size_t number = table.schedules.size();
	for (const singularity::context::business_calendar::RulePassage &passage : raw_passages) {
		const std::string &text = passage.text;
		// Clauses: each line, and each semicolon part of it.
		struct Clause {
			std::size_t start = 0;
			std::size_t end = 0;
			std::size_t line = 0;
		};
		std::vector<Clause> clauses;
		std::size_t lineStart = 0;
		std::size_t lineNumber = 0;
		while (lineStart <= text.size()) {
			std::size_t lineEnd = text.find('\n', lineStart);
			if (lineEnd == std::string::npos) lineEnd = text.size();
			std::size_t part = lineStart;
			while (part <= lineEnd) {
				std::size_t stop = text.find(';', part);
				if (stop == std::string::npos || stop > lineEnd) stop = lineEnd;
				clauses.push_back({part, stop, lineNumber});
				part = stop + 1;
			}
			++lineNumber;
			if (lineEnd == text.size()) break;
			lineStart = lineEnd + 1;
		}
		std::vector<Band> run;
		std::size_t runFirstClause = 0;
		const auto close = [&](std::size_t clauseIndex) {
			if (run.size() >= 2) {
				ProtectedSchedule schedule;
				schedule.handle = "Schedule" + std::to_string(++number);
				schedule.document_id = passage.document;
				schedule.location = passage.title;
				schedule.source.passage_sha256 = passage.sha256;
				schedule.source.origin = "source";
				schedule.source.offset = run.front().start;
				schedule.source.length = run.back().end - run.front().start;
				// Its name: what the line before the first band says, or the words
				// before the first band on its own line.
				const Clause &first = clauses[runFirstClause];
				std::string title;
				// A name before a colon in the first band's own clause ("Discount
				// scale: from ..."), else the line before the bands.
				const std::size_t colon = text.find(':', first.start);
				if (colon != std::string::npos && colon < run.front().value.start) {
					const std::string head = text.substr(first.start, colon - first.start);
					if (head.find_first_of("0123456789") == std::string::npos) title = trim(head);
				}
				if (title.empty() && first.start > 0 && text[first.start - 1] == '\n') {
					// The nearest line above that is not empty.
					std::size_t lineEnd = first.start - 1;
					while (true) {
						const std::size_t newline = lineEnd == 0 ? std::string::npos : text.rfind('\n', lineEnd - 1);
						const std::size_t lineBegin = newline == std::string::npos ? 0 : newline + 1;
						title = trim(text.substr(lineBegin, lineEnd - lineBegin));
						if (!title.empty() || lineBegin == 0) break;
						lineEnd = lineBegin - 1;
					}
				}
				if (title.size() > 120) title.clear();
				while (!title.empty() && (title.back() == ':' || title.back() == ' ')) title.pop_back();
				schedule.title = title;
				std::string keyType;
				std::string valueType;
				// A figure that is a protected fact whose symbol cannot be told makes
				// the schedule unusable: its writing is never kept as a constant.
				bool unusable = false;
				const auto figure_of = [&](const Figure &figure, std::string &written, std::optional<semantic::Value> &value,
						std::string &fact) {
					value = figure.value;
					const FactAt at = fact_at(table, passage.sha256, figure);
					if (at.ambiguous) {
						unusable = true;
					} else if (at.row != nullptr) {
						written = at.row->placeholder;
						fact = at.row->fact_key;
						value = at.row->value;
					} else {
						written = figure.text;
					}
				};
				for (const Band &read : run) {
					ScheduleBand band;
					const auto side = [&](const std::optional<Figure> &figure, std::string &written,
							std::optional<semantic::Value> &value, std::string &fact) {
						if (!figure) return;
						figure_of(*figure, written, value, fact);
						if (keyType.empty() && value) keyType = type_of_value(*value);
					};
					side(read.lower, band.lower, band.lower_value, band.lower_fact);
					side(read.upper, band.upper, band.upper_value, band.upper_fact);
					band.lower_inclusive = read.lower_inclusive;
					band.upper_inclusive = read.upper_inclusive;
					figure_of(read.value, band.value, band.band_value, band.value_fact);
					if (valueType.empty() && band.band_value) valueType = type_of_value(*band.band_value);
					schedule.bands.push_back(std::move(band));
				}
				schedule.key_type = keyType.empty() ? "NUMBER" : keyType;
				schedule.value_type = valueType.empty() ? "NUMBER" : valueType;
				if (unusable) --number;
				else schedules.push_back(std::move(schedule));
			}
			run.clear();
			runFirstClause = clauseIndex;
		};
		for (std::size_t index = 0; index < clauses.size(); ++index) {
			const std::optional<Band> band = band_in(text, clauses[index].start, clauses[index].end);
			if (!band) {
				const std::vector<Band> joined = rate_first_bands(text, clauses[index].start, clauses[index].end);
				if (joined.size() >= 2) {
					close(index);
					runFirstClause = index;
					run.insert(run.end(), joined.begin(), joined.end());
					close(index + 1);
					continue;
				}
				// An empty clause (a semicolon at a line's end) does not break a run.
				if (trim(text.substr(clauses[index].start, clauses[index].end - clauses[index].start)).empty()) continue;
				close(index + 1);
				continue;
			}
			if (run.empty()) runFirstClause = index;
			run.push_back(*band);
		}
		close(clauses.size());
	}
	return schedules;
}

} // namespace singularity::protected_api::papinian::tables
