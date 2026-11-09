#include "Context/ComputedStatements/ComputedStatements.hpp"

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "Context/RussianNames/RussianNames.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace singularity::context::computed_statements {
namespace {

namespace semantic = singularity::protected_api::papinian::semantic;
using semantic::Language;

constexpr std::string_view kOpen = "\xE2\x9F\xA6";  // ⟦
constexpr std::string_view kClose = "\xE2\x9F\xA7"; // ⟧
constexpr std::size_t kNone = std::string::npos;
// Statements inside statements, as deep as a model writes them and no deeper.
constexpr int kDeepest = 16;

bool starts_with_at(std::string_view text, std::size_t position, std::string_view literal) {
	return position <= text.size() && text.compare(position, literal.size(), literal) == 0;
}

std::string lower_ascii(std::string value) {
	for (char &character : value)
		if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
	return value;
}

bool is_space(char character) { return character == ' ' || character == '\t' || character == '\n' || character == '\r'; }

std::string trimmed(std::string text) {
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.erase(text.begin());
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.pop_back();
	return text;
}

std::string say(Language language, const std::string &english, const std::string &russian) {
	return language == Language::Russian ? russian : english;
}

// Where the ⟦…⟧ opening at `position` ends (the text's end when it is never closed).
std::size_t operand_end(std::string_view text, std::size_t position) {
	const std::size_t close = text.find(kClose, position + kOpen.size());
	return close == kNone ? text.size() : close + kClose.size();
}

// ---- One statement in the text --------------------------------------------------------------

enum class StatementKind { Value, Condition, Case, Gender };

// "{{= head}}" or "{{if head}}yes{{else}}no{{end}}", found at `start`.
struct Statement {
	StatementKind kind = StatementKind::Value;
	std::size_t start = 0;
	std::size_t end = 0;
	std::size_t head_start = 0;
	std::size_t head_end = 0;
	std::string when_true;
	std::string when_false;
	bool unclosed = false;  // an {{if}} the model never closed with {{end}}
};

// Where `word` (any case) stands at `at` as a whole word, the position after it.
std::size_t word_at(std::string_view text, std::size_t at, std::string_view word) {
	if (at > text.size() || lower_ascii(std::string(text.substr(at, word.size()))) != word) return kNone;
	const std::size_t after = at + word.size();
	if (after < text.size() && (std::isalnum(static_cast<unsigned char>(text[after])) != 0 || text[after] == '_')) return kNone;
	return after;
}

// The keyword a "{{" opens: "=", "if", "elseif", "else", "end", "case" or
// "gender" (spaces and case forgiven), with where the rest of the tag begins;
// empty for anything else. Models write an {{if}} chain the ways templates
// do: "{{else if C}}" and "{{elseif C}}" are "elseif", "{{/if}}" and
// "{{endif}}" are "end".
std::string tag_at(std::string_view text, std::size_t position, std::size_t &rest) {
	if (!starts_with_at(text, position, "{{")) return {};
	std::size_t cursor = position + 2;
	while (cursor < text.size() && text[cursor] == ' ') ++cursor;
	if (cursor < text.size() && text[cursor] == '=') {
		rest = cursor + 1;
		return "=";
	}
	std::size_t after = kNone;
	if (cursor < text.size() && text[cursor] == '/') {
		std::size_t word = cursor + 1;
		while (word < text.size() && text[word] == ' ') ++word;
		if ((after = word_at(text, word, "if")) == kNone) return {};
		rest = after;
		return "end";
	}
	if ((after = word_at(text, cursor, "endif")) != kNone) {
		rest = after;
		return "end";
	}
	for (const char *word : {"elseif", "elif"}) {
		if ((after = word_at(text, cursor, word)) == kNone) continue;
		rest = after;
		return "elseif";
	}
	if ((after = word_at(text, cursor, "else")) != kNone) {
		std::size_t next = after;
		while (next < text.size() && text[next] == ' ') ++next;
		const std::size_t condition = next > after ? word_at(text, next, "if") : kNone;
		rest = condition == kNone ? after : condition;
		return condition == kNone ? "else" : "elseif";
	}
	for (const char *word : {"if", "end", "case", "gender"}) {
		if ((after = word_at(text, cursor, word)) == kNone) continue;
		rest = after;
		return word;
	}
	return {};
}

bool is_statement_tag(const std::string &tag) { return tag == "=" || tag == "if" || tag == "case" || tag == "gender"; }

// The "}}" closing a head that begins at `from`: the one that balances every
// "{{" opened inside it, so "{{= max({{if a > b}}c{{else}}0{{end}}, d)}}" is
// one head. What stands inside ⟦ ⟧ is a value, never a brace.
std::size_t balanced_close(std::string_view text, std::size_t from) {
	int depth = 0;
	std::size_t index = from;
	while (index + 1 < text.size()) {
		if (starts_with_at(text, index, kOpen)) {
			index = operand_end(text, index);
			continue;
		}
		if (text[index] == '{' && text[index + 1] == '{') {
			++depth;
			index += 2;
			continue;
		}
		if (text[index] == '}' && text[index + 1] == '}') {
			if (depth == 0) return index;
			--depth;
			index += 2;
			continue;
		}
		++index;
	}
	return kNone;
}

// "{{{else}}}": a tag a template language wrote in three braces, whose outer
// ones are the template's. The tag's first byte (`at`) and its end (just
// past its "}}" at `close`), widened over them.
std::pair<std::size_t, std::size_t> tag_span(std::string_view text, std::size_t at, std::size_t close) {
	const bool tripled = at > 0 && text[at - 1] == '{' && close + 2 < text.size() && text[close + 2] == '}';
	return tripled ? std::make_pair(at - 1, close + 3) : std::make_pair(at, close + 2);
}

// The words an {{if}} chooses between, to its own {{else}} and {{end}}:
// statements inside them are skipped whole. An {{else if C}} is an {{else}}
// whose words are an {{if C}} of their own, closed by the same {{end}}.
struct Block {
	std::size_t else_at = kNone;
	std::size_t else_end = kNone;
	// {{else if C}}: where C begins, just after the "if".
	std::size_t else_if_condition = kNone;
	std::size_t end_at = kNone;
	std::size_t after_end = kNone;
	bool closed = false;
};

Block block_after(std::string_view text, std::size_t from) {
	Block block;
	std::size_t depth = 0;
	std::size_t cursor = from;
	while (cursor < text.size()) {
		const std::size_t next = text.find("{{", cursor);
		if (next == kNone) break;
		std::size_t rest = 0;
		const std::string inner = tag_at(text, next, rest);
		const std::size_t innerClose = inner.empty() ? kNone : balanced_close(text, rest);
		if (innerClose == kNone) {
			// One brace at a time: "{{{else}}}" still holds a tag.
			cursor = next + 1;
			continue;
		}
		const auto [tagStart, tagEnd] = tag_span(text, next, innerClose);
		if (inner == "if") {
			++depth;
		} else if (inner == "end" && depth > 0) {
			--depth;
		} else if ((inner == "else" || inner == "elseif") && depth == 0 && block.else_at == kNone) {
			// A second {{else}} is left in the words it stands in, where it is
			// reported as a tag without its {{if}}.
			block.else_at = tagStart;
			block.else_end = tagEnd;
			if (inner == "elseif") block.else_if_condition = rest;
		} else if (inner == "end" && depth == 0) {
			block.end_at = tagStart;
			block.after_end = tagEnd;
			block.closed = true;
			return block;
		}
		cursor = innerClose + 2;
	}
	return block;
}

// The words chosen when the condition fails, up to `end_at`: after an
// {{else if C}}, the {{if C}} they are.
std::string false_branch(std::string_view text, const Block &block, std::size_t end_at) {
	if (block.else_at == kNone) return {};
	if (block.else_if_condition == kNone) return std::string(text.substr(block.else_end, end_at - block.else_end));
	return "{{if" + std::string(text.substr(block.else_if_condition, end_at - block.else_if_condition)) + "{{end}}";
}

// The statement that opens at `position`, if one does.
std::optional<Statement> statement_at(const std::string &text, std::size_t position) {
	std::size_t rest = 0;
	const std::string tag = tag_at(text, position, rest);
	if (!is_statement_tag(tag)) return std::nullopt;
	const std::size_t close = balanced_close(text, rest);
	if (close == kNone) return std::nullopt;
	Statement statement;
	statement.start = position;
	statement.head_start = rest;
	statement.head_end = close;
	if (tag != "if") {
		statement.kind = tag == "=" ? StatementKind::Value : tag == "case" ? StatementKind::Case : StatementKind::Gender;
		statement.end = close + 2;
		return statement;
	}
	statement.kind = StatementKind::Condition;
	// A block the model never closed ends with the text - and is reported.
	const std::size_t bodyStart = tag_span(text, position, close).second;
	const Block block = block_after(text, bodyStart);
	const std::size_t endAt = block.closed ? block.end_at : text.size();
	if (block.else_at != kNone) {
		statement.when_true = text.substr(bodyStart, block.else_at - bodyStart);
		statement.when_false = false_branch(text, block, endAt);
	} else {
		statement.when_true = text.substr(bodyStart, endAt - bodyStart);
	}
	statement.end = block.closed ? block.after_end : text.size();
	statement.unclosed = !block.closed;
	return statement;
}

// ---- A head as an expression ------------------------------------------------------------------

// A head Singularity cannot turn into an expression, and why.
struct Problem {
	std::string reason;
};

// A value's arithmetic and the format asked for after its last top-level
// " as " - never one inside ⟦ ⟧, a parenthesis or a nested statement.
std::pair<std::string, std::string> split_format(const std::string &head) {
	int parentheses = 0;
	std::size_t found = kNone;
	std::size_t index = 0;
	while (index < head.size()) {
		if (starts_with_at(head, index, kOpen)) {
			index = operand_end(head, index);
			continue;
		}
		if (starts_with_at(head, index, "{{")) {
			const std::size_t close = balanced_close(head, index + 2);
			if (close == kNone) break;
			index = close + 2;
			continue;
		}
		const char character = head[index];
		if (character == '(') ++parentheses;
		else if (character == ')' && parentheses > 0) --parentheses;
		else if (parentheses == 0 && is_space(character) && index + 3 < head.size()
				&& lower_ascii(head.substr(index + 1, 2)) == "as" && is_space(head[index + 3]))
			found = index;
		++index;
	}
	if (found == kNone) return {head, std::string()};
	return {head.substr(0, found), trimmed(head.substr(found + 3))};
}

// The head with every statement nested in it written as the expression it
// stands for: {{if C}}A{{else}}B{{end}} is if(C, A, B) - the model's way of
// choosing a value - and {{= X as f}} is (X).
std::string as_expression(const std::string &head, Language language, int depth) {
	if (depth > kDeepest)
		throw Problem{say(language, "the statement is nested too deeply", "\320\262 \320\262\321\213\321\200\320\260\320\266\320\265\320\275\320\270\320\270 \321\201\320\273\320\270\321\210\320\272\320\276\320\274 \320\263\320\273\321\203\320\261\320\276\320\272\320\260\321\217 \320\262\320\273\320\276\320\266\320\265\320\275\320\275\320\276\321\201\321\202\321\214")};
	std::string expression;
	std::size_t index = 0;
	while (index < head.size()) {
		if (starts_with_at(head, index, kOpen)) {
			const std::size_t end = operand_end(head, index);
			expression.append(head, index, end - index);
			index = end;
			continue;
		}
		if (!starts_with_at(head, index, "{{")) {
			expression.push_back(head[index++]);
			continue;
		}
		std::size_t rest = 0;
		const std::string tag = tag_at(head, index, rest);
		const std::size_t close = tag.empty() ? kNone : balanced_close(head, rest);
		if (close == kNone)
			throw Problem{say(language, "\"{{\" inside the calculation opens no statement that is closed with \"}}\"",
				"«{{» \320\262\320\275\321\203\321\202\321\200\320\270 \321\200\320\260\321\201\321\207\320\265\321\202\320\260 \320\275\320\265 \320\276\321\202\320\272\321\200\321\213\320\262\320\260\320\265\321\202 \320\262\321\213\321\200\320\260\320\266\320\265\320\275\320\270\321\217, \320\267\320\260\320\272\321\200\321\213\321\202\320\276\320\263\320\276 «}}»")};
		if (tag == "=") {
			expression += "(" + as_expression(split_format(head.substr(rest, close - rest)).first, language, depth + 1) + ")";
			index = close + 2;
			continue;
		}
		if (tag == "if") {
			const Block block = block_after(head, close + 2);
			if (!block.closed)
				throw Problem{say(language, "an {{if}} inside the calculation is not closed with {{end}}",
					"{{if}} \320\262\320\275\321\203\321\202\321\200\320\270 \321\200\320\260\321\201\321\207\320\265\321\202\320\260 \320\275\320\265 \320\267\320\260\320\272\321\200\321\213\321\202 {{end}}")};
			if (block.else_at == kNone)
				throw Problem{say(language, "a value chosen with {{if}} inside the calculation needs {{else}}: without it there is no value when the condition fails",
					"\320\267\320\275\320\260\321\207\320\265\320\275\320\270\321\216, \320\262\321\213\320\261\321\200\320\260\320\275\320\275\320\276\320\274\321\203 \321\207\320\265\321\200\320\265\320\267 {{if}} \320\262\320\275\321\203\321\202\321\200\320\270 \321\200\320\260\321\201\321\207\320\265\321\202\320\260, \320\275\321\203\320\266\320\265\320\275 {{else}}: \320\261\320\265\320\267 \320\275\320\265\320\263\320\276 \320\277\321\200\320\270 \320\275\320\265\320\262\321\213\320\277\320\276\320\273\320\275\320\265\320\275\320\275\320\276\320\274 \321\203\321\201\320\273\320\276\320\262\320\270\320\270 \320\267\320\275\320\260\321\207\320\265\320\275\320\270\321\217 \320\275\320\265\321\202")};
			const std::string condition = head.substr(rest, close - rest);
			const std::string whenTrue = head.substr(close + 2, block.else_at - close - 2);
			const std::string whenFalse = false_branch(head, block, block.end_at);
			expression += "if(" + as_expression(condition, language, depth + 1) + ", " + as_expression(whenTrue, language, depth + 1)
				+ ", " + as_expression(whenFalse, language, depth + 1) + ")";
			index = block.after_end;
			continue;
		}
		throw Problem{say(language, "{{" + tag + "}} cannot stand inside a calculation", "{{" + tag + "}} \320\275\320\265 \320\274\320\276\320\266\320\265\321\202 \321\201\321\202\320\276\321\217\321\202\321\214 \320\262\320\275\321\203\321\202\321\200\320\270 \321\200\320\260\321\201\321\207\320\265\321\202\320\260")};
	}
	return expression;
}

// Every ⟦…⟧ in `text`, as written inside the marks.
std::vector<std::string> operands_in(std::string_view text) {
	std::vector<std::string> operands;
	std::size_t from = 0;
	while ((from = text.find(kOpen, from)) != kNone) {
		const std::size_t close = text.find(kClose, from + kOpen.size());
		if (close == kNone) break;
		operands.emplace_back(text.substr(from + kOpen.size(), close - from - kOpen.size()));
		from = close + kClose.size();
	}
	return operands;
}

// ---- Working a statement out ------------------------------------------------------------------

struct Worked {
	std::optional<semantic::Value> value;
	semantic::Format format;
	std::string format_text;  // as asked, "" when none
	std::string expression;
	std::string reason;
	std::string error_kind;  // the algebra's failure kind ("undetermined", "arity", ...), "" for Singularity's own
	bool undetermined = false;
};

std::string unknown_format(const std::string &asked, Language language) {
	return say(language,
		"\"as " + asked + "\" is not a format Singularity knows (money, $, \xE2\x82\xAC, \xE2\x82\xBD, percent, number, days, months, date, words)",
		"«as " + asked + "» - \320\275\320\265 \321\204\320\276\321\200\320\274\320\260\321\202, \320\270\320\267\320\262\320\265\321\201\321\202\320\275\321\213\320\271 Singularity (money, $, \xE2\x82\xAC, \xE2\x82\xBD, percent, number, days, months, date, words)");
}

// A value statement's head: its expression worked out, and how it is to be written.
Worked work_value(const std::string &head, Language language) {
	Worked worked;
	const auto [written, asked] = split_format(head);
	worked.format_text = asked;
	if (!asked.empty()) {
		const auto format = semantic::parse_format(asked);
		if (!format) {
			worked.reason = unknown_format(asked, language);
			return worked;
		}
		worked.format = *format;
	}
	try {
		worked.expression = as_expression(written, language, 0);
	} catch (const Problem &problem) {
		worked.reason = problem.reason;
		return worked;
	}
	const semantic::Evaluation evaluation = semantic::evaluate(worked.expression, language);
	if (evaluation.error) {
		worked.reason = evaluation.error->reason;
		worked.error_kind = evaluation.error->kind;
		worked.undetermined = evaluation.error->kind == "undetermined";
		return worked;
	}
	worked.value = evaluation.value;
	return worked;
}

// A condition's head: true or false, or why it cannot be told.
Worked work_condition(const std::string &head, Language language) {
	Worked worked;
	try {
		worked.expression = as_expression(head, language, 0);
	} catch (const Problem &problem) {
		worked.reason = problem.reason;
		return worked;
	}
	const semantic::Evaluation evaluation = semantic::evaluate(worked.expression, language);
	if (evaluation.error) {
		worked.reason = evaluation.error->reason;
		worked.error_kind = evaluation.error->kind;
		worked.undetermined = evaluation.error->kind == "undetermined";
		return worked;
	}
	if (evaluation.value->kind != semantic::Kind::Boolean) {
		worked.reason = say(language, trimmed(head) + " is not a condition (true or false)", trimmed(head) + " - \320\275\320\265 \321\203\321\201\320\273\320\276\320\262\320\270\320\265 (\320\264\320\260 \320\270\320\273\320\270 \320\275\320\265\321\202)");
		return worked;
	}
	worked.value = evaluation.value;
	return worked;
}

// What a value is, for a reason that must not write the value itself.
std::string kind_phrase(const semantic::Value &value, Language language) {
	const auto unit = [&]() -> std::string {
		switch (value.unit) {
			case semantic::DurationUnit::Day: return say(language, "days", "\320\264\320\275\321\217\321\205");
			case semantic::DurationUnit::BusinessDay: return say(language, "business days", "\321\200\320\260\320\261\320\276\321\207\320\270\321\205 \320\264\320\275\321\217\321\205");
			case semantic::DurationUnit::Week: return say(language, "weeks", "\320\275\320\265\320\264\320\265\320\273\321\217\321\205");
			case semantic::DurationUnit::Month: return say(language, "months", "\320\274\320\265\321\201\321\217\321\206\320\260\321\205");
			case semantic::DurationUnit::Year: return say(language, "years", "\320\263\320\276\320\264\320\260\321\205");
			case semantic::DurationUnit::Quarter: return say(language, "quarters", "\320\272\320\262\320\260\321\200\321\202\320\260\320\273\320\260\321\205");
			case semantic::DurationUnit::Hour: return say(language, "hours", "\321\207\320\260\321\201\320\260\321\205");
			case semantic::DurationUnit::Minute: return say(language, "minutes", "\320\274\320\270\320\275\321\203\321\202\320\260\321\205");
		}
		return "";
	};
	switch (value.kind) {
		case semantic::Kind::Money: return say(language, "an amount of money", "\320\264\320\265\320\275\320\265\320\266\320\275\320\260\321\217 \321\201\321\203\320\274\320\274\320\260");
		case semantic::Kind::Number: return say(language, "a number", "\321\207\320\270\321\201\320\273\320\276");
		case semantic::Kind::Percentage: return say(language, "a percentage", "\320\277\321\200\320\276\321\206\320\265\320\275\321\202");
		case semantic::Kind::Duration: return say(language, "a period in ", "\321\201\321\200\320\276\320\272 \320\262 ") + unit();
		case semantic::Kind::Date: return say(language, "a date", "\320\264\320\260\321\202\320\260");
		case semantic::Kind::Boolean: return say(language, "true or false", "\320\264\320\260 \320\270\320\273\320\270 \320\275\320\265\321\202");
		case semantic::Kind::DayOfYear: return say(language, "a day of the year", "\320\264\320\265\320\275\321\214 \320\263\320\276\320\264\320\260");
		case semantic::Kind::Count: return say(language, "a count", "\320\272\320\276\320\273\320\270\321\207\320\265\321\201\321\202\320\262\320\276");
	}
	return "";
}

// Why a result is not written as asked - naming what the result is, never
// the result (it is worked out from protected values).
std::string refused_format(const semantic::Value &value, const std::string &asked, const semantic::AlgebraError &error, Language language) {
	if (error.kind() == "currency") return error.what();
	return say(language, "the result is " + kind_phrase(value, language) + " and cannot be written \"as " + asked + "\"",
		"\321\200\320\265\320\267\321\203\320\273\321\214\321\202\320\260\321\202 - " + kind_phrase(value, language) + ", \320\265\320\263\320\276 \320\275\320\265\320\273\321\214\320\267\321\217 \320\267\320\260\320\277\320\270\321\201\320\260\321\202\321\214 «as " + asked + "»");
}

// ---- Writing a result -------------------------------------------------------------------------

// The currency a mark names: "$", "US$", "€", "…", "USD" ...
std::string currency_of_mark(const std::string &mark) {
	const std::string lower = lower_ascii(mark);
	if (lower == "$" || lower == "us$" || lower == "usd" || lower == "dollar" || lower == "dollars") return "USD";
	if (lower == "\xE2\x82\xAC" || lower == "eur" || lower == "euro" || lower == "euros") return "EUR";
	if (lower == "\xC2\xA3" || lower == "gbp" || lower == "pound" || lower == "pounds") return "GBP";
	if (lower == "\xC2\xA5" || lower == "jpy" || lower == "yen") return "JPY";
	if (lower == "\xE2\x82\xBD" || lower == "rub" || starts_with_at(lower, 0, "\xD1\x80\xD1\x83\xD0\xB1")) return "RUB";  // …
	return "";
}

std::size_t skipped_spaces(std::string_view text, std::size_t at) {
	while (at < text.size()) {
		if (text[at] == ' ' || text[at] == '\t') ++at;
		else if (starts_with_at(text, at, "\xC2\xA0")) at += 2;
		else if (starts_with_at(text, at, "\xE2\x80\xAF") || starts_with_at(text, at, "\xE2\x80\x89")) at += 3;
		else break;
	}
	return at;
}

// A word of the text: where it begins and ends, and its letters.
struct Word {
	std::string text;
	std::size_t begin = 0;
	std::size_t end = 0;
};

// The word at `from` in `after`, past spaces: letters (Latin or Cyrillic), or
// one sign. A full stop after it is the sentence's, not the word's.
Word word_from(std::string_view after, std::size_t from) {
	Word word;
	word.begin = word.end = skipped_spaces(after, from);
	for (const char *sign : {"\xE2\x82\xBD", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "$"})
		if (starts_with_at(after, word.begin, sign)) {
			word.text = sign;
			word.end = word.begin + word.text.size();
			return word;
		}
	std::size_t index = word.begin;
	while (index < after.size()) {
		const unsigned char byte = static_cast<unsigned char>(after[index]);
		if (std::isalpha(byte) != 0 || after[index] == '.') {
			word.text.push_back(after[index++]);
		} else if ((byte == 0xD0 || byte == 0xD1) && index + 1 < after.size()) {
			word.text.append(after.substr(index, 2));
			index += 2;
		} else {
			break;
		}
	}
	while (!word.text.empty() && word.text.back() == '.') word.text.pop_back();
	word.end = word.begin + word.text.size();
	return word;
}

// The word right after a statement.
std::string word_after(std::string_view after) { return word_from(after, 0).text; }

// Cyrillic capitals as small letters, and Latin ones (…; A-Z).
std::string lower_letters(std::string text) {
	text = lower_ascii(std::move(text));
	for (std::size_t index = 0; index + 1 < text.size(); ++index) {
		const unsigned char lead = static_cast<unsigned char>(text[index]);
		const unsigned char next = static_cast<unsigned char>(text[index + 1]);
		if (lead == 0xD0 && next >= 0x90 && next <= 0x9F) text[index + 1] = static_cast<char>(next + 0x20);
		else if (lead == 0xD0 && next >= 0xA0 && next <= 0xAF) {
			text[index] = static_cast<char>(0xD1);
			text[index + 1] = static_cast<char>(next - 0x20);
		} else if (lead == 0xD0 && next == 0x81) {
			text[index] = static_cast<char>(0xD1);
			text[index + 1] = static_cast<char>(0x91);
		}
		if (lead == 0xD0 || lead == 0xD1) ++index;
	}
	return text;
}

// "calendar", "…": a word that says what kind of days, months or
// years follow it, never the unit itself.
bool is_calendar_word(const std::string &lower) {
	return lower == "calendar" || starts_with_at(lower, 0, "\xD0\xBA\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB4");  // …
}

// The currency the text already writes right next to a statement: "${{= …}}",
// "…", "USD {{= …}}".
std::string currency_beside(std::string_view before, std::string_view after) {
	std::size_t end = before.size();
	while (end > 0 && (before[end - 1] == ' ' || before[end - 1] == '\t')) --end;
	if (end >= 2 && before.substr(end - 2, 2) == "\xC2\xA0") end -= 2;
	const std::string_view tail = before.substr(0, end);
	for (const char *sign : {"US$", "$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5"})
		if (tail.size() >= std::string_view(sign).size() && tail.substr(tail.size() - std::string_view(sign).size()) == sign)
			return currency_of_mark(sign);
	if (tail.size() >= 3 && (tail.size() == 3 || std::isalpha(static_cast<unsigned char>(tail[tail.size() - 4])) == 0)) {
		const std::string code(tail.substr(tail.size() - 3));
		if (code == "USD" || code == "EUR" || code == "GBP" || code == "RUB" || code == "JPY") return code;
	}
	const std::string word = word_after(after);
	if (word.empty()) return "";
	const std::string lower = lower_ascii(word);
	if (starts_with_at(lower, 0, "\xD1\x80\xD1\x83\xD0\xB1") || starts_with_at(lower, 0, "\xD0\xA0\xD1\x83\xD0\xB1")) return "RUB";
	return currency_of_mark(word);
}

// The unit of time written right after a statement: "{{= …}} days", "…",
// "… calendar days", "…".
std::optional<semantic::DurationUnit> unit_after(std::string_view after) {
	using semantic::DurationUnit;
	Word first = word_from(after, 0);
	// "calendar months", "…": the unit is the noun after it.
	if (is_calendar_word(lower_letters(first.text))) first = word_from(after, first.end);
	const std::string word = lower_letters(first.text);
	if (word.empty()) return std::nullopt;
	if (word == "day" || word == "days") return DurationUnit::Day;
	if (word == "business") return DurationUnit::BusinessDay;
	if (word == "week" || word == "weeks") return DurationUnit::Week;
	if (word == "month" || word == "months") return DurationUnit::Month;
	if (word == "year" || word == "years") return DurationUnit::Year;
	if (starts_with_at(word, 0, "\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87")) return DurationUnit::BusinessDay;  // …
	for (const char *stem : {"\xD0\xB4\xD0\xBD",          // …
			 "\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C"})  // …
		if (starts_with_at(word, 0, stem)) return DurationUnit::Day;
	if (starts_with_at(word, 0, "\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB")) return DurationUnit::Week;  // …
	if (starts_with_at(word, 0, "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86")) return DurationUnit::Month;  // …
	if (word == "\xD0\xB3\xD0\xBE\xD0\xB4" || word == "\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xB0"  // …
			|| word == "\xD0\xBB\xD0\xB5\xD1\x82")                                         // …
		return DurationUnit::Year;
	return std::nullopt;
}

std::optional<semantic::DurationUnit> unit_of_style(semantic::FormatStyle style) {
	switch (style) {
		case semantic::FormatStyle::Days: return semantic::DurationUnit::Day;
		case semantic::FormatStyle::Weeks: return semantic::DurationUnit::Week;
		case semantic::FormatStyle::Months: return semantic::DurationUnit::Month;
		case semantic::FormatStyle::Years: return semantic::DurationUnit::Year;
		default: return std::nullopt;
	}
}

std::string unit_words(semantic::DurationUnit unit, Language language) {
	switch (unit) {
		case semantic::DurationUnit::Day: return say(language, "days", "\320\264\320\275\321\217\321\205");
		case semantic::DurationUnit::BusinessDay: return say(language, "business days", "\321\200\320\260\320\261\320\276\321\207\320\270\321\205 \320\264\320\275\321\217\321\205");
		case semantic::DurationUnit::Week: return say(language, "weeks", "\320\275\320\265\320\264\320\265\320\273\321\217\321\205");
		case semantic::DurationUnit::Month: return say(language, "months", "\320\274\320\265\321\201\321\217\321\206\320\260\321\205");
		case semantic::DurationUnit::Year: return say(language, "years", "\320\263\320\276\320\264\320\260\321\205");
		case semantic::DurationUnit::Quarter: return say(language, "quarters", "\320\272\320\262\320\260\321\200\321\202\320\260\320\273\320\260\321\205");
		case semantic::DurationUnit::Hour: return say(language, "hours", "\321\207\320\260\321\201\320\260\321\205");
		case semantic::DurationUnit::Minute: return say(language, "minutes", "\320\274\320\270\320\275\321\203\321\202\320\260\321\205");
	}
	return "";
}

// A period counted in another unit, when that is exact: weeks and days, years
// and months. Business days are never calendar days, and days are never months.
std::optional<semantic::Value> period_in(const semantic::Value &value, semantic::DurationUnit unit) {
	using semantic::DurationUnit;
	using semantic::Rational;
	if (value.unit == unit) return value;
	if (value.unit == DurationUnit::Week && unit == DurationUnit::Day) return semantic::make_duration(value.amount * Rational(7), unit);
	if (value.unit == DurationUnit::Day && unit == DurationUnit::Week) return semantic::make_duration(value.amount / Rational(7), unit);
	if (value.unit == DurationUnit::Year && unit == DurationUnit::Month) return semantic::make_duration(value.amount * Rational(12), unit);
	if (value.unit == DurationUnit::Month && unit == DurationUnit::Year) return semantic::make_duration(value.amount / Rational(12), unit);
	return std::nullopt;
}

// ---- Russian words a count takes -----------------------------------------------------------------

// A Russian unit of time as a count takes it: nominative for 1, 2-4 and 5+,
// and the genitive singular (the genitive plural is the 5+ form).
struct RussianUnit {
	const char *one;
	const char *few;
	const char *many;
	const char *genitive_one;
	bool masculine;
};

const RussianUnit kRussianUnits[] = {
	{"\320\264\320\265\320\275\321\214", "\320\264\320\275\321\217", "\320\264\320\275\320\265\320\271", "\320\264\320\275\321\217", true},
	{"\320\274\320\265\321\201\321\217\321\206", "\320\274\320\265\321\201\321\217\321\206\320\260", "\320\274\320\265\321\201\321\217\321\206\320\265\320\262", "\320\274\320\265\321\201\321\217\321\206\320\260", true},
	{"\320\263\320\276\320\264", "\320\263\320\276\320\264\320\260", "\320\273\320\265\321\202", "\320\263\320\276\320\264\320\260", true},
	{"\320\275\320\265\320\264\320\265\320\273\321\217", "\320\275\320\265\320\264\320\265\320\273\320\270", "\320\275\320\265\320\264\320\265\320\273\321\214", "\320\275\320\265\320\264\320\265\320\273\320\270", false},
};

// The last word of `before`, in small letters: "…" in "…".
std::string last_word(std::string_view before) {
	std::size_t end = before.size();
	while (end > 0) {
		if (before[end - 1] == ' ' || before[end - 1] == '\t') --end;
		else if (end >= 2 && before.substr(end - 2, 2) == "\xC2\xA0") end -= 2;
		else break;
	}
	std::size_t begin = end;
	while (begin > 0) {
		const unsigned char byte = static_cast<unsigned char>(before[begin - 1]);
		if (std::isalpha(byte) != 0) --begin;
		else if (begin >= 2 && byte >= 0x80 && byte <= 0xBF
				&& (static_cast<unsigned char>(before[begin - 2]) == 0xD0 || static_cast<unsigned char>(before[begin - 2]) == 0xD1))
			begin -= 2;
		else break;
	}
	return lower_letters(std::string(before.substr(begin, end - begin)));
}

// A word before a count that puts it in the genitive: "…",
bool governs_genitive(const std::string &word) {
	static const char *const kWords[] = {"\321\202\320\265\321\207\320\265\320\275\320\270\320\265", "\320\264\320\276", "\320\274\320\265\320\275\320\265\320\265", "\320\261\320\276\320\273\320\265\320\265", "\320\261\320\276\320\273\321\214\321\210\320\265", "\320\274\320\265\320\275\321\214\321\210\320\265", "\321\201\320\262\321\213\321\210\320\265", "\320\276\321\202", "\320\276\320\272\320\276\320\273\320\276", "\320\277\320\276\321\201\320\273\320\265",
		"\320\270\320\267", "\320\264\320\273\321\217", "\320\261\320\265\320\267", "\321\201\320\262\320\265\321\200\321\205", "\320\270\321\201\321\202\320\265\321\207\320\265\320\275\320\270\320\270", "\320\277\321\200\320\265\320\264\320\265\320\273\320\260\321\205", "\320\277\320\276\320\267\320\264\320\275\320\265\320\265", "\320\277\320\276\320\267\320\266\320\265", "\321\200\320\260\320\275\320\265\320\265", "\321\200\320\260\320\275\321\214\321\210\320\265"};
	for (const char *governing : kWords)
		if (word == governing) return true;
	return false;
}

// The Russian words the text writes after a count, agreed with it: the model
// writes them before the count is known (it is worked out from protected
// values), so "…" for 171 is "…"
// for 5 is "…", and after «…» a count of 21 is
// "…". Returns how much of `after` the agreed words replace, and them
// with the text's own spaces; nothing for words that are not a unit of time
// (or a case other than these: "…" stays as written).
std::optional<std::pair<std::size_t, std::string>> agreed_russian_unit(std::string_view before, std::string_view after,
	const semantic::Rational &count) {
	if (!count.is_integer() || count.sign() < 0) return std::nullopt;
	const Word first = word_from(after, 0);
	const std::string firstLower = lower_letters(first.text);
	const bool calendar = starts_with_at(firstLower, 0, "\320\272\320\260\320\273\320\265\320\275\320\264\320\260\321\200\320\275");
	const bool working = starts_with_at(firstLower, 0, "\321\200\320\260\320\261\320\276\321\207");
	const Word noun = calendar || working ? word_from(after, first.end) : first;
	const std::string nounLower = lower_letters(noun.text);
	const RussianUnit *unit = nullptr;
	for (const RussianUnit &each : kRussianUnits)
		if (nounLower == each.one || nounLower == each.few || nounLower == each.many) unit = &each;
	if (unit == nullptr || ((calendar || working) && !unit->masculine)) return std::nullopt;
	const semantic::Int128 number = count.numerator();
	const bool one = number % 10 == 1 && number % 100 != 11;
	const bool genitive = governs_genitive(last_word(before));
	std::string replaced(after.substr(0, first.begin));
	if (calendar || working) {
		const std::string stem = calendar ? "\320\272\320\260\320\273\320\265\320\275\320\264\320\260\321\200\320\275" : "\321\200\320\260\320\261\320\276\321\207";
		const std::string ending = !one ? (calendar ? "\321\213\321\205" : "\320\270\321\205") : genitive ? (calendar ? "\320\276\320\263\320\276" : "\320\265\320\263\320\276") : (calendar ? "\321\213\320\271" : "\320\270\320\271");
		replaced += stem + ending;
		replaced.append(after.substr(first.end, noun.begin - first.end));
	}
	replaced += genitive ? (one ? unit->genitive_one : unit->many) : semantic::russian_plural(number, unit->one, unit->few, unit->many);
	return std::make_pair(noun.end, replaced);
}

// How the amounts of a statement were written, for a result the model asked
// no format for: "…" stays "…" stays words, "USD" a code. A
// Russian word stays only in Russian text.
std::string mark_of_operands(const std::string &expression, const std::string &currency, Language language) {
	for (const std::string &operand : operands_in(expression)) {
		const auto value = semantic::parse_value(operand);
		if (!value || value->kind != semantic::Kind::Money || value->currency != currency) continue;
		if (language != Language::Russian && operand.find("\xD1\x80\xD1\x83\xD0\xB1") != kNone) return "";
		if (operand.find("\xD1\x80\xD1\x83\xD0\xB1\xD0\xBB") != kNone) return "\xD1\x80\xD1\x83\xD0\xB1\xD0\xBB\xD0\xB5\xD0\xB9";  // …
		if (operand.find("\xD1\x80\xD1\x83\xD0\xB1") != kNone) return "\xD1\x80\xD1\x83\xD0\xB1.";  // …
		for (const char *code : {"USD", "EUR", "GBP", "RUB", "JPY"}) {
			const std::size_t at = operand.find(code);
			if (at == kNone) continue;
			const bool alone = (at == 0 || std::isalpha(static_cast<unsigned char>(operand[at - 1])) == 0)
				&& (at + 3 >= operand.size() || std::isalpha(static_cast<unsigned char>(operand[at + 3])) == 0);
			if (alone && operand.find('$') == kNone) return code;
		}
		return "";
	}
	return "";
}

// A value as the lawyer reads it in its place: in the format asked, else as
// its operands are written; a currency or a unit of time the text already
// writes next to it is not written twice, and Russian words for the unit are
// agreed with the count. `consumed`: how much of `after` the result replaces.
std::string written_value(const Worked &worked, Language language, std::string_view before, std::string_view after,
	std::size_t &consumed) {
	consumed = 0;
	const semantic::Value &value = *worked.value;
	semantic::Format format = worked.format;
	const bool asked = !worked.format_text.empty();
	if (!asked && value.kind == semantic::Kind::Money && !value.currency.empty()) {
		const std::string mark = mark_of_operands(worked.expression, value.currency, language);
		if (!mark.empty()) {
			format.style = semantic::FormatStyle::Money;
			format.currency = value.currency;
			format.mark = mark;
		}
	}
	const bool money = value.kind == semantic::Kind::Money || format.style == semantic::FormatStyle::Money;
	if (money && !format.words && value.kind != semantic::Kind::Date && value.kind != semantic::Kind::Boolean) {
		const std::string currency = !format.currency.empty() ? format.currency : value.kind == semantic::Kind::Money ? value.currency : "";
		if (!currency.empty() && currency_beside(before, after) == currency) {
			// Written already: the figure alone, in the money's own decimals.
			semantic::Value figure = value;
			figure.kind = semantic::Kind::Money;
			figure.currency.clear();
			return semantic::display(figure, {}, language);
		}
	}
	// A unit of time the text writes right after the figure is the unit the
	// figure is counted in: "{{= ⟦two (2) weeks⟧}} days" is 14, never 2. A
	// period that is not exactly a count of that unit is not written at all.
	const auto written_unit = format.words ? std::nullopt : unit_after(after);
	const auto asked_unit = unit_of_style(format.style);
	const bool counted = value.kind == semantic::Kind::Duration
		|| (value.kind == semantic::Kind::Number && asked_unit.has_value());
	if (written_unit && counted) {
		const std::string unit = unit_words(*written_unit, language);
		if (asked_unit && *asked_unit != *written_unit)
			throw Problem{say(language, "\"as " + worked.format_text + "\" asks for a figure the text then counts in " + unit,
				"«as " + worked.format_text + "» \320\277\321\200\320\276\321\201\320\270\321\202 \321\207\320\270\321\201\320\273\320\276, \320\272\320\276\321\202\320\276\321\200\320\276\320\265 \321\202\320\265\320\272\321\201\321\202 \320\267\320\260\321\202\320\265\320\274 \321\201\321\207\320\270\321\202\320\260\320\265\321\202 \320\262 " + unit)};
		semantic::Value period = value;
		if (value.kind == semantic::Kind::Duration) {
			const auto converted = period_in(value, *written_unit);
			if (!converted)
				throw Problem{say(language, "the result is " + kind_phrase(value, language) + ", and the text counts it in " + unit
						+ ": it is not an exact number of them",
					"\321\200\320\265\320\267\321\203\320\273\321\214\321\202\320\260\321\202 - " + kind_phrase(value, language) + ", \320\260 \321\202\320\265\320\272\321\201\321\202 \321\201\321\207\320\270\321\202\320\260\320\265\321\202 \320\265\320\263\320\276 \320\262 " + unit + ": \321\202\320\276\321\207\320\275\320\276\320\263\320\276 \320\270\321\205 \321\207\320\270\321\201\320\273\320\260 \320\275\320\265\321\202")};
			period = *converted;
		} else {
			period = semantic::make_duration(value.amount, *written_unit);
		}
		semantic::Format figure;
		figure.style = format.style == semantic::FormatStyle::Integer ? format.style : semantic::FormatStyle::Number;
		const std::string count = semantic::display(period, figure, language);
		const semantic::Rational counted = figure.style == semantic::FormatStyle::Integer ? period.amount.rounded(0) : period.amount;
		if (const auto agreed = agreed_russian_unit(before, after, counted)) {
			consumed = agreed->first;
			return count + agreed->second;
		}
		return count;
	}
	return semantic::display(value, format, language);
}

std::string without_operand_marks(std::string text) {
	for (const std::string_view mark : {kOpen, kClose}) {
		std::size_t at = 0;
		while ((at = text.find(mark, at)) != kNone) text.erase(at, mark.size());
	}
	return text;
}

// A name in the case a Russian sentence needs, or the word that agrees with a
// name's gender - "…"
// - worked out from the restored name on the lawyer's computer
// (Context/RussianNames). What cannot be told is written in its plainest form:
// the name as it is, the first of the words offered.
std::string grammar_of(const Statement &statement, const std::string &head) {
	std::string value;
	std::string rest;
	const std::size_t open = head.find(kOpen);
	const std::size_t close = open == kNone ? kNone : head.find(kClose, open + kOpen.size());
	if (open != kNone && close != kNone) {
		value = head.substr(open + kOpen.size(), close - open - kOpen.size());
		rest = head.substr(close + kClose.size());
	}
	if (statement.kind == StatementKind::Case) {
		const std::string asked = trimmed(open == kNone ? head : head.substr(0, open));
		const std::string words = trimmed(value);
		const auto form = russian_names::case_from_word(asked.substr(0, asked.find(' ')));
		if (words.empty()) {
			// Written without a protected name: "…".
			const std::string plain = trimmed(head);
			const std::size_t space = plain.find(' ');
			if (space == kNone) return plain;
			const auto plainForm = russian_names::case_from_word(plain.substr(0, space));
			const std::string name = trimmed(plain.substr(space + 1));
			return plainForm ? russian_names::declined(name, *plainForm) : name;
		}
		return form ? russian_names::declined(words, *form) : words;
	}
	// Gender: "⟦name⟧: masculine|feminine|neuter".
	const std::size_t colon = rest.find(':');
	const std::string offered = trimmed(colon == kNone ? rest : rest.substr(colon + 1));
	std::vector<std::string> forms;
	std::size_t from = 0;
	while (true) {
		const std::size_t bar = offered.find('|', from);
		forms.push_back(trimmed(offered.substr(from, bar == kNone ? kNone : bar - from)));
		if (bar == kNone) break;
		from = bar + 1;
	}
	if (forms.empty() || forms.front().empty()) return std::string();
	switch (russian_names::gender_of(trimmed(value))) {
		case russian_names::Gender::Feminine: return forms.size() > 1 ? forms[1] : forms[0];
		case russian_names::Gender::Neuter: return forms.size() > 2 ? forms[2] : forms[0];
		default: return forms[0];
	}
}

// ---- Reasons that name no protected result ------------------------------------------------------

// Where a number written at `at` (a digit) ends: digits, with a separator
// only between digits ("1,234.5", "828 000" with no-break spaces).
std::size_t number_end(std::string_view text, std::size_t at) {
	const auto digit = [&](std::size_t index) { return index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0; };
	while (at < text.size()) {
		if (digit(at)) ++at;
		else if ((text[at] == ',' || text[at] == '.') && digit(at + 1)) at += 1;
		else if (starts_with_at(text, at, "\xC2\xA0") && digit(at + 2)) at += 2;
		else if ((starts_with_at(text, at, "\xE2\x80\xAF") || starts_with_at(text, at, "\xE2\x80\x89")) && digit(at + 3)) at += 3;
		else break;
	}
	return at;
}

// A written number as an exact value, however it is grouped: "1,234.5",
// "1 234,5", "45".
std::optional<semantic::Rational> number_value(std::string text) {
	std::string plain;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (std::isdigit(byte) != 0 || text[index] == ',' || text[index] == '.') plain.push_back(text[index]);
	}
	const std::size_t comma = plain.rfind(',');
	const std::size_t point = plain.rfind('.');
	if (comma != kNone && point != kNone) {
		const char grouping = comma < point ? ',' : '.';
		std::string ungrouped;
		for (const char character : plain)
			if (character != grouping) ungrouped.push_back(character == ',' ? '.' : character);
		plain = ungrouped;
	} else if (comma != kNone) {
		// "1,234" and "12,000,000" group; "0,1" and "1 234,56" are decimals.
		const bool several = plain.find(',') != comma;
		const bool grouping = several || (plain.size() - comma - 1 == 3 && plain.substr(0, comma) != "0");
		std::string read;
		for (const char character : plain)
			if (character != ',' || !grouping) read.push_back(character == ',' ? '.' : character);
		plain = read;
	}
	return semantic::Rational::parse_decimal(plain);
}

