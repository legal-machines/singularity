#include "Document/XlsxNumberFormat/XlsxNumberFormat.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::documents {
namespace {

// The spaces Excel leaves where a character would stand: "_(", "_)" and "_-"
// leave a thin space (the width of a parenthesis or a minus), "?" leaves the
// width of a digit so that decimals line up.
const char* const kThinSpace = "\xE2\x80\x89";   // U+2009
const char* const kFigureSpace = "\xE2\x80\x87"; // U+2007

// Excel's largest date: 31 December 9999.
constexpr double kLastDateSerial = 2958466.0;

constexpr std::array<const char*, 64> kIndexedPalette = {
	"000000", "FFFFFF", "FF0000", "00FF00", "0000FF", "FFFF00", "FF00FF", "00FFFF",
	"000000", "FFFFFF", "FF0000", "00FF00", "0000FF", "FFFF00", "FF00FF", "00FFFF",
	"800000", "008000", "000080", "808000", "800080", "008080", "C0C0C0", "808080",
	"9999FF", "993366", "FFFFCC", "CCFFFF", "660066", "FF8080", "0066CC", "CCCCFF",
	"000080", "FF00FF", "FFFF00", "00FFFF", "800080", "800000", "008080", "0000FF",
	"00CCFF", "CCFFFF", "CCFFCC", "FFFF99", "99CCFF", "FF99CC", "CC99FF", "FFCC99",
	"3366FF", "33CCCC", "99CC00", "FFCC00", "FF9900", "FF6600", "666699", "969696",
	"003366", "339966", "003300", "333300", "993300", "993366", "333399", "333333",
};

constexpr std::array<const char*, 12> kMonthNames = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December",
};

constexpr std::array<const char*, 7> kWeekdayNames = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

char ascii_lower(char character) {
	if (character >= 'A' && character <= 'Z') return static_cast<char>(character - 'A' + 'a');
	return character;
}

std::string ascii_lower(const std::string& text) {
	std::string lowered = text;
	for (char& character : lowered) character = ascii_lower(character);
	return lowered;
}

bool starts_with_ignoring_case(const std::string& text, std::size_t index, const char* prefix) {
	const std::size_t length = std::strlen(prefix);
	if (index + length > text.size()) return false;
	for (std::size_t offset = 0; offset < length; ++offset) {
		if (ascii_lower(text[index + offset]) != prefix[offset]) return false;
	}
	return true;
}

// The byte length of the UTF-8 character that starts with `lead`.
std::size_t utf8_length(char lead) {
	const unsigned char byte = static_cast<unsigned char>(lead);
	if (byte < 0x80) return 1;
	if ((byte >> 5) == 0x6) return 2;
	if ((byte >> 4) == 0xE) return 3;
	if ((byte >> 3) == 0x1E) return 4;
	return 1;
}

std::string utf8_character_at(const std::string& text, std::size_t index) {
	if (index >= text.size()) return std::string();
	return text.substr(index, std::min(utf8_length(text[index]), text.size() - index));
}

// Numbers inside a format code ("[>=100]") are written the C way whatever the
// process locale says.
bool parse_classic_double(const std::string& text, double& value) {
	std::istringstream stream(text);
	stream.imbue(std::locale::classic());
	stream >> value;
	return !stream.fail();
}

std::string pad_number(long long number, std::size_t width) {
	std::string text = std::to_string(number);
	if (text.size() < width) text.insert(0, width - text.size(), '0');
	return text;
}

/* - - - - - - */

// A non-negative value as Excel sees it: at most 15 significant digits.
// value = 0.<digits> x 10^point. The digits carry no leading or trailing zeros,
// and zero is the empty string.
struct DecimalValue {
	std::string digits;
	int point = 0;
};

void strip_trailing_zeros(DecimalValue& value) {
	while (value.digits.empty() == false && value.digits.back() == '0') value.digits.pop_back();
	if (value.digits.empty() == true) value.point = 0;
}

DecimalValue decimal_from_magnitude(double magnitude) {
	DecimalValue value;
	if (!(magnitude > 0.0) || std::isfinite(magnitude) == false) return value;

	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.14e", magnitude);
	// "d.dddddddddddddde+XX": the digits before the exponent, whatever the C
	// locale's decimal separator is.
	const char* exponentMark = std::strchr(buffer, 'e');
	if (exponentMark == nullptr) return value;
	for (const char* cursor = buffer; cursor < exponentMark; ++cursor) {
		if (*cursor >= '0' && *cursor <= '9') value.digits.push_back(*cursor);
	}
	value.point = std::atoi(exponentMark + 1) + 1;
	strip_trailing_zeros(value);
	return value;
}

// Keeps the first `significant` digits, rounding half away from zero.
DecimalValue round_significant(DecimalValue value, int significant) {

	if (value.digits.empty() == true || significant >= static_cast<int>(value.digits.size())) return value;
	if (significant < 0) return DecimalValue();

	const bool isRoundingUp = value.digits[static_cast<std::size_t>(significant)] >= '5';
	value.digits.resize(static_cast<std::size_t>(significant));
	if (isRoundingUp == true) {
		int index = significant - 1;
		while (index >= 0 && value.digits[static_cast<std::size_t>(index)] == '9') {
			value.digits[static_cast<std::size_t>(index)] = '0';
			--index;
		}
		if (index < 0) {
			value.digits.insert(value.digits.begin(), '1');
			++value.point;
		} else {
			++value.digits[static_cast<std::size_t>(index)];
		}
	}
	strip_trailing_zeros(value);
	return value;

}

DecimalValue round_decimals(const DecimalValue& value, int decimals) {
	return round_significant(value, value.point + decimals);
}

// The integer part's digits, "" when it is zero.
std::string integer_digits(const DecimalValue& value) {
	if (value.point <= 0 || value.digits.empty() == true) return std::string();
	const std::size_t integerLength = static_cast<std::size_t>(value.point);
	std::string digits = value.digits.substr(0, std::min(integerLength, value.digits.size()));
	digits.append(integerLength - digits.size(), '0');
	return digits;
}

// Exactly `count` digits after the decimal point.
std::string fraction_digits(const DecimalValue& value, int count) {
	std::string digits;
	for (int index = 0; index < count; ++index) {
		const int position = value.point + index;
		if (position >= 0 && position < static_cast<int>(value.digits.size())) {
			digits.push_back(value.digits[static_cast<std::size_t>(position)]);
		} else {
			digits.push_back('0');
		}
	}
	return digits;
}

std::string fixed_text(const DecimalValue& value, int decimals) {
	const DecimalValue rounded = round_decimals(value, decimals);
	std::string text = integer_digits(rounded);
	if (text.empty() == true) text = "0";
	if (decimals > 0) text += "." + fraction_digits(rounded, decimals);
	return text;
}

std::string strip_fraction_zeros(std::string text) {
	if (text.find('.') == std::string::npos) return text;
	while (text.back() == '0') text.pop_back();
	if (text.back() == '.') text.pop_back();
	return text;
}

// "1.23457E+11": `decimals` places with trailing zeros dropped and an exponent
// of at least two digits.
std::string exponential_text(const DecimalValue& value, int decimals) {
	const DecimalValue rounded = round_significant(value, decimals + 1);
	if (rounded.digits.empty() == true) return "0E+00";
	std::string text(1, rounded.digits[0]);
	if (rounded.digits.size() > 1) text += "." + rounded.digits.substr(1);
	const int exponent = rounded.point - 1;
	return text + "E" + (exponent < 0 ? "-" : "+") + pad_number(std::abs(exponent), 2);
}

// Excel's General: what fits in 11 characters, the way Excel (and SheetJS SSF,
// which is verified against it) chooses - fixed notation up to 11 significant
// characters, then 6 significant digits in scientific notation.
std::string general_text(double value) {

	if (std::isfinite(value) == false) return "#NUM!";
	if (value == 0.0) return "0";

	const bool isNegative = value < 0.0;
	const DecimalValue magnitude = decimal_from_magnitude(std::fabs(value));
	const int order = magnitude.point - 1; // floor(log10(|value|))

	std::string text;
	if (order >= -4 && order <= -1) {
		// 10 + order significant digits, which below 1 is always 9 decimals.
		text = strip_fraction_zeros(fixed_text(magnitude, 9));
	} else if (order >= -9 && order <= 9) {
		text = strip_fraction_zeros(fixed_text(magnitude, 12));
		if (text.size() > 11) {
			if (order >= 0) text = strip_fraction_zeros(fixed_text(magnitude, 10 - magnitude.point));
			else text = exponential_text(magnitude, 5);
		}
	} else if (order == 10) {
		const DecimalValue rounded = round_decimals(magnitude, 0);
		text = rounded.point > 11 ? exponential_text(magnitude, 5) : integer_digits(rounded);
	} else {
		text = exponential_text(magnitude, 5);
	}
	return isNegative == true ? "-" + text : text;

}

