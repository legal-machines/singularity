#pragma once

#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"

#include <cstddef>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A relation Singularity worked out on this device and sent, read for the lawyer:
// "[[Amount7]] > [[Amount8]]" as "$65,125 > $21,350". Pure; the originals come
// from the operation's own passages (PassageDisclosure), never from a record -
// no record holds a value.
namespace singularity::privacy::passage_disclosure {

namespace relation_reading {

// "[[Amount7]]" of "[[Amount7.short]]" or of itself; anything else unchanged.
inline std::string base_of(const std::string &written) {
	if (written.size() < 5 || written.compare(0, 2, "[[") != 0 || written.compare(written.size() - 2, 2, "]]") != 0)
		return written;
	const std::string inside = written.substr(2, written.size() - 4);
	const std::size_t part = inside.find('.');
	return "[[" + (part == std::string::npos ? inside : inside.substr(0, part)) + "]]";
}

// Every "[[...]]" of a text, with where it stands.
inline std::vector<std::pair<std::size_t, std::size_t>> symbol_spans(const std::string &text) {
	std::vector<std::pair<std::size_t, std::size_t>> spans;
	for (std::size_t open = text.find("[["); open != std::string::npos; open = text.find("[[", open + 2)) {
		const std::size_t close = text.find("]]", open + 2);
		if (close == std::string::npos) break;
		spans.emplace_back(open, close + 2 - open);
		open = close;
	}
	return spans;
}

} // namespace relation_reading

// `relation.sent` with every symbol written as what it stood for, from
// `originals` (a base marker to its local writing). A value Singularity worked out
// stands in no passage: it is worked out again from `derivation` over the
// operands' originals. Nothing when any symbol stays unknown.
inline std::optional<std::string> local_reading(const protected_api::papinian::OutboundRelation &relation,
		std::map<std::string, std::string> originals) {
	namespace semantic = protected_api::papinian::semantic;
	using relation_reading::base_of;
	using relation_reading::symbol_spans;
	if (relation.derivation.empty() == false)
		for (const std::string &result : relation.results) {
			if (originals.count(result) > 0) continue;
			std::string expression = relation.derivation;
			const std::size_t equals = expression.find(" = ");
			if (equals != std::string::npos && expression.compare(0, equals, result) == 0) expression.erase(0, equals + 3);
			bool known = true;
			std::string worked;
			std::size_t cursor = 0;
			for (const auto &[at, length] : symbol_spans(expression)) {
				const auto original = originals.find(base_of(expression.substr(at, length)));
				if (original == originals.end()) {
					known = false;
					break;
				}
				worked.append(expression, cursor, at - cursor);
				worked += "\xE2\x9F\xA6" + original->second + "\xE2\x9F\xA7";
				cursor = at + length;
			}
			if (known == false) continue;
			worked.append(expression, cursor, std::string::npos);
			const context::computed_statements::WorkedValue value = context::computed_statements::work_out(worked);
			if (value.value.has_value() == false) continue;
			try {
				originals.emplace(result, semantic::display(*value.value));
			} catch (const std::exception &) {
			}
		}
	std::string local;
	std::size_t cursor = 0;
	for (const auto &[at, length] : symbol_spans(relation.sent)) {
		const auto original = originals.find(base_of(relation.sent.substr(at, length)));
		if (original == originals.end()) return std::nullopt;
		local.append(relation.sent, cursor, at - cursor);
		local += original->second;
		cursor = at + length;
	}
	local.append(relation.sent, cursor, std::string::npos);
	return local;
}

// ---------------------------------------------------------------------------
// An operation Singularity worked out at the model's request (DESIGN4 §1.5, §11.3),
// read for the lawyer from the line the model was sent back:
//   "q1 ok [[Amount3]] MONEY(USD) profit := subtract([[Amount1]], [[Amount2]]) · POSITIVE"
//     -> "$1,240,000.00 − $1,017,000.00 = $223,000.00"
//   "q3 ok TRUE greater_or_equal([[Rate2]], [[Rate1]])" -> "17.98% is at least 17.5%: yes"
// with every symbol's original (`originals`, a base marker to its local
// writing). A value an operation worked out stands in no passage: it is worked
// out again here when the operation is arithmetic, and added to `originals`
// for the operations after it. Nothing when a symbol stays unknown, or for a
// line that returned no result (an error, a refusal).

namespace operation_reading {

// The op's name and its arguments, top-level, of "op(a, "4%", b)".
inline bool split_call(const std::string &call, std::string &op, std::vector<std::string> &args) {
	const std::size_t open = call.find('(');
	const std::size_t close = call.rfind(')');
	if (open == std::string::npos || close == std::string::npos || close < open) return false;
	op = call.substr(0, open);
	while (!op.empty() && op.back() == ' ') op.pop_back();
	std::size_t level = 0;
	bool quoted = false;
	std::string current;
	for (std::size_t at = open + 1; at < close; ++at) {
		const char character = call[at];
		if (character == '"') quoted = !quoted;
		if (!quoted && character == '(') ++level;
		if (!quoted && character == ')' && level > 0) --level;
		if (!quoted && level == 0 && (character == ',' || character == ';')) {
			args.push_back(current);
			current.clear();
			if (character == ';') return true;
			continue;
		}
		current += character;
	}
	args.push_back(current);
	for (std::string &arg : args) {
		while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
		while (!arg.empty() && arg.back() == ' ') arg.pop_back();
		if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') arg = arg.substr(1, arg.size() - 2);
	}
	return true;
}

// An argument as the lawyer reads it: a symbol's original, or the text itself.
inline std::optional<std::string> read_argument(const std::string &arg, const std::map<std::string, std::string> &originals) {
	if (arg.rfind("[[", 0) != 0) return arg;
	const auto found = originals.find(relation_reading::base_of(arg));
	if (found == originals.end()) return std::nullopt;
	return found->second;
}

inline const char *infix_of(const std::string &op) {
	if (op == "add" || op == "sum") return " + ";
	if (op == "subtract") return " \xE2\x88\x92 ";
	if (op == "multiply") return " \xC3\x97 ";
	if (op == "divide" || op == "ratio") return " \xC3\xB7 ";
	return nullptr;
}

inline const char *comparison_of(const std::string &op) {
	if (op == "equal") return " equals ";
	if (op == "not_equal") return " differs from ";
	if (op == "greater_than") return " is more than ";
	if (op == "less_than") return " is less than ";
	if (op == "greater_or_equal") return " is at least ";
	if (op == "less_or_equal") return " is at most ";
	if (op == "before") return " is before ";
	if (op == "after") return " is after ";
	if (op == "same_time") return " is the same time as ";
	return nullptr;
}

} // namespace operation_reading

inline std::optional<std::string> local_operation_reading(const std::string &returned,
		std::map<std::string, std::string> &originals) {
	namespace semantic = protected_api::papinian::semantic;
	using namespace operation_reading;
	// "q1 ok ...": its id, its status, then what it gave.
	const std::size_t first = returned.find(' ');
	if (first == std::string::npos) return std::nullopt;
	const std::size_t second = returned.find(' ', first + 1);
	if (second == std::string::npos || returned.compare(first + 1, second - first - 1, "ok") != 0) return std::nullopt;
	std::string rest = returned.substr(second + 1);
	std::string result;
	std::string relation;
	std::string derivation;
	if (rest.rfind("[[", 0) == 0) {
		result = relation_reading::base_of(rest.substr(0, rest.find("]]") + 2));
		const std::size_t made = rest.find(":= ");
		if (made == std::string::npos) return std::nullopt;
		derivation = rest.substr(made + 3);
	} else {
		const std::size_t space = rest.find(' ');
		if (space == std::string::npos) return std::nullopt;
		relation = rest.substr(0, space);
		derivation = rest.substr(space + 1);
	}
	// A difference's sign follows " · ".
	const std::size_t dot = derivation.find(" \xC2\xB7 ");
	if (dot != std::string::npos) derivation = derivation.substr(0, dot);
	std::string op;
	std::vector<std::string> args;
	if (!split_call(derivation, op, args)) return std::nullopt;
	std::vector<std::string> read;
	for (const std::string &arg : args) {
		const std::optional<std::string> value = read_argument(arg, originals);
		if (!value) return std::nullopt;
		read.push_back(*value);
	}
	if (!result.empty()) {
		const char *infix = infix_of(op);
		if (infix != nullptr && read.size() >= 2 && originals.count(result) == 0) {
			// Worked out again from the operands' writings.
			std::string expression;
			for (std::size_t index = 0; index < read.size(); ++index) {
				if (index > 0) expression += op == "add" || op == "sum" ? " + " : op == "subtract" ? " - " : op == "multiply" ? " * " : " / ";
				expression += "\xE2\x9F\xA6" + read[index] + "\xE2\x9F\xA7";
			}
			const context::computed_statements::WorkedValue worked = context::computed_statements::work_out(expression);
			if (worked.value.has_value()) {
				try {
					originals.emplace(result, semantic::display(*worked.value));
				} catch (const std::exception &) {
				}
			}
		}
		const auto known = originals.find(result);
		if (known == originals.end()) return std::nullopt;
		if (infix != nullptr && read.size() >= 2) {
			std::string line;
			for (std::size_t index = 0; index < read.size(); ++index) line += (index == 0 ? "" : infix) + read[index];
			return line + " = " + known->second;
		}
		std::string line = op + "(";
		for (std::size_t index = 0; index < read.size(); ++index) line += (index == 0 ? "" : ", ") + read[index];
		return line + ") = " + known->second;
	}
	const char *comparison = comparison_of(op);
	if (comparison != nullptr && read.size() == 2) {
		if (relation == "TRUE") return read[0] + comparison + read[1] + ": yes";
		if (relation == "FALSE") return read[0] + comparison + read[1] + ": no";
	}
	if (op == "compare" && read.size() == 2) {
		if (relation == "LESS") return read[0] + " is less than " + read[1];
		if (relation == "EQUAL") return read[0] + " equals " + read[1];
		if (relation == "GREATER") return read[0] + " is more than " + read[1];
	}
	if (op == "sign" && read.size() == 1) {
		if (relation == "POSITIVE") return read[0] + " is positive";
		if (relation == "NEGATIVE") return read[0] + " is negative";
		if (relation == "ZERO") return read[0] + " is zero";
	}
	std::string line = op + "(";
	for (std::size_t index = 0; index < read.size(); ++index) line += (index == 0 ? "" : ", ") + read[index];
	return line + "): " + relation;
}

// Why a relation was sent, in the lawyer's words, from its closed justification.
inline std::string relation_reason(const std::string &justification) {
	static const std::map<std::string, std::string> intents{
		{"total", "a total"}, {"shortfall", "what is short or owed"}, {"compare", "a comparison"},
		{"deadline", "a deadline"}, {"business_days", "business days"}, {"days_between", "the days between dates"},
		{"escalation", "an increase"}, {"contact", "whose contact details are whose"}, {"chronology", "a chronology"},
		{"sheet_check", "a check of the sheet"}};
	if (justification.rfind("question_intent:", 0) == 0) {
		const auto found = intents.find(justification.substr(16));
		return "Your question asked for " + (found == intents.end() ? std::string("this") : found->second) + ".";
	}
	if (justification == "question_names_operand") return "Your question names this figure.";
	if (justification == "model_operation") return "The model asked Singularity to work this out.";
	if (justification == "lawyer_value_in_request") return "You wrote this figure in your request.";
	if (justification == "sheet_total_check") return "To check the sheet's own total.";
	return "Your question needed it.";
}

} // namespace singularity::privacy::passage_disclosure