// The numbers a statement writes itself, outside its ⟦operands⟧: 45 in
// add_days(⟦July 2026⟧, 45) is the model's own, not a protected result.
std::vector<semantic::Rational> literal_numbers(std::string_view statement) {
	std::vector<semantic::Rational> numbers;
	std::size_t at = 0;
	while (at < statement.size()) {
		if (starts_with_at(statement, at, kOpen)) {
			at = operand_end(statement, at);
			continue;
		}
		const unsigned char byte = static_cast<unsigned char>(statement[at]);
		if (std::isdigit(byte) == 0) {
			++at;
			continue;
		}
		const std::size_t end = number_end(statement, at);
		const bool partOfWord = at > 0 && (std::isalpha(static_cast<unsigned char>(statement[at - 1])) != 0 || statement[at - 1] == '_');
		if (!partOfWord)
			if (const auto number = number_value(std::string(statement.substr(at, end - at)))) numbers.push_back(*number);
		at = end;
	}
	return numbers;
}

// A reason an algebra failure gives may name a figure it worked out - "the
// date 45 days later", where 45 is ⟦$90,000⟧ / ⟦$2,000⟧. Every figure it
// writes that the statement does not write itself goes inside ⟦ ⟧, where
// protection gives it a symbol like any other derived value; the rule of
// thumb a reason teaches ("a month has 28 to 31 days") is not a figure.
std::string without_derived_figures(const std::string &reason, const std::string &statement) {
	static const char *const kRules[] = {"28 to 31", "28 \xD0\xB4\xD0\xBE 31", "365, 366 or 360", "365, 366 \xD0\xB8\xD0\xBB\xD0\xB8 360",
		"365 or 366", "365 \xD0\xB8\xD0\xBB\xD0\xB8 366", "/ 365", "(1 + ", " or 1 ", " \xD0\xB8\xD0\xBB\xD0\xB8 1 "};
	const std::vector<semantic::Rational> written = literal_numbers(statement);
	std::string result;
	std::size_t at = 0;
	while (at < reason.size()) {
		if (starts_with_at(reason, at, kOpen)) {
			const std::size_t end = operand_end(reason, at);
			result.append(reason, at, end - at);
			at = end;
			continue;
		}
		bool rule = false;
		for (const char *phrase : kRules) {
			if (!starts_with_at(reason, at, phrase)) continue;
			result += phrase;
			at += std::string_view(phrase).size();
			rule = true;
			break;
		}
		if (rule) continue;
		const unsigned char byte = static_cast<unsigned char>(reason[at]);
		const bool partOfWord = at > 0 && (std::isalnum(static_cast<unsigned char>(reason[at - 1])) != 0 || reason[at - 1] == '_');
		if (std::isdigit(byte) == 0 || partOfWord) {
			result.push_back(reason[at++]);
			continue;
		}
		std::size_t end = number_end(reason, at);
		const auto number = number_value(reason.substr(at, end - at));
		bool own = false;
		for (const semantic::Rational &literal : written) own = own || (number && *number == literal);
		if (own) {
			result.append(reason, at, end - at);
			at = end;
			continue;
		}
		// The figure's own signs belong inside the marks with it: "$", "-", "%", "₽".
		std::size_t begin = result.size();
		for (const char *sign : {"$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "-"}) {
			const std::string_view mark = sign;
			if (begin >= mark.size() && result.compare(begin - mark.size(), mark.size(), mark) == 0) begin -= mark.size();
		}
		for (const char *sign : {"%", "\xC2\xA0%", " %", "\xC2\xA0\xE2\x82\xBD", " \xE2\x82\xBD"})
			if (starts_with_at(reason, end, sign)) {
				end += std::string_view(sign).size();
				break;
			}
		const std::string figure = result.substr(begin) + reason.substr(at, end - at);
		result.erase(begin);
		result += std::string(kOpen) + figure + std::string(kClose);
		at = end;
	}
	return result;
}

// ---- The language a statement is written in -------------------------------------------------------

// The letters of a text: those of its words - never the statements' own
// function names and formats, never a bracketed handle ("[[E5]]",
// "[SOURCE:DOC_1]") - and those of its operands, apart.
struct Letters {
	std::size_t cyrillic = 0;
	std::size_t latin = 0;
	std::size_t operand_cyrillic = 0;
	std::size_t operand_latin = 0;
};

Letters letters_of(std::string_view text) {
	Letters letters;
	const auto count = [](std::string_view part, std::size_t &cyr, std::size_t &lat) {
		for (std::size_t index = 0; index < part.size(); ++index) {
			const unsigned char byte = static_cast<unsigned char>(part[index]);
			if ((byte == 0xD0 || byte == 0xD1) && index + 1 < part.size()) {
				++cyr;
				++index;
			} else if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
				++lat;
			}
		}
	};
	std::size_t at = 0;
	while (at < text.size()) {
		if (starts_with_at(text, at, kOpen)) {
			const std::size_t end = operand_end(text, at);
			count(text.substr(at, end - at), letters.operand_cyrillic, letters.operand_latin);
			at = end;
			continue;
		}
		if (text[at] == '[') {
			const std::size_t shut = text.find(']', at);
			const std::size_t line = text.find('\n', at);
			if (shut != kNone && (line == kNone || shut < line)) {
				at = shut + 1;
				continue;
			}
		}
		std::size_t rest = 0;
		const std::string tag = tag_at(text, at, rest);
		const std::size_t close = tag.empty() ? kNone : balanced_close(text, rest);
		if (close != kNone) {
			// The head's operands still tell a cell's language.
			for (const std::string &operand : operands_in(text.substr(rest, close - rest)))
				count(operand, letters.operand_cyrillic, letters.operand_latin);
			at = close + 2;
			continue;
		}
		const unsigned char byte = static_cast<unsigned char>(text[at]);
		const std::size_t width = (byte == 0xD0 || byte == 0xD1) && at + 1 < text.size() ? 2 : 1;
		count(text.substr(at, width), letters.cyrillic, letters.latin);
		at += width;
	}
	return letters;
}