/* - - - - - - */

enum class TokenKind {
	Literal,
	Fill,
	Digit,
	DecimalPoint,
	Comma,
	Percent,
	Exponent,
	Slash,
	FixedDenominator,
	TextPlaceholder,
	General,
	DatePart,
	AmPm,
	Elapsed,
	SubSecond,
};

struct FormatToken {
	TokenKind kind = TokenKind::Literal;
	// The literal text, the fill character, the digit symbol ("0", "#", "?"),
	// the date part in lower case ("yyyy", "mm", ...), "E+" / "E-", "AM/PM"
	// or "a/p" as written, the elapsed unit ("h", "mm"), or a SubSecond's zeros.
	std::string text;
	// An "m"/"mm" date part that stands next to hours or seconds: minutes.
	bool is_minutes = false;
};

struct FormatSection {
	std::vector<FormatToken> tokens;
	std::string color_hex;
	bool has_condition = false;
	std::string condition_operator;
	double condition_value = 0.0;
	bool is_date = false;
	bool has_text_placeholder = false;
	bool has_digit = false;
};

struct ParsedFormat {
	// Empty for General (and for a code Excel would refuse).
	std::vector<FormatSection> sections;
};

void add_token(FormatSection& section, TokenKind kind, std::string text) {
	FormatToken token;
	token.kind = kind;
	token.text = std::move(text);
	section.tokens.push_back(std::move(token));
}

// Sections split at ";" outside quotes, escapes and brackets.
std::vector<std::string> split_sections(const std::string& code) {

	std::vector<std::string> sections(1);
	std::size_t index = 0;
	while (index < code.size()) {
		const char character = code[index];
		if (character == '"' || character == '[') {
			const std::size_t closing = code.find(character == '"' ? '"' : ']', index + 1);
			const std::size_t end = closing == std::string::npos ? code.size() : closing + 1;
			sections.back().append(code, index, end - index);
			index = end;
			continue;
		}
		if (character == '\\' || character == '_' || character == '*') {
			const std::size_t length = index + 1 < code.size() ? 1 + utf8_length(code[index + 1]) : 1;
			sections.back().append(code, index, std::min(length, code.size() - index));
			index += length;
			continue;
		}
		if (character == ';') {
			sections.emplace_back();
			++index;
			continue;
		}
		sections.back().push_back(character);
		++index;
	}
	return sections;

}

void apply_bracket(FormatSection& section, const std::string& content) {

	if (content.empty() == true) return;
	const std::string lowered = ascii_lower(content);

	// Elapsed time: [h], [mm], [ss] count past a day (or an hour, a minute).
	const char unit = lowered[0];
	if ((unit == 'h' || unit == 'm' || unit == 's')
			&& lowered.find_first_not_of(unit) == std::string::npos) {
		add_token(section, TokenKind::Elapsed, lowered);
		return;
	}

	static const std::unordered_map<std::string, std::string> namedColors = {
		{"black", "000000"}, {"blue", "0000FF"}, {"cyan", "00FFFF"}, {"green", "00FF00"},
		{"magenta", "FF00FF"}, {"red", "FF0000"}, {"white", "FFFFFF"}, {"yellow", "FFFF00"},
	};
	const auto namedColor = namedColors.find(lowered);
	if (namedColor != namedColors.end()) {
		section.color_hex = namedColor->second;
		return;
	}
	// [Color1]..[Color56] are the palette's entries 8..63.
	if (lowered.compare(0, 5, "color") == 0) {
		const int number = std::atoi(lowered.c_str() + 5);
		if (number >= 1 && number <= 56) section.color_hex = kIndexedPalette[static_cast<std::size_t>(number + 7)];
		return;
	}

	if (content[0] == '<' || content[0] == '>' || content[0] == '=') {
		std::size_t operatorLength = 1;
		if (content.size() > 1 && (content[1] == '=' || (content[0] == '<' && content[1] == '>'))) operatorLength = 2;
		double threshold = 0.0;
		if (parse_classic_double(content.substr(operatorLength), threshold) == true) {
			section.has_condition = true;
			section.condition_operator = content.substr(0, operatorLength);
			section.condition_value = threshold;
		}
		return;
	}

	// [$€-407] draws the symbol; [$-409] only names a locale.
	if (content[0] == '$') {
		const std::size_t dash = content.find('-', 1);
		const std::string symbol = content.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
		if (symbol.empty() == false) add_token(section, TokenKind::Literal, symbol);
		return;
	}

	// [DBNum1], [ENG], ...: nothing an en-US reader sees.

}

char date_letter(const FormatToken& token) {
	if (token.kind != TokenKind::DatePart && token.kind != TokenKind::Elapsed) return '\0';
	return token.text.empty() == true ? '\0' : token.text[0];
}

// What only a finished section can tell: whether it is a date, which "m" means
// minutes, where fractional seconds stand.
void finish_section(FormatSection& section) {

	for (const FormatToken& token : section.tokens) {
		if (token.kind == TokenKind::DatePart || token.kind == TokenKind::AmPm || token.kind == TokenKind::Elapsed) {
			section.is_date = true;
		}
		if (token.kind == TokenKind::TextPlaceholder) section.has_text_placeholder = true;
		if (token.kind == TokenKind::Digit) section.has_digit = true;
	}
	if (section.is_date == false) return;

	/* - - - - - - */

	// "m" and "mm" are minutes right after hours or right before seconds.
	std::vector<FormatToken>& tokens = section.tokens;
	for (std::size_t index = 0; index < tokens.size(); ++index) {
		FormatToken& token = tokens[index];
		if (token.kind != TokenKind::DatePart || token.text[0] != 'm' || token.text.size() > 2) continue;
		char previous = '\0';
		for (std::size_t back = index; back > 0; --back) {
			previous = date_letter(tokens[back - 1]);
			if (previous != '\0') break;
		}
		char next = '\0';
		for (std::size_t forward = index + 1; forward < tokens.size(); ++forward) {
			next = date_letter(tokens[forward]);
			if (next != '\0') break;
		}
		token.is_minutes = previous == 'h' || next == 's';
	}

	/* - - - - - - */

	// ".0", ".00", ".000" are fractional seconds; every other number sign in a
	// date section stands for itself.
	std::vector<FormatToken> finished;
	for (std::size_t index = 0; index < tokens.size(); ++index) {
		FormatToken token = tokens[index];
		if (token.kind == TokenKind::DecimalPoint) {
			std::size_t zeros = 0;
			while (index + 1 + zeros < tokens.size() && tokens[index + 1 + zeros].kind == TokenKind::Digit
					&& tokens[index + 1 + zeros].text == "0") {
				++zeros;
			}
			if (zeros > 0) {
				token.kind = TokenKind::SubSecond;
				token.text = std::string(std::min<std::size_t>(zeros, 3), '0');
				finished.push_back(token);
				index += zeros;
				continue;
			}
			token.text = ".";
		}
		if (token.kind == TokenKind::Digit || token.kind == TokenKind::DecimalPoint || token.kind == TokenKind::Percent
				|| token.kind == TokenKind::Exponent || token.kind == TokenKind::FixedDenominator) {
			token.kind = TokenKind::Literal;
		} else if (token.kind == TokenKind::Comma) {
			token.kind = TokenKind::Literal;
			token.text = ",";
		} else if (token.kind == TokenKind::Slash) {
			token.kind = TokenKind::Literal;
			token.text = "/";
		}
		finished.push_back(token);
	}
	tokens = std::move(finished);

}

