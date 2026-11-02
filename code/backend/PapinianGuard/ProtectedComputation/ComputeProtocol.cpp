#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/ProtectedOperations/TemporalOps.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Platform/TextFold/TextFold.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace singularity::protected_api::papinian::compute {

namespace {

namespace control = singularity::context::control_text;
using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// The closed vocabulary of the schema (DESIGN4 §2.1).

const std::vector<std::string> &where_tests() {
	static const std::vector<std::string> tests{"eq", "ne", "gt", "lt", "ge", "le", "before", "after", "on_or_before",
		"on_or_after", "within", "outside", "blank", "not_blank", "hidden", "visible"};
	return tests;
}

const std::vector<std::string> &unit_words() {
	static const std::vector<std::string> units{"minutes", "hours", "days", "calendar_days", "business_days", "weeks",
		"months", "quarters", "years"};
	return units;
}

const std::vector<std::string> &rounding_words() {
	static const std::vector<std::string> words{"half_up", "half_even", "down", "up"};
	return words;
}

const std::vector<std::string> &fn_words() {
	static const std::vector<std::string> words{"sum", "count", "min", "max", "average"};
	return words;
}

const std::vector<std::string> &mode_words() {
	static const std::vector<std::string> words{"share", "increase", "decrease", "le", "ge"};
	return words;
}

const std::vector<std::string> &expect_words() {
	static const std::vector<std::string> words{"MONEY", "NUMBER", "COUNT", "RATE", "PERCENTAGE", "DATE", "DURATION",
		"BOOLEAN", "ORDER", "SIGN", "ROW", "ROW_SET", "GROUP_SET", "TABLE", "SCHEDULE", "COLUMN", "CALENDAR"};
	return words;
}

bool one_of(const std::vector<std::string> &words, const std::string &word) {
	return std::find(words.begin(), words.end(), word) != words.end();
}

// The signature of each op (DESIGN4 §1.9): args in order; [ ] optional;
// options after ";". Ops that share one line share its text.
struct Signature {
	std::vector<std::string> ops;
	std::string line;
};

const std::vector<std::vector<Signature>> &signature_table() {
	static const std::vector<std::vector<Signature>> lines{
		{{{"add", "subtract"}, "add|subtract(a, b, \xE2\x80\xA6): subtract is a less the rest"},
			{{"multiply", "divide"}, "multiply|divide(a, b)"},
			{{"sum", "average", "min", "max"}, "sum|average|min|max(a, b, \xE2\x80\xA6)"},
			{{"abs", "positive_part"}, "abs|positive_part(a)"},
			{{"round"}, "round(a; places, rounding)"}},
		{{{"equal", "not_equal", "greater_than", "less_than", "greater_or_equal", "less_or_equal", "compare"},
				"equal|not_equal|greater_than|less_than|greater_or_equal|less_or_equal|compare(a, b)"},
			{{"between"}, "between(x, lo, hi)"},
			{{"sign"}, "sign(a)"}},
		{{{"apply_rate"}, "apply_rate(amount, rate[, duration]; mode=share|increase|decrease)"},
			{{"ratio"}, "ratio(part, whole)"},
			{{"percentage_of"}, "percentage_of(percent, base)"},
			{{"percentage_change"}, "percentage_change(old, new)"}},
		{{{"duration_between"}, "duration_between(from, to; unit[, calendar])"},
			{{"add_duration"}, "add_duration(date, duration[; calendar])"},
			{{"deadline"}, "deadline(date, duration, calendar)"}},
		{{{"within", "outside"}, "within|outside(date, start, end) | within|outside(date, anchor, \xC2\xB1" "duration)"},
			{{"before", "after", "same_time", "overlaps"}, "before|after|same_time|overlaps(a, b)"},
			{{"start_of", "end_of"}, "start_of|end_of(date; unit[, anchor])"},
			{{"days_in"}, "days_in(date)"},
			{{"next_business_day"}, "next_business_day(date, calendar)"},
			{{"day_class"}, "day_class(date, calendar)"},
			{{"next_occurrence"}, "next_occurrence(day_of_year, date)"}},
		{{{"filter"}, "filter(table|rows; where=[{column, test, value, value2}])"},
			{{"count"}, "count(rows)"},
			{{"list_rows"}, "list_rows(rows)"},
			{{"sum_column", "average_column", "min_column", "max_column"},
				"sum_column|average_column|min_column|max_column(rows, column)"}},
		{{{"derive_column"}, "derive_column(rows, op, column|value, column|value)"},
			{{"group"}, "group(rows, key_column[; unit, anchor])"},
			{{"aggregate"}, "aggregate(groups, column; fn)"}},
		{{{"lookup"}, "lookup(table|schedule, key_column, value, result_column; mode=le|ge)"},
			{{"progressive"}, "progressive(value, schedule)"},
			{{"cell"}, "cell([[Table1]].B12)"},
			{{"recalc"}, "recalc([[Table1]].E19)"}},
		{{{"union", "intersect", "minus"}, "union|intersect|minus(rows, rows)"},
			{{"describe_tables"}, "describe_tables(DOC_n)"},
			{{"and", "or"}, "and|or(a, b, \xE2\x80\xA6)"},
			{{"not"}, "not(a)"},
			{{"if_then_else"}, "if_then_else(flag, a, b)"}},
	};
	return lines;
}

// ---------------------------------------------------------------------------
// Reading what the model wrote.

bool is_upper(char character) { return character >= 'A' && character <= 'Z'; }
bool is_lower(char character) { return character >= 'a' && character <= 'z'; }
bool is_digit(char character) { return character >= '0' && character <= '9'; }

std::string trimmed(std::string_view text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos) return {};
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

// "[[Prefix12]]" or "[[Prefix12.part]]" at the start of `text`: its length, the
// prefix, the digits and the part; 0 when none stands there.
struct SymbolShape {
	std::size_t length = 0;
	std::string prefix;
	std::string digits;
	std::string part;
};

SymbolShape symbol_at(std::string_view text) {
	SymbolShape shape;
	if (text.substr(0, 2) != "[[") return shape;
	std::size_t cursor = 2;
	if (cursor >= text.size() || !is_upper(text[cursor])) return shape;
	const std::size_t prefix = cursor;
	while (cursor < text.size() && (is_upper(text[cursor]) || is_lower(text[cursor])) && cursor - prefix < 24) ++cursor;
	const std::size_t number = cursor;
	while (cursor < text.size() && is_digit(text[cursor]) && cursor - number < 6) ++cursor;
	if (cursor == number) return shape;
	std::string part;
	if (cursor < text.size() && text[cursor] == '.') {
		const std::size_t partStart = ++cursor;
		while (cursor < text.size() && is_lower(text[cursor]) && cursor - partStart < 16) ++cursor;
		if (cursor == partStart) return shape;
		part = std::string(text.substr(partStart, cursor - partStart));
	}
	if (text.substr(cursor, 2) != "]]") return shape;
	shape.length = cursor + 2;
	shape.prefix = std::string(text.substr(prefix, number - prefix));
	shape.digits = std::string(text.substr(number, (part.empty() ? cursor : cursor - part.size() - 1) - number));
	shape.part = std::move(part);
	return shape;
}

bool all_upper_letters(std::string_view text, std::size_t most) {
	if (text.empty() || text.size() > most) return false;
	return std::all_of(text.begin(), text.end(), [](char character) { return is_upper(character); });
}

bool all_digits(std::string_view text, std::size_t most) {
	if (text.empty() || text.size() > most) return false;
	return std::all_of(text.begin(), text.end(), [](char character) { return is_digit(character); });
}

TypeError error_of(Code code, std::size_t argument = 0, std::string hint = {}) {
	TypeError error;
	error.code = code;
	error.argument = argument;
	error.hint = std::move(hint);
	return error;
}

// A string field of an op, as written; a number the model wrote bare is read
// as the text it writes ("2" for 2).
std::optional<std::string> text_of(const Json &value) {
	if (value.is_string()) return value.get<std::string>();
	if (value.is_number() || value.is_boolean()) return value.dump();
	return std::nullopt;
}

// Whether `key` is present and not null.
bool present(const Json &object, const char *key) {
	return object.contains(key) && !object.at(key).is_null();
}

// ---------------------------------------------------------------------------
// Writing Singularity's lines.

// A slot value that is not of its kind is never rendered (control text can
// never be made to carry data): its place falls back to a closed word.
std::string safe_type(const ArgType &type) {
	const std::string written_type = written(type);
	if (control::fits(control::Slot::Kind::TypeName, written_type)) return written_type;
	const std::size_t open = written_type.find('(');
	return open == std::string::npos ? std::string("NUMBER") : written_type.substr(0, open);
}

std::string label_for(const OpNode &node) {
	if (node.bind && control::fits(control::Slot::Kind::Label, *node.bind)) return *node.bind;
	return node.id;
}

// The hint of an error line: an op's signature for the errors that are about
// its arguments; the executor's own closed hint when it gave one; else Singularity's
// hint for the code.
// Whether an executor's hint is closed words Singularity can pass on (DESIGN4
// §10: no raw value in any error line): letters, spaces, punctuation and
// symbols its table issued - never a figure, a quotation or anything long.
bool closed_hint(std::string_view hint) {
	if (hint.empty() || hint.size() > 240) return false;
	for (std::size_t at = 0; at < hint.size();) {
		if (hint.compare(at, 2, "[[") == 0) {
			const std::size_t close = hint.find("]]", at);
			if (close == std::string_view::npos
					|| !control::fits(control::Slot::Kind::Symbol, hint.substr(at, close + 2 - at)))
				return false;
			at = close + 2;
			continue;
		}
		const char character = hint[at];
		if (is_digit(character) || character == '"' || character == '\'' || character == '\n' || character == '\r')
			return false;
		++at;
	}
	return true;
}

std::string hint_for(const OpNode &node, const TypeError &error) {
	// A hint the evaluator wrote for this very mistake says more than the
	// signature ("a date column is tested against a date").
	if (error.code == Code::TypeMismatch && closed_hint(error.hint)) return error.hint;
	if (error.code == Code::Arity || error.code == Code::TypeMismatch) {
		const std::string signature = signature_of(node.op);
		if (!signature.empty()) return signature;
	}
	if (closed_hint(error.hint)) return error.hint;
	switch (error.code) {
		case Code::BadJson: return "write each operation as the schema shows: id, op, args, where, options, bind, expect, why";
		case Code::BadId: return "ids are q1, q2, q3 ... unique in the reply";
		case Code::DuplicateId: return "ids are q1, q2, q3 ... unique in the reply";
		case Code::BadLabel: return "a name is lower-case letters, digits and _, at most 48, with no long run of digits";
		case Code::BindReused: return "this name is already used: choose another name";
		case Code::UnknownOp: return "use one of the operations the instruction lists";
		case Code::Arity: return "give the operation the arguments its signature lists";
		case Code::UnknownSymbol: return "use a symbol this request shows";
		// No "@" in a line protection reads again: "@name" was once taken for an
		// e-mail address and masked (live, 26 Sep 2026).
		case Code::UnknownLabel: return "no earlier result of this task has that name: use a name an earlier op bound";
		case Code::DependencyFailed: return "an operation it uses gave no result";
		case Code::TypeMismatch: return "the operands do not fit this operation";
		case Code::CurrencyMismatch: return "amounts in different currencies are not combined";
		case Code::BasisMismatch: return "amounts or rates for different periods are not combined";
		case Code::PerPeriod: return "a per-period rate needs a duration: add the period as a third argument";
		case Code::UnitRequired: return "say the unit in options.unit";
		case Code::CalendarRequired: return "business days need a calendar: a [[Calendar]] handle, governing or calendar_days";
		case Code::UnknownCalendar: return "use a [[Calendar]] handle the request shows, governing or calendar_days";
		case Code::RuleConflict:
		case Code::DefinitionNotFound:
		case Code::RuleUnknown: return "the business-day rule is not certain: ask the lawyer, or count calendar_days";
		case Code::GroupKeyType: return "group by a name, a text column, or a date column with a unit";
		case Code::DepthExceeded: return "work it out in fewer steps";
		case Code::TooManyOps: return "at most 24 operations a reply";
		case Code::RoundBudget: return "no more operations can be worked out for this answer";
		case Code::TaskBudget: return "no more operations can be worked out for this task";
		case Code::NotEvaluable: return "this formula is not recalculated here: use the table operations";
		case Code::AmbiguousOperand: return "columns are named by letter (B, C, D); a part of a name is not a value";
		case Code::UnknownColumn: return "use a column letter the table shows";
		case Code::BadPredicate: return "a filter atom is {column, test, value, value2} with a test that fits the column";
		case Code::TableTooLarge: return "the table is too large to work over whole: filter it first";
		case Code::CrossTable: return "rows of two tables are not combined";
		case Code::QueryLiteral: return "search by symbol, or by words the material uses";
		default: break;
	}
	return "change the operation or answer without it";
}

control::Template refusal_template(Code code) {
	switch (code) {
		case Code::ConstantNotInMaterial: return control::Template::ComputeRefusedConstantNotInMaterial;
		case Code::UnsafeLiteral: return control::Template::ComputeRefusedUnsafeLiteral;
		case Code::ConstantDepth: return control::Template::ComputeRefusedConstantDepth;
		case Code::DisclosureBudget: return control::Template::ComputeRefusedDisclosureBudget;
		case Code::SameOperand: return control::Template::ComputeRefusedSameOperand;
		case Code::ExternalOperand: return control::Template::ComputeRefusedExternalOperand;
		default: break;
	}
	return control::Template::ComputeRefusedOther;
}

// The op id and the code of a line's head, as the slots hold them; an op id
// that is not one (a local check caught it) is written as q0 and a code that
// is none as not_evaluable - never the model's own text.
std::string op_slot(const OpNode &node) {
	return control::fits(control::Slot::Kind::OpId, node.id) ? node.id : std::string("q0");
}

std::string code_slot(Code code) {
	const std::string name = to_string(code);
	return control::fits(control::Slot::Kind::Code, name) ? name : std::string("not_evaluable");
}

// " arg N" where an error or refusal concerns one argument.
std::string argument_part(std::size_t argument) {
	if (argument == 0 || argument > 999) return {};
	return control::render(control::Template::ComputeErrorHint,
		{{control::Slot::Kind::Count, std::to_string(argument)}});
}

std::string failure_line(const OpNode &node, OpStatus status, const TypeError &error, bool repeated) {
	const std::string op = op_slot(node);
	const std::string code = code_slot(error.code);
	const bool refused = status == OpStatus::Refused;
	std::string line = control::render(refused ? control::Template::ComputeLineRefused : control::Template::ComputeLineError,
		{{control::Slot::Kind::OpId, op}, {control::Slot::Kind::Code, code}});
	line += argument_part(error.argument);
	if (repeated) return line + control::render(control::Template::ComputeRepeated);
	if (refused) return line + control::render(refusal_template(error.code));
	// Types only, each as a type is written: anything else is left out.
	if (control::fits(control::Slot::Kind::TypeName, error.expected) && control::fits(control::Slot::Kind::TypeName, error.got))
		line += ": expected " + error.expected + ", got " + error.got;
	return line + " - " + hint_for(node, error) + "\n";
}

// A constant as the derivation writes it: a column letter and a lower-case
// word (an op name, a test) bare, anything else in quotes.
std::string written_constant(const std::string &constant) {
	if (all_upper_letters(constant, 3)) return constant;
	if (!constant.empty() && std::all_of(constant.begin(), constant.end(),
			[](char character) { return is_lower(character) || character == '_'; }))
		return constant;
	std::string quoted = "\"";
	for (const char character : constant) {
		if (character == '"' || character == '\\') quoted += '\\';
		if (character == '\n' || character == '\r') {
			quoted += ' ';
			continue;
		}
		quoted += character;
	}
	return quoted + "\"";
}

std::string written_arg(const std::string &written, const LabelSymbols &labels) {
	TypeError ignored;
	const std::optional<Arg> arg = parse_arg(written, ignored);
	if (!arg) return written_constant(trimmed(written));
	if (arg->kind == ArgKind::Constant) return written_constant(arg->constant);
	if (arg->kind == ArgKind::Ref) {
		const auto named = labels.find(arg->label);
		if (named != labels.end() && control::fits(control::Slot::Kind::Symbol, named->second)) return named->second;
	}
	return trimmed(written);
}

} // namespace