// The language of the words around a text's statements: a Russian sentence
// asking "as days" is Russian, an English one over ruble operands is English.
// With no words around them (a cell, a question), the operands' language.
Language language_around(std::string_view text) {
	const Letters letters = letters_of(text);
	if (letters.cyrillic + letters.latin == 0)
		return letters.operand_cyrillic > letters.operand_latin ? Language::Russian : Language::English;
	return letters.cyrillic > letters.latin ? Language::Russian : Language::English;
}

// The language of the words on the line a statement stands on (from `start`
// to `end`): a bilingual contract writes the Russian clause's figures in
// Russian and the English clause's in English. A line with no words of its
// own - a cell, a statement alone - takes `fallback`, the text's.
Language language_near(std::string_view text, std::size_t start, std::size_t end, Language fallback) {
	const std::size_t newline = start == 0 ? kNone : text.rfind('\n', start - 1);
	const std::size_t lineStart = newline == kNone ? 0 : newline + 1;
	std::size_t lineEnd = text.find('\n', std::max(start, end));
	if (lineEnd == kNone) lineEnd = text.size();
	const Letters letters = letters_of(text.substr(lineStart, lineEnd - lineStart));
	if (letters.cyrillic + letters.latin == 0) return fallback;
	return letters.cyrillic > letters.latin ? Language::Russian : Language::English;
}