FormatSection parse_section(const std::string& source) {

	FormatSection section;
	std::size_t index = 0;
	const std::size_t size = source.size();

	while (index < size) {
		const char character = source[index];
		const char lower = ascii_lower(character);

		if (character == '"') {
			const std::size_t closing = source.find('"', index + 1);
			const std::size_t end = closing == std::string::npos ? size : closing;
			add_token(section, TokenKind::Literal, source.substr(index + 1, end - index - 1));
			index = closing == std::string::npos ? size : closing + 1;
			continue;
		}
		if (character == '\\' || character == '_' || character == '*') {
			if (index + 1 >= size) {
				++index;
				continue;
			}
			const std::string next = utf8_character_at(source, index + 1);
			if (character == '\\') add_token(section, TokenKind::Literal, next);
			if (character == '_') {
				const bool isThin = next == "(" || next == ")" || next == "-";
				add_token(section, TokenKind::Literal, isThin == true ? kThinSpace : " ");
			}
			if (character == '*') add_token(section, TokenKind::Fill, next);
			index += 1 + next.size();
			continue;
		}
		if (character == '[') {
			const std::size_t closing = source.find(']', index + 1);
			const std::size_t end = closing == std::string::npos ? size : closing;
			apply_bracket(section, source.substr(index + 1, end - index - 1));
			index = closing == std::string::npos ? size : closing + 1;
			continue;
		}
		if (character == '@') {
			add_token(section, TokenKind::TextPlaceholder, "@");
			++index;
			continue;
		}
		if (character == '0' || character == '#' || character == '?') {
			add_token(section, TokenKind::Digit, std::string(1, character));
			++index;
			continue;
		}
		if (character == '.' || character == ',' || character == '%') {
			const TokenKind kind = character == '.' ? TokenKind::DecimalPoint
				: character == ',' ? TokenKind::Comma : TokenKind::Percent;
			add_token(section, kind, std::string(1, character));
			++index;
			continue;
		}
		if (lower == 'e' && index + 1 < size && (source[index + 1] == '+' || source[index + 1] == '-')) {
			add_token(section, TokenKind::Exponent, source.substr(index, 2));
			index += 2;
			continue;
		}
		if (lower == 'g' && starts_with_ignoring_case(source, index, "general") == true) {
			add_token(section, TokenKind::General, "General");
			index += 7;
			continue;
		}
		if (character == '/') {
			add_token(section, TokenKind::Slash, "/");
			++index;
			// "/8", "/100": a fixed denominator.
			if (index < size && source[index] >= '1' && source[index] <= '9') {
				std::size_t end = index;
				while (end < size && source[end] >= '0' && source[end] <= '9') ++end;
				add_token(section, TokenKind::FixedDenominator, source.substr(index, end - index));
				index = end;
			}
			continue;
		}
		if (lower == 'a' && starts_with_ignoring_case(source, index, "am/pm") == true) {
			add_token(section, TokenKind::AmPm, "AM/PM");
			index += 5;
			continue;
		}
		if (lower == 'a' && starts_with_ignoring_case(source, index, "a/p") == true) {
			add_token(section, TokenKind::AmPm, source.substr(index, 3));
			index += 3;
			continue;
		}
		if (lower == 'y' || lower == 'm' || lower == 'd' || lower == 'h' || lower == 's') {
			std::size_t end = index;
			while (end < size && ascii_lower(source[end]) == lower) ++end;
			add_token(section, TokenKind::DatePart, std::string(end - index, lower));
			index = end;
			continue;
		}
		// Everything else stands for itself: "$", "-", "+", "(", ")", ":", " ", ...
		const std::string literal = utf8_character_at(source, index);
		add_token(section, TokenKind::Literal, literal);
		index += literal.size();
	}

	finish_section(section);
	return section;

}

ParsedFormat parse_format(const std::string& code) {

	ParsedFormat format;
	if (code.empty() == true || ascii_lower(code) == "general") return format;

	const std::vector<std::string> sources = split_sections(code);
	// Excel refuses a fifth section; such a code draws General.
	if (sources.size() > 4) return format;
	for (const std::string& source : sources) format.sections.push_back(parse_section(source));
	return format;

}

// A workbook repeats a handful of codes over thousands of cells.
const ParsedFormat& parsed_format(const std::string& code) {
	thread_local std::unordered_map<std::string, ParsedFormat> cache;
	const auto found = cache.find(code);
	if (found != cache.end()) return found->second;
	if (cache.size() >= 512) cache.clear();
	return cache.emplace(code, parse_format(code)).first->second;
}

/* - - - - - - */

// Collects the drawn text on either side of the "*x" fill.
struct TextBuilder {
	std::string before;
	std::string after;
	bool has_fill = false;

	void add(const std::string& text) {
		if (has_fill == true) after += text;
		else before += text;
	}
	// Excel honours one fill per section.
	void fill() { has_fill = true; }
};

FormattedCellValue finish_text(const TextBuilder& builder, const std::string& colorHex, bool isDate) {
	FormattedCellValue result;
	result.text = builder.before + builder.after;
	result.has_fill = builder.has_fill;
	if (builder.has_fill == true) {
		result.fill_before = builder.before;
		result.fill_after = builder.after;
	}
	result.color_hex = colorHex;
	result.is_date = isDate;
	return result;
}

bool condition_holds(const FormatSection& section, double value) {
	const std::string& op = section.condition_operator;
	const double threshold = section.condition_value;
	if (op == "<") return value < threshold;
	if (op == "<=") return value <= threshold;
	if (op == ">") return value > threshold;
	if (op == ">=") return value >= threshold;
	if (op == "=") return value == threshold;
	if (op == "<>") return value != threshold;
	return false;
}

// Which section draws a number, and whether it draws the minus sign itself.
// Without conditions Excel's rule holds: one section draws every number and
// signs negatives; with two or more, the second draws negatives unsigned and a
// third draws zero. With conditions the sign is always drawn (Excel shows
// "-25" for -25 under "[<=-25]General;...").
struct SectionChoice {
	const FormatSection* section = nullptr;
	bool draws_sign = true;
};

SectionChoice choose_number_section(const ParsedFormat& format, double value) {

	std::vector<const FormatSection*> numeric;
	const std::size_t count = format.sections.size();
	for (std::size_t index = 0; index < count && index < 3; ++index) {
		const bool isTextSection = index == count - 1 && count < 4 && format.sections[index].has_text_placeholder == true;
		if (isTextSection == true) break;
		numeric.push_back(&format.sections[index]);
	}
	if (numeric.empty() == true) return SectionChoice();

	const FormatSection* first = numeric[0];
	const FormatSection* second = numeric.size() > 1 ? numeric[1] : nullptr;
	const bool isConditional = first->has_condition == true || (second != nullptr && second->has_condition == true);
	if (isConditional == true) {
		if (first->has_condition == true && condition_holds(*first, value) == true) return {first, true};
		if (second != nullptr && second->has_condition == true && condition_holds(*second, value) == true) return {second, true};
		if (first->has_condition == true && second != nullptr && second->has_condition == true) {
			return {numeric.size() > 2 ? numeric[2] : nullptr, true};
		}
		return {second != nullptr ? second : first, true};
	}

	if (numeric.size() == 1) return {first, true};
	if (value < 0.0) return {second, false};
	if (value == 0.0 && numeric.size() > 2) return {numeric[2], false};
	return {first, false};

}

/* - - - - - - */

// Right-aligned digits into integer placeholders (`positions` index `tokens`,
// left to right): "0" pads with zeros, "?" with a digit-wide space, "#" with
// nothing. Digits the placeholders cannot hold all stand at the leftmost one.
void assign_integer_digits(const std::vector<FormatToken>& tokens, const std::vector<std::size_t>& positions,
		const std::string& digits, bool isGrouped, std::vector<std::string>& assigned) {

	int digitIndex = static_cast<int>(digits.size()) - 1;
	int drawnDigits = 0;
	for (int slot = static_cast<int>(positions.size()) - 1; slot >= 0; --slot) {
		const char symbol = tokens[positions[static_cast<std::size_t>(slot)]].text[0];
		std::string piece;
		bool isDigitDrawn = true;
		if (digitIndex >= 0) {
			piece = std::string(1, digits[static_cast<std::size_t>(digitIndex)]);
			--digitIndex;
		} else if (symbol == '0') {
			piece = "0";
		} else {
			piece = symbol == '?' ? kFigureSpace : "";
			isDigitDrawn = false;
		}
		if (isDigitDrawn == true) {
			if (isGrouped == true && drawnDigits > 0 && drawnDigits % 3 == 0) piece += ",";
			++drawnDigits;
		}
		if (slot == 0) {
			while (digitIndex >= 0) {
				std::string extra(1, digits[static_cast<std::size_t>(digitIndex)]);
				--digitIndex;
				if (isGrouped == true && drawnDigits > 0 && drawnDigits % 3 == 0) extra += ",";
				++drawnDigits;
				piece = extra + piece;
			}
		}
		assigned[positions[static_cast<std::size_t>(slot)]] = piece;
	}

}

// Left-aligned digits into decimal placeholders; trailing zeros vanish under
// "#" and become a digit-wide space under "?".
void assign_fraction_digits(const std::vector<FormatToken>& tokens, const std::vector<std::size_t>& positions,
		const std::string& digits, std::vector<std::string>& assigned) {

	for (std::size_t slot = 0; slot < positions.size(); ++slot) {
		assigned[positions[slot]] = std::string(1, digits[slot]);
	}
	for (std::size_t slot = positions.size(); slot > 0; --slot) {
		const char symbol = tokens[positions[slot - 1]].text[0];
		if (digits[slot - 1] != '0' || symbol == '0') break;
		assigned[positions[slot - 1]] = symbol == '?' ? kFigureSpace : "";
	}

}

