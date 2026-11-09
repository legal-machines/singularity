#include "PapinianGuard/AnswerContract/AnswerContract.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "PapinianGuard/ProtectedMarkers/ProtectedMarkers.hpp"
#include "PapinianGuard/ProtectedOperations/ProtectedOperations.hpp"
#include "PapinianGuard/RestorationGrammar/SpelledFigures.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <utf8proc.h>

// The final answer contract (DESIGN4 §9). Every check reads the model's own
// protected text, the text that was sent, and the operation's table of facts
// and operations by their declared types, provenance and derivations - never a
// value. What a check says is therefore the same whether a claim the text makes
// is true or false (DESIGN4 §8.4 L7 iv).
//
// Pure: linked into every build that reads operations (the link rule of the
// seam), it calls only SemanticAlgebra, ControlText and ProtectedOperations'
// figure core, and reads the table's fields directly.
namespace singularity::protected_api::papinian::answer_contract {

namespace {

constexpr std::size_t kNone = std::string::npos;

// What kind of quantity a symbol stands for, from its declared type.
enum class Quantity { None, Money, Rate, Date, Duration, Number, Count, Rows, Flag };

// ---------------------------------------------------------------------------
// Text helpers.

// A case-folded copy (utf8proc): "…" and "…" read alike. The text
// itself when it is not valid UTF-8.
std::string folded(std::string_view text) {
	if (text.empty()) return {};
	utf8proc_uint8_t *mapped = nullptr;
	const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
		static_cast<utf8proc_ssize_t>(text.size()), &mapped, UTF8PROC_CASEFOLD);
	if (length < 0 || mapped == nullptr) return std::string(text);
	std::string result(reinterpret_cast<const char *>(mapped), static_cast<std::size_t>(length));
	std::free(mapped);
	return result;
}

bool is_space(char character) {
	return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

bool is_ascii_letter(char character) {
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

bool is_digit(char character) {
	return character >= '0' && character <= '9';
}

// A byte that begins a letter of any script: ASCII letters and every lead byte
// of a multi-byte UTF-8 character (Cyrillic, accented Latin).
bool is_letter_byte(char character) {
	const auto byte = static_cast<unsigned char>(character);
	return is_ascii_letter(character) || byte >= 0xC0;
}

// Where the space after `from` ends: plain spaces and the no-break ones.
std::size_t skip_spaces(std::string_view text, std::size_t from) {
	while (from < text.size()) {
		if (text[from] == ' ' || text[from] == '\t') {
			++from;
			continue;
		}
		if (text.compare(from, 2, "\xC2\xA0") == 0) {
			from += 2;
			continue;
		}
		if (text.compare(from, 3, "\xE2\x80\xAF") == 0) {
			from += 3;
			continue;
		}
		break;
	}
	return from;
}

// The same, backwards: where the space before `to` begins.
std::size_t skip_spaces_back(std::string_view text, std::size_t to) {
	while (to > 0) {
		if (text[to - 1] == ' ' || text[to - 1] == '\t') {
			--to;
			continue;
		}
		if (to >= 2 && text.compare(to - 2, 2, "\xC2\xA0") == 0) {
			to -= 2;
			continue;
		}
		if (to >= 3 && text.compare(to - 3, 3, "\xE2\x80\xAF") == 0) {
			to -= 3;
			continue;
		}
		break;
	}
	return to;
}

// A start that is not inside a UTF-8 character.
std::size_t character_start(std::string_view text, std::size_t at) {
	while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
	return at;
}

// Up to `count` bytes of `text` from `at`, cut where a character ends, folded.
std::string folded_ahead(std::string_view text, std::size_t at, std::size_t count) {
	if (at >= text.size()) return {};
	std::size_t end = std::min(text.size(), at + count);
	if (end < text.size()) end = character_start(text, end);
	return folded(text.substr(at, end - at));
}

bool starts_with(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string trimmed(std::string_view text) {
	std::size_t from = 0;
	std::size_t to = text.size();
	while (from < to && is_space(text[from])) ++from;
	while (to > from && is_space(text[to - 1])) --to;
	return std::string(text.substr(from, to - from));
}

// The text between a PromptAssembler section's own lines, or nothing.
std::string section_of(const std::string &prompt, std::string_view name) {
	const std::string opening = "<<<LM_SECTION:" + std::string(name) + ">>>\n";
	const std::size_t start = prompt.find(opening);
	if (start == kNone) return {};
	const std::size_t from = start + opening.size();
	const std::size_t close = prompt.find("\n<<<LM_END_SECTION:" + std::string(name) + ">>>", from);
	return close == kNone ? std::string() : prompt.substr(from, close - from);
}

bool contains_any(const std::string &haystack, std::initializer_list<const char *> needles) {
	for (const char *needle : needles)
		if (haystack.find(needle) != kNone) return true;
	return false;
}

// ---------------------------------------------------------------------------
// The table read by symbol.

std::string lowered_ascii(std::string text) {
	for (char &character : text)
		if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
	return text;
}

// "Amount" of "Amount3".
std::string letters_of(const std::string &base) {
	std::string letters;
	for (const char character : base)
		if (is_ascii_letter(character)) letters += character;
	return letters;
}

Quantity quantity_of_prefix(const std::string &letters) {
	const std::string prefix = lowered_ascii(letters);
	if (prefix == "amount") return Quantity::Money;
	if (prefix == "rate") return Quantity::Rate;
	if (prefix == "date") return Quantity::Date;
	if (prefix == "duration") return Quantity::Duration;
	if (prefix == "number") return Quantity::Number;
	if (prefix == "count") return Quantity::Count;
	if (prefix == "rows") return Quantity::Rows;
	if (prefix == "flag") return Quantity::Flag;
	return Quantity::None;
}

// The type head a DerivedFact writes ("MONEY(USD/month)").
Quantity quantity_of_type(const std::string &type) {
	if (starts_with(type, "MONEY")) return Quantity::Money;
	if (starts_with(type, "RATE") || starts_with(type, "PERCENTAGE")) return Quantity::Rate;
	if (starts_with(type, "DATE")) return Quantity::Date;
	if (starts_with(type, "DURATION")) return Quantity::Duration;
	if (starts_with(type, "NUMBER")) return Quantity::Number;
	if (starts_with(type, "COUNT")) return Quantity::Count;
	if (starts_with(type, "ROW")) return Quantity::Rows;
	if (starts_with(type, "BOOLEAN")) return Quantity::Flag;
	return Quantity::None;
}

Quantity quantity_of_row(const RequestMappingEntry &row) {
	if (row.value) {
		switch (row.value->kind) {
			case semantic::Kind::Money: return Quantity::Money;
			case semantic::Kind::Percentage: return Quantity::Rate;
			case semantic::Kind::Date:
			case semantic::Kind::DayOfYear: return Quantity::Date;
			case semantic::Kind::Duration: return Quantity::Duration;
			case semantic::Kind::Number: return Quantity::Number;
			case semantic::Kind::Count: return Quantity::Count;
			case semantic::Kind::Boolean: return Quantity::Flag;
		}
	}
	if (!row.type) return Quantity::None;
	switch (*row.type) {
		case EntityType::MoneyAmount: return Quantity::Money;
		case EntityType::Percentage: return Quantity::Rate;
		case EntityType::Date: return Quantity::Date;
		case EntityType::Number: return Quantity::Number;
		default: return Quantity::None;
	}
}

// "USD" of "MONEY(USD/month)"; "" when none is declared.
std::string currency_of_type(const std::string &type) {
	if (!starts_with(type, "MONEY(")) return {};
	std::string code;
	for (std::size_t at = 6; at < type.size() && type[at] >= 'A' && type[at] <= 'Z'; ++at) code += type[at];
	return code.size() == 3 ? code : std::string();
}

// One symbol of the text as the table knows it.
struct Symbol {
	std::string base;
	std::size_t start = 0;
	std::size_t length = 0;
	const RequestMappingEntry *row = nullptr;
	const compute::DerivedFact *derived = nullptr;
	Quantity quantity = Quantity::None;
	std::string fact_key;
	std::string currency;
	// Issued in this operation and carried by one of its calls (or worked out
	// for one of them): what the model may name.
	bool belongs = false;
};

// The table's rows and derived facts by their symbol, compared case-blind the
// way restoration reads a symbol a model varied ("[[ amount3 ]]").
class TableIndex {
public:
	explicit TableIndex(const OperationProtectedSymbolTable &table)
		: table_(table)
	{
		for (std::size_t index = 0; index < table.mapping.size(); ++index)
			rows_.emplace(lowered_ascii(table.mapping[index].placeholder), index);
		for (const auto &[placeholder, fact] : table.derived_facts) {
			derived_.emplace(lowered_ascii(placeholder), &fact);
			if (!fact.fact_key.empty()) derivedByKey_.emplace(fact.fact_key, &fact);
		}
	}

	const RequestMappingEntry *row(const std::string &base) const {
		const auto found = rows_.find(lowered_ascii(base));
		return found == rows_.end() ? nullptr : &table_.mapping[found->second];
	}

	const compute::DerivedFact *derived(const std::string &base) const {
		const auto found = derived_.find(lowered_ascii(base));
		return found == derived_.end() ? nullptr : found->second;
	}

	const compute::DerivedFact *derived_by_key(const std::string &key) const {
		const auto found = derivedByKey_.find(key);
		return found == derivedByKey_.end() ? nullptr : found->second;
	}

	const RequestMappingEntry *row_by_key(const std::string &key) const {
		const auto found = table_.fact_by_key.find(key);
		if (found == table_.fact_by_key.end() || found->second >= table_.mapping.size()) return nullptr;
		return &table_.mapping[found->second];
	}

	const OperationProtectedSymbolTable &table() const { return table_; }

private:
	const OperationProtectedSymbolTable &table_;
	std::map<std::string, std::size_t> rows_;
	std::map<std::string, const compute::DerivedFact *> derived_;
	std::map<std::string, const compute::DerivedFact *> derivedByKey_;
};

Symbol symbol_at(const markers::Parsed &parsed, const TableIndex &index, const std::string &sent_prompt) {
	Symbol symbol;
	symbol.base = "[[" + parsed.base + "]]";
	symbol.start = parsed.start;
	symbol.length = parsed.length;
	symbol.row = index.row(symbol.base);
	symbol.derived = index.derived(symbol.base);
	if (symbol.row != nullptr) symbol.base = symbol.row->placeholder;
	else if (symbol.derived != nullptr) symbol.base = symbol.derived->placeholder;
	if (symbol.derived != nullptr) {
		symbol.quantity = quantity_of_type(symbol.derived->type);
		symbol.fact_key = symbol.derived->fact_key;
		symbol.currency = currency_of_type(symbol.derived->type);
	}
	if (symbol.row != nullptr) {
		if (symbol.quantity == Quantity::None) symbol.quantity = quantity_of_row(*symbol.row);
		if (symbol.fact_key.empty()) symbol.fact_key = symbol.row->fact_key;
		if (symbol.currency.empty() && symbol.row->value && symbol.row->value->kind == semantic::Kind::Money)
			symbol.currency = symbol.row->value->currency;
	}
	if (symbol.quantity == Quantity::None) symbol.quantity = quantity_of_prefix(letters_of(parsed.base));
	const bool issued = symbol.row != nullptr || symbol.derived != nullptr;
	// One answer whether or not the table holds a symbol no call carried
	// (DESIGN4 §2.6 unknown_symbol): membership is read from what was sent -
	// the symbols the operation's calls carried, or, before any call was
	// noted, the sent text itself. A value worked out here belongs the same
	// way: once a call carried it (its result line, a planner line) - never
	// one policy withheld or the budget left out, which the model cannot know.
	const std::set<std::string> &sent = index.table().sent_symbols;
	symbol.belongs = issued
		&& (sent.count(symbol.base) > 0 || (sent.empty() && sent_prompt.find(symbol.base) != kNone));
	return symbol;
}

// ---------------------------------------------------------------------------
// Statements: {{= ...}}, {{if ...}} ... {{end}}, and Singularity's grammar ones.

struct Statement {
	std::size_t start = 0;
	std::size_t end = 0;
	// A calculation or a condition: not the protected protocol's (D6).
	bool forbidden = false;
};

// {{= [[Amount3]] as words}}: one symbol and a format, nothing worked out -
// Singularity only writes that one fact the way the text asks (in words, as money,
// as a date). The same statement over two symbols computes, and is not this.
bool format_only(std::string_view content) {
	const std::string body = trimmed(content);
	const std::vector<markers::Parsed> found = markers::find_all(body);
	if (found.size() != 1 || found.front().start != 0 || body.compare(0, 2, "[[") != 0) return false;
	const std::string rest = trimmed(std::string_view(body).substr(found.front().length));
	if (rest.empty()) return true;
	if (!starts_with(rest, "as ")) return false;
	return semantic::parse_format(trimmed(std::string_view(rest).substr(3))).has_value();
}

std::vector<Statement> statements_in(const std::string &text) {
	std::vector<Statement> statements;
	for (std::size_t open = text.find("{{"); open != kNone;) {
		const std::size_t close = text.find("}}", open + 2);
		if (close == kNone) break;
		Statement statement;
		statement.start = open;
		statement.end = close + 2;
		const std::string_view content = std::string_view(text).substr(open + 2, close - open - 2);
		const std::string body = trimmed(content);
		if (starts_with(body, "=")) statement.forbidden = !format_only(std::string_view(body).substr(1));
		else if (starts_with(body, "if ") || starts_with(body, "if(")) statement.forbidden = true;
		statements.push_back(statement);
		open = text.find("{{", close + 2);
	}
	return statements;
}

const Statement *statement_around(const std::vector<Statement> &statements, std::size_t at) {
	for (const Statement &statement : statements)
		if (at >= statement.start && at < statement.end) return &statement;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Words beside a symbol (type in context).

// The currency a sign, a code or a word right before or after a symbol names:
// "$[[X]]", "[[X]] USD", "[[X]] dollars", «…». "" when none.
std::string currency_before(std::string_view text, std::size_t start) {
	const std::size_t end = skip_spaces_back(text, start);
	// "$" is a dollar of any country: its code is "$".
	static const std::pair<const char *, const char *> signs[] = {{"US$", "USD"}, {"$", "$"}, {"\xE2\x82\xAC", "EUR"},
		{"\xC2\xA3", "GBP"}, {"\xC2\xA5", "JPY"}, {"\xE2\x82\xBD", "RUB"}, {"USD", "USD"}, {"EUR", "EUR"}, {"GBP", "GBP"},
		{"RUB", "RUB"}, {"JPY", "JPY"}};
	for (const auto &[sign, code] : signs) {
		const std::size_t length = std::string_view(sign).size();
		if (end < length || text.compare(end - length, length, sign) != 0) continue;
		// A code is a word of its own ("in USD"), never the end of one.
		if (is_ascii_letter(sign[0]) && end > length && is_letter_byte(text[end - length - 1])) continue;
		return code;
	}
	return {};
}

bool word_ends_at(std::string_view text, std::size_t at) {
	return at >= text.size() || !(is_letter_byte(text[at]) || (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80);
}

std::string currency_after(std::string_view text, std::size_t end) {
	const std::size_t from = skip_spaces(text, end);
	if (from >= text.size()) return {};
	const std::string ahead = folded_ahead(text, from, 24);
	static const std::pair<const char *, const char *> signs[] = {{"$", "$"}, {"\xE2\x82\xAC", "EUR"}, {"\xC2\xA3", "GBP"},
		{"\xC2\xA5", "JPY"}, {"\xE2\x82\xBD", "RUB"}};
	for (const auto &[sign, code] : signs)
		if (starts_with(ahead, sign)) return code;
	static const std::pair<const char *, const char *> codes[] = {{"usd", "USD"}, {"eur", "EUR"}, {"gbp", "GBP"},
		{"rub", "RUB"}, {"jpy", "JPY"}};
	for (const auto &[word, code] : codes)
		if (starts_with(ahead, word) && word_ends_at(ahead, 3)) return code;
	static const std::pair<const char *, const char *> words[] = {{"dollar", "USD"}, {"euro", "EUR"}, {"rouble", "RUB"},
		{"ruble", "RUB"}, {"pound", "GBP"}, {"\xD1\x80\xD1\x83\xD0\xB1", "RUB"} /* … */,
		{"\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xBB", "USD"} /* … */, {"\xD0\xB5\xD0\xB2\xD1\x80\xD0\xBE", "EUR"} /* … */};
	for (const auto &[word, code] : words)
		if (starts_with(ahead, word)) return code;
	return {};
}

bool percent_after(std::string_view text, std::size_t end) {
	const std::size_t from = skip_spaces(text, end);
	if (from >= text.size()) return false;
	if (text[from] == '%' || text.compare(from, 3, "\xEF\xBC\x85") == 0) return true;
	const std::string ahead = folded_ahead(text, from, 24);
	return starts_with(ahead, "percent") || starts_with(ahead, "per cent")
		|| starts_with(ahead, "\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82");  // …
}

// A unit of time right after a symbol ("[[X]] days", «…»); `years`
// says whether a year word counts ("…" is how a Russian date ends).
bool period_after(std::string_view text, std::size_t end, bool years) {
	const std::size_t from = skip_spaces(text, end);
	if (from >= text.size()) return false;
	const std::string ahead = folded_ahead(text, from, 32);
	for (const char *word : {"day", "business day", "calendar day", "week", "month"}) {
		const std::size_t length = std::string_view(word).size();
		if (!starts_with(ahead, word)) continue;
		std::size_t after = length;
		if (after < ahead.size() && ahead[after] == 's') ++after;
		if (word_ends_at(ahead, after)) return true;
	}
	if (years && (starts_with(ahead, "year") && word_ends_at(ahead, ahead.size() > 4 && ahead[4] == 's' ? 5 : 4)))
		return true;
	for (const char *stem : {"\xD0\xB4\xD0\xBD", "\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C", "\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB",
			"\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86", "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87",
			"\xD0\xBA\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB4\xD0\xB0\xD1\x80\xD0\xBD"})
		if (starts_with(ahead, stem)) return true;
	if (years && (starts_with(ahead, "\xD0\xB3\xD0\xBE\xD0\xB4") || starts_with(ahead, "\xD0\xBB\xD0\xB5\xD1\x82")))
		return true;
	return false;
}

bool type_fits_context(const Symbol &symbol, std::string_view text) {
	const std::size_t end = symbol.start + symbol.length;
	std::string currency = currency_before(text, symbol.start);
	if (currency.empty()) currency = currency_after(text, end);
	if (!currency.empty()) {
		if (symbol.quantity != Quantity::Money) return false;
		// "$" beside any dollar (USD, CAD, AUD ...) fits.
		const bool anyDollar = currency == "$" && symbol.currency.size() == 3 && symbol.currency.back() == 'D';
		if (!symbol.currency.empty() && symbol.currency != "?" && symbol.currency != currency && !anyDollar) return false;
	}
	if (percent_after(text, end) && symbol.quantity != Quantity::Rate) return false;
	if (symbol.quantity == Quantity::Money || symbol.quantity == Quantity::Rate)
		if (period_after(text, end, true)) return false;
	if (symbol.quantity == Quantity::Date && period_after(text, end, false)) return false;
	return true;
}

// ---------------------------------------------------------------------------
// Bounds (DESIGN4 §9.1): a Bounded end written with a bound word of its own
// direction before it.

enum class Direction { None, Lower, Upper };

// The direction of the bound word nearest before `start` (on the same line),
// or None when there is none. «…» is an upper bound.
Direction bound_word_before(std::string_view text, std::size_t start, std::size_t end) {
	std::size_t from = start > 72 ? start - 72 : 0;
	const std::size_t line = text.rfind('\n', start == 0 ? 0 : start - 1);
	if (line != kNone && line + 1 > from && line < start) from = line + 1;
	from = character_start(text, from);
	const std::string window = folded(text.substr(from, start - from));
	static const char *lower[] = {"not before", "on or after", "at least", "no earlier than", "not earlier than",
		"not less than", "no less than", "no sooner than",
		"\xD0\xBD\xD0\xB5 \xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xB5",                  // …
		"\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",                  // …
		"\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB8\xD0\xBD\xD0\xB0\xD1\x8F \xD1\x81",          // …
		"\xD0\xBD\xD0\xB5 \xD1\x80\xD0\xB0\xD0\xBD\xD1\x8C\xD1\x88\xD0\xB5",          // …
		"\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8C\xD1\x88\xD0\xB5"};         // …
	static const char *upper[] = {"no later than", "not later than", "on or before", "at most", "not after",
		"not more than", "no more than",
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB5",  // …
		"\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5",                  // …
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB6\xD0\xB5",                  // …
		"\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD1\x8C\xD1\x88\xD0\xB5"};         // …
	const auto last_of = [&](const char *const *words, std::size_t count) {
		std::size_t best = kNone;
		for (std::size_t index = 0; index < count; ++index) {
			const std::size_t at = window.rfind(words[index]);
			if (at == kNone) continue;
			const std::size_t wordEnd = at + std::string_view(words[index]).size();
			if (best == kNone || wordEnd > best) best = wordEnd;
		}
		return best;
	};
	std::size_t lowerAt = last_of(lower, std::size(lower));
	std::size_t upperAt = last_of(upper, std::size(upper));
	// «…»: "…" right before, "…" right after.
	const std::string before = trimmed(window);
	const std::string until = "\xD0\xB4\xD0\xBE";  // …
	const bool untilAlone = before.size() >= until.size()
		&& before.compare(before.size() - until.size(), until.size(), until) == 0
		&& (before.size() == until.size() || (!is_letter_byte(before[before.size() - until.size() - 1])
			&& (static_cast<unsigned char>(before[before.size() - until.size() - 1]) & 0xC0) != 0x80));
	if (untilAlone) {
		const std::string after = folded_ahead(text, end, 40);
		if (after.find("\xD0\xB2\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xBE") != kNone)
			upperAt = window.size() + 1;  // …: nearest of all
	}
	// A range written out: "between [[Lo]] and [[Hi]]", "from [[Lo]] to [[Hi]]",
	// «…» - each end by its place.
	const auto ends_with_word = [&](const char *word) {
		const std::string_view wanted(word);
		if (before.size() < wanted.size() || before.compare(before.size() - wanted.size(), wanted.size(), wanted) != 0) return false;
		if (before.size() == wanted.size()) return true;
		const char previous = before[before.size() - wanted.size() - 1];
		return previous == ' ' || previous == '(' || previous == ',';
	};
	const bool opensRange = ends_with_word("between") || ends_with_word("from")
		|| ends_with_word("\xD0\xBC\xD0\xB5\xD0\xB6\xD0\xB4\xD1\x83")   // …
		|| ends_with_word("\xD1\x81");                                          // …
	const bool closesRange = (ends_with_word("and") || ends_with_word("to") || ends_with_word("through")
			|| ends_with_word("\xD0\xB8") || ends_with_word("\xD0\xBF\xD0\xBE"))  // …
		&& (window.find("between ") != kNone || window.find("from ") != kNone
			|| window.find("\xD0\xBC\xD0\xB5\xD0\xB6\xD0\xB4\xD1\x83 ") != kNone || window.find("\xD1\x81 ") != kNone)
		&& window.find("[[") != kNone;
	if (opensRange) lowerAt = window.size() + 2;
	if (closesRange) upperAt = window.size() + 2;
	if (lowerAt == kNone && upperAt == kNone) return Direction::None;
	if (lowerAt == kNone) return Direction::Upper;
	if (upperAt == kNone) return Direction::Lower;
	return upperAt > lowerAt ? Direction::Upper : Direction::Lower;
}

// ---------------------------------------------------------------------------
// Claims matched by structure only (DESIGN4 §9.1): an expression canonical
// modulo commutativity and associativity of + and x, - as + of a negation and
// / as x of an inverse. No value is read.

struct Node {
	enum class Kind { Atom, Sum, Product };
	Kind kind = Kind::Atom;
	std::string atom;
	// Sum: (+1 | -1, term); Product: (+1 | -1 exponent, factor).
	std::vector<std::pair<int, Node>> children;
};

Node atom_node(std::string atom) {
	Node node;
	node.atom = std::move(atom);
	return node;
}

void collect(const Node &node, Node::Kind kind, int sign, std::vector<std::pair<int, std::string>> &parts,
		const std::function<std::string(const Node &)> &canonical) {
	if (node.kind == kind) {
		for (const auto &[inner, child] : node.children) collect(child, kind, sign * inner, parts, canonical);
		return;
	}
	parts.emplace_back(sign, canonical(node));
}

std::string canonical(const Node &node) {
	if (node.kind == Node::Kind::Atom) return node.atom;
	std::vector<std::pair<int, std::string>> parts;
	const std::function<std::string(const Node &)> self = [](const Node &child) { return canonical(child); };
	for (const auto &[sign, child] : node.children) collect(child, node.kind, sign, parts, self);
	if (parts.size() == 1 && parts.front().first == 1) return parts.front().second;
	std::vector<std::string> written;
	for (const auto &[sign, part] : parts) written.push_back((sign > 0 ? "+" : "-") + part);
	std::sort(written.begin(), written.end());
	std::string result = node.kind == Node::Kind::Sum ? "S[" : "P[";
	for (std::size_t index = 0; index < written.size(); ++index) result += (index == 0 ? "" : ",") + written[index];
	return result + "]";
}

// How a claim or a derivation names one operand: by the fact it is.
struct Atoms {
	const TableIndex &index;

	std::string of_symbol(const std::string &written) const {
		const std::string base = written;
		if (const compute::DerivedFact *fact = index.derived(base))
			if (!fact->fact_key.empty()) return "fk:" + fact->fact_key;
		if (const RequestMappingEntry *row = index.row(base))
			if (!row->fact_key.empty()) return "fk:" + row->fact_key;
		return "sym:" + lowered_ascii(base);
	}

	std::string of_label(const std::string &label) const {
		const auto found = index.table().compute_labels.find(label);
		return found == index.table().compute_labels.end() ? "label:" + label : of_symbol(found->second);
	}

	static std::string of_constant(const std::string &written) {
		std::string text = written;
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
		try {
			if (const std::optional<semantic::Value> value = semantic::parse_value(text)) return "c:" + semantic::value_key(*value);
		} catch (const std::exception &) {
		}
		return "c:" + trimmed(text);
	}
};

// Expands every operand that is a derived fact into its own derivation, so a
// claim over the roots matches a derivation over intermediate results.
Node expanded(const Node &node, const TableIndex &index, unsigned depth);

// "subtract(fk:a,fk:b)", "sum(k1,k2)|..." (a cycle-3 derivation), or the
// op log's "subtract([[Amount1]],@base;where;options)", as a Node.
std::optional<Node> parse_call(std::string_view text, const Atoms &atoms, unsigned depth) {
	if (depth > 8) return std::nullopt;
	const std::size_t bar = text.find('|');
	if (bar != kNone && text.find('(') != kNone && bar > text.rfind(')')) text = text.substr(0, bar);
	const std::size_t open = text.find('(');
	const std::size_t close = text.rfind(')');
	if (open == kNone || close == kNone || close < open) return std::nullopt;
	const std::string op = trimmed(text.substr(0, open));
	const std::string_view inside = text.substr(open + 1, close - open - 1);
	std::vector<std::string> args;
	std::size_t level = 0;
	bool quoted = false;
	std::string current;
	for (const char character : inside) {
		if (character == '"') quoted = !quoted;
		if (!quoted && character == '(') ++level;
		if (!quoted && character == ')' && level > 0) --level;
		if (!quoted && level == 0 && (character == ',' || character == ';')) {
			args.push_back(trimmed(current));
			current.clear();
			// Where-atoms and options follow the first ';': not operands.
			if (character == ';') break;
			continue;
		}
		current += character;
	}
	if (!trimmed(current).empty() && (args.empty() || inside.find(';') == kNone)) args.push_back(trimmed(current));
	std::vector<Node> operands;
	for (const std::string &arg : args) {
		if (arg.empty()) continue;
		if (arg.find('(') != kNone && arg.back() == ')' && arg.front() != '"') {
			const std::optional<Node> nested = parse_call(arg, atoms, depth + 1);
			if (nested) {
				operands.push_back(*nested);
				continue;
			}
		}
		if (starts_with(arg, "[[")) operands.push_back(atom_node(atoms.of_symbol(arg)));
		else if (starts_with(arg, "@")) operands.push_back(atom_node(atoms.of_label(arg.substr(1))));
		else if (starts_with(arg, "fk:")) operands.push_back(atom_node(arg));
		else if (starts_with(arg, "c:")) operands.push_back(atom_node(arg));
		else if (atoms.index.row_by_key(arg) != nullptr || atoms.index.derived_by_key(arg) != nullptr)
			operands.push_back(atom_node("fk:" + arg));
		else if (arg.front() == '"' || is_digit(arg.front())) operands.push_back(atom_node(Atoms::of_constant(arg)));
		else operands.push_back(atom_node("w:" + arg));
	}
	Node node;
	if ((op == "add" || op == "sum") && operands.size() >= 2) {
		node.kind = Node::Kind::Sum;
		for (Node &operand : operands) node.children.emplace_back(1, std::move(operand));
		return node;
	}
	if ((op == "subtract" || op == "difference") && operands.size() >= 2) {
		node.kind = Node::Kind::Sum;
		for (std::size_t index = 0; index < operands.size(); ++index)
			node.children.emplace_back(index == 0 ? 1 : -1, std::move(operands[index]));
		return node;
	}
	if ((op == "multiply" || op == "product" || op == "percentage_of") && operands.size() >= 2) {
		node.kind = Node::Kind::Product;
		for (Node &operand : operands) node.children.emplace_back(1, std::move(operand));
		return node;
	}
	if ((op == "divide" || op == "ratio") && operands.size() == 2) {
		node.kind = Node::Kind::Product;
		node.children.emplace_back(1, std::move(operands[0]));
		node.children.emplace_back(-1, std::move(operands[1]));
		return node;
	}
	std::string written = "op:" + op + "(";
	for (std::size_t index = 0; index < operands.size(); ++index) written += (index == 0 ? "" : ",") + canonical(operands[index]);
	return atom_node(written + ")");
}

Node expanded(const Node &node, const TableIndex &index, unsigned depth) {
	if (node.kind != Node::Kind::Atom) {
		Node copy = node;
		for (auto &[sign, child] : copy.children) child = expanded(child, index, depth);
		return copy;
	}
	if (depth > 8 || !starts_with(node.atom, "fk:")) return node;
	const compute::DerivedFact *fact = index.derived_by_key(node.atom.substr(3));
	const Atoms atoms{index};
	std::optional<Node> inner;
	if (fact != nullptr && !fact->ast.empty()) inner = parse_call(fact->ast, atoms, 0);
	if (!inner) {
		const RequestMappingEntry *row = index.row_by_key(node.atom.substr(3));
		if (row != nullptr && row->derived && !row->provenance.derivation.empty())
			inner = parse_call(row->provenance.derivation, atoms, 0);
	}
	if (!inner || inner->kind == Node::Kind::Atom) return node;
	return expanded(*inner, index, depth + 1);
}

// The claim side of an equation: symbols, @labels and figures joined by
// + - x / (and their words), with brackets. Read left to right from `at`.
class ExpressionReader {
public:
	ExpressionReader(std::string_view text, const Atoms &atoms)
		: text_(text)
		, atoms_(atoms)
	{
	}

	// The expression starting at `at`, where it ends, and how many operators
	// it has; nothing when none starts there.
	std::optional<Node> read(std::size_t at, std::size_t &end, std::size_t &operators) {
		at_ = at;
		operators_ = 0;
		std::optional<Node> node = sum();
		end = at_;
		operators = operators_;
		return node;
	}

private:
	std::string_view text_;
	const Atoms &atoms_;
	std::size_t at_ = 0;
	std::size_t operators_ = 0;

	void spaces() { at_ = skip_spaces(text_, at_); }

	// One of `words` (case-folded) at the cursor, as a whole word.
	bool word(std::initializer_list<const char *> words) {
		const std::string ahead = folded_ahead(text_, at_, 24);
		for (const char *candidate : words) {
			const std::size_t length = std::string_view(candidate).size();
			if (!starts_with(ahead, candidate) || !word_ends_at(ahead, length)) continue;
			at_ += length;
			return true;
		}
		return false;
	}

	// +1 plus, -1 minus, 0 neither.
	int additive() {
		spaces();
		if (at_ >= text_.size()) return 0;
		if (text_[at_] == '+') {
			++at_;
			return 1;
		}
		if (text_[at_] == '-' && (at_ + 1 >= text_.size() || text_[at_ + 1] != '>')) {
			++at_;
			return -1;
		}
		if (text_.compare(at_, 3, "\xE2\x88\x92") == 0) {
			at_ += 3;
			return -1;
		}
		if (word({"plus"})) return 1;
		if (word({"minus", "\xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x81"})) return -1;
		return 0;
	}

	int multiplicative() {
		spaces();
		if (at_ >= text_.size()) return 0;
		if (text_[at_] == '*') {
			++at_;
			return 1;
		}
		if (text_[at_] == '/') {
			++at_;
			return -1;
		}
		if (text_.compare(at_, 2, "\xC3\x97") == 0) {  // ×
			at_ += 2;
			return 1;
		}
		if (text_.compare(at_, 2, "\xC3\xB7") == 0) {  // ÷
			at_ += 2;
			return -1;
		}
		if (word({"times", "multiplied by"})) return 1;
		if (word({"divided by"})) return -1;
		return 0;
	}

	std::optional<Node> sum() {
		std::optional<Node> first = product();
		if (!first) return std::nullopt;
		Node node;
		node.kind = Node::Kind::Sum;
		node.children.emplace_back(1, *first);
		while (true) {
			const std::size_t mark = at_;
			const int sign = additive();
			if (sign == 0) {
				at_ = mark;
				break;
			}
			std::optional<Node> next = product();
			if (!next) {
				at_ = mark;
				break;
			}
			++operators_;
			node.children.emplace_back(sign, *next);
		}
		return node.children.size() == 1 ? node.children.front().second : node;
	}

	std::optional<Node> product() {
		std::optional<Node> first = operand();
		if (!first) return std::nullopt;
		Node node;
		node.kind = Node::Kind::Product;
		node.children.emplace_back(1, *first);
		while (true) {
			const std::size_t mark = at_;
			const int exponent = multiplicative();
			if (exponent == 0) {
				at_ = mark;
				break;
			}
			std::optional<Node> next = operand();
			if (!next) {
				at_ = mark;
				break;
			}
			++operators_;
			node.children.emplace_back(exponent, *next);
		}
		return node.children.size() == 1 ? node.children.front().second : node;
	}

	std::optional<Node> operand() {
		spaces();
		if (at_ >= text_.size()) return std::nullopt;
		if (text_[at_] == '(') {
			const std::size_t mark = at_;
			++at_;
			std::optional<Node> inner = sum();
			spaces();
			if (!inner || at_ >= text_.size() || text_[at_] != ')') {
				at_ = mark;
				return std::nullopt;
			}
			++at_;
			return inner;
		}
		if (text_.compare(at_, 2, "[[") == 0) {
			const std::size_t close = text_.find("]]", at_ + 2);
			if (close == kNone || close - at_ > 48) return std::nullopt;
			const std::vector<markers::Parsed> parsed = markers::find_all(text_.substr(at_, close + 2 - at_));
			if (parsed.size() != 1 || parsed.front().start != 0) return std::nullopt;
			const std::string base = "[[" + parsed.front().base + "]]";
			at_ = close + 2;
			return atom_node(atoms_.of_symbol(base));
		}
		if (text_[at_] == '@') {
			std::size_t cursor = at_ + 1;
			while (cursor < text_.size() && (is_ascii_letter(text_[cursor]) || is_digit(text_[cursor]) || text_[cursor] == '_'))
				++cursor;
			if (cursor == at_ + 1) return std::nullopt;
			const std::string label(text_.substr(at_ + 1, cursor - at_ - 1));
			at_ = cursor;
			return atom_node(atoms_.of_label(label));
		}
		if (is_digit(text_[at_])) {
			std::size_t cursor = at_;
			while (cursor < text_.size() && (is_digit(text_[cursor]) || ((text_[cursor] == '.' || text_[cursor] == ',')
					&& cursor + 1 < text_.size() && is_digit(text_[cursor + 1]))))
				++cursor;
			if (cursor < text_.size() && text_[cursor] == '%') ++cursor;
			const std::string constant(text_.substr(at_, cursor - at_));
			at_ = cursor;
			return atom_node(Atoms::of_constant(constant));
		}
		return std::nullopt;
	}
};

struct Claim {
	std::string result;   // the symbol the claim is about, as its base
	std::size_t offset = 0;
	Node expression;
};

// Every arithmetic claim the text makes about one symbol: "[[X]] = [[A]] -
// [[B]]", "[[A]] + [[B]] = [[X]]", "[[X]] ([[A]] minus [[B]])", "[[X]], the
// sum of [[A]] and [[B]]" (and «…»).
std::vector<Claim> claims_in(const std::string &text, const std::vector<Symbol> &symbols,
		const std::vector<Statement> &statements, const Atoms &atoms) {
	std::vector<Claim> claims;
	ExpressionReader reader(text, atoms);
	std::map<std::size_t, const Symbol *> byStart;
	for (const Symbol &symbol : symbols) byStart.emplace(symbol.start, &symbol);

	// Equations around "=".
	for (std::size_t at = text.find('='); at != kNone; at = text.find('=', at + 1)) {
		if (statement_around(statements, at) != nullptr) continue;
		if (at > 0 && (text[at - 1] == '<' || text[at - 1] == '>' || text[at - 1] == '!' || text[at - 1] == '='
				|| text[at - 1] == ':'))
			continue;
		if (at + 1 < text.size() && text[at + 1] == '=') continue;
		std::size_t rightEnd = 0;
		std::size_t rightOperators = 0;
		const std::optional<Node> right = reader.read(at + 1, rightEnd, rightOperators);
		// The left side: the earliest start before "=" whose expression ends
		// exactly there.
		std::optional<Node> left;
		std::size_t leftOperators = 0;
		std::size_t leftStart = kNone;
		const std::size_t leftEnd = skip_spaces_back(text, at);
		const std::size_t from = leftEnd > 240 ? leftEnd - 240 : 0;
		for (std::size_t start = from; start < leftEnd; ++start) {
			const char character = text[start];
			if (character != '[' && character != '(' && character != '@' && !is_digit(character)) continue;
			if (character == '[' && (start + 1 >= text.size() || text[start + 1] != '[')) continue;
			if (character == '[' && start > 0 && text[start - 1] == '[') continue;
			std::size_t end = 0;
			std::size_t operators = 0;
			std::optional<Node> candidate = reader.read(start, end, operators);
			if (!candidate || skip_spaces_back(text, end) != leftEnd) continue;
			left = candidate;
			leftOperators = operators;
			leftStart = start;
			break;
		}
		if (!left || !right) continue;
		const auto single = [&](std::size_t start, const Node &node, std::size_t operators) -> const Symbol * {
			if (operators != 0 || node.kind != Node::Kind::Atom) return nullptr;
			const std::size_t position = skip_spaces(text, start);
			const auto found = byStart.find(position);
			return found == byStart.end() ? nullptr : found->second;
		};
		const Symbol *leftSymbol = single(leftStart, *left, leftOperators);
		const Symbol *rightSymbol = single(at + 1, *right, rightOperators);
		if (leftSymbol != nullptr && rightOperators > 0)
			claims.push_back({leftSymbol->base, leftSymbol->start, *right});
		else if (rightSymbol != nullptr && leftOperators > 0)
			claims.push_back({rightSymbol->base, rightSymbol->start, *left});
	}

	// Words and brackets right after a symbol.
	for (const Symbol &symbol : symbols) {
		if (symbol.quantity == Quantity::None) continue;
		std::size_t cursor = skip_spaces(text, symbol.start + symbol.length);
		if (cursor < text.size() && text[cursor] == '(') {
			std::size_t end = 0;
			std::size_t operators = 0;
			const std::optional<Node> inside = reader.read(cursor + 1, end, operators);
			const std::size_t close = skip_spaces(text, end);
			if (inside && operators > 0 && close < text.size() && text[close] == ')') {
				claims.push_back({symbol.base, symbol.start, *inside});
				continue;
			}
		}
		if (cursor < text.size() && (text[cursor] == ',' || text[cursor] == '(')) cursor = skip_spaces(text, cursor + 1);
		else if (text.compare(cursor, 3, "\xE2\x80\x94") == 0 || text.compare(cursor, 3, "\xE2\x80\x93") == 0)
			cursor = skip_spaces(text, cursor + 3);
		const std::string ahead = folded_ahead(text, cursor, 40);
		Node::Kind kind = Node::Kind::Sum;
		bool difference = false;
		std::size_t skip = 0;
		static const std::pair<const char *, int> phrases[] = {{"the sum of ", 0}, {"the total of ", 0},
			{"the difference between ", 1}, {"the difference of ", 1}, {"the product of ", 2},
			{"\xD1\x81\xD1\x83\xD0\xBC\xD0\xBC\xD0\xB0 ", 0},                                        // …
			{"\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBD\xD0\xB8\xD1\x86\xD0\xB0 \xD0\xBC\xD0\xB5\xD0\xB6\xD0\xB4\xD1\x83 ", 1},  // …
			{"\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C ", 1}};              // …
		for (const auto &[phrase, meaning] : phrases) {
			if (!starts_with(ahead, phrase)) continue;
			skip = std::string_view(phrase).size();
			kind = meaning == 2 ? Node::Kind::Product : Node::Kind::Sum;
			difference = meaning == 1;
			break;
		}
		if (skip == 0) continue;
		// The folded phrase has the text's own length for these words.
		std::size_t at = cursor + skip;
		std::vector<Node> operands;
		while (true) {
			at = skip_spaces(text, at);
			std::size_t end = 0;
			std::size_t operators = 0;
			const std::optional<Node> operand = text.compare(at, 2, "[[") == 0 || (at < text.size() && text[at] == '@')
				? reader.read(at, end, operators) : std::nullopt;
			if (!operand || operators != 0) break;
			operands.push_back(*operand);
			at = skip_spaces(text, end);
			const std::string joint = folded_ahead(text, at, 8);
			if (starts_with(joint, ",")) at += 1;
			else if (starts_with(joint, "and ")) at += 4;
			else if (starts_with(joint, "\xD0\xB8 ")) at += 3;  // …
			else break;
		}
		if (operands.size() < 2) continue;
		Node node;
		node.kind = kind;
		for (std::size_t index = 0; index < operands.size(); ++index)
			node.children.emplace_back(difference && index > 0 ? -1 : 1, operands[index]);
		claims.push_back({symbol.base, symbol.start, node});
	}
	return claims;
}

// Whether the text's claim about `result` is a derivation Singularity recorded for
// it: its DerivedFact, an operation whose result it is, or a cycle-3 derived
// row's derivation - each as written and fully expanded.
bool claim_recorded(const Claim &claim, const TableIndex &index) {
	const Atoms atoms{index};
	std::set<std::string> recorded;
	const auto add = [&](const std::optional<Node> &node) {
		if (!node) return;
		recorded.insert(canonical(*node));
		recorded.insert(canonical(expanded(*node, index, 0)));
	};
	if (const compute::DerivedFact *fact = index.derived(claim.result))
		if (!fact->ast.empty()) add(parse_call(fact->ast, atoms, 0));
	if (const RequestMappingEntry *row = index.row(claim.result))
		if (row->derived && !row->provenance.derivation.empty()) add(parse_call(row->provenance.derivation, atoms, 0));
	for (const compute::OpRecord &record : index.table().op_log) {
		if (record.status != "ok") continue;
		const bool isResult = std::any_of(record.results.begin(), record.results.end(),
			[&](const std::string &symbol) { return lowered_ascii(symbol) == lowered_ascii(claim.result); });
		if (isResult) add(parse_call(record.request_canonical, atoms, 0));
	}
	if (recorded.empty()) return false;
	return recorded.count(canonical(claim.expression)) > 0
		|| recorded.count(canonical(expanded(claim.expression, index, 0))) > 0;
}

// ---------------------------------------------------------------------------
// Signs (DESIGN4 §3.3): a difference used whose sign the model never learned.

bool sign_observed(const compute::DerivedFact &fact, const OperationProtectedSymbolTable &table) {
	for (const compute::OpRecord &record : table.op_log) {
		const bool isResult = std::find(record.results.begin(), record.results.end(), fact.placeholder) != record.results.end()
			|| (!fact.op_id.empty() && record.op_id == fact.op_id);
		if (isResult && contains_any(record.result_line, {"POSITIVE", "NEGATIVE", "ZERO"})) return true;
		const bool isOperand = std::find(record.operand_symbols.begin(), record.operand_symbols.end(), fact.placeholder)
			!= record.operand_symbols.end();
		if (isOperand && record.observation && (record.status == "ok" || record.status == "bounded")) return true;
	}
	return false;
}

bool needs_sign(const compute::DerivedFact &fact) {
	const Quantity quantity = quantity_of_type(fact.type);
	if (quantity != Quantity::Money && quantity != Quantity::Number && quantity != Quantity::Duration) return false;
	return fact.op == "subtract" || fact.ast.find("subtract(") != kNone;
}

// ---------------------------------------------------------------------------
// Provenance.

bool located(const FactProvenance &provenance) {
	return !provenance.passage_sha256.empty() || !provenance.cell.empty()
		|| (provenance.anchored && provenance.origin == FactProvenance::Origin::History);
}

bool provenance_complete(const compute::DerivedFact &fact, const TableIndex &index) {
	// A count over rows already listed owes no root again (membership was
	// disclosed), yet rests on the rows it counted: those are its provenance (a
	// fake-provider run: every such count was withheld as "to be confirmed").
	if (fact.roots.empty()) {
		if (fact.root_sources.empty()) return false;
		return std::all_of(fact.root_sources.begin(), fact.root_sources.end(), [](const RelationSource &source) {
			return !source.passage_sha256.empty() || !source.cell.empty();
		});
	}
	bool everyRootLocated = true;
	for (const std::string &root : fact.roots) {
		const RequestMappingEntry *row = index.row_by_key(root);
		if (row == nullptr || !located(row->provenance)) everyRootLocated = false;
	}
	if (everyRootLocated) return true;
	// A root that no prompt carried (a table cell) is located by its source:
	// the passage it was read in, or the workbook cell it stands in.
	if (fact.root_sources.size() < fact.roots.size()) return false;
	for (const RelationSource &source : fact.root_sources)
		if (source.passage_sha256.empty() && source.cell.empty()) return false;
	return true;
}

// ---------------------------------------------------------------------------
// Figures written outside a symbol (raw_figure).

// The safe constants (DESIGN4 §8.1 R4): figures any text may write.
bool safe_constant(const semantic::Value &value) {
	if (value.kind != semantic::Kind::Number && value.kind != semantic::Kind::Duration) return false;
	if (!value.amount.is_integer()) return false;
	static const std::set<std::string> safe{"0", "1", "7", "12", "24", "60", "100", "360", "365", "366", "1440"};
	return safe.count(value.amount.to_string()) > 0;
}

// The lines of what Singularity read from the Project at the model's request that
// are the model's own words sent back: "You asked to search the Project for
// "...":", "You asked to read ...", and what Singularity says of a request it did
// not run. What the model wrote never grounds a figure (DESIGN4 §8.2, §8.7).
std::string without_echoes(std::string_view part) {
	std::string kept;
	for (std::size_t from = 0; from < part.size();) {
		std::size_t end = part.find('\n', from);
		if (end == kNone) end = part.size();
		const std::string_view line = part.substr(from, end - from);
		from = end + 1;
		const std::string lead = trimmed(line);
		if (starts_with(lead, "You asked ") || starts_with(lead, "This Project search was not run")
				|| starts_with(lead, "This Project request could not be answered"))
			continue;
		kept.append(line);
		kept += '\n';
	}
	return kept;
}

bool numbered_line(std::string_view line) {
	std::size_t at = 0;
	while (at < line.size() && is_digit(line[at])) ++at;
	return at > 0 && at + 1 < line.size() && line[at] == '.' && line[at + 1] == ' ';
}

// A lawyer's answer block (Tool/LawyerQuestion answer_block): each question as
// the model put it ("1. ..."), then what the lawyer chose and wrote. Only the
// lawyer's part is theirs; the question is the model's own words sent back.
std::string lawyers_words(std::string_view part) {
	bool isNumbered = false;
	for (std::size_t from = 0; from < part.size() && !isNumbered;) {
		std::size_t end = part.find('\n', from);
		if (end == kNone) end = part.size();
		isNumbered = numbered_line(part.substr(from, end - from));
		from = end + 1;
	}
	if (!isNumbered) return std::string(part);
	std::string kept;
	bool inWords = false;
	for (std::size_t from = 0; from < part.size();) {
		std::size_t end = part.find('\n', from);
		if (end == kNone) end = part.size();
		const std::string_view line = part.substr(from, end - from);
		from = end + 1;
		const std::string lead = trimmed(line);
		if (numbered_line(line) || starts_with(lead, "Left to you:")) {
			inWords = false;
			continue;
		}
		if (starts_with(lead, "Chosen:")) inWords = false;
		else if (starts_with(lead, "In their words:")) inWords = true;
		else if (!inWords) continue;
		kept.append(line);
		kept += '\n';
	}
	return kept;
}

// What a call sent that grounds a figure the answer writes openly (DESIGN4
// §8.2): its source, request and lawyer-answer segments, as PromptAssembler
// and the loop mark them (ComputeProtocol payload_segments) - never what the
// model wrote and Singularity sent back (an echoed search, a question put to the
// lawyer, an earlier answer, a results block) and never Singularity's own lines.
// And the value key of every figure it writes, by the one recogniser. Worked
// out once per sent text: every field of an answer, and a repair, reads the
// same one again.
struct Grounding {
	std::string text;
	std::set<std::string> keys;
};

// The parts of `sent_prompt` of the kinds `wanted` admits, as grounding text.
template <typename Wanted>
void append_segments(const std::string &sent_prompt, Wanted wanted,
		const std::function<void(const std::string &)> &append) {
	for (const compute::PayloadSegment &segment : compute::payload_segments(sent_prompt)) {
		if (!wanted(segment.kind) || segment.start >= sent_prompt.size()) continue;
		const std::string_view part = std::string_view(sent_prompt).substr(segment.start, segment.length);
		append(segment.kind == compute::PayloadSegment::Kind::LawyerAnswer ? lawyers_words(part) : without_echoes(part));
	}
}

// This call's source, request and lawyer-answer segments, and the lawyer's
// material earlier calls of the attempt sent (`earlier`, already read).
// The documents' own words as this operation sent them: every source passage
// of this call, and the material its earlier calls sent. A symbol standing in
// them is sourced, wherever its row first met its fact - the Matter's memory,
// an earlier answer - since the model read it in a document.
const std::string &material_of(const std::string &sent_prompt, const std::string &earlier) {
	thread_local std::size_t cachedHash = 0;
	thread_local std::size_t cachedSize = static_cast<std::size_t>(-1);
	thread_local std::size_t cachedEarlier = static_cast<std::size_t>(-1);
	thread_local std::string material;
	const std::size_t hash = std::hash<std::string>{}(sent_prompt) ^ (std::hash<std::string>{}(earlier) << 1);
	if (hash == cachedHash && sent_prompt.size() == cachedSize && earlier.size() == cachedEarlier) return material;
	material.clear();
	append_segments(sent_prompt, [](compute::PayloadSegment::Kind kind) { return kind == compute::PayloadSegment::Kind::Source; },
		[&](const std::string &part) { material += part + "\n"; });
	material += earlier;
	cachedHash = hash;
	cachedSize = sent_prompt.size();
	cachedEarlier = earlier.size();
	return material;
}

const Grounding &grounding_of(const std::string &sent_prompt, const std::string &earlier, semantic::Language language) {
	thread_local std::size_t cachedHash = 0;
	thread_local std::size_t cachedSize = static_cast<std::size_t>(-1);
	thread_local std::size_t cachedEarlier = static_cast<std::size_t>(-1);
	thread_local semantic::Language cachedLanguage = semantic::Language::English;
	thread_local Grounding grounding;
	const std::size_t hash = std::hash<std::string>{}(sent_prompt) ^ (std::hash<std::string>{}(earlier) << 1);
	if (hash == cachedHash && sent_prompt.size() == cachedSize && earlier.size() == cachedEarlier && language == cachedLanguage)
		return grounding;
	grounding = {};
	append_segments(sent_prompt, [](compute::PayloadSegment::Kind kind) {
		return kind == compute::PayloadSegment::Kind::Source || kind == compute::PayloadSegment::Kind::Request
			|| kind == compute::PayloadSegment::Kind::LawyerAnswer;
	}, [&](const std::string &part) { grounding.text += part + "\n"; });
	grounding.text += earlier;
	for (const semantic::RecognisedFigure &figure : semantic::FigureRecogniser::find(grounding.text, language))
		if (figure.value && !figure.structural) grounding.keys.insert(semantic::value_key(*figure.value));
	cachedHash = hash;
	cachedSize = sent_prompt.size();
	cachedEarlier = earlier.size();
	cachedLanguage = language;
	return grounding;
}

// An identifier's number: "No. 17", "№ 17", "#17", "Nr. 4" - a name for a
// thing, never a quantity the answer states.
bool identifier_number(std::string_view text, std::size_t start) {
	const std::size_t end = skip_spaces_back(text, start);
	for (const char *mark : {"No.", "No", "Nos.", "Nr.", "#", "\xE2\x84\x96" /* № */}) {
		const std::size_t length = std::string_view(mark).size();
		if (end < length || text.compare(end - length, length, mark) != 0) continue;
		if (end > length && is_letter_byte(text[end - length - 1])) continue;
		return true;
	}
	return false;
}

// The figure the recogniser finds at `offset`: the earliest-starting, longest
// one that covers it. Nothing when it finds none there.
const semantic::RecognisedFigure *figure_covering(const std::vector<semantic::RecognisedFigure> &figures, std::size_t offset) {
	const semantic::RecognisedFigure *best = nullptr;
	for (const semantic::RecognisedFigure &figure : figures) {
		if (figure.length == 0 || offset < figure.start || offset >= figure.start + figure.length) continue;
		if (best == nullptr || figure.start < best->start || (figure.start == best->start && figure.length > best->length))
			best = &figure;
	}
	return best;
}

// ---------------------------------------------------------------------------
// Spans a withheld figure covers, and what stands there instead.

// The symbol, the statement or the figure that starts at `at`.
std::size_t figure_length(std::string_view text, std::size_t at) {
	if (at >= text.size()) return 0;
	if (text.compare(at, 2, "[[") == 0) {
		const std::size_t close = text.find("]]", at + 2);
		return close == kNone ? 2 : close + 2 - at;
	}
	if (text.compare(at, 2, "{{") == 0) {
		const std::size_t close = text.find("}}", at + 2);
		return close == kNone ? 2 : close + 2 - at;
	}
	std::size_t cursor = at;
	// A sign before the figure.
	for (const char *sign : {"US$", "$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"})
		if (text.compare(cursor, std::string_view(sign).size(), sign) == 0) {
			cursor += std::string_view(sign).size();
			break;
		}
	if (cursor < text.size() && is_digit(text[cursor])) {
		while (cursor < text.size()) {
			if (is_digit(text[cursor])) {
				++cursor;
				continue;
			}
			// A separator inside the figure: followed by a digit.
			std::size_t next = cursor;
			if (text[cursor] == ',' || text[cursor] == '.' || text[cursor] == ' ' || text[cursor] == '\'') next = cursor + 1;
			else if (text.compare(cursor, 2, "\xC2\xA0") == 0) next = cursor + 2;
			else if (text.compare(cursor, 3, "\xE2\x80\xAF") == 0) next = cursor + 3;
			if (next != cursor && next < text.size() && is_digit(text[next])) {
				cursor = next;
				continue;
			}
			break;
		}
		std::size_t after = skip_spaces(text, cursor);
		if (after < text.size() && text[after] == '%') return after + 1 - at;
		const std::string ahead = folded_ahead(text, after, 16);
		for (const char *unit : {"usd", "eur", "rub", "gbp", "\xE2\x82\xBD", "\xD1\x80\xD1\x83\xD0\xB1."}) {
			if (!starts_with(ahead, unit)) continue;
			return after + std::string_view(unit).size() - at;
		}
		return cursor - at;
	}
	// Words: the longest run from here that still reads as a number.
	std::size_t end = cursor;
	std::size_t best = 0;
	while (end < text.size() && end - cursor < 160 && text[end] != '\n' && text[end] != ')' && text[end] != ','
			&& text[end] != '.' && text[end] != ';') {
		++end;
		if (end >= text.size() || text[end] == ' ' || text[end] == ')' || text[end] == ',' || text[end] == '.') {
			const std::string words = trimmed(text.substr(cursor, end - cursor));
			if (semantic::parse_number_words(words)) best = end;
		}
	}
	return best > cursor ? best - at : std::max<std::size_t>(1, end - at);
}

enum class GapKind { Amount, Date, Rate };

GapKind gap_kind(std::string_view text, std::size_t at, std::size_t length) {
	const std::string written(text.substr(at, length));
	if (starts_with(written, "[[")) {
		const std::string letters = lowered_ascii(letters_of(written));
		if (letters == "date") return GapKind::Date;
		if (letters == "rate") return GapKind::Rate;
		return GapKind::Amount;
	}
	const std::string lower = folded(written);
	if (lower.find('%') != kNone || lower.find("percent") != kNone
			|| lower.find("\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82") != kNone)
		return GapKind::Rate;
	try {
		if (const std::optional<semantic::Value> value = semantic::parse_value(written))
			if (value->kind == semantic::Kind::Date || value->kind == semantic::Kind::DayOfYear) return GapKind::Date;
	} catch (const std::exception &) {
	}
	return GapKind::Amount;
}

// Where a figure of the model's own stands in the text, and the gap that
// stands there instead: the recogniser's own span - a date whole with its
// month and year, an amount with its sign, its magnitude word and the words
// that spell it - so no part of a guessed figure stays in the lawyer's
// document. A period keeps its unit: only its number becomes the gap.
struct FigureSpan {
	std::size_t start = 0;
	std::size_t length = 0;
	GapKind kind = GapKind::Amount;
};

FigureSpan raw_span(const std::string &text, std::size_t offset, const std::vector<semantic::RecognisedFigure> &figures) {
	FigureSpan span;
	span.start = offset;
	const semantic::RecognisedFigure *figure = figure_covering(figures, offset);
	if (figure == nullptr) {
		span.length = std::max<std::size_t>(1, figure_length(text, offset));
		span.kind = gap_kind(text, offset, span.length);
		return span;
	}
	span.start = figure->start;
	span.length = figure->length;
	switch (figure->kind) {
		case semantic::RecognisedFigure::Kind::Date: {
			span.kind = GapKind::Date;
			// «…»: the year word is the date's own.
			const std::size_t after = skip_spaces(text, span.start + span.length);
			for (const char *word : {"\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xB0", "\xD0\xB3."}) {
				const std::size_t length = std::string_view(word).size();
				if (text.compare(after, length, word) != 0) continue;
				if (word[length - 1] != '.' && !word_ends_at(text, after + length)) continue;
				span.length = after + length - span.start;
				break;
			}
			return span;
		}
		case semantic::RecognisedFigure::Kind::Percentage: span.kind = GapKind::Rate; break;
		case semantic::RecognisedFigure::Kind::Duration: {
			const std::size_t number = figure_length(text, figure->start);
			if (number > 0 && number < figure->length) span.length = number;
			return span;
		}
		default: break;
	}
	if (figure->value)
		if (const std::optional<std::size_t> words = restoration_grammar::spelling_after(text, span.start + span.length,
				*figure->value))
			span.length += *words;
	return span;
}

std::string gap_text(GapKind kind, semantic::Language language) {
	namespace control = singularity::context::control_text;
	const bool russian = language == semantic::Language::Russian;
	switch (kind) {
		case GapKind::Date: return control::render(russian ? control::Template::FigureGapDateRu : control::Template::FigureGapDateEn);
		case GapKind::Rate: return control::render(russian ? control::Template::FigureGapRateRu : control::Template::FigureGapRateEn);
		case GapKind::Amount: break;
	}
	return control::render(russian ? control::Template::FigureGapAmountRu : control::Template::FigureGapAmountEn);
}

// Whether a "." at `at` closes an abbreviation or an initial ("No. 17",
// "Mr. Smith", "J. Smith", «…») rather than a sentence.
bool abbreviation_before(std::string_view text, std::size_t at) {
	std::size_t from = at;
	while (from > 0 && (is_letter_byte(text[from - 1]) || (static_cast<unsigned char>(text[from - 1]) & 0xC0) == 0x80
			|| text[from - 1] == '.'))
		--from;
	const std::string word = folded(text.substr(from, at - from));
	if (word.empty()) return false;
	// One letter: an initial or «…».
	std::size_t letters = 0;
	for (std::size_t index = 0; index < word.size(); ++index)
		if ((static_cast<unsigned char>(word[index]) & 0xC0) != 0x80 && word[index] != '.') ++letters;
	if (letters == 1) return true;
	static const std::set<std::string> known{"no", "nos", "nr", "mr", "mrs", "ms", "dr", "st", "inc", "ltd", "co", "corp",
		"vs", "e.g", "i.e", "art", "arts", "sec", "secs", "para", "paras", "pp", "cl", "ch", "fig", "approx", "ref", "cf",
		"jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec",
		"\xD1\x81\xD1\x82",                              // …
		"\xD0\xBF\xD0\xBF",                              // …
		"\xD0\xB3\xD0\xB3",                              // …
		"\xD1\x80\xD1\x83\xD0\xB1",                    // …
		"\xD0\xBA\xD0\xBE\xD0\xBF",                    // …
		"\xD1\x82\xD1\x8B\xD1\x81",                    // …
		"\xD0\xBC\xD0\xBB\xD0\xBD",                    // …
		"\xD0\xBC\xD0\xBB\xD1\x80\xD0\xB4",          // …
		"\xD1\x81\xD0\xBC",                              // …
		"\xD1\x81\xD1\x82\xD1\x80",                    // …
		"\xD1\x83\xD0\xBB",                              // …
		"\xD1\x82.\xD0\xB5",                             // …
		"\xD0\xB8\xD0\xBC"};                             // …
	return known.count(word) > 0;
}

// Whether the text from `at` goes on as the same sentence: a lower-case
// letter (Latin or Cyrillic), a digit, or a bracket that continues a figure.
bool continues_sentence(std::string_view text, std::size_t at) {
	at = skip_spaces(text, at);
	if (at >= text.size()) return false;
	const auto byte = static_cast<unsigned char>(text[at]);
	if ((text[at] >= 'a' && text[at] <= 'z') || is_digit(text[at])) return true;
	if (at + 1 < text.size()) {
		const auto next = static_cast<unsigned char>(text[at + 1]);
		if (byte == 0xD0 && next >= 0xB0 && next <= 0xBF) return true;  // …
		if (byte == 0xD1 && next >= 0x80 && next <= 0x8F) return true;  // …
	}
	return false;
}

// The sentences of a chat answer, each with the space after it: a sentence
// ends at "!", "?" or a "." that closes no abbreviation, before a space and
// what begins a new sentence; every line ends one.
std::vector<std::pair<std::size_t, std::size_t>> sentences_of(const std::string &text) {
	std::vector<std::pair<std::size_t, std::size_t>> sentences;
	std::size_t start = 0;
	for (std::size_t at = 0; at < text.size(); ++at) {
		const char character = text[at];
		std::size_t end = kNone;
		if (character == '\n') {
			end = at + 1;
		} else if ((character == '.' || character == '!' || character == '?')
				&& (at + 1 >= text.size() || text[at + 1] == ' ' || text[at + 1] == '\n')) {
			if (character == '.' && at + 1 < text.size() && text[at + 1] == ' '
					&& (abbreviation_before(text, at) || continues_sentence(text, at + 1)))
				continue;
			end = at + 1;
			while (end < text.size() && text[end] == ' ') ++end;
		}
		if (end == kNone) continue;
		sentences.emplace_back(start, end);
		start = end;
		at = end - 1;
	}
	if (start < text.size()) sentences.emplace_back(start, text.size());
	return sentences;
}

// A value of the kind a symbol's declared type or prefix names: all the
// spelled-words reader looks at (never at the amount).
std::optional<semantic::Value> kind_of_quantity(Quantity quantity) {
	switch (quantity) {
		case Quantity::Money: return semantic::make_money(semantic::Rational(0), "");
		case Quantity::Rate: return semantic::make_percentage(semantic::Rational(0));
		case Quantity::Duration: return semantic::make_duration(semantic::Rational(0), semantic::DurationUnit::Day);
		case Quantity::Number: return semantic::make_number(semantic::Rational(0));
		case Quantity::Count: {
			semantic::Value count = semantic::make_number(semantic::Rational(0));
			count.kind = semantic::Kind::Count;
			return count;
		}
		default: return std::nullopt;
	}
}

// Where the words spelling each quantitative symbol's figure stand after it.
std::vector<std::pair<std::size_t, std::size_t>> spelled_spans(const std::string &text, const std::vector<Symbol> &symbols) {
	std::vector<std::pair<std::size_t, std::size_t>> spans;
	for (const Symbol &symbol : symbols) {
		const std::optional<semantic::Value> kind = kind_of_quantity(symbol.quantity);
		if (!kind) continue;
		const std::size_t end = symbol.start + symbol.length;
		if (const std::optional<std::size_t> length = restoration_grammar::spelling_after(text, end, *kind))
			spans.emplace_back(end, *length);
	}
	return spans;
}

bool has_letter(std::string_view text) {
	for (const char character : text)
		if (is_letter_byte(character)) return true;
	return false;
}

} // namespace

// ---------------------------------------------------------------------------

void remember_sent_material(OperationProtectedSymbolTable &table, const std::string &sent_prompt) {
	// The lawyer's material only: a request of a later call may carry what Singularity
	// assembled around the lawyer's words (a draft under review), which
	// is the model's, never a ground.
	append_segments(sent_prompt, [](compute::PayloadSegment::Kind kind) {
		return kind == compute::PayloadSegment::Kind::Source || kind == compute::PayloadSegment::Kind::LawyerAnswer;
	}, [&](const std::string &part) {
		if (part.empty() || !table.sent_material_digests.insert(std::hash<std::string>{}(part)).second) return;
		table.sent_material += part + "\n";
	});
}

ContractResult check(const std::string &protected_text, const std::string &json_pointer,
		const OperationProtectedSymbolTable &table, const std::string &sent_prompt, semantic::Language language) {
	ContractResult result;
	if (protected_text.empty()) return result;
	const TableIndex index(table);
	const Atoms atoms{index};
	const std::vector<Statement> statements = statements_in(protected_text);
	const auto add = [&](const char *code, const std::string &symbol, std::size_t offset, bool soft = false) {
		result.violations.push_back({code, symbol, offset, json_pointer, soft});
	};

	// Bounded ends, by their facts' keys (DESIGN4 §4.1 Certainty).
	std::map<std::string, Direction> boundEnds;
	for (const auto &[placeholder, fact] : table.derived_facts) {
		if (fact.certainty.kind != compute::Certainty::Kind::Bounded) continue;
		if (!fact.certainty.lo_fact_key.empty()) boundEnds[fact.certainty.lo_fact_key] = Direction::Lower;
		if (!fact.certainty.hi_fact_key.empty()) boundEnds[fact.certainty.hi_fact_key] = Direction::Upper;
	}

	/* - - - - - - */

	// Every symbol, as the table knows it.
	std::vector<Symbol> symbols;
	for (const markers::Parsed &parsed : markers::find_all(protected_text)) {
		if (protected_text.compare(parsed.start, 2, "[[") != 0) continue;
		const Statement *inside = statement_around(statements, parsed.start);
		if (inside != nullptr && inside->forbidden) continue;
		symbols.push_back(symbol_at(parsed, index, sent_prompt));
	}
	for (const Symbol &symbol : symbols) {
		if (symbol.row == nullptr && symbol.derived == nullptr) {
			// A symbol never issued is named here - a quantity, or a party, a
			// person, an identifier - whatever the table holds: what is restored
			// shows a gap in its place (ProtectedInference), and the model may
			// write it again properly. The protocol's own handles (a table, a row
			// set, a schedule, a calendar) are no fact.
			static const std::set<std::string> entityKinds{"Org", "Person", "Place", "Id", "Email", "Phone", "Court",
				"Address", "Location"};
			std::size_t letters = 2;
			while (letters < symbol.base.size() && std::isalpha(static_cast<unsigned char>(symbol.base[letters])) != 0) ++letters;
			const std::string kind = symbol.base.size() > 2 ? symbol.base.substr(2, letters - 2) : std::string();
			if (symbol.quantity != Quantity::None || entityKinds.count(kind) != 0) add("unknown_symbol", "", symbol.start);
			continue;
		}
		if (symbol.quantity == Quantity::None) continue;
		++result.quantitative_symbols;
		if (!symbol.belongs) {
			add("unknown_symbol", "", symbol.start);
			continue;
		}
		// Sourced, or deterministically derived, with provenance to the material.
		if (symbol.derived != nullptr) {
			if (symbol.derived->certainty.kind == compute::Certainty::Kind::Unknown) {
				add("derivation_failed", symbol.base, symbol.start);
				continue;
			}
			if (!provenance_complete(*symbol.derived, index)) {
				add("provenance_incomplete", symbol.base, symbol.start);
				continue;
			}
		} else if (symbol.row->derived) {
			if (symbol.row->provenance.derivation.empty()) {
				add("not_sourced", symbol.base, symbol.start);
				continue;
			}
		} else if (!located(symbol.row->provenance)
				&& material_of(sent_prompt, table.sent_material).find(symbol.row->placeholder) == std::string::npos) {
			// Neither its row's own place nor any document passage sent holds it.
			add("not_sourced", symbol.base, symbol.start);
			continue;
		}
		if (!type_fits_context(symbol, protected_text)) add("type_in_context", symbol.base, symbol.start);
		const auto bound = symbol.fact_key.empty() ? boundEnds.end() : boundEnds.find(symbol.fact_key);
		if (bound != boundEnds.end()) {
			const Direction written = bound_word_before(protected_text, symbol.start, symbol.start + symbol.length);
			if (written == Direction::None) add("bound_as_exact", symbol.base, symbol.start);
			else if (written != bound->second) add("bound_direction", symbol.base, symbol.start);
		}
		if (symbol.derived != nullptr && needs_sign(*symbol.derived) && !sign_observed(*symbol.derived, table))
			add("sign_unobserved", symbol.base, symbol.start);
	}

	/* - - - - - - */

	// Claims: structure only.
	for (const Claim &claim : claims_in(protected_text, symbols, statements, atoms)) {
		const auto found = std::find_if(symbols.begin(), symbols.end(),
			[&](const Symbol &symbol) { return symbol.start == claim.offset; });
		if (found == symbols.end() || found->quantity == Quantity::None || !found->belongs) continue;
		if (!claim_recorded(claim, index)) add("claim_not_computed", found->base, claim.offset);
	}

	// Calculations written as statements (D6): they leave the protected
	// protocol where it replaces them - a call that offered the model its
	// operations - unless the same call still told it to write statements (a
	// contract never contradicts what the model was told).
	const bool computeOffered = sent_prompt.find(compute::compute_instruction(compute::Carrier::FinalField)) != kNone
		|| sent_prompt.find(compute::compute_instruction(compute::Carrier::NativeTool)) != kNone
		|| sent_prompt.find("Singularity does all arithmetic, comparisons, date and table work") != kNone;
	const bool statementsOffered = sent_prompt.find(singularity::context::computed_statements::kInstruction) != kNone
		|| sent_prompt.find("as {{= expression}}") != kNone;
	for (const Statement &statement : statements)
		if (statement.forbidden && computeOffered && !statementsOffered) add("statement_not_allowed", "", statement.start);

	/* - - - - - - */

	// Figures of the model's own: one recogniser for the answer and for what
	// the material writes (DESIGN4 §8.2), with the cycle-3 figure core beside it
	// where the recogniser finds nothing. Grounded only in the lawyer's
	// material as sent: nothing the model wrote, sent back to it, grounds one.
	const Grounding &grounding = grounding_of(sent_prompt, table.sent_material, language);
	const std::vector<semantic::RecognisedFigure> found = semantic::FigureRecogniser::find(protected_text, language);
	std::vector<std::size_t> raw;
	const operations::FigureCheck figures = operations::check_figures(protected_text, grounding.text, table.mapping);
	for (const std::size_t offset : figures.untraced) {
		if (protected_text.compare(offset, 2, "[[") == 0) continue;
		// Where the recogniser reads a figure, its verdict is the one below.
		if (figure_covering(found, offset) != nullptr) continue;
		if (identifier_number(protected_text, offset)) continue;
		raw.push_back(offset);
	}
	const auto insideSymbolOrStatement = [&](std::size_t at, std::size_t length) {
		for (const Symbol &symbol : symbols)
			if (at < symbol.start + symbol.length && symbol.start < at + length) return true;
		for (const Statement &statement : statements)
			if (at < statement.end && statement.start < at + length) return true;
		return false;
	};
	for (const semantic::RecognisedFigure &figure : found) {
		if (figure.structural || figure.incidental || figure.length == 0) continue;
		if (insideSymbolOrStatement(figure.start, figure.length)) continue;
		if (identifier_number(protected_text, figure.start)) continue;
		if (grounding.text.find(protected_text.substr(figure.start, figure.length)) != kNone) continue;
		if (figure.value && (safe_constant(*figure.value) || grounding.keys.count(semantic::value_key(*figure.value)) > 0))
			continue;
		// A count the material writes as a bare number - a ledger's "E7 = 19",
		// written back as "…": the same count, with its unit said (live R2,
		// 27 Sep 2026: correct day counts were cut from the answer).
		if (figure.value && (figure.value->kind == semantic::Kind::Duration || figure.value->kind == semantic::Kind::Count)
				&& figure.value->amount.is_integer()
				&& grounding.keys.count(semantic::value_key(semantic::make_number(figure.value->amount))) > 0)
			continue;
		raw.push_back(figure.start);
	}
	// Words right after a figure's own symbol that spell some figure -
	// "[[Amount9]] (eight hundred thousand) dollars" - are that symbol's words:
	// restoration writes them again from the value it restores, so they are
	// never the model's figure (RestorationGrammar/SpelledFigures).
	const std::vector<std::pair<std::size_t, std::size_t>> spelled = spelled_spans(protected_text, symbols);
	const auto spelledAt = [&](std::size_t at) {
		for (const auto &[from, length] : spelled)
			if (at >= from && at < from + length) return true;
		return false;
	};
	raw.erase(std::remove_if(raw.begin(), raw.end(), spelledAt), raw.end());
	std::sort(raw.begin(), raw.end());
	std::size_t coveredTo = 0;
	for (const std::size_t offset : raw) {
		if (offset < coveredTo) continue;
		add("raw_figure", "", offset);
		const FigureSpan span = raw_span(protected_text, offset, found);
		result.violations.back().written = protected_text.substr(span.start, span.length);
		coveredTo = span.start + span.length;
	}

	/* - - - - - - */

	// Soft nudges: each given once by the caller, never a failure.
	{
		const std::string request = folded(section_of(sent_prompt, "CURRENT_REQUEST"));
		const std::string answer = folded(protected_text);
		const bool asksForFigure = !request.empty() && contains_any(request, {"how much", "amount", "how many", "total",
			"owed", "outstanding", "balance", "deadline", "due date", "when ", "what date", "which date", "interest",
			"penalty", "shortfall",
			"\xD1\x81\xD0\xBA\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xBA\xD0\xBE",                       // …
			"\xD1\x81\xD1\x83\xD0\xBC\xD0\xBC",                                               // …
			"\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80",                               // …
			"\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA",                                               // …
			"\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xB4\xD0\xB0",                                       // …
			"\xD0\xB7\xD0\xB0\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBD",       // …
			"\xD0\xBD\xD0\xB5\xD1\x83\xD1\x81\xD1\x82\xD0\xBE\xD0\xB9\xD0\xBA",               // …
			"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82"});                     // …
		static const char *undetermined[] = {"cannot be determined", "can't be determined", "cannot determine",
			"can't determine", "cannot be calculated", "can't be calculated", "cannot calculate", "cannot be computed",
			"not possible to determine", "not possible to calculate", "unable to determine", "unable to calculate",
			"impossible to determine", "impossible to calculate", "cannot be established",
			"\xD0\xBD\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB7\xD0\xBC\xD0\xBE\xD0\xB6\xD0\xBD\xD0\xBE \xD0\xBE\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C",  // …
			"\xD0\xBD\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xB7\xD1\x8F \xD0\xBE\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C",  // …
			"\xD0\xBD\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F \xD0\xB2\xD0\xBE\xD0\xB7\xD0\xBC\xD0\xBE\xD0\xB6\xD0\xBD\xD1\x8B\xD0\xBC",  // …
			"\xD0\xB1\xD0\xB5\xD0\xB7 \xD0\xBF\xD0\xBE\xD0\xB4\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBA\xD0\xB8",  // …
			"\xD0\xBD\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB7\xD0\xBC\xD0\xBE\xD0\xB6\xD0\xBD\xD0\xBE \xD1\x80\xD0\xB0\xD1\x81\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C",  // …
			"\xD0\xBD\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xB7\xD1\x8F \xD1\x80\xD0\xB0\xD1\x81\xD1\x81\xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C"};  // …
		std::size_t undeterminedAt = kNone;
		for (const char *phrase : undetermined) {
			const std::size_t at = answer.find(phrase);
			if (at != kNone && (undeterminedAt == kNone || at < undeterminedAt)) undeterminedAt = at;
		}
		const bool quantitiesSent = contains_any(sent_prompt, {"[[Amount", "[[Date", "[[Rate", "[[Duration", "[[Number"})
			|| std::any_of(table.mapping.begin(), table.mapping.end(), [&](const RequestMappingEntry &row) {
				return quantity_of_row(row) != Quantity::None && sent_prompt.find(row.placeholder) != kNone;
			});
		// The offset of a folded text is near enough: a nudge never withholds.
		if (asksForFigure && undeterminedAt != kNone && table.op_log.empty() && quantitiesSent)
			add("compute_not_attempted", "", std::min(undeterminedAt, protected_text.size()), true);
	}
	{
		// "same role, two sources" lines (DESIGN4 §5.7): an answer citing one of
		// the two before any operation checked them against each other.
		std::vector<std::vector<std::string>> pairs;
		for (std::size_t from = 0; from < sent_prompt.size();) {
			std::size_t end = sent_prompt.find('\n', from);
			if (end == kNone) end = sent_prompt.size();
			const std::string line = sent_prompt.substr(from, end - from);
			from = end + 1;
			const std::string lower = folded(line);
			if (lower.find("same role, two sources") == kNone
					&& lower.find("\xD0\xBE\xD0\xB4\xD0\xBD\xD0\xB0 \xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C") == kNone)  // …
				continue;
			std::vector<std::string> sides;
			for (const markers::Parsed &parsed : markers::find_all(line))
				if (line.compare(parsed.start, 2, "[[") == 0) sides.push_back("[[" + parsed.base + "]]");
			if (sides.size() >= 2) pairs.push_back(std::move(sides));
		}
		for (const std::vector<std::string> &sides : pairs) {
			bool checked = false;
			for (const compute::OpRecord &record : table.op_log) {
				std::size_t seen = 0;
				for (const std::string &side : sides)
					if (std::find(record.operand_symbols.begin(), record.operand_symbols.end(), side) != record.operand_symbols.end()
							|| record.request_canonical.find(side) != kNone)
						++seen;
				if (seen >= 2) checked = true;
			}
			if (checked) continue;
			for (const Symbol &symbol : symbols) {
				const bool cited = std::find(sides.begin(), sides.end(), symbol.base) != sides.end()
					|| (symbol.derived != nullptr && std::any_of(symbol.derived->operand_symbols.begin(),
						symbol.derived->operand_symbols.end(), [&](const std::string &operand) {
							return std::find(sides.begin(), sides.end(), operand) != sides.end();
						}));
				if (!cited) continue;
				add("unchecked_cross_source", symbol.base, symbol.start, true);
				break;
			}
		}
	}

	std::stable_sort(result.violations.begin(), result.violations.end(),
		[](const ContractViolation &left, const ContractViolation &right) { return left.offset < right.offset; });
	return result;
}

Withheld withhold(const std::string &protected_text, const std::vector<ContractViolation> &violations,
		Surface surface, semantic::Language language) {
	// Where each unverifiable figure stands, and the gap for it; one figure once.
	std::vector<FigureSpan> spans;
	std::optional<std::vector<semantic::RecognisedFigure>> figures;
	for (const ContractViolation &violation : violations) {
		if (violation.soft || violation.offset >= protected_text.size()) continue;
		const bool isSymbol = protected_text.compare(violation.offset, 2, "[[") == 0;
		const bool isStatement = protected_text.compare(violation.offset, 2, "{{") == 0;
		if (!isSymbol && !isStatement) {
			// A figure of the model's own: the recogniser's span, as check() read it.
			if (!figures) figures = semantic::FigureRecogniser::find(protected_text, language);
			FigureSpan span = raw_span(protected_text, violation.offset, *figures);
			// A currency sign right before a figure is the figure's own ("$1,275").
			for (const char *sign : {"US$", "$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"}) {
				const std::size_t size = std::string_view(sign).size();
				if (span.start < size || protected_text.compare(span.start - size, size, sign) != 0) continue;
				span.start -= size;
				span.length += size;
				break;
			}
			spans.push_back(span);
			continue;
		}
		FigureSpan span;
		span.start = violation.offset;
		span.length = std::max<std::size_t>(1, figure_length(protected_text, violation.offset));
		span.kind = gap_kind(protected_text, span.start, span.length);
		// A symbol goes with the words that spell it: stale words never stay
		// beside a gap or after a sentence was cut.
		if (isSymbol) {
			const std::vector<markers::Parsed> parsed = markers::find_all(protected_text.substr(span.start, span.length));
			const std::optional<semantic::Value> kind = parsed.size() == 1
				? kind_of_quantity(quantity_of_prefix(letters_of(parsed.front().base))) : std::nullopt;
			if (kind)
				if (const std::optional<std::size_t> words = restoration_grammar::spelling_after(protected_text,
						span.start + span.length, *kind))
					span.length += *words;
		}
		spans.push_back(span);
	}
	if (spans.empty()) return {protected_text, 0};
	std::sort(spans.begin(), spans.end(), [](const FigureSpan &left, const FigureSpan &right) {
		return left.start < right.start || (left.start == right.start && left.length > right.length);
	});
	std::vector<FigureSpan> distinct;
	for (const FigureSpan &span : spans) {
		if (!distinct.empty() && span.start < distinct.back().start + distinct.back().length) continue;
		distinct.push_back(span);
	}

	Withheld withheld;
	withheld.figures = distinct.size();
	if (surface == Surface::Artifact) {
		// In a document only the figure goes, and a visible gap stands in its
		// place: the sentence around it is the lawyer's work.
		std::string text = protected_text;
		for (auto span = distinct.rbegin(); span != distinct.rend(); ++span)
			text.replace(span->start, span->length, gap_text(span->kind, language));
		withheld.text = std::move(text);
		return withheld;
	}
	// In a chat answer the sentence that carries it is left out.
	std::string text;
	for (const std::pair<std::size_t, std::size_t> &sentence : sentences_of(protected_text)) {
		const std::size_t start = sentence.first;
		const std::size_t end = sentence.second;
		const bool carries = std::any_of(distinct.begin(), distinct.end(),
			[&](const FigureSpan &span) { return span.start >= start && span.start < end; });
		if (!carries) text.append(protected_text, start, end - start);
	}
	// What leaving out sentences left behind: no run of blank lines, no
	// trailing space.
	std::string tidy;
	std::size_t newlines = 0;
	for (const char character : text) {
		if (character == '\n') {
			while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
			if (++newlines > 2) continue;
		} else {
			newlines = 0;
		}
		tidy += character;
	}
	while (!tidy.empty() && is_space(tidy.back())) tidy.pop_back();
	while (!tidy.empty() && is_space(tidy.front())) tidy.erase(tidy.begin());
	withheld.text = has_letter(tidy) ? tidy : std::string();
	return withheld;
}

} // namespace singularity::protected_api::papinian::answer_contract