// Where the arithmetic of a statement never closed with "}}" ends, from
// `from` (just past its tag) and before `limit`: operands, placeholders,
// numbers, operators, function calls and their parentheses. The words after
// it are the lawyer's and stay.
std::size_t arithmetic_end(std::string_view text, std::size_t from, std::size_t limit) {
	std::size_t at = from;
	std::size_t last = from;
	int parentheses = 0;
	const auto digit = [&](std::size_t index) { return index < limit && std::isdigit(static_cast<unsigned char>(text[index])) != 0; };
	while (at < limit) {
		const char character = text[at];
		if (is_space(character)) {
			++at;
			continue;
		}
		if (starts_with_at(text, at, kOpen) || starts_with_at(text, at, "[[")) {
			const std::size_t shut = text.find(starts_with_at(text, at, kOpen) ? kClose : std::string_view("]]"), at + 2);
			if (shut == kNone || shut >= limit) break;
			at = last = shut + (starts_with_at(text, at, kOpen) ? kClose.size() : 2);
			continue;
		}
		if (digit(at) || (character == '.' && digit(at + 1))) {
			at = last = at + 1;
			continue;
		}
		if (std::string_view("+-*/%<>=!").find(character) != std::string_view::npos) {
			at = last = at + 1;
			continue;
		}
		if (character == '(') {
			++parentheses;
			at = last = at + 1;
			continue;
		}
		if ((character == ')' || character == ',') && parentheses > 0) {
			if (character == ')') --parentheses;
			at = last = at + 1;
			continue;
		}
		if (std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_') {
			// A name is arithmetic only as a function called: "max(".
			std::size_t name = at;
			while (name < limit && (std::isalnum(static_cast<unsigned char>(text[name])) != 0 || text[name] == '_')) ++name;
			std::size_t next = name;
			while (next < limit && text[next] == ' ') ++next;
			if (next < limit && text[next] == '(') {
				at = last = name;
				continue;
			}
		}
		break;
	}
	return last;
}