// The fraction closest to `value` with a denominator of at most
// `maximumDenominator`: continued fractions, then the last semiconvergent.
std::pair<long long, long long> closest_fraction(double value, long long maximumDenominator) {

	if (value > 1e15) return {static_cast<long long>(std::llround(value)), 1};

	long long previousNumerator = 0;
	long long previousDenominator = 1;
	long long numerator = 1;
	long long denominator = 0;
	double remainder = value;
	for (int step = 0; step < 64; ++step) {
		const double wholePart = std::floor(remainder);
		if (wholePart > 1e15) break;
		const long long term = static_cast<long long>(wholePart);
		const long long nextNumerator = term * numerator + previousNumerator;
		const long long nextDenominator = term * denominator + previousDenominator;
		if (nextDenominator > maximumDenominator) {
			const long long multiple = (maximumDenominator - previousDenominator) / denominator;
			const long long semiNumerator = previousNumerator + multiple * numerator;
			const long long semiDenominator = previousDenominator + multiple * denominator;
			const double semiError = std::fabs(value - static_cast<double>(semiNumerator) / static_cast<double>(semiDenominator));
			const double convergentError = std::fabs(value - static_cast<double>(numerator) / static_cast<double>(denominator));
			if (multiple > 0 && semiError < convergentError) return {semiNumerator, semiDenominator};
			return {numerator, denominator};
		}
		previousNumerator = numerator;
		previousDenominator = denominator;
		numerator = nextNumerator;
		denominator = nextDenominator;
		const double fractional = remainder - wholePart;
		if (fractional < 1e-12) break;
		remainder = 1.0 / fractional;
	}
	if (denominator == 0) return {0, 1};
	return {numerator, denominator};

}

// The run of Digit tokens that ends right before `end` (exclusive).
std::vector<std::size_t> digit_run_before(const std::vector<FormatToken>& tokens, std::size_t end) {
	std::vector<std::size_t> run;
	std::size_t index = end;
	while (index > 0 && tokens[index - 1].kind == TokenKind::Digit) {
		--index;
		run.insert(run.begin(), index);
	}
	return run;
}

void draw_fraction(const FormatSection& section, std::size_t slash, double magnitude, TextBuilder& builder) {

	const std::vector<FormatToken>& tokens = section.tokens;
	const std::vector<std::size_t> numeratorSlots = digit_run_before(tokens, slash);
	const std::size_t numeratorStart = numeratorSlots.empty() == true ? slash : numeratorSlots.front();

	std::vector<std::size_t> denominatorSlots;
	long long fixedDenominator = 0;
	std::size_t denominatorEnd = slash + 1;
	if (denominatorEnd < tokens.size() && tokens[denominatorEnd].kind == TokenKind::FixedDenominator) {
		fixedDenominator = std::atoll(tokens[denominatorEnd].text.c_str());
		++denominatorEnd;
	} else {
		while (denominatorEnd < tokens.size() && tokens[denominatorEnd].kind == TokenKind::Digit) {
			denominatorSlots.push_back(denominatorEnd);
			++denominatorEnd;
		}
	}

	std::vector<std::size_t> integerSlots;
	for (std::size_t index = 0; index < numeratorStart; ++index) {
		if (tokens[index].kind == TokenKind::Digit) integerSlots.push_back(index);
	}
	const bool isMixed = integerSlots.empty() == false;

	/* - - - - - - */

	double whole = isMixed == true ? std::floor(magnitude) : 0.0;
	const double part = magnitude - whole;
	long long numerator = 0;
	long long denominator = 1;
	if (fixedDenominator > 0) {
		denominator = fixedDenominator;
		numerator = std::llround(part * static_cast<double>(denominator));
	} else {
		long long maximumDenominator = 1;
		for (std::size_t digit = 0; digit < std::min<std::size_t>(denominatorSlots.size(), 7); ++digit) maximumDenominator *= 10;
		const std::pair<long long, long long> closest = closest_fraction(part, std::max(maximumDenominator - 1, 1LL));
		numerator = closest.first;
		denominator = closest.second;
	}
	if (isMixed == true && numerator == denominator) {
		whole += 1.0;
		numerator = 0;
	}

	/* - - - - - - */

	std::vector<std::string> assigned(tokens.size());
	if (isMixed == true) {
		std::string wholeDigits = integer_digits(decimal_from_magnitude(whole));
		if (wholeDigits.empty() == true && numerator == 0) wholeDigits = "0";
		assign_integer_digits(tokens, integerSlots, wholeDigits, false, assigned);
	}
	// A whole number drawn by a mixed fraction leaves the fraction blank.
	const bool isFractionBlank = isMixed == true && numerator == 0;
	const std::string numeratorDigits = std::to_string(numerator);
	const std::string denominatorDigits = std::to_string(denominator);
	assign_integer_digits(tokens, numeratorSlots, numeratorDigits, false, assigned);
	for (std::size_t slot = 0; slot < denominatorSlots.size(); ++slot) {
		const char symbol = tokens[denominatorSlots[slot]].text[0];
		std::string piece;
		if (slot < denominatorDigits.size()) {
			piece = slot + 1 == denominatorSlots.size() ? denominatorDigits.substr(slot) : std::string(1, denominatorDigits[slot]);
		} else {
			piece = symbol == '#' ? "" : kFigureSpace;
		}
		assigned[denominatorSlots[slot]] = piece;
	}

	for (std::size_t index = 0; index < tokens.size(); ++index) {
		const FormatToken& token = tokens[index];
		const bool isFractionPart = index >= numeratorStart && index < denominatorEnd;
		if (isFractionPart == true && isFractionBlank == true) {
			if (token.kind == TokenKind::Digit) builder.add(kFigureSpace);
			else builder.add(std::string(token.text.size(), ' '));
			continue;
		}
		switch (token.kind) {
			case TokenKind::Digit: builder.add(assigned[index]); break;
			case TokenKind::Fill: builder.fill(); break;
			case TokenKind::Comma: break;
			case TokenKind::TextPlaceholder: break;
			case TokenKind::General: builder.add(general_text(magnitude)); break;
			default: builder.add(token.text); break;
		}
	}

}

void draw_number(const FormatSection& section, double magnitude, TextBuilder& builder) {

	const std::vector<FormatToken>& tokens = section.tokens;

	std::size_t exponentIndex = tokens.size();
	std::size_t slashIndex = tokens.size();
	for (std::size_t index = 0; index < tokens.size(); ++index) {
		if (tokens[index].kind == TokenKind::Exponent && exponentIndex == tokens.size()) exponentIndex = index;
		if (tokens[index].kind == TokenKind::Slash && slashIndex == tokens.size()) {
			const bool hasNumerator = index > 0 && tokens[index - 1].kind == TokenKind::Digit;
			const bool hasDenominator = index + 1 < tokens.size()
				&& (tokens[index + 1].kind == TokenKind::Digit || tokens[index + 1].kind == TokenKind::FixedDenominator);
			if (hasNumerator == true && hasDenominator == true) slashIndex = index;
		}
	}
	if (exponentIndex == tokens.size() && slashIndex != tokens.size()) {
		draw_fraction(section, slashIndex, magnitude, builder);
		return;
	}

	/* - - - - - - */

	// Where the mantissa's decimal point stands, which placeholders hold its
	// integer and decimal digits, and what the commas do: a comma between
	// integer placeholders groups thousands, a comma after the last placeholder
	// divides by a thousand.
	const std::size_t mantissaEnd = exponentIndex;
	std::size_t decimalIndex = mantissaEnd;
	for (std::size_t index = 0; index < mantissaEnd; ++index) {
		if (tokens[index].kind == TokenKind::DecimalPoint) {
			decimalIndex = index;
			break;
		}
	}
	std::vector<std::size_t> integerSlots;
	std::vector<std::size_t> decimalSlots;
	for (std::size_t index = 0; index < mantissaEnd; ++index) {
		if (tokens[index].kind != TokenKind::Digit) continue;
		if (index < decimalIndex) integerSlots.push_back(index);
		else decimalSlots.push_back(index);
	}

	int percentCount = 0;
	int scalingCommas = 0;
	bool isGrouped = false;
	std::vector<bool> isLiteralComma(tokens.size(), false);
	bool hasDigitBefore = false;
	for (std::size_t index = 0; index < tokens.size(); ++index) {
		const FormatToken& token = tokens[index];
		if (token.kind == TokenKind::Digit) hasDigitBefore = true;
		if (token.kind == TokenKind::Percent) ++percentCount;
		if (token.kind != TokenKind::Comma) continue;
		if (hasDigitBefore == false) {
			isLiteralComma[index] = true;
			continue;
		}
		std::size_t next = index + 1;
		while (next < tokens.size() && tokens[next].kind == TokenKind::Comma) ++next;
		const bool isFollowedByDigit = next < tokens.size() && tokens[next].kind == TokenKind::Digit;
		if (isFollowedByDigit == true) {
			if (index < decimalIndex) isGrouped = true;
		} else {
			++scalingCommas;
		}
	}

	DecimalValue scaled = decimal_from_magnitude(magnitude);
	if (scaled.digits.empty() == false) scaled.point += 2 * percentCount - 3 * scalingCommas;

	/* - - - - - - */

	std::vector<std::string> assigned(tokens.size());
	const int decimals = static_cast<int>(decimalSlots.size());
	int exponent = 0;
	DecimalValue mantissa = scaled;
	if (exponentIndex != tokens.size() && scaled.digits.empty() == false) {
		// Scientific: the exponent is a multiple of the integer placeholder count
		// ("##0.0E+0" is engineering notation), so the mantissa holds that many
		// integer digits at most.
		const int step = integerSlots.size() > 1 ? static_cast<int>(integerSlots.size()) : 1;
		const int order = scaled.point - 1;
		exponent = (order >= 0 ? order / step : -((-order + step - 1) / step)) * step;
		mantissa.point = scaled.point - exponent;
		const DecimalValue trial = round_decimals(mantissa, decimals);
		if (trial.point > step) {
			exponent += step;
			mantissa.point -= step;
		}
	}
	const DecimalValue rounded = round_decimals(mantissa, decimals);
	const std::string integerPart = integer_digits(rounded);
	assign_integer_digits(tokens, integerSlots, integerPart, isGrouped, assigned);
	assign_fraction_digits(tokens, decimalSlots, fraction_digits(rounded, decimals), assigned);
	// A code with no integer placeholder (".00") still shows the integer part.
	if (integerSlots.empty() == true && integerPart.empty() == false && decimalIndex < mantissaEnd) {
		assigned[decimalIndex] = integerPart;
	}

	if (exponentIndex != tokens.size()) {
		std::vector<std::size_t> exponentSlots;
		for (std::size_t index = exponentIndex + 1; index < tokens.size() && tokens[index].kind == TokenKind::Digit; ++index) {
			exponentSlots.push_back(index);
		}
		assign_integer_digits(tokens, exponentSlots, std::to_string(std::abs(exponent)), false, assigned);
		const std::string& mark = tokens[exponentIndex].text;
		std::string sign;
		if (exponent < 0) sign = "-";
		else if (mark[1] == '+') sign = "+";
		assigned[exponentIndex] = std::string(1, mark[0]) + sign;
	}

	/* - - - - - - */

	for (std::size_t index = 0; index < tokens.size(); ++index) {
		const FormatToken& token = tokens[index];
		switch (token.kind) {
			case TokenKind::Digit:
			case TokenKind::Exponent:
				builder.add(assigned[index]);
				break;
			case TokenKind::DecimalPoint:
				builder.add(index == decimalIndex ? assigned[index] + "." : ".");
				break;
			case TokenKind::Comma:
				if (isLiteralComma[index] == true) builder.add(",");
				break;
			case TokenKind::Fill:
				builder.fill();
				break;
			case TokenKind::TextPlaceholder:
				break;
			case TokenKind::General:
				builder.add(general_text(magnitude));
				break;
			default:
				builder.add(token.text);
				break;
		}
	}

}

