#include "DocumentIntelligence/MaterialFacts/MaterialFacts.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <set>

namespace singularity::document_intelligence {
namespace {

namespace semantic = protected_api::papinian::semantic;

std::string lowered(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

std::string trimmed(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool isLetter(char character) {
	return std::isalpha(static_cast<unsigned char>(character)) != 0;
}

bool isDigit(char character) {
	return std::isdigit(static_cast<unsigned char>(character)) != 0;
}

// ---- Reading values ---------------------------------------------------------------

const std::string kMonths = "(?:January|February|March|April|May|June|July|August|September|October|November|December"
	"|Jan|Feb|Mar|Apr|Jun|Jul|Aug|Sept|Sep|Oct|Nov|Dec)";
const std::string kNumberWord = "(?:zero|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen"
	"|fifteen|sixteen|seventeen|eighteen|nineteen|twenty|thirty|forty|fifty|sixty|seventy|eighty|ninety|hundred|thousand)";
const std::string kNumberWords = kNumberWord + "(?:[- ]" + kNumberWord + ")*";

// A span the scan found, before it is read as a value.
struct Span {
	std::size_t start = 0;
	std::size_t length = 0;
	// "date", "money", "percent", "period", "multiple", "reference", "bare".
	std::string shape;
	std::smatch::string_type text;
	// A reference's kind word and number, an item's number.
	std::string referenceKind;
	std::string referenceNumber;
	std::string itemNumber;
};

const std::vector<std::pair<std::string, std::regex>>& patterns() {
	static const std::vector<std::pair<std::string, std::regex>> all = [] {
		const auto flags = std::regex::ECMAScript | std::regex::icase;
		std::vector<std::pair<std::string, std::regex>> made;
		// Two days of one month: "December 1 to 8, 2026" gives December 1 and
		// December 8, 2026.
		made.push_back({"dates", std::regex("\\b" + kMonths + "\\.?\\s+\\d{1,2}(?:st|nd|rd|th)?\\s*(?:to|through|and|-|\xE2\x80\x93)"
			"\\s*\\d{1,2}(?:st|nd|rd|th)?,?\\s+\\d{4}\\b", flags)});
		made.push_back({"date", std::regex("\\b" + kMonths + "\\.?\\s+\\d{1,2}(?:st|nd|rd|th)?,?\\s+\\d{4}\\b", flags)});
		made.push_back({"date", std::regex("\\b\\d{1,2}(?:st|nd|rd|th)?\\s+" + kMonths + "\\.?,?\\s+\\d{4}\\b", flags)});
		made.push_back({"date", std::regex("\\b\\d{4}-\\d{2}-\\d{2}\\b", flags)});
		made.push_back({"date", std::regex("\\b\\d{1,2}/\\d{1,2}/\\d{4}\\b", flags)});
		made.push_back({"date", std::regex("\\b" + kMonths + "\\s+\\d{4}\\b", flags)});
		made.push_back({"money", std::regex("(?:US\\$|USD|EUR|GBP|\\$|\xE2\x82\xAC|\xC2\xA3)\\s?\\d+(?:,\\d{3})*(?:\\.\\d+)?"
			"(?:\\s?(?:million|billion|thousand|mn|bn|m|k)\\b)?", flags)});
		made.push_back({"money", std::regex("\\b\\d+(?:,\\d{3})*(?:\\.\\d+)?\\s?(?:million|billion|thousand)?\\s?"
			"(?:dollars|USD|EUR|euros|GBP|pounds)\\b", flags)});
		made.push_back({"percent", std::regex("\\b\\d+(?:\\.\\d+)?\\s?(?:%|percent\\b|per cent\\b)", flags)});
		made.push_back({"period", std::regex("\\b(?:" + kNumberWords + "(?:\\s*\\(\\d+(?:\\.\\d+)?\\))?|\\d+(?:\\.\\d+)?)"
			"(?:\\s+|\\s*-\\s*)(?:(?:business|calendar|working)\\s+)?(?:days?|weeks?|months?|years?)\\b", flags)});
		made.push_back({"multiple", std::regex("\\b(?:" + kNumberWords + "(?:\\s*\\(\\d+(?:\\.\\d+)?\\))?|\\d+(?:\\.\\d+)?)"
			"\\s*(?:x|times)\\b", flags)});
		return made;
	}();
	return all;
}

const std::regex& referencePattern() {
	static const std::regex reference("\\b(Sections?|Clauses?|Articles?|Paragraphs?|Schedules?|Exhibits?|Annex(?:es)?|"
		"Appendix|Appendices|Items?)\\s+(\\d{1,3}(?:\\.\\d{1,3}){0,3}(?:\\([a-z0-9]{1,4}\\))*|[A-Z](?![A-Za-z]))"
		"(?:,?\\s+items?\\s+(\\d{1,3}))?", std::regex::ECMAScript | std::regex::icase);
	return reference;
}

// "12.2" in "Supply Agreement 12.2; Schedule 1", "(17.3)" in "allowed
// (17.3)": a clause a cell cites by its number alone.
const std::regex& bareReferencePattern() {
	static const std::regex bare("(?:^|[^0-9.$,])(\\d{1,2}\\.\\d{1,2}(?:\\.\\d{1,2})?(?:\\([a-z0-9]{1,4}\\))?)(?![0-9.,%])",
		std::regex::ECMAScript | std::regex::icase);
	return bare;
}

std::string referenceKindOf(std::string word) {
	word = lowered(word);
	if (word.size() > 1 && word.back() == 's' && word != "annexes") word.pop_back();
	if (word == "annexe" || word == "annexes") word = "annex";
	if (word == "appendice") word = "appendix";
	if (word == "clause" || word == "article" || word == "paragraph") return "section";
	return word;
}

std::vector<Span> spansIn(const std::string& text, bool isLabelled) {

	std::vector<Span> found;
	for (const auto& [shape, pattern] : patterns()) {
		for (auto match = std::sregex_iterator(text.begin(), text.end(), pattern); match != std::sregex_iterator(); ++match) {
			Span span;
			span.start = static_cast<std::size_t>(match->position(0));
			span.length = static_cast<std::size_t>(match->length(0));
			span.shape = shape;
			span.text = match->str(0);
			found.push_back(span);
		}
	}
	for (auto match = std::sregex_iterator(text.begin(), text.end(), referencePattern()); match != std::sregex_iterator(); ++match) {
		const std::string kind = referenceKindOf((*match)[1].str());
		Span span;
		span.start = static_cast<std::size_t>(match->position(0));
		span.length = static_cast<std::size_t>(match->length(0));
		span.shape = "reference";
		span.text = match->str(0);
		span.referenceKind = kind;
		span.referenceNumber = lowered((*match)[2].str());
		span.itemNumber = (*match)[3].matched == true ? (*match)[3].str() : std::string();
		// "Items 1 and 2" alone names items of nothing it says.
		if (kind == "item") continue;
		found.push_back(span);
	}
	if (isLabelled == true) {
		for (auto match = std::sregex_iterator(text.begin(), text.end(), bareReferencePattern()); match != std::sregex_iterator(); ++match) {
			Span span;
			span.start = static_cast<std::size_t>(match->position(1));
			span.length = static_cast<std::size_t>(match->length(1));
			span.shape = "reference";
			span.text = (*match)[1].str();
			span.referenceKind = "section";
			span.referenceNumber = lowered((*match)[1].str());
			found.push_back(span);
		}
	}
	// One reading of each stretch of text: the longer span first, then the one
	// that starts first; a bare clause number never inside a value.
	std::sort(found.begin(), found.end(), [](const Span& left, const Span& right) {
		if (left.start != right.start) return left.start < right.start;
		return left.length > right.length;
	});
	std::vector<Span> kept;
	for (const Span& span : found) {
		bool isInside = false;
		for (Span& earlier : kept) {
			const bool overlaps = span.start < earlier.start + earlier.length && earlier.start < span.start + span.length;
			if (overlaps == false) continue;
			// A longer span that starts later takes the place of a shorter one:
			// "sixty (60) days" over "60 days".
			if (span.length > earlier.length && earlier.shape != "reference") {
				earlier = span;
			}
			isInside = true;
			break;
		}
		if (isInside == false) kept.push_back(span);
	}
	std::sort(kept.begin(), kept.end(), [](const Span& left, const Span& right) { return left.start < right.start; });
	kept.erase(std::unique(kept.begin(), kept.end(), [](const Span& left, const Span& right) {
		return left.start == right.start && left.length == right.length;
	}), kept.end());
	return kept;

}

std::optional<semantic::Rational> countIn(const std::string& words) {
	// "sixty (60)": the figures in brackets; else the figures; else the words.
	static const std::regex bracketed("\\((\\d+(?:\\.\\d+)?)\\)");
	static const std::regex figures("(\\d+(?:\\.\\d+)?)");
	std::smatch found;
	if (std::regex_search(words, found, bracketed)) return semantic::Rational::parse_decimal(found[1].str());
	if (std::regex_search(words, found, figures)) return semantic::Rational::parse_decimal(found[1].str());
	// "one year", "twelve months", "two times": the number's own words.
	static const std::regex units("\\b(?:business|calendar|working|days?|weeks?|months?|years?|times|x)\\b",
		std::regex::ECMAScript | std::regex::icase);
	std::string spoken;
	for (const char character : std::regex_replace(lowered(words), units, " ")) spoken.push_back(character == '-' ? ' ' : character);
	return semantic::parse_number_words(trimmed(spoken));
}

std::string kindOf(const semantic::Value& value) {
	switch (value.kind) {
		case semantic::Kind::Money: return "money";
		case semantic::Kind::Number: return "number";
		case semantic::Kind::Count: return "number";
		case semantic::Kind::Percentage: return "percent";
		case semantic::Kind::Duration: return "period";
		case semantic::Kind::Date: return "date";
		default: return std::string();
	}
}

std::optional<StatedValue> periodOf(const Span& span) {
	const std::string words = lowered(span.text);
	semantic::DurationUnit unit = semantic::DurationUnit::Day;
	std::string unitName = "Day";
	if (words.find("business") != std::string::npos || words.find("working") != std::string::npos) {
		unit = semantic::DurationUnit::BusinessDay;
		unitName = "BusinessDay";
	} else if (words.find("week") != std::string::npos) {
		unit = semantic::DurationUnit::Week;
		unitName = "Week";
	} else if (words.find("month") != std::string::npos) {
		unit = semantic::DurationUnit::Month;
		unitName = "Month";
	} else if (words.find("year") != std::string::npos) {
		unit = semantic::DurationUnit::Year;
		unitName = "Year";
	}
	const std::optional<semantic::Rational> count = countIn(span.text);
	if (count.has_value() == false) return std::nullopt;
	StatedValue value;
	value.kind = "period";
	value.key = semantic::value_key(semantic::make_duration(*count, unit));
	(void)unitName;
	return value;
}

std::optional<StatedValue> readSpan(const Span& span) {

	StatedValue value;
	value.text = span.text;
	value.start = span.start;
	value.length = span.length;
	if (span.shape == "reference") {
		value.kind = "reference";
		value.key = "REF:" + span.referenceKind + ":" + span.referenceNumber
			+ (span.itemNumber.empty() == true ? std::string() : ":item:" + span.itemNumber);
		return value;
	}
	if (span.shape == "multiple") {
		const std::optional<semantic::Rational> count = countIn(span.text);
		if (count.has_value() == false) return std::nullopt;
		value.kind = "number";
		value.key = "MULT:" + count->to_string();
		return value;
	}
	if (span.shape == "period") {
		std::optional<StatedValue> period = periodOf(span);
		if (period.has_value() == false) return std::nullopt;
		period->text = value.text;
		period->start = value.start;
		period->length = value.length;
		return period;
	}
	try {
		const std::optional<semantic::Value> read = semantic::parse_value(span.text);
		if (read.has_value() == false) return std::nullopt;
		value.kind = kindOf(*read);
		if (value.kind.empty() == true) return std::nullopt;
		value.key = semantic::value_key(*read);
		return value;
	} catch (const std::exception&) {
		return std::nullopt;
	}

}

// The two dates a span of days of one month gives: "December 1 to 8, 2026" is
// December 1, 2026 and December 8, 2026, each as Papinian Guard reads a date.
std::vector<StatedValue> datesOfRange(const Span& span) {
	std::vector<StatedValue> dates;
	static const std::regex parts("^(" + kMonths + ")\\.?\\s+(\\d{1,2})(?:st|nd|rd|th)?\\s*(?:to|through|and|-|\xE2\x80\x93)\\s*"
		"(\\d{1,2})(?:st|nd|rd|th)?,?\\s+(\\d{4})$", std::regex::ECMAScript | std::regex::icase);
	std::smatch found;
	const std::string text = span.text;
	if (std::regex_match(text, found, parts) == false) return dates;
	for (const std::string day : {found[2].str(), found[3].str()}) {
		const std::string written = found[1].str() + " " + day + ", " + found[4].str();
		try {
			const std::optional<semantic::Value> read = semantic::parse_value(written);
			if (read.has_value() == false || kindOf(*read) != "date") continue;
			StatedValue value;
			value.kind = "date";
			value.key = semantic::value_key(*read);
			value.text = written;
			value.start = span.start;
			value.length = span.length;
			value.isBound = true;
			dates.push_back(value);
		} catch (const std::exception&) {
		}
	}
	return dates;
}

// A value the statement gives only to set it aside: "not $1.58 million",
// "instead of fifteen (15) days". "Not more than", "not later than" bound a
// value; they give it.
bool isSetAsideAt(const std::string& text, std::size_t start) {
	// A figure inside a document's file name is part of its name, never a claim:
	// "Sablewood - Staff Schedule (Oct 2026).xlsx" was checked as a date against
	// the index's own, and a workplan read as unreconciled. The name runs on to
	// its extension with no new phrase - an opening bracket, a quote, a cell's end
	// - and no word of prose ("dated March 6, 2026 in Letter.docx").
	{
		static const std::vector<std::string> extensions{".docx", ".xlsx", ".pptx", ".pdf", ".doc", ".xls", ".eml", ".msg",
			".txt", ".csv", ".md"};
		std::string word;
		for (std::size_t at = start; at < text.size() && at < start + 120; ++at) {
			const char character = text[at];
			if (character == '\n' || character == '|' || character == ';' || character == '"' || character == '('
					|| text.compare(at, 3, "\xE2\x80\x9C") == 0 || text.compare(at, 3, "\xE2\x80\x9D") == 0)
				break;
			if (std::isalpha(static_cast<unsigned char>(character)) != 0) {
				word.push_back(character);
				continue;
			}
			const bool isProse = word.size() >= 2 && std::all_of(word.begin(), word.end(),
				[](char letter) { return letter >= 'a' && letter <= 'z'; });
			if (isProse == true) break;
			word.clear();
			if (character != '.') continue;
			bool isExtension = false;
			for (const std::string& extension : extensions) {
				if (text.compare(at, extension.size(), extension) != 0) continue;
				const std::size_t after = at + extension.size();
				if (after >= text.size() || std::isalnum(static_cast<unsigned char>(text[after])) == 0) isExtension = true;
			}
			if (isExtension == true) return true;
		}
	}
	const std::size_t from = start > 32 ? start - 32 : 0;
	std::string before = lowered(text.substr(from, start - from));
	while (before.empty() == false && (before.back() == ' ' || before.back() == '(')) before.pop_back();
	for (const char* filler : {" about", " approximately", " around", " some", " only"}) {
		const std::string word = filler;
		if (before.size() >= word.size() && before.compare(before.size() - word.size(), word.size(), word) == 0) {
			before.erase(before.size() - word.size());
			while (before.empty() == false && before.back() == ' ') before.pop_back();
		}
	}
	for (const char* ending : {"not", "instead of", "rather than", "replacing", "in place of", "formerly", "previously"}) {
		const std::string word = ending;
		if (before.size() < word.size() || before.compare(before.size() - word.size(), word.size(), word) != 0) continue;
		const std::size_t at = before.size() - word.size();
		if (at == 0 || isLetter(before[at - 1]) == false) return true;
	}
	return false;

}

// A bare figure that is the whole of a cell, read by what its column says it
// is: money under "(USD)", years under "Term (years)", a rate under "%".
std::optional<StatedValue> bareValueOf(const std::string& text, const std::string& label) {

	const std::string figure = trimmed(text);
	static const std::regex number("^[-(]?\\$?\\d+(?:,\\d{3})*(?:\\.\\d+)?\\)?%?$");
	if (std::regex_match(figure, number) == false) return std::nullopt;
	try {
		semantic::ParseHints hints;
		hints.currency = semantic::currency_named(label);
		std::optional<semantic::Value> read;
		if (hints.currency.empty() == true) {
			if (const std::optional<semantic::NamedMeasure> measure = semantic::measure_named(label);
					measure.has_value() == true && measure->kind == semantic::Kind::Duration) {
				std::string digits;
				for (const char character : figure) {
					if (isDigit(character) == true || character == '.') digits.push_back(character);
				}
				const std::optional<semantic::Rational> count = semantic::Rational::parse_decimal(digits);
				if (count.has_value() == true) read = semantic::make_duration(*count, measure->unit);
			}
			const std::string words = lowered(label);
			if (read.has_value() == false && (words.find("(years)") != std::string::npos || words.find("in years") != std::string::npos)) {
				std::string digits;
				for (const char character : figure) {
					if (isDigit(character) == true || character == '.') digits.push_back(character);
				}
				const std::optional<semantic::Rational> count = semantic::Rational::parse_decimal(digits);
				if (count.has_value() == true) read = semantic::make_duration(*count, semantic::DurationUnit::Year);
			}
		}
		if (read.has_value() == false) read = semantic::parse_value(figure, hints);
		if (read.has_value() == false) return std::nullopt;
		StatedValue value;
		value.kind = kindOf(*read);
		if (value.kind.empty() == true) return std::nullopt;
		value.key = semantic::value_key(*read);
		value.text = figure;
		value.start = text.find(figure);
		value.length = figure.size();
		return value;
	} catch (const std::exception&) {
		return std::nullopt;
	}

}

// ---- Comparing values -------------------------------------------------------------

struct KeyParts {
	std::string head;
	std::string currency;
	std::string unit;
	std::string amount;
};

KeyParts partsOf(const std::string& key) {
	KeyParts parts;
	const std::size_t first = key.find(':');
	parts.head = key.substr(0, first);
	if (first == std::string::npos) return parts;
	const std::string rest = key.substr(first + 1);
	if (parts.head == "MONEY") {
		const std::size_t second = rest.find(':');
		parts.currency = rest.substr(0, second);
		parts.amount = second == std::string::npos ? std::string() : rest.substr(second + 1);
	} else if (parts.head == "DUR") {
		const std::size_t second = rest.find(':');
		parts.unit = rest.substr(0, second);
		parts.amount = second == std::string::npos ? std::string() : rest.substr(second + 1);
	} else {
		parts.amount = rest;
	}
	return parts;
}

// A period in days or in months, where one converts: weeks to days, years to
// months. Business days stay business days.
std::optional<std::pair<std::string, semantic::Rational>> periodMeasure(const KeyParts& parts) {
	const std::optional<semantic::Rational> amount = semantic::Rational::parse_decimal(parts.amount);
	if (amount.has_value() == false) return std::nullopt;
	if (parts.unit == "Day") return std::make_pair(std::string("day"), *amount);
	if (parts.unit == "Week") return std::make_pair(std::string("day"), *amount * semantic::Rational(7));
	if (parts.unit == "Month") return std::make_pair(std::string("month"), *amount);
	if (parts.unit == "Year") return std::make_pair(std::string("month"), *amount * semantic::Rational(12));
	return std::make_pair(parts.unit, *amount);
}

// What can be compared with what: an amount with an amount or a bare figure,
// a period with a period, a date with a date.
std::string comparableClass(const StatedValue& value) {
	if (value.kind == "money" || (value.kind == "number" && value.key.rfind("NUM:", 0) == 0)) return "amount";
	if (value.kind == "number") return "multiple";
	return value.kind;
}

} // namespace

std::vector<StatedValue> valuesIn(const std::string& text, const std::string& label) {

	std::vector<StatedValue> values;
	if (trimmed(text).empty() == true) return values;
	if (label.empty() == false) {
		if (std::optional<StatedValue> bare = bareValueOf(text, label); bare.has_value() == true) {
			values.push_back(*bare);
			return values;
		}
	}
	for (const Span& span : spansIn(text, label.empty() == false)) {
		if (span.shape == "dates") {
			for (StatedValue date : datesOfRange(span)) {
				date.isSetAside = isSetAsideAt(text, span.start);
				values.push_back(date);
			}
			continue;
		}
		std::optional<StatedValue> value = readSpan(span);
		if (value.has_value() == false) continue;
		value->isSetAside = isSetAsideAt(text, span.start);
		values.push_back(*value);
	}
	return values;

}

void readValues(Statement& statement) {
	statement.values = valuesIn(statement.text, statement.label);
	statement.isRead = true;
}

bool isSameValue(const StatedValue& left, const StatedValue& right) {

	if (left.key == right.key) return true;
	if (comparableClass(left) != comparableClass(right)) return false;
	const KeyParts one = partsOf(left.key);
	const KeyParts other = partsOf(right.key);
	if (comparableClass(left) == "amount") {
		if (one.head == "MONEY" && other.head == "MONEY" && one.currency != "?" && other.currency != "?"
				&& one.currency != other.currency)
			return false;
		const std::optional<semantic::Rational> a = semantic::Rational::parse_decimal(one.amount);
		const std::optional<semantic::Rational> b = semantic::Rational::parse_decimal(other.amount);
		return a.has_value() == true && b.has_value() == true && *a == *b;
	}
	if (one.head == "DUR" && other.head == "DUR") {
		const auto a = periodMeasure(one);
		const auto b = periodMeasure(other);
		return a.has_value() == true && b.has_value() == true && a->first == b->first && a->second == b->second;
	}
	if (one.head == "REF" && other.head == "REF") {
		return one.amount == other.amount;
	}
	return false;

}

namespace {

// ---- What a statement is about ----------------------------------------------------

const std::set<std::string>& stopWords() {
	static const std::set<std::string> words = {"the", "and", "for", "with", "from", "into", "under", "over", "per", "its",
		"this", "that", "these", "those", "are", "was", "were", "been", "any", "all", "each", "every", "other", "such",
		"than", "then", "not", "shall", "will", "may", "must", "can", "has", "have", "had", "which", "who", "whom", "whose",
		"what", "when", "where", "there", "their", "them", "they", "our", "your", "his", "her", "after", "before", "within",
		"about", "also", "only", "more", "less", "both", "either", "neither", "same", "said", "upon", "would", "should",
		"could", "does", "did", "done", "being", "here", "hereby", "herein", "thereof", "including", "between", "against",
		"out", "but", "nor", "yes", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven",
		"twelve", "fifteen", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred",
		"thousand", "million", "billion", "usd", "eur", "gbp", "day", "days", "month", "months", "year", "years", "week",
		"weeks", "times", "per", "cent", "percent", "dated", "date", "says", "say", "now", "new", "see", "set", "made",
		// A company's form names no company.
		"inc", "llc", "ltd", "corp", "corporation", "company", "plc", "gmbh", "llp",
		// A month is part of the date it is in, never what the date is about.
		"january", "february", "march", "april", "june", "july", "august", "september", "october", "november", "december",
		"jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec"};
	return words;
}

std::string stemmed(std::string word) {
	if (word.size() > 2 && word.compare(word.size() - 2, 2, "'s") == 0) word.erase(word.size() - 2);
	if (word.size() > 4 && word.back() == 's' && word[word.size() - 2] != 's' && word[word.size() - 2] != 'u'
			&& word[word.size() - 2] != 'i')
		word.pop_back();
	if (word.size() > 5 && word.compare(word.size() - 3, 3, "ing") == 0) word.erase(word.size() - 3);
	else if (word.size() > 4 && word.compare(word.size() - 2, 2, "ed") == 0) word.erase(word.size() - 2);
	return word;
}

// A code that names one thing - a part "CV-220", a contract "C-01" - never a
// cell ("G2"), an ordinal ("12th") or a multiple ("2x").
bool isIdentifier(const std::string& token) {
	bool hasLetter = false;
	bool hasDigit = false;
	for (const char character : token) {
		if (isLetter(character) == true) hasLetter = true;
		if (isDigit(character) == true) hasDigit = true;
	}
	if (hasLetter == false || hasDigit == false) return false;
	// Read by hand, never by a regular expression: every word of every
	// statement passes here.
	std::size_t digits = 0;
	while (digits < token.size() && isDigit(token[digits]) == true) ++digits;
	if (digits > 0) {
		const std::string suffix = lowered(token.substr(digits));
		if (suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th" || suffix == "x" || suffix == "k" || suffix == "m")
			return false;
	}
	std::size_t letters = 0;
	while (letters < token.size() && isLetter(token[letters]) == true) ++letters;
	std::size_t trailing = letters;
	while (trailing < token.size() && isDigit(token[trailing]) == true) ++trailing;
	const bool isCell = letters >= 1 && letters <= 3 && trailing == token.size() && trailing - letters >= 1
		&& trailing - letters <= 7;
	if (isCell == true) return false;
	return true;
}

struct Tokens {
	std::set<std::string> words;
};

Tokens tokensOf(const std::string& text) {

	Tokens tokens;
	std::string piece;
	const auto take = [&]() {
		while (piece.empty() == false && (piece.back() == '-' || piece.back() == '\'')) piece.pop_back();
		while (piece.empty() == false && (piece.front() == '-' || piece.front() == '\'')) piece.erase(0, 1);
		if (piece.empty() == true) return;
		if (isIdentifier(piece) == true) {
			std::string upper;
			for (const char character : piece) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
			tokens.words.insert("#" + upper);
		} else {
			std::string part;
			for (const char character : piece + "-") {
				if (character == '-') {
					std::string word = stemmed(lowered(part));
					bool isWord = word.size() >= 3;
					for (const char letter : word) isWord = isWord && (isLetter(letter) == true || letter == '\'');
					if (isWord == true && stopWords().count(word) == 0) tokens.words.insert(word);
					part.clear();
					continue;
				}
				part.push_back(character);
			}
		}
		piece.clear();
	};
	for (const char character : text) {
		if (isLetter(character) == true || isDigit(character) == true || character == '-' || character == '\''
				|| static_cast<unsigned char>(character) >= 0x80) {
			// A curly apostrophe ends nothing: "Halvane’s" is Halvane's.
			if (static_cast<unsigned char>(character) >= 0x80) {
				if (piece.empty() == false) take();
				continue;
			}
			piece.push_back(character);
			continue;
		}
		take();
	}
	take();
	return tokens;

}

// The words of the clause a value stands in: "Halvane's annual purchases
// $1.58 million" of "Corvane supply: Halvane's annual purchases $1.58
// million; prices fixed for the term". In a table's row, the column the
// value stands in, by the header's own words.
std::string clauseAround(const Statement& statement, const StatedValue& value) {

	const std::string& text = statement.text;
	if (text.find(" | ") != std::string::npos && statement.label.find(" | ") != std::string::npos) {
		std::vector<std::string> header;
		std::size_t from = 0;
		for (std::size_t at = statement.label.find(" | "); ; at = statement.label.find(" | ", from)) {
			header.push_back(statement.label.substr(from, at == std::string::npos ? std::string::npos : at - from));
			if (at == std::string::npos) break;
			from = at + 3;
		}
		std::size_t column = 0;
		for (std::size_t at = text.find(" | "); at != std::string::npos && at < value.start; at = text.find(" | ", at + 3)) ++column;
		const std::size_t rowEnd = text.find(" | ");
		const std::string first = rowEnd == std::string::npos ? text : text.substr(0, rowEnd);
		return (column < header.size() ? header[column] : std::string()) + " " + first;
	}
	std::size_t start = value.start;
	while (start > 0 && std::string(";:|\n").find(text[start - 1]) == std::string::npos
			&& (text[start - 1] != ',' || start < 2 || text[start - 2] == ' ' || isDigit(text[start - 2]) == true)
			&& (text[start - 1] != '.' || start < 2 || isDigit(text[start - 2]) == true))
		--start;
	std::size_t end = value.start + value.length;
	while (end < text.size() && std::string(";:|\n").find(text[end]) == std::string::npos
			&& (text[end] != ',' || end + 1 >= text.size() || isDigit(text[end + 1]) == true)
			&& (text[end] != '.' || end + 1 >= text.size() || isDigit(text[end + 1]) == true))
		++end;
	// A value after a short label is what the label says it is: "NDA
	// signature: October 31, 2026" is the NDA's signature date.
	if (start > 0 && text[start - 1] == ':') {
		std::size_t labelStart = start - 1;
		while (labelStart > 0 && std::string(";:|\n").find(text[labelStart - 1]) == std::string::npos
				&& (text[labelStart - 1] != '.' || labelStart < 2 || isDigit(text[labelStart - 2]) == true))
			--labelStart;
		if (start - 1 - labelStart <= 60) start = labelStart;
	}
	return text.substr(start, end - start);

}

// A label's words as one phrase: "As of", "NDA signature:" - lower case, one
// space between words, without the colon that ends it.
std::string phraseOf(const std::string& text) {
	std::string phrase;
	for (const char character : text) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			if (phrase.empty() == false && phrase.back() != ' ') phrase.push_back(' ');
			continue;
		}
		phrase.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	while (phrase.empty() == false && (phrase.back() == ' ' || phrase.back() == ':')) phrase.pop_back();
	return phrase;
}

// The words a value follows in its clause, as a phrase: "as of" in "As of
// October 24, 2026", "nda signature" in "NDA signature: October 31, 2026".
std::string wordsBefore(const Statement& statement, const StatedValue& value) {
	const std::string clause = clauseAround(statement, value);
	const std::size_t at = clause.find(value.text);
	if (at == std::string::npos) return std::string();
	return phraseOf(clause.substr(0, at));
}

// How much each word tells apart what it is said in: a word every document
// has tells nothing; a part number two documents share tells a great deal.
struct Weights {
	std::map<std::string, double> weight;
	double of(const std::string& word) const {
		const auto found = weight.find(word);
		return found == weight.end() ? 0.0 : found->second;
	}
};

std::string documentKeyOf(const Statement& statement) {
	return statement.documentId.empty() == true ? statement.documentName : statement.documentId;
}

// The weights over words already read, each with the document it is in.
Weights weightsOfTokens(const std::vector<std::pair<std::string, const Tokens*>>& tokenized) {

	std::map<std::string, std::set<std::string>> documentsOf;
	std::set<std::string> documents;
	for (const auto& [document, tokens] : tokenized) {
		documents.insert(document);
		for (const std::string& word : tokens->words) documentsOf[word].insert(document);
	}
	// 1 for a word one document has, falling toward 0 as more have it, however
	// many documents there are; a code counts twice.
	Weights weights;
	const double all = static_cast<double>(std::max<std::size_t>(documents.size(), 2));
	const double most = std::log((all + 1.0) / 1.5);
	for (const auto& [word, holders] : documentsOf) {
		double weight = std::log((all + 1.0) / (static_cast<double>(holders.size()) + 0.5)) / most;
		if (weight < 0.0) weight = 0.0;
		if (word.empty() == false && word.front() == '#') weight *= 2.0;
		weights.weight[word] = weight;
	}
	return weights;

}

Weights weightsOf(const std::vector<const Statement*>& statements) {
	std::vector<Tokens> read;
	read.reserve(statements.size());
	for (const Statement* statement : statements) read.push_back(tokensOf(statement->label + " " + statement->subject + " "
		+ statement->text));
	std::vector<std::pair<std::string, const Tokens*>> tokenized;
	for (std::size_t index = 0; index < statements.size(); ++index) tokenized.push_back({documentKeyOf(*statements[index]), &read[index]});
	return weightsOfTokens(tokenized);
}

struct Prepared {
	const Statement* statement = nullptr;
	Tokens all;
	// What its own heading or header says it is.
	Tokens heading;
	std::vector<StatedValue> values;
};

Prepared prepared(const Statement& statement) {
	Prepared made;
	made.statement = &statement;
	made.all = tokensOf(statement.label + " " + statement.subject + " " + statement.text);
	made.heading = tokensOf(statement.label);
	made.values = statement.isRead == true ? statement.values : valuesIn(statement.text, statement.label);
	return made;
}

// A statement's values: as read once, or read now.
std::vector<StatedValue> valuesOf(const Statement& statement) {
	return statement.isRead == true ? statement.values : valuesIn(statement.text, statement.label);
}

std::string quoteOf(const Statement& statement) {
	std::string text = trimmed(statement.text);
	if (text.size() > 240) {
		std::size_t end = 240;
		while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
		text = text.substr(0, end) + "...";
	}
	return text;
}

std::string placeWords(const Statement& statement) {
	return statement.documentName + (statement.place.empty() == true ? std::string() : ", " + statement.place);
}

} // namespace

std::vector<Statement> statementsOfLines(const std::string& text, const std::string& documentId,
	const std::string& documentName, const std::string& standing) {

	std::vector<Statement> statements;
	std::string title;
	std::size_t number = 0;
	std::size_t from = 0;
	while (from <= text.size()) {
		std::size_t end = text.find('\n', from);
		if (end == std::string::npos) end = text.size();
		const std::string line = trimmed(text.substr(from, end - from));
		++number;
		from = end + 1;
		if (line.empty() == true) continue;
		if (title.empty() == true) title = line;
		Statement statement;
		statement.documentId = documentId;
		statement.documentName = documentName;
		statement.place = "line " + std::to_string(number);
		statement.subject = title.size() > 160 ? title.substr(0, 160) : title;
		statement.text = line;
		statement.standing = standing;
		statements.push_back(std::move(statement));
		if (end >= text.size()) break;
	}
	return statements;

}

std::vector<FactCheck> checkFacts(const std::vector<Statement>& targets, const std::vector<Statement>& sources) {

	// Every statement read once: its words, and a source's values.
	std::vector<Prepared> preparedSources;
	preparedSources.reserve(sources.size());
	for (const Statement& statement : sources) preparedSources.push_back(prepared(statement));
	std::vector<Tokens> targetWords;
	targetWords.reserve(targets.size());
	for (const Statement& target : targets) targetWords.push_back(tokensOf(target.label + " " + target.subject + " " + target.text));
	std::vector<std::pair<std::string, const Tokens*>> tokenized;
	for (std::size_t index = 0; index < targets.size(); ++index) tokenized.push_back({documentKeyOf(targets[index]), &targetWords[index]});
	for (const Prepared& source : preparedSources) tokenized.push_back({documentKeyOf(*source.statement), &source.all});
	const Weights weights = weightsOfTokens(tokenized);
	std::vector<const Prepared*> readSources;
	for (const Prepared& source : preparedSources) {
		if (source.statement->isCalculated == true || source.values.empty() == true) continue;
		readSources.push_back(&source);
	}

	std::vector<FactCheck> checks;
	for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
		const Statement& target = targets[targetIndex];
		if (target.isCalculated == true) continue;
		const Tokens& about = targetWords[targetIndex];
		const Tokens header = tokensOf(target.label);
		// A labelled cell is said of its row's subject; a line of prose or of a
		// slide says what it is about in its own words.
		const bool hasSubject = trimmed(target.subject).empty() == false && trimmed(target.label).empty() == false;
		for (const StatedValue& value : valuesOf(target)) {
			if (value.kind == "reference" || value.isSetAside == true) continue;
			const Tokens what = tokensOf(target.label + " " + clauseAround(target, value));
			// The words the value follows, when they are a label of two words or
			// more: "As of" in "As of October 24, 2026".
			const std::string before = wordsBefore(target, value);
			const bool isLabelled = before.find(' ') != std::string::npos;
			FactCheck check;
			check.documentId = target.documentId;
			check.documentName = target.documentName;
			check.place = target.place;
			check.label = target.label;
			check.quote = quoteOf(target);
			check.value = value;
			check.state = "unsourced";

			// A source that gives this very value and shares what it is about
			// bears it out where none names it well enough to compare: a slide's
			// "NDA signature: October 31, 2026" and a checklist's "Mutual NDA signed
			// by both parties", due 2026-10-31. A date or an amount is specific
			// enough that one word naming what it is will do.
			const auto bearOut = [&]() {
				const Prepared* bearing = nullptr;
				const StatedValue* bearingValue = nullptr;
				double bearingShared = 0.0;
				const bool isSpecific = value.kind == "date" || value.kind == "money";
				for (const Prepared* read : readSources) {
					const Statement& statement = *read->statement;
					const bool isSameDocument = (target.documentId.empty() == false && statement.documentId == target.documentId)
						|| (target.documentId.empty() == true && statement.documentName == target.documentName);
					if (isSameDocument == true) continue;
					const StatedValue* given = nullptr;
					for (const StatedValue& stated : read->values) {
						if (stated.isSetAside == false && isSameValue(stated, value) == true) given = &stated;
					}
					if (given == nullptr) continue;
					double shared = 0.0;
					bool namesIt = false;
					for (const std::string& word : read->all.words) {
						if (about.words.count(word) == 0) continue;
						const double weight = weights.of(word);
						shared += weight;
						if (what.words.count(word) > 0 && weight >= 0.1) namesIt = true;
					}
					if (isLabelled == true && phraseOf(statement.label) == before) {
						namesIt = true;
						shared += 1.0;
					}
					const bool bears = shared >= 0.3 || (isSpecific == true && namesIt == true);
					if (bears == true && (bearing == nullptr || shared > bearingShared)) {
						bearing = read;
						bearingValue = given;
						bearingShared = shared;
					}
				}
				if (bearing == nullptr) return;
				FactEvidence evidence;
				evidence.documentId = bearing->statement->documentId;
				evidence.documentName = bearing->statement->documentName;
				evidence.place = bearing->statement->place;
				evidence.quote = quoteOf(*bearing->statement);
				evidence.value = bearingValue->text;
				evidence.isSame = true;
				evidence.stand = bearing->statement->stand;
				check.state = "agrees";
				check.strength = bearingShared;
				check.evidence.push_back(evidence);
			};
			// One end of a span of days stands for some of what the span covers:
			// borne out by a source that gives it, never said to differ.
			if (value.isBound == true) {
				bearOut();
				checks.push_back(std::move(check));
				continue;
			}

			// Each source that gives a value of the same kind, by how closely it
			// names what this value is (twice) and what it is about.
			struct Candidate {
				const Prepared* source = nullptr;
				double score = 0.0;
			};
			std::vector<Candidate> candidates;
			for (const Prepared* read : readSources) {
				const Prepared& source = *read;
				const Statement& statement = *source.statement;
				const bool isSameDocument = (target.documentId.empty() == false && statement.documentId == target.documentId)
					|| (target.documentId.empty() == true && statement.documentName == target.documentName);
				if (isSameDocument == true) continue;
				bool hasKind = false;
				for (const StatedValue& given : source.values) {
					if (comparableClass(given) == comparableClass(value)) hasKind = true;
				}
				if (hasKind == false) continue;
				double named = 0.0;
				double related = 0.0;
				bool sharesCode = false;
				for (const std::string& word : source.all.words) {
					const double weight = weights.of(word);
					if (what.words.count(word) > 0) {
						named += weight;
						if (word.front() == '#') sharesCode = true;
					} else if (about.words.count(word) > 0) {
						related += weight;
					}
				}
				// A header the source's own heading carries whole names what the
				// value is, however common its words: "Term (years)" and "Term and
				// Termination". So does the label the value follows, where a source's
				// cell has it for its own: "As of".
				if (header.words.empty() == false && std::all_of(header.words.begin(), header.words.end(),
						[&](const std::string& word) { return source.heading.words.count(word) > 0; }))
					named += 1.0;
				if (isLabelled == true && phraseOf(statement.label) == before) named += 1.0;
				// It must name what the value is, not only what it is about: a
				// code they share, or words of real weight; and where the value is
				// said of a row's subject, be about that too.
				if (sharesCode == false && named < 0.6) continue;
				if (sharesCode == false && hasSubject == true && related < 0.3) continue;
				candidates.push_back(Candidate{&source, 2.0 * named + related});
			}
			if (candidates.empty() == true) {
				bearOut();
				checks.push_back(std::move(check));
				continue;
			}
			std::sort(candidates.begin(), candidates.end(),
				[](const Candidate& left, const Candidate& right) { return left.score > right.score; });
			const double best = candidates.front().score;
			check.strength = best;
			// The sources that name it nearly as well as the best one does.
			std::vector<const Candidate*> close;
			for (const Candidate& candidate : candidates) {
				if (candidate.score >= best * 0.7 && close.size() < 4) close.push_back(&candidate);
			}
			const Candidate* agreeing = nullptr;
			bool isAgreeingSource = false;
			for (const Candidate* candidate : close) {
				for (const StatedValue& given : candidate->source->values) {
					if (given.isSetAside == true || isSameValue(given, value) == false) continue;
					const bool isSource = candidate->source->statement->standing == "source";
					if (agreeing == nullptr || (isSource == true && isAgreeingSource == false)) {
						agreeing = candidate;
						isAgreeingSource = isSource;
					}
				}
			}
			const auto evidenceOf = [&](const Candidate* candidate) {
				FactEvidence evidence;
				const Statement& statement = *candidate->source->statement;
				evidence.documentId = statement.documentId;
				evidence.documentName = statement.documentName;
				evidence.place = statement.place;
				evidence.quote = quoteOf(statement);
				evidence.stand = statement.stand;
				std::vector<std::string> stated;
				for (const StatedValue& given : candidate->source->values) {
					if (given.isSetAside == true || comparableClass(given) != comparableClass(value)) continue;
					if (isSameValue(given, value) == true) evidence.isSame = true;
					if (std::find(stated.begin(), stated.end(), given.text) == stated.end()) stated.push_back(given.text);
				}
				for (std::size_t index = 0; index < stated.size(); ++index)
					evidence.value += (index == 0 ? std::string() : index + 1 == stated.size() ? " and " : ", ") + stated[index];
				return evidence;
			};
			// A source that settles facts outweighs a summary that restates them:
			// where one agrees, the value agrees.
			bool sourceDiffers = false;
			for (const Candidate* candidate : close) {
				if (candidate->source->statement->standing != "source") continue;
				const FactEvidence evidence = evidenceOf(candidate);
				if (evidence.isSame == false && evidence.value.empty() == false) sourceDiffers = true;
			}
			if (agreeing != nullptr && (isAgreeingSource == true || sourceDiffers == false)) {
				check.state = "agrees";
				check.evidence.push_back(evidenceOf(agreeing));
				for (const Candidate* candidate : close) {
					if (candidate != agreeing && check.evidence.size() < 3) {
						const FactEvidence evidence = evidenceOf(candidate);
						if (evidence.isSame == false) check.evidence.push_back(evidence);
					}
				}
				checks.push_back(std::move(check));
				continue;
			}
			// The source that names it best gives one value, another: it differs.
			// Several values, none this one: unclear, for the reader to judge.
			std::size_t statedThere = 0;
			for (const StatedValue& given : candidates.front().source->values) {
				if (given.isSetAside == false && comparableClass(given) == comparableClass(value)) ++statedThere;
			}
			check.state = statedThere == 1 ? "differs" : "unclear";
			for (const Candidate* candidate : close) {
				if (check.evidence.size() >= 3) break;
				const FactEvidence evidence = evidenceOf(candidate);
				if (evidence.value.empty() == false) check.evidence.push_back(evidence);
			}
			if (check.evidence.empty() == true) check.state = "unsourced";
			checks.push_back(std::move(check));
		}
	}
	return checks;

}

namespace {

// The document words name, by the words of its name: a tracker's row "C-01 |
// Supply Agreement (Corvane comments 10-22)" names "Halvane Supply Agreement
// (Corvane comments 10-22).docx"; a cell "Disclosure: Schedule 3.12(a)" names
// "Sablewood Disclosure Schedule (Draft 10-23).docx".
const FactDocument* documentNamedIn(const std::string& words, const std::vector<FactDocument>& documents) {

	const Tokens said = tokensOf(words);
	const FactDocument* best = nullptr;
	double bestShare = 0.0;
	std::size_t bestShared = 0;
	for (const FactDocument& document : documents) {
		std::string name = document.documentName;
		const std::size_t dot = name.rfind('.');
		if (dot != std::string::npos && name.size() - dot <= 5) name = name.substr(0, dot);
		const Tokens own = tokensOf(name);
		if (own.words.empty() == true) continue;
		std::size_t shared = 0;
		for (const std::string& word : own.words) {
			if (said.words.count(word) > 0) ++shared;
		}
		const double share = static_cast<double>(shared) / static_cast<double>(own.words.size());
		if (shared > bestShared || (shared == bestShared && share > bestShare)) {
			best = &document;
			bestShared = shared;
			bestShare = share;
		}
	}
	if (best == nullptr || bestShared < 2 || bestShare < 0.5) return nullptr;
	return best;

}

std::string numberOfKey(const std::string& key) {
	// "REF:section:16.3:item:3" gives "16.3".
	const std::size_t first = key.find(':');
	const std::size_t second = first == std::string::npos ? std::string::npos : key.find(':', first + 1);
	if (second == std::string::npos) return std::string();
	const std::size_t third = key.find(':', second + 1);
	return key.substr(second + 1, third == std::string::npos ? std::string::npos : third - second - 1);
}

std::string kindOfKey(const std::string& key) {
	const std::size_t first = key.find(':');
	const std::size_t second = first == std::string::npos ? std::string::npos : key.find(':', first + 1);
	if (second == std::string::npos) return std::string();
	return key.substr(first + 1, second - first - 1);
}

std::string itemOfKey(const std::string& key) {
	const std::size_t at = key.find(":item:");
	return at == std::string::npos ? std::string() : key.substr(at + 6);
}

const FactPlace* placeIn(const FactDocument& document, const std::string& kind, const std::string& number) {
	// A dotted number is a place whatever a reference calls it ("Schedule
	// 3.12(a)" and "Section 3.12(a)"); a whole number is one of its kind.
	const bool isDotted = number.find('.') != std::string::npos || number.find('(') != std::string::npos;
	for (const FactPlace& place : document.places) {
		if (isDotted == true && (place.key == number || place.key.substr(place.key.find(':') + 1) == number)) return &place;
		if (isDotted == false && (place.key == kind + ":" + number || (kind == "section" && place.key == number))) return &place;
	}
	return nullptr;
}

std::string highestOf(const FactDocument& document) {
	int most = 0;
	for (const FactPlace& place : document.places) {
		if (place.key.empty() == true || isDigit(place.key.front()) == false) continue;
		most = std::max(most, std::atoi(place.key.c_str()));
	}
	return most > 0 ? std::to_string(most) : std::string();
}

} // namespace

std::vector<FactCheck> checkReferences(const std::vector<Statement>& targets, const std::vector<FactDocument>& documents) {

	// The words' weights over the targets and every place the documents have,
	// so a word every list of contracts has ("Agreement") tells nothing.
	std::vector<Statement> placesRead;
	for (const FactDocument& document : documents) {
		for (const FactPlace& place : document.places) {
			Statement read;
			read.documentId = document.documentId;
			read.documentName = document.documentName;
			read.text = place.words;
			for (const std::string& item : place.items) read.text += "\n" + item;
			placesRead.push_back(std::move(read));
		}
	}
	std::vector<const Statement*> everything;
	for (const Statement& statement : targets) everything.push_back(&statement);
	for (const Statement& statement : placesRead) everything.push_back(&statement);
	const Weights weights = weightsOf(everything);
	std::vector<FactCheck> checks;
	for (const Statement& target : targets) {
		if (target.isCalculated == true) continue;
		for (const StatedValue& value : valuesOf(target)) {
			if (value.kind != "reference") continue;
			const std::string kind = kindOfKey(value.key);
			const std::string number = numberOfKey(value.key);
			const std::string item = itemOfKey(value.key);
			FactCheck check;
			check.documentId = target.documentId;
			check.documentName = target.documentName;
			check.place = target.place;
			check.label = target.label;
			check.quote = quoteOf(target);
			check.value = value;
			check.state = "unsourced";

			// The document cited: the one the value's own words name ("Disclosure:
			// Schedule 3.12(a), item 2" names the disclosure schedule); else the one
			// the statement is about, where it has the place; else the one document
			// that has it - never the statement's own. A place the document the
			// statement is about lacks, which one other document has, is the
			// reader's to judge unless that document's item bears it out.
			const auto isOwn = [&](const FactDocument* document) {
				return document != nullptr && document->documentId.empty() == false && document->documentId == target.documentId;
			};
			const FactDocument* byWhat = documentNamedIn(target.label + " " + target.text, documents);
			const FactDocument* bySubject = documentNamedIn(target.subject + " " + target.text, documents);
			if (isOwn(byWhat) == true) byWhat = nullptr;
			if (isOwn(bySubject) == true) bySubject = nullptr;
			std::vector<const FactDocument*> holders;
			for (const FactDocument& document : documents) {
				if (isOwn(&document) == false && placeIn(document, kind, number) != nullptr) holders.push_back(&document);
			}
			const FactDocument* cited = nullptr;
			const FactDocument* lacking = nullptr;
			if (byWhat != nullptr) {
				cited = byWhat;
			} else if (bySubject != nullptr && placeIn(*bySubject, kind, number) != nullptr) {
				cited = bySubject;
			} else if (bySubject != nullptr) {
				cited = holders.size() == 1 ? holders.front() : bySubject;
				if (holders.size() == 1) lacking = bySubject;
			} else if (holders.size() == 1) {
				cited = holders.front();
			}
			if (cited == nullptr) {
				checks.push_back(std::move(check));
				continue;
			}
			const auto hasNo = [&](const FactDocument& document) {
				FactEvidence missing;
				missing.documentId = document.documentId;
				missing.documentName = document.documentName;
				const std::string highest = highestOf(document);
				std::string kindWord = kind;
				if (kindWord.empty() == false) kindWord.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(kindWord.front())));
				missing.quote = document.documentName + " has no " + kindWord + " " + number
					+ (highest.empty() == true ? std::string() : "; its sections run 1 to " + highest) + ".";
				return missing;
			};
			// What the document cited gives, and whether it bears the citation out.
			FactEvidence evidence;
			evidence.documentId = cited->documentId;
			evidence.documentName = cited->documentName;
			const FactPlace* place = placeIn(*cited, kind, number);
			std::string state;
			if (place == nullptr) {
				// A clause the document does not have; another document has it.
				evidence = hasNo(*cited);
				state = holders.empty() == true ? "differs" : "unclear";
			} else if (item.empty() == true) {
				evidence.place = place->words;
				evidence.quote = place->words;
				evidence.isSame = true;
				state = "agrees";
			} else {
				const std::size_t index = static_cast<std::size_t>(std::max(0, std::atoi(item.c_str()) - 1));
				evidence.place = place->words;
				if (index >= place->items.size()) {
					evidence.quote = place->words + " lists " + std::to_string(place->items.size()) + " items; it has no item " + item
						+ ".";
					state = "differs";
				} else {
					// An item cited for something must name it: the words the
					// statement's subject gives, by their weight - a contract's party,
					// its kind, its place - never its code, which the item does not
					// carry.
					const Tokens subject = tokensOf(target.subject);
					const Tokens itemWords = tokensOf(place->items[index]);
					double shared = 0.0;
					double named = 0.0;
					for (const std::string& word : subject.words) {
						if (word.front() == '#') continue;
						const double weight = weights.of(word);
						named += weight;
						if (itemWords.words.count(word) > 0) shared += weight;
					}
					evidence.quote = place->items[index];
					evidence.place = place->words + ", item " + item;
					evidence.isSame = shared > 0.0 && shared >= named * 0.3;
					state = evidence.isSame == true ? "agrees" : "differs";
				}
			}
			// The document the statement is about lacks the place: only an item
			// that names what the statement is about settles it.
			if (lacking != nullptr && (state != "agrees" || item.empty() == true)) {
				check.evidence.push_back(hasNo(*lacking));
				state = "unclear";
			}
			check.state = state;
			check.evidence.push_back(evidence);
			checks.push_back(std::move(check));
		}
	}
	return checks;

}