// ---------------------------------------------------------------------------

bool is_compute_tool(std::string_view name) {
	return name == kComputeToolWireName || name == kComputeCanonicalToolId;
}

// What the model is shown (owner, 26 Sep 2026: a compact, reliable API; tool
// accuracy falls past ~20-25 functions): the core ops, one line per family.
// Every other op of signature_table() is still read and answered - its
// signature is what an error about it shows.
const std::vector<std::vector<Signature>> &core_signature_table() {
	static const std::vector<std::vector<Signature>> lines{
		{{{"add", "subtract"}, "add|subtract(a, b, \xE2\x80\xA6): subtract is a less the rest"},
			{{"multiply", "divide"}, "multiply|divide(a, b)"},
			{{"min", "max"}, "min|max(a, b, \xE2\x80\xA6)"},
			{{"round"}, "round(a; places)"}},
		{{{"compare"}, "compare(a, b): LESS, EQUAL or GREATER"},
			{{"between"}, "between(x, lo, hi)"},
			{{"apply_rate"}, "apply_rate(amount, rate[, duration]; mode=share|increase|decrease)"},
			{{"ratio"}, "ratio(part, whole)"}},
		{{{"add_duration"}, "add_duration(date, \xC2\xB1" "duration[; calendar])"},
			{{"duration_between"}, "duration_between(from, to; unit[, calendar])"},
			{{"deadline"}, "deadline(date, duration, calendar)"},
			{{"within"}, "within(date, start, end) | within(date, anchor, \xC2\xB1" "duration)"},
			{{"end_of"}, "end_of(date; unit)"}},
		{{{"filter"}, "filter(table|rows; where=[{column, test, value, value2}])"},
			{{"sum_column"}, "sum_column(rows, column)"},
			{{"count"}, "count(rows)"},
			{{"list_rows"}, "list_rows(rows)"}},
		{{{"group"}, "group(rows, key_column[; unit])"},
			{{"aggregate"}, "aggregate(groups, column; fn)"},
			{{"derive_column"}, "derive_column(rows, op, column|value, column|value)"}},
		{{{"cell"}, "cell([[Table1]].B12)"},
			{{"recalc"}, "recalc([[Table1]].E19)"},
			{{"lookup"}, "lookup(table|schedule, key_column, value, result_column; mode=le|ge)"},
			{{"progressive"}, "progressive(value, schedule)"},
			{{"describe_tables"}, "describe_tables(DOC_n)"}},
	};
	return lines;
}

// One example of each shape the model most often got wrong live (a window
// filter, a column total, a rate applied, an excess), in the exact form
// (tool-use examples: complex-parameter accuracy 72% -> 90%, Anthropic 2025).
const char *kComputeExamples =
	"Examples (the 0-numbered symbols never occur; fields not shown are null):\n"
	"{\"id\":\"q1\",\"op\":\"filter\",\"args\":[\"[[Table0]]\"],\"where\":[{\"column\":\"B\",\"test\":\"within\","
	"\"value\":\"[[Date0]]\",\"value2\":\"-24 months\"}],\"bind\":\"in_window\",\"why\":\"rows in the lookback\"}\n"
	"{\"id\":\"q2\",\"op\":\"sum_column\",\"args\":[\"@in_window\",\"L\"],\"bind\":\"window_total\",\"why\":\"their total\"}\n"
	"{\"id\":\"q3\",\"op\":\"apply_rate\",\"args\":[\"[[Amount0]]\",\"[[Rate0]]\"],\"options\":{\"mode\":\"increase\"},"
	"\"bind\":\"stepped\",\"why\":\"rent after the step\"}\n"
	"{\"id\":\"q4\",\"op\":\"subtract\",\"args\":[\"@window_total\",\"@stepped\"],\"bind\":\"difference\",\"why\":\"the gap\"}\n";

const std::string &signature_lines() {
	static const std::string lines = [] {
		std::string text = "Signatures ([ ] optional; options after \";\"):\n";
		for (const std::vector<Signature> &line : core_signature_table()) {
			for (std::size_t index = 0; index < line.size(); ++index) {
				if (index > 0) text += " \xC2\xB7 ";
				text += line[index].line;
			}
			text += "\n";
		}
		return text;
	}();
	return lines;
}

std::string signature_of(std::string_view op) {
	for (const std::vector<Signature> &line : signature_table())
		for (const Signature &signature : line)
			for (const std::string &name : signature.ops)
				if (name == op) return signature.line;
	return {};
}

const std::vector<std::string> &op_names() {
	static const std::vector<std::string> names{"add", "subtract", "multiply", "divide", "sum", "average", "min", "max",
		"abs", "round", "positive_part", "equal", "not_equal", "greater_than", "less_than", "greater_or_equal",
		"less_or_equal", "between", "compare", "sign", "apply_rate", "ratio", "percentage_of", "percentage_change",
		"before", "after", "same_time", "duration_between", "add_duration", "deadline", "within", "outside", "overlaps",
		"start_of", "end_of", "days_in", "next_business_day", "next_occurrence", "day_class", "filter", "count", "group",
		"aggregate", "sum_column", "min_column", "max_column", "average_column", "derive_column", "union", "intersect",
		"minus", "lookup", "progressive", "cell", "list_rows", "recalc", "describe_tables", "and", "or", "not",
		"if_then_else"};
	return names;
}

const std::string &compute_instruction() {
	return compute_instruction(Carrier::FinalField);
}

const std::string &compute_instruction(Carrier carrier) {
	// Where the operations go is the one sentence the carriers differ in.
	const auto build = [](const std::string &where) {
		return "Values written [[Org1]], [[Amount3]], [[Date4]] \xE2\x80\xA6 are the lawyer's protected facts; you never "
			"see them. A symbol is one fact: two facts of equal value are two symbols. protectionMetadata gives each "
			"symbol's type and role, spreadsheet tables ([[Table1]]: columns by letter, one cell as [[Table1]].B16) and "
			"business-day rules ([[Calendar1]]). Singularity does all arithmetic, comparisons, date and table work: " + where
			+ " args are symbols, @name of an earlier result, [[Table1]].B, or a constant exactly as the material writes "
			"it; bind names the result (snake_case); why is few words; null where a field does not apply. Ops run "
			"in order. A result is a new symbol, TRUE/FALSE, "
			"LESS/EQUAL/GREATER, a sign, BOUNDED (state only the bound), UNKNOWN (never guess: say what is certain, or ask "
			"the lawyer), or an error to fix. Check a table total against the same role's figure elsewhere before relying "
			"on it. Each amount, date or row range you write (an excess over a cap, a rate applied, a "
			"deadline, rows to total) is a result you asked for; a relation is no amount. Never call open what ops can settle "
			"(rows or dates in a window, a deadline): ask them first. State findings, never narrate checks. Write figures, dates, results, people and companies only as "
			"symbols ([[Person5.first]] names a part); values are filled in. An op that fails is fixed and asked "
			"again; never answer from it. At most 24 ops a reply.\n" + signature_lines() + kComputeExamples;
	};
	static const std::string finalField = build("put operations in localFacts.ops and nothing else in that reply.");
	static const std::string tool = build("call lm_compute with {\"ops\": [...]} and nothing else in that turn.");
	static const std::string textual = build("reply {\"type\":\"tool_call\",\"tool\":\"papinian.compute\","
		"\"arguments\":{\"ops\":[...]}} and nothing else in that turn.");
	if (carrier == Carrier::FinalField) return finalField;
	return carrier == Carrier::NativeTool ? tool : textual;
}

namespace {

Json nullable(const char *type) {
	return Json{{"type", Json::array({type, "null"})}};
}

Json nullable_enum(const std::vector<std::string> &words) {
	Json values = Json::array();
	for (const std::string &word : words) values.push_back(word);
	values.push_back(nullptr);
	return Json{{"type", Json::array({"string", "null"})}, {"enum", std::move(values)}};
}

Json ops_schema(bool withSignatures) {
	Json ops = Json::array();
	for (const std::string &name : op_names()) ops.push_back(name);
	Json op{{"type", "string"}, {"enum", std::move(ops)}};
	// Carrier F's instruction travels beside it and says the rest; carrier N
	// has only its schema, so its descriptions stay.
	if (withSignatures) op["description"] = signature_lines();
	Json tests = Json::array();
	for (const std::string &test : where_tests()) tests.push_back(test);
	const Json atom{{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"column", "test", "value", "value2"})},
		{"properties", {
			{"column", withSignatures ? Json{{"type", "string"}, {"description", "column letter of the table: B"}}
				: Json{{"type", "string"}}},
			{"test", {{"type", "string"}, {"enum", std::move(tests)}}},
			{"value", nullable("string")},
			{"value2", nullable("string")}}}};
	Json calendar = nullable("string");
	Json anchor = nullable("string");
	Json bind = nullable("string");
	if (withSignatures) {
		calendar["description"] = "[[Calendar1]] or governing";
		anchor["description"] = "a DATE symbol or @name: periods counted from it";
		bind["description"] = "snake_case name for the result";
	}
	const Json options{{"type", Json::array({"object", "null"})}, {"additionalProperties", false},
		{"required", Json::array({"unit", "calendar", "places", "rounding", "fn", "mode", "reveal", "anchor"})},
		{"properties", {
			{"unit", nullable_enum(unit_words())},
			{"calendar", std::move(calendar)},
			{"places", nullable("integer")},
			{"rounding", nullable_enum(rounding_words())},
			{"fn", nullable_enum(fn_words())},
			{"mode", nullable_enum(mode_words())},
			{"reveal", nullable("boolean")},
			{"anchor", std::move(anchor)}}}};
	const Json item{{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"id", "op", "args", "where", "options", "bind", "expect", "why"})},
		{"properties", {
			{"id", withSignatures ? Json{{"type", "string"}, {"description", "q1, q2 \xE2\x80\xA6 unique in this reply"}}
				: Json{{"type", "string"}}},
			{"op", std::move(op)},
			{"args", {{"type", "array"}, {"maxItems", kMaxArguments}, {"items", {{"type", "string"}}}}},
			{"where", {{"type", Json::array({"array", "null"})}, {"maxItems", kMaxWhereAtoms}, {"items", atom}}},
			{"options", options},
			{"bind", std::move(bind)},
			{"expect", nullable_enum(expect_words())},
			{"why", withSignatures ? Json{{"type", "string"}, {"description", "the subproblem, a few words"}}
				: Json{{"type", "string"}}}}}};
	return Json{{"type", "object"}, {"additionalProperties", false}, {"required", Json::array({"ops"})},
		{"properties", {{"ops", {{"type", "array"}, {"minItems", 1}, {"maxItems", kOpsPerRound}, {"items", item}}}}}};
}

} // namespace