/* - - - - - - */

// Howard Hinnant's civil calendar algorithms (proleptic Gregorian): days since
// 1 January 1970 and back.
long long days_from_civil(long long year, int month, int day) {
	year -= month <= 2 ? 1 : 0;
	const long long era = (year >= 0 ? year : year - 399) / 400;
	const long long yearOfEra = year - era * 400;
	const long long dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return era * 146097 + dayOfEra - 719468;
}

void civil_from_days(long long days, long long& year, int& month, int& day) {
	days += 719468;
	const long long era = (days >= 0 ? days : days - 146096) / 146097;
	const long long dayOfEra = days - era * 146097;
	const long long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
	const long long dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
	const long long monthPrime = (5 * dayOfYear + 2) / 153;
	day = static_cast<int>(dayOfYear - (153 * monthPrime + 2) / 5 + 1);
	month = static_cast<int>(monthPrime < 10 ? monthPrime + 3 : monthPrime - 9);
	year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
}

int weekday_from_days(long long days) {
	return static_cast<int>(((days % 7) + 7 + 4) % 7); // 1 January 1970 was a Thursday; 0 is Sunday.
}

struct DateParts {
	long long days = 0;
	long long year = 1900;
	int month = 1;
	int day = 0;
	int weekday = 0;
	long long hour = 0;
	long long minute = 0;
	long long second = 0;
	long long subsecond = 0;
};

// A serial as Excel reads it. The 1900 system keeps Excel's own mistakes:
// serial 0 is "0 January 1900", serial 60 is 29 February 1900 (a day that never
// was), and the weekdays before it are one off. The 1904 system counts from
// 1 January 1904.
DateParts date_parts(double serial, bool date1904, int subsecondDigits) {

	long long scale = 1;
	for (int digit = 0; digit < subsecondDigits; ++digit) scale *= 10;
	const long long ticksPerDay = 86400LL * scale;

	DateParts parts;
	parts.days = static_cast<long long>(std::floor(serial));
	long long ticks = std::llround((serial - std::floor(serial)) * static_cast<double>(ticksPerDay));
	if (ticks >= ticksPerDay) {
		++parts.days;
		ticks -= ticksPerDay;
	}
	parts.subsecond = ticks % scale;
	const long long seconds = ticks / scale;
	parts.hour = seconds / 3600;
	parts.minute = (seconds / 60) % 60;
	parts.second = seconds % 60;

	if (date1904 == true) {
		const long long civilDays = days_from_civil(1904, 1, 1) + parts.days;
		civil_from_days(civilDays, parts.year, parts.month, parts.day);
		parts.weekday = weekday_from_days(civilDays);
		return parts;
	}
	if (parts.days == 0) {
		parts.year = 1900;
		parts.month = 1;
		parts.day = 0;
		parts.weekday = 6;
		return parts;
	}
	if (parts.days == 60) {
		parts.year = 1900;
		parts.month = 2;
		parts.day = 29;
		parts.weekday = 3;
		return parts;
	}
	const long long civilDays = days_from_civil(1899, 12, 31) + (parts.days > 60 ? parts.days - 1 : parts.days);
	civil_from_days(civilDays, parts.year, parts.month, parts.day);
	parts.weekday = weekday_from_days(civilDays);
	if (parts.days < 60) parts.weekday = (parts.weekday + 6) % 7;
	return parts;

}

void draw_date(const FormatSection& section, double serial, bool date1904, TextBuilder& builder) {

	// Excel fills a cell with "#" for a date or time it cannot show.
	if (serial < 0.0 || serial >= kLastDateSerial) {
		builder.add("########");
		return;
	}

	int subsecondDigits = 0;
	bool isTwelveHour = false;
	for (const FormatToken& token : section.tokens) {
		if (token.kind == TokenKind::SubSecond) subsecondDigits = std::max(subsecondDigits, static_cast<int>(token.text.size()));
		if (token.kind == TokenKind::AmPm) isTwelveHour = true;
	}
	const DateParts parts = date_parts(serial, date1904, subsecondDigits);

	for (const FormatToken& token : section.tokens) {
		const std::size_t count = token.text.size();
		switch (token.kind) {
			case TokenKind::DatePart: {
				const char letter = token.text[0];
				if (letter == 'y') {
					builder.add(count <= 2 ? pad_number(parts.year % 100, 2) : pad_number(parts.year, 4));
				} else if (letter == 'm' && token.is_minutes == true) {
					builder.add(pad_number(parts.minute, count >= 2 ? 2 : 1));
				} else if (letter == 'm') {
					const std::string name = kMonthNames[static_cast<std::size_t>(parts.month - 1)];
					if (count <= 2) builder.add(pad_number(parts.month, count));
					else if (count == 3) builder.add(name.substr(0, 3));
					else if (count == 5) builder.add(name.substr(0, 1));
					else builder.add(name);
				} else if (letter == 'd') {
					const std::string name = kWeekdayNames[static_cast<std::size_t>(parts.weekday)];
					if (count <= 2) builder.add(pad_number(parts.day, count));
					else if (count == 3) builder.add(name.substr(0, 3));
					else builder.add(name);
				} else if (letter == 'h') {
					long long hour = parts.hour;
					if (isTwelveHour == true) hour = hour % 12 == 0 ? 12 : hour % 12;
					builder.add(pad_number(hour, count >= 2 ? 2 : 1));
				} else if (letter == 's') {
					builder.add(pad_number(parts.second, count >= 2 ? 2 : 1));
				}
				break;
			}
			case TokenKind::Elapsed: {
				const long long hours = parts.days * 24 + parts.hour;
				const long long minutes = hours * 60 + parts.minute;
				const long long seconds = minutes * 60 + parts.second;
				const char letter = token.text[0];
				builder.add(pad_number(letter == 'h' ? hours : letter == 'm' ? minutes : seconds, count));
				break;
			}
			case TokenKind::AmPm:
				if (token.text == "AM/PM") builder.add(parts.hour < 12 ? "AM" : "PM");
				else builder.add(std::string(1, parts.hour < 12 ? token.text[0] : token.text[2]));
				break;
			case TokenKind::SubSecond:
				builder.add("." + pad_number(parts.subsecond, static_cast<std::size_t>(subsecondDigits)).substr(0, count));
				break;
			case TokenKind::Fill:
				builder.fill();
				break;
			case TokenKind::Literal:
				builder.add(token.text);
				break;
			default:
				break;
		}
	}

}