// ---- Working a text out ------------------------------------------------------------------------

// Works every statement of a text out, keeping each failure for the caller.
class Worker {
public:
	// `fallback`: the language of a statement whose line has no words of its own.
	std::string run(const std::string &text, int depth, Language fallback) {
		std::string result;
		std::size_t cursor = 0;
		while (cursor < text.size()) {
			const std::size_t next = text.find("{{", cursor);
			if (next == kNone) break;
			std::size_t rest = 0;
			const std::string tag = tag_at(text, next, rest);
			if (tag.empty()) {
				// Ordinary braces, one at a time: "{{{= …}}}" still holds a statement.
				result.append(text, cursor, next + 1 - cursor);
				cursor = next + 1;
				continue;
			}
			result.append(text, cursor, next - cursor);
			// "{{{= …}}}": the braces a template language adds are not the lawyer's.
			if (!result.empty() && result.back() == '{' && next > 0 && text[next - 1] == '{') result.pop_back();
			const auto statement = statement_at(text, next);
			if (!statement) {
				language_ = language_near(text, next, next, fallback);
				cursor = broken(text, next, tag, rest);
				// The words on either side of what is taken away keep one space.
				if (!result.empty() && result.back() == ' ' && cursor < text.size() && text[cursor] == ' ') ++cursor;
				continue;
			}
			language_ = language_near(text, statement->start, statement->end, fallback);
			// A "}" left over from a closing "}}}" is the template's, not the lawyer's.
			std::size_t end = statement->end;
			if (end < text.size() && text[end] == '}') ++end;
			std::size_t consumed = 0;
			const std::string head = text.substr(statement->head_start, statement->head_end - statement->head_start);
			const std::string written = text.substr(statement->start, statement->head_end + 2 - statement->start);
			if (depth > kDeepest) {
				fail(text.substr(statement->start, statement->end - statement->start),
					say(language_, "the statement is nested too deeply", "\320\262 \320\262\321\213\321\200\320\260\320\266\320\265\320\275\320\270\320\270 \321\201\320\273\320\270\321\210\320\272\320\276\320\274 \320\263\320\273\321\203\320\261\320\276\320\272\320\260\321\217 \320\262\320\273\320\276\320\266\320\265\320\275\320\275\320\276\321\201\321\202\321\214"));
			} else if (statement->kind == StatementKind::Case || statement->kind == StatementKind::Gender) {
				result += grammar_of(*statement, head);
			} else if (statement->kind == StatementKind::Value) {
				result += value_of(text, *statement, head, end, consumed);
			} else {
				if (statement->unclosed)
					fail(written, say(language_, "the {{if}} is not closed with {{end}}", "{{if}} \320\275\320\265 \320\267\320\260\320\272\321\200\321\213\321\202 {{end}}"));
				const Worked worked = work_condition(head, language_);
				const Language language = language_;
				if (worked.value) result += run(worked.value->truth ? statement->when_true : statement->when_false, depth + 1, language);
				else fail(written, worked.reason, worked.error_kind);
			}
			cursor = end + consumed;
		}
		result.append(text, cursor, kNone);
		return result;
	}

	std::vector<StatementFailure> failures;

private:
	Language language_ = Language::English;

	void fail(const std::string &statement, const std::string &reason, const std::string &kind = {}) {
		// The count of values a function takes is no figure of the lawyer's.
		failures.push_back({statement, kind == "arity" ? reason : without_derived_figures(reason, statement)});
	}