std::string ops_schema_json() {
	static const std::string schema = ops_schema(false).dump();
	return schema;
}

std::string ops_tool_schema_json() {
	static const std::string schema = ops_schema(true).dump();
	return schema;
}

bool valid_label(std::string_view label) {
	return control::fits(control::Slot::Kind::Label, label);
}

std::optional<Arg> parse_arg(std::string_view written, TypeError &error) {
	const std::string text = trimmed(written);
	Arg arg;
	arg.written = std::string(written);
	if (text.empty()) {
		error = error_of(Code::BadJson, 0, "an argument is empty");
		return std::nullopt;
	}
	// @name: the bind of an earlier op of this task.
	if (text.front() == '@') {
		const std::string label = text.substr(1);
		if (!valid_label(label)) {
			error = error_of(Code::UnknownLabel);
			return std::nullopt;
		}
		arg.kind = ArgKind::Ref;
		arg.label = label;
		return arg;
	}
	if (text == "governing" || text == "calendar_days") {
		arg.kind = ArgKind::Calendar;
		arg.symbol = text;
		return arg;
	}
	if (text.rfind("DOC_", 0) == 0 && all_digits(std::string_view(text).substr(4), 9)) {
		arg.kind = ArgKind::Document;
		arg.symbol = text;
		return arg;
	}
	// A table's handle in either spelling, then what may follow it.
	std::string table;
	std::size_t after = 0;
	const SymbolShape symbol = symbol_at(text);
	if (symbol.length > 0 && symbol.prefix == "Table" && symbol.part.empty()) {
		table = "Table" + symbol.digits;
		after = symbol.length;
	} else if (text.rfind("Table", 0) == 0) {
		std::size_t cursor = 5;
		while (cursor < text.size() && is_digit(text[cursor]) && cursor - 5 < 6) ++cursor;
		if (cursor > 5 && (cursor == text.size() || text[cursor] == '.')) {
			table = text.substr(0, cursor);
			after = cursor;
		}
	}
	if (!table.empty()) {
		arg.symbol = table;
		if (after == text.size()) {
			arg.kind = ArgKind::Table;
			return arg;
		}
		const std::string rest = text.substr(after + 1);
		// [[Table1]].r7: a row.
		if (rest.size() > 1 && rest.front() == 'r' && all_digits(std::string_view(rest).substr(1), 7)) {
			arg.kind = ArgKind::Row;
			arg.row = static_cast<std::size_t>(std::stoul(rest.substr(1)));
			return arg;
		}
		std::size_t letters = 0;
		while (letters < rest.size() && is_upper(rest[letters])) ++letters;
		if (letters >= 1 && letters <= 3 && letters == rest.size()) {
			arg.kind = ArgKind::Column;
			arg.column = rest;
			return arg;
		}
		if (letters >= 1 && letters <= 3 && all_digits(std::string_view(rest).substr(letters), 7)) {
			arg.kind = ArgKind::CellRef;
			arg.column = rest.substr(0, letters);
			arg.row = static_cast<std::size_t>(std::stoul(rest.substr(letters)));
			return arg;
		}
		// A column by header name: names repeat and translate; letters do not.
		error = error_of(Code::AmbiguousOperand, 0, "columns are named by letter: B, C, D");
		return std::nullopt;
	}
	if (symbol.length > 0) {
		if (symbol.length != text.size()) {
			error = error_of(Code::UnknownSymbol);
			return std::nullopt;
		}
		const std::string handle = "[[" + symbol.prefix + symbol.digits + "]]";
		if (symbol.prefix == "Calendar" && symbol.part.empty()) {
			arg.kind = ArgKind::Calendar;
			arg.symbol = handle;
			return arg;
		}
		if (symbol.prefix == "Schedule" && symbol.part.empty()) {
			arg.kind = ArgKind::Schedule;
			arg.symbol = handle;
			return arg;
		}
		arg.kind = ArgKind::Symbol;
		arg.symbol = handle;
		arg.part = symbol.part;
		return arg;
	}
	// Something that opens as a symbol but is none is never read as a figure.
	if (text.rfind("[[", 0) == 0 || text.find("]]") != std::string::npos) {
		error = error_of(Code::UnknownSymbol);
		return std::nullopt;
	}
	arg.kind = ArgKind::Constant;
	arg.constant = text;
	return arg;
}

ParsedOps parse_ops(const std::string &ops_json) {
	ParsedOps parsed;
	Json value = Json::parse(ops_json, nullptr, false);
	// The object may come wrapped as the final field carries it.
	if (value.is_object() && !value.contains("ops") && value.contains("localFacts") && value.at("localFacts").is_object())
		value = value.at("localFacts");
	if (!value.is_object() || !value.contains("ops") || !value.at("ops").is_array() || value.at("ops").empty()) {
		parsed.code = Code::BadJson;
		return parsed;
	}
	std::set<std::string> ids;
	std::set<std::string> binds;
	// Every id the reply writes, so one Singularity gives an op never takes another's.
	std::set<std::string> written;
	for (const Json &entry : value.at("ops"))
		if (entry.is_object() && entry.contains("id") && entry.at("id").is_string()) written.insert(trimmed(entry.at("id").get<std::string>()));
	std::size_t index = 0;
	for (const Json &entry : value.at("ops")) {
		++index;
		OpNode node;
		std::optional<TypeError> check;
		const auto fail = [&](TypeError error) {
			if (!check) check = std::move(error);
		};
		if (!entry.is_object()) {
			node.id = "q" + std::to_string(index);
			fail(error_of(Code::BadJson));
		} else {
			// id: q1 ... q999, unique in the reply.
			const std::optional<std::string> id = present(entry, "id") ? text_of(entry.at("id")) : std::nullopt;
			node.id = id ? trimmed(*id) : std::string();
			// An id only ties an answer to its op: "Q1" and a bare "1" are q1, and
			// one no line could carry ("op1", missing) is the op's place in the
			// reply - never a reason to refuse the round.
			if (!node.id.empty() && node.id.front() == 'Q') node.id.front() = 'q';
			if (!node.id.empty() && all_digits(node.id, 3)) node.id = "q" + node.id;
			const auto shaped = [](const std::string &candidate) {
				return candidate.size() >= 2 && candidate.front() == 'q' && all_digits(std::string_view(candidate).substr(1), 3);
			};
			if (!shaped(node.id) || ids.count(node.id) != 0) {
				std::size_t place = index;
				while ((ids.count("q" + std::to_string(place)) != 0 || written.count("q" + std::to_string(place)) != 0)
						&& place < 999)
					++place;
				node.id = "q" + std::to_string(place);
			}
			if (!shaped(node.id)) fail(error_of(Code::BadId));
			else ids.insert(node.id);
			// op: one of the enum.
			const std::optional<std::string> op = present(entry, "op") ? text_of(entry.at("op")) : std::nullopt;
			node.op = op ? trimmed(*op) : std::string();
			if (!one_of(op_names(), node.op)) fail(error_of(Code::UnknownOp));
			// args: strings, at most 64.
			if (present(entry, "args")) {
				if (!entry.at("args").is_array()) {
					fail(error_of(Code::BadJson, 0, "args is a list of strings"));
				} else {
					for (const Json &argument : entry.at("args")) {
						const std::optional<std::string> written_argument = text_of(argument);
						if (!written_argument) {
							fail(error_of(Code::BadJson, node.args.size() + 1, "an argument is a string"));
							continue;
						}
						node.args.push_back(*written_argument);
					}
					if (node.args.size() > kMaxArguments) fail(error_of(Code::Arity));
				}
			}
			// where: at most 8 atoms of a closed test.
			if (present(entry, "where")) {
				if (!entry.at("where").is_array()) {
					fail(error_of(Code::BadPredicate, 0, "where is a list of {column, test, value, value2}"));
				} else {
					std::vector<WhereAtom> atoms;
					std::size_t atomIndex = 0;
					for (const Json &atom : entry.at("where")) {
						++atomIndex;
						WhereAtom read;
						const std::optional<std::string> column = atom.is_object() && present(atom, "column")
							? text_of(atom.at("column")) : std::nullopt;
						const std::optional<std::string> test = atom.is_object() && present(atom, "test")
							? text_of(atom.at("test")) : std::nullopt;
						if (!column || !test || !one_of(where_tests(), trimmed(*test))) {
							fail(error_of(Code::BadPredicate, atomIndex));
							continue;
						}
						read.column = trimmed(*column);
						read.test = trimmed(*test);
						if (present(atom, "value")) read.value = text_of(atom.at("value"));
						if (present(atom, "value2")) read.value2 = text_of(atom.at("value2"));
						atoms.push_back(std::move(read));
					}
					if (atomIndex > kMaxWhereAtoms) fail(error_of(Code::BadPredicate, 0, "at most 8 atoms"));
					node.where = std::move(atoms);
				}
			}
			// options: closed words; places 0-9.
			if (present(entry, "options")) {
				const Json &options = entry.at("options");
				if (!options.is_object()) {
					fail(error_of(Code::BadJson, 0, "options is an object or null"));
				} else {
					OpOptions read;
					const auto word = [&](const char *key, const std::vector<std::string> *allowed,
							std::optional<std::string> &into) {
						if (!present(options, key)) return;
						const std::optional<std::string> text = text_of(options.at(key));
						if (!text || (allowed != nullptr && !one_of(*allowed, trimmed(*text)))) {
							fail(error_of(Code::BadJson, 0, std::string("options.") + key + " is not one of its words"));
							return;
						}
						into = trimmed(*text);
					};
					word("unit", &unit_words(), read.unit);
					word("calendar", nullptr, read.calendar);
					word("rounding", &rounding_words(), read.rounding);
					word("fn", &fn_words(), read.fn);
					word("mode", &mode_words(), read.mode);
					word("anchor", nullptr, read.anchor);
					if (present(options, "places")) {
						const Json &places = options.at("places");
						if (!places.is_number_integer() || places.get<long long>() < 0
								|| places.get<long long>() > kMaxPlaces)
							fail(error_of(Code::BadJson, 0, "options.places is 0 to 9"));
						else read.places = places.get<int>();
					}
					if (present(options, "reveal")) {
						if (!options.at("reveal").is_boolean()) fail(error_of(Code::BadJson, 0, "options.reveal is true or false"));
						else read.reveal = options.at("reveal").get<bool>();
					}
					node.options = std::move(read);
				}
			}
			// bind: the §2.1 label rule, unique in the reply (the task's own
			// earlier names are the executor's to know).
			if (present(entry, "bind")) {
				const std::optional<std::string> bind = text_of(entry.at("bind"));
				const std::string label = bind ? trimmed(*bind) : std::string();
				node.bind = label;
				if (!valid_label(label)) fail(error_of(Code::BadLabel));
				else if (!binds.insert(label).second) fail(error_of(Code::BindReused));
			}
			// expect: one of its words.
			if (present(entry, "expect")) {
				const std::optional<std::string> expect = text_of(entry.at("expect"));
				if (!expect || !one_of(expect_words(), trimmed(*expect)))
					fail(error_of(Code::BadJson, 0, "expect is one of the types the schema lists"));
				else node.expect = trimmed(*expect);
			}
			// why: a few words.
			if (present(entry, "why")) {
				const std::optional<std::string> why = text_of(entry.at("why"));
				node.why = why ? trimmed(*why) : std::string();
				if (node.why.size() > kMaxWhyBytes) fail(error_of(Code::BadJson, 0, "why is a few words, at most 120 bytes"));
			}
			// Each argument must read by the grammar.
			if (!check)
				for (std::size_t argument = 0; argument < node.args.size(); ++argument) {
					TypeError error;
					if (parse_arg(node.args[argument], error)) continue;
					error.argument = argument + 1;
					fail(std::move(error));
					break;
				}
		}
		if (index > kOpsPerRound) fail(error_of(Code::TooManyOps));
		if (check) parsed.errors.emplace_back(node.id, *check);
		parsed.checks.push_back(std::move(check));
		parsed.ops.push_back(std::move(node));
	}
	return parsed;
}