/* - - - - - - */

// ---- Typing -------------------------------------------------------------------

// The currencies an amount is typed with, as symbols and as codes.
constexpr std::array<const char*, 4> kCurrencySymbols = {"$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5"};
constexpr std::array<const char*, 3> kCurrencyCodes = {"USD", "EUR", "GBP"};

// The currency a symbol or a code names: "$" and "USD" are one currency.
std::string currency_identity(const std::string& written) {
	if (written == "$" || written == "USD") return "USD";
	if (written == "\xE2\x82\xAC" || written == "EUR") return "EUR";
	if (written == "\xC2\xA3" || written == "GBP") return "GBP";
	if (written == "\xC2\xA5") return "JPY";
	return written;
}

// The currency a section's words show ("\"$\"#,##0", "[$\xE2\x82\xAC-407]#,##0",
// "#,##0\" USD\""), "" when they show none.
std::string shown_currency(const FormatSection& section) {
	for (const FormatToken& token : section.tokens) {
		if (token.kind != TokenKind::Literal) continue;
		for (const char* symbol : kCurrencySymbols)
			if (token.text.find(symbol) != std::string::npos) return currency_identity(symbol);
		for (const char* code : kCurrencyCodes)
			if (token.text.find(code) != std::string::npos) return currency_identity(code);
	}
	return {};
}

// A decimal as a workbook stores it, from its integer and fraction digits
// with the point moved `shift` places (a percentage moves it two to the left):
// no leading zeros, no trailing fraction zeros, "0" for zero - or nothing past
// Excel's 15 significant digits.
std::optional<std::string> stored_decimal(bool isNegative, const std::string& integerDigits,
		const std::string& fractionDigits, int shift) {

	std::string digits = integerDigits + fractionDigits;
	int point = static_cast<int>(integerDigits.size()) + shift;
	const std::size_t first = digits.find_first_not_of('0');
	if (first == std::string::npos) return std::string("0");
	digits.erase(0, first);
	point -= static_cast<int>(first);
	while (digits.back() == '0') digits.pop_back();
	if (static_cast<int>(digits.size()) > 15) return std::nullopt;

	std::string text;
	if (point <= 0) {
		text = "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
	} else if (point >= static_cast<int>(digits.size())) {
		text = digits + std::string(static_cast<std::size_t>(point) - digits.size(), '0');
	} else {
		text = digits.substr(0, static_cast<std::size_t>(point)) + "." + digits.substr(static_cast<std::size_t>(point));
	}
	return isNegative == true ? "-" + text : text;

}

// "1,250,000.50", "7.5", "0.25" at `at`: its integer digits (without the
// separators), its fraction digits, whether it was grouped, and where it
// ends - or 0 when no such number stands there. A group of thousands is always
// three digits, and a number never starts with a zero it does not need.
std::size_t read_typed_digits(const std::string& text, std::size_t at, std::string& integerDigits,
		std::string& fractionDigits, bool& isGrouped) {

	integerDigits.clear();
	fractionDigits.clear();
	isGrouped = false;
	std::size_t index = at;
	while (index < text.size() && text[index] >= '0' && text[index] <= '9') integerDigits.push_back(text[index++]);
	if (integerDigits.empty() == true) return 0;
	if (integerDigits.size() > 1 && integerDigits[0] == '0') return 0;
	if (index < text.size() && text[index] == ',' && integerDigits.size() > 3) return 0;
	while (index < text.size() && text[index] == ',') {
		std::size_t digits = 0;
		while (index + 1 + digits < text.size() && text[index + 1 + digits] >= '0' && text[index + 1 + digits] <= '9') ++digits;
		if (digits != 3) return 0;
		integerDigits += text.substr(index + 1, 3);
		isGrouped = true;
		index += 4;
	}
	if (index < text.size() && text[index] == '.') {
		++index;
		while (index < text.size() && text[index] >= '0' && text[index] <= '9') fractionDigits.push_back(text[index++]);
		if (fractionDigits.empty() == true) return 0;
	}
	return index;

}

std::string zeros_after_point(std::size_t count) {
	return count == 0 ? std::string() : "." + std::string(count, '0');
}

// An amount, a percentage or grouped digits, as Excel reads them typed.
std::optional<TypedCellValue> typed_amount(const std::string& text) {

	std::string body = text;
	const bool isParenthesized = body.size() > 2 && body.front() == '(' && body.back() == ')';
	if (isParenthesized == true) body = body.substr(1, body.size() - 2);

	std::size_t at = 0;
	bool isNegative = false;
	if (at < body.size() && body[at] == '-') {
		isNegative = true;
		++at;
	}
	std::string currency;
	bool isCode = false;
	bool isSpaced = false;
	for (const char* symbol : kCurrencySymbols) {
		if (body.compare(at, std::strlen(symbol), symbol) == 0) {
			currency = symbol;
			at += currency.size();
			break;
		}
	}
	if (currency.empty() == true) {
		for (const char* code : kCurrencyCodes) {
			if (body.compare(at, 3, code) == 0 && at + 3 < body.size() && body[at + 3] == ' ') {
				currency = code;
				isCode = true;
				at += 4;
				break;
			}
		}
	}
	const bool isPrefixed = currency.empty() == false;
	if (isPrefixed == true && isNegative == false && at < body.size() && body[at] == '-') {
		isNegative = true;
		++at;
	}

	std::string integerDigits;
	std::string fractionDigits;
	bool isGrouped = false;
	const std::size_t end = read_typed_digits(body, at, integerDigits, fractionDigits, isGrouped);
	if (end == 0) return std::nullopt;
	at = end;

	bool isPercent = false;
	if (at < body.size() && body[at] == '%') {
		isPercent = true;
		++at;
	} else if (isPrefixed == false && at < body.size()) {
		// A currency written after the amount: "1,250 USD", "1,250 €", "1,250€".
		std::size_t cursor = at;
		if (body[cursor] == ' ') {
			isSpaced = true;
			++cursor;
		}
		const std::string rest = body.substr(cursor);
		for (const char* symbol : kCurrencySymbols) if (rest == symbol) currency = symbol;
		for (const char* code : kCurrencyCodes) {
			if (rest == code && isSpaced == true) {
				currency = code;
				isCode = true;
			}
		}
		if (currency.empty() == false) at = body.size();
	}
	if (at != body.size()) return std::nullopt;
	if (isParenthesized == true && (isNegative == true || isPercent == true)) return std::nullopt;
	if (isPercent == true && currency.empty() == false) return std::nullopt;
	if (isPercent == false && currency.empty() == true && isGrouped == false) return std::nullopt;

	const std::optional<std::string> stored =
		stored_decimal(isNegative || isParenthesized, integerDigits, fractionDigits, isPercent == true ? -2 : 0);
	if (stored.has_value() == false) return std::nullopt;

	TypedCellValue value;
	value.stored = *stored;
	if (parse_classic_double(value.stored, value.number) == false) return std::nullopt;
	const std::string digitsCode = (isGrouped == true || currency.empty() == false ? "#,##0" : "0")
		+ zeros_after_point(fractionDigits.size());
	if (isPercent == true) {
		value.kind = TypedCellKind::Percent;
		value.format_code = digitsCode + "%";
	} else if (currency.empty() == false) {
		value.kind = TypedCellKind::Currency;
		value.currency = currency;
		if (isPrefixed == true) value.format_code = "\"" + currency + (isCode == true ? " " : "") + "\"" + digitsCode;
		else value.format_code = digitsCode + "\"" + (isSpaced == true ? " " : "") + currency + "\"";
	} else {
		value.kind = TypedCellKind::Grouped;
		value.format_code = digitsCode;
	}
	// "(1,250)" is how its writer shows a negative amount: in brackets.
	if (isParenthesized == true) value.format_code += ";(" + value.format_code + ")";
	return value;

}

// A month by its name or its abbreviation ("Dec", "Sept"), 1-12; 0 for none.
int month_from_name(const std::string& word, bool& isFullName) {
	const std::string lowered = ascii_lower(word);
	for (std::size_t index = 0; index < kMonthNames.size(); ++index) {
		const std::string name = ascii_lower(std::string(kMonthNames[index]));
		isFullName = lowered == name;
		if (isFullName == true || lowered == name.substr(0, 3) || (index == 8 && lowered == "sept")) {
			return static_cast<int>(index) + 1;
		}
	}
	isFullName = false;
	return 0;
}