std::vector<ValueOrigin> originsOfNewValues(const std::string& before, const std::string& after, const std::string& label,
	const std::vector<Statement>& sources, const std::vector<GivenText>& given) {

	std::vector<ValueOrigin> origins;
	const std::vector<StatedValue> earlier = valuesIn(before, label);
	std::vector<StatedValue> added;
	for (const StatedValue& value : valuesIn(after, label)) {
		// A Section a change cites is no fact to source: whether it exists is the
		// integrity check's (IntegrityCheck, references). A DPA's markup listed
		// "Opening now gives Sections 6.2, which no document of the Matter ...
		// give" five times.
		if (value.isSetAside == true || value.kind == "reference") continue;
		const bool wasThere = std::any_of(earlier.begin(), earlier.end(),
			[&](const StatedValue& old) { return isSameValue(old, value); });
		const bool isRepeated = std::any_of(added.begin(), added.end(),
			[&](const StatedValue& kept) { return isSameValue(kept, value); });
		if (wasThere == false && isRepeated == false) added.push_back(value);
	}
	if (added.empty() == true) return origins;
	Statement written;
	written.text = after;
	written.label = label;
	written.documentName = "the change";
	std::vector<Statement> targets{written};
	const std::vector<FactCheck> checked = checkFacts(targets, sources);
	for (const StatedValue& value : added) {
		ValueOrigin origin;
		origin.value = value;
		origin.state = "unsourced";
		for (const FactCheck& check : checked) {
			if (check.value.start != value.start || check.state != "agrees" || check.evidence.empty() == true) continue;
			origin.state = "sourced";
			origin.evidence = check.evidence.front();
		}
		// Else any source that gives the value, for anything; else what the work
		// was given.
		if (origin.state == "unsourced") {
			for (const Statement& source : sources) {
				if (origin.state == "sourced") break;
				for (const StatedValue& stated : valuesOf(source)) {
					if (stated.isSetAside == true || isSameValue(stated, value) == false) continue;
					origin.state = "sourced";
					origin.evidence.documentId = source.documentId;
					origin.evidence.documentName = source.documentName;
					origin.evidence.place = source.place;
					origin.evidence.quote = quoteOf(source);
					origin.evidence.value = stated.text;
					origin.evidence.isSame = true;
					break;
				}
			}
		}
		if (origin.state == "unsourced") {
			for (const GivenText& text : given) {
				if (origin.state == "sourced") break;
				for (const StatedValue& stated : valuesIn(text.text)) {
					if (stated.isSetAside == true || isSameValue(stated, value) == false) continue;
					origin.state = "sourced";
					origin.evidence.documentName = text.name;
					origin.evidence.value = stated.text;
					origin.evidence.isSame = true;
					break;
				}
			}
		}
		origins.push_back(std::move(origin));
	}
	return origins;

}

