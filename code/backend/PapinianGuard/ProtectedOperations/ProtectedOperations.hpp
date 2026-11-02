#pragma once

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/ProtectedRelations/ProtectedRelations.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Local operations over protected facts, chosen by the model inside its answer
// turn and worked out on the lawyer's computer (owner correction, 26 Sep 2026:
// "local execution owns exactness").
//
// The cloud model reasons, interprets and chooses which operation it needs; it
// never reconstructs a calculation Singularity can do exactly. It asks, in its
// answer's localFacts, for
//
//   compare([[Amount7]], [[Amount8]])            -> GREATER: [[Amount7]] is greater than [[Amount8]]
//   difference([[Amount7]], [[Amount8]])         -> [[Amount9]] = [[Amount7]] - [[Amount8]]
//   deadline([[Date1]], 10 business days)        -> [[Date2]], or BOUNDED: not before [[Date2]], or UNKNOWN
//
// and reasons over the answer. The questions arrive in the SYMBOL plane - the
// operands as the model wrote them, never restored - and every answer is
// symbols and closed words: a result is a new derived fact (a symbol of its
// own, keyed by how it was worked out, never merged with an equal value of the
// material), a relation is a word. A business-day count or deadline follows
// the rule that governs it (Context/BusinessCalendar): exact only when that
// rule is known and its holidays are; otherwise only the bound that holds
// whatever the holidays are, or UNKNOWN with a closed reason - never a guess.
//
// A relation can itself be confidential ("[[Org1]] owes more than [[Org2]]"):
// each answer is classified and checked against the relation policy before it
// is given, and one the policy withholds is answered UNKNOWN
// (withheld_by_policy), with nothing of it said. Every answer - given or
// withheld - carries its local provenance record (OutboundRelation), which the
// call that carries its line records; no record holds a raw value.
//
// Nothing here reads files or talks to anyone. The table, the policy and the
// symbols come in through Context, so this module is pure computation, linked
// alike into every build that reads operations.
namespace singularity::protected_api::papinian::operations {

namespace business_calendar = singularity::context::business_calendar;

// One operation the model asked for: its op, its operands (symbols such as
// [[Amount3]], or its own literals such as "3%" or "10 business days"), a
// unit for a duration, a rule ("governing", "calendar" or ""), and for op
// "expression" the calculation over symbols.
struct OperationRequest {
	std::string op;
	std::vector<std::string> operands;
	std::string unit;
	std::string rule;
	std::string expression;
};

enum class Status { Exact, Bounded, Unknown, Refused };
[[nodiscard]] const char *to_string(Status status);

// The answer to one operation. `line` is what the model reads - in the symbol
// plane, from ControlText's Operation* templates - and `local_line` the same
// line as the loop's prompt carries it on this computer: each symbol written
// as its local anchor (⟪writing|fact:key⟫, or ⟦writing⟧), which protection
// replaces by the same symbol again. `reason` is a closed code: rule_unknown,
// rule_conflict, definition_not_found, precision_insufficient,
// currency_mismatch, operand_unknown, not_a_quantity, withheld_by_policy,
// division_by_zero, no_exact_writing, unknown_op, own_figure, ratio_operand,
// same_operand, too_many.
struct OperationAnswer {
	std::size_t index = 0;
	Status status = Status::Unknown;
	std::string reason;
	std::string line;
	std::string local_line;
	std::vector<std::string> results;
	OutboundRelation record;
};

// What the executor needs, given by its caller (operation_context wires Singularity's
// own).
struct Context {
	// The operation's table: its rows are read, and a derived fact is issued
	// into it (through `issue`).
	OperationProtectedSymbolTable *table = nullptr;
	// The row a symbol names ("[[Amount3]]", "[[ amount3 ]]", "[[E17]]").
	std::function<const RequestMappingEntry *(std::string_view)> row_of;
	// The symbol of a derived fact: keyed only by `derivation` (canonical, over
	// the operands' fact keys), so the same operation on the same facts keeps
	// its symbol and an equal value worked out another way is another fact.
	std::function<std::string(const semantic::Value &, EntityType, const std::string &derivation)> issue;
	// A row's category ("MONEY_AMOUNT") and origin ("source", "request" ...).
	std::function<std::string(const RequestMappingEntry &)> category;
	std::function<std::string(const RequestMappingEntry &)> origin;
	// The relation policy.
	std::function<RelationSensitivity(const RelationFacts &)> classify;
	std::function<bool(const RelationFacts &)> permit;
	// The party symbol a fact belongs to, where the material ties it to one.
	std::function<std::string(const RequestMappingEntry &)> party_of;
	// The business-day rule that governs (BusinessCalendar::governing_rule over
	// the RAW local passages), and the raw workbook readings a table total reads.
	business_calendar::GoverningRules rules;
	std::vector<std::string> raw_passages;
	semantic::Language language = semantic::Language::English;
	// The loop's prompt as protection sends it - every protected value a
	// symbol: a figure of the model's own is an operand only where this writes
	// that figure openly (a period, a rate or a count the material states).
	// Never the local text: what it writes is no answer's business.
	std::string sent;
	// The business-day rules for one operation's facts (their raw writings):
	// the documents that count in business days and write them are the
	// operative ones. Unset: `rules` serves every operation.
	std::function<business_calendar::GoverningRules(const std::vector<std::string> &)> rules_for;
	// Singularity's own words and exact arithmetic (Context/ControlText,
	// Context/ComputedStatements): given, not linked, so this module builds
	// into every target that reads relations.
	std::function<std::string(singularity::context::control_text::Template, const std::vector<singularity::context::control_text::Slot> &)> render;
	std::function<singularity::context::computed_statements::WorkedValue(const std::string &)> work_out;
	std::function<std::optional<std::string>(const semantic::Value &)> exact_writing;
};

// The questions one answer may ask; each past them is REFUSED (too_many) and
// recorded, whatever it asks.
inline constexpr std::size_t kMaximumQuestions = 12;

// Each operation worked out, in order (index 1 ...). Never throws for what a
// model asks: a question Singularity cannot answer exactly is BOUNDED, UNKNOWN or
// REFUSED, with its reason. No answer depends on a protected value in any way
// but the relation it states over symbols: a figure of the model's own is an
// operand only where the sent text writes it openly - a period, a rate, a
// count, a rounding's places - never beside an amount or a date, and a refusal
// reads the same whatever the values are.
[[nodiscard]] std::vector<OperationAnswer> execute(const std::vector<OperationRequest> &requests, Context &context);

// The answers as the loop's prompt carries them (`local` true: local lines) or
// as the model reads them, one per line.
[[nodiscard]] std::string block(const std::vector<OperationAnswer> &answers, bool local);

// How a symbol's row is written into text this computer keeps and protection
// reads again: its anchor ⟪writing|fact:key⟫ when the row is a fact with a
// key, else ⟦writing⟧ - either way protection sends that row's symbol, and a
// send nothing protects writes the value (strip_fact_anchor_marks).
[[nodiscard]] std::string local_writing(const RequestMappingEntry &row);

// ---------------------------------------------------------------------------
// The answer check (DESIGN3 9.8 b, c), on the model's final PROTECTED text.

struct FigureCheck {
	// Figures written outside a symbol that the sent prompt does not write
	// (money, percentages, dates, durations, and plain numbers of four digits
	// or more): each located by its byte offset in the text, never by value.
	std::vector<std::size_t> untraced;
	// Arithmetic or comparisons the model states over symbols that the values
	// contradict ("[[Amount9]] = [[Amount7]] - [[Amount8]]" where they do not
	// make it, "[[Amount7]] < [[Amount8]]" where it is greater).
	std::vector<std::size_t> wrong;
	[[nodiscard]] std::size_t count() const { return untraced.size() + wrong.size(); }
};

[[nodiscard]] FigureCheck check_figures(const std::string &provider_text, const std::string &sent_prompt,
	const std::vector<RequestMappingEntry> &mapping);

// ---------------------------------------------------------------------------
// Singularity's own wiring: the guard's table, symbol lookup, derived facts and
// relation policy. Header-only, so this module links without the guard.

// The text between a PromptAssembler section's own lines, or nothing.
inline std::string prompt_section(const std::string &prompt, std::string_view name) {
	const std::string opening = "<<<LM_SECTION:" + std::string(name) + ">>>\n";
	const std::size_t start = prompt.find(opening);
	if (start == std::string::npos) return {};
	const std::size_t from = start + opening.size();
	const std::size_t close = prompt.find("\n<<<LM_END_SECTION:" + std::string(name) + ">>>", from);
	return close == std::string::npos ? std::string() : prompt.substr(from, close - from);
}

// The RAW passages of a prompt - each source passage on its own ("[SOURCE:
// DOC_2:CHUNK_1]" names its document), then the Project context - as the
// business-day rule and a table total read them. Local only, never rendered.
inline std::vector<business_calendar::RulePassage> rule_passages(const std::string &raw_prompt) {
	std::vector<business_calendar::RulePassage> passages;
	const std::string sources = prompt_section(raw_prompt, "SOURCE_MATERIAL");
	for (std::size_t at = sources.find("[SOURCE:"); at != std::string::npos;) {
		const std::size_t next = sources.find("[SOURCE:", at + 8);
		business_calendar::RulePassage passage;
		const std::string whole = sources.substr(at, next == std::string::npos ? std::string::npos : next - at);
		const std::size_t colon = whole.find(':', 8);
		if (colon != std::string::npos) passage.document = whole.substr(8, colon - 8);
		// The passage is its text after the "[SOURCE:DOC_2:CHUNK_1]" line: the
		// text protection read and filed its facts under (their offsets and
		// passage digests), and the text SentPassages resolves.
		const std::size_t body = whole.find('\n');
		passage.text = body == std::string::npos ? std::string() : whole.substr(body + 1);
		if (const std::size_t closing = passage.text.find("\n[/SOURCE]"); closing != std::string::npos)
			passage.text.erase(closing);
		passage.sha256 = platform::sha256_hex_digest(passage.text);
		passages.push_back(std::move(passage));
		at = next;
	}
	const std::string project = prompt_section(raw_prompt, "PROJECT_CONTEXT");
	if (!project.empty()) {
		// A Project record, not a document: a rule it states is the matter rule.
		business_calendar::RulePassage record;
		record.text = project;
		record.sha256 = platform::sha256_hex_digest(project);
		record.document = "PROJECT_CONTEXT";
		record.matter_record = true;
		passages.push_back(std::move(record));
	}
	return passages;
}

// A business-day rule the lawyer gave: what they answered to Singularity's question
// ("LAWYER'S ANSWER ..." up to Singularity's next line) - the latest answer stands;
// else a sentence of the request itself that states one; else one a Project
// record states. A question, or words that only mention holidays, state none.
inline std::optional<business_calendar::BusinessDayRule> lawyer_rule_in(const std::string &raw_prompt) {
	namespace control = singularity::context::control_text;
	const std::string header = control::render(control::Template::LawyerAnswerHeader);
	std::optional<business_calendar::BusinessDayRule> found;
	for (std::size_t at = raw_prompt.find(header); at != std::string::npos; at = raw_prompt.find(header, at + header.size())) {
		const std::size_t from = at + header.size();
		const std::size_t end = raw_prompt.find("\nSINGULARITY:", from);
		if (auto rule = business_calendar::lawyer_rule(raw_prompt.substr(from, end == std::string::npos ? std::string::npos : end - from), true))
			found = rule;  // the latest answer stands
	}
	if (!found) found = business_calendar::lawyer_rule(prompt_section(raw_prompt, "CURRENT_REQUEST"), false);
	if (!found) {
		// Only a record a person set or confirmed: a value Singularity derived from a
		// document on its own is no lawyer's rule.
		const std::string project = prompt_section(raw_prompt, "PROJECT_CONTEXT");
		for (std::size_t at = 0; at < project.size() && !found;) {
			const std::size_t end = project.find('\n', at);
			const std::string line = project.substr(at, end == std::string::npos ? std::string::npos : end - at);
			at = end == std::string::npos ? project.size() : end + 1;
			if (line.find("\"authority\":\"user_authored\"") == std::string::npos
					&& line.find("\"authority\":\"user_confirmed\"") == std::string::npos)
				continue;
			found = business_calendar::lawyer_rule(line, false);
		}
	}
	return found;
}

// The business-day rule that governs a request, from its RAW prompt.
inline business_calendar::GoverningRules governing_rules_of(const std::string &raw_prompt) {
	business_calendar::GoverningInput input;
	input.passages = rule_passages(raw_prompt);
	input.matter_rule = lawyer_rule_in(raw_prompt);
	return business_calendar::governing_rule(input);
}

inline Context operation_context(const PapinianGuard &guard, OperationProtectedSymbolTable &table, const Policy &policy) {
	Context wired;
	wired.table = &table;
	wired.row_of = [&table](std::string_view marker) { return row_of_marker(table, marker); };
	wired.issue = [&guard, &table](const semantic::Value &value, EntityType type, const std::string &derivation) {
		return guard.issue_derived_value(table, value, type, derivation);
	};
	wired.category = [](const RequestMappingEntry &entry) {
		return entry.type ? std::string(to_string(*entry.type))
			: std::string(entry.source == DetectionSource::CustomTerm ? "CUSTOM_TERM" : "PROTECTED_VALUE");
	};
	wired.origin = [](const RequestMappingEntry &entry) {
		return entry.derived ? std::string("derived") : std::string(to_string(entry.provenance.origin));
	};
	wired.classify = [relations = policy.relations](const RelationFacts &facts) { return classify_relation(facts, relations); };
	wired.permit = [policy](const RelationFacts &facts) { return permit_relation(facts, policy); };
	wired.render = [](singularity::context::control_text::Template id, const std::vector<singularity::context::control_text::Slot> &slots) {
		return singularity::context::control_text::render(id, slots);
	};
	wired.work_out = [](const std::string &expression) { return singularity::context::computed_statements::work_out(expression); };
	wired.exact_writing = [](const semantic::Value &value) { return singularity::context::computed_statements::exact_writing_of(value); };
	return wired;
}

// The localFacts executor of a protected call (ResearchToolLoop's
// localFactsExecutor): the model's questions, read in the symbol plane,
// answered over `table` under `policy`, and returned as the loop's prompt
// carries them - each symbol as its local anchor, which the next turn's
// protection sends as the same symbol. The governing business-day rule, a
// lawyer's answer and a workbook's raw readings are read from the prompt as it
// stands; which party each fact belongs to only when a relation needs it.
inline std::function<std::string(const std::vector<singularity::context::computed_statements::FactQuestion> &, const std::string &)>
local_facts_executor(const PapinianGuard &guard, OperationProtectedSymbolTable &table, const Policy &policy) {
	return [&guard, &table, policy](const std::vector<singularity::context::computed_statements::FactQuestion> &questions,
			const std::string &prompt) {
		Context context = operation_context(guard, table, policy);
		// The prompt as it is sent: what the model sees, and all an answer may
		// depend on besides the relation it states. Nothing sent, nothing open.
		auto sent = std::make_shared<std::string>();
		try {
			*sent = guard.apply_frozen_mapping(prompt, table);
		} catch (const std::exception &) {
			sent->clear();
		}
		context.sent = *sent;
		context.rules = governing_rules_of(prompt);
		auto passages = std::make_shared<std::vector<business_calendar::RulePassage>>(rule_passages(prompt));
		auto matter = std::make_shared<std::optional<business_calendar::BusinessDayRule>>(lawyer_rule_in(prompt));
		context.rules_for = [passages, matter](const std::vector<std::string> &writings) {
			business_calendar::GoverningInput input;
			input.passages = *passages;
			input.matter_rule = *matter;
			input.operand_writings = writings;
			return business_calendar::governing_rule(input);
		};
		for (const business_calendar::RulePassage &passage : *passages) context.raw_passages.push_back(passage.text);
		context.language = semantic::language_of(prompt_section(prompt, "CURRENT_REQUEST"));
		auto parties = std::make_shared<std::optional<std::map<std::string, std::string>>>();
		context.party_of = [parties, sent, &table](const RequestMappingEntry &entry) -> std::string {
			if (!*parties) {
				std::map<std::string, std::string> byMarker;
				const std::vector<RequestMappingEntry> rows = table.mapping;
				relations::Input input;
				input.mapping = &rows;
				if (!sent->empty()) input.passages = {*sent};
				for (const auto &[row, party] : relations::parties_of(input))
					if (!party.empty() && row < rows.size()) byMarker[rows[row].placeholder] = party;
				*parties = std::move(byMarker);
			}
			const auto found = (*parties)->find(entry.placeholder);
			return found == (*parties)->end() ? std::string() : found->second;
		};
		std::vector<OperationRequest> requests;
		for (const auto &question : questions)
			requests.push_back({question.op, question.operands, question.unit, question.rule, question.expression});
		return block(execute(requests, context), true);
	};
}

} // namespace singularity::protected_api::papinian::operations