// A whole calendar date as Excel reads it typed, written the way its writer
// wrote it: the words and numbers of the date become the format's date parts
// and everything between them stays as it stood.
std::optional<TypedCellValue> typed_date(const std::string& text, bool date1904) {

	struct Part {
		bool is_word = false;
		std::string text;
		std::string before;
	};
	std::vector<Part> parts;
	std::string between;
	std::size_t index = 0;
	while (index < text.size()) {
		const char character = text[index];
		const bool isDigit = character >= '0' && character <= '9';
		const bool isLetter = std::isalpha(static_cast<unsigned char>(character)) != 0;
		if (isDigit == false && isLetter == false) {
			between.push_back(character);
			++index;
			continue;
		}
		Part part;
		part.is_word = isLetter;
		part.before = between;
		between.clear();
		while (index < text.size() && (isLetter == true ? std::isalpha(static_cast<unsigned char>(text[index])) != 0
				: text[index] >= '0' && text[index] <= '9')) {
			part.text.push_back(text[index++]);
		}
		parts.push_back(part);
	}
	if (between.empty() == false || parts.empty() == true) return std::nullopt;

	// "Monday, December 15, 2025": the weekday leads.
	std::string weekdayCode;
	int weekday = -1;
	if (parts.size() >= 4 && parts[0].is_word == true && parts[1].is_word == true) {
		const std::string lowered = ascii_lower(parts[0].text);
		for (std::size_t at = 0; at < kWeekdayNames.size(); ++at) {
			const std::string name = ascii_lower(std::string(kWeekdayNames[at]));
			if (lowered == name) weekdayCode = "dddd";
			else if (lowered == name.substr(0, 3)) weekdayCode = "ddd";
			else continue;
			weekday = static_cast<int>(at);
		}
		if (weekdayCode.empty() == true || (parts[1].before != ", " && parts[1].before != " ")) return std::nullopt;
		weekdayCode += parts[1].before;
		parts[1].before.clear();
		parts.erase(parts.begin());
	}
	// "15th": the ordinal's letters say nothing a date part does not.
	for (std::size_t at = 0; at + 1 < parts.size(); ++at) {
		const std::string suffix = ascii_lower(parts[at + 1].text);
		const bool isOrdinal = parts[at].is_word == false && parts[at + 1].is_word == true
			&& parts[at + 1].before.empty() == true && (suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th");
		if (isOrdinal == true) {
			parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(at) + 1);
			break;
		}
	}
	if (parts.size() != 3 || parts[0].before.empty() == false) return std::nullopt;
	for (std::size_t at = 1; at < parts.size(); ++at) {
		static const std::array<const char*, 7> separators = {" ", ", ", ",", "/", "-", ".", ". "};
		bool isSeparator = false;
		for (const char* separator : separators) if (parts[at].before == separator) isSeparator = true;
		if (isSeparator == false) return std::nullopt;
	}

	int year = 0;
	int month = 0;
	int day = 0;
	// Which part is which, and its code.
	std::array<std::string, 3> codes;
	const auto number = [](const Part& part) { return part.text.size() <= 4 ? std::atoi(part.text.c_str()) : -1; };
	const auto dayCode = [](const Part& part) { return part.text.size() == 2 && part.text[0] == '0' ? "dd" : "d"; };
	const auto yearOf = [&](const Part& part, std::string& code) {
		if (part.is_word == true) return -1;
		if (part.text.size() == 4) {
			code = "yyyy";
			return number(part);
		}
		if (part.text.size() != 2) return -1;
		// Excel's own century window for a two-digit year.
		code = "yy";
		const int shortYear = number(part);
		return shortYear < 30 ? 2000 + shortYear : 1900 + shortYear;
	};
	std::size_t words = 0;
	for (const Part& part : parts) words += part.is_word == true ? 1 : 0;
	if (words > 1 || parts[2].is_word == true) return std::nullopt;
	if (words == 1) {
		bool isFullName = false;
		const std::size_t monthAt = parts[0].is_word == true ? 0 : 1;
		const std::size_t dayAt = monthAt == 0 ? 1 : 0;
		month = month_from_name(parts[monthAt].text, isFullName);
		if (month == 0 || parts[dayAt].text.size() > 2) return std::nullopt;
		day = number(parts[dayAt]);
		codes[monthAt] = isFullName == true ? "mmmm" : "mmm";
		codes[dayAt] = dayCode(parts[dayAt]);
		year = yearOf(parts[2], codes[2]);
	} else {
		const std::string separator = parts[1].before;
		if (separator != parts[2].before || (separator != "/" && separator != "-" && separator != ".")) return std::nullopt;
		// A two-digit year is read only after slashes ("12/15/25"): "9.2.25" is
		// far more often a clause than a date.
		if (parts[0].text.size() != 4 && parts[2].text.size() != 4 && separator != "/") return std::nullopt;
		// Month and day written with their leading zeros ("01/05/2026"), or in
		// the forms that always write them ("2025-12-15", "15.12.2025"), keep
		// them; "12/15/2025" is Excel's own m/d/yyyy.
		const bool isYearFirst = parts[0].text.size() == 4 && separator != ".";
		const Part& monthDayFirst = parts[isYearFirst == true ? 1 : 0];
		const Part& monthDaySecond = parts[isYearFirst == true ? 2 : 1];
		const bool isPadded = (monthDayFirst.text.size() == 2 && monthDayFirst.text[0] == '0')
			|| (monthDaySecond.text.size() == 2 && monthDaySecond.text[0] == '0')
			|| ((isYearFirst == true || separator == ".") && monthDayFirst.text.size() == 2 && monthDaySecond.text.size() == 2);
		const auto monthCode = [&](const Part&) { return isPadded == true ? "mm" : "m"; };
		const auto paddedDayCode = [&](const Part&) { return isPadded == true ? "dd" : "d"; };
		if (isYearFirst == true) {
			// "2025-12-15": the year first, then the month, then the day.
			year = number(parts[0]);
			codes[0] = "yyyy";
			month = parts[1].text.size() <= 2 ? number(parts[1]) : 0;
			codes[1] = monthCode(parts[1]);
			day = parts[2].text.size() <= 2 ? number(parts[2]) : 0;
			codes[2] = paddedDayCode(parts[2]);
		} else {
			if (parts[0].text.size() > 2 || parts[1].text.size() > 2) return std::nullopt;
			const int first = number(parts[0]);
			const int second = number(parts[1]);
			// A dotted date puts the day first, as everywhere it is written so;
			// otherwise the month always does (en-US). "13/12/2025" is no date
			// to Excel (en-US) and stays text: reading it day-first while
			// "1/2/2025" reads month-first would store two orders side by side,
			// each cell showing what was typed and one of them wrong.
			const bool isDayFirst = separator == ".";
			month = isDayFirst == true ? second : first;
			day = isDayFirst == true ? first : second;
			codes[isDayFirst == true ? 1 : 0] = monthCode(parts[isDayFirst == true ? 1 : 0]);
			codes[isDayFirst == true ? 0 : 1] = paddedDayCode(parts[isDayFirst == true ? 0 : 1]);
			year = yearOf(parts[2], codes[2]);
		}
	}
	if (year < (date1904 == true ? 1904 : 1900) || year > 9999 || month < 1 || month > 12 || day < 1) return std::nullopt;
	// 31 February is no date: the calendar gives it back as another day.
	long long checkedYear = 0;
	int checkedMonth = 0;
	int checkedDay = 0;
	civil_from_days(days_from_civil(year, month, day), checkedYear, checkedMonth, checkedDay);
	if (checkedYear != year || checkedMonth != month || checkedDay != day) return std::nullopt;
	// "Friday, December 15, 2025" names two different days: no date at all.
	if (weekday >= 0 && weekday_from_days(days_from_civil(year, month, day)) != weekday) return std::nullopt;
	const std::optional<double> serial = excel_date_serial(year, month, day, 0.0, date1904);
	if (serial.has_value() == false) return std::nullopt;

	TypedCellValue value;
	value.kind = TypedCellKind::Date;
	value.number = *serial;
	value.stored = std::to_string(static_cast<long long>(*serial));
	value.format_code = weekdayCode;
	for (std::size_t at = 0; at < parts.size(); ++at) value.format_code += (at == 0 ? std::string() : parts[at].before) + codes[at];
	return value;

}

} // namespace

FormattedCellValue format_cell_value(double value, const std::string& format_code, bool date1904) {

	if (std::isfinite(value) == false) {
		FormattedCellValue result;
		result.text = "#NUM!";
		return result;
	}

	const ParsedFormat& format = parsed_format(format_code);
	const SectionChoice choice = choose_number_section(format, value);
	const FormatSection* section = choice.section;
	TextBuilder builder;
	if (section == nullptr) {
		builder.add(general_text(value));
		return finish_text(builder, std::string(), false);
	}

	const double magnitude = std::fabs(value);
	// A section with nothing but a colour or a condition draws General; a truly
	// empty section ("0;;0") hides the value.
	if (section->tokens.empty() == true) {
		const bool isGeneral = section->color_hex.empty() == false || section->has_condition == true;
		if (isGeneral == true) builder.add(general_text(choice.draws_sign == true ? value : magnitude));
		return finish_text(builder, section->color_hex, false);
	}

	if (section->is_date == true) {
		draw_date(*section, choice.draws_sign == true ? value : magnitude, date1904, builder);
		return finish_text(builder, section->color_hex, true);
	}

	// Excel puts the minus of a one-section format in front of everything the
	// section draws ("-$5.00", "-Total: 5").
	if (choice.draws_sign == true && value < 0.0) builder.add("-");
	draw_number(*section, magnitude, builder);
	return finish_text(builder, section->color_hex, false);

}