std::string checkWords(const FactCheck& check) {

	std::string label = trimmed(check.label);
	if (label.size() > 80) label = label.substr(0, 80) + "...";
	std::string words = check.place + (label.empty() == true ? std::string() : " (" + label + ")") + " gives "
		+ check.value.text;
	if (check.value.kind == "reference") words = check.place + " cites " + check.value.text;
	if (check.state == "unsourced") return words + "; no document of the Matter gives it.";
	for (std::size_t index = 0; index < check.evidence.size(); ++index) {
		const FactEvidence& evidence = check.evidence[index];
		words += index == 0 ? "; " : "; and ";
		const std::string where = evidence.documentName + (evidence.place.empty() == true ? std::string() : ", " + evidence.place);
		if (check.value.kind == "reference") {
			// A place a document lacks is said in plain words; a place it has, in
			// its own.
			if (evidence.place.empty() == true) {
				std::string said = evidence.quote;
				while (said.empty() == false && said.back() == '.') said.pop_back();
				words += said;
				continue;
			}
			words += where + ": “" + evidence.quote + "”";
			continue;
		}
		words += where + " gives " + (evidence.value.empty() == true ? std::string("it") : evidence.value) + ": “"
			+ evidence.quote + "”";
		// Whether an agreement's words are agreed is part of what they settle: a
		// price no open change touches is the price (run 8 of 2026-10-25 took
		// Schedule 1's agreed price for one still negotiated).
		if (evidence.stand == "agreed") words += " (agreed words: no tracked change is open there)";
		else if (evidence.stand.empty() == false) words += " (" + evidence.stand + ")";
	}
	return words + ".";

}

} // namespace singularity::document_intelligence