	// A tag that opens no statement: an {{else}} or {{end}} without its {{if}},
	// or a statement never closed with "}}". Reported, and never left in the
	// lawyer's text. An unclosed statement ends at a "}}" on its line (one
	// whose operand mark was never closed: "{{= ⟦$5⟧ + ⟦$6}}"), else where its
	// arithmetic does: the words after it stay. Returns where the text goes on.
	std::size_t broken(const std::string &text, std::size_t at, const std::string &tag, std::size_t rest) {
		const std::size_t close = balanced_close(text, rest);
		if (!is_statement_tag(tag) && close != kNone) {
			const std::string written = text.substr(at, close + 2 - at);
			fail(written, say(language_, written + " stands without the {{if}} it belongs to", written + " \321\201\321\202\320\276\320\270\321\202 \320\261\320\265\320\267 \321\201\320\262\320\276\320\265\320\263\320\276 {{if}}"));
			return tag_span(text, at, close).second;
		}
		std::size_t line = text.find('\n', at);
		if (line == kNone) line = text.size();
		const std::size_t shut = text.find("}}", rest);
		const std::size_t nextOpen = text.find("{{", rest);
		const std::size_t end = shut != kNone && shut < line && (nextOpen == kNone || shut < nextOpen) ? shut + 2
			: arithmetic_end(text, rest, line);
		fail(text.substr(at, end - at), say(language_, "the statement is not closed with }}", "\320\262\321\213\321\200\320\260\320\266\320\265\320\275\320\270\320\265 \320\275\320\265 \320\267\320\260\320\272\321\200\321\213\321\202\320\276 }}"));
		return end;
	}

	// `end`: where the statement ends in `text`; `consumed`, how much of the
	// words after it its result replaces.
	std::string value_of(const std::string &text, const Statement &statement, const std::string &head, std::size_t end,
		std::size_t &consumed) {
		consumed = 0;
		const std::string written = text.substr(statement.start, statement.end - statement.start);
		const Worked worked = work_value(head, language_);
		if (worked.value) {
			const std::string_view whole = text;
			try {
				return written_value(worked, language_, whole.substr(0, statement.start), whole.substr(end, 96), consumed);
			} catch (const Problem &problem) {
				fail(written, problem.reason);
			} catch (const semantic::AlgebraError &error) {
				fail(written, refused_format(*worked.value, worked.format_text, error, language_));
			}
		} else {
			fail(written, worked.reason, worked.error_kind);
		}
		consumed = 0;
		// Never a guess and never a note: the one value the statement names, as
		// it is written, or nothing.
		const std::vector<std::string> operands = operands_in(head);
		return operands.size() == 1 ? trimmed(operands.front()) : std::string();
	}
};

// ---- Facts for a model --------------------------------------------------------------------------

std::string basis_phrase(semantic::RateBasis basis) {
	switch (basis) {
		case semantic::RateBasis::None: return "";
		case semantic::RateBasis::PerDay: return " per day";
		case semantic::RateBasis::PerMonth: return " per month";
		case semantic::RateBasis::PerYear: return " per annum";
	}
	return "";
}

// What a derived value is, so that protection gives its symbol the right type.
std::string kind_of(const semantic::Value &value) {
	switch (value.kind) {
		case semantic::Kind::Money:
			return (value.currency.empty() ? std::string("money, currency not known") : "money in " + value.currency) + basis_phrase(value.basis);
		case semantic::Kind::Number: return "number";
		case semantic::Kind::Percentage: return "percentage" + basis_phrase(value.basis);
		case semantic::Kind::Duration:
			switch (value.unit) {
				case semantic::DurationUnit::Day: return "period in days";
				case semantic::DurationUnit::BusinessDay: return "period in business days";
				case semantic::DurationUnit::Week: return "period in weeks";
				case semantic::DurationUnit::Month: return "period in months";
				case semantic::DurationUnit::Year: return "period in years";
				case semantic::DurationUnit::Quarter: return "period in quarters";
				case semantic::DurationUnit::Hour: return "period in hours";
				case semantic::DurationUnit::Minute: return "period in minutes";
			}
			break;
		case semantic::Kind::Date:
			switch (value.date.precision) {
				case temporal::Precision::Day: return "date with day precision";
				case temporal::Precision::Month: return "date with month precision";
				case temporal::Precision::Quarter: return "date with quarter precision";
				case temporal::Precision::Year: return "date with year precision";
			}
			break;
		case semantic::Kind::Boolean: return "true or false";
		case semantic::Kind::DayOfYear: return "day of the year (no year)";
		case semantic::Kind::Count: return "count";
	}
	return "";
}

// `read` is `value` written another way: the same identity for the same
// period (0.1 % per day is not 0.1 %: a symbol that lost its period would
// be multiplied by months as readily as by days), or the plain number a
// format asked to be money, a rate or a period, or one period in another unit.
bool same_value(const semantic::Value &read, const semantic::Value &value) {
	if (semantic::value_key(read) == semantic::value_key(value)) return read.basis == value.basis;
	if (value.kind == semantic::Kind::Number && read.kind != semantic::Kind::Date && read.kind != semantic::Kind::Boolean)
		return read.amount == value.amount;
	if (read.kind == semantic::Kind::Duration && value.kind == semantic::Kind::Duration) {
		const auto converted = period_in(value, read.unit);
		return converted && converted->amount == read.amount;
	}
	return false;
}

std::optional<std::string> exact_decimal(const semantic::Rational &number) { return number.exact_decimal(12); }

// A derived value as it goes inside ⟦ ⟧: a writing protection reads back as
// exactly this value, so that a symbol made from it stands for the value
// itself and never for its rounding ($100 / 3 is not $33.33). The display
// asked for, else the plain display, else a plain exact writing; `read` is
// the value the writing is read as.
std::optional<std::string> exact_writing(const semantic::Value &value, const Worked &worked, Language language, semantic::Value &read) {
	std::vector<std::string> writings;
	const auto add = [&](const semantic::Format &format) {
		try {
			writings.push_back(semantic::display(value, format, language));
		} catch (const semantic::AlgebraError &) {
		}
	};
	if (!worked.format_text.empty()) add(worked.format);
	add({});
	const auto exact = exact_decimal(value.amount);
	if (exact) {
		switch (value.kind) {
			case semantic::Kind::Money:
				if (!value.currency.empty()) writings.push_back(value.currency + " " + *exact);
				break;
			case semantic::Kind::Number: writings.push_back(*exact); break;
			case semantic::Kind::Percentage:
				if (const auto points = exact_decimal(value.amount * semantic::Rational(100)))
					writings.push_back(*points + "%" + basis_phrase(value.basis));
				break;
			case semantic::Kind::Duration: {
				const std::string noun = semantic::display(semantic::make_duration(semantic::Rational(2), value.unit), {}, Language::English);
				writings.push_back(*exact + noun.substr(noun.find(' ')));
				break;
			}
			default: break;
		}
	}
	for (const std::string &writing : writings) {
		const auto parsed = semantic::parse_value(writing);
		if (!parsed || !same_value(*parsed, value)) continue;
		read = *parsed;
		return writing;
	}
	return std::nullopt;
}

std::string failed_answer(const Worked &worked, const std::string &question) {
	const std::string reason = worked.error_kind == "arity" ? worked.reason : without_derived_figures(worked.reason, question);
	return (worked.undetermined ? "not determined: " : "could not be worked out: ") + reason;
}

std::string derived_answer(const Worked &worked, Language language);

// The condition of an expression that is an if(condition, a, b) as a whole,
// split at its own top-level comma - never one inside ⟦$2,500⟧ or a nested call.
std::optional<std::string> if_condition(const std::string &head) {
	std::size_t at = head.find_first_not_of(' ');
	if (at == std::string::npos || head.compare(at, 2, "if") != 0) return std::nullopt;
	at = head.find_first_not_of(' ', at + 2);
	if (at == std::string::npos || head[at] != '(') return std::nullopt;
	const std::string open = "\xE2\x9F\xA6";
	const std::string close = "\xE2\x9F\xA7";
	int depth = 0;
	bool marked = false;
	for (std::size_t index = at + 1; index < head.size(); ++index) {
		if (head.compare(index, open.size(), open) == 0) { marked = true; index += open.size() - 1; continue; }
		if (head.compare(index, close.size(), close) == 0) { marked = false; index += close.size() - 1; continue; }
		if (marked) continue;
		if (head[index] == '(') ++depth;
		else if (head[index] == ')') {
			if (depth == 0) return std::nullopt;
			--depth;
		} else if (head[index] == ',' && depth == 0) {
			std::string condition = head.substr(at + 1, index - at - 1);
			while (!condition.empty() && condition.front() == ' ') condition.erase(0, 1);
			while (!condition.empty() && condition.back() == ' ') condition.pop_back();
			if (condition.empty()) return std::nullopt;
			return condition;
		}
	}
	return std::nullopt;
}

std::string answer_of(const std::string &question) {
	const Language language = language_around(question);
	std::optional<Statement> statement;
	for (std::size_t at = question.find("{{"); at != kNone && !statement; at = question.find("{{", at + 1)) statement = statement_at(question, at);
	if (!statement || statement->kind == StatementKind::Case || statement->kind == StatementKind::Gender)
		return "could not be worked out: the question is not written as {{= expression}}";
	const std::string head = question.substr(statement->head_start, statement->head_end - statement->head_start);
	if (statement->kind == StatementKind::Condition) {
		const Worked worked = work_condition(head, language);
		if (!worked.value) return failed_answer(worked, question);
		return worked.value->truth ? "true" : "false";
	}
	const Worked worked = work_value(head, language);
	if (!worked.value) return failed_answer(worked, question);
	std::string answer = derived_answer(worked, language);
	// "if(c, a, b)": which branch was taken is said too - "Above the de minimis"
	// was written over a value whose test the model never saw (ab2 EN2, 25 Sep
	// 2026). The condition's operands stay inside the marks.
	if (const std::optional<std::string> condition = if_condition(head)) {
		const Worked tested = work_condition(*condition, language);
		if (tested.value && tested.value->kind == semantic::Kind::Boolean)
			answer += std::string("; the condition ") + *condition + (tested.value->truth ? " holds" : " does not hold");
	}
	return answer;
}