FormattedCellValue format_cell_text(const std::string& text, const std::string& format_code) {

	const ParsedFormat& format = parsed_format(format_code);
	const std::size_t count = format.sections.size();
	const FormatSection* section = nullptr;
	if (count == 4) section = &format.sections[3];
	else if (count > 0 && format.sections[count - 1].has_text_placeholder == true) section = &format.sections[count - 1];

	TextBuilder builder;
	if (section == nullptr) {
		builder.add(text);
		return finish_text(builder, std::string(), false);
	}
	for (const FormatToken& token : section->tokens) {
		if (token.kind == TokenKind::TextPlaceholder) builder.add(text);
		else if (token.kind == TokenKind::Literal) builder.add(token.text);
		else if (token.kind == TokenKind::Fill) builder.fill();
	}
	return finish_text(builder, section->color_hex, false);

}

std::string builtin_number_format(unsigned int format_id) {

	switch (format_id) {
		case 0: return "General";
		case 1: return "0";
		case 2: return "0.00";
		case 3: return "#,##0";
		case 4: return "#,##0.00";
		case 5: return "\"$\"#,##0_);\\(\"$\"#,##0\\)";
		case 6: return "\"$\"#,##0_);[Red]\\(\"$\"#,##0\\)";
		case 7: return "\"$\"#,##0.00_);\\(\"$\"#,##0.00\\)";
		case 8: return "\"$\"#,##0.00_);[Red]\\(\"$\"#,##0.00\\)";
		case 9: return "0%";
		case 10: return "0.00%";
		case 11: return "0.00E+00";
		case 12: return "# ?/?";
		case 13: return "# \?\?/\?\?";
		case 14: return "m/d/yyyy";
		case 15: return "d-mmm-yy";
		case 16: return "d-mmm";
		case 17: return "mmm-yy";
		case 18: return "h:mm AM/PM";
		case 19: return "h:mm:ss AM/PM";
		case 20: return "h:mm";
		case 21: return "h:mm:ss";
		case 22: return "m/d/yyyy h:mm";
		case 37: return "#,##0_);\\(#,##0\\)";
		case 38: return "#,##0_);[Red]\\(#,##0\\)";
		case 39: return "#,##0.00_);\\(#,##0.00\\)";
		case 40: return "#,##0.00_);[Red]\\(#,##0.00\\)";
		case 41: return "_(* #,##0_);_(* \\(#,##0\\);_(* \"-\"_);_(@_)";
		case 42: return "_(\"$\"* #,##0_);_(\"$\"* \\(#,##0\\);_(\"$\"* \"-\"_);_(@_)";
		case 43: return "_(* #,##0.00_);_(* \\(#,##0.00\\);_(* \"-\"??_);_(@_)";
		case 44: return "_(\"$\"* #,##0.00_);_(\"$\"* \\(#,##0.00\\);_(\"$\"* \"-\"??_);_(@_)";
		case 45: return "mm:ss";
		case 46: return "[h]:mm:ss";
		case 47: return "mmss.0";
		case 48: return "##0.0E+0";
		case 49: return "@";
		default: break;
	}
	// Ids reserved for East Asian locales: the format en-US Excel shows for
	// them (the same mapping SheetJS SSF uses).
	if ((format_id >= 27 && format_id <= 31) || (format_id >= 50 && format_id <= 58)) return builtin_number_format(14);
	switch (format_id) {
		case 59: return builtin_number_format(1);
		case 60: return builtin_number_format(2);
		case 61: return builtin_number_format(3);
		case 62: return builtin_number_format(4);
		case 67: return builtin_number_format(9);
		case 68: return builtin_number_format(10);
		case 69: return builtin_number_format(12);
		case 70: return builtin_number_format(13);
		case 71: return builtin_number_format(14);
		case 72: return builtin_number_format(14);
		case 73: return builtin_number_format(15);
		case 74: return builtin_number_format(16);
		case 75: return builtin_number_format(17);
		case 76: return builtin_number_format(20);
		case 77: return builtin_number_format(21);
		case 78: return builtin_number_format(22);
		case 79: return builtin_number_format(45);
		case 80: return builtin_number_format(46);
		case 81: return builtin_number_format(47);
		default: break;
	}
	return "General";

}

std::string excel_indexed_color(unsigned int index) {
	if (index >= kIndexedPalette.size()) return std::string();
	return kIndexedPalette[index];
}

std::optional<double> excel_date_serial(int year, int month, int day, double seconds_of_day, bool date1904) {

	if (month < 1 || month > 12 || day < 1 || day > 31 || seconds_of_day < 0.0 || seconds_of_day >= 86400.0) {
		return std::nullopt;
	}
	const long long days = days_from_civil(year, month, day);
	long long wholeDays = 0;
	if (date1904 == true) {
		wholeDays = days - days_from_civil(1904, 1, 1);
	} else {
		// From 1 March 1900 on, Excel's serials run one ahead of the calendar:
		// they count the 29 February 1900 that never was.
		const bool isAfterFictionalLeapDay = days >= days_from_civil(1900, 3, 1);
		wholeDays = days - days_from_civil(1899, 12, isAfterFictionalLeapDay == true ? 30 : 31);
	}
	const double serial = static_cast<double>(wholeDays) + seconds_of_day / 86400.0;
	if (serial < 0.0 || serial >= kLastDateSerial) return std::nullopt;
	return serial;

}

bool is_date_number_format(const std::string& format_code) {
	const ParsedFormat& format = parsed_format(format_code);
	return format.sections.empty() == false && format.sections[0].is_date == true;
}

std::optional<TypedCellValue> read_typed_cell_value(const std::string& text, bool date1904) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::nullopt;
	const std::string typed = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
	if (typed.size() > 64) return std::nullopt;
	const std::optional<TypedCellValue> date = typed_date(typed, date1904);
	if (date.has_value() == true) return date;
	return typed_amount(typed);
}

TypedValueFit typed_value_fit(TypedCellKind kind, const std::string& format_code) {

	const ParsedFormat& format = parsed_format(format_code);
	if (format.sections.empty() == true) return TypedValueFit::TakesTypedFormat;
	const FormatSection& section = format.sections[0];
	bool isGeneral = false;
	bool isPercent = false;
	for (const FormatToken& token : section.tokens) {
		if (token.kind == TokenKind::General) isGeneral = true;
		if (token.kind == TokenKind::Percent) isPercent = true;
	}
	if (section.is_date == true) {
		if (kind != TypedCellKind::Date) return TypedValueFit::Conflicts;
		// A format that shows only a time ("h:mm") would show the date as
		// 0:00: the cell takes a date format, as the date is typed.
		for (const FormatToken& token : section.tokens) {
			if (token.kind != TokenKind::DatePart || token.is_minutes == true) continue;
			const char letter = token.text.empty() == true ? '\0' : token.text[0];
			if (letter == 'd' || letter == 'm' || letter == 'y' || letter == 'e' || letter == 'b' || letter == 'g') {
				return TypedValueFit::Fits;
			}
		}
		return TypedValueFit::TakesTypedFormat;
	}
	if (isGeneral == true && section.has_digit == false) return TypedValueFit::TakesTypedFormat;
	// "@" shows text, and a code of words alone shows no number at all.
	if (section.has_digit == false) return TypedValueFit::Conflicts;
	if (isPercent == true) return kind == TypedCellKind::Percent ? TypedValueFit::Fits : TypedValueFit::Conflicts;
	return kind == TypedCellKind::Date || kind == TypedCellKind::Percent ? TypedValueFit::Conflicts : TypedValueFit::Fits;

}

TypedValueFit typed_value_fit(const TypedCellValue& value, const std::string& format_code) {

	const TypedValueFit fit = typed_value_fit(value.kind, format_code);
	if (fit != TypedValueFit::Fits || value.kind != TypedCellKind::Currency) return fit;
	// An amount cell of another currency, or of none, would draw these digits
	// as an amount they are not.
	const ParsedFormat& format = parsed_format(format_code);
	return shown_currency(format.sections[0]) == currency_identity(value.currency) ? TypedValueFit::Fits
		: TypedValueFit::TakesTypedFormat;

}

bool is_text_number_format(const std::string& format_code) {
	const ParsedFormat& format = parsed_format(format_code);
	if (format.sections.empty() == true) return false;
	const FormatSection& section = format.sections[0];
	return section.has_text_placeholder == true && section.has_digit == false && section.is_date == false;
}

} // namespace singularity::documents
