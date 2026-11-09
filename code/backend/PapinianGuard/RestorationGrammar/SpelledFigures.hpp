#pragma once

#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A protected figure written the way a document writes it: in digits, in
// words, or in digits with its words - «828 000,00 (…
// …) … 00 …», "$21,350.00 (twenty-one thousand three hundred
// fifty dollars)", «…
// …» - written on the lawyer's computer from the real value (owner, 26 Sep
// 2026). The model never sees the value, so it can never write its words: it
// asks for them by the symbol's display format ([[Amount3.words]],
// [[Amount3.digitsandwords]]), and restoration writes them here,
// deterministically, never by a model. Words a model left beside a figure
// ("…") are the words of
// whatever figure stood there before; they are written again from the value
// the symbol restores to, so a changed figure never keeps stale words.
//
// Header-only: it reads SemanticAlgebra's own number words and plural rule,
// and is compiled only where SemanticAlgebra is.
namespace singularity::protected_api::papinian::restoration_grammar {

// The closed set of display formats a symbol may ask for (DESIGN4 §4.4).
enum class DisplayFormat { Digits, Words, DigitsAndWords };

// The format a symbol's part names ("digits", "words", "digitsandwords"), or
// nothing: any other part is a name's part (.first, .short) or unknown.
[[nodiscard]] inline std::optional<DisplayFormat> display_format_named(std::string_view part) {
	if (part == "digits") return DisplayFormat::Digits;
	if (part == "words") return DisplayFormat::Words;
	if (part == "digitsandwords") return DisplayFormat::DigitsAndWords;
	return std::nullopt;
}

namespace spelled_detail {

namespace semantic = papinian::semantic;
using semantic::Int128;
using semantic::Language;

inline std::string two_digits(Int128 number) {
	const int value = static_cast<int>(number % 100);
	return std::string(1, static_cast<char>('0' + value / 10)) + static_cast<char>('0' + value % 10);
}

// Digits of a whole number, grouped the way the language groups them.
inline std::string whole_digits(Int128 number, Language language) {
	semantic::Format plain;
	plain.style = semantic::FormatStyle::Number;
	return semantic::display(semantic::make_number(semantic::Rational(number)), plain, language);
}

inline std::string russian_money_noun(const std::string &currency, Int128 count) {
	if (currency == "RUB") return semantic::russian_plural(count, "\321\200\321\203\320\261\320\273\321\214", "\321\200\321\203\320\261\320\273\321\217", "\321\200\321\203\320\261\320\273\320\265\320\271");
	if (currency == "USD") return semantic::russian_plural(count, "\320\264\320\276\320\273\320\273\320\260\321\200 \320\241\320\250\320\220", "\320\264\320\276\320\273\320\273\320\260\321\200\320\260 \320\241\320\250\320\220", "\320\264\320\276\320\273\320\273\320\260\321\200\320\276\320\262 \320\241\320\250\320\220");
	if (currency == "EUR") return "\320\265\320\262\321\200\320\276";
	if (currency == "GBP") return semantic::russian_plural(count, "\321\204\321\203\320\275\321\202 \321\201\321\202\320\265\321\200\320\273\320\270\320\275\320\263\320\276\320\262", "\321\204\321\203\320\275\321\202\320\260 \321\201\321\202\320\265\321\200\320\273\320\270\320\275\320\263\320\276\320\262", "\321\204\321\203\320\275\321\202\320\276\320\262 \321\201\321\202\320\265\321\200\320\273\320\270\320\275\320\263\320\276\320\262");
	if (currency == "CNY") return semantic::russian_plural(count, "\321\216\320\260\320\275\321\214", "\321\216\320\260\320\275\321\217", "\321\216\320\260\320\275\320\265\320\271");
	if (currency == "JPY") return semantic::russian_plural(count, "\320\270\320\265\320\275\320\260", "\320\270\320\265\320\275\321\213", "\320\270\320\265\320\275");
	return currency;
}

inline std::optional<std::string> russian_minor_noun(const std::string &currency, Int128 count) {
	if (currency == "RUB") return semantic::russian_plural(count, "\320\272\320\276\320\277\320\265\320\271\320\272\320\260", "\320\272\320\276\320\277\320\265\320\271\320\272\320\270", "\320\272\320\276\320\277\320\265\320\265\320\272");
	if (currency == "USD" || currency == "EUR" || currency == "CAD" || currency == "AUD")
		return semantic::russian_plural(count, "\321\206\320\265\320\275\321\202", "\321\206\320\265\320\275\321\202\320\260", "\321\206\320\265\320\275\321\202\320\276\320\262");
	if (currency == "GBP") return semantic::russian_plural(count, "\320\277\320\265\320\275\321\201", "\320\277\320\265\320\275\321\201\320\260", "\320\277\320\265\320\275\321\201\320\276\320\262");
	return std::nullopt;
}

// Feminine nouns take «…» in their count.
inline bool russian_feminine_currency(const std::string &currency) { return currency == "JPY"; }

// English ordinal from a cardinal's words: "twenty-one" -> "twenty-first".
inline std::string english_ordinal(Int128 number) {
	std::string words = semantic::number_in_words(number, Language::English);
	std::size_t start = words.find_last_of(" -");
	start = start == std::string::npos ? 0 : start + 1;
	const std::string last = words.substr(start);
	std::string ordinal;
	if (last == "one") ordinal = "first";
	else if (last == "two") ordinal = "second";
	else if (last == "three") ordinal = "third";
	else if (last == "five") ordinal = "fifth";
	else if (last == "eight") ordinal = "eighth";
	else if (last == "nine") ordinal = "ninth";
	else if (last == "twelve") ordinal = "twelfth";
	else if (!last.empty() && last.back() == 'y') ordinal = last.substr(0, last.size() - 1) + "ieth";
	else ordinal = last + "th";
	return words.substr(0, start) + ordinal;
}

// The Russian ordinal in the genitive (masculine and neuter): «…
// …», «…». Cardinal words
// for every part but the last, as Russian writes a compound ordinal.
inline std::string russian_ordinal_genitive(Int128 number) {
	static const char *units[] = {"", "\320\277\320\265\321\200\320\262\320\276\320\263\320\276", "\320\262\321\202\320\276\321\200\320\276\320\263\320\276", "\321\202\321\200\320\265\321\202\321\214\320\265\320\263\320\276", "\321\207\320\265\321\202\320\262\321\221\321\200\321\202\320\276\320\263\320\276", "\320\277\321\217\321\202\320\276\320\263\320\276", "\321\210\320\265\321\201\321\202\320\276\320\263\320\276", "\321\201\320\265\320\264\321\214\320\274\320\276\320\263\320\276", "\320\262\320\276\321\201\321\214\320\274\320\276\320\263\320\276",
		"\320\264\320\265\320\262\321\217\321\202\320\276\320\263\320\276", "\320\264\320\265\321\201\321\217\321\202\320\276\320\263\320\276", "\320\276\320\264\320\270\320\275\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\320\264\320\262\320\265\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\321\202\321\200\320\270\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\321\207\320\265\321\202\321\213\321\200\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\320\277\321\217\321\202\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\321\210\320\265\321\201\321\202\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276",
		"\321\201\320\265\320\274\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\320\262\320\276\321\201\320\265\320\274\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\320\264\320\265\320\262\321\217\321\202\320\275\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276"};
	static const char *tens[] = {"", "", "\320\264\320\262\320\260\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\321\202\321\200\320\270\320\264\321\206\320\260\321\202\320\276\320\263\320\276", "\321\201\320\276\321\200\320\276\320\272\320\276\320\262\320\276\320\263\320\276", "\320\277\321\217\321\202\320\270\320\264\320\265\321\201\321\217\321\202\320\276\320\263\320\276", "\321\210\320\265\321\201\321\202\320\270\320\264\320\265\321\201\321\217\321\202\320\276\320\263\320\276", "\321\201\320\265\320\274\320\270\320\264\320\265\321\201\321\217\321\202\320\276\320\263\320\276",
		"\320\262\320\276\321\201\321\214\320\274\320\270\320\264\320\265\321\201\321\217\321\202\320\276\320\263\320\276", "\320\264\320\265\320\262\321\217\320\275\320\276\321\201\321\202\320\276\320\263\320\276"};
	static const char *hundreds[] = {"", "\321\201\320\276\321\202\320\276\320\263\320\276", "\320\264\320\262\321\203\321\205\321\201\320\276\321\202\320\276\320\263\320\276", "\321\202\321\200\321\221\321\205\321\201\320\276\321\202\320\276\320\263\320\276", "\321\207\320\265\321\202\321\213\321\200\321\221\321\205\321\201\320\276\321\202\320\276\320\263\320\276", "\320\277\321\217\321\202\320\270\321\201\320\276\321\202\320\276\320\263\320\276", "\321\210\320\265\321\201\321\202\320\270\321\201\320\276\321\202\320\276\320\263\320\276",
		"\321\201\320\265\320\274\320\270\321\201\320\276\321\202\320\276\320\263\320\276", "\320\262\320\276\321\201\321\214\320\274\320\270\321\201\320\276\321\202\320\276\320\263\320\276", "\320\264\320\265\320\262\321\217\321\202\320\270\321\201\320\276\321\202\320\276\320\263\320\276"};
	static const char *thousandsStem[] = {"", "", "\320\264\320\262\321\203\321\205", "\321\202\321\200\321\221\321\205", "\321\207\320\265\321\202\321\213\321\200\321\221\321\205", "\320\277\321\217\321\202\320\270", "\321\210\320\265\321\201\321\202\320\270", "\321\201\320\265\320\274\320\270", "\320\262\320\276\321\201\321\214\320\274\320\270", "\320\264\320\265\320\262\321\217\321\202\320\270"};
	if (number <= 0 || number >= 10000) return semantic::number_in_words(number, Language::Russian);
	// Each part below is a small whole number that indexes one of the lists.
	const auto at = [](Int128 part) { return static_cast<std::size_t>(part); };
	const Int128 thousands = number / 1000;
	const Int128 rest = number % 1000;
	std::string head;
	if (thousands > 0) {
		if (rest == 0) return std::string(thousandsStem[at(thousands)]) + "\321\202\321\213\321\201\321\217\321\207\320\275\320\276\320\263\320\276";
		head = semantic::number_in_words(thousands * 1000, Language::Russian) + " ";
	}
	const Int128 hundred = rest / 100;
	const Int128 tail = rest % 100;
	if (tail == 0) return head + hundreds[at(hundred)];
	if (hundred > 0) head += semantic::number_in_words(hundred * 100, Language::Russian) + " ";
	if (tail < 20) return head + units[at(tail)];
	if (tail % 10 == 0) return head + tens[at(tail / 10)];
	return head + semantic::number_in_words(tail / 10 * 10, Language::Russian) + " " + units[at(tail % 10)];
}

// A decimal fraction in words: "one point five", «…»
// (Russian fractions count in the feminine and name their denominator).
inline std::optional<std::string> decimal_words(const semantic::Rational &number, Language language, bool feminine = false) {
	if (number.is_integer()) return semantic::number_in_words(number.numerator(), language, feminine);
	const std::optional<std::string> exact = number.abs().exact_decimal(4);
	if (!exact) return std::nullopt;
	const std::size_t point = exact->find('.');
	if (point == std::string::npos) return std::nullopt;
	const std::string wholeText = exact->substr(0, point);
	const std::string fraction = exact->substr(point + 1);
	Int128 whole = 0;
	for (const char digit : wholeText) whole = whole * 10 + (digit - '0');
	Int128 part = 0;
	for (const char digit : fraction) part = part * 10 + (digit - '0');
	const std::string sign = number.sign() < 0 ? (language == Language::Russian ? "\320\274\320\270\320\275\321\203\321\201 " : "minus ") : "";
	if (language == Language::English) {
		std::string words = semantic::number_in_words(whole, Language::English) + " point";
		static const char *digits[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
		for (const char digit : fraction) words += std::string(" ") + digits[digit - '0'];
		return sign + words;
	}
	static const char *one[] = {"", "\320\264\320\265\321\201\321\217\321\202\320\260\321\217", "\321\201\320\276\321\202\320\260\321\217", "\321\202\321\213\321\201\321\217\321\207\320\275\320\260\321\217", "\320\264\320\265\321\201\321\217\321\202\320\270\321\202\321\213\321\201\321\217\321\207\320\275\320\260\321\217"};
	static const char *many[] = {"", "\320\264\320\265\321\201\321\217\321\202\321\213\321\205", "\321\201\320\276\321\202\321\213\321\205", "\321\202\321\213\321\201\321\217\321\207\320\275\321\213\321\205", "\320\264\320\265\321\201\321\217\321\202\320\270\321\202\321\213\321\201\321\217\321\207\320\275\321\213\321\205"};
	const std::size_t places = fraction.size();
	return sign + semantic::number_in_words(whole, Language::Russian, true) + " "
		+ semantic::russian_plural(whole, "\321\206\320\265\320\273\320\260\321\217", "\321\206\320\265\320\273\321\213\321\205", "\321\206\320\265\320\273\321\213\321\205") + " " + semantic::number_in_words(part, Language::Russian, true)
		+ " " + semantic::russian_plural(part, one[places], many[places], many[places]);
}

inline std::string english_month(unsigned month) {
	static const char *names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
		"November", "December"};
	return month >= 1 && month <= 12 ? names[month - 1] : "";
}

inline std::string russian_month_genitive(unsigned month) {
	static const char *names[] = {"\321\217\320\275\320\262\320\260\321\200\321\217", "\321\204\320\265\320\262\321\200\320\260\320\273\321\217", "\320\274\320\260\321\200\321\202\320\260", "\320\260\320\277\321\200\320\265\320\273\321\217", "\320\274\320\260\321\217", "\320\270\321\216\320\275\321\217", "\320\270\321\216\320\273\321\217", "\320\260\320\262\320\263\321\203\321\201\321\202\320\260", "\321\201\320\265\320\275\321\202\321\217\320\261\321\200\321\217", "\320\276\320\272\321\202\321\217\320\261\321\200\321\217",
		"\320\275\320\276\321\217\320\261\321\200\321\217", "\320\264\320\265\320\272\320\260\320\261\321\200\321\217"};
	return month >= 1 && month <= 12 ? names[month - 1] : "";
}

// The civil day of a day count since 1970-01-01 (Howard Hinnant's algorithm).
struct CivilDay {
	long long year = 1970;
	unsigned month = 1;
	unsigned day = 1;
};
inline CivilDay civil_of(long long days) {
	days += 719468;
	const long long era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
	const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
	const long long year = static_cast<long long>(yearOfEra) + era * 400;
	const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
	const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
	const unsigned day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
	const unsigned month = monthPrime < 10 ? monthPrime + 3 : monthPrime - 9;
	return {month <= 2 ? year + 1 : year, month, day};
}

// A money amount's words: "twenty-one thousand three hundred fifty dollars",
inline std::string money_words(const semantic::Value &value, Language language) {
	const bool minorUnit = semantic::minor_unit_places(value.currency) > 0;
	const semantic::Rational cents = value.amount.rounded(minorUnit ? 2 : 0);
	const semantic::Rational magnitude = cents.abs();
	const Int128 whole = magnitude.floored().numerator();
	const Int128 minor = ((magnitude - semantic::Rational(whole)) * semantic::Rational(100)).numerator();
	const bool negative = cents.sign() < 0;
	if (language == Language::English) {
		// SemanticAlgebra's own words of the amount, as it writes them after the figure.
		semantic::Value positive = value;
		positive.amount = magnitude;
		positive.basis = semantic::RateBasis::None;
		semantic::Format words;
		words.words = true;
		const std::string shown = semantic::display(positive, words, Language::English);
		const std::size_t open = shown.find(" (");
		const std::size_t close = shown.rfind(')');
		std::string text = open != std::string::npos && close != std::string::npos && close > open
			? shown.substr(open + 2, close - open - 2) : semantic::number_in_words(whole, Language::English);
		return negative ? "minus " + text : text;
	}
	std::string text = semantic::number_in_words(whole, Language::Russian, russian_feminine_currency(value.currency));
	if (!value.currency.empty()) {
		text += " " + russian_money_noun(value.currency, whole);
		if (minorUnit) {
			if (const auto noun = russian_minor_noun(value.currency, minor)) text += " " + two_digits(minor) + " " + *noun;
			else if (minor != 0) text += " " + two_digits(minor) + "/100";
		}
	}
	return negative ? "\320\274\320\270\320\275\321\203\321\201 " + text : text;
}

inline std::string money_digits_and_words(const semantic::Value &value, Language language) {
	if (language == Language::English) {
		semantic::Value plain = value;
		plain.basis = semantic::RateBasis::None;
		semantic::Format words;
		words.words = true;
		return semantic::display(plain, words, Language::English);
	}
	// «…»: the
	// kopecks after the figure when the source wrote them or there are any.
	const bool minorUnit = semantic::minor_unit_places(value.currency) > 0;
	const semantic::Rational cents = value.amount.rounded(minorUnit ? 2 : 0);
	const semantic::Rational magnitude = cents.abs();
	const Int128 whole = magnitude.floored().numerator();
	const Int128 minor = ((magnitude - semantic::Rational(whole)) * semantic::Rational(100)).numerator();
	std::string text = whole_digits(whole, Language::Russian);
	if (minorUnit && (minor != 0 || value.source_decimals > 0)) text += "," + two_digits(minor);
	text += " (" + semantic::number_in_words(whole, Language::Russian, russian_feminine_currency(value.currency)) + ")";
	if (!value.currency.empty()) {
		text += " " + russian_money_noun(value.currency, whole);
		if (minorUnit) {
			if (const auto noun = russian_minor_noun(value.currency, minor)) text += " " + two_digits(minor) + " " + *noun;
			else if (minor != 0) text += " " + two_digits(minor) + "/100";
		}
	}
	return cents.sign() < 0 ? "\320\274\320\270\320\275\321\203\321\201 " + text : text;
}

inline std::optional<std::string> date_words(const semantic::Value &value, Language language) {
	if (value.date.precision != singularity::context::temporal::Precision::Day) return std::nullopt;
	const CivilDay day = civil_of(value.date.first_day);
	if (day.year < 1 || day.year > 9999) return std::nullopt;
	if (language == Language::English)
		return "the " + english_ordinal(day.day) + " day of " + english_month(day.month) + ", "
			+ semantic::number_in_words(day.year, Language::English);
	return russian_ordinal_genitive(day.day) + " " + russian_month_genitive(day.month) + " " + russian_ordinal_genitive(day.year)
		+ " \320\263\320\276\320\264\320\260";
}

} // namespace spelled_detail

// `value` in `format` and the language of the text it stands in, or nothing
// when that value has no such form (a month, a quarter; a figure too long for
// words): the caller then writes it as it would without a format. Digits is
// the value's own display.
[[nodiscard]] inline std::optional<std::string> spelled(const semantic::Value &value, DisplayFormat format,
		semantic::Language language) {
	namespace sd = spelled_detail;
	using semantic::Kind;
	try {
		// The figure a format writes beside its words: a rate's period and an
		// amount's are the sentence's to write, as in any restored prose.
		semantic::Value figure = value;
		if (figure.kind == Kind::Percentage || figure.kind == Kind::Money) figure.basis = semantic::RateBasis::None;
		if (figure.kind == Kind::Count) {
			figure.kind = Kind::Number;
		}
		const auto digits = [&]() { return semantic::display(figure, {}, language); };
		if (format == DisplayFormat::Digits) return digits();
		std::optional<std::string> words;
		switch (figure.kind) {
			case Kind::Money:
				if (format == DisplayFormat::DigitsAndWords) return sd::money_digits_and_words(figure, language);
				return sd::money_words(figure, language);
			case Kind::Number:
				words = sd::decimal_words(figure.amount, language);
				break;
			case Kind::Percentage: {
				const semantic::Rational points = figure.amount * semantic::Rational(100);
				const std::optional<std::string> number = sd::decimal_words(points, language, language == semantic::Language::Russian
					&& !points.is_integer());
				if (!number) return std::nullopt;
				if (language == semantic::Language::English) words = *number + " percent";
				else if (!points.is_integer()) words = *number + " \320\277\321\200\320\276\321\206\320\265\320\275\321\202\320\260";
				else words = *number + " " + semantic::russian_plural(points.numerator(), "\320\277\321\200\320\276\321\206\320\265\320\275\321\202", "\320\277\321\200\320\276\321\206\320\265\320\275\321\202\320\260", "\320\277\321\200\320\276\321\206\320\265\320\275\321\202\320\276\320\262");
				break;
			}
			case Kind::Duration: {
				if (!figure.amount.is_integer()) return std::nullopt;
				// The unit noun as the value's own display writes it: "…", "1 business day".
				const std::string shown = digits();
				const std::size_t space = shown.find(' ');
				const std::string noun = space == std::string::npos ? std::string() : shown.substr(space + 1);
				const std::string count = semantic::number_in_words(figure.amount.numerator(), language,
					language == semantic::Language::Russian && figure.unit == semantic::DurationUnit::Week);
				if (format == DisplayFormat::Words) return noun.empty() ? count : count + " " + noun;
				return shown.substr(0, space) + " (" + count + ")" + (noun.empty() ? "" : " " + noun);
			}
			case Kind::Date:
				words = sd::date_words(figure, language);
				break;
			case Kind::Boolean:
				return digits();
			default:
				return std::nullopt;
		}
		if (!words) return std::nullopt;
		if (format == DisplayFormat::Words) return words;
		return digits() + " (" + *words + ")";
	} catch (const semantic::AlgebraError &) {
		return std::nullopt;
	}
}

namespace spelled_detail {

inline bool space_at(const std::string &text, std::size_t at, std::size_t &length) {
	if (at >= text.size()) return false;
	if (text[at] == ' ') { length = 1; return true; }
	if (text.compare(at, 2, "\xC2\xA0") == 0) { length = 2; return true; }
	return false;
}

inline std::size_t skip_spaces(const std::string &text, std::size_t at) {
	std::size_t length = 0;
	while (space_at(text, at, length)) at += length;
	return at;
}

inline std::string lowered_ascii(std::string text) {
	for (char &character : text)
		if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
	return text;
}

// Whether `word` (lowered) is a currency's or its sub-unit's noun, or a joining
// word of an amount in words ("and", "only", «…»).
inline bool money_word(const std::string &word) {
	const auto starts = [&](const char *stem) { return word.rfind(stem, 0) == 0; };
	return starts("dollar") || starts("euro") || starts("rubl") || starts("roubl") || starts("pound") || starts("cent")
		|| starts("kopeck") || starts("kopek") || starts("penn") || starts("pence") || word == "u.s." || word == "us" || word == "and"
		|| word == "only" || word == "yen" || starts("\321\200\321\203\320\261\320\273") || starts("\320\264\320\276\320\273\320\273\320\260\321\200") || starts("\320\265\320\262\321\200\320\276") || starts("\320\272\320\276\320\277\320\265") || starts("\321\206\320\265\320\275\321\202")
		|| starts("\321\204\321\203\320\275\321\202") || starts("\321\201\321\202\320\265\321\200\320\273\320\270\320\275\320\263") || word == "\321\201\321\210\320\260" || word == "\321\200\320\276\320\262\320\275\320\276";
}

// Whether `word` (lowered) is a duration's or a percentage's noun.
inline bool unit_word(const std::string &word) {
	const auto starts = [&](const char *stem) { return word.rfind(stem, 0) == 0; };
	return starts("day") || starts("week") || starts("month") || starts("year") || starts("business") || starts("calendar")
		|| starts("percent") || starts("\320\264\320\275") || starts("\320\264\320\265\320\275") || starts("\320\275\320\265\320\264\320\265\320\273") || starts("\320\274\320\265\321\201\321\217\321\206") || starts("\320\263\320\276\320\264") || word == "\320\273\320\265\321\202"
		|| starts("\321\200\320\260\320\261\320\276\321\207") || starts("\320\272\320\260\320\273\320\265\320\275\320\264\320\260\321\200") || starts("\320\277\321\200\320\276\321\206\320\265\320\275\321\202");
}

// The words of `text` split on spaces, lowered (ASCII only; Cyrillic is
// matched on its lower-case stems, which is how contracts write these words).
inline std::vector<std::string> words_of(const std::string &text) {
	std::vector<std::string> words;
	std::string word;
	for (std::size_t at = 0; at <= text.size(); ++at) {
		std::size_t length = 0;
		if (at == text.size() || space_at(text, at, length) || text[at] == ',') {
			if (!word.empty()) words.push_back(lowered_ascii(word));
			word.clear();
			if (length > 1) at += length - 1;
			continue;
		}
		word.push_back(text[at]);
	}
	return words;
}

} // namespace spelled_detail

// The words a text writes right after a restored figure that spell some figure
// - "…", " (twenty-one
// thousand dollars)", " ([[Amount5.words]])" - as the length from `at` to
// their end, or nothing. Such words belong to whatever figure the model saw
// there before; the caller writes the figure with its own words in their
// place, so a changed figure never stands beside stale words. Only an amount,
// a count, a percentage or a period reads so; a paren of other words is the
// model's own and is kept.
[[nodiscard]] inline std::optional<std::size_t> spelling_after(const std::string &text, std::size_t at,
		const semantic::Value &value) {
	namespace sd = spelled_detail;
	using semantic::Kind;
	if (value.kind != Kind::Money && value.kind != Kind::Number && value.kind != Kind::Count && value.kind != Kind::Percentage
			&& value.kind != Kind::Duration)
		return std::nullopt;
	std::size_t open = sd::skip_spaces(text, at);
	if (open >= text.size() || text[open] != '(') return std::nullopt;
	const std::size_t close = text.find(')', open + 1);
	if (close == std::string::npos || close - open > 240) return std::nullopt;
	const std::string inside = text.substr(open + 1, close - open - 1);
	bool spelled = false;
	// A symbol of some figure written in words: [[Amount5.words]].
	if (inside.size() > 12 && inside.rfind("[[", 0) == 0 && inside.compare(inside.size() - 8, 8, ".words]]") == 0
			&& inside.find(' ') == std::string::npos)
		spelled = true;
	if (!spelled) {
		// Number words, then only money or unit words.
		const std::vector<std::string> words = sd::words_of(inside);
		std::size_t numberWords = 0;
		std::string joined;
		for (std::size_t index = 0; index < words.size(); ++index) {
			const std::string candidate = joined.empty() ? words[index] : joined + " " + words[index];
			if (semantic::parse_number_words(candidate)) {
				joined = candidate;
				numberWords = index + 1;
				continue;
			}
			if (index == 0) break;
			// «…» and "forty-five" grow word by word; "and" joins them.
			if (words[index] == "and" && index + 1 < words.size()
					&& semantic::parse_number_words(candidate + " " + words[index + 1])) {
				joined = candidate;
				continue;
			}
			break;
		}
		if (numberWords == 0) return std::nullopt;
		// After the number, only the words of what this value is: a currency's
		// for an amount, a period's for a period, "percent" for a rate.
		for (std::size_t index = numberWords; index < words.size(); ++index) {
			const std::string &word = words[index];
			const bool digitsOnly = !word.empty() && word.find_first_not_of("0123456789/") == std::string::npos;
			const bool percent = word.rfind("percent", 0) == 0 || word.rfind("\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82", 0) == 0;
			bool fits = false;
			if (value.kind == Kind::Money) fits = sd::money_word(word) || (digitsOnly && index > numberWords);
			else if (value.kind == Kind::Percentage) fits = percent;
			else if (value.kind == Kind::Duration) fits = sd::unit_word(word) && !percent;
			if (!fits) return std::nullopt;
		}
		spelled = true;
	}
	if (!spelled) return std::nullopt;
	std::size_t end = close + 1;
	// The noun a Russian amount or period writes after its words, and its
	// kopecks: «…», "(ten) days".
	for (int step = 0; step < 4; ++step) {
		const std::size_t wordStart = sd::skip_spaces(text, end);
		std::size_t wordEnd = wordStart;
		while (wordEnd < text.size() && text[wordEnd] != ' ' && text[wordEnd] != ',' && text[wordEnd] != '.' && text[wordEnd] != ';'
				&& text[wordEnd] != ')' && text[wordEnd] != '\n' && text.compare(wordEnd, 2, "\xC2\xA0") != 0)
			++wordEnd;
		if (wordEnd == wordStart || wordStart == end) break;
		const std::string word = sd::lowered_ascii(text.substr(wordStart, wordEnd - wordStart));
		const bool digitsOnly = word.find_first_not_of("0123456789") == std::string::npos && word.size() <= 2;
		const bool percentWord = word.rfind("percent", 0) == 0 || word.rfind("\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82", 0) == 0;
		const bool noun = value.kind == Kind::Money ? sd::money_word(word) && word != "and"
			: value.kind == Kind::Percentage ? percentWord
			: value.kind == Kind::Duration ? sd::unit_word(word) && !percentWord : false;
		// Kopecks are two digits followed by their noun.
		if (digitsOnly && value.kind == Kind::Money) {
			const std::size_t nextStart = sd::skip_spaces(text, wordEnd);
			std::size_t nextEnd = nextStart;
			while (nextEnd < text.size() && text[nextEnd] != ' ' && text[nextEnd] != ',' && text[nextEnd] != '.'
					&& text[nextEnd] != ';' && text[nextEnd] != '\n')
				++nextEnd;
			if (nextStart == wordEnd || !sd::money_word(sd::lowered_ascii(text.substr(nextStart, nextEnd - nextStart)))) break;
			end = nextEnd;
			continue;
		}
		if (!noun) break;
		end = wordEnd;
	}
	return end - at;
}

// The minor units a text writes right after an amount - "…", " 50
// cents", " and 50 cents" - as the length from `at` to their end, or nothing.
// A rendering that already writes them (an amount in words, or its figure and
// words: «…») takes their place, so they are never written
// twice ("…").
[[nodiscard]] inline std::optional<std::size_t> minor_units_after(const std::string &text, std::size_t at) {
	namespace sd = spelled_detail;
	std::size_t cursor = sd::skip_spaces(text, at);
	if (cursor == at) return std::nullopt;
	if (text.compare(cursor, 4, "and ") == 0 || text.compare(cursor, 3, "\xD0\xB8 ") == 0)  // "and", «…»
		cursor = sd::skip_spaces(text, cursor + (text[cursor] == 'a' ? 3 : 2));
	const std::size_t digitsStart = cursor;
	while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9' && cursor - digitsStart < 2) ++cursor;
	if (cursor == digitsStart || (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')) return std::nullopt;
	const std::size_t wordStart = sd::skip_spaces(text, cursor);
	if (wordStart == cursor) return std::nullopt;
	std::size_t wordEnd = wordStart;
	while (wordEnd < text.size() && text[wordEnd] != ' ' && text[wordEnd] != ',' && text[wordEnd] != '.' && text[wordEnd] != ';'
			&& text[wordEnd] != ')' && text[wordEnd] != '\n' && text.compare(wordEnd, 2, "\xC2\xA0") != 0)
		++wordEnd;
	const std::string word = sd::lowered_ascii(text.substr(wordStart, wordEnd - wordStart));
	const auto starts = [&](const char *stem) { return word.rfind(stem, 0) == 0; };
	if (!(starts("cent") || starts("kopeck") || starts("kopek") || starts("penn") || starts("pence")
			|| starts("\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB5")        // …
			|| starts("\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82")        // …
			|| starts("\xD0\xBF\xD0\xB5\xD0\xBD\xD1\x81")))      // …
		return std::nullopt;
	return wordEnd - at;
}

// Whether a rendering of an amount writes its minor units in words' company.
[[nodiscard]] inline bool writes_minor_units(const std::string &rendered) {
	for (const char *noun : {"\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB5", "\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82",
			"\xD0\xBF\xD0\xB5\xD0\xBD\xD1\x81", " cent", " penn", " pence", "/100"})
		if (rendered.find(noun) != std::string::npos) return true;
	return false;
}

// A row set Singularity worked out ([[Rows1]], DESIGN4 §4.4) as the lawyer reads
// it: its rows' own labels, joined as the answer's language joins a list -
// "July, August and September 2026", «…». The
// issuer writes the labels into the row's `original`, each ended by
// kRowLabelSeparator; a label list with one label is that label.
inline constexpr char kRowLabelSeparator = '\x1F';

[[nodiscard]] inline std::optional<std::string> row_labels(const std::string &original, semantic::Language language) {
	if (original.find(kRowLabelSeparator) == std::string::npos) return std::nullopt;
	std::vector<std::string> labels;
	std::string label;
	for (const char character : original) {
		if (character == kRowLabelSeparator) {
			if (!label.empty()) labels.push_back(label);
			label.clear();
		} else {
			label.push_back(character);
		}
	}
	if (!label.empty()) labels.push_back(label);
	if (labels.empty()) return std::nullopt;
	std::string text = labels.front();
	for (std::size_t index = 1; index < labels.size(); ++index)
		text += (index + 1 == labels.size() ? (language == semantic::Language::Russian ? " \xD0\xB8 " : " and ") : ", ") + labels[index];
	return text;
}

} // namespace singularity::protected_api::papinian::restoration_grammar
