#pragma once

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// What a model needs to know about a request's protected amounts and dates,
// worked out on the lawyer's computer - and what of it may leave.
//
// Papinian Guard sends "[[Amount3]]" for $42,500 and "[[Date4]]" for July 1,
// 2026, so a provider's model cannot tell that the July rent was billed at last
// year's rate, how many business days lie between an offer letter and the start
// of employment, or when "10 calendar days after the end of the quarter" falls
// against the workbook's own cut-off. Singularity works such relations out here,
// from the real values, as lines over symbols:
//
//   [[Amount9]] = [[Amount3]] increased by [[Rate1]] (one step, to the cent)
//   ledger rows 16-18 (...): B16 [[Amount4]], ... each is less than [[Amount9]] by [[Amount10]]
//   [[Duration1]] calendar days and [[Duration2]] business days (as the agreement defines Business Day) ...
//
// It is a query planner, not a relation dump (owner correction, 26 Sep 2026).
// Every relation Singularity can work out is derived locally (derive_candidates);
// only those the current task needs are sent (plan): a relation serves one of
// the intents the lawyer's question has, it relates a fact the question names
// or a fact of the passages the question targets, and the relation policy
// permits it. Relations compose - several one-bit lines can give a ledger's
// shape away - so no count of lines is a privacy argument; `Limits` is a
// safety cap, and what it drops is a correctness event. Every relation sent or
// held back is recorded locally with its provenance (OutboundRelation), and
// none of that record ever leaves.
//
// A value worked out here is a new symbol (a derived fact), never a figure; a
// relation is a word over symbols (less, before, a business day); the rest is
// closed vocabulary and what the sent text itself writes. Business days are
// counted under the rule that governs them (Context/BusinessCalendar): the
// operative document's own definition first - and never an exact count or day
// when that rule is not known here, only what holds whatever the holidays are.
// Nothing here reads files, keeps state or talks to anyone.
namespace singularity::protected_api::papinian::relations {

namespace business_calendar = singularity::context::business_calendar;

enum class Kind {
	ColumnTotal, RowBalance, HiddenRow,
	Escalation, StepOnDueDate, EscalatedVsLedger,
	DaysBetween, Deadline, NextBusinessDay, DateOrder, RuleVsCell,
	NamedSum, ValueEquality,
	ContactOwner,
};

// A relation kind as a record names it: "column_total", "days_between" ...
[[nodiscard]] const char *to_string(Kind kind);

// What the lawyer's question asks for - read from the RAW question on this
// computer, with closed English and Russian word lists. A relation is sent only
// when it serves one of them.
enum class Intent { Total, Shortfall, Compare, Deadline, BusinessDays, DaysBetween, Escalation, Contact, Chronology, SheetCheck };

[[nodiscard]] const char *to_string(Intent intent);
[[nodiscard]] std::vector<Intent> intents_of(std::string_view question);

// A value worked out here: what it is, how the model is told its type, and a
// canonical description of how it was worked out (over symbols, never values),
// so the same derivation keeps one symbol across the calls of an operation.
struct Derived {
	semantic::Value value;
	EntityType type = EntityType::Number;
	std::string derivation;
};

// One line Singularity could send. `text` writes each derived value as kDerivedMark
// + its index + kDerivedMark; plan() puts the value's symbol there.
struct Relation {
	Kind kind = Kind::DaysBetween;
	std::string text;
	double rank = 0.0;
	// The facts of the material it relates (rows of the operation's table).
	std::vector<std::size_t> operand_rows;
	// The intents it answers.
	std::set<Intent> serves;
	// The business-day rule it counted under, when it counted business days.
	std::optional<business_calendar::BusinessDayRule> rule;
	// An equality of a figure the lawyer wrote in the request with the facts of
	// the material it is (sent whatever the intent: the lawyer's own figure).
	bool lawyer_value = false;
	// A column total checked against the sheet's own total.
	bool sheet_total = false;
};

inline constexpr char kDerivedMark = '\x1D';

// Whether a protected request carries its relations (ProtectedInference); kept
// switchable for a live A/B against the model asking for each fact itself.
inline constexpr bool kProtectedRelations = true;

// A SAFETY cap on what one request carries - never a privacy argument. A
// relation the task needs that the cap drops is counted (dropped_by_cap) as a
// correctness event; the model can still ask for it as an operation.
struct Limits {
	std::size_t relations = 40;
	std::size_t bytes = 6000;
	std::size_t date_pairs = 10;
	std::size_t tables = 6;
	std::size_t rows_per_table = 200;
	std::size_t thresholds_per_pair = 3;
};

struct Input {
	// The operation's rows, frozen for this call: each symbol's value.
	const std::vector<RequestMappingEntry> *mapping = nullptr;
	// PROTECTED text (symbols in place), most relevant first: the sources the
	// request carries, its Project context and its history.
	std::vector<std::string> passages;
	// The lawyer's protected request, and the last answer of this chat.
	std::string question;
	std::string last_answer;
	semantic::Language language = semantic::Language::English;
	// RAW local text - never rendered, never sent: the question, for its
	// intents, and the passages (index for index with `passages` where both are
	// given), for the business-day rule that governs.
	std::string raw_question;
	std::vector<business_calendar::RulePassage> raw_passages;
	// A business-day rule the lawyer gave in this chat or a Project record states.
	std::optional<business_calendar::BusinessDayRule> matter_rule;
	Limits limits;
};

// Everything Singularity can work out for the request, all local.
struct Candidates {
	std::vector<Relation> relations;
	std::vector<Derived> derived;
	// The party symbol each fact belongs to where the material ties it to one
	// (the one Org or Person symbol of its sentence or table row).
	std::map<std::size_t, std::string> party_of;
};

[[nodiscard]] Candidates derive_candidates(const Input &input);

// Candidates::party_of alone: which party each fact of the passages belongs
// to (row index -> party symbol, "" when two places give two).
[[nodiscard]] std::map<std::size_t, std::string> parties_of(const Input &input);

// What the current task targets: the facts the question writes, its intents,
// and the passages it is about (those sharing two content words with it, else
// the first three), with every fact standing in them.
struct TaskFocus {
	std::set<std::size_t> question_rows;
	std::vector<Intent> intents;
	std::vector<std::size_t> focus_passages;
	std::set<std::size_t> focus_rows;
};

[[nodiscard]] TaskFocus focus_of(const Input &input);

// What is sent, each line with its local record, and what was held back by the
// relation policy (only counted for the lawyer). `timeline_markers` are the
// date symbols the legend's timeline may order: the dates of sent date
// relations, and the question's dates - every date of the targeted passages
// when the question asks for a chronology.
struct Plan {
	std::vector<std::string> lines;
	std::vector<OutboundRelation> records;
	std::vector<OutboundRelation> withheld;
	std::set<std::string> timeline_markers;
	std::size_t dropped_by_cap = 0;
	std::size_t candidates = 0;
	// Candidates no intent or fact of the task asks for: never sent.
	std::size_t skipped_irrelevant = 0;
};

// The symbol of a derived value (PapinianGuard::issue_derived_value): asked for
// only for a relation that is sent.
using IssueDerived = std::function<std::string(const Derived &)>;

// What the planner needs from the privacy policy and the fact registry. The
// caller gives it - plan_context(policy) wires Singularity's own - so this module
// stays pure computation, linked alike into every build that reads relations.
struct PlanContext {
	// A row's category as records name it ("MONEY_AMOUNT").
	std::function<std::string(const RequestMappingEntry &)> category;
	// Where the fact comes from ("source", "request" ...: FactProvenance).
	std::function<std::string(const RequestMappingEntry &)> origin;
	// The relation policy: how sensitive a relation is, and whether it may leave.
	std::function<RelationSensitivity(const RelationFacts &)> classify;
	std::function<bool(const RelationFacts &)> permit;
};

// Singularity's own planner context under `policy`.
inline PlanContext plan_context(const Policy &policy) {
	PlanContext context;
	context.category = [](const RequestMappingEntry &entry) {
		return entry.type ? std::string(to_string(*entry.type))
			: std::string(entry.source == DetectionSource::CustomTerm ? "CUSTOM_TERM" : "PROTECTED_VALUE");
	};
	context.origin = [](const RequestMappingEntry &entry) { return std::string(to_string(entry.provenance.origin)); };
	context.classify = [relations = policy.relations](const RelationFacts &facts) { return classify_relation(facts, relations); };
	context.permit = [policy](const RelationFacts &facts) { return permit_relation(facts, policy); };
	return context;
}

[[nodiscard]] Plan plan(const Input &input, const Candidates &candidates, const TaskFocus &focus,
	const PlanContext &context, const IssueDerived &issue);

// A column's total read from RAW workbook readings on this computer ("Row 16:
// A16 = ... | B16 = 42500 (shown in $)"): the sheet named `table` (its "Sheet"
// line, or a passage handle such as DOC_2), column `column`, its body rows -
// the hidden ones only when `include_hidden` - and which rows were hidden.
struct TableTotal {
	semantic::Value total;
	std::vector<std::string> hidden_cells;
	std::vector<std::string> counted_cells;
};
[[nodiscard]] std::optional<TableTotal> table_total(const std::vector<std::string> &raw_passages, const std::string &table,
	const std::string &column, bool include_hidden);

} // namespace singularity::protected_api::papinian::relations