std::string request_canonical(const OpNode &node) {
	std::string text = node.op + "(";
	for (std::size_t index = 0; index < node.args.size(); ++index) {
		if (index > 0) text += ",";
		text += trimmed(node.args[index]);
	}
	if (node.where && !node.where->empty()) {
		text += ";where:";
		for (std::size_t index = 0; index < node.where->size(); ++index) {
			const WhereAtom &atom = (*node.where)[index];
			if (index > 0) text += "&";
			text += atom.column + " " + atom.test;
			if (atom.value) text += " " + trimmed(*atom.value);
			if (atom.value2) text += " " + trimmed(*atom.value2);
		}
	}
	if (node.options) {
		std::string options;
		const auto add = [&](const char *key, const std::optional<std::string> &value) {
			if (!value) return;
			options += (options.empty() ? "" : ",") + std::string(key) + "=" + *value;
		};
		add("unit", node.options->unit);
		add("calendar", node.options->calendar);
		if (node.options->places) add("places", std::to_string(*node.options->places));
		add("rounding", node.options->rounding);
		add("fn", node.options->fn);
		add("mode", node.options->mode);
		if (node.options->reveal) add("reveal", *node.options->reveal ? std::string("true") : std::string("false"));
		add("anchor", node.options->anchor);
		if (!options.empty()) text += ";" + options;
	}
	return text + ")";
}

std::optional<std::string> ops_in_final_text(const std::string &final_text, bool strict) {
	Json value = Json::parse(final_text, nullptr, false);
	if (!value.is_object()) {
		// Restored text keeps localFacts in the symbol plane only where the
		// object is the whole text: anything else was restored as prose.
		if (strict) return std::nullopt;
		// A final with a word or a fence around its object.
		const std::size_t open = final_text.find('{');
		const std::size_t close = final_text.rfind('}');
		if (open == std::string::npos || close == std::string::npos || close <= open) return std::nullopt;
		value = Json::parse(final_text.substr(open, close - open + 1), nullptr, false);
		if (!value.is_object()) return std::nullopt;
	}
	// The textual protocol's final carries its answer as one string.
	if (value.contains("type") && value.at("type").is_string() && value.at("type").get<std::string>() == "final"
			&& value.contains("text") && value.at("text").is_string())
		return ops_in_final_text(value.at("text").get<std::string>(), strict);
	if (!value.contains("localFacts") || !value.at("localFacts").is_object()) return std::nullopt;
	const Json &facts = value.at("localFacts");
	if (!facts.contains("ops") || !facts.at("ops").is_array() || facts.at("ops").empty()) return std::nullopt;
	return Json{{"ops", facts.at("ops")}}.dump();
}

std::string derivation_of(const OpNode &node, const LabelSymbols &labels) {
	std::vector<std::string> parts;
	for (const std::string &argument : node.args) parts.push_back(written_arg(argument, labels));
	if (node.where)
		for (const WhereAtom &atom : *node.where) {
			std::string text = (atom.column.rfind("@", 0) == 0 ? written_arg(atom.column, labels) : atom.column) + " " + atom.test;
			if (atom.value) text += " " + written_arg(*atom.value, labels);
			if (atom.value2) text += ".." + written_arg(*atom.value2, labels);
			parts.push_back(std::move(text));
		}
	if (node.options) {
		const OpOptions &options = *node.options;
		if (options.unit) parts.push_back(*options.unit);
		if (options.calendar) parts.push_back(written_arg(*options.calendar, labels));
		if (options.places) parts.push_back(written_constant(std::to_string(*options.places)));
		if (options.rounding) parts.push_back(*options.rounding);
		if (options.fn) parts.push_back(*options.fn);
		if (options.mode) parts.push_back(*options.mode);
		if (options.anchor) parts.push_back(written_arg(*options.anchor, labels));
		if (options.reveal && *options.reveal == false) parts.push_back("reveal=false");
	}
	std::string text = node.op + "(";
	for (std::size_t index = 0; index < parts.size(); ++index) {
		if (index > 0) text += ", ";
		text += parts[index];
	}
	return text + ")";
}

bool has_word(const std::string &words, const std::string &word) {
	std::istringstream stream(words);
	std::string one;
	while (stream >> one)
		if (one == word) return true;
	return false;
}

// A one-sided day bound whose only reason is the holidays Singularity does not
// keep (a weekend end and the side left open aside): no partial date, month
// length, open roll or other calendar's reading in it.
bool holiday_only(const std::string &words) {
	std::istringstream stream(words);
	std::string one;
	bool holidays = false;
	while (stream >> one) {
		if (one == kWordHolidaysNotKnown) holidays = true;
		else if (one != kWordNoLatestDay && one != kWordNoEarliestDay && one != kWordEndsOnSaturday && one != kWordEndsOnSunday)
			return false;
	}
	return holidays;
}

std::string result_line(const OpNode &node, const OpAnswer &answer, bool repeated, const LabelSymbols &labels) {
	try {
		const std::string op = op_slot(node);
		switch (answer.status) {
			case OpStatus::Ok: {
				if (!answer.relation.empty() || answer.results.empty()) {
					// A relation word (TRUE, FALSE, LESS, EQUAL, GREATER, a sign) -
					// or nothing new to name at all (a table described, rows
					// listed): then what the executor wrote follows on its own line.
					std::string line = control::render(control::Template::ComputeLineRelation,
						{{control::Slot::Kind::OpId, op}});
					if (!answer.relation.empty()) line += answer.relation + " ";
					line += derivation_of(node, labels);
					if (!answer.relation.empty() && answer.words == "as_written")
						line += control::render(control::Template::ComputeEqualAsWritten);
					if (answer.relation.empty() && !answer.words.empty()) line += "\n  " + answer.words;
					return line + "\n";
				}
				const std::string &symbol = answer.results.front();
				if (!control::fits(control::Slot::Kind::Symbol, symbol))
					return failure_line(node, OpStatus::Error, error_of(Code::NotEvaluable), false);
				std::string line = control::render(control::Template::ComputeLineOk,
					{{control::Slot::Kind::OpId, op}, {control::Slot::Kind::Symbol, symbol},
					 {control::Slot::Kind::TypeName, safe_type(answer.result.type)},
					 {control::Slot::Kind::Label, label_for(node)}}) + derivation_of(node, labels);
				if (!answer.sign.empty()) line += " \xC2\xB7 " + answer.sign;
				// An op with a result per group (aggregate): each further result
				// as the executor wrote it - its key and symbol - on a line of its own.
				if (answer.results.size() > 1 && !answer.words.empty()) line += "\n  " + answer.words;
				return line + "\n";
			}
			case OpStatus::Bounded: {
				// One end known: that end, and that the other is not.
				if (answer.results.size() == 1 && control::fits(control::Slot::Kind::Symbol, answer.results[0])) {
					const bool date = answer.result.type.kind == TypeKind::Date;
					const bool from = answer.lo.has_value();
					std::string line = control::render(from ? control::Template::ComputeLineBoundedFrom
							: control::Template::ComputeLineBoundedTo,
						{{control::Slot::Kind::OpId, op}, {control::Slot::Kind::Symbol, answer.results[0]},
						 {control::Slot::Kind::TypeName, safe_type(answer.result.type)},
						 {control::Slot::Kind::Label, label_for(node)}}) + derivation_of(node, labels);
					// Only a holiday could move the day: the day, on that condition.
					if (date && holiday_only(answer.words)) {
						line += control::render(from ? control::Template::ComputeHolidayFromDate : control::Template::ComputeHolidayToDate);
						for (const char *weekend : {kWordEndsOnSaturday, kWordEndsOnSunday})
							if (has_word(answer.words, weekend)) line += std::string("; ") + weekend;
						return line + "\n";
					}
					line += control::render(from ? (date ? control::Template::ComputeBoundedFromDate : control::Template::ComputeBoundedFromValue)
						: (date ? control::Template::ComputeBoundedToDate : control::Template::ComputeBoundedToValue));
					if (!answer.words.empty()) line += "; " + answer.words;
					if (date) line += control::render(control::Template::ComputeBoundedAsk);
					return line + "\n";
				}
				if (answer.results.size() < 2 || !control::fits(control::Slot::Kind::Symbol, answer.results[0])
						|| !control::fits(control::Slot::Kind::Symbol, answer.results[1]))
					return failure_line(node, OpStatus::Error, error_of(Code::NotEvaluable), false);
				const bool date = answer.result.type.kind == TypeKind::Date;
				std::string line = control::render(control::Template::ComputeLineBounded,
					{{control::Slot::Kind::OpId, op}, {control::Slot::Kind::Symbol, answer.results[0]},
					 {control::Slot::Kind::Symbol, answer.results[1]},
					 {control::Slot::Kind::TypeName, safe_type(answer.result.type)},
					 {control::Slot::Kind::Label, label_for(node)}}) + derivation_of(node, labels)
					+ control::render(date ? control::Template::ComputeBoundedDate : control::Template::ComputeBoundedValue);
				if (!answer.words.empty()) line += "; " + answer.words;
				if (date) line += control::render(control::Template::ComputeBoundedAsk);
				return line + "\n";
			}
			case OpStatus::Unknown: {
				const Code reason = answer.code != Code::None ? answer.code : answer.certainty.reason;
				return control::render(control::Template::ComputeLineUnknown,
						{{control::Slot::Kind::OpId, op}, {control::Slot::Kind::Code, code_slot(reason)}})
					+ derivation_of(node, labels) + control::render(control::Template::ComputeUnknownReason)
					+ (answer.words.empty() ? std::string() : "; " + answer.words) + "\n";
			}
			case OpStatus::Error:
			case OpStatus::Refused:
			case OpStatus::NotServed: {
				TypeError error = answer.error;
				if (answer.code != Code::None) error.code = answer.code;
				return failure_line(node, answer.status == OpStatus::Refused ? OpStatus::Refused : OpStatus::Error, error,
					repeated);
			}
		}
	} catch (const std::exception &) {
	}
	// A slot no template can hold: the op is answered as not worked out, never
	// with the text that did not fit.
	return control::render(control::Template::ComputeLineError,
			{{control::Slot::Kind::OpId, op_slot(node)}, {control::Slot::Kind::Code, "not_evaluable"}})
		+ " - " + hint_for(node, error_of(Code::NotEvaluable)) + "\n";
}

std::string refusal_line(const OpNode &node, const TypeError &error) {
	try {
		return failure_line(node, OpStatus::Error, error, false);
	} catch (const std::exception &) {
		return control::render(control::Template::ComputeLineError,
				{{control::Slot::Kind::OpId, op_slot(node)}, {control::Slot::Kind::Code, "bad_json"}})
			+ " - " + hint_for(node, error_of(Code::BadJson)) + "\n";
	}
}

namespace {

// The block with the rounds that remain given as they are.
std::string results_block_after(const std::vector<OpRecord> &records, std::size_t round, std::size_t rounds,
		std::size_t remaining, std::size_t ops_left) {
	const auto count = [](std::size_t value) {
		return control::Slot{control::Slot::Kind::Count, std::to_string(std::min<std::size_t>(value, 999))};
	};
	std::string block = control::render(control::Template::ComputeHeader, {count(round), count(rounds), count(ops_left)});
	// A run of ops that failed only because one they use failed is one line
	// (live c9-c13: 67 of 362 lines), so the failure to fix stands out.
	const auto id_of = [](const OpRecord &record) {
		const std::size_t space = record.result_line.find(' ');
		return space == std::string::npos ? std::string() : record.result_line.substr(0, space);
	};
	for (std::size_t index = 0; index < records.size();) {
		std::size_t end = index;
		while (end < records.size() && records[end].code == "dependency_failed") ++end;
		if (end - index >= 2) {
			block += id_of(records[index]) + "\xE2\x80\x93" + id_of(records[end - 1])
				+ " error dependency_failed - each uses a result that failed: fix that op, then send them again\n";
			index = end;
			continue;
		}
		block += records[index].result_line;
		++index;
	}
	if (remaining > 1) block += control::render(control::Template::ComputeContinueRounds, {count(remaining)});
	else if (remaining == 1) block += control::render(control::Template::ComputeContinueRound, {count(remaining)});
	else block += control::render(control::Template::ComputeContinueLast);
	return block;
}

} // namespace