// A value worked out for a model: true or false, or the value inside the marks
// with its kind, where protection replaces it by a new symbol of that kind.
std::string derived_answer(const Worked &given, Language language) {
	if (given.value->kind == semantic::Kind::Boolean) return given.value->truth ? "true" : "false";
	// An amount of money is paid in cents (kopecks, pence; whole yen): a rent
	// scaled by 3.5 % is $63,480.00, never $63,479.99655, which the lawyer's
	// letter would carry, and which the next sum would add as it stands. So a
	// finer amount worked out is rounded half up to what is paid, and the model
	// is told so - the symbol stands for that amount, the one the lawyer reads.
	Worked worked = given;
	semantic::Value &value = *worked.value;
	std::string rounding;
	if (value.kind == semantic::Kind::Money && value.basis == semantic::RateBasis::None) {
		const int places = semantic::minor_unit_places(value.currency);
		try {
			const semantic::Rational paid = value.amount.rounded(places);
			if (paid != value.amount) {
				value.amount = paid;
				value.source_decimals = places;
				rounding = places == 0 ? ", rounded half up to a whole unit" : ", rounded half up to the cent";
			}
		} catch (const semantic::AlgebraError &) {
		}
	}
	// The value goes inside the marks, where protection replaces it by a
	// symbol of the kind it is written as.
	semantic::Value read;
	const auto written = exact_writing(value, worked, language, read);
	if (!written && value.kind == semantic::Kind::Money && value.basis != semantic::RateBasis::None)
		return "could not be worked out: " + say(language,
			"the result is an amount owed for each "
				+ std::string(value.basis == semantic::RateBasis::PerDay ? "day" : value.basis == semantic::RateBasis::PerMonth ? "month" : "year")
				+ ", which a symbol cannot carry; ask for it over the period it is owed for (times the days, months or years)",
			"\321\200\320\265\320\267\321\203\320\273\321\214\321\202\320\260\321\202 - \321\201\321\203\320\274\320\274\320\260 \320\267\320\260 \320\272\320\260\320\266\320\264\321\213\320\271 \320\277\320\265\321\200\320\270\320\276\320\264 \321\201\321\202\320\260\320\262\320\272\320\270, \320\272\320\276\321\202\320\276\321\200\321\203\321\216 \321\201\320\270\320\274\320\262\320\276\320\273 \320\277\320\265\321\200\320\265\320\264\320\260\321\202\321\214 \320\275\320\265 \320\274\320\276\320\266\320\265\321\202; \321\201\320\277\321\200\320\276\321\201\320\270\321\202\320\265 \321\201\321\203\320\274\320\274\321\203 \320\267\320\260 \320\262\320\265\321\201\321\214 \321\201\321\200\320\276\320\272 (\321\203\320\274\320\275\320\276\320\266\320\270\320\262 \320\275\320\260 \320\264\320\275\320\270, \320\274\320\265\321\201\321\217\321\206\321\213 \320\270\320\273\320\270 \320\263\320\276\320\264\321\213)");
	if (!written)
		return "could not be worked out: " + say(language,
			"the result has no exact decimal writing, so a symbol for it would stand for a rounding; ask for it rounded "
			"(op round with the places, or round(x, places) in an expression), or write the calculation itself where the "
			"figure is needed",
			"\321\203 \321\200\320\265\320\267\321\203\320\273\321\214\321\202\320\260\321\202\320\260 \320\275\320\265\321\202 \321\202\320\276\321\207\320\275\320\276\320\271 \320\264\320\265\321\201\321\217\321\202\320\270\321\207\320\275\320\276\320\271 \320\267\320\260\320\277\320\270\321\201\320\270, \320\270 \321\201\320\270\320\274\320\262\320\276\320\273 \320\264\320\273\321\217 \320\275\320\265\320\263\320\276 \320\276\320\267\320\275\320\260\321\207\320\260\320\273 \320\261\321\213 \320\276\320\272\321\200\321\203\320\263\320\273\320\265\320\275\320\270\320\265; \321\201\320\277\321\200\320\276\321\201\320\270\321\202\320\265 \320\265\320\263\320\276 \320\276\320\272\321\200\321\203\320\263\320\273\320\265\320\275\320\275\321\213\320\274 "
			"(op round \321\201 \321\207\320\270\321\201\320\273\320\276\320\274 \320\267\320\275\320\260\320\272\320\276\320\262 \320\270\320\273\320\270 round(x, places) \320\262 \320\262\321\213\321\200\320\260\320\266\320\265\320\275\320\270\320\270) \320\270\320\273\320\270 \320\275\320\260\320\277\320\270\321\210\320\270\321\202\320\265 \321\201\320\260\320\274 \321\200\320\260\321\201\321\207\320\265\321\202 \321\202\320\260\320\274, \320\263\320\264\320\265 \320\275\321\203\320\266\320\275\320\260 \321\206\320\270\321\204\321\200\320\260");
	return std::string(kOpen) + *written + std::string(kClose) + " (" + kind_of(read) + rounding + ")";
}

// ---- Typed facts ---------------------------------------------------------------------------------

// An operand as it goes into a calculation and back to the model: inside the
// marks, where protection gives a protected value its own symbol again and
// a derived one a new symbol - a raw value never stands outside them.
std::string operand_mark(const std::string &operand) {
	std::string text = trimmed(operand);
	if (starts_with_at(text, 0, kOpen) && text.size() >= kOpen.size() + kClose.size()
			&& text.compare(text.size() - kClose.size(), kClose.size(), kClose) == 0)
		text = trimmed(text.substr(kOpen.size(), text.size() - kOpen.size() - kClose.size()));
	return std::string(kOpen) + text + std::string(kClose);
}

// The unit a question asks a period in, from the words a model writes.
std::optional<semantic::DurationUnit> asked_unit(const std::string &unit, bool &known) {
	std::string words = lower_ascii(trimmed(unit));
	known = true;
	if (words.empty()) return std::nullopt;
	if (words.rfind("calendar ", 0) == 0) words = words.substr(9);
	if (words == "day" || words == "days") return semantic::DurationUnit::Day;
	if (words == "business day" || words == "business days" || words == "working day" || words == "working days")
		return semantic::DurationUnit::BusinessDay;
	if (words == "week" || words == "weeks") return semantic::DurationUnit::Week;
	if (words == "month" || words == "months") return semantic::DurationUnit::Month;
	if (words == "year" || words == "years") return semantic::DurationUnit::Year;
	known = false;
	return std::nullopt;
}

std::string unit_name(semantic::DurationUnit unit) {
	switch (unit) {
		case semantic::DurationUnit::Day: return "days";
		case semantic::DurationUnit::BusinessDay: return "business days";
		case semantic::DurationUnit::Week: return "weeks";
		case semantic::DurationUnit::Month: return "months";
		case semantic::DurationUnit::Year: return "years";
		case semantic::DurationUnit::Quarter: return "quarters";
		case semantic::DurationUnit::Hour: return "hours";
		case semantic::DurationUnit::Minute: return "minutes";
	}
	return "";
}

bool is_date_operand(const std::string &operand) {
	const auto value = semantic::parse_value(trimmed(operand));
	return value && (value->kind == semantic::Kind::Date || value->kind == semantic::Kind::DayOfYear);
}

// Whether a condition over marked operands holds: true, false, or not told -
// with why, a figure the reason works out kept inside the marks.
struct Told {
	std::optional<bool> truth;
	std::string answer;  // the whole answer when it is not told
};

Told tell(const std::string &condition, Language language) {
	const Worked worked = work_condition(condition, language);
	if (worked.value) return {worked.value->truth, {}};
	return {std::nullopt, failed_answer(worked, condition)};
}

// A relation, in words over the marked operands: "⟦a⟧ is before ⟦b⟧", or "⟦a⟧
// is not before ⟦b⟧", or why it is not told.
std::string relation_answer(const std::string &condition, const std::string &holds, const std::string &fails, Language language) {
	const Told told = tell(condition, language);
	if (!told.truth) return told.answer;
	return *told.truth ? holds : fails;
}

// Two quantities or two dates compared: which is larger, or earlier.
std::string comparison_answer(const std::string &a, const std::string &b, bool dates, Language language) {
	const Told equal = tell(a + " == " + b, language);
	if (equal.truth && *equal.truth) return a + (dates ? " is the same date as " : " is equal to ") + b;
	const Told greater = tell(a + " > " + b, language);
	if (greater.truth && *greater.truth) return a + (dates ? " is after " : " is greater than ") + b;
	const Told less = tell(a + " < " + b, language);
	if (less.truth && *less.truth) return a + (dates ? " is before " : " is less than ") + b;
	for (const Told *told : {&equal, &greater, &less})
		if (!told->truth && told->answer.rfind("not determined", 0) == 0) return told->answer;
	for (const Told *told : {&equal, &greater, &less})
		if (!told->truth) return told->answer;
	return "not determined: " + a + " and " + b + " are neither equal nor one larger than the other";
}

std::string value_answer(const std::string &expression, Language language) {
	const Worked worked = work_value(expression, language);
	if (!worked.value) return failed_answer(worked, expression);
	return derived_answer(worked, language);
}

// The period from one date to another, counted in `unit`: days and months
// exactly; weeks and years when the days or months make whole ones.
std::string period_answer(const std::string &from, const std::string &to, std::optional<semantic::DurationUnit> unit,
		Language language) {
	using semantic::DurationUnit;
	const DurationUnit counted = unit.value_or(DurationUnit::Day);
	if (counted == DurationUnit::BusinessDay) {
		// Business days follow the rule that governs them - the agreement's own
		// definition first (Context/BusinessCalendar) - and this path reads no
		// agreement: a question answered here, on values nothing protects, knows
		// no rule. The language a date is written in never picks a calendar. So
		// weekdays are counted and only what holds whatever the holidays are is
		// said: the count is at most that many business days, never exactly it.
		namespace business_calendar = context::business_calendar;
		const Worked start = work_value(from, language);
		const Worked end = work_value(to, language);
		if (!start.value || start.value->kind != semantic::Kind::Date) return failed_answer(start, from);
		if (!end.value || end.value->kind != semantic::Kind::Date) return failed_answer(end, to);
		if (start.value->date.precision != context::temporal::Precision::Day
				|| end.value->date.precision != context::temporal::Precision::Day)
			return "not determined: business days are counted between two days, and " + from + " or " + to
				+ " names a longer period";
		const business_calendar::BusinessDayRule weekdays;  // the weekday fallback: holidays not known
		const business_calendar::CountBound bound =
			business_calendar::count_bound(weekdays, start.value->date.first_day, end.value->date.first_day);
		Worked counted;
		counted.value = semantic::make_duration(semantic::Rational(bound.at_most), DurationUnit::BusinessDay);
		std::string answer = derived_answer(counted, language);
		if (answer.rfind(kOpen, 0) != 0) return answer;
		return "not determined exactly: at most " + answer.substr(0, answer.rfind(" ("))
			+ " (" + business_calendar::describe(weekdays) + "; the rule that governs business days is not known here)";
	}
	const bool inMonths = counted == DurationUnit::Month || counted == DurationUnit::Year;
	Worked worked = work_value(std::string(inMonths ? "months_between(" : "days_between(") + from + ", " + to + ")", language);
	if (!worked.value) return failed_answer(worked, from + " " + to);
	std::string note;
	if (counted == DurationUnit::Week || counted == DurationUnit::Year) {
		const auto converted = period_in(*worked.value, counted);
		if (converted && converted->amount.is_integer()) worked.value = converted;
		else note = counted == DurationUnit::Week ? "; not a whole number of weeks, so given in days"
			: "; not a whole number of years, so given in whole months";
	}
	if (inMonths) note += "; whole months completed";
	std::string answer = derived_answer(worked, language);
	if (!note.empty() && answer.size() > 1 && answer.back() == ')') answer.insert(answer.size() - 1, note);
	return answer;
}

