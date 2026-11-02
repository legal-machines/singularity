#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <regex>
#include <vector>

namespace singularity::protected_api::papinian::semantic {
namespace {

using UInt128 = singularity::platform::UInt128;

// ---------------------------------------------------------------------------
// Exact whole numbers that say so when they would not fit.

AlgebraError overflow() {
	return AlgebraError("overflow", "the numbers are too large to work out exactly");
}

Int128 add128(Int128 a, Int128 b) {
	Int128 result;
	if (singularity::platform::add_overflow(a, b, &result)) throw overflow();
	return result;
}

Int128 sub128(Int128 a, Int128 b) {
	Int128 result;
	if (singularity::platform::sub_overflow(a, b, &result)) throw overflow();
	return result;
}

Int128 mul128(Int128 a, Int128 b) {
	Int128 result;
	if (singularity::platform::mul_overflow(a, b, &result)) throw overflow();
	return result;
}

Int128 neg128(Int128 a) { return sub128(0, a); }

Int128 abs128(Int128 a) { return a < 0 ? neg128(a) : a; }

Int128 gcd128(Int128 a, Int128 b) {
	a = abs128(a);
	b = abs128(b);
	while (b != 0) {
		const Int128 rest = a % b;
		a = b;
		b = rest;
	}
	return a;
}

Int128 pow10(int places) {
	if (places < 0 || places > 36) throw overflow();
	Int128 result = 1;
	for (int index = 0; index < places; ++index) result *= 10;
	return result;
}

// Floor of a / b for b > 0.
Int128 floor_div(Int128 a, Int128 b) {
	Int128 quotient = a / b;
	if (a % b != 0 && a < 0) --quotient;
	return quotient;
}

std::string int_text(Int128 value) {
	if (value == 0) return "0";
	const bool negative = value < 0;
	UInt128 magnitude = negative ? UInt128(0) - static_cast<UInt128>(value) : static_cast<UInt128>(value);
	std::string digits;
	while (magnitude != 0) {
		digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
		magnitude /= 10;
	}
	if (negative) digits.push_back('-');
	std::reverse(digits.begin(), digits.end());
	return digits;
}

// a/b < c/d for b, d > 0, by continued fractions: never multiplies past the
// numbers themselves, so it cannot overflow.
bool fraction_less(Int128 a, Int128 b, Int128 c, Int128 d) {
	for (;;) {
		const Int128 wholeA = floor_div(a, b);
		const Int128 wholeC = floor_div(c, d);
		if (wholeA != wholeC) return wholeA < wholeC;
		const Int128 restA = a - wholeA * b;
		const Int128 restC = c - wholeC * d;
		if (restA == 0 || restC == 0) return restA == 0 && restC != 0;
		// restA/b < restC/d exactly when d/restC < b/restA.
		const Int128 nextA = d, nextB = restC, nextC = b, nextD = restA;
		a = nextA;
		b = nextB;
		c = nextC;
		d = nextD;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Rational

Rational::Rational(Int128 numerator, Int128 denominator) {
	if (denominator == 0) throw AlgebraError("division_by_zero", "division by zero");
	if (denominator < 0) {
		numerator = neg128(numerator);
		denominator = neg128(denominator);
	}
	const Int128 common = gcd128(numerator, denominator);
	if (common > 1) {
		numerator /= common;
		denominator /= common;
	}
	if (numerator == 0) denominator = 1;
	numerator_ = numerator;
	denominator_ = denominator;
}

std::optional<Rational> Rational::parse_decimal(std::string_view text) {
	std::size_t at = 0;
	bool negative = false;
	if (at < text.size() && (text[at] == '-' || text[at] == '+')) {
		negative = text[at] == '-';
		++at;
	}
	Int128 digits = 0;
	int count = 0;
	int decimals = 0;
	bool point = false;
	for (; at < text.size(); ++at) {
		const char character = text[at];
		if (character == '.') {
			if (point) return std::nullopt;
			point = true;
			continue;
		}
		if (character < '0' || character > '9') return std::nullopt;
		if (++count > 30) return std::nullopt;
		digits = digits * 10 + (character - '0');
		if (point) ++decimals;
	}
	if (count == 0) return std::nullopt;
	return Rational(negative ? -digits : digits, pow10(decimals));
}

Rational Rational::from_decimal(std::string_view text) {
	if (auto value = parse_decimal(text)) return *value;
	throw AlgebraError("syntax", std::string("\"") + std::string(text) + "\" is not a number");
}

Rational Rational::abs() const { return numerator_ < 0 ? -*this : *this; }

Rational Rational::operator-() const {
	Rational result;
	result.numerator_ = neg128(numerator_);
	result.denominator_ = denominator_;
	return result;
}

Rational operator+(const Rational &a, const Rational &b) {
	const Int128 common = gcd128(a.denominator_, b.denominator_);
	const Int128 denominator = mul128(a.denominator_ / common, b.denominator_);
	const Int128 numerator = add128(mul128(a.numerator_, b.denominator_ / common), mul128(b.numerator_, a.denominator_ / common));
	return Rational(numerator, denominator);
}

Rational operator-(const Rational &a, const Rational &b) { return a + (-b); }

Rational operator*(const Rational &a, const Rational &b) {
	if (a.numerator_ == 0 || b.numerator_ == 0) return Rational();
	const Int128 first = gcd128(a.numerator_, b.denominator_);
	const Int128 second = gcd128(b.numerator_, a.denominator_);
	return Rational(mul128(a.numerator_ / first, b.numerator_ / second), mul128(a.denominator_ / second, b.denominator_ / first));
}

Rational operator/(const Rational &a, const Rational &b) {
	if (b.numerator_ == 0) throw AlgebraError("division_by_zero", "division by zero");
	return a * Rational(b.denominator_, b.numerator_);
}

bool operator<(const Rational &a, const Rational &b) {
	return fraction_less(a.numerator_, a.denominator_, b.numerator_, b.denominator_);
}

Rational Rational::rounded(int places) const {
	const Int128 scale = pow10(places);
	const Rational scaled = *this * Rational(scale);
	const Int128 magnitude = abs128(scaled.numerator_);
	// floor(|x| + 1/2): half up, away from zero on an exact half.
	const Int128 whole = add128(mul128(magnitude, 2), scaled.denominator_) / mul128(scaled.denominator_, 2);
	return Rational(scaled.numerator_ < 0 ? -whole : whole, scale);
}

Rational Rational::floored(int places) const {
	const Int128 scale = pow10(places);
	const Rational scaled = *this * Rational(scale);
	return Rational(floor_div(scaled.numerator_, scaled.denominator_), scale);
}

Rational Rational::ceiled(int places) const { return -((-*this).floored(places)); }

std::string Rational::to_decimal(int places) const {
	const Rational value = rounded(places);
	const Int128 scale = pow10(places);
	const Int128 scaled = mul128(value.numerator_, scale / value.denominator_);
	std::string digits = int_text(abs128(scaled));
	if (places > 0) {
		if (digits.size() < static_cast<std::size_t>(places) + 1)
			digits.insert(0, static_cast<std::size_t>(places) + 1 - digits.size(), '0');
		digits.insert(digits.size() - static_cast<std::size_t>(places), 1, '.');
	}
	return scaled < 0 ? "-" + digits : digits;
}

std::optional<std::string> Rational::exact_decimal(int max_places) const {
	for (int places = 0; places <= max_places && places <= 36; ++places)
		if (pow10(places) % denominator_ == 0) return to_decimal(places);
	return std::nullopt;
}

std::string Rational::to_string() const {
	if (denominator_ == 1) return int_text(numerator_);
	return int_text(numerator_) + "/" + int_text(denominator_);
}

namespace {

// ---------------------------------------------------------------------------
// Text as it is written: spaces of every kind, capitals, ё.

bool starts_at(std::string_view text, std::size_t at, std::string_view piece) {
	return at <= text.size() && text.size() - at >= piece.size() && text.compare(at, piece.size(), piece) == 0;
}

bool starts_with(std::string_view text, std::string_view piece) { return starts_at(text, 0, piece); }

bool is_digit(char character) { return character >= '0' && character <= '9'; }

bool is_ascii_letter(char character) {
	return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool is_cyrillic_lead(char character) {
	const unsigned char byte = static_cast<unsigned char>(character);
	return byte == 0xD0 || byte == 0xD1;
}

// A Cyrillic letter at `at`: its lead byte and the byte that completes it. A
// lead byte alone (text cut mid-letter) is not a letter.
bool cyrillic_at(std::string_view text, std::size_t at) {
	return at + 1 < text.size() && is_cyrillic_lead(text[at]) && (static_cast<unsigned char>(text[at + 1]) & 0xC0) == 0x80;
}

// Lower case for ASCII and Cyrillic, ё read as е, byte for byte the same length.
std::string lowered(std::string_view text) {
	std::string result(text);
	for (std::size_t index = 0; index < result.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(result[index]);
		if (byte >= 'A' && byte <= 'Z') {
			result[index] = static_cast<char>(byte - 'A' + 'a');
		} else if (byte == 0xD0 && index + 1 < result.size()) {
			const unsigned char next = static_cast<unsigned char>(result[index + 1]);
			if (next >= 0x90 && next <= 0x9F) result[index + 1] = static_cast<char>(next + 0x20);
			else if (next >= 0xA0 && next <= 0xAF) { result[index] = '\xD1'; result[index + 1] = static_cast<char>(next - 0x20); }
			else if (next == 0x81) result[index + 1] = '\xB5';
			++index;
		} else if (byte == 0xD1 && index + 1 < result.size()) {
			if (static_cast<unsigned char>(result[index + 1]) == 0x91) { result[index] = '\xD0'; result[index + 1] = '\xB5'; }
			++index;
		}
	}
	return result;
}

// One plain space for every space a document writes (no-break, thin, narrow,
// figure, tabs, line breaks), "-" for the minus sign and dashes, runs of
// spaces as one, and none at either end.
std::string normalized(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	const auto space = [&result]() {
		if (!result.empty() && result.back() != ' ') result.push_back(' ');
	};
	for (std::size_t index = 0; index < text.size(); ++index) {
		const char character = text[index];
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') { space(); continue; }
		if (starts_at(text, index, "\xC2\xA0")) { space(); ++index; continue; }
		if (starts_at(text, index, "\xE2\x80") && index + 2 < text.size()) {
			const unsigned char third = static_cast<unsigned char>(text[index + 2]);
			if ((third >= 0x80 && third <= 0x8A) || third == 0xAF) { space(); index += 2; continue; }
			if (third == 0x8B) { index += 2; continue; }  // zero-width space
			if (third == 0x93 || third == 0x94) { result.push_back('-'); index += 2; continue; }
		}
		if (starts_at(text, index, "\xE2\x81\x9F")) { space(); index += 2; continue; }
		if (starts_at(text, index, "\xE2\x88\x92")) { result.push_back('-'); index += 2; continue; }
		result.push_back(character);
	}
	while (!result.empty() && result.back() == ' ') result.pop_back();
	return result;
}

bool has_cyrillic(std::string_view text) {
	for (const char character : text)
		if (is_cyrillic_lead(character)) return true;
	return false;
}

std::string without_dot(std::string_view word) {
	std::string result(word);
	if (!result.empty() && result.back() == '.') result.pop_back();
	return result;
}

// ---------------------------------------------------------------------------
// Numbers written in words.

struct WordNumber {
	const char *form;
	int value;
};

const std::vector<WordNumber> &english_numbers() {
	static const std::vector<WordNumber> all{
		{"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7},
		{"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14},
		{"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20},
		{"thirty", 30}, {"forty", 40}, {"fifty", 50}, {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90},
	};
	return all;
}

// Russian numerals in every case a document writes them in.
const std::vector<WordNumber> &russian_numbers() {
	static const std::vector<WordNumber> all = [] {
		std::vector<WordNumber> forms{
			{"ноль", 0}, {"нуля", 0}, {"нулю", 0}, {"нулем", 0},
			{"один", 1}, {"одна", 1}, {"одно", 1}, {"одного", 1}, {"одной", 1}, {"одному", 1}, {"одним", 1},
			{"одном", 1}, {"одну", 1},
			{"два", 2}, {"две", 2}, {"двух", 2}, {"двум", 2}, {"двумя", 2},
			{"три", 3}, {"трех", 3}, {"трем", 3}, {"тремя", 3},
			{"четыре", 4}, {"четырех", 4}, {"четырем", 4}, {"четырьмя", 4},
			{"восемь", 8}, {"восьми", 8}, {"восемью", 8}, {"восьмью", 8},
			{"сорок", 40}, {"сорока", 40},
			{"пятьдесят", 50}, {"пятидесяти", 50}, {"пятьюдесятью", 50},
			{"шестьдесят", 60}, {"шестидесяти", 60}, {"шестьюдесятью", 60},
			{"семьдесят", 70}, {"семидесяти", 70}, {"семьюдесятью", 70},
			{"восемьдесят", 80}, {"восьмидесяти", 80}, {"восемьюдесятью", 80},
			{"девяносто", 90}, {"девяноста", 90},
			{"сто", 100}, {"ста", 100},
			{"двести", 200}, {"двухсот", 200}, {"двумстам", 200}, {"двумястами", 200}, {"двухстах", 200},
			{"триста", 300}, {"трехсот", 300}, {"тремстам", 300}, {"тремястами", 300}, {"трехстах", 300},
			{"четыреста", 400}, {"четырехсот", 400}, {"четыремстам", 400}, {"четырьмястами", 400}, {"четырехстах", 400},
			{"пятьсот", 500}, {"пятисот", 500}, {"пятистам", 500}, {"пятьюстами", 500}, {"пятистах", 500},
			{"шестьсот", 600}, {"шестисот", 600}, {"шестистам", 600}, {"шестьюстами", 600}, {"шестистах", 600},
			{"семьсот", 700}, {"семисот", 700}, {"семистам", 700}, {"семьюстами", 700}, {"семистах", 700},
			{"восемьсот", 800}, {"восьмисот", 800}, {"восьмистам", 800}, {"восемьюстами", 800}, {"восьмистах", 800},
			{"девятьсот", 900}, {"девятисот", 900}, {"девятистам", 900}, {"девятьюстами", 900}, {"девятистах", 900},
		};
		// пять, пяти, пятью - the numerals in -ь decline alike.
		static const std::vector<std::pair<std::string, int>> stems{
			{"пят", 5}, {"шест", 6}, {"сем", 7}, {"девят", 9}, {"десят", 10}, {"одиннадцат", 11}, {"двенадцат", 12},
			{"тринадцат", 13}, {"четырнадцат", 14}, {"пятнадцат", 15}, {"шестнадцат", 16}, {"семнадцат", 17},
			{"восемнадцат", 18}, {"девятнадцат", 19}, {"двадцат", 20}, {"тридцат", 30},
		};
		static std::vector<std::string> generated;
		generated.reserve(stems.size() * 3);
		for (const auto &stem : stems)
			for (const char *ending : {"ь", "и", "ью"}) generated.push_back(stem.first + ending);
		for (std::size_t index = 0; index < generated.size(); ++index)
			forms.push_back({generated[index].c_str(), stems[index / 3].second});
		return forms;
	}();
	return all;
}

std::optional<int> small_number(const std::string &word) {
	for (const WordNumber &entry : english_numbers())
		if (word == entry.form) return entry.value;
	for (const WordNumber &entry : russian_numbers())
		if (word == entry.form) return entry.value;
	return std::nullopt;
}

// thousand, million, тысяча/тысячи/тысяч, миллионов, млн, тыс.
std::optional<Int128> scale_word(const std::string &raw) {
	const std::string word = without_dot(raw);
	if (word == "thousand" || word == "thousands" || word == "тыс" || starts_with(word, "тысяч")) return Int128(1000);
	if (word == "million" || word == "millions" || word == "mm" || word == "mn" || word == "mln" || word == "млн"
		|| starts_with(word, "миллион"))
		return Int128(1000000);
	if (word == "billion" || word == "billions" || word == "bn" || word == "млрд" || starts_with(word, "миллиард"))
		return Int128(1000000000);
	if (word == "trillion" || word == "trillions" || word == "трлн" || starts_with(word, "триллион"))
		return Int128(1000000000000LL);
	return std::nullopt;
}

bool spelled_scale(const std::string &raw) {
	const std::string word = without_dot(raw);
	return scale_word(word) && word != "mm" && word != "mn" && word != "mln" && word != "bn" && word != "тыс"
		&& word != "млн" && word != "млрд" && word != "трлн";
}

// A word of a number written out: "twenty-one" is two.
bool number_word(const std::string &word) {
	if (word == "hundred" || word == "hundreds" || spelled_scale(word)) return true;
	if (small_number(word)) return true;
	const std::size_t dash = word.find('-');
	if (dash != std::string::npos)
		return small_number(word.substr(0, dash)).has_value() && small_number(word.substr(dash + 1)).has_value();
	return false;
}

std::optional<Rational> words_value(const std::vector<std::string> &raw) {
	std::vector<std::string> words;
	for (const std::string &word : raw) {
		const std::size_t dash = word.find('-');
		if (dash != std::string::npos && word.find('-', dash + 1) == std::string::npos) {
			words.push_back(word.substr(0, dash));
			words.push_back(word.substr(dash + 1));
		} else {
			words.push_back(word);
		}
	}
	Int128 total = 0;
	Int128 current = 0;
	Int128 lastScale = 0;
	bool any = false;
	bool zero = false;
	for (std::size_t index = 0; index < words.size(); ++index) {
		const std::string &word = words[index];
		// "zero" is a number only alone.
		if (zero) return std::nullopt;
		if (word == "and" || word == "и") {
			if (!any || index + 1 == words.size()) return std::nullopt;
			continue;
		}
		if (const auto small = small_number(word)) {
			if (*small == 0) {
				if (any) return std::nullopt;
				zero = true;
			}
			// "twenty five", "сто двадцать": a smaller part follows a larger one;
			// only a unit follows a ten, and nothing follows ten itself or a teen
			// ("ten one" is not a number).
			if (current != 0 && *small >= 100 && current % 1000 != 0) return std::nullopt;
			const Int128 tail = current % 100;
			if (tail != 0 && *small < 100) {
				if (tail < 20 || tail % 10 != 0 || *small >= 10) return std::nullopt;
			}
			current += *small;
			any = true;
			continue;
		}
		if (word == "hundred" || word == "hundreds") {
			if (current >= 100) return std::nullopt;
			current = (current == 0 ? 1 : current) * 100;
			any = true;
			continue;
		}
		if (const auto scale = scale_word(word)) {
			if (lastScale != 0 && *scale >= lastScale) return std::nullopt;
			total = add128(total, mul128(current == 0 ? 1 : current, *scale));
			current = 0;
			lastScale = *scale;
			any = true;
			continue;
		}
		return std::nullopt;
	}
	if (!any) return std::nullopt;
	return Rational(add128(total, current));
}

// ---------------------------------------------------------------------------
// Currencies, units, rates as words.

std::string currency_of_symbol(const std::string &symbol) {
	if (symbol == "$" || symbol == "us$") return "USD";
	if (symbol == "\xE2\x82\xAC") return "EUR";
	if (symbol == "\xC2\xA3") return "GBP";
	if (symbol == "\xC2\xA5") return "JPY";
	if (symbol == "\xE2\x82\xBD") return "RUB";
	return "";
}

// An ISO code standing before or after an amount.
std::string currency_of_code(const std::string &word) {
	static const char *codes[] = {"usd", "eur", "gbp", "jpy", "rub", "chf", "cny", "cad", "aud", "hkd", "sgd", "kzt", "byn", "aed", "inr"};
	for (const char *code : codes)
		if (word == code) {
			std::string upper = code;
			for (char &character : upper) character = static_cast<char>(character - 'a' + 'A');
			return upper;
		}
	if (word == "rur") return "RUB";
	return "";
}

std::string currency_of_word(const std::string &raw) {
	const std::string word = without_dot(raw);
	if (const std::string code = currency_of_code(word); !code.empty()) return code;
	if (word == "dollar" || word == "dollars" || starts_with(word, "доллар") || word == "долл") return "USD";
	if (word == "euro" || word == "euros" || word == "евро") return "EUR";
	if (word == "pound" || word == "pounds" || starts_with(word, "фунт")) return "GBP";
	if (word == "yen" || starts_with(word, "иен") || starts_with(word, "йен")) return "JPY";
	if (word == "руб" || starts_with(word, "рубл") || word == "ruble" || word == "rubles" || word == "rouble" || word == "roubles")
		return "RUB";
	return "";
}

// A sub-unit and the currency it belongs to ("*": cents of dollars or euros).
std::string subunit_of_word(const std::string &raw) {
	const std::string word = without_dot(raw);
	if (word == "коп" || starts_with(word, "копе")) return "RUB";
	if (word == "kopecks" || word == "kopeks" || word == "kopeck" || word == "kopek") return "RUB";
	if (word == "cent" || word == "cents" || starts_with(word, "цент")) return "*";
	if (word == "pence" || word == "penny" || starts_with(word, "пенс")) return "GBP";
	return "";
}

bool percent_word(const std::string &raw) {
	const std::string word = without_dot(raw);
	return word == "percent" || word == "pct" || word == "percentage" || starts_with(word, "процент");
}

std::optional<DurationUnit> base_unit_word(const std::string &raw) {
	const std::string word = without_dot(raw);
	static const char *days[] = {"day", "days", "день", "дня", "дней", "дню", "днями", "днях", "дн", "сутки", "суток"};
	for (const char *form : days)
		if (word == form) return DurationUnit::Day;
	if (word == "week" || word == "weeks" || starts_with(word, "недел")) return DurationUnit::Week;
	if (word == "month" || word == "months" || starts_with(word, "месяц")) return DurationUnit::Month;
	static const char *years[] = {"year", "years", "год", "года", "лет", "году", "годом", "годах", "годам"};
	for (const char *form : years)
		if (word == form) return DurationUnit::Year;
	// The protected computation protocol's own units (DESIGN4 §2.4): hours and
	// minutes (an SLA's downtime), quarters. Whole words only: «часть» is no
	// hour, «минимум» no minute.
	static const char *hours[] = {"hour", "hours", "hr", "hrs", "час", "часа", "часов", "часу", "часам", "часами", "часах"};
	for (const char *form : hours)
		if (word == form) return DurationUnit::Hour;
	static const char *minutes[] = {"minute", "minutes", "min", "mins", "минута", "минуты", "минут", "минуту", "минутой",
		"минутам", "минутами", "минутах"};
	for (const char *form : minutes)
		if (word == form) return DurationUnit::Minute;
	static const char *quarters[] = {"quarter", "quarters", "квартал", "квартала", "кварталов", "кварталу", "кварталом",
		"кварталам", "кварталами", "кварталах"};
	for (const char *form : quarters)
		if (word == form) return DurationUnit::Quarter;
	return std::nullopt;
}

// "calendar", "business", «календарных», «рабочих»: 0 none, 1 calendar, 2 business.
int day_modifier(const std::string &raw) {
	const std::string word = without_dot(raw);
	if (word == "calendar" || starts_with(word, "календарн")) return 1;
	if (word == "business" || word == "working" || word == "banking" || starts_with(word, "рабоч") || starts_with(word, "банковск"))
		return 2;
	return 0;
}

// A unit of time at `words[at]`, and how many words it takes.
std::optional<std::pair<DurationUnit, std::size_t>> unit_at(const std::vector<std::string> &words, std::size_t at) {
	if (at >= words.size()) return std::nullopt;
	if (const int modifier = day_modifier(words[at]); modifier != 0 && at + 1 < words.size()) {
		const auto unit = base_unit_word(words[at + 1]);
		if (!unit) return std::nullopt;
		if (modifier == 2) {
			if (*unit != DurationUnit::Day) return std::nullopt;
			return std::make_pair(DurationUnit::BusinessDay, std::size_t(2));
		}
		return std::make_pair(*unit, std::size_t(2));
	}
	if (const auto unit = base_unit_word(words[at])) return std::make_pair(*unit, std::size_t(1));
	return std::nullopt;
}

struct BasisPhrase {
	std::vector<std::string> words;
	RateBasis basis;
	bool open;  // the words of the clause that follow belong to it ("за каждый день просрочки")
};

const std::vector<BasisPhrase> &basis_phrases() {
	static const std::vector<BasisPhrase> all{
		{{"per", "day"}, RateBasis::PerDay, false}, {{"per", "diem"}, RateBasis::PerDay, false},
		{{"per", "calendar", "day"}, RateBasis::PerDay, false}, {{"daily"}, RateBasis::PerDay, false},
		{{"a", "day"}, RateBasis::PerDay, false}, {{"each", "day"}, RateBasis::PerDay, false},
		{{"for", "each", "day"}, RateBasis::PerDay, true}, {{"for", "every", "day"}, RateBasis::PerDay, true},
		{{"for", "each", "calendar", "day"}, RateBasis::PerDay, true}, {{"в", "день"}, RateBasis::PerDay, false},
		{{"в", "сутки"}, RateBasis::PerDay, false}, {{"за", "каждый", "день"}, RateBasis::PerDay, true},
		{{"за", "каждый", "календарный", "день"}, RateBasis::PerDay, true}, {{"за", "день"}, RateBasis::PerDay, true},
		{{"ежедневно"}, RateBasis::PerDay, false},
		{{"per", "annum"}, RateBasis::PerYear, false}, {{"per", "year"}, RateBasis::PerYear, false},
		{{"a", "year"}, RateBasis::PerYear, false}, {{"annual"}, RateBasis::PerYear, false},
		{{"annually"}, RateBasis::PerYear, false}, {{"yearly"}, RateBasis::PerYear, false},
		{{"p.a"}, RateBasis::PerYear, false}, {{"годовых"}, RateBasis::PerYear, false},
		{{"годовые"}, RateBasis::PerYear, false}, {{"в", "год"}, RateBasis::PerYear, false},
		{{"ежегодно"}, RateBasis::PerYear, false},
		{{"per", "month"}, RateBasis::PerMonth, false}, {{"a", "month"}, RateBasis::PerMonth, false},
		{{"monthly"}, RateBasis::PerMonth, false}, {{"в", "месяц"}, RateBasis::PerMonth, false},
		{{"ежемесячно"}, RateBasis::PerMonth, false},
	};
	return all;
}

// ---------------------------------------------------------------------------
// Reading a written value: tokens.

enum class TokenType { Number, Word, Symbol };

struct Token {
	TokenType type = TokenType::Symbol;
	std::string text;
	Rational number;
	int decimals = 0;
	int digits = 0;
	// "61,300" read the other way ("61,3" in a Russian text, "61 300" in an
	// English one): the words beside it or the currency before it may settle it.
	std::optional<Rational> other;
	int other_decimals = 0;
};

// "18 400 000", "1 183 452,05", "61,300.00", "2,8", "1.234.567": the
// separators are read from the digits. With both marks the later one is the
// decimal; spaces group, so a mark after them is the decimal; one comma before
// exactly three digits groups thousands, unless the text is Russian - and then
// the thousands reading is kept as the other one.
std::optional<Token> read_number(const std::string &raw, bool russian) {
	std::vector<std::string> groups(1);
	std::vector<char> separators;
	for (const char character : raw) {
		if (is_digit(character)) {
			groups.back().push_back(character);
		} else {
			separators.push_back(character);
			groups.emplace_back();
		}
	}
	const auto count = [&separators](char mark) { return std::count(separators.begin(), separators.end(), mark); };
	const long spaces = count(' ');
	const long commas = count(',');
	const long points = count('.');
	char decimal = 0;
	bool ambiguous = false;
	if (spaces > 0) {
		if (commas + points > 1) return std::nullopt;
		if (commas + points == 1) {
			if (separators.back() == ' ') return std::nullopt;
			decimal = separators.back();
		}
	} else if (commas > 0 && points > 0) {
		decimal = separators.back();
		if (count(decimal) != 1) return std::nullopt;
	} else if (commas == 1) {
		ambiguous = groups[1].size() == 3 && groups[0].size() <= 3 && groups[0] != "0";
		if (!ambiguous || russian) decimal = ',';
	} else if (points == 1) {
		// "150.000 руб.", "€150.000": the continental way of grouping. Read as
		// a decimal, with the thousands reading kept for a currency to settle
		// (no currency writes three decimals).
		decimal = '.';
		ambiguous = groups[1].size() == 3 && groups[0].size() <= 3 && groups[0] != "0";
	}
	// Every grouping separator stands before exactly three digits, the first
	// group has one to three.
	const std::size_t groupsEnd = decimal != 0 ? groups.size() - 1 : groups.size();
	if (groupsEnd > 1) {
		if (groups[0].empty() || groups[0].size() > 3) return std::nullopt;
		for (std::size_t index = 1; index < groupsEnd; ++index)
			if (groups[index].size() != 3) return std::nullopt;
	}
	std::string digits;
	for (std::size_t index = 0; index < groupsEnd; ++index) digits += groups[index];
	Token token;
	token.type = TokenType::Number;
	token.text = raw;
	if (decimal != 0) {
		token.decimals = static_cast<int>(groups.back().size());
		digits += "." + groups.back();
	}
	const auto value = Rational::parse_decimal(digits);
	if (!value) return std::nullopt;
	token.number = *value;
	token.digits = static_cast<int>(groupsEnd == 1 && decimal == 0 ? groups[0].size() : digits.size());
	if (ambiguous) {
		token.other = decimal != 0 ? Rational::parse_decimal(groups[0] + groups[1]) : Rational::parse_decimal(groups[0] + "." + groups[1]);
		token.other_decimals = decimal != 0 ? 0 : 3;
	}
	return token;
}

Token plain_token(TokenType type, std::string text) {
	Token token;
	token.type = type;
	token.text = std::move(text);
	return token;
}

std::optional<std::vector<Token>> tokens_of(const std::string &text) {
	const bool russian = has_cyrillic(text);
	std::vector<Token> tokens;
	std::size_t index = 0;
	const std::size_t size = text.size();
	while (index < size) {
		const char character = text[index];
		if (character == ' ') { ++index; continue; }
		if (is_digit(character)) {
			std::string raw;
			while (index < size && is_digit(text[index])) raw.push_back(text[index++]);
			for (;;) {
				// "18 400 000": a space before exactly three digits groups them.
				if (text[index] == ' ' && index + 3 < size && is_digit(text[index + 1]) && is_digit(text[index + 2])
					&& is_digit(text[index + 3]) && (index + 4 >= size || !is_digit(text[index + 4]))
					&& raw.find_first_of(",.") == std::string::npos) {
					raw += text.substr(index, 4);
					index += 4;
					continue;
				}
				if ((text[index] == ',' || text[index] == '.') && index + 1 < size && is_digit(text[index + 1])) {
					raw.push_back(text[index++]);
					while (index < size && is_digit(text[index])) raw.push_back(text[index++]);
					continue;
				}
				break;
			}
			auto number = read_number(raw, russian);
			if (!number) return std::nullopt;
			tokens.push_back(*number);
			continue;
		}
		if (is_ascii_letter(character) || cyrillic_at(text, index)) {
			std::string word;
			while (index < size) {
				if (is_ascii_letter(text[index])) { word.push_back(text[index++]); continue; }
				if (cyrillic_at(text, index)) { word += text.substr(index, 2); index += 2; continue; }
				if (text[index] == '-' && !word.empty() && index + 1 < size
					&& (is_ascii_letter(text[index + 1]) || cyrillic_at(text, index + 1))) {
					word.push_back(text[index++]);
					continue;
				}
				break;
			}
			if (word == "us" && index < size && text[index] == '$') {
				tokens.push_back(plain_token(TokenType::Symbol, "us$"));
				++index;
				continue;
			}
			// "p.a." is one word.
			if (word == "p" && starts_at(text, index, ".a")) { word = "p.a"; index += 2; }
			if (index < size && text[index] == '.') { word.push_back('.'); ++index; }
			tokens.push_back(plain_token(TokenType::Word, word));
			continue;
		}
		bool matched = false;
		for (const char *symbol : {"$", "%", "(", ")", "-", "+", "/", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"}) {
			if (starts_at(text, index, symbol)) {
				tokens.push_back(plain_token(TokenType::Symbol, symbol));
				index += std::string_view(symbol).size();
				matched = true;
				break;
			}
		}
		if (!matched) return std::nullopt;
	}
	return tokens;
}

bool is_symbol(const Token &token, std::string_view symbol) { return token.type == TokenType::Symbol && token.text == symbol; }

bool is_number_word(const Token &token) { return token.type == TokenType::Word && number_word(without_dot(token.text)); }

// The fraction a number in words may end with: «ноль целых одна десятая»,
// "zero point one", "five tenths".
bool fraction_word(const std::string &word) {
	if (word == "point" || starts_with(word, "tenth") || starts_with(word, "hundredth") || starts_with(word, "thousandth")) return true;
	if (starts_with(word, "цел")) return true;
	for (const char *stem : {"десят", "сот", "тысячн", "десятитысячн", "стотысячн", "миллионн"})
		if (starts_with(word, stem))
			for (const char *ending : {"ая", "ых", "ой", "ую", "ые"})
				if (word.size() == std::string(stem).size() + std::string(ending).size() && word.compare(std::string(stem).size(), std::string::npos, ending) == 0)
					return true;
	return false;
}

// Words a parenthesised amount-in-words may hold.
bool amount_word(const Token &token) {
	if (token.type != TokenType::Word) return false;
	const std::string word = without_dot(token.text);
	return number_word(word) || fraction_word(word) || word == "and" || word == "и" || !currency_of_word(word).empty()
		|| !subunit_of_word(word).empty() || percent_word(word) || base_unit_word(word) || day_modifier(word) != 0 || scale_word(word)
		|| word == "сша" || word == "стерлингов" || word == "sterling"
		// "US dollars", "U.S. dollars", "United States dollars".
		|| word == "us" || word == "u" || word == "s" || word == "united" || word == "states" || word == "of" || word == "america";
}

// What an amount-in-words beside digits says: its currency, whether it
// writes the minor unit ("рублей 00 копеек", "and 00/100 dollars"), and the
// whole number its words spell, when they spell one.
struct WordsNote {
	std::string currency;
	bool cents = false;
	bool fraction = false;  // «одна целая пять десятых»: the words write a fraction
	std::optional<Rational> value;
};

bool read_words_group(const std::vector<Token> &tokens, std::size_t open, std::size_t close, WordsNote &note) {
	std::vector<std::string> run;
	bool runDone = false;
	bool fraction = false;
	for (std::size_t index = open + 1; index < close; ++index) {
		const Token &token = tokens[index];
		if (token.type == TokenType::Number) {
			// Only the minor unit is written in digits: "00 копеек", "00/100".
			if (!token.number.is_integer() || token.decimals != 0 || token.text.size() > 2) return false;
			runDone = true;
			if (index + 2 < close && is_symbol(tokens[index + 1], "/") && tokens[index + 2].type == TokenType::Number && tokens[index + 2].text == "100") {
				note.cents = true;
				index += 2;
				continue;
			}
			if (index + 1 < close && tokens[index + 1].type == TokenType::Word && !subunit_of_word(without_dot(tokens[index + 1].text)).empty()) {
				note.cents = true;
				continue;
			}
			return false;
		}
		if (!amount_word(token)) return false;
		const std::string word = without_dot(token.text);
		if (fraction_word(word)) fraction = true;
		if (const std::string currency = currency_of_word(word); !currency.empty()) {
			if (!note.currency.empty() && note.currency != currency) return false;
			note.currency = currency;
		}
		const bool joins = (word == "and" || word == "и") && !run.empty() && index + 1 < close && is_number_word(tokens[index + 1]);
		if (!runDone && (is_number_word(token) || joins)) {
			run.push_back(word);
		} else if (!run.empty()) {
			runDone = true;
		}
	}
	note.fraction = fraction;
	if (!fraction && !run.empty()) {
		try {
			note.value = words_value(run);
		} catch (const AlgebraError &) {
			note.value = std::nullopt;
		}
	}
	return true;
}

// A value written twice - in digits and in words - read once, from the digits.
// "18 400 000 (восемнадцать миллионов …) рублей", "ten (10) days",
// "twenty thousand dollars ($20,000)", and an accounting negative "$(1,250)".
std::optional<std::vector<Token>> resolved_parentheses(const std::vector<Token> &tokens, bool &negative, WordsNote &note) {
	struct Group {
		std::size_t open;
		std::size_t close;
		bool hasNumber;
	};
	std::vector<Group> groups;
	for (std::size_t index = 0; index < tokens.size(); ++index) {
		if (is_symbol(tokens[index], ")")) return std::nullopt;
		if (!is_symbol(tokens[index], "(")) continue;
		std::size_t close = index + 1;
		bool hasNumber = false;
		while (close < tokens.size() && !is_symbol(tokens[close], ")")) {
			if (is_symbol(tokens[close], "(")) return std::nullopt;
			hasNumber = hasNumber || tokens[close].type == TokenType::Number;
			++close;
		}
		if (close >= tokens.size() || close == index + 1) return std::nullopt;
		groups.push_back({index, close, hasNumber});
		index = close;
	}
	if (groups.empty()) return tokens;
	std::vector<Token> outside;
	std::vector<std::size_t> outsideAt;  // where each outside token stood
	bool outsideNumber = false;
	bool outsideWords = false;
	for (std::size_t index = 0, group = 0; index < tokens.size(); ++index) {
		if (group < groups.size() && index == groups[group].open) {
			index = groups[group].close;
			++group;
			continue;
		}
		outside.push_back(tokens[index]);
		outsideAt.push_back(index);
		outsideNumber = outsideNumber || tokens[index].type == TokenType::Number;
		outsideWords = outsideWords || is_number_word(tokens[index]);
	}
	if (outsideNumber) {
		// Digits outside: every parenthesis is the amount in words, and goes.
		for (const Group &group : groups) {
			WordsNote read;
			if (!read_words_group(tokens, group.open, group.close, read)) return std::nullopt;
			if (!read.currency.empty()) {
				if (!note.currency.empty() && note.currency != read.currency) return std::nullopt;
				note.currency = read.currency;
			}
			note.cents = note.cents || read.cents;
			note.fraction = note.fraction || read.fraction;
			if (!note.value) note.value = read.value;
		}
		return outside;
	}
	if (groups.size() != 1 || !groups.front().hasNumber) return std::nullopt;
	const Group &group = groups.front();
	std::vector<Token> inside(tokens.begin() + static_cast<long>(group.open) + 1, tokens.begin() + static_cast<long>(group.close));
	if (!outsideWords) {
		// "(1,250)" with a currency: an accounting negative.
		if (!inside.empty() && (is_symbol(inside.front(), "-") || is_symbol(inside.front(), "+"))) return std::nullopt;
		negative = true;
		std::vector<Token> result(tokens.begin(), tokens.begin() + static_cast<long>(group.open));
		result.insert(result.end(), inside.begin(), inside.end());
		result.insert(result.end(), tokens.begin() + static_cast<long>(group.close) + 1, tokens.end());
		return result;
	}
	// Words outside, digits inside: the digits stand where the words were.
	std::vector<Token> result;
	bool replaced = false;
	for (std::size_t index = 0; index < outside.size(); ++index) {
		const bool word = is_number_word(outside[index])
			|| (outside[index].type == TokenType::Word && without_dot(outside[index].text) == "and" && index > 0
				&& index + 1 < outside.size() && is_number_word(outside[index - 1]) && is_number_word(outside[index + 1]));
		if (word) {
			if (replaced && (index == 0 || !(is_number_word(outside[index - 1]) || without_dot(outside[index - 1].text) == "and")))
				return std::nullopt;  // a second amount in words
			if (!replaced) {
				result.insert(result.end(), inside.begin(), inside.end());
				replaced = true;
			}
			continue;
		}
		result.push_back(outside[index]);
	}
	return result;
}

struct Reading {
	Rational amount;
	int decimals = 0;
	std::string currency;
	bool percent = false;
	std::optional<DurationUnit> unit;
	RateBasis basis = RateBasis::None;
	bool negative = false;
};

bool set_currency(Reading &reading, const std::string &currency) {
	if (currency.empty()) return false;
	if (!reading.currency.empty() && reading.currency != currency) return false;
	reading.currency = currency;
	return true;
}

std::optional<Reading> read_quantity(const std::vector<Token> &input) {
	Reading reading;
	std::vector<Token> tokens = input;
	WordsNote note;
	auto resolved = resolved_parentheses(tokens, reading.negative, note);
	if (!resolved) return std::nullopt;
	tokens = *resolved;
	std::vector<std::string> words;
	for (const Token &token : tokens) words.push_back(token.type == TokenType::Word ? without_dot(token.text) : std::string());
	std::size_t at = 0;
	const std::size_t size = tokens.size();
	// Signs and a currency before the amount: "-$5", "$-5", "US$ 2.8", "USD 1,000".
	bool sawSign = false;
	bool markBefore = false;  // "$", "€", "USD" before the digits: written the English way
	std::optional<Rational> thousands;  // "1,000" still open to the thousands reading
	while (at < size) {
		const Token &token = tokens[at];
		if (is_symbol(token, "-") || is_symbol(token, "+")) {
			if (sawSign) return std::nullopt;
			sawSign = true;
			reading.negative = reading.negative != is_symbol(token, "-");
			++at;
			continue;
		}
		if (token.type == TokenType::Symbol && !currency_of_symbol(token.text).empty()) {
			if (!set_currency(reading, currency_of_symbol(token.text))) return std::nullopt;
			markBefore = markBefore || currency_of_symbol(token.text) != "RUB";
			++at;
			continue;
		}
		if (token.type == TokenType::Word && !currency_of_code(words[at]).empty()) {
			if (!set_currency(reading, currency_of_code(words[at]))) return std::nullopt;
			markBefore = true;
			++at;
			continue;
		}
		break;
	}
	if (at >= size) return std::nullopt;
	if (tokens[at].type == TokenType::Number) {
		const Token &number = tokens[at];
		reading.amount = number.number;
		reading.decimals = number.decimals;
		// "$2.125 million" is a decimal before a scale, never 2 125 million.
		const bool scaled = at + 1 < size && tokens[at + 1].type == TokenType::Word && scale_word(words[at + 1]).has_value();
		if (number.other && !(scaled && number.text.find('.') != std::string::npos)) {
			// "$61,300 (шестьдесят одна тысяча триста долларов США)": the words
			// say which reading the comma has; failing them, a currency written
			// before the digits writes them the English way ("USD 1,500").
			const bool wordsSayThis = note.value && *note.value == number.number;
			const bool wordsSayOther = note.value && *note.value == *number.other;
			if (!wordsSayThis && (wordsSayOther || (markBefore && number.other_decimals == 0))) {
				reading.amount = *number.other;
				reading.decimals = number.other_decimals;
			} else if (!wordsSayThis && !note.fraction && number.other_decimals == 0) {
				thousands = number.other;
			}
		}
		++at;
		if (at < size && tokens[at].type == TokenType::Word) {
			if (const auto scale = scale_word(words[at])) {
				reading.amount = reading.amount * Rational(*scale);
				reading.decimals = 0;
				++at;
			}
		}
	} else if (is_number_word(tokens[at])) {
		std::vector<std::string> run;
		while (at < size && tokens[at].type == TokenType::Word
			&& (is_number_word(tokens[at])
				|| ((words[at] == "and") && at + 1 < size && is_number_word(tokens[at + 1])))) {
			run.push_back(words[at]);
			++at;
		}
		const auto value = words_value(run);
		if (!value) return std::nullopt;
		reading.amount = *value;
		// "three quarters of the price" is a fraction, not nine months.
		if (at < size && tokens[at].type == TokenType::Word && base_unit_word(words[at]) == DurationUnit::Quarter) return std::nullopt;
	} else {
		return std::nullopt;
	}
	while (at < size) {
		const Token &token = tokens[at];
		if (token.type == TokenType::Symbol) {
			if (token.text == "%") { reading.percent = true; ++at; continue; }
			if (!currency_of_symbol(token.text).empty()) {
				if (!set_currency(reading, currency_of_symbol(token.text))) return std::nullopt;
				++at;
				continue;
			}
			return std::nullopt;
		}
		if (token.type == TokenType::Number || (words[at] == "and" && at + 1 < size && tokens[at + 1].type == TokenType::Number)) {
			// "рублей 00 копеек", "dollars and 50 cents".
			const std::size_t numberAt = token.type == TokenType::Number ? at : at + 1;
			const Token &minor = tokens[numberAt];
			if (numberAt + 1 >= size || !minor.number.is_integer() || minor.decimals != 0 || minor.text.size() > 2) return std::nullopt;
			const std::string owner = subunit_of_word(words[numberAt + 1]);
			if (owner.empty() || reading.currency.empty()) return std::nullopt;
			if (owner != "*" && owner != reading.currency) return std::nullopt;
			if (reading.decimals > 0) return std::nullopt;
			reading.amount = reading.amount + minor.number / Rational(100);
			reading.decimals = 2;
			at = numberAt + 2;
			continue;
		}
		const std::string &word = words[at];
		// "US dollars", "U.S. dollars", "United States dollars".
		const std::size_t country = word == "us" ? 1
			: at + 1 < size && ((word == "u" && words[at + 1] == "s") || (word == "united" && words[at + 1] == "states")) ? 2 : 0;
		if (country != 0 && at + country < size && currency_of_word(words[at + country]) == "USD") {
			at += country;
			continue;
		}
		if (!currency_of_word(word).empty()) {
			if (!set_currency(reading, currency_of_word(word))) return std::nullopt;
			++at;
			// «долларов США», "pounds sterling".
			if (at < size && (words[at] == "сша" || words[at] == "sterling" || words[at] == "стерлингов")) ++at;
			continue;
		}
		if (percent_word(word)) { reading.percent = true; ++at; continue; }
		if (word == "per" && at + 1 < size && words[at + 1] == "cent") { reading.percent = true; at += 2; continue; }
		if (const auto unit = unit_at(words, at)) {
			if (reading.unit && *reading.unit != unit->first) return std::nullopt;
			reading.unit = unit->first;
			at += unit->second;
			continue;
		}
		bool phrase = false;
		for (const BasisPhrase &basis : basis_phrases()) {
			if (at + basis.words.size() > size) continue;
			bool same = true;
			for (std::size_t index = 0; index < basis.words.size() && same; ++index)
				same = tokens[at + index].type == TokenType::Word && words[at + index] == basis.words[index];
			if (!same) continue;
			reading.basis = basis.basis;
			at += basis.words.size();
			if (basis.open)
				while (at < size && tokens[at].type == TokenType::Word) ++at;
			phrase = true;
			break;
		}
		if (phrase) continue;
		return std::nullopt;
	}
	// "100 000 (Сто тысяч рублей 00 копеек)": the currency is in the words.
	if (!note.currency.empty()) {
		if (reading.percent || reading.unit || !set_currency(reading, note.currency)) return std::nullopt;
	}
	// No currency writes three decimals: "1,000 долларов США" is a thousand.
	if (thousands && !reading.currency.empty() && reading.decimals == 3) {
		reading.amount = *thousands;
		reading.decimals = 0;
	}
	if (note.cents && !reading.currency.empty() && reading.decimals == 0) reading.decimals = 2;
	if (reading.basis != RateBasis::None && !reading.percent) return std::nullopt;
	if (reading.percent && (!reading.currency.empty() || reading.unit)) return std::nullopt;
	if (!reading.currency.empty() && reading.unit) return std::nullopt;
	if (reading.negative) reading.amount = -reading.amount;
	return reading;
}

// ---------------------------------------------------------------------------
// Calendar days.

long long days_from_civil(long long year, unsigned month, unsigned day) {
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(year - era * 400);
	const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<long long>(doe) - 719468;
}

struct Civil {
	long long year;
	unsigned month;
	unsigned day;
};

Civil civil_from_days(long long days) {
	days += 719468;
	const long long era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(days - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long year = static_cast<long long>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	const unsigned day = doy - (153 * mp + 2) / 5 + 1;
	const unsigned month = mp < 10 ? mp + 3 : mp - 9;
	if (month <= 2) ++year;
	return {year, month, day};
}

bool leap_year(long long year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

unsigned month_length(long long year, unsigned month) {
	static const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month == 2 && leap_year(year) ? 29 : lengths[month - 1];
}

temporal::TemporalValue day_value(long long days) { return {days, days, temporal::Precision::Day}; }

temporal::TemporalValue month_value(long long year, unsigned month) {
	return {days_from_civil(year, month, 1), days_from_civil(year, month, month_length(year, month)), temporal::Precision::Month};
}

std::string date_style_of(const std::string &text) {
	static const std::regex iso("^\\d{4}-\\d{1,2}-\\d{1,2}$");
	static const std::regex dotted("^\\d{1,2}\\.\\d{1,2}\\.\\d{4}$");
	if (std::regex_match(text, iso)) return "iso";
	if (std::regex_match(text, dotted)) return "dotted";
	if (text.find('/') != std::string::npos) return "slashed";
	if (has_cyrillic(text)) return "russian";
	return "english";
}

// A cell's note of how it is shown: "(shown in ₽)", "(shown in $ in millions)",
// "(shown as a percentage)".
struct Shown {
	std::string currency;
	bool percent = false;
	bool date = false;
	Int128 scale = 1;
};

std::optional<Shown> read_shown(const std::string &note) {
	Shown shown;
	std::string rest = note;
	if (!starts_with(rest, "shown")) return std::nullopt;
	for (const char *symbol : {"us$", "$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"})
		if (rest.find(symbol) != std::string::npos) {
			shown.currency = currency_of_symbol(symbol);
			break;
		}
	std::vector<std::string> words;
	std::string word;
	for (const char character : rest + " ") {
		if (is_ascii_letter(character) || static_cast<unsigned char>(character) >= 0x80) {
			word.push_back(character);
		} else {
			if (!word.empty()) words.push_back(word);
			word.clear();
		}
	}
	for (const std::string &each : words) {
		if (shown.currency.empty() && !currency_of_word(each).empty()) shown.currency = currency_of_word(each);
		if (percent_word(each) || each == "percentages") shown.percent = true;
		if (each == "date") shown.date = true;
		if (const auto scale = scale_word(each)) shown.scale = *scale;
	}
	return shown;
}

// temporal::parse takes any word that starts like a month for the month
// ("Decree 10, 2026", "Marketing 2026"): a date's English words are a month's
// name or its abbreviation, or say which quarter.
bool english_date_words(const std::string &lower) {
	static const char *allowed[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
		"november", "december", "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec", "q", "quarter", "of",
		"first", "second", "third", "fourth", "st", "nd", "rd", "th", "i", "ii", "iii", "iv"};
	std::string word;
	for (const char character : lower + " ") {
		if (is_ascii_letter(character)) {
			word.push_back(character);
			continue;
		}
		if (!word.empty() && std::find_if(std::begin(allowed), std::end(allowed), [&word](const char *each) { return word == each; }) == std::end(allowed))
			return false;
		word.clear();
	}
	return true;
}

// How Russian contracts write the listed forms: a day in straight or curly
// quotes ("10" февраля), «г.» against the year (2026г.), and the year with
// «год» in any case («в 2026 году»).
std::string russian_date_forms(const std::string &text, std::string &bare) {
	static const std::regex quoted("^(?:\"|\xE2\x80\x9C|\xE2\x80\x9D|\xE2\x80\x9E)(\\d{1,2})(?:\"|\xE2\x80\x9C|\xE2\x80\x9D) ");
	static const std::regex year("(\\d{4}) ?(?:\xD0\xB3\\.?|\xD0\xB3\xD0\xBE\xD0\xB4(?:\xD0\xB0|\xD1\x83|\xD0\xB5)?)$");
	const std::string unquoted = std::regex_replace(text, quoted, "\xC2\xAB$1\xC2\xBB ");
	bare = std::regex_replace(unquoted, year, "$1");
	return std::regex_replace(unquoted, year, "$1 \xD0\xB3.");
}

std::optional<Value> read_date(const std::string &raw) {
	if (!english_date_words(lowered(raw))) return std::nullopt;
	// "February 10th, 2026", "10th of February 2026": the day's ordinal letters
	// and its "of" are how the day is said, not part of it.
	static const std::regex ordinal("(\\d)(?:st|nd|rd|th|ST|ND|RD|TH)\\b(?: of\\b| OF\\b)?");
	const std::string written = std::regex_replace(raw, ordinal, "$1");
	std::string bare;
	const std::string text = russian_date_forms(written, bare);
	auto date = temporal::parse(text);
	if (!date) {
		// "July 1", «1 июля»: a day of the year, which no year places in time.
		const auto day = temporal::parse_day_of_year(bare);
		if (!day) return std::nullopt;
		return make_day_of_year(day->month, day->day, has_cyrillic(bare) ? "russian" : "english");
	}
	std::string style = date_style_of(lowered(bare));
	if (style == "english" && has_cyrillic(text)) style = "russian";
	return make_date(*date, style);
}

} // namespace

// ---------------------------------------------------------------------------
// Values

Value make_money(Rational amount, std::string currency, int source_decimals) {
	Value value;
	value.kind = Kind::Money;
	value.amount = amount;
	value.currency = std::move(currency);
	value.source_decimals = source_decimals;
	return value;
}

Value make_number(Rational amount) {
	Value value;
	value.kind = Kind::Number;
	value.amount = amount;
	return value;
}

Value make_percentage(Rational fraction, RateBasis basis) {
	Value value;
	value.kind = Kind::Percentage;
	value.amount = fraction;
	value.basis = basis;
	return value;
}

Value make_duration(Rational count, DurationUnit unit) {
	Value value;
	value.kind = Kind::Duration;
	value.amount = count;
	value.unit = unit;
	return value;
}

Value make_date(const temporal::TemporalValue &date, std::string style) {
	Value value;
	value.kind = Kind::Date;
	value.date = date;
	value.date_style = std::move(style);
	return value;
}

Value make_boolean(bool truth) {
	Value value;
	value.kind = Kind::Boolean;
	value.truth = truth;
	return value;
}

Value make_day_of_year(unsigned month, unsigned day, std::string style) {
	Value value;
	value.kind = Kind::DayOfYear;
	value.day_of_year = temporal::DayOfYear{month, day};
	value.date_style = std::move(style);
	return value;
}

namespace {

// The exact decimal, or the fraction when it has none (or none that fits):
// an identity is never refused, so it never throws.
std::string canonical(const Rational &number) {
	try {
		if (auto exact = number.exact_decimal(36)) return *exact;
	} catch (const AlgebraError &) {
	}
	return number.to_string();
}

const char *unit_name(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Day: return "Day";
		case DurationUnit::Week: return "Week";
		case DurationUnit::Month: return "Month";
		case DurationUnit::Year: return "Year";
		case DurationUnit::BusinessDay: return "BusinessDay";
		case DurationUnit::Quarter: return "Quarter";
		case DurationUnit::Hour: return "Hour";
		case DurationUnit::Minute: return "Minute";
	}
	return "Day";
}

std::string two_digits(unsigned number) {
	return std::string(1, static_cast<char>('0' + number / 10)) + static_cast<char>('0' + number % 10);
}

} // namespace

std::string value_key(const Value &value) {
	switch (value.kind) {
		case Kind::Money: return "MONEY:" + (value.currency.empty() ? std::string("?") : value.currency) + ":" + canonical(value.amount);
		case Kind::Number: return "NUM:" + canonical(value.amount);
		case Kind::Count: return "CNT:" + canonical(value.amount);
		case Kind::Percentage: return "PCT:" + value.amount.to_string();
		case Kind::Duration: return std::string("DUR:") + unit_name(value.unit) + ":" + canonical(value.amount);
		case Kind::Boolean: return value.truth ? "BOOL:true" : "BOOL:false";
		case Kind::DayOfYear: return "DOY:" + two_digits(value.day_of_year.month) + two_digits(value.day_of_year.day);
		case Kind::Date: {
			const Civil first = civil_from_days(value.date.first_day);
			const std::string year = std::to_string(first.year);
			switch (value.date.precision) {
				case temporal::Precision::Day: return "DATE:D:" + year + two_digits(first.month) + two_digits(first.day);
				case temporal::Precision::Month: return "DATE:M:" + year + two_digits(first.month);
				case temporal::Precision::Quarter: return "DATE:Q:" + year + "Q" + std::to_string((first.month - 1) / 3 + 1);
				case temporal::Precision::Year: return "DATE:Y:" + year;
			}
		}
	}
	return "";
}

std::string currency_named(std::string_view label) {
	const std::string lower = lowered(normalized(label));
	for (const char *symbol : {"us$", "$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"})
		if (lower.find(symbol) != std::string::npos) return currency_of_symbol(symbol);
	std::string word;
	for (const char character : lower + " ") {
		if (is_ascii_letter(character) || static_cast<unsigned char>(character) >= 0x80 || character == '.') {
			word.push_back(character);
			continue;
		}
		if (!word.empty()) {
			if (const std::string currency = currency_of_word(word); !currency.empty()) return currency;
			// «р.», the ruble's shortest writing.
			if (word == "\xD1\x80.") return "RUB";
		}
		word.clear();
	}
	return "";
}

std::optional<NamedMeasure> measure_named(std::string_view label) {
	const std::string lower = lowered(normalized(label));
	if (std::any_of(lower.begin(), lower.end(), [](char character) { return character >= '0' && character <= '9'; }))
		return std::nullopt;
	if (!currency_named(label).empty()) return std::nullopt;
	std::vector<std::string> words;
	std::string word;
	for (std::size_t index = 0; index <= lower.size(); ++index) {
		if (index < lower.size() && cyrillic_at(lower, index)) {
			word += lower.substr(index, 2);
			++index;
			continue;
		}
		if (index < lower.size() && (is_ascii_letter(lower[index]) || (lower[index] == '-' && !word.empty()))) {
			word.push_back(lower[index]);
			continue;
		}
		if (!word.empty()) words.push_back(word);
		word.clear();
	}
	const auto has = [&](std::initializer_list<const char *> forms) {
		return std::any_of(words.begin(), words.end(), [&](const std::string &each) {
			return std::any_of(forms.begin(), forms.end(), [&](const char *form) { return each == form; });
		});
	};
	if (has({"days", "\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9", "\xD0\xB4\xD0\xBD\xD0\xB8", "\xD0\xB4\xD0\xBD"})) {  // дней, дни, дн
		const bool business = has({"business", "working", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD1\x85"});  // рабочих
		return NamedMeasure{Kind::Duration, business ? DurationUnit::BusinessDay : DurationUnit::Day};
	}
	if (has({"months", "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86\xD0\xB5\xD0\xB2", "\xD0\xBC\xD0\xB5\xD1\x81"}))  // месяцев, мес
		return NamedMeasure{Kind::Duration, DurationUnit::Month};
	if (has({"hours", "\xD1\x87\xD0\xB0\xD1\x81\xD0\xBE\xD0\xB2"}))  // часов
		return NamedMeasure{Kind::Duration, DurationUnit::Hour};
	if (has({"minutes", "\xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82", "\xD0\xBC\xD0\xB8\xD0\xBD"}))  // минут, мин
		return NamedMeasure{Kind::Duration, DurationUnit::Minute};
	const bool numberOf = std::adjacent_find(words.begin(), words.end(), [](const std::string &first, const std::string &second) {
		return first == "number" && second == "of";
	}) != words.end();
	if (numberOf || has({"count", "qty", "quantity", "\xD0\xBA\xD0\xBE\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE",  // количество
			"\xD0\xBA\xD0\xBE\xD0\xBB-\xD0\xB2\xD0\xBE"}))  // кол-во
		return NamedMeasure{Kind::Count, DurationUnit::Day};
	return std::nullopt;
}

std::optional<Rational> parse_number_words(std::string_view text) {
	const auto tokens = tokens_of(lowered(normalized(text)));
	if (!tokens || tokens->empty()) return std::nullopt;
	std::vector<std::string> words;
	for (const Token &token : *tokens) {
		if (token.type != TokenType::Word) return std::nullopt;
		words.push_back(without_dot(token.text));
	}
	try {
		return words_value(words);
	} catch (const AlgebraError &) {
		return std::nullopt;
	}
}

std::optional<Value> parse_value(std::string_view raw, const ParseHints &given) {
	try {
		std::string text = normalized(raw);
		while (!text.empty() && (text.back() == ',' || text.back() == ';' || text.back() == ':')) text.pop_back();
		while (!text.empty() && text.back() == ' ') text.pop_back();
		if (text.empty() || text.size() > 400) return std::nullopt;
		ParseHints hints = given;
		std::string lower = lowered(text);
		// "B12 = 0.001", "B12: 3000000": the cell reference is not the value.
		static const std::regex cell("^\\$?[a-z]{1,3}\\$?[0-9]{1,7} ?[:=] ?");
		std::smatch reference;
		if (std::regex_search(lower, reference, cell)) {
			const std::size_t cut = static_cast<std::size_t>(reference.length(0));
			text = text.substr(cut);
			lower = lower.substr(cut);
		}
		Int128 scale = 1;
		bool serialDate = false;
		// "(shown in ₽)": how the workbook shows the stored number.
		const std::size_t note = lower.rfind("(shown");
		if (note != std::string::npos && !lower.empty() && lower.back() == ')') {
			const auto shown = read_shown(lower.substr(note + 1, lower.size() - note - 2));
			if (!shown) return std::nullopt;
			if (!shown->currency.empty()) hints.currency = shown->currency;
			if (shown->percent) hints.percent_display = true;
			serialDate = shown->date;
			scale = shown->scale;
			text = text.substr(0, note);
			lower = lower.substr(0, note);
			while (!text.empty() && text.back() == ' ') { text.pop_back(); lower.pop_back(); }
			if (text.empty()) return std::nullopt;
		}
		const bool quantityHinted = !hints.currency.empty() || hints.percent_display
			|| (hints.expect && *hints.expect != Kind::Date);
		if (!quantityHinted && !serialDate) {
			if (auto date = read_date(text)) return date;
		}
		if (hints.expect && *hints.expect == Kind::Date && !serialDate) return std::nullopt;
		// A trailing full stop after digits ends a sentence, not the value.
		if (lower.size() > 1 && lower.back() == '.' && is_digit(lower[lower.size() - 2])) lower.pop_back();
		const auto tokens = tokens_of(lower);
		if (!tokens || tokens->empty()) return std::nullopt;
		auto reading = read_quantity(*tokens);
		if (!reading) return std::nullopt;
		if (reading->negative && reading->currency.empty() && hints.currency.empty()) {
			// "(1,250)" is an accounting negative only where it is money.
			bool parenthesised = false;
			for (const Token &token : *tokens) parenthesised = parenthesised || is_symbol(token, "(");
			bool numberOutside = false;
			int depth = 0;
			for (const Token &token : *tokens) {
				if (is_symbol(token, "(")) ++depth;
				else if (is_symbol(token, ")")) --depth;
				else if (depth == 0 && token.type == TokenType::Number) numberOutside = true;
			}
			if (parenthesised && !numberOutside) return std::nullopt;
		}
		Value value;
		if (serialDate) {
			// A workbook date is days since 30 December 1899.
			if (!reading->amount.is_integer() || reading->percent || !reading->currency.empty() || reading->unit) return std::nullopt;
			if (reading->amount < Rational(0) || reading->amount > Rational(200000)) return std::nullopt;
			const long long days = static_cast<long long>(reading->amount.numerator()) + days_from_civil(1899, 12, 30);
			const Civil civil = civil_from_days(days);
			if (civil.year < 1900 || civil.year > 2200) return std::nullopt;
			return make_date(day_value(days), "english");
		}
		if (reading->percent) {
			value = make_percentage(reading->amount / Rational(100), reading->basis);
		} else if (reading->unit) {
			value = make_duration(reading->amount, *reading->unit);
		} else if (!reading->currency.empty()) {
			value = make_money(reading->amount * Rational(scale), reading->currency, scale == 1 ? reading->decimals : 0);
		} else if (hints.percent_display || (hints.expect && *hints.expect == Kind::Percentage)) {
			value = make_percentage(reading->amount, RateBasis::None);
		} else if (!hints.currency.empty() || (hints.expect && *hints.expect == Kind::Money)) {
			value = make_money(reading->amount * Rational(scale), hints.currency, scale == 1 ? reading->decimals : 0);
		} else {
			value = make_number(reading->amount * Rational(scale));
			value.source_decimals = scale == 1 ? reading->decimals : 0;
		}
		if (value.kind == Kind::Money && value.source_decimals < 2 && value.source_decimals > 0) value.source_decimals = 2;
		return value;
	} catch (const AlgebraError &) {
		return std::nullopt;
	}
}

// ---------------------------------------------------------------------------
// The period a rate is for, read from its clause.

namespace {

struct ClauseWord {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string word;
	bool rate = false;  // a figure written as a rate: "5%", "5 percent", «5 процентов»
};

bool clause_letter(std::string_view text, std::size_t at) { return at < text.size() && (is_ascii_letter(text[at]) || cyrillic_at(text, at)); }

// Where a sentence (or a line, a list item, a table cell) ends from `at` on,
// in either direction: a line break, a semicolon, a table's bar, or a full stop
// before a capital or the end - never the dot of "p.a." or of "U.S.".
bool clause_break(std::string_view text, std::size_t at) {
	const char character = text[at];
	if (character == '\n' || character == ';' || character == '|' || character == '\t') return true;
	if (character != '.') return false;
	if (at + 1 >= text.size()) return true;
	if (text[at + 1] != ' ') return false;
	std::size_t next = at + 1;
	while (next < text.size() && text[next] == ' ') ++next;
	if (next >= text.size()) return true;
	const unsigned char first = static_cast<unsigned char>(text[next]);
	if (first >= 'A' && first <= 'Z') return true;
	return first == 0xD0 && next + 1 < text.size() && ((static_cast<unsigned char>(text[next + 1]) >= 0x90
		&& static_cast<unsigned char>(text[next + 1]) <= 0xAF) || static_cast<unsigned char>(text[next + 1]) == 0x81);
}

// The words of text[from, to), lower case, each figure written as a rate marked.
std::vector<ClauseWord> clause_words(std::string_view text, std::size_t from, std::size_t to) {
	const std::string lower = lowered(text.substr(from, to - from));
	std::vector<ClauseWord> words;
	std::size_t at = 0;
	while (at < lower.size()) {
		// A protected symbol is one word, and a rate's symbol ([[Rate4]]) is a
		// rate: the sent text writes the clause's earlier rate so, and a period
		// after it is still that rate's (live R1 c27: «[[Rate4]] ... за каждый
		// день просрочки, но не более 10 %» made the 10 % cap daily).
		if (lower.compare(at, 2, "[[") == 0) {
			const std::size_t close = lower.find("]]", at + 2);
			if (close != std::string::npos && close - at < 48) {
				const std::string inner = lower.substr(at + 2, close - at - 2);
				words.push_back({from + at, from + close + 2, inner, inner.rfind("rate", 0) == 0});
				at = close + 2;
				continue;
			}
		}
		if (clause_letter(lower, at)) {
			std::size_t end = at;
			while (end < lower.size() && (clause_letter(lower, end) || (lower[end] == '.' && clause_letter(lower, end + 1))))
				end += cyrillic_at(lower, end) ? 2 : 1;
			words.push_back({from + at, from + end, lower.substr(at, end - at), false});
			at = end;
			continue;
		}
		if (is_digit(lower[at])) {
			std::size_t end = at;
			while (end < lower.size() && (is_digit(lower[end]) || ((lower[end] == '.' || lower[end] == ',') && end + 1 < lower.size()
					&& is_digit(lower[end + 1]))))
				++end;
			std::size_t after = end;
			while (after < lower.size() && lower[after] == ' ') ++after;
			const bool sign = after < lower.size() && lower[after] == '%';
			std::size_t wordEnd = after;
			while (wordEnd < lower.size() && clause_letter(lower, wordEnd)) wordEnd += cyrillic_at(lower, wordEnd) ? 2 : 1;
			const bool word = wordEnd > after && percent_word(lower.substr(after, wordEnd - after));
			words.push_back({from + at, from + end, lower.substr(at, end - at), sign || word});
			at = end;
			continue;
		}
		at += (static_cast<unsigned char>(lower[at]) & 0x80) != 0 && cyrillic_at(lower, at) ? 2 : 1;
	}
	return words;
}

// A phrase of basis_phrases() at words[at]: its basis and its length in words.
std::optional<std::pair<const BasisPhrase *, std::size_t>> basis_phrase_at(const std::vector<ClauseWord> &words, std::size_t at) {
	for (const BasisPhrase &phrase : basis_phrases()) {
		if (at + phrase.words.size() > words.size()) continue;
		bool same = true;
		for (std::size_t index = 0; index < phrase.words.size() && same; ++index)
			same = !words[at + index].rate && without_dot(words[at + index].word) == phrase.words[index];
		if (same) return std::make_pair(&phrase, phrase.words.size());
	}
	return std::nullopt;
}

// A phrase that names a period by itself anywhere in the clause ("per annum",
// «за каждый день»), or only right beside the rate ("annual", "a day",
// «годовых»), where it could otherwise be the year of a statement or a report.
bool names_period_alone(const BasisPhrase &phrase) {
	return phrase.words.size() >= 2 && phrase.words.front() != "a" && phrase.words.front() != "each";
}

} // namespace

RateBasis rate_basis_of_clause(std::string_view text, std::size_t start, std::size_t end) {
	if (start > end || end > text.size()) return RateBasis::None;
	constexpr std::size_t nearWords = 3;
	// Forward: to the clause's end, or to the next rate written in figures.
	std::size_t clauseEnd = end;
	while (clauseEnd < text.size() && !clause_break(text, clauseEnd)) ++clauseEnd;
	const std::vector<ClauseWord> after = clause_words(text, end, clauseEnd);
	for (std::size_t index = 0; index < after.size(); ++index) {
		if (after[index].rate) break;
		const auto phrase = basis_phrase_at(after, index);
		if (!phrase) continue;
		if (names_period_alone(*phrase->first) || index < nearWords) return phrase->first->basis;
	}
	// Backward: from the clause's start, the phrase nearest the rate.
	std::size_t clauseStart = start;
	while (clauseStart > 0 && !clause_break(text, clauseStart - 1)) --clauseStart;
	const std::vector<ClauseWord> before = clause_words(text, clauseStart, start);
	std::size_t from = 0;
	for (std::size_t index = 0; index < before.size(); ++index)
		if (before[index].rate) from = index + 1;
	// A period written after an earlier rate of the clause is that rate's -
	// «0,1 % ... за каждый день просрочки, но не более 10 % цены работ»: the
	// cap is 10 % of the price, never 10 % a day (live R1 c26: the cap and the
	// penalty under it were restored "в день"). This rate takes it only when
	// the period stands right before it.
	std::optional<RateBasis> nearest;
	for (std::size_t index = from; index < before.size(); ++index) {
		const auto phrase = basis_phrase_at(before, index);
		if (!phrase) continue;
		const std::size_t wordsAfter = before.size() - (index + phrase->second);
		if ((from == 0 && names_period_alone(*phrase->first)) || wordsAfter < nearWords) nearest = phrase->first->basis;
	}
	return nearest.value_or(RateBasis::None);
}

// ---------------------------------------------------------------------------
// Relations

namespace {

std::string say(Language language, const std::string &english, const std::string &russian) {
	return language == Language::Russian ? russian : english;
}

std::string precision_noun(temporal::Precision precision, Language language) {
	switch (precision) {
		case temporal::Precision::Day: return say(language, "a day", "день");
		case temporal::Precision::Month: return say(language, "a month", "месяц");
		case temporal::Precision::Quarter: return say(language, "a quarter", "квартал");
		case temporal::Precision::Year: return say(language, "a year", "год");
	}
	return "";
}

std::string named(const Value &value, std::string_view written, Language language) {
	if (!written.empty()) return std::string(written);
	try {
		return display(value, {}, language);
	} catch (const AlgebraError &) {
		return value_key(value);
	}
}

// Why two dates leave `question` open: "⟦July 2026⟧ is a month that contains
// ⟦July 25, 2026⟧, so which comes first is not known from the dates".
std::string dates_reason(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, bool ordering,
	Language language) {
	const std::string question = ordering ? say(language, "which comes first", "что было раньше")
		: say(language, "whether they are the same day", "один ли это день");
	const std::string tail = say(language, ", so " + question + " is not known from the dates",
		", поэтому " + question + ", по датам неизвестно");
	switch (relate_dates(a.date, b.date)) {
		case DateRelation::Contains:
			return say(language, nameA + " is " + precision_noun(a.date.precision, language) + " that contains " + nameB,
				nameA + " - это " + precision_noun(a.date.precision, language) + ", в который входит " + nameB) + tail;
		case DateRelation::Within:
			return say(language, nameB + " is " + precision_noun(b.date.precision, language) + " that contains " + nameA,
				nameB + " - это " + precision_noun(b.date.precision, language) + ", в который входит " + nameA) + tail;
		case DateRelation::SamePeriod:
			return say(language, nameA + " and " + nameB + " are the same " + precision_noun(a.date.precision, language).substr(2)
					+ ", not a day",
				nameA + " и " + nameB + " - один и тот же " + precision_noun(a.date.precision, language) + ", а не день") + tail;
		default:
			return say(language, nameA + " and " + nameB + " overlap", nameA + " и " + nameB + " пересекаются") + tail;
	}
}

// The smallest and largest number of days in `months` consecutive months.
std::pair<Int128, Int128> days_in_months(Int128 months) {
	static const std::array<std::pair<long long, long long>, 12> runs = [] {
		std::array<std::pair<long long, long long>, 12> result{};
		static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
		for (int count = 0; count < 12; ++count) {
			long long least = 1000, most = 0;
			for (int start = 0; start < 12; ++start) {
				long long total = 0;
				bool february = false;
				for (int index = 0; index < count; ++index) {
					const int month = (start + index) % 12;
					total += lengths[month];
					february = february || month == 1;
				}
				least = std::min(least, total);
				most = std::max(most, total + (february ? 1 : 0));
			}
			result[static_cast<std::size_t>(count)] = {least, most};
		}
		return result;
	}();
	const Int128 years = months / 12;
	const auto &run = runs[static_cast<std::size_t>(months % 12)];
	return {add128(mul128(365, years), run.first), add128(mul128(366, years), run.second)};
}

struct Span {
	Rational low;
	Rational high;
};

// Minutes, hours, days and weeks convert exactly (x 60, x 24, x 7); so do
// months, quarters and years (x 3, x 12). Across the two, only a bound.
bool day_class(DurationUnit unit) {
	return unit == DurationUnit::Day || unit == DurationUnit::Week || unit == DurationUnit::Hour || unit == DurationUnit::Minute;
}
bool month_class(DurationUnit unit) {
	return unit == DurationUnit::Month || unit == DurationUnit::Year || unit == DurationUnit::Quarter;
}

Rational in_days(const Value &duration) {
	switch (duration.unit) {
		case DurationUnit::Week: return duration.amount * Rational(7);
		case DurationUnit::Hour: return duration.amount / Rational(24);
		case DurationUnit::Minute: return duration.amount / Rational(1440);
		default: return duration.amount;
	}
}

Rational in_months(const Value &duration) {
	if (duration.unit == DurationUnit::Quarter) return duration.amount * Rational(3);
	return duration.unit == DurationUnit::Year ? duration.amount * Rational(12) : duration.amount;
}

Span month_span_in_days(const Rational &months) {
	if (months.sign() < 0) {
		const Span positive = month_span_in_days(-months);
		return {-positive.high, -positive.low};
	}
	const Rational lowCount = months.floored();
	const Rational highCount = months.ceiled();
	const auto low = days_in_months(lowCount.numerator());
	const auto high = days_in_months(highCount.numerator());
	return {Rational(low.first), Rational(high.second)};
}

// Why a count of months or years is not a count of days: whole years say
// so of the year, anything else of the month.
std::string why_not_days(const Value &months, Language language) {
	const Rational count = in_months(months);
	if (!count.is_zero() && (count / Rational(12)).is_integer()) return say(language, "a year has 365 or 366 days", "в году 365 или 366 дней");
	return say(language, "a month has 28 to 31 days", "в месяце от 28 до 31 дня");
}

std::string unit_words(DurationUnit unit, Language language) {
	switch (unit) {
		case DurationUnit::BusinessDay: return say(language, "business days", "рабочие дни");
		case DurationUnit::Day: return say(language, "days", "дни");
		case DurationUnit::Week: return say(language, "weeks", "недели");
		case DurationUnit::Month: return say(language, "months", "месяцы");
		case DurationUnit::Year: return say(language, "years", "годы");
		case DurationUnit::Quarter: return say(language, "quarters", "кварталы");
		case DurationUnit::Hour: return say(language, "hours", "часы");
		case DurationUnit::Minute: return say(language, "minutes", "минуты");
	}
	return "";
}

std::string kind_words(Kind kind, Language language) {
	switch (kind) {
		case Kind::Money: return say(language, "an amount of money", "денежная сумма");
		case Kind::Number: return say(language, "a number", "число");
		case Kind::Count: return say(language, "a count", "количество");
		case Kind::Percentage: return say(language, "a percentage", "процент");
		case Kind::Duration: return say(language, "a period", "срок");
		case Kind::Date: return say(language, "a date", "дата");
		case Kind::Boolean: return say(language, "true or false", "да или нет");
		case Kind::DayOfYear: return say(language, "a day of the year with no year", "день года без указания года");
	}
	return "";
}

AlgebraError different_currencies(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language) {
	return AlgebraError("currency", say(language,
		nameA + " and " + nameB + " are amounts in different currencies (" + a.currency + " and " + b.currency
			+ "), and Singularity does not convert between them",
		nameA + " и " + nameB + " - суммы в разных валютах (" + a.currency + " и " + b.currency
			+ "), а Singularity не пересчитывает валюты"));
}

// An amount whose currency is not known is not an amount in any currency: it
// meets only another amount whose currency is not known either.
AlgebraError unknown_currency(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language) {
	const bool firstUnknown = a.currency.empty();
	const std::string &unknown = firstUnknown ? nameA : nameB;
	const std::string &known = firstUnknown ? nameB : nameA;
	const std::string &code = firstUnknown ? b.currency : a.currency;
	return AlgebraError("currency", say(language,
		unknown + " is an amount whose currency is not known and " + known + " is in " + code
			+ ", so Singularity cannot take them as one currency",
		"валюта суммы " + unknown + " неизвестна, а " + known + " указана в " + code
			+ ", поэтому Singularity не может считать их суммами в одной валюте"));
}

// Two amounts in one currency, or why they are not.
void require_one_currency(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language) {
	if (a.currency == b.currency) return;
	if (a.currency.empty() || b.currency.empty()) throw unknown_currency(a, b, nameA, nameB, language);
	throw different_currencies(a, b, nameA, nameB, language);
}

// A plain number is not an amount of money: it has no currency (a bare
// workbook figure may be rubles). Only zero is zero in every currency.
AlgebraError number_is_not_money(const std::string &number, const std::string &whole, Language language) {
	return AlgebraError("type", say(language,
		whole + " mixes an amount of money with " + number + ", a number with no currency; write that amount with its currency",
		whole + " смешивает денежную сумму и " + number + " - число без валюты; запишите эту сумму с ее валютой"));
}

AlgebraError do_not_compare(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language) {
	return AlgebraError("type", say(language,
		nameA + " is " + kind_words(a.kind, language) + " and " + nameB + " is " + kind_words(b.kind, language)
			+ ", so they do not compare",
		nameA + " - " + kind_words(a.kind, language) + ", а " + nameB + " - " + kind_words(b.kind, language)
			+ ", их нельзя сравнить"));
}

std::string basis_name(RateBasis basis, Language language) {
	switch (basis) {
		case RateBasis::None: return "";
		case RateBasis::PerDay: return say(language, "per day", "в день");
		case RateBasis::PerMonth: return say(language, "per month", "в месяц");
		case RateBasis::PerYear: return say(language, "per annum", "в год");
	}
	return "";
}

// Rates (or amounts) for different periods, as their yearly span: a year is
// 12 months and 365 or 366 days, so 0.1 % per day is 36.5 % to 36.6 % a year.
Span per_year(const Rational &amount, RateBasis basis) {
	switch (basis) {
		case RateBasis::PerDay: {
			const Rational low = amount * Rational(365), high = amount * Rational(366);
			return low < high ? Span{low, high} : Span{high, low};
		}
		case RateBasis::PerMonth: return {amount * Rational(12), amount * Rational(12)};
		default: return {amount, amount};
	}
}

bool different_bases(const Value &a, const Value &b) {
	return a.basis != RateBasis::None && b.basis != RateBasis::None && a.basis != b.basis;
}

// The period a rate (or an amount owed per period) is for once it is scaled
// by a plain number: its own - twice 0.1 % per day is 0.2 % per day - unless
// the number is the count of its periods in a longer one, or of the shorter
// periods in its own: 0.1 % per day * 365 is per annum, 18 % per annum / 365
// (360 or 366, as the contract counts a year) is per day, / 12 per month, and
// a monthly one / 30 (28 to 31) per day.
RateBasis rescaled_basis(RateBasis basis, const Rational &number, bool divided) {
	const bool whole = number.is_integer();
	const bool yearOfDays = whole && number >= Rational(360) && number <= Rational(366);
	const bool monthOfDays = whole && number >= Rational(28) && number <= Rational(31);
	const bool yearOfMonths = number == Rational(12);
	if (divided) {
		if (basis == RateBasis::PerYear && yearOfDays) return RateBasis::PerDay;
		if (basis == RateBasis::PerYear && yearOfMonths) return RateBasis::PerMonth;
		if (basis == RateBasis::PerMonth && monthOfDays) return RateBasis::PerDay;
		return basis;
	}
	if (basis == RateBasis::PerDay && yearOfDays) return RateBasis::PerYear;
	if (basis == RateBasis::PerMonth && yearOfMonths) return RateBasis::PerYear;
	if (basis == RateBasis::PerDay && monthOfDays) return RateBasis::PerMonth;
	return basis;
}

// $1 per day is not $1: an amount owed for each day, month or year meets a
// lump sum only once it is multiplied by its period.
AlgebraError per_period_and_lump_sum(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language) {
	const bool firstPer = a.basis != RateBasis::None;
	const std::string &per = firstPer ? nameA : nameB;
	const std::string &lump = firstPer ? nameB : nameA;
	const RateBasis basis = firstPer ? a.basis : b.basis;
	const std::string example = basis == RateBasis::PerDay ? say(language, "* 45 days", "* 45 дней")
		: basis == RateBasis::PerMonth ? say(language, "* 6 months", "* 6 месяцев") : say(language, "* 2 years", "* 2 года");
	return AlgebraError("type", say(language,
		per + " is an amount " + basis_name(basis, language) + " and " + lump
			+ " is a lump sum, so they do not add up or compare: multiply the amount " + basis_name(basis, language)
			+ " by its period first (for example " + example + ")",
		per + " - сумма " + basis_name(basis, language) + ", а " + lump
			+ " - единовременная сумма, их нельзя складывать и сравнивать: сначала умножьте сумму " + basis_name(basis, language)
			+ " на ее срок (например, " + example + ")"));
}

std::pair<Span, Span> yearly_spans(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language,
	std::string &undecided) {
	undecided = say(language,
		nameA + " is " + basis_name(a.basis, language) + " and " + nameB + " is " + basis_name(b.basis, language)
			+ ", and a year has 365 or 366 days, so they cannot be compared exactly",
		nameA + " - " + basis_name(a.basis, language) + ", а " + nameB + " - " + basis_name(b.basis, language)
			+ ", а в году 365 или 366 дней, поэтому точно их сравнить нельзя");
	return {per_year(a.amount, a.basis), per_year(b.amount, b.basis)};
}

// Both values as intervals of one measure, or why they cannot be.
std::pair<Span, Span> spans(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language,
	std::string &undecided) {
	const auto point = [](const Rational &value) { return Span{value, value}; };
	if (a.kind == Kind::Date || b.kind == Kind::Date) {
		if (a.kind != Kind::Date || b.kind != Kind::Date) throw do_not_compare(a, b, nameA, nameB, language);
		return {Span{Rational(a.date.first_day), Rational(a.date.last_day)}, Span{Rational(b.date.first_day), Rational(b.date.last_day)}};
	}
	if (a.kind == Kind::Boolean || b.kind == Kind::Boolean) {
		if (a.kind != b.kind) throw do_not_compare(a, b, nameA, nameB, language);
		return {point(Rational(a.truth ? 1 : 0)), point(Rational(b.truth ? 1 : 0))};
	}
	if (a.kind == Kind::Duration && b.kind == Kind::Duration) {
		const bool businessA = a.unit == DurationUnit::BusinessDay, businessB = b.unit == DurationUnit::BusinessDay;
		if (businessA || businessB) {
			if (businessA && businessB) return {point(a.amount), point(b.amount)};
			throw AlgebraError("undetermined", say(language,
				nameA + " and " + nameB + " cannot be compared: business days depend on a holiday calendar Singularity does not have",
				nameA + " и " + nameB + " нельзя сравнить: рабочие дни зависят от календаря праздников, которого у Singularity нет"));
		}
		if (day_class(a.unit) && day_class(b.unit)) return {point(in_days(a)), point(in_days(b))};
		if (month_class(a.unit) && month_class(b.unit)) return {point(in_months(a)), point(in_months(b))};
		const std::string why = why_not_days(month_class(a.unit) ? a : b, language);
		undecided = say(language, nameA + " and " + nameB + " cannot be compared exactly: " + why,
			nameA + " и " + nameB + " нельзя сравнить точно: " + why);
		const Span first = day_class(a.unit) ? point(in_days(a)) : month_span_in_days(in_months(a));
		const Span second = day_class(b.unit) ? point(in_days(b)) : month_span_in_days(in_months(b));
		return {first, second};
	}
	if (a.kind == Kind::Duration || b.kind == Kind::Duration) {
		// A plain number against a period counts its units.
		const Value &other = a.kind == Kind::Duration ? b : a;
		if (other.kind != Kind::Number) throw do_not_compare(a, b, nameA, nameB, language);
		return {point(a.amount), point(b.amount)};
	}
	if (a.kind == Kind::Money && b.kind == Kind::Money) {
		require_one_currency(a, b, nameA, nameB, language);
		if (different_bases(a, b)) return yearly_spans(a, b, nameA, nameB, language, undecided);
		if (a.basis != b.basis) throw per_period_and_lump_sum(a, b, nameA, nameB, language);
		return {point(a.amount), point(b.amount)};
	}
	if (a.kind == Kind::Money || b.kind == Kind::Money) {
		// Only zero, which is zero in every currency, compares with an amount.
		const bool numberFirst = a.kind != Kind::Money;
		const Value &other = numberFirst ? a : b;
		if (other.kind != Kind::Number) throw do_not_compare(a, b, nameA, nameB, language);
		if (!other.amount.is_zero())
			throw number_is_not_money(numberFirst ? nameA : nameB, say(language, "comparing " + nameA + " with " + nameB, "сравнение " + nameA + " и " + nameB), language);
		return {point(a.amount), point(b.amount)};
	}
	// Numbers and percentages: a percentage is its fraction, and rates for
	// different periods are compared as rates for a year.
	if (different_bases(a, b)) return yearly_spans(a, b, nameA, nameB, language, undecided);
	return {point(a.amount), point(b.amount)};
}

Truth truth_of(bool decidedTrue, bool decidedFalse) {
	if (decidedTrue) return Truth::True;
	if (decidedFalse) return Truth::False;
	return Truth::Undetermined;
}

Truth negated(Truth truth) {
	return truth == Truth::True ? Truth::False : truth == Truth::False ? Truth::True : Truth::Undetermined;
}

} // namespace

DateRelation relate_dates(const temporal::TemporalValue &a, const temporal::TemporalValue &b) {
	if (a.last_day < b.first_day) return DateRelation::Before;
	if (a.first_day > b.last_day) return DateRelation::After;
	if (a.first_day == b.first_day && a.last_day == b.last_day)
		return a.first_day == a.last_day ? DateRelation::SameDay : DateRelation::SamePeriod;
	if (a.first_day <= b.first_day && b.last_day <= a.last_day) return DateRelation::Contains;
	if (b.first_day <= a.first_day && a.last_day <= b.last_day) return DateRelation::Within;
	return DateRelation::Overlaps;
}

Relation compare(const Value &a, Comparator comparator, const Value &b, Language language, std::string_view written_a,
	std::string_view written_b) {
	const std::string nameA = named(a, written_a, language);
	const std::string nameB = named(b, written_b, language);
	// A day of the year is equal or not to another, and nothing else: which of
	// two comes first, or where one stands against a date, depends on a year.
	if (a.kind == Kind::DayOfYear || b.kind == Kind::DayOfYear) {
		const bool equality = comparator == Comparator::Equal || comparator == Comparator::NotEqual;
		if (a.kind != b.kind || !equality) {
			const std::string &day = a.kind == Kind::DayOfYear ? nameA : nameB;
			throw AlgebraError("type", say(language,
				day + " is a day of the year with no year, so it is before or after nothing: ask same_day_of_year() whether a "
					"date falls on it, or next_occurrence() for the date it next comes on",
				day + " - день года без указания года, он не раньше и не позже чего-либо: спросите same_day_of_year(), "
					"приходится ли дата на него, или next_occurrence() - когда он наступит"));
		}
		const bool same = a.day_of_year.month == b.day_of_year.month && a.day_of_year.day == b.day_of_year.day;
		return {(same == (comparator == Comparator::Equal)) ? Truth::True : Truth::False, ""};
	}
	std::string undecided;
	const auto [x, y] = spans(a, b, nameA, nameB, language, undecided);
	if (a.kind == Kind::Boolean && comparator != Comparator::Equal && comparator != Comparator::NotEqual)
		throw AlgebraError("type", say(language, "true and false have no order", "у «да» и «нет» нет порядка"));
	Truth truth = Truth::Undetermined;
	switch (comparator) {
		case Comparator::Less: truth = truth_of(x.high < y.low, x.low >= y.high); break;
		case Comparator::LessOrEqual: truth = truth_of(x.high <= y.low, x.low > y.high); break;
		case Comparator::Greater: truth = truth_of(x.low > y.high, x.high <= y.low); break;
		case Comparator::GreaterOrEqual: truth = truth_of(x.low >= y.high, x.high < y.low); break;
		case Comparator::Equal:
		case Comparator::NotEqual: {
			const bool single = x.low == x.high && y.low == y.high && x.low == y.low;
			const bool apart = x.high < y.low || y.high < x.low;
			truth = truth_of(single, apart);
			if (comparator == Comparator::NotEqual) truth = negated(truth);
			break;
		}
	}
	Relation relation{truth, ""};
	if (truth == Truth::Undetermined) {
		const bool ordering = comparator != Comparator::Equal && comparator != Comparator::NotEqual;
		relation.reason = a.kind == Kind::Date ? dates_reason(a, b, nameA, nameB, ordering, language) : undecided;
	}
	return relation;
}

namespace {

void require_dates(const Value &a, const Value &b, const std::string &nameA, const std::string &nameB, Language language) {
	for (const auto &[value, name] : {std::make_pair(&a, &nameA), std::make_pair(&b, &nameB)})
		if (value->kind != Kind::Date)
			throw AlgebraError("type", say(language, *name + " is not a date", *name + " - не дата"));
}

} // namespace

Relation before(const Value &a, const Value &b, Language language, std::string_view written_a, std::string_view written_b) {
	const std::string nameA = named(a, written_a, language), nameB = named(b, written_b, language);
	require_dates(a, b, nameA, nameB, language);
	return compare(a, Comparator::Less, b, language, nameA, nameB);
}

Relation after(const Value &a, const Value &b, Language language, std::string_view written_a, std::string_view written_b) {
	const std::string nameA = named(a, written_a, language), nameB = named(b, written_b, language);
	require_dates(a, b, nameA, nameB, language);
	return compare(a, Comparator::Greater, b, language, nameA, nameB);
}

Relation same_day(const Value &a, const Value &b, Language language, std::string_view written_a, std::string_view written_b) {
	const std::string nameA = named(a, written_a, language), nameB = named(b, written_b, language);
	require_dates(a, b, nameA, nameB, language);
	return compare(a, Comparator::Equal, b, language, nameA, nameB);
}

Relation within(const Value &date, const Value &period, Language language, std::string_view written_date, std::string_view written_period) {
	const std::string nameA = named(date, written_date, language), nameB = named(period, written_period, language);
	require_dates(date, period, nameA, nameB, language);
	const auto &d = date.date;
	const auto &p = period.date;
	Relation relation{truth_of(p.first_day <= d.first_day && d.last_day <= p.last_day, d.last_day < p.first_day || d.first_day > p.last_day), ""};
	if (relation.truth == Truth::Undetermined)
		relation.reason = say(language,
			nameA + " is " + precision_noun(d.precision, language) + " only partly inside " + nameB
				+ ", so whether it falls within it is not known from the dates",
			nameA + " - " + precision_noun(d.precision, language) + ", лишь частью входящий в " + nameB
				+ ", поэтому, попадает ли он в этот период, по датам неизвестно");
	return relation;
}

Relation within(const Value &date, const Value &from, const Value &to, Language language, std::string_view written_date) {
	const std::string nameFrom = named(from, {}, language), nameTo = named(to, {}, language);
	require_dates(from, to, nameFrom, nameTo, language);
	if (from.date.first_day > to.date.last_day)
		throw AlgebraError("type", say(language, "the period from " + nameFrom + " to " + nameTo + " ends before it starts",
			"период с " + nameFrom + " по " + nameTo + " заканчивается раньше, чем начинается"));
	Value period = make_date({from.date.first_day, to.date.last_day, temporal::Precision::Day});
	return within(date, period, language, written_date,
		say(language, "the period from " + nameFrom + " to " + nameTo, "период с " + nameFrom + " по " + nameTo));
}

Relation overlaps(const Value &a, const Value &b) {
	if (a.kind != Kind::Date || b.kind != Kind::Date) throw AlgebraError("type", "only dates overlap");
	const bool meet = a.date.first_day <= b.date.last_day && b.date.first_day <= a.date.last_day;
	return {meet ? Truth::True : Truth::False, ""};
}

namespace {

// The day of the year a value is: its own, or a single day's; nothing for a
// date that is a longer period.
std::optional<temporal::DayOfYear> day_of_year_of(const Value &value) {
	if (value.kind == Kind::DayOfYear) return value.day_of_year;
	if (value.kind == Kind::Date && value.date.first_day == value.date.last_day) {
		const Civil day = civil_from_days(value.date.first_day);
		return temporal::DayOfYear{day.month, day.day};
	}
	return std::nullopt;
}

bool falls_on(long long days, const temporal::DayOfYear &day) {
	const Civil civil = civil_from_days(days);
	return civil.month == day.month && civil.day == day.day;
}

void require_day_or_date(const Value &value, const std::string &name, Language language) {
	if (value.kind != Kind::DayOfYear && value.kind != Kind::Date)
		throw AlgebraError("type", say(language, name + " is neither a date nor a day of the year",
			name + " - не дата и не день года"));
}

// The first day from `from` on that is `day`: at most eight years on, for a
// February 29.
long long occurrence_from(long long from, const temporal::DayOfYear &day) {
	const Civil start = civil_from_days(from);
	for (long long year = start.year; year <= start.year + 8; ++year) {
		if (day.day > month_length(year, day.month)) continue;
		const long long at = days_from_civil(year, day.month, day.day);
		if (at >= from) return at;
	}
	throw AlgebraError("undetermined", "no such day of the year");
}

} // namespace

Relation same_day_of_year(const Value &a, const Value &b, Language language, std::string_view written_a, std::string_view written_b) {
	const std::string nameA = named(a, written_a, language), nameB = named(b, written_b, language);
	require_day_or_date(a, nameA, language);
	require_day_or_date(b, nameB, language);
	const std::optional<temporal::DayOfYear> dayA = day_of_year_of(a), dayB = day_of_year_of(b);
	if (dayA && dayB) return {dayA->month == dayB->month && dayA->day == dayB->day ? Truth::True : Truth::False, ""};
	if (!dayA && !dayB)
		return {Truth::Undetermined, say(language, nameA + " and " + nameB + " are periods, not days, so whether they fall on the same "
			"day of the year is not known from the dates", nameA + " и " + nameB + " - периоды, а не дни, поэтому, приходятся ли они "
			"на один день года, по датам неизвестно")};
	// A day of the year against a period: false when no day of the period is it.
	const temporal::DayOfYear day = dayA ? *dayA : *dayB;
	const Value &period = dayA ? b : a;
	const std::string &periodName = dayA ? nameB : nameA;
	const std::string &dayName = dayA ? nameA : nameB;
	const long long last = std::min(period.date.last_day, period.date.first_day + 366);
	bool any = false;
	for (long long at = period.date.first_day; at <= last && !any; ++at) any = falls_on(at, day);
	if (!any) return {Truth::False, ""};
	return {Truth::Undetermined, say(language,
		periodName + " is " + precision_noun(period.date.precision, language) + " that holds " + dayName
			+ ", so whether it is that day is not known from the dates",
		periodName + " - " + precision_noun(period.date.precision, language) + ", в который входит " + dayName
			+ ", поэтому, тот ли это день, по датам неизвестно")};
}

Value next_occurrence(const Value &day_of_year, const Value &on_or_after, Language language, std::string_view written_day,
	std::string_view written_date) {
	const std::string nameDay = named(day_of_year, written_day, language), nameDate = named(on_or_after, written_date, language);
	const std::optional<temporal::DayOfYear> day = day_of_year_of(day_of_year);
	if (!day)
		throw AlgebraError("type", say(language, nameDay + " is not a day of the year", nameDay + " - не день года"));
	if (on_or_after.kind != Kind::Date)
		throw AlgebraError("type", say(language, nameDate + " is not a date", nameDate + " - не дата"));
	const long long first = occurrence_from(on_or_after.date.first_day, *day);
	const long long last = occurrence_from(on_or_after.date.last_day, *day);
	if (first != last)
		throw AlgebraError("undetermined", say(language,
			nameDate + " is " + precision_noun(on_or_after.date.precision, language) + ", and the next " + nameDay
				+ " on or after it depends on which day of it is meant",
			nameDate + " - " + precision_noun(on_or_after.date.precision, language) + ", и когда после него наступит " + nameDay
				+ ", зависит от того, какой это день"));
	// Written as the date it is counted from is written: "08.09.2026" gives "01.07.2027".
	return make_date(day_value(first), on_or_after.date_style.empty() ? "english" : on_or_after.date_style);
}

// ---------------------------------------------------------------------------
// Display

namespace {

const std::string kNbsp = "\xC2\xA0";

std::string grouped(const std::string &digits, const std::string &separator) {
	std::string result;
	const std::size_t size = digits.size();
	for (std::size_t index = 0; index < size; ++index) {
		if (index > 0 && (size - index) % 3 == 0) result += separator;
		result.push_back(digits[index]);
	}
	return result;
}

// A decimal as the language writes it: "1,183,452.05" / "1 183 452,05".
std::string written_decimal(const std::string &decimal, Language language) {
	std::string text = decimal;
	const bool negative = !text.empty() && text.front() == '-';
	if (negative) text.erase(0, 1);
	const std::size_t point = text.find('.');
	const std::string whole = point == std::string::npos ? text : text.substr(0, point);
	std::string result = grouped(whole, language == Language::Russian ? kNbsp : ",");
	if (point != std::string::npos) result += (language == Language::Russian ? "," : ".") + text.substr(point + 1);
	return negative ? "-" + result : result;
}

// Exact when it has at most `exact` decimals, else rounded to `rounded`
// with the zeros that say nothing dropped. A small figure that would round to
// 0 keeps two significant digits instead: $5 of $1,000,000 is 0.000005, not 0.
std::string plain_decimal(const Rational &number, int exact, int rounded) {
	if (auto text = number.exact_decimal(exact)) return *text;
	int places = rounded;
	if (!number.is_zero() && number.rounded(places).is_zero()) {
		while (places < 30 && number.rounded(places).is_zero()) ++places;
		if (places < 30) ++places;
	}
	std::string text = number.to_decimal(places);
	if (text.find('.') != std::string::npos) {
		while (text.back() == '0') text.pop_back();
		if (text.back() == '.') text.pop_back();
	}
	return text;
}

std::string number_text(const Rational &number, Language language, int minimumDecimals = 0) {
	std::string text = plain_decimal(number, 4, 2);
	const std::size_t point = text.find('.');
	const int decimals = point == std::string::npos ? 0 : static_cast<int>(text.size() - point - 1);
	if (decimals < minimumDecimals) text = number.to_decimal(minimumDecimals);
	return written_decimal(text, language);
}

const char *english_units[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven",
	"twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char *english_tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

std::string english_hundreds(int number) {
	std::string result;
	if (number >= 100) {
		result = std::string(english_units[number / 100]) + " hundred";
		number %= 100;
		if (number == 0) return result;
		result += " ";
	}
	if (number < 20) return result + english_units[number];
	result += english_tens[number / 10];
	if (number % 10 != 0) result += std::string("-") + english_units[number % 10];
	return result;
}

const char *russian_units_masculine[] = {"", "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять", "десять",
	"одиннадцать", "двенадцать", "тринадцать", "четырнадцать", "пятнадцать", "шестнадцать", "семнадцать", "восемнадцать", "девятнадцать"};
const char *russian_tens[] = {"", "", "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят", "семьдесят", "восемьдесят", "девяносто"};
const char *russian_hundreds_words[] = {"", "сто", "двести", "триста", "четыреста", "пятьсот", "шестьсот", "семьсот", "восемьсот", "девятьсот"};

std::string russian_hundreds(int number, bool feminine) {
	std::vector<std::string> parts;
	if (number >= 100) parts.push_back(russian_hundreds_words[number / 100]);
	number %= 100;
	if (number >= 20) {
		parts.push_back(russian_tens[number / 10]);
		number %= 10;
	}
	if (number > 0) {
		if (feminine && number == 1) parts.push_back("одна");
		else if (feminine && number == 2) parts.push_back("две");
		else parts.push_back(russian_units_masculine[number]);
	}
	std::string result;
	for (const std::string &part : parts) result += (result.empty() ? "" : " ") + part;
	return result;
}

std::string english_money_noun(const std::string &currency, bool one) {
	if (currency == "USD") return one ? "dollar" : "dollars";
	if (currency == "EUR") return one ? "euro" : "euros";
	if (currency == "GBP") return one ? "pound" : "pounds";
	if (currency == "JPY") return "yen";
	if (currency == "RUB") return one ? "ruble" : "rubles";
	return currency;
}

bool counted_in_cents(const std::string &currency) {
	for (const char *code : {"USD", "EUR", "CAD", "AUD", "HKD", "SGD"})
		if (currency == code) return true;
	return false;
}

// The sub-unit a currency is written with, or nothing when Singularity does not
// know it (then the minor part is written "50/100").
std::optional<std::string> english_minor_noun(const std::string &currency, bool one) {
	if (counted_in_cents(currency)) return std::string(one ? "cent" : "cents");
	if (currency == "GBP") return std::string(one ? "penny" : "pence");
	if (currency == "RUB") return std::string(one ? "kopeck" : "kopecks");
	if (currency == "CHF") return std::string(one ? "centime" : "centimes");
	if (currency == "KZT") return std::string("tiyn");
	return std::nullopt;
}

std::string russian_money_noun(const std::string &currency, Int128 count, bool fraction) {
	if (currency == "USD") return fraction ? "доллара США" : russian_plural(count, "доллар США", "доллара США", "долларов США");
	if (currency == "EUR") return "евро";
	if (currency == "GBP") return fraction ? "фунта стерлингов" : russian_plural(count, "фунт стерлингов", "фунта стерлингов", "фунтов стерлингов");
	if (currency == "JPY") return fraction ? "иены" : russian_plural(count, "иена", "иены", "иен");
	if (currency == "RUB") return fraction ? "рубля" : russian_plural(count, "рубль", "рубля", "рублей");
	return currency;
}

std::optional<std::string> russian_minor_noun(const std::string &currency, Int128 count) {
	if (currency == "RUB") return russian_plural(count, "копейка", "копейки", "копеек");
	if (currency == "GBP") return russian_plural(count, "пенс", "пенса", "пенсов");
	if (counted_in_cents(currency)) return russian_plural(count, "цент", "цента", "центов");
	if (currency == "CHF") return russian_plural(count, "сантим", "сантима", "сантимов");
	if (currency == "KZT") return std::string("тиын");
	return std::nullopt;
}

bool has_minor_unit(const std::string &currency) { return currency != "JPY"; }

std::string symbol_of(const std::string &currency) {
	if (currency == "USD") return "$";
	if (currency == "EUR") return "\xE2\x82\xAC";
	if (currency == "GBP") return "\xC2\xA3";
	if (currency == "JPY") return "\xC2\xA5";
	if (currency == "RUB") return "\xE2\x82\xBD";
	return currency;
}

bool mark_after(const std::string &mark, Language language) {
	if (mark == "\xE2\x82\xBD" || mark == "руб.") return true;
	if (mark == "$" || mark == "US$" || mark == "\xE2\x82\xAC" || mark == "\xC2\xA3" || mark == "\xC2\xA5") return false;
	return language == Language::Russian;  // a code: "USD 1,000", "1 000 USD"
}

bool is_word_mark(const std::string &mark) { return starts_with(mark, "рубл"); }

std::string ru_month_genitive(unsigned month) {
	static const char *names[] = {"января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября",
		"ноября", "декабря"};
	return names[month - 1];
}

std::string ru_month_nominative(unsigned month) {
	static const char *names[] = {"январь", "февраль", "март", "апрель", "май", "июнь", "июль", "август", "сентябрь", "октябрь",
		"ноябрь", "декабрь"};
	return names[month - 1];
}

std::string en_month(unsigned month) {
	static const char *names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
		"November", "December"};
	return names[month - 1];
}

std::string date_text(const Value &value, Language language) {
	const Civil first = civil_from_days(value.date.first_day);
	const std::string year = std::to_string(first.year);
	const bool dotted = value.date_style == "dotted";
	const bool slashed = value.date_style == "slashed" && language == Language::English;
	switch (value.date.precision) {
		case temporal::Precision::Day:
			if (dotted) return two_digits(first.day) + "." + two_digits(first.month) + "." + year;
			if (slashed) return two_digits(first.month) + "/" + two_digits(first.day) + "/" + year;
			if (language == Language::Russian) return std::to_string(first.day) + " " + ru_month_genitive(first.month) + " " + year + " г.";
			return en_month(first.month) + " " + std::to_string(first.day) + ", " + year;
		case temporal::Precision::Month:
			if (dotted) return two_digits(first.month) + "." + year;
			if (language == Language::Russian) return ru_month_nominative(first.month) + " " + year + " г.";
			return en_month(first.month) + " " + year;
		case temporal::Precision::Quarter: {
			static const char *roman[] = {"I", "II", "III", "IV"};
			const unsigned quarter = (first.month - 1) / 3;
			if (language == Language::Russian) return std::string(roman[quarter]) + " квартал " + year + " г.";
			return "Q" + std::to_string(quarter + 1) + " " + year;
		}
		case temporal::Precision::Year:
			return language == Language::Russian ? year + " г." : year;
	}
	return year;
}

// "July 1", «1 июля»: the day and the month, and no year.
std::string day_of_year_text(const Value &value, Language language) {
	const temporal::DayOfYear &day = value.day_of_year;
	if (day.month < 1 || day.month > 12) return value_key(value);
	if (language == Language::Russian) return std::to_string(day.day) + " " + ru_month_genitive(day.month);
	return en_month(day.month) + " " + std::to_string(day.day);
}

std::string duration_noun(DurationUnit unit, const Rational &count, Language language) {
	const bool whole = count.is_integer();
	const Int128 number = whole ? abs128(count.numerator()) : 0;
	if (language == Language::Russian) {
		switch (unit) {
			case DurationUnit::Day: return whole ? russian_plural(number, "день", "дня", "дней") : "дня";
			case DurationUnit::BusinessDay:
				return whole ? russian_plural(number, "рабочий день", "рабочих дня", "рабочих дней") : "рабочего дня";
			case DurationUnit::Week: return whole ? russian_plural(number, "неделя", "недели", "недель") : "недели";
			case DurationUnit::Month: return whole ? russian_plural(number, "месяц", "месяца", "месяцев") : "месяца";
			case DurationUnit::Year: return whole ? russian_plural(number, "год", "года", "лет") : "года";
			case DurationUnit::Quarter: return whole ? russian_plural(number, "квартал", "квартала", "кварталов") : "квартала";
			case DurationUnit::Hour: return whole ? russian_plural(number, "час", "часа", "часов") : "часа";
			case DurationUnit::Minute: return whole ? russian_plural(number, "минута", "минуты", "минут") : "минуты";
		}
	}
	const bool one = whole && number == 1;
	switch (unit) {
		case DurationUnit::Day: return one ? "day" : "days";
		case DurationUnit::BusinessDay: return one ? "business day" : "business days";
		case DurationUnit::Week: return one ? "week" : "weeks";
		case DurationUnit::Month: return one ? "month" : "months";
		case DurationUnit::Year: return one ? "year" : "years";
		case DurationUnit::Quarter: return one ? "quarter" : "quarters";
		case DurationUnit::Hour: return one ? "hour" : "hours";
		case DurationUnit::Minute: return one ? "minute" : "minutes";
	}
	return "";
}

std::string basis_words(RateBasis basis, Language language) {
	switch (basis) {
		case RateBasis::None: return "";
		case RateBasis::PerDay: return say(language, " per day", " в день");
		case RateBasis::PerMonth: return say(language, " per month", " в месяц");
		case RateBasis::PerYear: return say(language, " per annum", " годовых");
	}
	return "";
}

// An amount of money owed for each day, month or year: "$1 per day".
std::string money_basis_words(RateBasis basis, Language language) {
	switch (basis) {
		case RateBasis::None: return "";
		case RateBasis::PerDay: return say(language, " per day", " в день");
		case RateBasis::PerMonth: return say(language, " per month", " в месяц");
		case RateBasis::PerYear: return say(language, " per year", " в год");
	}
	return "";
}

std::string percent_text(const Value &value, Language language) {
	const Rational points = value.amount * Rational(100);
	const std::string number = written_decimal(plain_decimal(points, 4, 2), language);
	return language == Language::Russian ? number + kNbsp + "%" : number + "%";
}

std::string with_words(const std::string &digits, const Rational &number, Language language, bool feminine) {
	if (!number.is_integer()) return digits;
	return digits + " (" + number_in_words(number.numerator(), language, feminine) + ")";
}

// A Duration (or a plain count) in the unit the format asks for.
Value duration_as(const Value &value, DurationUnit unit, Language language) {
	if (value.kind == Kind::Number) return make_duration(value.amount, unit);
	if (value.kind != Kind::Duration)
		throw AlgebraError("format", say(language, kind_words(value.kind, language) + " cannot be written in " + unit_words(unit, language),
			"значение (" + kind_words(value.kind, language) + ") нельзя записать в единицах «" + unit_words(unit, language) + "»"));
	if (value.unit == unit) return value;
	if (value.unit == DurationUnit::Week && unit == DurationUnit::Day) return make_duration(value.amount * Rational(7), unit);
	if (value.unit == DurationUnit::Day && unit == DurationUnit::Week) return make_duration(value.amount / Rational(7), unit);
	if (value.unit == DurationUnit::Year && unit == DurationUnit::Month) return make_duration(value.amount * Rational(12), unit);
	if (value.unit == DurationUnit::Month && unit == DurationUnit::Year) return make_duration(value.amount / Rational(12), unit);
	// Minutes, hours, days, weeks; months, quarters, years: through days or months.
	if (value.unit != DurationUnit::BusinessDay && day_class(value.unit) && day_class(unit))
		return make_duration(in_days(value) / in_days(make_duration(Rational(1), unit)), unit);
	if (month_class(value.unit) && month_class(unit))
		return make_duration(in_months(value) / in_months(make_duration(Rational(1), unit)), unit);
	if (value.unit == DurationUnit::BusinessDay)
		throw AlgebraError("undetermined", say(language,
			display(value, {}, language) + " cannot be written in " + unit_words(unit, language)
				+ ": business days depend on a holiday calendar Singularity does not have",
			display(value, {}, language) + " нельзя выразить в единицах «" + unit_words(unit, language)
				+ "»: рабочие дни зависят от календаря праздников, которого у Singularity нет"));
	const std::string why = why_not_days(month_class(value.unit) ? value : make_duration(Rational(1), unit), language);
	throw AlgebraError("undetermined", say(language,
		display(value, {}, language) + " cannot be written exactly in " + unit_words(unit, language) + ": " + why,
		display(value, {}, language) + " нельзя точно выразить в единицах «" + unit_words(unit, language) + "»: " + why));
}

std::string money_text(const Value &value, const Format &format, Language language) {
	std::string currency = value.currency;
	if (!format.currency.empty()) {
		if (!currency.empty() && currency != format.currency)
			throw AlgebraError("currency", say(language,
				"the amount is in " + currency + ", not " + format.currency + ", and Singularity does not convert between them",
				"сумма указана в " + currency + ", а не в " + format.currency + ", а Singularity не пересчитывает валюты"));
		currency = format.currency;
	}
	// Yen have no sub-unit: the figure and its words are whole yen.
	const bool minorUnit = currency.empty() || has_minor_unit(currency);
	const Rational cents = value.amount.rounded(minorUnit ? 2 : 0);
	const bool negative = cents.sign() < 0;
	const Rational magnitude = cents.abs();
	const Int128 whole = magnitude.floored().numerator();
	const Int128 minor = ((magnitude - Rational(whole)) * Rational(100)).numerator();
	std::string mark = format.mark.empty() ? (currency.empty() ? "" : symbol_of(currency)) : format.mark;
	// The words form in Russian: «828 000 (восемьсот двадцать восемь тысяч) рублей 00 копеек».
	if (format.words && language == Language::Russian) {
		std::string text = written_decimal(int_text(whole), language) + " ("
			+ number_in_words(whole, language, currency == "JPY") + ")";
		if (!currency.empty()) {
			text += " " + russian_money_noun(currency, whole, false);
			if (has_minor_unit(currency)) {
				if (const auto noun = russian_minor_noun(currency, minor)) text += " " + two_digits(static_cast<unsigned>(minor)) + " " + *noun;
				else if (minor != 0) text += " " + two_digits(static_cast<unsigned>(minor)) + "/100";
			}
		} else if (minor != 0) {
			text = written_decimal(magnitude.to_decimal(2), language);
		}
		return negative ? "минус " + text : text;
	}
	const bool cents_shown = format.words || !magnitude.is_integer() || value.source_decimals > 0;
	const int decimals = cents_shown && (currency.empty() || has_minor_unit(currency)) ? 2 : 0;
	std::string digits = written_decimal(magnitude.to_decimal(decimals), language);
	std::string text;
	if (is_word_mark(mark)) {
		// «рублей» asked: the noun agrees with the amount; kopecks as a lawyer writes them.
		const std::string whole_digits = written_decimal(int_text(whole), language);
		text = whole_digits + " " + russian_money_noun("RUB", whole, false);
		if (minor != 0 || value.source_decimals > 0) text += " " + two_digits(static_cast<unsigned>(minor)) + " " + *russian_minor_noun("RUB", minor);
	} else if (mark.empty()) {
		text = digits;
	} else if (mark_after(mark, language)) {
		text = digits + kNbsp + mark;
	} else {
		const bool code = mark.size() == 3 && is_ascii_letter(mark[0]);
		text = code ? mark + kNbsp + digits : mark + digits;
	}
	if (format.words) {
		std::string words = number_in_words(whole, Language::English);
		if (!currency.empty()) words += " " + english_money_noun(currency, whole == 1);
		if (minor != 0) {
			const auto noun = english_minor_noun(currency, minor == 1);
			words += noun ? " and " + number_in_words(minor, Language::English) + " " + *noun : " and " + two_digits(static_cast<unsigned>(minor)) + "/100";
		}
		// The words carry the sign the figure does.
		text += " (" + (negative ? "minus " + words : words) + ")";
	}
	return negative ? "-" + text : text;
}

} // namespace

int minor_unit_places(std::string_view currency) { return has_minor_unit(std::string(currency)) ? 2 : 0; }

std::string russian_plural(Int128 count, std::string_view one, std::string_view few, std::string_view many) {
	const Int128 number = abs128(count);
	const Int128 lastTwo = number % 100;
	const Int128 last = number % 10;
	if (lastTwo >= 11 && lastTwo <= 14) return std::string(many);
	if (last == 1) return std::string(one);
	if (last >= 2 && last <= 4) return std::string(few);
	return std::string(many);
}

std::string number_in_words(Int128 number, Language language, bool feminine) {
	if (number < 0) return (language == Language::Russian ? "минус " : "minus ") + number_in_words(neg128(number), language, feminine);
	if (number == 0) return language == Language::Russian ? "ноль" : "zero";
	struct Scale {
		Int128 size;
		const char *english;
		const char *one;
		const char *few;
		const char *many;
		bool feminine;
	};
	static const Scale scales[] = {
		{Int128(1000000000000LL), "trillion", "триллион", "триллиона", "триллионов", false},
		{Int128(1000000000), "billion", "миллиард", "миллиарда", "миллиардов", false},
		{Int128(1000000), "million", "миллион", "миллиона", "миллионов", false},
		{Int128(1000), "thousand", "тысяча", "тысячи", "тысяч", true},
	};
	if (number >= Int128(1000000000000LL) * 1000) return int_text(number);
	std::vector<std::string> parts;
	for (const Scale &scale : scales) {
		const Int128 count = number / scale.size;
		number %= scale.size;
		if (count == 0) continue;
		if (language == Language::Russian) {
			parts.push_back(russian_hundreds(static_cast<int>(count), scale.feminine));
			parts.push_back(russian_plural(count, scale.one, scale.few, scale.many));
		} else {
			parts.push_back(english_hundreds(static_cast<int>(count)) + " " + scale.english);
		}
	}
	if (number > 0)
		parts.push_back(language == Language::Russian ? russian_hundreds(static_cast<int>(number), feminine)
													   : english_hundreds(static_cast<int>(number)));
	std::string result;
	for (const std::string &part : parts) result += (result.empty() ? "" : " ") + part;
	return result;
}

// The language of the words a text writes. What stands inside a marker
// ([[Amount5]], [[E3.short]]), an expression ({{= … as ₽}}) or an operand
// (⟦…⟧) is Singularity's own notation, not the writer's language: typed markers
// are English words, and a Russian answer full of them is still Russian.
Language language_of(std::string_view text) {
	std::size_t cyrillic = 0, latin = 0;
	const auto skip_to = [&](std::size_t from, std::string_view close) {
		const std::size_t end = text.find(close, from);
		return end == std::string_view::npos ? text.size() : end + close.size();
	};
	for (std::size_t index = 0; index < text.size();) {
		const std::string_view rest = text.substr(index);
		if (rest.rfind("[[", 0) == 0) { index = skip_to(index + 2, "]]"); continue; }
		if (rest.rfind("{{", 0) == 0) { index = skip_to(index + 2, "}}"); continue; }
		if (rest.rfind("\u27E6", 0) == 0) { index = skip_to(index + 3, "\u27E7"); continue; }
		if (is_cyrillic_lead(text[index])) { ++cyrillic; index += 2; continue; }
		if (is_ascii_letter(text[index])) ++latin;
		++index;
	}
	return cyrillic > latin ? Language::Russian : Language::English;
}

std::optional<Format> parse_format(std::string_view raw) {
	Format format;
	const std::string text = lowered(normalized(raw));
	if (text.empty()) return format;
	std::vector<std::string> words;
	std::string word;
	for (const char character : text + " ") {
		if (character == ' ') {
			if (!word.empty()) words.push_back(word);
			word.clear();
		} else {
			word.push_back(character);
		}
	}
	bool styled = false;
	const auto style = [&](FormatStyle chosen) {
		if (styled && format.style != chosen) return false;
		format.style = chosen;
		styled = true;
		return true;
	};
	for (const std::string &each : words) {
		if (each == "words" || each == "прописью") { format.words = true; continue; }
		if (each == "money" || each == "currency") { if (!style(FormatStyle::Money)) return std::nullopt; continue; }
		std::string currency;
		std::string mark;
		if (each == "$" || each == "us$") { currency = "USD"; mark = "$"; }
		else if (each == "\xE2\x82\xAC") { currency = "EUR"; mark = each; }
		else if (each == "\xC2\xA3") { currency = "GBP"; mark = each; }
		else if (each == "\xC2\xA5") { currency = "JPY"; mark = each; }
		else if (each == "\xE2\x82\xBD") { currency = "RUB"; mark = each; }
		else if (each == "руб." || each == "руб") { currency = "RUB"; mark = "руб."; }
		else if (starts_with(each, "рубл")) { currency = "RUB"; mark = "рублей"; }
		else if (!currency_of_code(each).empty()) { currency = currency_of_code(each); mark = currency; }
		if (!currency.empty()) {
			if (!format.currency.empty() && format.currency != currency) return std::nullopt;
			format.currency = currency;
			format.mark = mark;
			if (!style(FormatStyle::Money)) return std::nullopt;
			continue;
		}
		FormatStyle chosen;
		if (each == "percent" || each == "percentage" || each == "%" || each == "процент" || each == "процентах") chosen = FormatStyle::Percent;
		else if (each == "number" || each == "число") chosen = FormatStyle::Number;
		else if (each == "integer" || each == "whole") chosen = FormatStyle::Integer;
		else if (each == "days" || each == "day" || each == "дней" || each == "дни") chosen = FormatStyle::Days;
		else if (each == "weeks" || each == "недель") chosen = FormatStyle::Weeks;
		else if (each == "months" || each == "месяцев") chosen = FormatStyle::Months;
		else if (each == "years" || each == "лет") chosen = FormatStyle::Years;
		else if (each == "date" || each == "дата") chosen = FormatStyle::Date;
		else if (each == "yes/no" || each == "yesno" || each == "boolean" || each == "да/нет") chosen = FormatStyle::YesNo;
		else return std::nullopt;
		if (!style(chosen)) return std::nullopt;
	}
	return format;
}

std::string display(const Value &value, const Format &format, Language language) {
	const auto refuse = [&](const std::string &asked) {
		return AlgebraError("format", say(language, kind_words(value.kind, language) + " cannot be written as " + asked,
			"значение (" + kind_words(value.kind, language) + ") нельзя записать как " + asked));
	};
	switch (format.style) {
		case FormatStyle::Default:
			switch (value.kind) {
				case Kind::Money: return money_text(value, format, language) + money_basis_words(value.basis, language);
				case Kind::Number:
				case Kind::Count: {
					const std::string digits = number_text(value.amount, language, value.source_decimals);
					return format.words ? with_words(digits, value.amount, language, false) : digits;
				}
				case Kind::Percentage: {
					std::string text = percent_text(value, language);
					const Rational points = value.amount * Rational(100);
					if (format.words && points.is_integer()) {
						text += " (" + number_in_words(points.numerator(), language)
							+ (language == Language::Russian ? " " + russian_plural(points.numerator(), "процент", "процента", "процентов")
															 : std::string(" percent"))
							+ ")";
					}
					return text + basis_words(value.basis, language);
				}
				case Kind::Duration: {
					const std::string digits = number_text(value.amount, language);
					const std::string noun = duration_noun(value.unit, value.amount, language);
					if (format.words && value.amount.is_integer())
						return digits + " (" + number_in_words(value.amount.numerator(), language,
							value.unit == DurationUnit::Week || value.unit == DurationUnit::Minute) + ") " + noun;
					return digits + " " + noun;
				}
				case Kind::Date: return date_text(value, language);
				case Kind::Boolean: return value.truth ? say(language, "yes", "да") : say(language, "no", "нет");
				case Kind::DayOfYear: return day_of_year_text(value, language);
			}
			break;
		case FormatStyle::Money:
			if (value.kind == Kind::Money || value.kind == Kind::Number) return money_text(value, format, language) + money_basis_words(value.basis, language);
			throw refuse(say(language, "money", "денежная сумма"));
		case FormatStyle::Percent:
			if (value.kind == Kind::Percentage || value.kind == Kind::Number) {
				Value rate = make_percentage(value.amount, RateBasis::None);
				Format plain;
				plain.words = format.words;
				return display(rate, plain, language);
			}
			throw refuse(say(language, "a percentage", "процент"));
		case FormatStyle::Number:
		case FormatStyle::Integer: {
			if (value.kind == Kind::Date || value.kind == Kind::Boolean || value.kind == Kind::DayOfYear)
				throw refuse(say(language, "a number", "число"));
			const Rational number = format.style == FormatStyle::Integer ? value.amount.rounded(0) : value.amount;
			const std::string digits = number_text(number, language);
			return format.words ? with_words(digits, number, language, false) : digits;
		}
		case FormatStyle::Days:
		case FormatStyle::Weeks:
		case FormatStyle::Months:
		case FormatStyle::Years: {
			const DurationUnit unit = format.style == FormatStyle::Days ? DurationUnit::Day
				: format.style == FormatStyle::Weeks ? DurationUnit::Week
				: format.style == FormatStyle::Months ? DurationUnit::Month : DurationUnit::Year;
			Value duration = value.kind == Kind::Duration && value.unit == DurationUnit::BusinessDay && unit == DurationUnit::Day
				? value : duration_as(value, unit, language);
			Format plain;
			plain.words = format.words;
			return display(duration, plain, language);
		}
		case FormatStyle::Date:
			if (value.kind == Kind::DayOfYear) return day_of_year_text(value, language);
			if (value.kind != Kind::Date) throw refuse(say(language, "a date", "дата"));
			return date_text(value, language);
		case FormatStyle::YesNo:
			if (value.kind != Kind::Boolean) throw refuse("yes/no");
			return value.truth ? say(language, "yes", "да") : say(language, "no", "нет");
	}
	return "";
}

// ---------------------------------------------------------------------------
// Expressions

namespace {

struct Failure {
	std::string kind;
	std::string reason;
};

enum class Lex { Number, Operand, Word, Symbol, End };

struct Lexeme {
	Lex type = Lex::End;
	std::string text;
	std::size_t begin = 0;
	std::size_t end = 0;
};

const std::string kOpen = "\xE2\x9F\xA6";   // ⟦
const std::string kClose = "\xE2\x9F\xA7";  // ⟧

std::vector<Lexeme> lexemes_of(const std::string &text, Language language) {
	std::vector<Lexeme> result;
	std::size_t index = 0;
	while (index < text.size()) {
		const char character = text[index];
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') { ++index; continue; }
		if (starts_at(text, index, "\xC2\xA0")) { index += 2; continue; }
		if (starts_at(text, index, "\xE2\x80\x89") || starts_at(text, index, "\xE2\x80\xAF")) { index += 3; continue; }
		const std::size_t begin = index;
		if (starts_at(text, index, kOpen)) {
			const std::size_t close = text.find(kClose, index + kOpen.size());
			if (close == std::string::npos)
				throw Failure{"syntax", say(language, "a value opened with ⟦ is not closed with ⟧", "значение, открытое ⟦, не закрыто ⟧")};
			result.push_back({Lex::Operand, text.substr(index + kOpen.size(), close - index - kOpen.size()), begin, close + kClose.size()});
			index = close + kClose.size();
			continue;
		}
		if (is_digit(character)) {
			std::string number;
			while (index < text.size() && is_digit(text[index])) number.push_back(text[index++]);
			if (index + 1 < text.size() && text[index] == '.' && is_digit(text[index + 1])) {
				number.push_back(text[index++]);
				while (index < text.size() && is_digit(text[index])) number.push_back(text[index++]);
			}
			result.push_back({Lex::Number, number, begin, index});
			continue;
		}
		if (is_ascii_letter(character) || character == '_' || cyrillic_at(text, index)) {
			std::string word;
			while (index < text.size()) {
				if (is_ascii_letter(text[index]) || text[index] == '_' || is_digit(text[index])) { word.push_back(text[index++]); continue; }
				if (cyrillic_at(text, index)) { word += text.substr(index, 2); index += 2; continue; }
				break;
			}
			result.push_back({Lex::Word, word, begin, index});
			continue;
		}
		bool matched = false;
		for (const char *symbol : {"<=", ">=", "==", "!=", "&&", "||", "<>", "<", ">", "=", "+", "-", "*", "/", "%", "(", ")", ",", "!",
				 "\xE2\x88\x92", "\xC3\x97", "\xC3\xB7", "\xE2\x89\xA4", "\xE2\x89\xA5", "\xE2\x89\xA0"}) {
			if (!starts_at(text, index, symbol)) continue;
			std::string written = symbol;
			// −, ×, ÷, ≤, ≥, ≠ as they are typed.
			if (written == "\xE2\x88\x92") written = "-";
			else if (written == "\xC3\x97") written = "*";
			else if (written == "\xC3\xB7") written = "/";
			else if (written == "\xE2\x89\xA4") written = "<=";
			else if (written == "\xE2\x89\xA5") written = ">=";
			else if (written == "\xE2\x89\xA0" || written == "<>") written = "!=";
			index += std::string_view(symbol).size();
			result.push_back({Lex::Symbol, written, begin, index});
			matched = true;
			break;
		}
		if (!matched) {
			std::size_t length = 1;
			const unsigned char byte = static_cast<unsigned char>(character);
			if (byte >= 0xF0) length = 4;
			else if (byte >= 0xE0) length = 3;
			else if (byte >= 0xC0) length = 2;
			const std::string what = text.substr(index, length);
			bool whole = index + length <= text.size();
			for (std::size_t at = index + 1; whole && at < index + length; ++at)
				whole = (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80;
			if (!whole)
				throw Failure{"syntax", say(language, "the expression has a broken character (text cut in the middle of a letter)",
					"в выражении испорченный символ (текст обрезан посреди буквы)")};
			if (what == "[") {
				const std::size_t end = text.find("]]", index);
				const std::string symbol = end == std::string::npos ? what : text.substr(index, end + 2 - index);
				throw Failure{"operand", say(language, symbol + " is a protected symbol whose value Singularity could not put back",
					symbol + " - защищенное обозначение, значение которого Singularity не смог подставить")};
			}
			throw Failure{"syntax", say(language, "\"" + what + "\" is not part of an expression", "«" + what + "» не может стоять в выражении")};
		}
	}
	result.push_back({Lex::End, "", text.size(), text.size()});
	return result;
}

enum class NodeType { Literal, Operand, Negate, Plus, Binary, Compare, And, Or, Not, Call, Percent, Unit };

// A chain "a + b - c", "a * b / c", "a and b and c" is one node with its
// operands side by side (`ops[i]` stands before `children[i]`), so a long sum
// the model writes is worked out in a loop, not in as many nested calls as it
// has terms. Where a node is written is kept as offsets into the expression.
struct Node {
	NodeType type = NodeType::Literal;
	std::string op;
	std::vector<std::string> ops;
	std::size_t begin = 0;
	std::size_t end = 0;
	Value literal;
	DurationUnit unit = DurationUnit::Day;
	std::vector<std::unique_ptr<Node>> children;
};

using NodePtr = std::unique_ptr<Node>;

std::string lower_ascii(std::string text) {
	for (char &character : text)
		if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
	return lowered(text);
}

class Parser {
public:
	Parser(const std::string &text, Language language) : text_(text), language_(language), lexemes_(lexemes_of(text, language)) {}

	NodePtr parse() {
		NodePtr root = disjunction();
		if (peek().type != Lex::End) unexpected();
		return root;
	}

private:
	const std::string &text_;
	Language language_;
	std::vector<Lexeme> lexemes_;
	std::size_t at_ = 0;
	int depth_ = 0;

	const Lexeme &peek(std::size_t ahead = 0) const { return lexemes_[std::min(at_ + ahead, lexemes_.size() - 1)]; }

	bool symbol(const char *wanted) const { return peek().type == Lex::Symbol && peek().text == wanted; }

	bool keyword(const char *wanted) const { return peek().type == Lex::Word && lower_ascii(peek().text) == wanted; }

	// The expression as the reason quotes it: whole when it is short.
	std::string excerpt() const {
		if (text_.size() <= 120) return text_;
		std::size_t cut = 117;
		while (cut > 0 && (static_cast<unsigned char>(text_[cut]) & 0xC0) == 0x80) --cut;
		return text_.substr(0, cut) + "...";
	}

	[[noreturn]] void unexpected() const {
		const Lexeme &here = peek();
		if (here.type == Lex::End)
			throw Failure{"syntax", say(language_, "the expression \"" + excerpt() + "\" ends too early", "выражение «" + excerpt() + "» не закончено")};
		const std::string what = text_.substr(here.begin, here.end - here.begin);
		throw Failure{"syntax", say(language_, "\"" + what + "\" is not expected where it stands in \"" + excerpt() + "\"",
			"«" + what + "» стоит не на своем месте в «" + excerpt() + "»")};
	}

	// Nesting is bounded so that working an expression out fits a small thread
	// stack (32 nested if(...) take under 128 KB unoptimised; a thread has 512
	// KB). A chain of any length is one level.
	void deeper() {
		if (++depth_ > 32)
			throw Failure{"syntax", say(language_, "the expression is nested too deeply", "в выражении слишком глубокая вложенность")};
	}

	void expect(const char *wanted) {
		if (!symbol(wanted)) unexpected();
		++at_;
	}

	NodePtr made(NodeType type, std::size_t begin, std::string op = {}) {
		auto node = std::make_unique<Node>();
		node->type = type;
		node->op = std::move(op);
		close(*node, begin);
		return node;
	}

	void close(Node &node, std::size_t begin) {
		const std::size_t end = at_ > 0 ? lexemes_[at_ - 1].end : begin;
		node.begin = begin;
		node.end = end > begin ? end : begin;
	}

	// One chain node for "a or b or c" / "a and b and c".
	NodePtr logical(NodeType type, NodePtr (Parser::*operand)(), const char *word, const char *mark) {
		const std::size_t begin = peek().begin;
		NodePtr first = (this->*operand)();
		if (!(keyword(word) || symbol(mark))) return first;
		NodePtr node = made(type, begin);
		node->children.push_back(std::move(first));
		while (keyword(word) || symbol(mark)) {
			++at_;
			node->children.push_back((this->*operand)());
		}
		close(*node, begin);
		return node;
	}

	NodePtr disjunction() { return logical(NodeType::Or, &Parser::conjunction, "or", "||"); }

	NodePtr conjunction() { return logical(NodeType::And, &Parser::negation, "and", "&&"); }

	// One chain node for "a + b - c" / "a * b / c".
	NodePtr arithmetic(NodePtr (Parser::*operand)(), const char *first, const char *second) {
		const std::size_t begin = peek().begin;
		NodePtr head = (this->*operand)();
		if (!(symbol(first) || symbol(second))) return head;
		NodePtr node = made(NodeType::Binary, begin);
		node->ops.emplace_back();
		node->children.push_back(std::move(head));
		while (symbol(first) || symbol(second)) {
			node->ops.push_back(peek().text);
			++at_;
			node->children.push_back((this->*operand)());
		}
		close(*node, begin);
		return node;
	}

	NodePtr negation() {
		const std::size_t begin = peek().begin;
		if (keyword("not") || symbol("!")) {
			++at_;
			deeper();
			NodePtr inner = negation();
			--depth_;
			NodePtr node = made(NodeType::Not, begin);
			node->children.push_back(std::move(inner));
			return node;
		}
		return comparison();
	}

	NodePtr comparison() {
		const std::size_t begin = peek().begin;
		NodePtr left = sum();
		for (const char *op : {"<=", ">=", "==", "!=", "<", ">", "="}) {
			if (!symbol(op)) continue;
			++at_;
			NodePtr right = sum();
			NodePtr node = made(NodeType::Compare, begin, std::string(op) == "=" ? "==" : op);
			node->children.push_back(std::move(left));
			node->children.push_back(std::move(right));
			return node;
		}
		return left;
	}

	NodePtr sum() { return arithmetic(&Parser::product, "+", "-"); }

	NodePtr product() { return arithmetic(&Parser::unary, "*", "/"); }

	NodePtr unary() {
		const std::size_t begin = peek().begin;
		if (symbol("-") || symbol("+")) {
			const bool minus = symbol("-");
			++at_;
			deeper();
			NodePtr inner = unary();
			--depth_;
			NodePtr node = made(minus ? NodeType::Negate : NodeType::Plus, begin);
			node->children.push_back(std::move(inner));
			return node;
		}
		return postfix();
	}

	// A unit of time after a number or a parenthesis: "30 days", "5 business days", "45 дней".
	std::optional<std::pair<DurationUnit, std::size_t>> unit_here() const {
		std::vector<std::string> words;
		for (std::size_t ahead = 0; ahead < 2 && peek(ahead).type == Lex::Word; ++ahead) words.push_back(lower_ascii(peek(ahead).text));
		if (words.empty()) return std::nullopt;
		return unit_at(words, 0);
	}

	NodePtr postfix() {
		const std::size_t begin = peek().begin;
		NodePtr node = primary();
		if (const auto unit = unit_here()) {
			at_ += unit->second;
			NodePtr wrapped = made(NodeType::Unit, begin);
			wrapped->unit = unit->first;
			wrapped->children.push_back(std::move(node));
			node = std::move(wrapped);
		}
		if (symbol("%")) {
			++at_;
			NodePtr wrapped = made(NodeType::Percent, begin);
			wrapped->children.push_back(std::move(node));
			node = std::move(wrapped);
		}
		return node;
	}

	NodePtr primary() {
		const Lexeme here = peek();
		const std::size_t begin = here.begin;
		if (here.type == Lex::Number) {
			++at_;
			NodePtr node = made(NodeType::Literal, begin);
			const auto number = Rational::parse_decimal(here.text);
			if (!number) throw Failure{"syntax", say(language_, here.text + " has too many digits", "в " + here.text + " слишком много цифр")};
			node->literal = make_number(*number);
			return node;
		}
		if (here.type == Lex::Operand) {
			++at_;
			NodePtr node = made(NodeType::Operand, begin);
			node->op = here.text;
			return node;
		}
		if (symbol("(")) {
			++at_;
			deeper();
			NodePtr inner = disjunction();
			--depth_;
			expect(")");
			close(*inner, begin);
			return inner;
		}
		if (here.type == Lex::Word) {
			const std::string name = lower_ascii(here.text);
			if ((name == "true" || name == "false") && !(peek(1).type == Lex::Symbol && peek(1).text == "(")) {
				++at_;
				NodePtr node = made(NodeType::Literal, begin);
				node->literal = make_boolean(name == "true");
				return node;
			}
			if (peek(1).type == Lex::Symbol && peek(1).text == "(") {
				at_ += 2;
				deeper();
				NodePtr node = made(NodeType::Call, begin, name);
				if (!symbol(")")) {
					node->children.push_back(disjunction());
					while (symbol(",")) {
						++at_;
						node->children.push_back(disjunction());
					}
				}
				--depth_;
				expect(")");
				close(*node, begin);
				return node;
			}
			throw Failure{"syntax", say(language_, "\"" + here.text + "\" is not a value or a function Singularity knows",
				"«" + here.text + "» - не значение и не функция, известная Singularity")};
		}
		unexpected();
	}
};

// ---------------------------------------------------------------------------
// Working an expression out.

class Evaluator {
public:
	Evaluator(const std::string &text, Language language) : text_(text), language_(language) {}

	Value run(const Node &node) {
		try {
			return eval(node);
		} catch (const AlgebraError &error) {
			throw Failure{error.kind(), std::string(error.what())};
		}
	}

private:
	const std::string &text_;
	Language language_;

	std::string say(const std::string &english, const std::string &russian) const {
		return language_ == Language::Russian ? russian : english;
	}

	// The node as the expression writes it.
	std::string name(const Node &node) const { return text_.substr(node.begin, node.end - node.begin); }

	[[noreturn]] void type_error(const std::string &english, const std::string &russian) const {
		throw Failure{"type", say(english, russian)};
	}

	// A day of the year is no amount and no place in time: it is compared (==,
	// !=), matched to a date (same_day_of_year) or placed on or after one
	// (next_occurrence), and nothing is added to it or worked out from it.
	void no_day_of_year(const Value &value, const Node &node) const {
		if (value.kind == Kind::DayOfYear)
			type_error(name(node) + " is a day of the year with no year: use same_day_of_year() or next_occurrence() with a date",
				name(node) + " - день года без указания года: используйте same_day_of_year() или next_occurrence() с датой");
	}

	// Only dispatches: every case works in a function of its own, so a nested
	// level holds only the stack its own case needs.
	Value eval(const Node &node) {
		switch (node.type) {
			case NodeType::Literal: return node.literal;
			case NodeType::Operand: return operand(node);
			case NodeType::Negate: return negate(eval(*node.children[0]), *node.children[0]);
			case NodeType::Plus: return plus(node);
			case NodeType::Binary: return chain(node);
			case NodeType::Compare: return make_boolean(decide(node));
			case NodeType::And:
			case NodeType::Or: return make_boolean(logical(node));
			case NodeType::Not: return make_boolean(!condition(*node.children[0]));
			case NodeType::Percent: return percent(node);
			case NodeType::Unit: return period(node);
			case NodeType::Call: return call(node);
		}
		throw Failure{"syntax", "the expression cannot be read"};
	}

	Value plus(const Node &node) {
		Value value = eval(*node.children[0]);
		no_day_of_year(value, *node.children[0]);
		if (value.kind == Kind::Date || value.kind == Kind::Boolean)
			type_error(name(*node.children[0]) + " is not a quantity", name(*node.children[0]) + " - не величина");
		return value;
	}

	// "a + b - c", "a * b / c": left to right; the left operand is named as
	// all written so far.
	//
	// A rate per annum times days waits for the chain to say how many days
	// make the year: "principal * rate * days / 365" is exact once the "/ 365"
	// comes, and refused only when the chain ends (or adds) without it. A
	// parenthesised product hands its wait on: "(rate * days) / 365".
	// `carry`: where this chain leaves a wait it ends with, instead of refusing.
	Value chain(const Node &node, std::optional<Failure> *carry = nullptr) {
		std::optional<Failure> pending;
		const auto operand = [this, &pending](const Node &child, bool mayWait) {
			if (!mayWait || child.type != NodeType::Binary) return eval(child);
			std::optional<Failure> carried;
			Value value = chain(child, &carried);
			if (carried) {
				if (pending) throw *carried;
				pending = carried;
			}
			return value;
		};
		Value result = operand(*node.children[0], true);
		no_day_of_year(result, *node.children[0]);
		Node left;
		left.begin = node.children[0]->begin;
		left.end = node.children[0]->end;
		// A plain count in the product: "principal * rate * 45 / 365" is the
		// interest for 45 days, a lump sum - not a daily figure.
		bool counted = result.kind == Kind::Number;
		for (std::size_t index = 1; index < node.children.size(); ++index) {
			const Node &right = *node.children[index];
			const std::string &op = node.ops[index];
			const bool waited = pending.has_value();
			const Value b = operand(right, op == "*");
			no_day_of_year(b, right);
			if (op == "*" && b.kind == Kind::Number) counted = true;
			if (!waited && pending) {
				// The operand itself waits for its year; it multiplies as a factor.
				result = multiply(result, b, left, right);
				left.end = right.end;
				continue;
			}
			if (pending) {
				const bool year = op == "/" && days_in_a_year(b);
				const bool factor = op == "*"
					&& (b.kind == Kind::Number || b.kind == Kind::Money || (b.kind == Kind::Percentage && b.basis == RateBasis::None));
				if (!year && !factor) throw *pending;
				if (year) pending.reset();
			}
			if (op == "+") result = add(result, b, left, right, false);
			else if (op == "-") result = add(result, b, left, right, true);
			else if (op == "*") result = multiply(result, b, left, right, pending ? nullptr : &pending);
			else {
				const RateBasis basis = result.basis;
				result = divide(result, b, left, right);
				if (counted && basis != RateBasis::None && result.basis != basis) result.basis = RateBasis::None;
			}
			left.end = right.end;
		}
		if (pending) {
			if (carry == nullptr) throw *pending;
			*carry = pending;
		}
		return result;
	}

	// The days a contract gives a year: 360 to 366.
	static bool days_in_a_year(const Value &value) {
		return value.kind == Kind::Number && value.amount.is_integer() && value.amount >= Rational(360) && value.amount <= Rational(366);
	}

	Value percent(const Node &node) {
		const Value value = eval(*node.children[0]);
		if (value.kind != Kind::Number)
			type_error(name(*node.children[0]) + " is not a number, so it cannot be a percentage",
				name(*node.children[0]) + " - не число и не может быть процентом");
		return make_percentage(value.amount / Rational(100));
	}

	Value period(const Node &node) {
		const Value value = eval(*node.children[0]);
		if (value.kind != Kind::Number)
			type_error(name(*node.children[0]) + " is not a number of " + unit_words(node.unit, language_),
				name(*node.children[0]) + " - не число, а срок считается в числах");
		return make_duration(value.amount, node.unit);
	}

	Value operand(const Node &node) {
		const std::string written = "⟦" + node.op + "⟧";
		if (node.op.find("[[") != std::string::npos)
			throw Failure{"operand", say(written + " is a protected symbol Singularity could not put the value back into",
				written + " - защищенное обозначение, вместо которого Singularity не смог подставить значение")};
		auto value = parse_value(node.op);
		if (!value)
			throw Failure{"operand", say(written + " is not a quantity or a date Singularity can calculate with",
				written + " - не величина и не дата, с которой Singularity может считать")};
		return *value;
	}

	bool condition(const Node &node) {
		const Value value = eval(node);
		if (value.kind != Kind::Boolean)
			type_error(name(node) + " is not a condition (true or false)", name(node) + " - не условие (да или нет)");
		return value.truth;
	}

	// and / or, short-circuit; an undetermined operand is forgiven only when
	// another decides alone.
	bool logical(const Node &node) {
		const bool conjunction = node.type == NodeType::And;
		std::optional<Failure> firstFailure;
		for (const auto &child : node.children) {
			try {
				const bool truth = condition(*child);
				if (conjunction && !truth) return false;
				if (!conjunction && truth) return true;
			} catch (const Failure &failure) {
				if (failure.kind != "undetermined") throw;
				if (!firstFailure) firstFailure = failure;
			} catch (const AlgebraError &error) {
				if (error.kind() != "undetermined") throw;
				if (!firstFailure) firstFailure = Failure{error.kind(), error.what()};
			}
		}
		if (firstFailure) throw *firstFailure;
		return conjunction;
	}

	bool decide(const Node &node) {
		const Node &left = *node.children[0];
		const Node &right = *node.children[1];
		const Value a = eval(left);
		const Value b = eval(right);
		const std::string &op = node.op;
		const Comparator comparator = op == "<" ? Comparator::Less : op == "<=" ? Comparator::LessOrEqual
			: op == ">" ? Comparator::Greater : op == ">=" ? Comparator::GreaterOrEqual
			: op == "==" ? Comparator::Equal : Comparator::NotEqual;
		return settled(compare(a, comparator, b, language_, name(left), name(right)));
	}

	static bool settled(const Relation &relation) {
		if (relation.truth == Truth::Undetermined) throw Failure{"undetermined", relation.reason};
		return relation.truth == Truth::True;
	}

	Value negate(Value value, const Node &node) const {
		no_day_of_year(value, node);
		if (value.kind == Kind::Date || value.kind == Kind::Boolean)
			type_error(name(node) + " cannot be negative", name(node) + " не может быть отрицательным");
		value.amount = -value.amount;
		return value;
	}

	// ---- + and -

	std::string merged_currency(const Value &a, const Value &b, const Node &left, const Node &right) const {
		require_one_currency(a, b, name(left), name(right), language_);
		return a.currency;
	}

	[[noreturn]] void business_days(const Node &node) const {
		throw Failure{"undetermined", say(name(node) + " is counted in business days, which depend on a holiday calendar Singularity does not have",
			name(node) + " считается в рабочих днях, а они зависят от календаря праздников, которого у Singularity нет")};
	}

	Value add(const Value &a, const Value &b, const Node &left, const Node &right, bool subtract) {
		const auto combine = [subtract](const Rational &x, const Rational &y) { return subtract ? x - y : x + y; };
		const std::string op = subtract ? "-" : "+";
		// Dates.
		if (a.kind == Kind::Date && b.kind == Kind::Date) {
			if (!subtract) type_error(name(left) + " and " + name(right) + " are dates, and dates do not add up",
				name(left) + " и " + name(right) + " - даты, их нельзя складывать");
			return make_duration(Rational(days_between(a, b, left, right)) * Rational(-1), DurationUnit::Day);
		}
		if (a.kind == Kind::Date) return shifted(a, b, left, right, subtract);
		if (b.kind == Kind::Date) {
			if (subtract) type_error("a date cannot be taken from " + name(left), "дату нельзя вычесть из " + name(left));
			return shifted(b, a, right, left, false);
		}
		if (a.kind == Kind::Boolean || b.kind == Kind::Boolean)
			type_error("true or false cannot be added", "«да» и «нет» нельзя складывать");
		// Money.
		if (a.kind == Kind::Money || b.kind == Kind::Money) {
			const Value &money = a.kind == Kind::Money ? a : b;
			const Value &other = a.kind == Kind::Money ? b : a;
			const Node &otherNode = a.kind == Kind::Money ? right : left;
			if (other.kind == Kind::Number) {
				// A plain number has no currency; only zero is zero in every one.
				if (!other.amount.is_zero()) throw number_is_not_money(name(otherNode), name(left) + " " + op + " " + name(right), language_);
				Value result = money;
				result.amount = combine(a.amount, b.amount);
				return result;
			}
			if (other.kind == Kind::Money) {
				Value result = make_money(combine(a.amount, b.amount), merged_currency(a, b, left, right),
					std::max(a.source_decimals, b.source_decimals));
				if (different_bases(a, b)) different_periods(a, b, left, right, op);
				if (a.basis != b.basis) throw per_period_and_lump_sum(a, b, name(left), name(right), language_);
				result.basis = a.basis;
				return result;
			}
			if (other.kind == Kind::Percentage)
				type_error("a percentage cannot be added to an amount: write the amount times (1 + " + name(otherNode) + ")",
					"процент нельзя прибавить к сумме: запишите сумму, умноженную на (1 + " + name(otherNode) + ")");
			type_error(name(left) + " " + op + " " + name(right) + " mixes an amount of money with " + kind_words(other.kind, language_),
				name(left) + " " + op + " " + name(right) + " смешивает денежную сумму и " + kind_words(other.kind, language_));
		}
		// Periods.
		if (a.kind == Kind::Duration || b.kind == Kind::Duration) {
			if (a.kind == Kind::Duration && b.kind == Kind::Duration) {
				if ((a.unit == DurationUnit::BusinessDay) != (b.unit == DurationUnit::BusinessDay))
					business_days(a.unit == DurationUnit::BusinessDay ? left : right);
				if (a.unit == b.unit) return make_duration(combine(a.amount, b.amount), a.unit);
				if (day_class(a.unit) && day_class(b.unit)) return make_duration(combine(in_days(a), in_days(b)), DurationUnit::Day);
				if (month_class(a.unit) && month_class(b.unit)) return make_duration(combine(in_months(a), in_months(b)), DurationUnit::Month);
				throw Failure{"undetermined", say(name(left) + " " + op + " " + name(right) + " is not exact: a month has 28 to 31 days",
					name(left) + " " + op + " " + name(right) + " нельзя посчитать точно: в месяце от 28 до 31 дня")};
			}
			const Value &period = a.kind == Kind::Duration ? a : b;
			const Value &other = a.kind == Kind::Duration ? b : a;
			if (other.kind != Kind::Number)
				type_error(name(left) + " " + op + " " + name(right) + " mixes a period with " + kind_words(other.kind, language_),
					name(left) + " " + op + " " + name(right) + " смешивает срок и " + kind_words(other.kind, language_));
			return make_duration(combine(a.amount, b.amount), period.unit);
		}
		// Numbers and percentages.
		if (a.kind == Kind::Percentage && b.kind == Kind::Percentage) {
			if (different_bases(a, b)) different_periods(a, b, left, right, op);
			return make_percentage(combine(a.amount, b.amount), a.basis == b.basis ? a.basis : RateBasis::None);
		}
		if (a.kind == Kind::Percentage || b.kind == Kind::Percentage) {
			// A rate meets a plain number only as a growth factor (1 + 3 %) or
			// against zero; "3% + 2" says neither 2 % nor a factor.
			const Value &number = a.kind == Kind::Percentage ? b : a;
			const Node &numberNode = a.kind == Kind::Percentage ? right : left;
			if (number.amount.is_zero()) {
				Value result = a.kind == Kind::Percentage ? a : b;
				result.amount = combine(a.amount, b.amount);
				return result;
			}
			if (number.amount != Rational(1))
				type_error(name(left) + " " + op + " " + name(right) + " adds a plain number to a percentage: write " + name(numberNode)
						+ "% for percentage points, or 1 " + op + " the rate for a growth factor",
					name(left) + " " + op + " " + name(right) + " складывает число и процент: запишите " + name(numberNode)
						+ "%, если это процентные пункты, или 1 " + op + " ставка для коэффициента роста");
		}
		Value result = make_number(combine(a.amount, b.amount));
		result.source_decimals = std::max(a.source_decimals, b.source_decimals);
		return result;
	}

	[[noreturn]] void different_periods(const Value &a, const Value &b, const Node &left, const Node &right, const std::string &op) const {
		type_error(name(left) + " " + op + " " + name(right) + " mixes " + basis_name(a.basis, language_) + " and "
				+ basis_name(b.basis, language_) + ": put both for the same period first",
			name(left) + " " + op + " " + name(right) + " смешивает " + basis_name(a.basis, language_) + " и "
				+ basis_name(b.basis, language_) + ": сначала приведите их к одному периоду");
	}

	// `months`: the count asked for is of months (months_between), and the
	// reason says so.
	long long days_between(const Value &from, const Value &to, const Node &fromNode, const Node &toNode, bool months = false) const {
		for (const auto &[value, node] : {std::make_pair(&from, &fromNode), std::make_pair(&to, &toNode)}) {
			if (value->kind != Kind::Date) type_error(name(*node) + " is not a date", name(*node) + " - не дата");
			if (value->date.precision != temporal::Precision::Day)
				throw Failure{"undetermined", say(name(*node) + " is " + precision_noun(value->date.precision, language_)
						+ ", not a day, so the number of " + (months ? "months" : "days") + " is not known from the dates",
					name(*node) + " - " + precision_noun(value->date.precision, language_) + ", а не день, поэтому число "
						+ (months ? "месяцев" : "дней") + " по датам неизвестно")};
		}
		return to.date.first_day - from.date.first_day;
	}

	Int128 whole_count(const Value &value, const Node &node) const {
		if (!value.amount.is_integer())
			throw Failure{"type", say(name(node) + " is not a whole number", name(node) + " - не целое число")};
		const Int128 count = value.amount.numerator();
		if (count > 1000000 || count < -1000000) throw overflow();
		return count;
	}

	// A date moved by a period: days on a day; months on a day (the end of a
	// month stays the end: January 31 + 1 month is February 28 or 29), a month
	// or a quarter; years on any date.
	// `bare`: the period was written as a plain count ("add_days(d, 3)"), and a
	// reason names it with its unit ("3 days").
	Value shifted(const Value &date, const Value &period, const Node &dateNode, const Node &periodNode, bool subtract,
		bool bare = false) const {
		Value amount = period;
		bare = bare || period.kind == Kind::Number;
		if (period.kind == Kind::Number) amount = make_duration(period.amount, DurationUnit::Day);
		if (amount.kind != Kind::Duration)
			type_error(name(periodNode) + " is not a period, so it cannot move the date " + name(dateNode),
				name(periodNode) + " - не срок, им нельзя сдвинуть дату " + name(dateNode));
		if (amount.unit == DurationUnit::BusinessDay) business_days(periodNode);
		// Hours and minutes move a date only by whole days.
		if (amount.unit == DurationUnit::Hour || amount.unit == DurationUnit::Minute) {
			const Rational days = in_days(amount);
			if (!days.is_integer())
				type_error(name(periodNode) + " is not a whole number of days, so it cannot move the date " + name(dateNode),
					name(periodNode) + " - не целое число дней, им нельзя сдвинуть дату " + name(dateNode));
			amount = make_duration(days, DurationUnit::Day);
		}
		Int128 count = whole_count(amount, periodNode);
		if (subtract) count = -count;
		switch (amount.unit) {
			case DurationUnit::Hour:
			case DurationUnit::Minute: break;
			case DurationUnit::Quarter: return add_months_to(date, static_cast<long long>(count * 3), dateNode);
			case DurationUnit::Week: count *= 7; [[fallthrough]];
			case DurationUnit::Day: {
				// A bare count is named with its unit and its sign as a direction:
				// add_days(d, -3) asks for the date 3 days earlier.
				if (!bare) return add_days_to(date, static_cast<long long>(count), dateNode, name(periodNode), subtract);
				const Value days = make_duration(Rational(count).abs(), DurationUnit::Day);
				return add_days_to(date, static_cast<long long>(count), dateNode, display(days, {}, language_), count < 0);
			}
			case DurationUnit::Year: count *= 12; [[fallthrough]];
			case DurationUnit::Month: return add_months_to(date, static_cast<long long>(count), dateNode);
			case DurationUnit::BusinessDay: business_days(periodNode);
		}
		return date;
	}

	Value add_days_to(const Value &date, long long days, const Node &dateNode, const std::string &period, bool earlier) const {
		if (date.date.precision != temporal::Precision::Day)
			throw Failure{"undetermined", say(name(dateNode) + " is " + precision_noun(date.date.precision, language_)
					+ ", not a day, so the date " + period + (earlier ? " earlier" : " later") + " is not known",
				name(dateNode) + " - " + precision_noun(date.date.precision, language_) + ", а не день, поэтому дата "
					+ (earlier ? "за " + period + " до нее" : "через " + period) + " неизвестна")};
		Value result = date;
		result.date = day_value(date.date.first_day + days);
		check_year(result, dateNode);
		return result;
	}

	void check_year(const Value &value, const Node &node) const {
		const Civil civil = civil_from_days(value.date.first_day);
		if (civil.year < 1 || civil.year > 9999)
			throw Failure{"overflow", say("the date from " + name(node) + " falls outside any calendar year Singularity writes",
				"дата от " + name(node) + " выходит за пределы календаря")};
	}

	Value add_months_to(const Value &date, long long months, const Node &dateNode) const {
		Value result = date;
		const Civil first = civil_from_days(date.date.first_day);
		const long long index = first.year * 12 + static_cast<long long>(first.month - 1) + months;
		const long long year = index >= 0 ? index / 12 : (index - 11) / 12;
		const unsigned month = static_cast<unsigned>(index - year * 12 + 1);
		if (year < 1 || year > 9999) throw overflow();
		switch (date.date.precision) {
			case temporal::Precision::Day: {
				const Civil civil = civil_from_days(date.date.first_day);
				const unsigned day = std::min(civil.day, month_length(year, month));
				result.date = day_value(days_from_civil(year, month, day));
				return result;
			}
			case temporal::Precision::Month:
				result.date = month_value(year, month);
				return result;
			case temporal::Precision::Quarter:
				if (months % 3 == 0) {
					result.date = {days_from_civil(year, month, 1), days_from_civil(year, month + 2, month_length(year, month + 2)),
						temporal::Precision::Quarter};
					return result;
				}
				break;
			case temporal::Precision::Year:
				if (months % 12 == 0) {
					result.date = {days_from_civil(year, 1, 1), days_from_civil(year, 12, 31), temporal::Precision::Year};
					return result;
				}
				break;
		}
		throw Failure{"undetermined", say(name(dateNode) + " is " + precision_noun(date.date.precision, language_)
				+ ", so a date some months from it is not a whole period of the same kind",
			name(dateNode) + " - " + precision_noun(date.date.precision, language_) + ", и сдвиг на эти месяцы не дает такой же целый период")};
	}

	// ---- * and /

	// `pending`: where a rate per annum times days may wait for "/ 365" (see chain).
	Value multiply(const Value &a, const Value &b, const Node &left, const Node &right, std::optional<Failure> *pending = nullptr) {
		const std::string both = name(left) + " * " + name(right);
		if (a.kind == Kind::Date || b.kind == Kind::Date || a.kind == Kind::Boolean || b.kind == Kind::Boolean)
			type_error(both + ": only quantities multiply", both + ": умножать можно только величины");
		if (a.kind == Kind::Money && b.kind == Kind::Money)
			type_error(both + " multiplies an amount by an amount, which is not an amount",
				both + " умножает сумму на сумму, и результат - не сумма");
		if (a.kind == Kind::Duration && b.kind == Kind::Duration)
			type_error(both + " multiplies a period by a period", both + " умножает срок на срок");
		if (a.kind == Kind::Duration || b.kind == Kind::Duration) {
			const bool periodFirst = a.kind == Kind::Duration;
			const Value &period = periodFirst ? a : b;
			const Value &other = periodFirst ? b : a;
			if (other.kind == Kind::Money || other.kind == Kind::Percentage) {
				// A rate, or an amount owed per day, month or year, for a period:
				// 0.1 % per day for 45 days is 4.5 %; the period is counted in the
				// rate's own units.
				Value result = other;
				result.amount = other.amount * periods_in(other, period, periodFirst ? right : left, periodFirst ? left : right, pending);
				result.basis = RateBasis::None;
				return result;
			}
			return make_duration(a.amount * b.amount, period.unit);
		}
		if (a.kind == Kind::Money || b.kind == Kind::Money) {
			const Value &money = a.kind == Kind::Money ? a : b;
			const Value &other = a.kind == Kind::Money ? b : a;
			Value result = make_money(a.amount * b.amount, money.currency, money.source_decimals);
			// An amount at a daily rate is owed per day, and scaled by a plain
			// number it still is: never a lump sum that months could multiply.
			if (other.kind == Kind::Number) result.basis = rescaled_basis(money.basis, other.amount, false);
			if (other.kind == Kind::Percentage) {
				if (money.basis != RateBasis::None && other.basis != RateBasis::None)
					type_error(both + " applies a rate " + basis_name(other.basis, language_) + " to an amount that is already "
							+ basis_name(money.basis, language_),
						both + " применяет ставку " + basis_name(other.basis, language_) + " к сумме, которая уже "
							+ basis_name(money.basis, language_));
				result.basis = money.basis != RateBasis::None ? money.basis : other.basis;
			}
			return result;
		}
		// A rate scaled is a rate for the same period (see rescaled_basis); a
		// share of a rate (half of 18 % per annum) is too.
		if (a.kind == Kind::Percentage && b.kind == Kind::Percentage) {
			if (a.basis != RateBasis::None && b.basis != RateBasis::None)
				type_error(both + " multiplies a rate " + basis_name(a.basis, language_) + " by a rate " + basis_name(b.basis, language_),
					both + " умножает ставку " + basis_name(a.basis, language_) + " на ставку " + basis_name(b.basis, language_));
			return make_percentage(a.amount * b.amount, a.basis != RateBasis::None ? a.basis : b.basis);
		}
		if (a.kind == Kind::Percentage || b.kind == Kind::Percentage) {
			const Value &rate = a.kind == Kind::Percentage ? a : b;
			const Value &number = a.kind == Kind::Percentage ? b : a;
			return make_percentage(a.amount * b.amount, rescaled_basis(rate.basis, number.amount, false));
		}
		return make_number(a.amount * b.amount);
	}

	// How many of a rate's periods `period` is: 45 days of a daily rate is 45,
	// 6 months of a yearly one is 1/2. Days against months or years are not
	// exact, and a year of days is the contract's to say (365, 366 or 360).
	Rational periods_in(const Value &rated, const Value &period, const Node &ratedNode, const Node &periodNode,
		std::optional<Failure> *pending = nullptr) const {
		// A rate of no known period is never counted over one: 5 % "per annum"
		// that its clause did not let Singularity read, times 45 days, came out an
		// exact 225 %. The expression may say the period itself with plain
		// numbers ("[[Rate1]] * 45 / 365"), which never reaches here; else it is
		// UNKNOWN, as Singularity's own rate op is (ProtectedOperations,
		// rate_basis_unknown). An amount of no period times one ("[[Rent]] * 3
		// months") is that amount per unit of the period, as the expression writes.
		if (rated.basis == RateBasis::None) {
			if (rated.kind != Kind::Percentage) return period.amount;
			const std::string both = name(ratedNode) + " * " + name(periodNode);
			throw Failure{"undetermined", say(both + ": the text does not say what period " + name(ratedNode)
					+ " is for (per day, per month or per annum); write the period as plain numbers (rate * 45 / 365) "
					"or ask the lawyer",
				both + ": из текста не видно, за какой период " + name(ratedNode)
					+ " (в день, в месяц или годовых); запишите период числами (ставка * 45 / 365) или спросите юриста")};
		}
		if (period.unit == DurationUnit::BusinessDay) business_days(periodNode);
		const std::string both = name(ratedNode) + " * " + name(periodNode);
		switch (rated.basis) {
			case RateBasis::None: break;
			case RateBasis::PerDay: {
				if (day_class(period.unit)) return in_days(period);
				const bool years = period.unit == DurationUnit::Year;
				throw Failure{"undetermined", say(both + ": " + name(ratedNode) + " is per day and " + name(periodNode)
						+ " is counted in " + unit_words(period.unit, language_) + ", and "
						+ (years ? "a year has 365 or 366 days" : "a month has 28 to 31 days")
						+ "; count the days (days_between) or write the period in days",
					both + ": " + name(ratedNode) + " - в день, а " + name(periodNode) + " считается в единицах «"
						+ unit_words(period.unit, language_) + "», а " + (years ? "в году 365 или 366 дней" : "в месяце от 28 до 31 дня")
						+ "; посчитайте дни (days_between) или запишите срок в днях")};
			}
			case RateBasis::PerMonth:
				if (month_class(period.unit)) return in_months(period);
				throw Failure{"undetermined", say(both + ": " + name(ratedNode) + " is per month and " + name(periodNode)
						+ " is counted in " + unit_words(period.unit, language_) + ", and a month has 28 to 31 days; write the period in months",
					both + ": " + name(ratedNode) + " - в месяц, а " + name(periodNode) + " считается в единицах «"
						+ unit_words(period.unit, language_) + "», а в месяце от 28 до 31 дня; запишите срок в месяцах")};
			case RateBasis::PerYear: {
				if (month_class(period.unit)) return in_months(period) / Rational(12);
				const Failure failure{"undetermined", say(both + ": " + name(ratedNode) + " is per annum and " + name(periodNode)
						+ " is counted in " + unit_words(period.unit, language_)
						+ "; how many days make the year (365, 366 or 360) is for the contract to say, so write it out: rate * days / 365",
					both + ": " + name(ratedNode) + " - годовых, а " + name(periodNode) + " считается в единицах «"
						+ unit_words(period.unit, language_)
						+ "»; сколько дней в году (365, 366 или 360), решает договор, поэтому запишите это явно: ставка * дни / 365")};
				// The days, to be divided by the year the chain writes next.
				if (pending != nullptr && day_class(period.unit)) {
					*pending = failure;
					return in_days(period);
				}
				throw failure;
			}
		}
		return period.amount;
	}

	Value divide(const Value &a, const Value &b, const Node &left, const Node &right) {
		const std::string both = name(left) + " / " + name(right);
		if (a.kind == Kind::Date || b.kind == Kind::Date || a.kind == Kind::Boolean || b.kind == Kind::Boolean)
			type_error(both + ": only quantities divide", both + ": делить можно только величины");
		if (b.amount.is_zero())
			throw Failure{"division_by_zero", say("division by zero: " + name(right) + " is zero, so " + both + " has no value",
				"деление на ноль: " + name(right) + " равно нулю, поэтому у " + both + " нет значения")};
		if (a.kind == Kind::Money) {
			if (b.kind == Kind::Money) {
				merged_currency(a, b, left, right);
				return make_number(a.amount / b.amount);
			}
			// A per-day amount divided is still owed per day (see rescaled_basis);
			// a daily amount over its daily rate is the amount the rate is of.
			Value result = make_money(a.amount / b.amount, a.currency, a.source_decimals);
			if (b.kind == Kind::Number) result.basis = rescaled_basis(a.basis, b.amount, true);
			else if (b.kind == Kind::Percentage) {
				if (b.basis != RateBasis::None && b.basis != a.basis)
					type_error(both + " divides " + (a.basis == RateBasis::None ? std::string("a lump sum") : "an amount " + basis_name(a.basis, language_))
							+ " by a rate " + basis_name(b.basis, language_),
						both + " делит " + (a.basis == RateBasis::None ? std::string("единовременную сумму") : "сумму " + basis_name(a.basis, language_))
							+ " на ставку " + basis_name(b.basis, language_));
				result.basis = b.basis == a.basis ? RateBasis::None : a.basis;
			} else {
				result.basis = a.basis;
			}
			return result;
		}
		if (b.kind == Kind::Money)
			type_error(both + " divides " + kind_words(a.kind, language_) + " by an amount of money",
				both + " делит " + kind_words(a.kind, language_) + " на денежную сумму");
		if (a.kind == Kind::Duration) {
			if (b.kind == Kind::Duration) {
				if (a.unit == b.unit) return make_number(a.amount / b.amount);
				if ((a.unit == DurationUnit::BusinessDay) != (b.unit == DurationUnit::BusinessDay))
					business_days(a.unit == DurationUnit::BusinessDay ? left : right);
				if (day_class(a.unit) && day_class(b.unit)) return make_number(in_days(a) / in_days(b));
				if (month_class(a.unit) && month_class(b.unit)) return make_number(in_months(a) / in_months(b));
				throw Failure{"undetermined", say(both + " is not exact: a month has 28 to 31 days",
					both + " нельзя посчитать точно: в месяце от 28 до 31 дня")};
			}
			if (b.kind == Kind::Percentage) type_error(both + " divides a period by a percentage", both + " делит срок на процент");
			return make_duration(a.amount / b.amount, a.unit);
		}
		if (b.kind == Kind::Duration)
			type_error(both + " divides " + kind_words(a.kind, language_) + " by a period",
				both + " делит " + kind_words(a.kind, language_) + " на срок");
		// 18 % per annum / 365 is a daily rate, / 2 half the yearly one.
		if (a.kind == Kind::Percentage && b.kind == Kind::Number) return make_percentage(a.amount / b.amount, rescaled_basis(a.basis, b.amount, true));
		return make_number(a.amount / b.amount);
	}

	// ---- functions

	void arity(const Node &node, std::size_t least, std::size_t most) const {
		const std::size_t count = node.children.size();
		if (count < least || count > most) {
			const std::string wanted = least == most ? std::to_string(least)
				: most == static_cast<std::size_t>(-1) ? "at least " + std::to_string(least)
				: std::to_string(least) + " or " + std::to_string(most);
			const std::string wantedRu = least == most ? std::to_string(least)
				: most == static_cast<std::size_t>(-1) ? "не меньше " + std::to_string(least)
				: std::to_string(least) + " или " + std::to_string(most);
			throw Failure{"arity", say(node.op + "(...) takes " + wanted + (least == 1 && (most == 1 || most == static_cast<std::size_t>(-1)) ? " value" : " values") + ", and " + name(node)
					+ " gives " + std::to_string(count),
				"число значений в " + node.op + "(...): " + wantedRu + ", а в " + name(node) + " - " + std::to_string(count))};
		}
	}

	Value date_argument(const Node &node) {
		Value value = eval(node);
		if (value.kind != Kind::Date) type_error(name(node) + " is not a date", name(node) + " - не дата");
		return value;
	}

	bool less(const Value &a, const Value &b, const Node &left, const Node &right) {
		return settled(compare(a, Comparator::Less, b, language_, name(left), name(right)));
	}

	// min/max keep the value chosen; a plain number chosen among amounts is an amount.
	Value extreme(const Node &node, bool smallest) {
		arity(node, 1, static_cast<std::size_t>(-1));
		Value best = eval(*node.children[0]);
		std::size_t bestAt = 0;
		std::string currency = best.kind == Kind::Money ? best.currency : "";
		bool money = best.kind == Kind::Money;
		for (std::size_t index = 1; index < node.children.size(); ++index) {
			const Value next = eval(*node.children[index]);
			if (next.kind == Kind::Boolean) type_error(name(*node.children[index]) + " is not a quantity", name(*node.children[index]) + " - не величина");
			if (next.kind == Kind::Money) {
				money = true;
				if (currency.empty()) currency = next.currency;
			}
			const bool better = smallest ? less(next, best, *node.children[index], *node.children[bestAt])
				: less(best, next, *node.children[bestAt], *node.children[index]);
			if (better) {
				best = next;
				bestAt = index;
			}
		}
		if (best.kind == Kind::Boolean) type_error(name(*node.children[0]) + " is not a quantity", name(*node.children[0]) + " - не величина");
		if (money && best.kind == Kind::Number) best = make_money(best.amount, currency);
		return best;
	}

	int places_of(const Node &node, std::size_t index) {
		if (node.children.size() <= index) return 0;
		const Value places = eval(*node.children[index]);
		if (places.kind != Kind::Number || !places.amount.is_integer() || places.amount < Rational(-12) || places.amount > Rational(12))
			type_error(name(*node.children[index]) + " is not a number of decimal places", name(*node.children[index]) + " - не число знаков после запятой");
		return static_cast<int>(places.amount.numerator());
	}

	Value rounded_value(const Node &node, int mode) {
		arity(node, 1, 2);
		Value value = eval(*node.children[0]);
		no_day_of_year(value, *node.children[0]);
		if (value.kind == Kind::Date || value.kind == Kind::Boolean)
			type_error(name(*node.children[0]) + " cannot be rounded", name(*node.children[0]) + " нельзя округлить");
		const int places = places_of(node, 1);
		const auto apply = [mode, places](const Rational &number) {
			if (places < 0) {
				const Rational unit(pow10(-places));
				const Rational scaled = number / unit;
				return (mode == 0 ? scaled.rounded(0) : mode < 0 ? scaled.floored(0) : scaled.ceiled(0)) * unit;
			}
			return mode == 0 ? number.rounded(places) : mode < 0 ? number.floored(places) : number.ceiled(places);
		};
		// A percentage is rounded in its points: round(3.456 %, 2) is 3.46 %.
		if (value.kind == Kind::Percentage) value.amount = apply(value.amount * Rational(100)) / Rational(100);
		else value.amount = apply(value.amount);
		// Written with no more places than it was rounded to: round(2.996, 1) is
		// 3, never 3.000; an amount rounded to whole units shows no cents.
		value.source_decimals = value.kind == Kind::Money ? std::min(value.source_decimals, std::max(places, 0)) : 0;
		return value;
	}

	// Only dispatches, like eval: each function works in its own frame.
	Value call(const Node &node) {
		const std::string &function = node.op;
		if (function == "min" || function == "max") return extreme(node, function == "min");
		if (function == "sum" || function == "avg" || function == "average") return total(node, function == "sum");
		if (function == "abs") return absolute(node);
		if (function == "round") return rounded_value(node, 0);
		if (function == "floor") return rounded_value(node, -1);
		if (function == "ceil" || function == "ceiling") return rounded_value(node, 1);
		if (function == "if") {
			arity(node, 3, 3);
			return condition(*node.children[0]) ? eval(*node.children[1]) : eval(*node.children[2]);
		}
		if (function == "clamp") return clamped(node);
		if (function == "days_between" || function == "months_between") return between(node, function == "days_between");
		if (function == "add_days" || function == "add_months" || function == "add_years") return moved(node, function);
		if (function == "start" || function == "end") return bound(node, function == "start");
		if (function == "within" || function == "between") return make_boolean(inside(node));
		if (function == "before" || function == "after" || function == "same_day" || function == "overlaps") return make_boolean(related(node, function));
		if (function == "same_day_of_year") {
			arity(node, 2, 2);
			const Value a = eval(*node.children[0]);
			const Value b = eval(*node.children[1]);
			return make_boolean(settled(same_day_of_year(a, b, language_, name(*node.children[0]), name(*node.children[1]))));
		}
		if (function == "next_occurrence") {
			arity(node, 2, 2);
			const Value day = eval(*node.children[0]);
			const Value from = eval(*node.children[1]);
			return next_occurrence(day, from, language_, name(*node.children[0]), name(*node.children[1]));
		}
		throw Failure{"unknown_function", say("\"" + function + "\" is not a function Singularity knows", "«" + function + "» - не функция, известная Singularity")};
	}

	Value total(const Node &node, bool sum) {
		arity(node, 1, static_cast<std::size_t>(-1));
		Value result = eval(*node.children[0]);
		no_day_of_year(result, *node.children[0]);
		if (result.kind == Kind::Date || result.kind == Kind::Boolean)
			type_error(name(*node.children[0]) + " cannot be summed", name(*node.children[0]) + " нельзя суммировать");
		for (std::size_t index = 1; index < node.children.size(); ++index) {
			const Value next = eval(*node.children[index]);
			no_day_of_year(next, *node.children[index]);
			if (next.kind == Kind::Date) type_error(name(*node.children[index]) + " cannot be summed", name(*node.children[index]) + " нельзя суммировать");
			result = add(result, next, *node.children[0], *node.children[index], false);
		}
		if (sum) return result;
		const Rational count(static_cast<long long>(node.children.size()));
		result.amount = result.amount / count;
		return result;
	}

	Value absolute(const Node &node) {
		arity(node, 1, 1);
		Value value = eval(*node.children[0]);
		no_day_of_year(value, *node.children[0]);
		if (value.kind == Kind::Date || value.kind == Kind::Boolean)
			type_error(name(*node.children[0]) + " has no absolute value", name(*node.children[0]) + " не имеет модуля");
		value.amount = value.amount.abs();
		return value;
	}

	Value clamped(const Node &node) {
		arity(node, 3, 3);
		const Value value = eval(*node.children[0]);
		const Value low = eval(*node.children[1]);
		const Value high = eval(*node.children[2]);
		if (less(high, low, *node.children[2], *node.children[1]))
			type_error("in " + name(node) + " the upper bound is below the lower one", "в " + name(node) + " верхняя граница ниже нижней");
		if (less(value, low, *node.children[0], *node.children[1])) return low;
		if (less(high, value, *node.children[2], *node.children[0])) return high;
		return value;
	}

	Value between(const Node &node, bool days) {
		arity(node, 2, 2);
		const Value from = date_argument(*node.children[0]);
		const Value to = date_argument(*node.children[1]);
		const long long count = days_between(from, to, *node.children[0], *node.children[1], !days);
		if (days) return make_duration(Rational(count), DurationUnit::Day);
		return make_duration(Rational(whole_months(from.date.first_day, to.date.first_day)), DurationUnit::Month);
	}

	Value moved(const Node &node, const std::string &function) {
		arity(node, 2, 2);
		const Value date = date_argument(*node.children[0]);
		Value count = eval(*node.children[1]);
		const DurationUnit unit = function == "add_days" ? DurationUnit::Day : function == "add_months" ? DurationUnit::Month : DurationUnit::Year;
		const bool bare = count.kind == Kind::Number;
		if (bare) count = make_duration(count.amount, unit);
		if (count.kind != Kind::Duration)
			type_error(name(*node.children[1]) + " is not a number of " + unit_words(unit, language_),
				name(*node.children[1]) + " - не число: " + unit_words(unit, language_));
		if (count.unit == DurationUnit::BusinessDay) business_days(*node.children[1]);
		if ((unit == DurationUnit::Day) != day_class(count.unit))
			type_error(name(*node.children[1]) + " is not counted in " + unit_words(unit, language_),
				name(*node.children[1]) + " считается не в единицах «" + unit_words(unit, language_) + "»");
		return shifted(date, count, *node.children[0], *node.children[1], false, bare);
	}

	Value bound(const Node &node, bool start) {
		arity(node, 1, 1);
		Value date = date_argument(*node.children[0]);
		date.date = day_value(start ? date.date.first_day : date.date.last_day);
		return date;
	}

	bool inside(const Node &node) {
		arity(node, 2, 3);
		const Value date = date_argument(*node.children[0]);
		const Value from = date_argument(*node.children[1]);
		if (node.children.size() == 2) return settled(within(date, from, language_, name(*node.children[0]), name(*node.children[1])));
		const Value to = date_argument(*node.children[2]);
		if (from.date.first_day > to.date.last_day)
			type_error("the period from " + name(*node.children[1]) + " to " + name(*node.children[2]) + " ends before it starts",
				"период с " + name(*node.children[1]) + " по " + name(*node.children[2]) + " заканчивается раньше, чем начинается");
		const Value period = make_date({from.date.first_day, to.date.last_day, temporal::Precision::Day});
		return settled(within(date, period, language_, name(*node.children[0]),
			say("the period from " + name(*node.children[1]) + " to " + name(*node.children[2]),
				"период с " + name(*node.children[1]) + " по " + name(*node.children[2]))));
	}

	bool related(const Node &node, const std::string &function) {
		arity(node, 2, 2);
		const Value a = date_argument(*node.children[0]);
		const Value b = date_argument(*node.children[1]);
		if (function == "overlaps") return settled(overlaps(a, b));
		const Comparator comparator = function == "before" ? Comparator::Less : function == "after" ? Comparator::Greater : Comparator::Equal;
		return settled(compare(a, comparator, b, language_, name(*node.children[0]), name(*node.children[1])));
	}

	static long long whole_months(long long from, long long to) {
		if (to < from) return -whole_months(to, from);
		const Civil a = civil_from_days(from);
		const Civil b = civil_from_days(to);
		long long months = (b.year - a.year) * 12 + (static_cast<long long>(b.month) - static_cast<long long>(a.month));
		// The anniversary day in the last month, clamped to its end.
		const unsigned anniversary = std::min(a.day, month_length(b.year, b.month));
		if (b.day < anniversary) --months;
		return months;
	}
};

} // namespace

Evaluation evaluate(const std::string &expression, Language language) {
	Evaluation evaluation;
	try {
		if (expression.size() > 20000) throw Failure{"syntax", say(language, "the expression is too long", "выражение слишком длинное")};
		Parser parser(expression, language);
		const NodePtr root = parser.parse();
		Evaluator evaluator(expression, language);
		evaluation.value = evaluator.run(*root);
	} catch (const Failure &failure) {
		evaluation.error = EvaluationError{failure.kind, failure.reason};
	} catch (const AlgebraError &error) {
		evaluation.error = EvaluationError{error.kind(), error.what()};
	}
	return evaluation;
}

} // namespace singularity::protected_api::papinian::semantic

// ---------------------------------------------------------------------------
// BigInt and ExactRational (DESIGN4 §3.2): exact numbers of any size, so that
// nothing the protected computation protocol works out can overflow - whether
// an operation overflows would otherwise depend on a hidden value. Base 2^32
// limbs, schoolbook multiplication, Knuth's division, binary GCD; no library.

namespace singularity::protected_api::papinian::semantic {

namespace {

using Limbs = std::vector<std::uint32_t>;
constexpr std::uint64_t kLimbBase = std::uint64_t(1) << 32;

void trim(Limbs &limbs) {
	while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
}

int compare_magnitude(const Limbs &a, const Limbs &b) {
	if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
	for (std::size_t index = a.size(); index-- > 0;)
		if (a[index] != b[index]) return a[index] < b[index] ? -1 : 1;
	return 0;
}

Limbs add_magnitude(const Limbs &a, const Limbs &b) {
	const Limbs &longer = a.size() >= b.size() ? a : b;
	const Limbs &shorter = a.size() >= b.size() ? b : a;
	Limbs sum(longer.size() + 1, 0);
	std::uint64_t carry = 0;
	for (std::size_t index = 0; index < longer.size(); ++index) {
		carry += static_cast<std::uint64_t>(longer[index]) + (index < shorter.size() ? shorter[index] : 0U);
		sum[index] = static_cast<std::uint32_t>(carry);
		carry >>= 32;
	}
	sum[longer.size()] = static_cast<std::uint32_t>(carry);
	trim(sum);
	return sum;
}

// a - b, for a no smaller than b.
Limbs subtract_magnitude(const Limbs &a, const Limbs &b) {
	Limbs difference(a.size(), 0);
	std::uint32_t borrow = 0;
	for (std::size_t index = 0; index < a.size(); ++index) {
		const std::uint64_t take = static_cast<std::uint64_t>(index < b.size() ? b[index] : 0U) + borrow;
		borrow = static_cast<std::uint64_t>(a[index]) < take ? 1U : 0U;
		difference[index] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(a[index]) + (borrow ? kLimbBase : 0) - take);
	}
	trim(difference);
	return difference;
}

Limbs multiply_magnitude(const Limbs &a, const Limbs &b) {
	if (a.empty() || b.empty()) return {};
	Limbs product(a.size() + b.size(), 0);
	for (std::size_t i = 0; i < a.size(); ++i) {
		std::uint64_t carry = 0;
		for (std::size_t j = 0; j < b.size(); ++j) {
			const std::uint64_t step = static_cast<std::uint64_t>(a[i]) * b[j] + product[i + j] + carry;
			product[i + j] = static_cast<std::uint32_t>(step);
			carry = step >> 32;
		}
		product[i + b.size()] = static_cast<std::uint32_t>(carry);
	}
	trim(product);
	return product;
}

// The magnitude times `factor` plus `addend`, in place.
void multiply_add_small(Limbs &limbs, std::uint32_t factor, std::uint32_t addend) {
	std::uint64_t carry = addend;
	for (std::uint32_t &limb : limbs) {
		const std::uint64_t step = static_cast<std::uint64_t>(limb) * factor + carry;
		limb = static_cast<std::uint32_t>(step);
		carry = step >> 32;
	}
	if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
}

// Divides in place by a single limb; returns the remainder.
std::uint32_t divide_small(Limbs &limbs, std::uint32_t divisor) {
	std::uint64_t rest = 0;
	for (std::size_t index = limbs.size(); index-- > 0;) {
		const std::uint64_t current = (rest << 32) | limbs[index];
		limbs[index] = static_cast<std::uint32_t>(current / divisor);
		rest = current % divisor;
	}
	trim(limbs);
	return static_cast<std::uint32_t>(rest);
}

Limbs shift_left(const Limbs &limbs, std::size_t bits) {
	if (limbs.empty()) return {};
	const std::size_t whole = bits / 32;
	const unsigned part = static_cast<unsigned>(bits % 32);
	Limbs shifted(limbs.size() + whole + 1, 0);
	for (std::size_t index = 0; index < limbs.size(); ++index) {
		shifted[index + whole] |= part == 0 ? limbs[index] : static_cast<std::uint32_t>(limbs[index] << part);
		if (part != 0) shifted[index + whole + 1] |= limbs[index] >> (32 - part);
	}
	trim(shifted);
	return shifted;
}

Limbs shift_right(const Limbs &limbs, std::size_t bits) {
	const std::size_t whole = bits / 32;
	if (whole >= limbs.size()) return {};
	const unsigned part = static_cast<unsigned>(bits % 32);
	Limbs shifted(limbs.size() - whole, 0);
	for (std::size_t index = 0; index < shifted.size(); ++index) {
		shifted[index] = limbs[index + whole] >> part;
		if (part != 0 && index + whole + 1 < limbs.size())
			shifted[index] |= static_cast<std::uint32_t>(limbs[index + whole + 1] << (32 - part));
	}
	trim(shifted);
	return shifted;
}

std::size_t trailing_zero_bits(const Limbs &limbs) {
	std::size_t bits = 0;
	for (const std::uint32_t limb : limbs) {
		if (limb != 0) return bits + static_cast<std::size_t>(singularity::platform::count_trailing_zeros(limb));
		bits += 32;
	}
	return bits;
}

// Knuth's Algorithm D (TAOCP 4.3.1), for a non-zero divisor.
void divide_magnitude(const Limbs &numerator, const Limbs &divisor, Limbs &quotient, Limbs &remainder) {
	if (compare_magnitude(numerator, divisor) < 0) {
		quotient.clear();
		remainder = numerator;
		return;
	}
	if (divisor.size() == 1) {
		quotient = numerator;
		const std::uint32_t rest = divide_small(quotient, divisor[0]);
		remainder.clear();
		if (rest != 0) remainder.push_back(rest);
		return;
	}
	// Normalise: the divisor's top limb gets its top bit set.
	const unsigned shift = static_cast<unsigned>(singularity::platform::count_leading_zeros(divisor.back()));
	const std::size_t n = divisor.size();
	const std::size_t m = numerator.size() - n;
	Limbs v(n, 0);
	Limbs u(numerator.size() + 1, 0);
	for (std::size_t index = 0; index < n; ++index) {
		v[index] = shift == 0 ? divisor[index] : static_cast<std::uint32_t>(divisor[index] << shift);
		if (shift != 0 && index > 0) v[index] |= divisor[index - 1] >> (32 - shift);
	}
	for (std::size_t index = 0; index < numerator.size(); ++index) {
		u[index] |= shift == 0 ? numerator[index] : static_cast<std::uint32_t>(numerator[index] << shift);
		if (shift != 0) u[index + 1] = numerator[index] >> (32 - shift);
	}
	quotient.assign(m + 1, 0);
	for (std::size_t j = m + 1; j-- > 0;) {
		const std::uint64_t top = (static_cast<std::uint64_t>(u[j + n]) << 32) | u[j + n - 1];
		std::uint64_t estimate = top / v[n - 1];
		std::uint64_t rest = top % v[n - 1];
		while (estimate >= kLimbBase || estimate * v[n - 2] > ((rest << 32) | u[j + n - 2])) {
			--estimate;
			rest += v[n - 1];
			if (rest >= kLimbBase) break;
		}
		// Multiply and subtract.
		std::uint64_t carry = 0;
		std::uint32_t borrow = 0;
		for (std::size_t index = 0; index < n; ++index) {
			const std::uint64_t product = estimate * v[index] + carry;
			carry = product >> 32;
			const std::uint64_t take = (product & 0xFFFFFFFFULL) + borrow;
			borrow = static_cast<std::uint64_t>(u[index + j]) < take ? 1U : 0U;
			u[index + j] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(u[index + j]) + (borrow ? kLimbBase : 0) - take);
		}
		const std::uint64_t take = carry + borrow;
		const bool negative = static_cast<std::uint64_t>(u[j + n]) < take;
		u[j + n] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(u[j + n]) - take);
		if (negative) {
			// The estimate was one too large: add the divisor back.
			--estimate;
			std::uint64_t sum = 0;
			for (std::size_t index = 0; index < n; ++index) {
				sum += static_cast<std::uint64_t>(u[index + j]) + v[index];
				u[index + j] = static_cast<std::uint32_t>(sum);
				sum >>= 32;
			}
			u[j + n] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(u[j + n]) + sum);
		}
		quotient[j] = static_cast<std::uint32_t>(estimate);
	}
	trim(quotient);
	u.resize(n);
	trim(u);
	remainder = shift_right(u, shift);
}

Limbs magnitude_of(UInt128 value) {
	Limbs limbs;
	while (value != 0) {
		limbs.push_back(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
		value >>= 32;
	}
	return limbs;
}

// 10^places as a BigInt.
BigInt power_of_ten(int places) {
	if (places < 0 || places > 100000) throw AlgebraError("overflow", "the numbers are too large to work out exactly");
	BigInt result(1);
	const BigInt billion(1000000000LL);
	for (; places >= 9; places -= 9) result = result * billion;
	long long rest = 1;
	for (int index = 0; index < places; ++index) rest *= 10;
	return result * BigInt(rest);
}

} // namespace

BigInt::BigInt(long long value) {
	if (value == 0) return;
	negative_ = value < 0;
	unsigned long long magnitude = negative_ ? 0ULL - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
	while (magnitude != 0) {
		limbs_.push_back(static_cast<std::uint32_t>(magnitude & 0xFFFFFFFFULL));
		magnitude >>= 32;
	}
}

BigInt BigInt::from_int128(Int128 value) {
	BigInt result;
	result.negative_ = value < 0;
	const UInt128 magnitude = value < 0 ? static_cast<UInt128>(0) - static_cast<UInt128>(value)
		: static_cast<UInt128>(value);
	result.limbs_ = magnitude_of(magnitude);
	if (result.limbs_.empty()) result.negative_ = false;
	return result;
}

std::optional<BigInt> BigInt::parse_decimal(std::string_view text) {
	std::size_t at = 0;
	bool negative = false;
	if (at < text.size() && (text[at] == '-' || text[at] == '+')) {
		negative = text[at] == '-';
		++at;
	}
	if (at >= text.size() || text.size() - at > 100000) return std::nullopt;
	BigInt result;
	std::uint32_t chunk = 0;
	std::uint32_t scale = 1;
	for (; at < text.size(); ++at) {
		const char character = text[at];
		if (character < '0' || character > '9') return std::nullopt;
		chunk = chunk * 10 + static_cast<std::uint32_t>(character - '0');
		scale *= 10;
		if (scale == 1000000000U) {
			multiply_add_small(result.limbs_, scale, chunk);
			trim(result.limbs_);
			chunk = 0;
			scale = 1;
		}
	}
	if (scale != 1) multiply_add_small(result.limbs_, scale, chunk);
	trim(result.limbs_);
	result.negative_ = negative && !result.limbs_.empty();
	return result;
}

std::size_t BigInt::bit_length() const {
	if (limbs_.empty()) return 0;
	return 32 * (limbs_.size() - 1) + (32 - static_cast<std::size_t>(singularity::platform::count_leading_zeros(limbs_.back())));
}

std::optional<Int128> BigInt::to_int128() const {
	if (limbs_.size() > 4) return std::nullopt;
	UInt128 magnitude = 0;
	for (std::size_t index = limbs_.size(); index-- > 0;) magnitude = (magnitude << 32) | limbs_[index];
	const UInt128 limit = static_cast<UInt128>(1) << 127;
	if (negative_) {
		if (magnitude > limit) return std::nullopt;
		return static_cast<Int128>(static_cast<UInt128>(0) - magnitude);
	}
	if (magnitude >= limit) return std::nullopt;
	return static_cast<Int128>(magnitude);
}

std::string BigInt::to_string() const {
	if (limbs_.empty()) return "0";
	Limbs rest = limbs_;
	std::vector<std::uint32_t> chunks;
	while (!rest.empty()) chunks.push_back(divide_small(rest, 1000000000U));
	std::string text = negative_ ? "-" : "";
	text += std::to_string(chunks.back());
	for (std::size_t index = chunks.size() - 1; index-- > 0;) {
		const std::string part = std::to_string(chunks[index]);
		text.append(9 - part.size(), '0');
		text += part;
	}
	return text;
}

BigInt BigInt::abs() const {
	BigInt result = *this;
	result.negative_ = false;
	return result;
}

BigInt BigInt::operator-() const {
	BigInt result = *this;
	result.negative_ = !negative_ && !limbs_.empty();
	return result;
}

BigInt operator+(const BigInt &a, const BigInt &b) {
	BigInt result;
	if (a.negative_ == b.negative_) {
		result.limbs_ = add_magnitude(a.limbs_, b.limbs_);
		result.negative_ = a.negative_;
	} else if (compare_magnitude(a.limbs_, b.limbs_) >= 0) {
		result.limbs_ = subtract_magnitude(a.limbs_, b.limbs_);
		result.negative_ = a.negative_;
	} else {
		result.limbs_ = subtract_magnitude(b.limbs_, a.limbs_);
		result.negative_ = b.negative_;
	}
	if (result.limbs_.empty()) result.negative_ = false;
	return result;
}

BigInt operator-(const BigInt &a, const BigInt &b) { return a + (-b); }

BigInt operator*(const BigInt &a, const BigInt &b) {
	BigInt result;
	result.limbs_ = multiply_magnitude(a.limbs_, b.limbs_);
	result.negative_ = !result.limbs_.empty() && a.negative_ != b.negative_;
	return result;
}

void BigInt::divide(const BigInt &numerator, const BigInt &denominator, BigInt &quotient, BigInt &remainder) {
	if (denominator.limbs_.empty()) throw AlgebraError("division_by_zero", "division by zero");
	Limbs q;
	Limbs r;
	divide_magnitude(numerator.limbs_, denominator.limbs_, q, r);
	quotient.limbs_ = std::move(q);
	quotient.negative_ = !quotient.limbs_.empty() && numerator.negative_ != denominator.negative_;
	remainder.limbs_ = std::move(r);
	remainder.negative_ = !remainder.limbs_.empty() && numerator.negative_;
}

BigInt operator/(const BigInt &a, const BigInt &b) {
	BigInt quotient;
	BigInt remainder;
	BigInt::divide(a, b, quotient, remainder);
	return quotient;
}

BigInt operator%(const BigInt &a, const BigInt &b) {
	BigInt quotient;
	BigInt remainder;
	BigInt::divide(a, b, quotient, remainder);
	return remainder;
}

BigInt BigInt::gcd(const BigInt &a, const BigInt &b) {
	BigInt result;
	if (a.limbs_.empty()) {
		result.limbs_ = b.limbs_;
		return result;
	}
	if (b.limbs_.empty()) {
		result.limbs_ = a.limbs_;
		return result;
	}
	Limbs x = a.limbs_;
	Limbs y = b.limbs_;
	const std::size_t zerosX = trailing_zero_bits(x);
	const std::size_t zerosY = trailing_zero_bits(y);
	const std::size_t common = std::min(zerosX, zerosY);
	x = shift_right(x, zerosX);
	for (;;) {
		y = shift_right(y, trailing_zero_bits(y));
		if (compare_magnitude(x, y) > 0) std::swap(x, y);
		y = subtract_magnitude(y, x);
		if (y.empty()) break;
	}
	result.limbs_ = shift_left(x, common);
	return result;
}

int BigInt::compare(const BigInt &a, const BigInt &b) {
	if (a.negative_ != b.negative_) return a.negative_ ? -1 : 1;
	const int magnitude = compare_magnitude(a.limbs_, b.limbs_);
	return a.negative_ ? -magnitude : magnitude;
}

// ---------------------------------------------------------------------------
// ExactRational

namespace {

// |value| * 10^places, rounded to a whole number under `rounding`, with the
// value's sign: what a decimal of `places` places writes, without its point.
BigInt scaled_whole(const ExactRational &value, int places, ExactRational::Rounding rounding) {
	const BigInt scaled = value.numerator().abs() * power_of_ten(places);
	BigInt whole;
	BigInt rest;
	BigInt::divide(scaled, value.denominator(), whole, rest);
	bool up = false;
	if (!rest.is_zero()) {
		const int half = BigInt::compare(rest + rest, value.denominator());
		switch (rounding) {
			case ExactRational::Rounding::Down: up = false; break;
			case ExactRational::Rounding::Up: up = true; break;
			case ExactRational::Rounding::HalfUp: up = half >= 0; break;
			case ExactRational::Rounding::HalfEven: up = half > 0 || (half == 0 && !(whole % BigInt(2)).is_zero()); break;
		}
	}
	if (up) whole = whole + BigInt(1);
	return value.sign() < 0 ? -whole : whole;
}

} // namespace

ExactRational::ExactRational(const BigInt &integer) : numerator_(integer) {}

ExactRational::ExactRational(const BigInt &numerator, const BigInt &denominator) {
	if (denominator.is_zero()) throw AlgebraError("division_by_zero", "division by zero");
	BigInt top = denominator.sign() < 0 ? -numerator : numerator;
	BigInt bottom = denominator.abs();
	if (top.is_zero()) {
		numerator_ = BigInt();
		denominator_ = BigInt(1);
		return;
	}
	const BigInt common = BigInt::gcd(top, bottom);
	if (common != BigInt(1)) {
		top = top / common;
		bottom = bottom / common;
	}
	numerator_ = std::move(top);
	denominator_ = std::move(bottom);
}

ExactRational ExactRational::from(const Rational &value) {
	return ExactRational(BigInt::from_int128(value.numerator()), BigInt::from_int128(value.denominator()));
}

std::optional<ExactRational> ExactRational::parse_decimal(std::string_view text) {
	if (text.empty() || text.size() > 100000) return std::nullopt;
	std::string digits;
	std::size_t at = 0;
	if (text[at] == '-' || text[at] == '+') digits.push_back(text[at++]);
	int decimals = 0;
	bool point = false;
	bool any = false;
	for (; at < text.size(); ++at) {
		const char character = text[at];
		if (character == '.') {
			if (point) return std::nullopt;
			point = true;
			continue;
		}
		if (character < '0' || character > '9') return std::nullopt;
		digits.push_back(character);
		any = true;
		if (point) ++decimals;
	}
	if (!any) return std::nullopt;
	const auto whole = BigInt::parse_decimal(digits);
	if (!whole) return std::nullopt;
	return ExactRational(*whole, power_of_ten(decimals));
}

bool ExactRational::is_integer() const { return denominator_ == BigInt(1); }

std::size_t ExactRational::bits() const { return numerator_.bit_length() + denominator_.bit_length(); }

ExactRational ExactRational::abs() const {
	ExactRational result = *this;
	result.numerator_ = numerator_.abs();
	return result;
}

ExactRational ExactRational::rounded(int places, Rounding rounding) const {
	return ExactRational(scaled_whole(*this, places, rounding), power_of_ten(places));
}

std::optional<Rational> ExactRational::to_rational() const {
	const auto top = numerator_.to_int128();
	const auto bottom = denominator_.to_int128();
	if (!top || !bottom) return std::nullopt;
	return Rational(*top, *bottom);
}

std::optional<std::string> ExactRational::exact_decimal(int max_places) const {
	// Only a denominator of twos and fives ends: 10^p holds it for p the larger count.
	BigInt rest = denominator_;
	int twos = 0;
	int fives = 0;
	const BigInt two(2);
	const BigInt five(5);
	while ((rest % two).is_zero()) {
		rest = rest / two;
		++twos;
	}
	while ((rest % five).is_zero()) {
		rest = rest / five;
		++fives;
	}
	const int places = std::max(twos, fives);
	if (rest != BigInt(1) || places > max_places) return std::nullopt;
	return to_decimal(places);
}

std::string ExactRational::to_decimal(int places, Rounding rounding) const {
	const BigInt whole = scaled_whole(*this, places, rounding);
	std::string digits = whole.abs().to_string();
	if (places > 0) {
		if (digits.size() < static_cast<std::size_t>(places) + 1)
			digits.insert(0, static_cast<std::size_t>(places) + 1 - digits.size(), '0');
		digits.insert(digits.size() - static_cast<std::size_t>(places), 1, '.');
	}
	return whole.sign() < 0 ? "-" + digits : digits;
}

std::string ExactRational::to_string() const {
	if (is_integer()) return numerator_.to_string();
	return numerator_.to_string() + "/" + denominator_.to_string();
}

ExactRational operator+(const ExactRational &a, const ExactRational &b) {
	if (a.denominator_ == b.denominator_) return ExactRational(a.numerator_ + b.numerator_, a.denominator_);
	return ExactRational(a.numerator_ * b.denominator_ + b.numerator_ * a.denominator_, a.denominator_ * b.denominator_);
}

ExactRational operator-(const ExactRational &a, const ExactRational &b) { return a + (-b); }

ExactRational operator*(const ExactRational &a, const ExactRational &b) {
	if (a.is_zero() || b.is_zero()) return ExactRational();
	// Cross-cancel first: the products stay as small as the result.
	const BigInt first = BigInt::gcd(a.numerator_, b.denominator_);
	const BigInt second = BigInt::gcd(b.numerator_, a.denominator_);
	return ExactRational((a.numerator_ / first) * (b.numerator_ / second), (a.denominator_ / second) * (b.denominator_ / first));
}

ExactRational operator/(const ExactRational &a, const ExactRational &b) {
	if (b.is_zero()) throw AlgebraError("division_by_zero", "division by zero");
	return a * ExactRational(b.denominator_, b.numerator_);
}

ExactRational ExactRational::operator-() const {
	ExactRational result = *this;
	result.numerator_ = -numerator_;
	return result;
}

int ExactRational::compare(const ExactRational &a, const ExactRational &b) {
	if (a.sign() != b.sign()) return a.sign() < b.sign() ? -1 : 1;
	if (a.denominator_ == b.denominator_) return BigInt::compare(a.numerator_, b.numerator_);
	return BigInt::compare(a.numerator_ * b.denominator_, b.numerator_ * a.denominator_);
}

} // namespace singularity::protected_api::papinian::semantic

// ---------------------------------------------------------------------------
// Periods of a date (DESIGN4 §7.1): the start and the end of the month,
// quarter or year that holds it - calendar periods, or periods counted from an
// anchor's day (contract, lease and fiscal years).

namespace singularity::protected_api::papinian::semantic {

namespace {

// The period of `months` months (1, 3 or 12) that holds `day`, counted from
// the anchor's month and day: its first day, and the first day of the next.
std::pair<long long, long long> period_around(long long day, unsigned months, unsigned anchorMonth, unsigned anchorDay) {
	const auto start_in = [&](long long monthIndex) {
		const long long year = monthIndex >= 0 ? monthIndex / 12 : (monthIndex - 11) / 12;
		const unsigned month = static_cast<unsigned>(monthIndex - year * 12 + 1);
		return days_from_civil(year, month, std::min(anchorDay, month_length(year, month)));
	};
	const Civil civil = civil_from_days(day);
	const long long index = civil.year * 12 + static_cast<long long>(civil.month) - 1;
	const long long phase = static_cast<long long>(anchorMonth) - 1;
	const long long step = static_cast<long long>(months);
	long long aligned = index - (((index - phase) % step) + step) % step;
	if (start_in(aligned) > day) aligned -= step;
	return {start_in(aligned), start_in(aligned + step)};
}

unsigned months_of(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Month: return 1;
		case DurationUnit::Quarter: return 3;
		case DurationUnit::Year: return 12;
		default: return 0;
	}
}

// The period of `unit` that holds every day of `date`, or why none does.
std::pair<long long, long long> whole_period(const Value &date, DurationUnit unit, const std::optional<Value> &anchor) {
	if (date.kind != Kind::Date) throw AlgebraError("type", "only a date has a period of its own");
	const unsigned months = months_of(unit);
	if (months == 0) throw AlgebraError("type", "a period is a month, a quarter or a year");
	unsigned anchorMonth = 1;
	unsigned anchorDay = 1;
	if (anchor) {
		if (anchor->kind == Kind::DayOfYear) {
			anchorMonth = anchor->day_of_year.month;
			anchorDay = anchor->day_of_year.day;
		} else if (anchor->kind == Kind::Date && anchor->date.precision == temporal::Precision::Day) {
			const Civil civil = civil_from_days(anchor->date.first_day);
			anchorMonth = civil.month;
			anchorDay = civil.day;
		} else if (anchor->kind == Kind::Date) {
			throw AlgebraError("undetermined", "the anchor names no day, so the periods counted from it are not known");
		} else {
			throw AlgebraError("type", "periods are counted from a date");
		}
	}
	const auto first = period_around(date.date.first_day, months, anchorMonth, anchorDay);
	const auto last = period_around(date.date.last_day, months, anchorMonth, anchorDay);
	if (first != last) throw AlgebraError("undetermined", "the date's days fall in more than one such period");
	return first;
}

Value day_like(long long day, const Value &like) {
	return make_date(day_value(day), like.date_style.empty() ? std::string("english") : like.date_style);
}

} // namespace

Value start_of(const Value &date, DurationUnit unit, const std::optional<Value> &anchor) {
	return day_like(whole_period(date, unit, anchor).first, date);
}

Value end_of(const Value &date, DurationUnit unit, const std::optional<Value> &anchor) {
	return day_like(whole_period(date, unit, anchor).second - 1, date);
}

Value days_in(const Value &date) {
	if (date.kind != Kind::Date) throw AlgebraError("type", "only a date holds days");
	return make_duration(Rational(date.date.last_day - date.date.first_day + 1), DurationUnit::Day);
}

} // namespace singularity::protected_api::papinian::semantic

// ---------------------------------------------------------------------------
// FigureRecogniser (DESIGN4 §8.2): every figure a text writes, found the same
// way for the grounding index (what the lawyer's material writes openly) and
// the answer contract's raw-figure check (what a model wrote outside a
// symbol). A figure is read by parse_value() - the one reading of a value Singularity
// has - from the widest span around its digits or number words that reads
// as one value: "$1,275.00", «1 183 452,05 ₽», "ten (10) days", «десяти
// процентов», "0,1 % за каждый день". Numbers that only point somewhere - a
// clause, an article, a row, a cell, a list item, a function code - are found
// and marked structural; a date is one figure, never its digits.

namespace singularity::protected_api::papinian::semantic {

namespace {

// A space inside a figure ("18 400 000", «0,1 %»): its length, 0 when none.
std::size_t figure_space_at(std::string_view text, std::size_t at) {
	if (at >= text.size()) return 0;
	if (text[at] == ' ') return 1;
	if (starts_at(text, at, "\xC2\xA0")) return 2;
	if (starts_at(text, at, "\xE2\x80\xAF") || starts_at(text, at, "\xE2\x80\x89") || starts_at(text, at, "\xE2\x80\x87")) return 3;
	return 0;
}

// The space that ends just before `at`: its length, 0 when none.
std::size_t figure_space_before(std::string_view text, std::size_t at) {
	if (at >= 1 && text[at - 1] == ' ') return 1;
	if (at >= 2 && figure_space_at(text, at - 2) == 2) return 2;
	if (at >= 3 && figure_space_at(text, at - 3) == 3) return 3;
	return 0;
}

// A letter (ASCII or Cyrillic) or '_' ends just before `at`.
bool letter_before(std::string_view text, std::size_t at) {
	if (at == 0) return false;
	const char previous = text[at - 1];
	if (is_ascii_letter(previous) || previous == '_') return true;
	return at >= 2 && cyrillic_at(text, at - 2);
}

bool letter_at(std::string_view text, std::size_t at) {
	return at < text.size() && (is_ascii_letter(text[at]) || text[at] == '_' || cyrillic_at(text, at));
}

// The word at `at` (letters, inner hyphens), or "" when none starts there.
std::size_t word_end(std::string_view text, std::size_t at) {
	std::size_t cursor = at;
	while (cursor < text.size()) {
		if (is_ascii_letter(text[cursor])) {
			++cursor;
		} else if (cyrillic_at(text, cursor)) {
			cursor += 2;
		} else if (text[cursor] == '-' && cursor > at && letter_at(text, cursor + 1) && text[cursor + 1] != '_') {
			++cursor;
		} else {
			break;
		}
	}
	return cursor;
}

// The word that ends just before `at`, lowered, and where it starts.
std::string word_ending_at(std::string_view text, std::size_t at, std::size_t &start) {
	std::size_t cursor = at;
	while (cursor > 0) {
		if (is_ascii_letter(text[cursor - 1])) {
			--cursor;
		} else if (cursor >= 2 && cyrillic_at(text, cursor - 2)) {
			cursor -= 2;
		} else {
			break;
		}
	}
	start = cursor;
	return lowered(text.substr(cursor, at - cursor));
}

// Words that may follow a figure and belong to it: its currency, unit, scale,
// percent, the words it is spelled in, and a rate's period.
bool figure_tail_word(const std::string &raw) {
	const std::string word = without_dot(raw);
	if (word.empty()) return false;
	if (number_word(word) || scale_word(word) || !currency_of_word(word).empty() || !subunit_of_word(word).empty()
		|| percent_word(word) || base_unit_word(word) || day_modifier(word) != 0)
		return true;
	static const char *others[] = {"per", "cent", "and", "и", "сша", "sterling", "стерлингов", "us", "united", "states",
		"annum", "diem", "annual", "annually", "daily", "yearly", "monthly", "годовых", "годовые", "ежедневно",
		"ежегодно", "ежемесячно", "в", "за", "каждый", "календарный", "a", "each", "every", "for", "p.a", "день",
		"сутки", "год", "месяц", "day", "year", "month"};
	for (const char *other : others)
		if (word == other) return true;
	return false;
}

// Abbreviations whose full stop belongs to them ("руб.", "тыс."), not to the sentence.
bool keeps_full_stop(const std::string &word) {
	static const char *abbreviations[] = {"руб", "коп", "тыс", "млн", "млрд", "трлн", "долл", "дн", "мин", "p.a", "pct", "mln", "hrs", "mins"};
	for (const char *abbreviation : abbreviations)
		if (word == abbreviation) return true;
	return false;
}

const char *const kCurrencySigns[] = {"$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"};

// A currency sign ending just before `at`: its length, 0 when none.
std::size_t currency_sign_before(std::string_view text, std::size_t at) {
	for (const char *sign : kCurrencySigns) {
		const std::size_t length = std::string_view(sign).size();
		if (at >= length && text.compare(at - length, length, sign) == 0) return length;
	}
	return 0;
}

std::size_t currency_sign_at(std::string_view text, std::size_t at) {
	for (const char *sign : kCurrencySigns)
		if (starts_at(text, at, sign)) return std::string_view(sign).size();
	return 0;
}

// Words before a number that make it a pointer into a document, not a figure
// of the matter: a clause, a section, an article, a row, a list item.
bool structural_word(const std::string &word) {
	static const char *exact[] = {"section", "sections", "sec", "clause", "clauses", "subclause", "subsection", "article",
		"articles", "art", "paragraph", "paragraphs", "para", "item", "items", "schedule", "annex", "appendix", "exhibit",
		"chapter", "part", "rule", "row", "rows", "line", "lines", "page", "pages", "p", "pp", "doc", "п", "пп", "ст",
		"ч", "гл", "стр", "абз", "подп", "разд", "прил"};
	for (const char *each : exact)
		if (word == each) return true;
	static const char *stems[] = {"пункт", "подпункт", "стать", "част", "раздел", "глав", "приложени", "строк", "абзац"};
	for (const char *stem : stems)
		if (starts_with(word, stem)) return true;
	return false;
}

// Words between two numbers that make the second one of the same range or
// list: "rows 4-18", "sections 4 and 5", «пп. 3, 4».
bool range_joiner(std::string_view between) {
	const std::string words = lowered(normalized(between));
	if (words.empty()) return false;
	static const char *joiners[] = {"-", ",", "and", "to", "through", "or", "&", "и", "по", "или", ", and"};
	for (const char *joiner : joiners)
		if (words == joiner) return true;
	return false;
}

// A pointer word that names more than one pointer - "sections", "rows",
// «пп.», «статьи» - so that the numbers joined after it are pointers too.
// "Section 4.2, 1,250,000 remains" names one: the number after it is a figure.
bool plural_pointer_word(const std::string &word) {
	static const char *exact[] = {"sections", "secs", "clauses", "subclauses", "subsections", "articles", "arts", "paragraphs",
		"paras", "items", "schedules", "annexes", "appendices", "exhibits", "chapters", "parts", "rules", "rows", "lines", "pages",
		"pp", "\xD0\xBF\xD0\xBF" /* пп */};
	for (const char *each : exact)
		if (word == each) return true;
	// Russian plural forms (nominative, genitive, prepositional, dative, instrumental).
	static const char *plural[] = {"\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82\xD1\x8B" /* пункты */,
		"\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82\xD0\xBE\xD0\xB2" /* пунктов */,
		"\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82\xD0\xB0\xD1\x85" /* пунктах */,
		"\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82\xD0\xB0\xD0\xBC" /* пунктам */,
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82\xD1\x8B" /* подпункты */,
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82\xD0\xBE\xD0\xB2" /* подпунктов */,
		"\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C\xD0\xB8" /* статьи */, "\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB9" /* статей */,
		"\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C\xD1\x8F\xD1\x85" /* статьях */,
		"\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C\xD1\x8F\xD0\xBC" /* статьям */,
		"\xD1\x87\xD0\xB0\xD1\x81\xD1\x82\xD0\xB8" /* части */, "\xD1\x87\xD0\xB0\xD1\x81\xD1\x82\xD0\xB5\xD0\xB9" /* частей */,
		"\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8B" /* разделы */,
		"\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2" /* разделов */,
		"\xD0\xB3\xD0\xBB\xD0\xB0\xD0\xB2\xD1\x8B" /* главы */, "\xD0\xB3\xD0\xBB\xD0\xB0\xD0\xB2" /* глав */,
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F" /* приложения */,
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB9" /* приложений */,
		"\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB8" /* строки */, "\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA" /* строк */,
		"\xD0\xB0\xD0\xB1\xD0\xB7\xD0\xB0\xD1\x86\xD1\x8B" /* абзацы */,
		"\xD0\xB0\xD0\xB1\xD0\xB7\xD0\xB0\xD1\x86\xD0\xB5\xD0\xB2" /* абзацев */};
	for (const char *each : plural)
		if (word == each) return true;
	return false;
}

// A number shaped like a pointer - "18", "4.2", "12.3.1" - not like an amount:
// no digit grouping, at most five digits before the first dot, at most three
// in each part after it. "1,250,000" and "250000" are never pointers.
bool pointer_shaped(std::string_view number) {
	std::size_t part = 0;
	bool first = true;
	for (const char character : number) {
		if (character == '.') {
			if (part == 0) return false;
			part = 0;
			first = false;
			continue;
		}
		if (character < '0' || character > '9') return false;
		++part;
		if (part > (first ? 5u : 3u)) return false;
	}
	return part > 0;
}

struct Core {
	std::size_t start = 0;
	std::size_t end = 0;
	bool digits = false;
};

// Digits and their grouping and decimal marks: "18 400 000", "1,275.00", «1 183 452,05».
std::size_t digits_end(std::string_view text, std::size_t at) {
	std::size_t cursor = at;
	while (cursor < text.size() && is_digit(text[cursor])) ++cursor;
	bool marked = false;
	for (;;) {
		const std::size_t space = figure_space_at(text, cursor);
		if (!marked && space > 0 && cursor + space + 3 <= text.size() && is_digit(text[cursor + space])
			&& is_digit(text[cursor + space + 1]) && is_digit(text[cursor + space + 2])
			&& (cursor + space + 3 >= text.size() || !is_digit(text[cursor + space + 3]))) {
			cursor += space + 3;
			continue;
		}
		if (cursor + 1 < text.size() && (text[cursor] == ',' || text[cursor] == '.') && is_digit(text[cursor + 1])) {
			marked = true;
			++cursor;
			while (cursor < text.size() && is_digit(text[cursor])) ++cursor;
			continue;
		}
		break;
	}
	return cursor;
}

// A run of number words from `at`: "one million two hundred fifty thousand",
// «восемнадцать миллионов четыреста тысяч». Its end, or `at` when none.
std::size_t number_words_end(std::string_view text, std::size_t at) {
	std::size_t cursor = at;
	std::size_t last = at;
	for (;;) {
		const std::size_t end = word_end(text, cursor);
		if (end == cursor) break;
		const std::string word = lowered(text.substr(cursor, end - cursor));
		const bool joins = (word == "and" || word == "и") && last != at;
		if (!number_word(word) && !joins) break;
		if (!joins) last = end;
		const std::size_t space = figure_space_at(text, end);
		if (space == 0) break;
		cursor = end + space;
		if (joins && word_end(text, cursor) == cursor) break;
	}
	return last;
}

} // namespace

std::vector<RecognisedFigure> FigureRecogniser::find(std::string_view text, Language language) {
	std::vector<RecognisedFigure> figures;
	if (text.empty() || text.size() > 16 * 1024 * 1024) return figures;
	// Symbols and local anchors carry no figure: "[[Amount3]]", "⟪…⟫".
	std::vector<std::pair<std::size_t, std::size_t>> skipped;
	for (std::size_t at = text.find("[["); at != std::string_view::npos; at = text.find("[[", at + 2)) {
		const std::size_t close = text.find("]]", at + 2);
		if (close == std::string_view::npos || close - at > 80) continue;
		skipped.emplace_back(at, close + 2);
	}
	for (std::size_t at = text.find("\xE2\x9F\xAA"); at != std::string_view::npos; at = text.find("\xE2\x9F\xAA", at + 3)) {
		const std::size_t close = text.find("\xE2\x9F\xAB", at + 3);
		if (close == std::string_view::npos) break;
		skipped.emplace_back(at, close + 3);
	}
	const std::string owned(text);
	// Dates, whole: "March 3, 2026", «15.03.2026», "Q3 2026", "July 1". A bare
	// year with a currency or a unit after it is an amount, not a year. Every
	// date writes a figure: only the lines that hold one are read for dates,
	// each in the slash order the whole text shows.
	std::vector<temporal::FoundDate> dates;
	std::vector<temporal::FoundDayOfYear> daysOfYear;
	{
		const temporal::SlashOrder order = temporal::slash_order_of(owned);
		for (std::size_t line = 0; line < owned.size();) {
			const std::size_t end = std::min(owned.find('\n', line), owned.size());
			if (std::any_of(owned.begin() + static_cast<long>(line), owned.begin() + static_cast<long>(end), is_digit)) {
				const std::string part = owned.substr(line, end - line);
				for (temporal::FoundDate date : temporal::find(part, order)) {
					date.offset += line;
					dates.push_back(date);
				}
				for (temporal::FoundDayOfYear day : temporal::find_days_of_year(part)) {
					day.offset += line;
					daysOfYear.push_back(day);
				}
			}
			line = end + 1;
		}
	}
	for (const temporal::FoundDate &date : dates) {
		const std::size_t after = date.offset + date.length;
		const std::size_t space = figure_space_at(text, after);
		const std::size_t next = after + space;
		const std::string following = lowered(text.substr(next, word_end(text, next) - next));
		const bool bareYear = date.value.precision == temporal::Precision::Year && date.length == 4;
		if (bareYear && ((!following.empty() && figure_tail_word(following) && following != "a" && following != "for" && following != "в"
				&& following != "за" && following != "and" && following != "и")
				|| currency_sign_at(text, next) > 0 || starts_at(text, after, "%") || currency_sign_before(text, date.offset) > 0))
			continue;
		RecognisedFigure figure;
		figure.start = date.offset;
		figure.length = date.length;
		figure.kind = RecognisedFigure::Kind::Date;
		if (!date.ambiguous) figure.value = make_date(date.value);
		figures.push_back(figure);
		skipped.emplace_back(date.offset, after);
	}
	for (const temporal::FoundDayOfYear &day : daysOfYear) {
		bool inside = false;
		for (const auto &span : skipped) inside = inside || (day.offset < span.second && day.offset + day.length > span.first);
		if (inside) continue;
		RecognisedFigure figure;
		figure.start = day.offset;
		figure.length = day.length;
		figure.kind = RecognisedFigure::Kind::Date;
		figure.value = make_day_of_year(day.value.month, day.value.day);
		figures.push_back(figure);
		skipped.emplace_back(day.offset, day.offset + day.length);
	}
	std::sort(skipped.begin(), skipped.end());
	// The cursor only moves forward: so does the next span to skip.
	std::size_t nextSkip = 0;
	const auto skip_end = [&skipped, &nextSkip](std::size_t at) -> std::size_t {
		while (nextSkip < skipped.size() && skipped[nextSkip].second <= at) ++nextSkip;
		for (std::size_t index = nextSkip; index < skipped.size() && skipped[index].first <= at; ++index)
			if (at < skipped[index].second) return skipped[index].second;
		return 0;
	};

	std::size_t previousEnd = std::string_view::npos;  // the last structural figure's end
	bool previousPlural = false;  // it follows a plural pointer word ("sections 4"), or continues one's list
	std::size_t cursor = 0;
	while (cursor < text.size()) {
		if (const std::size_t past = skip_end(cursor); past != 0) {
			cursor = past;
			continue;
		}
		Core core;
		if (is_digit(text[cursor])) {
			core = {cursor, digits_end(text, cursor), true};
		} else if (letter_at(text, cursor) && !letter_before(text, cursor) && !(cursor > 0 && is_digit(text[cursor - 1]))) {
			const std::size_t end = number_words_end(text, cursor);
			if (end == cursor) {
				cursor = std::max(word_end(text, cursor), cursor + 1);
				continue;
			}
			core = {cursor, end, false};
		} else {
			++cursor;
			continue;
		}
		// Never across a symbol or a date.
		{
			const auto following = std::upper_bound(skipped.begin(), skipped.end(), std::make_pair(core.start, std::string_view::npos));
			if (following != skipped.end() && following->first < core.end) core.end = following->first;
		}

		// Wider: a currency sign or code before, the words and marks after.
		std::vector<std::size_t> lefts;
		{
			std::size_t left = core.start;
			const std::size_t space = figure_space_before(text, left);
			const std::size_t sign = currency_sign_before(text, left - space);
			std::size_t widened = core.start;
			if (sign > 0) {
				widened = left - space - sign;
				if (widened >= 2 && (text.compare(widened - 2, 2, "US") == 0 || text.compare(widened - 2, 2, "us") == 0)
						&& !letter_before(text, widened - 2))
					widened -= 2;
			} else if (space > 0) {
				std::size_t wordStart = 0;
				const std::string code = word_ending_at(text, left - space, wordStart);
				if (!currency_of_code(code).empty() && !letter_before(text, wordStart)) widened = wordStart;
			}
			if (core.digits && widened >= 1 && text[widened - 1] == '-' && (widened < 2 || !(is_digit(text[widened - 2]) || letter_at(text, widened - 2))))
				widened -= 1;
			else if (core.digits && widened >= 3 && text.compare(widened - 3, 3, "\xE2\x88\x92") == 0)
				widened -= 3;
			if (widened != core.start) lefts.push_back(widened);
			lefts.push_back(core.start);
		}
		std::vector<std::size_t> rights{core.end};
		{
			std::size_t at = core.end;
			for (int step = 0; step < 14 && at < text.size(); ++step) {
				std::size_t next = at + figure_space_at(text, at);
				if (next >= text.size()) break;
				if (text[next] == '%') {
					at = next + 1;
				} else if (const std::size_t sign = currency_sign_at(text, next); sign > 0) {
					at = next + sign;
				} else if (text[next] == '(') {
					const std::size_t close = text.find(')', next);
					const std::size_t line = text.find('\n', next);
					if (close == std::string_view::npos || close - next > 240 || (line != std::string_view::npos && line < close)
						|| text.substr(next + 1, close - next - 1).find('(') != std::string_view::npos)
						break;
					at = close + 1;
				} else if (is_digit(text[next]) && next == at + figure_space_at(text, at) && next != at) {
					// "рублей 00 копеек": the minor unit in figures.
					std::size_t end = next;
					while (end < text.size() && is_digit(text[end])) ++end;
					if (end - next > 2) break;
					at = end;
				} else if (letter_at(text, next)) {
					std::size_t end = word_end(text, next);
					std::string word = lowered(text.substr(next, end - next));
					if (word == "p" && starts_at(text, end, ".a")) {
						end += 2;
						word = "p.a";
					}
					if (!figure_tail_word(word)) break;
					if (end < text.size() && text[end] == '.' && keeps_full_stop(word)) ++end;
					at = end;
				} else {
					break;
				}
				rights.push_back(at);
			}
		}
		std::optional<Value> value;
		std::size_t start = core.start;
		std::size_t end = core.end;
		ParseHints hints;
		hints.expect = Kind::Number;
		for (const std::size_t left : lefts) {
			for (std::size_t index = rights.size(); index-- > 0 && !value;) {
				if (rights[index] <= left) continue;
				std::string span(text.substr(left, rights[index] - left));
				if (language == Language::Russian && core.digits && left == core.start && rights[index] == core.end
						&& std::count(span.begin(), span.end(), ',') == 1 && span.find_first_of(". \xC2\xE2") == std::string::npos) {
					// A bare "1,5" in a Russian text: the comma is the decimal mark.
					std::string dotted = span;
					std::replace(dotted.begin(), dotted.end(), ',', '.');
					if (auto number = Rational::parse_decimal(dotted)) value = make_number(*number);
				}
				if (!value) value = parse_value(span, hints);
				if (value) {
					start = left;
					end = rights[index];
				}
			}
			if (value) break;
		}
		if (!value && !core.digits) {
			cursor = core.end;
			continue;
		}
		RecognisedFigure figure;
		figure.start = start;
		figure.length = end - start;
		figure.value = value;
		const bool bare = !value || value->kind == Kind::Number;
		if (value) {
			switch (value->kind) {
				case Kind::Money: figure.kind = RecognisedFigure::Kind::Money; break;
				case Kind::Percentage: figure.kind = RecognisedFigure::Kind::Percentage; break;
				case Kind::Duration: figure.kind = RecognisedFigure::Kind::Duration; break;
				case Kind::Date:
				case Kind::DayOfYear: figure.kind = RecognisedFigure::Kind::Date; break;
				default: figure.kind = core.digits ? RecognisedFigure::Kind::Number : RecognisedFigure::Kind::Words; break;
			}
		}
		// A number word alone in prose ("one of the parties", «два экземпляра»).
		if (!core.digits && bare && value && value->amount < Rational(10) && start == core.start && end == core.end
				&& word_end(text, core.start) == core.end)
			figure.incidental = true;
		bool pluralPointer = false;
		if (core.digits && bare) {
			bool structural = false;
			// Inside a name: a cell (B12, $E$11, B4:B18), a handle (DOC_3, q3_total),
			// or an ordinal (2nd, 30th).
			if (letter_before(text, core.start) || (core.start > 0 && text[core.start - 1] == '$' && letter_before(text, core.start - 1)))
				structural = true;
			if (letter_at(text, core.end)) {
				const std::size_t suffix = word_end(text, core.end);
				const std::string word = lowered(text.substr(core.end, suffix - core.end));
				if (word == "st" || word == "nd" || word == "rd" || word == "th" || word == "го" || word == "й" || word == "я")
					structural = true;
				if (text[core.end] == '_') structural = true;
			}
			// A function's code: SUBTOTAL(109, …).
			for (const char *function : {"subtotal(", "aggregate("}) {
				const std::size_t length = std::string_view(function).size();
				std::size_t at = core.start;
				while (at > 0 && text[at - 1] == ' ') --at;
				if (at >= length && lowered(text.substr(at - length, length)) == function) structural = true;
			}
			// The word before it: "Section 5.2", "§ 4", «п. 3.1», «ст. 193», «Приложение № 1».
			{
				std::size_t at = core.start;
				at -= figure_space_before(text, at);
				bool numero = false;
				if (at >= 3 && text.compare(at - 3, 3, "\xE2\x84\x96") == 0) {
					at -= 3;
					numero = true;
				} else if (at >= 3 && lowered(text.substr(at - 3, 3)) == "no.") {
					at -= 3;
					numero = true;
				}
				if (numero) at -= figure_space_before(text, at);
				if (at >= 2 && text.compare(at - 2, 2, "\xC2\xA7") == 0) {
					structural = true;
					pluralPointer = at >= 4 && text.compare(at - 4, 2, "\xC2\xA7") == 0;  // "§§ 4 and 5"
				} else {
					if (at >= 1 && text[at - 1] == '.') --at;
					std::size_t wordStart = 0;
					const std::string word = word_ending_at(text, at, wordStart);
					if (!word.empty() && structural_word(word)) {
						structural = true;
						pluralPointer = plural_pointer_word(word);
					}
				}
			}
			// A list item's number at the start of its line: "1. ", "2) ", "(3) ".
			{
				std::size_t lineStart = core.start;
				while (lineStart > 0 && text[lineStart - 1] != '\n') --lineStart;
				std::string_view before = text.substr(lineStart, core.start - lineStart);
				while (!before.empty() && (before.front() == ' ' || before.front() == '\t')) before.remove_prefix(1);
				const bool opened = before == "(";
				if (before.empty() || opened) {
					const char after = core.end < text.size() ? text[core.end] : '\n';
					const char next = core.end + 1 < text.size() ? text[core.end + 1] : '\n';
					if ((after == '.' || after == ')') && (next == ' ' || next == '\t' || next == '\n' || next == '\r'))
						structural = true;
					if (opened && after == ')') structural = true;
				}
			}
			// The second number of a range or list of pointers: "rows 4-18",
			// "sections 4 and 5", «пп. 3, 4» - only after a word naming more
			// than one pointer, and only a number shaped like one. After a
			// single pointer ("Section 4.2, 1,250,000 remains", "clause 7 and
			// 737 units") the next number is a figure, so it is looked at.
			if (!structural && previousPlural && previousEnd != std::string_view::npos && previousEnd <= core.start
					&& core.start - previousEnd <= 12 && range_joiner(text.substr(previousEnd, core.start - previousEnd))
					&& pointer_shaped(text.substr(core.start, core.end - core.start))) {
				structural = true;
				pluralPointer = true;
			}
			figure.structural = structural;
		}
		previousEnd = figure.structural ? end : std::string_view::npos;
		previousPlural = figure.structural && pluralPointer;
		figures.push_back(figure);
		cursor = std::max(end, cursor + 1);
	}
	std::sort(figures.begin(), figures.end(), [](const RecognisedFigure &a, const RecognisedFigure &b) { return a.start < b.start; });
	return figures;
}

} // namespace singularity::protected_api::papinian::semantic