std::string results_block(const std::vector<OpRecord> &records, std::size_t round, std::size_t rounds,
		std::size_t ops_left, bool local) {
	(void)local;
	return results_block_after(records, round, rounds, rounds > round ? rounds - round : 0, ops_left);
}

std::string closed_text() {
	return control::render(control::Template::ComputeClosed);
}

std::string reopened_text() {
	return control::render(control::Template::ComputeReopened);
}

std::string labels_lost_text(const std::vector<std::string> &labels) {
	std::string text;
	for (const std::string &label : labels)
		if (valid_label(label))
			text += control::render(control::Template::ComputeLabelsLost, {{control::Slot::Kind::Label, label}});
	return text;
}

OpRecord record_of(const OpNode &node, const OpAnswer &answer, const std::string &line,
		std::size_t call, std::size_t round, std::size_t model_turn, Carrier carrier) {
	OpRecord record;
	record.op_id = "c" + std::to_string(call) + ".r" + std::to_string(round) + "." + node.id;
	record.call = call;
	record.round = round;
	record.model_turn = model_turn;
	record.carrier = to_string(carrier);
	record.request_canonical = request_canonical(node);
	for (const std::string &argument : node.args) {
		TypeError ignored;
		const std::optional<Arg> arg = parse_arg(argument, ignored);
		if (arg && (arg->kind == ArgKind::Symbol || arg->kind == ArgKind::Ref)) record.operand_symbols.push_back(trimmed(argument));
	}
	record.expected_type = node.expect.value_or(std::string());
	record.declared_label = node.bind.value_or(std::string());
	record.justification = node.why;
	record.status = to_string(answer.status);
	record.code = to_string(answer.code != Code::None ? answer.code : answer.error.code);
	if (answer.status == OpStatus::Ok || answer.status == OpStatus::Bounded) record.code.clear();
	if (answer.status == OpStatus::Unknown && record.code.empty()) record.code = to_string(answer.certainty.reason);
	record.results = answer.results;
	record.result_line = line;
	record.relation = answer.relation;
	record.sign = answer.sign;
	// The answer is a relation the model learns: a word, a sign, a bound.
	record.observation = !answer.relation.empty() || !answer.sign.empty() || answer.status == OpStatus::Bounded;
	return record;
}

namespace {

// A section's content by PromptAssembler's markers, and the kind it is.
PayloadSegment::Kind section_kind(const std::string &name) {
	if (name == "SOURCE_MATERIAL" || name == "PROJECT_CONTEXT") return PayloadSegment::Kind::Source;
	if (name == "CURRENT_REQUEST" || name == "USER_DEFAULT_INSTRUCTIONS" || name == "MATTER_INSTRUCTIONS")
		return PayloadSegment::Kind::Request;
	if (name == "HISTORY") return PayloadSegment::Kind::ModelAuthored;
	if (name == "CONVERSATION_STATE" || name == "CANDIDATE_CONTEXT") return PayloadSegment::Kind::LmBlock;
	return PayloadSegment::Kind::Control;
}

void add_segment(std::vector<PayloadSegment> &segments, PayloadSegment::Kind kind, std::size_t from, std::size_t to,
		std::string passage = {}) {
	if (to <= from) return;
	PayloadSegment segment;
	segment.kind = kind;
	segment.start = from;
	segment.length = to - from;
	segment.passage_sha256 = std::move(passage);
	segments.push_back(std::move(segment));
}

// Whether `text` at `at` starts with `word`.
bool starts_at(const std::string &text, std::size_t at, std::string_view word) {
	return text.compare(at, word.size(), word) == 0;
}

// "12. " at `at`: a question's number, as LawyerQuestion's answer block writes it.
bool numbered_at(const std::string &text, std::size_t at) {
	std::size_t cursor = at;
	while (cursor < text.size() && cursor - at < 3 && is_digit(text[cursor])) ++cursor;
	return cursor > at && starts_at(text, cursor, ". ");
}

// A typed range of the appended text.
struct Typed {
	std::size_t from = 0;
	std::size_t to = 0;
	PayloadSegment::Kind kind = PayloadSegment::Kind::LmBlock;
};

// The lines of a block, each typed by `kind_of(line start, line end)`.
template <typename KindOf>
void typed_lines(const std::string &payload, std::size_t from, std::size_t to, KindOf kind_of, std::vector<Typed> &typed) {
	for (std::size_t at = from; at < to;) {
		std::size_t end = payload.find('\n', at);
		end = end == std::string::npos || end >= to ? to : end + 1;
		kind_of(at, end, typed);
		at = end;
	}
}

// The appended part of a payload (after its last section), line by line
// where it is a block of Singularity's (DESIGN4 §8.2): a lawyer's answer grounds
// only what the lawyer gave - the option they chose and their own words - never
// the numbered prompts (the model's question, echoed); Project evidence
// is the lawyer's material, except the echo of the model's own request
// ("You asked ...") and Singularity's lines about it; a verbatim span anywhere is
// the model's own bytes; Singularity's templates are control; everything else is
// Singularity's own block. Only source, request and lawyer-answer segments ground
// a constant.
void continuation_segments(const std::string &payload, std::size_t from, std::vector<PayloadSegment> &segments) {
	const std::string answerHeader = control::render(control::Template::LawyerAnswerHeader);
	const std::string evidenceHeader = control::render(control::Template::ProjectEvidenceHeader);
	std::vector<Typed> typed;
	const auto block_end = [&](std::size_t start) {
		std::size_t end = payload.find("\nSINGULARITY", start);
		if (end == std::string::npos) end = payload.find("SINGULARITY:", start);
		return end == std::string::npos ? payload.size() : end + (payload[end] == '\n' ? 1 : 0);
	};
	for (std::size_t at = payload.find(answerHeader, from); at != std::string::npos;
			at = payload.find(answerHeader, at + answerHeader.size())) {
		const std::size_t start = at + answerHeader.size();
		bool inTheirWords = false;
		typed_lines(payload, start, block_end(start), [&](std::size_t line, std::size_t end, std::vector<Typed> &out) {
			static constexpr std::string_view kWords = "   In their words: ";
			static constexpr std::string_view kChosen = "   Chosen: ";
			if (numbered_at(payload, line)) {
				inTheirWords = false;
				out.push_back({line, end, PayloadSegment::Kind::ModelAuthored});
			} else if (starts_at(payload, line, kChosen)) {
				// What the lawyer chose is the lawyer's answer: the option they
				// picked, never the ones they did not (those stand only in the
				// question, which is the model's).
				inTheirWords = false;
				out.push_back({line, line + kChosen.size(), PayloadSegment::Kind::LmBlock});
				out.push_back({line + kChosen.size(), end, PayloadSegment::Kind::LawyerAnswer});
			} else if (starts_at(payload, line, kWords)) {
				inTheirWords = true;
				out.push_back({line, line + kWords.size(), PayloadSegment::Kind::LmBlock});
				out.push_back({line + kWords.size(), end, PayloadSegment::Kind::LawyerAnswer});
			} else if (starts_at(payload, line, "   Left to you")) {
				inTheirWords = false;
				out.push_back({line, end, PayloadSegment::Kind::LmBlock});
			} else {
				out.push_back({line, end, inTheirWords ? PayloadSegment::Kind::LawyerAnswer : PayloadSegment::Kind::LmBlock});
			}
		}, typed);
	}
	for (std::size_t at = payload.find(evidenceHeader, from); at != std::string::npos;
			at = payload.find(evidenceHeader, at + evidenceHeader.size())) {
		const std::size_t start = at + evidenceHeader.size();
		typed_lines(payload, start, block_end(start), [&](std::size_t line, std::size_t end, std::vector<Typed> &out) {
			if (starts_at(payload, line, "You asked "))
				out.push_back({line, end, PayloadSegment::Kind::ModelAuthored});
			else if (starts_at(payload, line, "This Project search was not run")
					|| starts_at(payload, line, "This Project request could not be answered")
					|| starts_at(payload, line, "Nothing more of the Project fits"))
				out.push_back({line, end, PayloadSegment::Kind::LmBlock});
			else
				out.push_back({line, end, PayloadSegment::Kind::Source});
		}, typed);
	}
	std::sort(typed.begin(), typed.end(), [](const Typed &left, const Typed &right) { return left.from < right.from; });
	// The model's own bytes, wherever they stand, and Singularity's templates.
	std::vector<VerbatimSpan> verbatim;
	for (VerbatimSpan span : verbatim_spans(std::string_view(payload).substr(from))) {
		span.from += from;
		span.to += from;
		verbatim.push_back(span);
	}
	const std::vector<control::ControlSpan> spans = control::recognise(std::string_view(payload).substr(from));
	// [a, b) as `kind`, with every verbatim span in it the model's own (but in
	// a lawyer's answer) and,
	// where Singularity's own block stands, every template control text.
	const auto emit = [&](PayloadSegment::Kind kind, std::size_t a, std::size_t b) {
		std::size_t at = a;
		const auto emit_plain = [&](std::size_t to) {
			if (kind != PayloadSegment::Kind::LmBlock) {
				add_segment(segments, kind, at, to);
				at = std::max(at, to);
				return;
			}
			for (const control::ControlSpan &span : spans) {
				const std::size_t spanFrom = from + span.from;
				const std::size_t spanTo = from + span.to;
				if (spanTo <= at || spanFrom >= to) continue;
				add_segment(segments, PayloadSegment::Kind::LmBlock, at, std::max(at, spanFrom));
				add_segment(segments, PayloadSegment::Kind::Control, std::max(at, spanFrom), std::min(to, spanTo));
				at = std::min(to, spanTo);
			}
			add_segment(segments, PayloadSegment::Kind::LmBlock, at, to);
			at = std::max(at, to);
		};
		// An option the lawyer chose is the model's bytes the lawyer made their
		// answer: it stays the lawyer's.
		if (kind != PayloadSegment::Kind::LawyerAnswer)
		for (const VerbatimSpan &span : verbatim) {
			if (span.to <= at || span.from >= b) continue;
			emit_plain(std::max(at, span.from));
			add_segment(segments, PayloadSegment::Kind::ModelAuthored, std::max(at, span.from), std::min(b, span.to));
			at = std::min(b, span.to);
		}
		emit_plain(b);
	};
	std::size_t cursor = from;
	for (const Typed &range : typed) {
		if (range.from < cursor) continue;
		emit(PayloadSegment::Kind::LmBlock, cursor, range.from);
		emit(range.kind, range.from, range.to);
		cursor = range.to;
	}
	emit(PayloadSegment::Kind::LmBlock, cursor, payload.size());
}

// Whether a failure is a function of the request as parsed and its declared
// type alone - the same request fails the same way whatever else the round or
// the task holds - so a repeat of it is answered from the op log. Not: the
// local checks of the op's id, name or JSON (a corrected op keeps its
// canonical form), a name taken or unknown, a failed dependency, and whatever
// reads the payload (a constant's grounding, a query, the governing rule) or
// the table's state (a symbol issued since).
bool answerable_from_log(Code code) {
	switch (code) {
		case Code::UnknownOp:
		case Code::Arity:
		case Code::TypeMismatch:
		case Code::CurrencyMismatch:
		case Code::BasisMismatch:
		case Code::PerPeriod:
		case Code::UnitRequired:
		case Code::CalendarRequired:
		case Code::UnknownCalendar:
		case Code::UnsafeLiteral:
		case Code::ConstantOnly:
		case Code::ConstantDepth:
		case Code::TooManyConstants:
		case Code::SameOperand:
		case Code::GroupKeyType:
		case Code::DepthExceeded:
		case Code::AmbiguousOperand:
		case Code::UnknownColumn:
		case Code::BadPredicate:
		case Code::TableTooLarge:
		case Code::CrossTable:
		case Code::ExternalOperand:
			return true;
		default:
			return false;
	}
}

} // namespace

std::vector<PayloadSegment> payload_segments(const std::string &payload) {
	std::vector<PayloadSegment> segments;
	std::size_t cursor = 0;
	for (;;) {
		const std::size_t open = payload.find("<<<LM_SECTION:", cursor);
		if (open == std::string::npos) break;
		const std::size_t nameEnd = payload.find(">>>\n", open);
		if (nameEnd == std::string::npos) break;
		const std::string name = payload.substr(open + 14, nameEnd - open - 14);
		const std::size_t contentFrom = nameEnd + 4;
		const std::string closing = "\n<<<LM_END_SECTION:" + name + ">>>";
		const std::size_t close = payload.find(closing, contentFrom);
		if (close == std::string::npos) break;
		// What stands between sections is Singularity's own framing.
		add_segment(segments, PayloadSegment::Kind::Control, cursor, contentFrom);
		const PayloadSegment::Kind kind = section_kind(name);
		if (name == "SOURCE_MATERIAL") {
			// One segment per passage, each with the digest of its text as sent.
			std::size_t at = payload.find("[SOURCE:", contentFrom);
			add_segment(segments, PayloadSegment::Kind::Control, contentFrom,
				at == std::string::npos || at > close ? close : at);
			while (at != std::string::npos && at < close) {
				std::size_t next = payload.find("[SOURCE:", at + 8);
				if (next == std::string::npos || next > close) next = close;
				add_segment(segments, kind, at, next, platform::sha256_hex_digest(payload.substr(at, next - at)));
				at = next < close ? next : std::string::npos;
			}
		} else {
			add_segment(segments, kind, contentFrom, close);
		}
		cursor = close + closing.size();
	}
	// A payload with no section at all (a typed turn, a direct prompt) is read
	// as nobody's material: nothing in it grounds a constant.
	if (cursor == 0) {
		add_segment(segments, PayloadSegment::Kind::LmBlock, 0, payload.size());
		return segments;
	}
	continuation_segments(payload, cursor, segments);
	return segments;
}