// The answer to one typed question, and the question as it is repeated beside it.
std::string typed_answer(const FactQuestion &question, std::string &asked) {
	std::string op = lower_ascii(trimmed(question.op));
	std::vector<std::string> operands;
	std::string joined;
	for (const std::string &operand : question.operands) {
		if (trimmed(operand).empty()) continue;
		operands.push_back(operand_mark(operand));
		joined += (joined.empty() ? "" : ", ") + operands.back();
	}
	// A result is written in the language of the values it is made of.
	std::string all;
	for (const std::string &operand : question.operands) all += operand + " ";
	const Language language = semantic::language_of(all + question.expression);
	bool unitKnown = true;
	const std::optional<semantic::DurationUnit> unit = asked_unit(question.unit, unitKnown);
	asked = op + (joined.empty() ? std::string() : " " + joined) + (unit ? " in " + unit_name(*unit) : std::string());

	if (op == "expression" || (op.empty() && !trimmed(question.expression).empty())) {
		std::string expression = trimmed(question.expression);
		if (expression.empty() && operands.size() == 1) expression = trimmed(question.operands.front());
		asked = expression;
		if (expression.empty()) return "could not be worked out: the question has no expression";
		return answer_of(expression.find("{{") == kNone ? "{{= " + expression + "}}" : expression);
	}
	if (!unitKnown)
		return "could not be worked out: \"" + trimmed(question.unit) + "\" is not a unit Singularity counts in (days, business "
			"days, weeks, months, years)";
	const auto needs = [&](std::size_t fewest, std::size_t most) -> std::optional<std::string> {
		if (operands.size() >= fewest && operands.size() <= most) return std::nullopt;
		const std::string count = fewest == most ? std::to_string(fewest)
			: most == kNone ? "at least " + std::to_string(fewest) : std::to_string(fewest) + " or " + std::to_string(most);
		return "could not be worked out: " + op + " takes " + count + " operands, and the question gives "
			+ std::to_string(operands.size());
	};
	const auto chained = [&](const std::string &sign) {
		std::string expression;
		for (const std::string &operand : operands) expression += (expression.empty() ? std::string() : " " + sign + " ") + operand;
		return expression;
	};
	if (op == "compare") {
		if (auto wrong = needs(2, 2)) return *wrong;
		return comparison_answer(operands[0], operands[1],
			is_date_operand(question.operands[0]) || is_date_operand(question.operands[1]), language);
	}
	if (op == "before" || op == "after" || op == "same_day" || op == "overlaps" || op == "same_day_of_year") {
		if (auto wrong = needs(2, 2)) return *wrong;
		const std::string &a = operands[0];
		const std::string &b = operands[1];
		const std::string condition = op + "(" + a + ", " + b + ")";
		if (op == "before") return relation_answer(condition, a + " is before " + b, a + " is not before " + b, language);
		if (op == "after") return relation_answer(condition, a + " is after " + b, a + " is not after " + b, language);
		if (op == "same_day")
			return relation_answer(condition, a + " is the same day as " + b, a + " is not the same day as " + b, language);
		if (op == "overlaps") return relation_answer(condition, a + " and " + b + " overlap", a + " and " + b + " do not overlap", language);
		return relation_answer(condition, a + " falls on the same day of the year as " + b,
			a + " does not fall on the same day of the year as " + b, language);
	}
	if (op == "within") {
		if (auto wrong = needs(2, 3)) return *wrong;
		const std::string period = operands.size() == 2 ? operands[1] : operands[1] + " to " + operands[2];
		return relation_answer("within(" + joined + ")", operands[0] + " is within " + period,
			operands[0] + " is not within " + period, language);
	}
	if (op == "difference") {
		if (auto wrong = needs(2, 2)) return *wrong;
		if (is_date_operand(question.operands[0]) && is_date_operand(question.operands[1]))
			return period_answer(operands[1], operands[0], unit, language);
		return value_answer(operands[0] + " - " + operands[1], language);
	}
	if (op == "duration_between") {
		if (auto wrong = needs(2, 2)) return *wrong;
		return period_answer(operands[0], operands[1], unit, language);
	}
	if (op == "sum") {
		if (auto wrong = needs(1, kNone)) return *wrong;
		return value_answer(chained("+"), language);
	}
	if (op == "product") {
		if (auto wrong = needs(2, kNone)) return *wrong;
		return value_answer(chained("*"), language);
	}
	if (op == "ratio") {
		if (auto wrong = needs(2, 2)) return *wrong;
		return value_answer(operands[0] + " / " + operands[1], language);
	}
	if (op == "round") {
		if (auto wrong = needs(1, 2)) return *wrong;
		// The places are the model's own figure; an amount of money is rounded to
		// what is paid unless it asks for other places.
		int places = 0;
		if (operands.size() == 2) {
			const auto count = semantic::parse_value(trimmed(question.operands[1]));
			if (!count || count->kind != semantic::Kind::Number || !count->amount.is_integer()
					|| count->amount < semantic::Rational(-9) || count->amount > semantic::Rational(9))
				return "could not be worked out: round takes the places to round to as a whole number from -9 to 9, "
					"after the value";
			places = static_cast<int>(count->amount.numerator());
		} else {
			const auto value = semantic::parse_value(trimmed(question.operands[0]));
			if (!value || value->kind != semantic::Kind::Money)
				return "could not be worked out: round needs the places to round to (a second operand such as 2) for "
					"anything but an amount of money";
			places = semantic::minor_unit_places(value->currency);
		}
		return value_answer("round(" + operands[0] + ", " + std::to_string(places) + ")", language);
	}
	if (op == "add_period") {
		if (auto wrong = needs(2, 2)) return *wrong;
		// A bare count with the unit asked: "12" in months is 12 months.
		std::string period = operands[1];
		const auto count = semantic::parse_value(trimmed(question.operands[1]));
		if (unit && count && count->kind == semantic::Kind::Number)
			period = operand_mark(trimmed(question.operands[1]) + " " + unit_name(*unit));
		return value_answer(operands[0] + " + " + period, language);
	}
	if (op == "next_occurrence") {
		if (auto wrong = needs(2, 2)) return *wrong;
		return value_answer("next_occurrence(" + joined + ")", language);
	}
	return "could not be worked out: \"" + op + "\" is not an operation Singularity knows (compare, difference, sum, ratio, "
		"product, round, before, after, same_day, within, overlaps, duration_between, add_period, next_occurrence, "
		"same_day_of_year, expression)";
}

} // namespace

bool has_statements(const std::string &text) {
	// Any statement tag, even one never closed: validation must see it.
	std::size_t from = 0;
	while ((from = text.find("{{", from)) != kNone) {
		std::size_t rest = 0;
		if (!tag_at(text, from, rest).empty()) return true;
		from += 1;
	}
	return false;
}

std::string mark_statement_operands(const std::string &provider_text) {
	std::string result;
	std::size_t cursor = 0;
	while (cursor < provider_text.size()) {
		const std::size_t next = provider_text.find("{{", cursor);
		if (next == kNone) break;
		std::size_t rest = 0;
		const std::string tag = tag_at(provider_text, next, rest);
		const std::size_t close = is_statement_tag(tag) || tag == "elseif" ? balanced_close(provider_text, rest) : kNone;
		if (close == kNone) {
			result.append(provider_text, cursor, next + 1 - cursor);
			cursor = next + 1;
			continue;
		}
		result.append(provider_text, cursor, rest - cursor);
		// Inside the head - statements nested in it too - "[[E3]]" and
		// "[[E5.first]]" become "⟦[[E3]]⟧", "⟦[[E5.first]]⟧", once.
		const std::string head = provider_text.substr(rest, close - rest);
		std::size_t at = 0;
		while (at < head.size()) {
			const std::size_t open = head.find("[[", at);
			if (open == kNone) break;
			const std::size_t shut = head.find("]]", open + 2);
			if (shut == kNone) break;
			result.append(head, at, open - at);
			const bool marked = open >= kOpen.size() && head.compare(open - kOpen.size(), kOpen.size(), kOpen) == 0;
			if (marked) result += head.substr(open, shut + 2 - open);
			else result += std::string(kOpen) + head.substr(open, shut + 2 - open) + std::string(kClose);
			at = shut + 2;
		}
		result.append(head, at, kNone);
		result += "}}";
		cursor = close + 2;
	}
	result.append(provider_text, cursor, kNone);
	return result;
}

namespace {

// A localFacts question's expression written without its {{= }}, as the
// statement it is.
std::string as_statement(const std::string &expression) {
	return trimmed(expression).empty() || expression.find("{{") != kNone ? expression : "{{= " + expression + "}}";
}

bool is_fact_expression_locator(const std::string &json_locator) {
	static const std::string kField = "/expression";
	return json_locator.find("/localFacts/questions/") != kNone && json_locator.size() >= kField.size()
		&& json_locator.compare(json_locator.size() - kField.size(), kField.size(), kField) == 0;
}

// A response document carried in a string - the research protocol's final
// text - with each localFacts question's expression as a statement: the
// JSON string after an "expression" key that follows "localFacts".
std::string with_fact_expressions_as_statements(const std::string &document) {
	const std::size_t facts = document.find("\"localFacts\"");
	if (facts == kNone) return document;
	static const std::string kKey = "\"expression\"";
	std::string result;
	std::size_t cursor = 0;
	std::size_t at = facts;
	const auto spaces = [&](std::size_t position) {
		while (position < document.size()
				&& (document[position] == ' ' || document[position] == '\n' || document[position] == '\r' || document[position] == '\t'))
			++position;
		return position;
	};
	while ((at = document.find(kKey, at)) != kNone) {
		std::size_t colon = spaces(at + kKey.size());
		at += kKey.size();
		if (colon >= document.size() || document[colon] != ':') continue;
		const std::size_t open = spaces(colon + 1);
		if (open >= document.size() || document[open] != '"') continue;
		std::size_t close = open + 1;
		while (close < document.size() && document[close] != '"') close += document[close] == '\\' ? 2 : 1;
		if (close >= document.size()) break;
		const std::string literal = document.substr(open + 1, close - open - 1);
		if (!trimmed(literal).empty() && literal.find("{{") == kNone) {
			result.append(document, cursor, open + 1 - cursor);
			result += "{{= " + literal + "}}";
			cursor = close;
		}
		at = close + 1;
	}
	result.append(document, cursor, kNone);
	return result;
}

} // namespace

std::string mark_response_operands(const std::string &provider_text, const std::string &json_locator) {
	return mark_statement_operands(is_fact_expression_locator(json_locator) ? as_statement(provider_text)
		: with_fact_expressions_as_statements(provider_text));
}

CheckedText compute_checked(const std::string &text) {
	if (text.find("{{") == kNone && text.find(kOpen) == kNone) return {text, {}};
	Worker worker;
	CheckedText checked;
	checked.text = without_operand_marks(worker.run(text, 0, language_around(text)));
	checked.failures = std::move(worker.failures);
	return checked;
}

std::string compute(const std::string &text) { return compute_checked(text).text; }

std::string answer_local_facts(const std::vector<std::string> &questions) {
	std::string block;
	for (std::size_t index = 0; index < questions.size(); ++index) {
		// One line per question, as the model wrote it.
		std::string question = trimmed(questions[index]);
		for (char &character : question)
			if (character == '\n' || character == '\r') character = ' ';
		block += std::to_string(index + 1) + ". " + question + " \xE2\x86\x92 " + answer_of(question) + "\n";
	}
	return block;
}

std::string answer_typed_facts(const std::vector<FactQuestion> &questions) {
	std::string block;
	for (std::size_t index = 0; index < questions.size(); ++index) {
		std::string asked;
		const std::string answer = typed_answer(questions[index], asked);
		for (char &character : asked)
			if (character == '\n' || character == '\r') character = ' ';
		block += std::to_string(index + 1) + ". " + asked + " \xE2\x86\x92 " + answer + "\n";
	}
	return block;
}

std::optional<std::string> exact_writing_of(const semantic::Value &value) {
	semantic::Value read;
	return exact_writing(value, Worked{}, Language::English, read);
}

WorkedValue work_out(const std::string &expression, Language language) {
	WorkedValue result;
	const semantic::Evaluation evaluation = semantic::evaluate(expression, language);
	if (evaluation.error) {
		const std::string &kind = evaluation.error->kind;
		result.failure = kind == "undetermined" ? "undetermined" : kind == "currency" ? "currency_mismatch"
			: kind == "type" || kind == "operand" ? "not_a_quantity" : kind == "division_by_zero" ? "division_by_zero"
			: "not_worked_out";
		return result;
	}
	if (!evaluation.value) {
		result.failure = "not_worked_out";
		return result;
	}
	semantic::Value value = *evaluation.value;
	if (value.kind == semantic::Kind::Money && value.basis == semantic::RateBasis::None) {
		const int places = semantic::minor_unit_places(value.currency);
		try {
			const semantic::Rational paid = value.amount.rounded(places);
			if (paid != value.amount) {
				value.amount = paid;
				value.source_decimals = places;
				result.rounded = true;
			}
		} catch (const semantic::AlgebraError &) {
		}
	}
	if (value.kind != semantic::Kind::Boolean && !exact_writing_of(value)) {
		result.failure = value.kind == semantic::Kind::Money && value.basis != semantic::RateBasis::None
			? "per_period" : "no_exact_writing";
		return result;
	}
	result.value = std::move(value);
	return result;
}

} // namespace singularity::context::computed_statements