std::vector<VerbatimSpan> verbatim_spans(std::string_view text) {
	std::vector<VerbatimSpan> spans;
	for (std::size_t open = text.find(kVerbatimOpen); open != std::string_view::npos;) {
		const std::size_t bytes = open + kVerbatimOpen.size();
		const std::size_t close = text.find(kVerbatimClose, bytes);
		if (close == std::string_view::npos) break;
		// An opening mark inside the bytes is theirs: the span still ends at
		// the first closing mark.
		spans.push_back({open, close + kVerbatimClose.size(), bytes, close});
		open = text.find(kVerbatimOpen, close + kVerbatimClose.size());
	}
	return spans;
}

namespace {

std::string verbatim_key(std::string_view bytes) {
	return "verbatim:" + platform::sha256_hex_digest(std::string(bytes));
}

} // namespace

std::string register_verbatim(OperationProtectedSymbolTable &table, const std::string &bytes) {
	// Bytes that would end their own span early, or open another, are not the
	// model's words as one span: they are sent as ordinary text.
	if (bytes.find(kVerbatimClose) != std::string::npos || bytes.find(kVerbatimOpen) != std::string::npos) return bytes;
	table.model_authored[verbatim_key(bytes)] = bytes;
	return std::string(kVerbatimOpen) + bytes + std::string(kVerbatimClose);
}

std::string neutralize_verbatim(const std::string &text, const OperationProtectedSymbolTable *table) {
	if (text.find(kVerbatimOpen) == std::string::npos && text.find(kVerbatimClose) == std::string::npos) return text;
	// Text with no mark left in it: a lone or unregistered mark is no mark.
	const auto unmarked = [](std::string_view part) {
		std::string plain(part);
		for (const std::string_view mark : {kVerbatimOpen, kVerbatimClose})
			for (std::size_t found = plain.find(mark); found != std::string::npos; found = plain.find(mark, found))
				plain.erase(found, mark.size());
		return plain;
	};
	std::string kept;
	std::size_t cursor = 0;
	for (const VerbatimSpan &span : verbatim_spans(text)) {
		const std::string_view bytes(text.data() + span.bytes_from, span.bytes_to - span.bytes_from);
		kept += unmarked(std::string_view(text).substr(cursor, span.from - cursor));
		if (table != nullptr && table->model_authored.count(verbatim_key(bytes)) > 0)
			kept.append(text, span.from, span.to - span.from);
		else
			kept += unmarked(bytes);
		cursor = span.to;
	}
	return kept + unmarked(std::string_view(text).substr(cursor));
}

void neutralize_verbatim(Json &value, const OperationProtectedSymbolTable *table) {
	if (value.is_string()) {
		const std::string &text = value.get_ref<const std::string &>();
		if (text.find("\xE2\x9F\xAA") != std::string::npos) value = neutralize_verbatim(text, table);
	} else if (value.is_object() || value.is_array()) {
		for (Json &entry : value) neutralize_verbatim(entry, table);
	}
}

VerbatimCut cut_verbatim(const std::string &text) {
	VerbatimCut cut;
	std::size_t cursor = 0;
	for (const VerbatimSpan &span : verbatim_spans(text)) {
		cut.without.append(text, cursor, span.bytes_from - cursor);
		cut.spans.push_back(text.substr(span.bytes_from, span.bytes_to - span.bytes_from));
		cursor = span.bytes_to;
	}
	cut.without.append(text, cursor, std::string::npos);
	return cut;
}

std::string paste_verbatim(const std::string &text, const VerbatimCut &cut) {
	const std::string empty = std::string(kVerbatimOpen) + std::string(kVerbatimClose);
	std::string pasted;
	std::size_t cursor = 0;
	for (const std::string &bytes : cut.spans) {
		const std::size_t at = text.find(empty, cursor);
		if (at == std::string::npos) throw std::runtime_error("a verbatim span was lost in protection");
		pasted.append(text, cursor, at + kVerbatimOpen.size() - cursor);
		pasted += bytes;
		cursor = at + kVerbatimOpen.size();
	}
	pasted.append(text, cursor, std::string::npos);
	return pasted;
}

std::string data_plane_text(const std::string &prompt) {
	if (prompt.empty() || prompt.front() != '{') return prompt;
	// The envelope is one JSON value; what the loop appended follows it.
	Json envelope;
	std::size_t consumed = 0;
	try {
		std::istringstream stream(prompt);
		stream >> envelope;
		const std::streampos at = stream.tellg();
		consumed = at < 0 ? prompt.size() : static_cast<std::size_t>(at);
	} catch (const std::exception &) {
		return prompt;
	}
	if (!envelope.is_object() || !envelope.contains("dataPlane") || !envelope.at("dataPlane").is_object()
			|| !envelope.at("dataPlane").contains("payload") || !envelope.at("dataPlane").at("payload").is_object())
		return prompt;
	const std::string appended = consumed < prompt.size() ? prompt.substr(consumed) : std::string();
	Json payload = envelope.at("dataPlane").at("payload");
	// A conversational turn carries its sectioned prompt whole.
	if (payload.contains("prompt") && payload.at("prompt").is_string())
		return payload.at("prompt").get<std::string>() + appended;
	std::string text;
	const auto section = [&text](const char *name, const std::string &content) {
		if (content.empty()) return;
		text += std::string("<<<LM_SECTION:") + name + ">>>\n" + content + "\n<<<LM_END_SECTION:" + name + ">>>\n\n";
	};
	const auto string_at = [](const Json &object, std::initializer_list<const char *> path) {
		const Json *at = &object;
		for (const char *key : path) {
			if (!at->is_object() || !at->contains(key)) return std::string();
			at = &at->at(key);
		}
		return at->is_string() ? at->get<std::string>() : std::string();
	};
	const auto contents = [](const Json &object, const char *key) {
		std::string joined;
		if (object.is_object() && object.contains(key) && object.at(key).is_array())
			for (const Json &item : object.at(key))
				if (item.is_object() && item.contains("content") && item.at("content").is_string()) {
					joined += item.at("content").get<std::string>();
					if (!joined.empty() && joined.back() != '\n') joined += '\n';
				}
		return joined;
	};
	const Json request = payload.contains("request") && payload.at("request").is_object() ? payload.at("request") : Json::object();
	section("SYSTEM", string_at(request, {"system", "instruction"}));
	section("USER_DEFAULT_INSTRUCTIONS", string_at(request, {"userDefaultInstruction"}));
	section("MATTER_INSTRUCTIONS", string_at(request, {"projectInstruction"}));
	section("TASK", string_at(request, {"task", "instruction"}));
	section("CAPABILITY", string_at(request, {"capabilityInstruction"}));
	section("CANDIDATE_CONTEXT", string_at(request, {"candidateContext"}));
	section("PROJECT_CONTEXT", contents(request, "projectContext"));
	section("SOURCE_MATERIAL", contents(request, "sourceMaterial"));
	section("SOURCE_COVERAGE", string_at(request, {"sourceCoverage"}));
	section("CONVERSATION_STATE", contents(request, "conversationState"));
	section("HISTORY", contents(request, "history"));
	section("REQUEST_COVERAGE", string_at(request, {"requestCoverage"}));
	// The current request is always a section, even an empty one, so what
	// follows it is read as appended.
	text += "<<<LM_SECTION:CURRENT_REQUEST>>>\n" + string_at(request, {"currentRequest"})
		+ "\n<<<LM_END_SECTION:CURRENT_REQUEST>>>\n\n";
	// The operation's own state and instructions: Singularity's block. Drafts and
	// proposed values in it are the model's, a document's current text is
	// shown again - none of it grounds a constant.
	payload.erase("request");
	if (!payload.empty()) text += payload.dump() + "\n";
	return text + appended;
}

ComputeRoundOutput serve_round(const ComputeRoundInput &input, OperationProtectedSymbolTable &table,
		OpContext context, const ComputeBudgetLimits &limits, const ExecuteOps &execute) {
	ComputeRoundOutput output;
	std::vector<OpRecord> records;
	const std::size_t call = input.call;
	const ParsedOps parsed = parse_ops(input.ops_json);

	/* - - - - - - */

	if (parsed.code != Code::None) {
		// The object does not parse: the whole round is one error.
		OpNode whole;
		whole.id = "q1";
		OpAnswer answer;
		answer.status = OpStatus::Error;
		answer.code = Code::BadJson;
		answer.error.code = Code::BadJson;
		records.push_back(record_of(whole, answer, refusal_line(whole, answer.error), call, input.round,
			input.model_turn, input.carrier));
	} else {
		const std::size_t count = parsed.ops.size();
		const bool taskClosed = table.compute_rounds_used >= limits.compute_rounds_per_task;
		const std::size_t opsLeft = limits.ops_per_task > table.ops_used ? limits.ops_per_task - table.ops_used : 0;
		std::vector<std::optional<OpAnswer>> decided(count);
		std::vector<bool> repeated(count, false);
		// Names this round's failed ops would have bound: an op that uses one
		// fails as dependency_failed, never runs.
		std::set<std::string> failedBinds;
		std::vector<OpNode> runnable;
		std::vector<std::size_t> runnableIndex;
		const auto refuse = [&](std::size_t index, OpStatus status, Code code, std::size_t argument = 0) {
			OpAnswer answer;
			answer.status = status;
			answer.code = code;
			answer.error.code = code;
			answer.error.argument = argument;
			decided[index] = answer;
			if (parsed.ops[index].bind) failedBinds.insert(*parsed.ops[index].bind);
		};
		for (std::size_t index = 0; index < count; ++index) {
			const OpNode &node = parsed.ops[index];
			if (parsed.checks[index]) {
				OpAnswer answer;
				answer.status = OpStatus::Error;
				answer.code = parsed.checks[index]->code;
				answer.error = *parsed.checks[index];
				decided[index] = answer;
				if (node.bind) failedBinds.insert(*node.bind);
				continue;
			}
			if (taskClosed) {
				refuse(index, OpStatus::NotServed, Code::TaskBudget);
				continue;
			}
			if (index >= limits.ops_per_round) {
				refuse(index, OpStatus::NotServed, Code::TooManyOps);
				continue;
			}
			// A name is unique in the task: an earlier round's name is not
			// taken again - unless this is the very op that bound it, asked again
			// (a model re-sends its whole batch after a repair round): the same
			// question, answered the same, free.
			if (node.bind && table.compute_labels.count(*node.bind) > 0) {
				const std::string asked = request_canonical(node);
				const bool same = std::any_of(table.op_log.begin(), table.op_log.end(), [&](const OpRecord &record) {
					return record.declared_label == *node.bind && record.request_canonical == asked
						&& (record.status == "ok" || record.status == "bounded");
				});
				if (!same) {
					refuse(index, OpStatus::Error, Code::BindReused);
					continue;
				}
			}
			std::size_t dependency = 0;
			for (std::size_t argument = 0; argument < node.args.size() && dependency == 0; ++argument) {
				TypeError ignored;
				const std::optional<Arg> arg = parse_arg(node.args[argument], ignored);
				if (arg && arg->kind == ArgKind::Ref && failedBinds.count(arg->label) > 0) dependency = argument + 1;
			}
			if (dependency > 0) {
				refuse(index, OpStatus::Error, Code::DependencyFailed, dependency);
				continue;
			}
			// The same request that failed the same static way before is answered
			// from the log, not asked of the executor again (DESIGN4 §10) - only
			// where that failure is a function of the request itself (and its
			// declared type): a failure that depends on the op's id, its name, an
			// earlier op, what the task has named since or what the payload now
			// shows is worked out again, so a corrected chain always runs.
			const std::string canonical = request_canonical(node);
			const std::string expected = node.expect.value_or(std::string());
			// An @name stands for whatever that name is bound to now - which an
			// earlier round may not have had: such an op is always worked out.
			const bool namesEarlier = canonical.find('@') != std::string::npos;
			bool answeredBefore = namesEarlier;
			for (auto record = table.op_log.rbegin(); record != table.op_log.rend() && !answeredBefore; ++record) {
				if (record->request_canonical != canonical || record->expected_type != expected
						|| (record->status != "error" && record->status != "refused"))
					continue;
				const std::optional<Code> code = code_from_string(record->code);
				if (!code || !answerable_from_log(*code)) continue;
				// The argument it named, as its line wrote it (" arg N").
				std::size_t argument = 0;
				const std::string head = record->status + " " + record->code + " arg ";
				const std::size_t named = record->result_line.find(head);
				if (named != std::string::npos) {
					std::size_t digits = named + head.size();
					while (digits < record->result_line.size() && is_digit(record->result_line[digits])) {
						argument = argument * 10 + static_cast<std::size_t>(record->result_line[digits] - '0');
						++digits;
					}
				}
				refuse(index, record->status == "refused" ? OpStatus::Refused : OpStatus::Error, *code, argument);
				repeated[index] = true;
				answeredBefore = true;
			}
			if (answeredBefore && repeated[index]) continue;
			if (runnable.size() >= opsLeft) {
				refuse(index, OpStatus::NotServed, Code::TaskBudget);
				continue;
			}
			runnable.push_back(node);
			runnableIndex.push_back(index);
		}

		/* - - - - - - */

		context.call = call;
		context.round = input.round;
		context.model_turn = input.model_turn;
		context.carrier = input.carrier;
		const std::size_t logBefore = table.op_log.size();
		std::vector<OpAnswer> answers;
		if (!runnable.empty() && execute) {
			try {
				answers = execute(runnable, context);
			} catch (const std::exception &) {
				answers.clear();
			}
		}
		// Records the executor appended for this round carry what only it knows
		// (charges, fact keys): kept, and completed with the line as sent.
		std::vector<OpRecord> executorRecords;
		if (table.op_log.size() == logBefore + runnable.size()) {
			executorRecords.assign(table.op_log.begin() + static_cast<std::ptrdiff_t>(logBefore), table.op_log.end());
			table.op_log.resize(logBefore);
		} else if (table.op_log.size() > logBefore) {
			table.op_log.resize(logBefore);
		}
		for (std::size_t run = 0; run < runnable.size(); ++run) {
			OpAnswer answer;
			if (run < answers.size()) {
				answer = answers[run];
			} else {
				// An op the executor did not answer is not served - never guessed.
				answer.status = OpStatus::NotServed;
				answer.code = Code::RoundBudget;
				answer.error.code = Code::RoundBudget;
			}
			decided[runnableIndex[run]] = answer;
		}

		/* - - - - - - */

		std::map<std::size_t, std::size_t> executorRecordOf;
		for (std::size_t run = 0; run < runnable.size() && run < executorRecords.size(); ++run)
			executorRecordOf[runnableIndex[run]] = run;
		for (std::size_t index = 0; index < count; ++index) {
			const OpNode &node = parsed.ops[index];
			const OpAnswer &answer = *decided[index];
			// A name an op bound stands for its symbol from here on.
			const bool bound = answer.status == OpStatus::Ok && node.bind && !answer.results.empty()
				&& answer.relation.empty();
			if (bound && table.compute_labels.count(*node.bind) == 0) table.compute_labels[*node.bind] = answer.results.front();
			const LabelSymbols labels(table.compute_labels.begin(), table.compute_labels.end());
			const std::string line = result_line(node, answer, repeated[index], labels);
			OpRecord record = record_of(node, answer, line, call, input.round, input.model_turn, input.carrier);
			const auto fromExecutor = executorRecordOf.find(index);
			if (fromExecutor != executorRecordOf.end()) {
				OpRecord merged = executorRecords[fromExecutor->second];
				merged.op_id = record.op_id;
				merged.call = record.call;
				merged.round = record.round;
				merged.model_turn = record.model_turn;
				merged.carrier = record.carrier;
				merged.request_canonical = record.request_canonical;
				if (merged.operand_symbols.empty()) merged.operand_symbols = record.operand_symbols;
				merged.expected_type = record.expected_type;
				merged.declared_label = record.declared_label;
				merged.justification = record.justification;
				merged.status = record.status;
				merged.code = record.code;
				merged.results = record.results;
				merged.result_line = record.result_line;
				merged.observation = merged.observation || record.observation;
				record = std::move(merged);
			}
			records.push_back(std::move(record));
		}
		table.ops_used += runnable.size();
	}

	/* - - - - - - */

	for (const OpRecord &record : records) {
		++output.ops;
		if (record.status == "ok") ++output.ok;
		else if (record.status == "bounded") ++output.bounded;
		else if (record.status == "unknown") ++output.unknown;
		else if (record.status == "refused") ++output.refused;
		else ++output.error;
		if (record.observation) ++output.observations;
	}
	output.answered = output.ok + output.bounded + output.unknown > 0;
	if (output.answered) ++table.compute_rounds_used;
	else ++table.repair_rounds_used;
	output.closed = table.compute_rounds_used >= limits.compute_rounds_per_task || table.ops_used >= limits.ops_per_task;
	const std::size_t opsLeft = limits.ops_per_task > table.ops_used ? limits.ops_per_task - table.ops_used : 0;
	// `input.round` is the compute round this reply is answered as (a repair
	// round spends none, so it keeps the number): the rounds that remain after
	// it are counted from the call's compute rounds alone.
	const std::size_t rounds = limits.compute_rounds_per_call;
	const std::size_t round = std::max<std::size_t>(input.round, 1);
	const std::size_t spent = std::min(rounds, round - (output.answered ? 0 : 1));
	output.local_block = results_block_after(records, std::min(round, std::max<std::size_t>(rounds, 1)), rounds,
		rounds - spent, opsLeft);
	table.op_log.insert(table.op_log.end(), records.begin(), records.end());
	return output;
}

std::string snapshot_json(const ComputeSnapshot &snapshot) {
	Json opLog = Json::array();
	for (const SnapshotOp &op : snapshot.op_log)
		opLog.push_back({{"opId", op.op_id}, {"request", op.request_canonical}, {"operandFactKeys", op.operand_fact_keys},
			{"status", op.status}, {"code", op.code}, {"label", op.declared_label}, {"resultFactKeys", op.result_fact_keys}});
	Json labels = Json::object();
	for (const auto &[label, factKey] : snapshot.labels) labels[label] = factKey;
	Json byRoot = Json::object();
	for (const auto &[root, count] : snapshot.task_by_root) byRoot[root] = count;
	Json byGroupKey = Json::object();
	for (const auto &[key, count] : snapshot.task_by_group_key) byGroupKey[key] = count;
	Json answered = Json::array();
	for (const std::string &key : snapshot.answered) answered.push_back(key);
	return Json{{"opLog", std::move(opLog)}, {"labels", std::move(labels)},
		{"taskObservations", snapshot.task_observations}, {"taskByRoot", std::move(byRoot)},
		{"taskByGroupKey", std::move(byGroupKey)}, {"answered", std::move(answered)},
		{"computeRoundsUsed", snapshot.compute_rounds_used}, {"opsUsed", snapshot.ops_used},
		{"repairRoundsUsed", snapshot.repair_rounds_used}, {"providerChain", snapshot.provider_chain}}.dump();
}

std::optional<ComputeSnapshot> snapshot_from_json(const std::string &json) {
	const Json value = Json::parse(json, nullptr, false);
	if (!value.is_object()) return std::nullopt;
	ComputeSnapshot snapshot;
	const auto strings = [](const Json &list) {
		std::vector<std::string> read;
		if (list.is_array())
			for (const Json &entry : list)
				if (entry.is_string()) read.push_back(entry.get<std::string>());
		return read;
	};
	const auto text = [](const Json &object, const char *key) {
		return object.contains(key) && object.at(key).is_string() ? object.at(key).get<std::string>() : std::string();
	};
	const auto count = [](const Json &object, const char *key) -> std::size_t {
		return object.contains(key) && object.at(key).is_number_unsigned() ? object.at(key).get<std::size_t>() : 0;
	};
	if (value.contains("opLog") && value.at("opLog").is_array())
		for (const Json &entry : value.at("opLog")) {
			if (!entry.is_object()) continue;
			SnapshotOp op;
			op.op_id = text(entry, "opId");
			op.request_canonical = text(entry, "request");
			op.operand_fact_keys = strings(entry.value("operandFactKeys", Json::array()));
			op.status = text(entry, "status");
			op.code = text(entry, "code");
			op.declared_label = text(entry, "label");
			op.result_fact_keys = strings(entry.value("resultFactKeys", Json::array()));
			snapshot.op_log.push_back(std::move(op));
		}
	const auto counts = [](const Json &object) {
		std::map<std::string, std::size_t> read;
		if (object.is_object())
			for (auto entry = object.begin(); entry != object.end(); ++entry)
				if (entry.value().is_number_unsigned()) read[entry.key()] = entry.value().get<std::size_t>();
		return read;
	};
	if (value.contains("labels") && value.at("labels").is_object())
		for (auto entry = value.at("labels").begin(); entry != value.at("labels").end(); ++entry)
			if (entry.value().is_string()) snapshot.labels[entry.key()] = entry.value().get<std::string>();
	snapshot.task_observations = count(value, "taskObservations");
	snapshot.task_by_root = counts(value.value("taskByRoot", Json::object()));
	snapshot.task_by_group_key = counts(value.value("taskByGroupKey", Json::object()));
	for (const std::string &key : strings(value.value("answered", Json::array()))) snapshot.answered.insert(key);
	snapshot.compute_rounds_used = count(value, "computeRoundsUsed");
	snapshot.ops_used = count(value, "opsUsed");
	snapshot.repair_rounds_used = count(value, "repairRoundsUsed");
	snapshot.provider_chain = text(value, "providerChain");
	return snapshot;
}

namespace {

// A sentence that leaves its conclusion open - the gate's only reading of the
// words, a closed list in the two languages Singularity answers in.
bool leaves_open(const std::string &sentence) {
	// Case in every script (utf8proc): "Не установлено" reads as "не установлено".
	const std::string folded = singularity::platform::folded_text(sentence);
	static const std::vector<std::string> open{"not establish", "cannot be determined", "can't be determined",
		"not determined", "cannot determine", "cannot be confirmed", "not confirmed", "cannot confirm", "not verified",
		"unverified", "cannot be calculated", "not calculated", "cannot calculate", "do not state", "does not state",
		"not stated", "unable to determine", "unable to confirm", "remains unresolved", "not resolved", "not quantif",
		"\xD0\xBD\xD0\xB5 \xD1\x83\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2",    // не установ
		"\xD0\xBD\xD0\xB5 \xD0\xBE\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB",    // не определ
		"\xD0\xBD\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB7\xD0\xBC\xD0\xBE\xD0\xB6\xD0\xBD\xD0\xBE \xD0\xBE\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4",  // невозможно опред
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB4\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB4",  // не подтвержд
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD"};  // не проверен
	return std::any_of(open.begin(), open.end(), [&](const std::string &word) { return folded.find(word) != std::string::npos; });
}

// The protected symbols a text writes that local operations work on: amounts,
// rates, dates, counts, periods and tables.
std::vector<std::string> computable_symbols(const std::string &text) {
	static const std::vector<std::string> prefixes{"Amount", "Rate", "Date", "Number", "Count", "Duration", "Table"};
	std::vector<std::string> found;
	for (std::size_t at = text.find("[["); at != std::string::npos; at = text.find("[[", at + 2)) {
		const std::size_t close = text.find("]]", at);
		if (close == std::string::npos || close - at > 40) continue;
		const std::string symbol = text.substr(at, close + 2 - at);
		const std::string inner = symbol.substr(2, symbol.size() - 4);
		const bool computable = std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string &prefix) {
			return inner.rfind(prefix, 0) == 0 && inner.size() > prefix.size() && std::isdigit(static_cast<unsigned char>(inner[prefix.size()]));
		});
		if (computable && std::find(found.begin(), found.end(), symbol) == found.end()) found.push_back(symbol);
	}
	return found;
}

// A relation between two protected quantities left as a condition - "if
// [[Rate26]] exceeds [[Rate6]]", "whether [[Date40]] falls after [[Date59]]",
// «если [[Amount3]] превышает [[Amount4]]»: a symbol, a comparison word and
// another symbol close after an if/whether. Live EN1 (27 Sep 2026): the fee
// shifting was left "if" the underpayment rate exceeded the threshold. The
// pair compared, never the rest of the sentence: what Singularity answered or
// refused about that pair is what the sentence leaves open (live EN1 c22: "if
// [[Rate26]] exceeds [[Rate5]], ... the audit fee of [[Amount8]]" was read as a
// relation of all three, and the refused comparison was never seen).
std::optional<std::pair<std::string, std::string>> conditional_comparison(const std::string &sentence) {
	static const std::vector<std::string> conditions{"if ", "whether ", "unless ",
		"\xD0\xB5\xD1\x81\xD0\xBB\xD0\xB8 "};  // если
	static const std::vector<std::string> comparisons{"exceed", "greater", "less than", "lower than", "higher than", "below",
		"above", "more than", "after", "before", "later than", "earlier than", "within",
		"\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB2\xD1\x8B\xD1\x88",  // превыш
		"\xD0\xB1\xD0\xBE\xD0\xBB\xD1\x8C\xD1\x88\xD0\xB5",  // больше
		"\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8C\xD1\x88\xD0\xB5",  // меньше
		"\xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB6\xD0\xB5",  // позже
		"\xD1\x80\xD0\xB0\xD0\xBD\xD1\x8C\xD1\x88\xD0\xB5"};  // раньше
	const auto has_any = [](const std::string &text, const std::vector<std::string> &words) {
		const std::string folded = singularity::platform::folded_text(text);
		return std::any_of(words.begin(), words.end(), [&](const std::string &word) { return folded.find(word) != std::string::npos; });
	};
	static const std::vector<std::string> joins{" and ", " or ", ",", ";", " \xD0\xB8 ", " \xD0\xB8\xD0\xBB\xD0\xB8 "};  // и, или
	const auto kind = [](const std::string &symbol) {
		std::size_t end = 2;
		while (end < symbol.size() && std::isalpha(static_cast<unsigned char>(symbol[end]))) ++end;
		const std::string head = symbol.substr(2, end - 2);
		return head == "Count" ? std::string("Number") : head;
	};
	// Each pair of computable symbols of one kind close together, as the
	// sentence writes them: the one compared with the other, never two ends of
	// a rule ("after [[Date5]] and before [[Date6]]") or a payment by a date.
	const std::vector<std::string> symbols = computable_symbols(sentence);
	for (std::size_t index = 0; index + 1 < symbols.size(); ++index) {
		if (kind(symbols[index]) != kind(symbols[index + 1])) continue;
		const std::size_t first = sentence.find(symbols[index]);
		const std::size_t second = sentence.find(symbols[index + 1], first + symbols[index].size());
		if (first == std::string::npos || second == std::string::npos) continue;
		const std::size_t end = first + symbols[index].size();
		if (second - end > 60) continue;
		const std::string between = sentence.substr(end, second - end);
		if (has_any(between, joins)) continue;
		const std::size_t from = first > 120 ? first - 120 : 0;
		if (has_any(between, comparisons) && has_any(sentence.substr(from, first - from), conditions))
			return std::make_pair(symbols[index], symbols[index + 1]);
	}
	return std::nullopt;
}

// What Singularity answered about the relation a sentence leaves open - a record
// of this task that names every computable symbol of it: answered exactly (the
// computation's own verdict); settled only as a bound or an honest unknown, or
// refused for the disclosure budget (Singularity cannot settle it here: a
// question for the lawyer - owner, 27 Sep 2026); or never asked.
enum class Asked { No, Answered, ForLawyer };

Asked asked_about(const std::vector<std::string> &symbols, const OperationProtectedSymbolTable &table) {
	bool forLawyer = false;
	// A value worked out over the symbols - days between two dates, a
	// difference whose sign was not given - does not answer how they relate: a
	// relation word, a listing or a sign does (a fake-provider run: "which date
	// is later" was left open after duration_between, its before refused).
	const auto answers_relation = [](const OpRecord &record) {
		if (record.results.empty()) return true;
		for (const char *sign : {" POSITIVE", " NEGATIVE", " ZERO"})
			if (record.result_line.find(sign) != std::string::npos) return true;
		return false;
	};
	for (const OpRecord &record : table.op_log) {
		const bool names = std::all_of(symbols.begin(), symbols.end(),
			[&](const std::string &symbol) { return record.request_canonical.find(symbol) != std::string::npos; });
		if (!names) continue;
		if (record.status == "ok" && answers_relation(record)) return Asked::Answered;
		if (record.status == "ok") continue;
		if (record.status == "bounded" || record.status == "unknown" || record.code == "disclosure_budget") forLawyer = true;
	}
	return forLawyer ? Asked::ForLawyer : Asked::No;
}

std::string family_of(const std::vector<std::string> &symbols) {
	const auto has = [&](const char *prefix) {
		return std::any_of(symbols.begin(), symbols.end(), [&](const std::string &symbol) { return symbol.rfind(std::string("[[") + prefix, 0) == 0; });
	};
	std::vector<std::string> families;
	if (has("Table")) families.push_back("table (filter, sum_column)");
	if (has("Date") || has("Duration")) families.push_back("dates (add_duration, within, deadline)");
	if (has("Rate")) families.push_back("rates (apply_rate, ratio, compare)");
	if (has("Amount") || has("Number") || has("Count")) families.push_back("arithmetic (add, subtract, compare)");
	std::string text;
	for (std::size_t index = 0; index < families.size(); ++index) text += (index == 0 ? "" : ", ") + families[index];
	return text;
}

} // namespace

std::optional<std::string> completeness_repair(const std::string &final_text, const OperationProtectedSymbolTable &table,
		bool lawyer_question_offered, bool operations_open) {
	// The answer's prose: the structured answer's response, else the text.
	std::string prose = final_text;
	const Json parsed = Json::parse(final_text, nullptr, false);
	if (parsed.is_object() && parsed.contains("response") && parsed.at("response").is_string())
		prose = parsed.at("response").get<std::string>();
	std::vector<std::string> items;
	// Relations Singularity may not disclose more about: for the lawyer.
	std::vector<std::string> forLawyer;
	// An open conclusion over protected quantities.
	std::string sentence;
	const auto sentence_done = [&] {
		const std::optional<std::pair<std::string, std::string>> compared =
			sentence.empty() ? std::nullopt : conditional_comparison(sentence);
		if (!sentence.empty() && (compared || leaves_open(sentence))) {
			const std::vector<std::string> symbols = compared
				? std::vector<std::string>{compared->first, compared->second} : computable_symbols(sentence);
			// Open after Singularity answered the relation exactly is the answer's own
			// reading of it. What Singularity cannot settle here - a bound, an honest
			// unknown, a refusal - is never left open: the lawyer decides (owner,
			// 27 Sep 2026).
			const Asked asked = symbols.empty() ? Asked::No : asked_about(symbols, table);
			std::string line;
			for (std::size_t index = 0; index < symbols.size() && index < 6; ++index) line += (index == 0 ? "" : " + ") + symbols[index];
			// With no operation left for this answer, what one would settle is
			// the lawyer's too (live EN1 c22: the call's rounds were spent and an
			// open "if" went out unchecked).
			if (!symbols.empty() && asked == Asked::No && operations_open) items.push_back(line + " \xE2\x86\x92 " + family_of(symbols));
			if (asked == Asked::ForLawyer || (!symbols.empty() && asked == Asked::No && !operations_open)) forLawyer.push_back(line);
		}
		sentence.clear();
	};
	for (std::size_t at = 0; at < prose.size(); ++at) {
		sentence.push_back(prose[at]);
		const bool stop = prose[at] == '\n' || ((prose[at] == '.' || prose[at] == '?' || prose[at] == '!')
			&& (at + 1 == prose.size() || prose[at + 1] == ' ' || prose[at + 1] == '\n'));
		if (stop) sentence_done();
	}
	sentence_done();
	// A table total a hidden row sits inside - or a cell that reads it - used
	// with no operation over that table in this task.
	std::map<std::string, std::string> unverified;  // symbol -> table handle
	for (const tables::ProtectedTable &checked : table.tables) {
		const bool hiddenBody = std::any_of(checked.rows.begin(), checked.rows.end(), [](const tables::TableRow &row) {
			return row.hidden && row.kind == tables::RowKind::Body;
		});
		if (!hiddenBody) continue;
		std::vector<std::string> totalRefs;
		for (const tables::TableRow &row : checked.rows) {
			if (row.kind != tables::RowKind::Total && row.kind != tables::RowKind::Subtotal) continue;
			for (const tables::TableCell &cell : row.cells) {
				if (!cell.placeholder.empty()) unverified[cell.placeholder] = checked.handle;
				totalRefs.push_back(cell.ref);
			}
		}
		for (const tables::ProtectedTable &other : table.tables)
			for (const tables::TableRow &row : other.rows)
				for (const tables::TableCell &cell : row.cells) {
					if (cell.formula.empty() || cell.placeholder.empty()) continue;
					for (const std::string &ref : totalRefs)
						if (cell.formula.find(checked.sheet + "!" + ref) != std::string::npos
								|| cell.formula.find("'" + checked.sheet + "'!" + ref) != std::string::npos)
							unverified[cell.placeholder] = checked.handle;
				}
	}
	// Checked means the rows themselves were read (a filter, a column total, a
	// grouping ...); reading the total's own cell, or working its formula out
	// again, leaves the hidden row inside it (live c19 smoke, 27 Sep 2026:
	// cell(L13) was taken as checking the table).
	static const std::set<std::string> rowReading{"filter", "sum_column", "average_column", "min_column", "max_column",
		"count", "list_rows", "group", "aggregate", "derive_column"};
	std::set<std::string> worked;
	for (const OpRecord &record : table.op_log) {
		const std::string op = record.request_canonical.substr(0, record.request_canonical.find('('));
		if (rowReading.count(op) == 0) continue;
		for (const tables::ProtectedTable &checked : table.tables)
			if (record.request_canonical.find("[[" + checked.handle + "]]") != std::string::npos
					|| record.request_canonical.find(checked.handle + "]]") != std::string::npos)
				worked.insert(checked.handle);
	}
	for (const auto &[symbol, handle] : unverified) {
		if (worked.count(handle) != 0 || prose.find(symbol) == std::string::npos) continue;
		if (operations_open)
			items.push_back(symbol + " (a total over [[" + handle + "]], a hidden row inside its range) \xE2\x86\x92 table (filter, sum_column)");
		else
			forLawyer.push_back(symbol + " (a total over [[" + handle + "]], a hidden row inside its range)");
	}
	// A material result Singularity worked out and put before the model, in a line
	// the answer draws on, that the answer neither uses nor names (owner, 27 Sep
	// 2026: from derivation and relevance, never from a relation's kind).
	// Material: an amount or a rate. Relevant: the answer uses a symbol of the
	// same line. Never the conclusion - only the unused symbols.
	for (const OutboundRelation &surfaced : table.surfaced_relations) {
		std::vector<std::string> material;
		for (const std::string &result : surfaced.results)
			if (result.rfind("[[Amount", 0) == 0 || result.rfind("[[Rate", 0) == 0) material.push_back(result);
		if (material.empty()) continue;
		const bool used = std::any_of(material.begin(), material.end(),
			[&](const std::string &symbol) { return prose.find(symbol) != std::string::npos; });
		if (used) continue;
		std::vector<std::string> drawnOn;
		for (const std::string &symbol : computable_symbols(surfaced.sent))
			if (std::find(surfaced.results.begin(), surfaced.results.end(), symbol) == surfaced.results.end()
					&& prose.find(symbol) != std::string::npos)
				drawnOn.push_back(symbol);
		if (drawnOn.empty()) continue;
		std::string line;
		for (std::size_t index = 0; index < material.size() && index < 3; ++index) line += (index == 0 ? "" : " + ") + material[index];
		line += " (worked out by Singularity beside ";
		for (std::size_t index = 0; index < drawnOn.size() && index < 3; ++index) line += (index == 0 ? "" : ", ") + drawnOn[index];
		items.push_back(line + ", which the answer uses) \xE2\x86\x92 arithmetic (add, subtract, multiply)");
	}
	if (items.empty() && forLawyer.empty()) return std::nullopt;
	std::string text;
	if (!items.empty()) {
		text = operations_open
			? "SINGULARITY COMPLETENESS: This conclusion is locally computable and remains unresolved. Use the available "
				"protected operands and local operations before finalizing.\n"
			: "SINGULARITY COMPLETENESS: Singularity already worked these out and the answer leaves them out: use them before "
				"finalizing.\n";
		for (std::size_t index = 0; index < items.size() && index < 4; ++index) text += "- " + items[index] + "\n";
	}
	if (!forLawyer.empty()) {
		text += lawyer_question_offered
			? "SINGULARITY COMPLETENESS: Singularity cannot settle this here (it may not disclose more, no more can be worked out "
				"for this answer, or the material does not decide it), so the conclusion cannot stay open: ask the lawyer "
				"in lawyerQuestion, with the symbols as the options, before finalizing.\n"
			: "SINGULARITY COMPLETENESS: Singularity cannot settle this here (it may not disclose more, no more can be worked out "
				"for this answer, or the material does not decide it), so the conclusion cannot stay open: end the answer "
				"by asking the lawyer which it is, with the symbols as the options - never as settled, and never as not "
				"established.\n";
		for (std::size_t index = 0; index < forLawyer.size() && index < 4; ++index) text += "- " + forLawyer[index] + "\n";
	}
	return text;
}

} // namespace singularity::protected_api::papinian::compute
