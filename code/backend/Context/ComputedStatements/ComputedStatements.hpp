#pragma once

#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <optional>
#include <string>
#include <vector>

// Arithmetic a model asks Singularity to do on the lawyer's computer.
//
// Papinian Guard replaces a Project's amounts and dates before a request
// leaves this Mac, so a provider's model reads "B12: [[E25]] (shown in $)"
// and a request's "$2.8 million" as [[E7]]. It cannot tell whether $3,000,000
// meets a $2,800,000 condition - a live run answered "not determinable" (24 Sep
// 2026). So the model writes the arithmetic instead of the answer to it:
//
//   {{= [[E25]] - [[E7]] as money}}                        a value
//   {{if [[E25]] >= [[E7]]}}is met{{else}}is not met{{end}}   words that follow
//                                                            from a comparison
//
// and Singularity works it out here, from the real values. Nothing new leaves the
// device: the answer is stored as the model wrote it (each protected operand
// restored inside ⟦ ⟧), so every later request that reads it - the next turn,
// Memory, a Consilium synthesis - carries the arithmetic again, protected
// again, never its result. The result exists only where the lawyer reads it.
//
// The values and the arithmetic are PapinianGuard/SemanticAlgebra's: an
// operand ⟦…⟧ is one typed value however it is written ("$2.8 million",
// "18 400 000 (восемнадцать миллионов четыреста тысяч) рублей", "0,1 % за
// каждый день просрочки", "July 2026", "B12 = 0.001 (shown as a percentage)"),
// worked out exactly, and a date is the interval of days it names - "July 2026"
// is neither before nor after "July 25, 2026", and that is never guessed. A
// statement's head ends at its own "}}": a {{if C}}A{{else}}B{{end}} inside a
// value is if(C, A, B), and a {{= X}} inside one is (X).
namespace singularity::context::computed_statements {

// What the model is told when the values it reads are protected. A symbol
// names one FACT of the material, not a value (owner correction, 26 Sep 2026):
// four $5m facts are four symbols, and their equality is a relation Singularity
// says only when the task needs it. Exact work is Singularity's: the model chooses
// the operation (PapinianGuard/ProtectedOperations) and reasons over its
// answer; arithmetic it writes itself stays only for presentation.
inline constexpr const char *kInstruction =
	"Values written [[Org1]], [[Amount3]], [[Date4]] \xE2\x80\xA6 are the lawyer's protected facts. They stay on the "
	"lawyer's computer: you never see them and never guess them, and Singularity writes each back in place of its symbol "
	"before the lawyer reads your answer. A symbol stands for one fact of the material: where it is stated and what it "
	"is, with its value. Two facts of equal value are two symbols. Singularity tells you when values are equal if your task "
	"needs it (relations), or when you ask (compare). protectionMetadata gives each symbol's type (MONEY with its "
	"currency, PERCENTAGE, DATE with its precision, PERSON, ...), the known order of the protected dates, and "
	"relations: what Singularity already worked out from the real values, each derived value a symbol of its own. Rely on "
	"them first. Singularity does the exact work: when your answer depends on a further fact about protected values, ask "
	"Singularity in localFacts.questions and return no other content - prefer these operations to writing arithmetic "
	"yourself. "
	"Each question (at most 12 an answer) is an op, its operands (symbols such as [[Amount3]]; a period, rate or count "
	"only as the material writes it, such as 10 business days or 3% - never a figure of your own, never an amount or a "
	"date), a unit (days, business days, weeks, months, years) or null, a rule (governing, or calendar for calendar days) "
	"or null, and an expression, null except for op expression. Ops: compare and equals (LESS, EQUAL, GREATER), "
	"difference, sum, product, ratio, rate (an amount times a rate; a rate per day also times the period), round (a "
	"value and its places), between (a duration in the unit; business days under the rule that governs them), "
	"deadline (a date plus a duration), next_business_day, day_class, before, after, same_day, within, overlaps, "
	"same_day_of_year, next_occurrence, table_total (sheet, column, include_hidden) and expression ({{= ...}} over "
	"symbols, only when no op fits). Singularity answers each with a relation word or a new symbol, never the value; money "
	"is rounded to the cent. An answer may be BOUNDED (at most [[Duration2]], not before [[Date9]]): state only the "
	"bound. It may be UNKNOWN with a reason (rule_conflict, rule_unknown, precision_insufficient ...): never guess it - "
	"say what is certain, or ask the lawyer in lawyerQuestion when the answer turns on it. State every figure, date and "
	"result by writing the symbol Singularity gave for it, wherever the lawyer needs the value - each event of a chronology with "
	"its date, each party by name - never leaving it out because it is protected, and never writing a figure no symbol "
	"or source gives. Only to write a figure no answer gives, use {{= [[Amount3]] - [[Amount4]] as money}} (+ - * / ( "
	"), min, max, round(x, 2), if(c, a, b), days_between(a, b), add_days(d, n), add_months(d, n), within(d, start, "
	"end); formats as money, as $, as \xE2\x82\xBD, as percent, as number, as days, as date, as words); wording that "
	"depends on a comparison: {{if [[Amount3]] > [[Amount4]]}}...{{else}}...{{end}}. Write people and companies as "
	"their symbols (a PERSON's parts are [[Person5.first]] and [[Person5.last]]): Singularity writes each name in the "
	"right grammatical form.";

// Whether a protected request first spends a call of its own asking which
// facts its answer rests on. It no longer does: that call was 42% of a
// protected request's time and 46% of its cost, and returned no question 18
// times in 38 (ab4/ab2, 25 Sep 2026). The relations Singularity works out itself
// travel with the request instead (PapinianGuard/ProtectedRelations), and the
// answer turn keeps localFacts for anything else. Kept for one live A/B.
inline constexpr bool kLocalFactsStep = false;

// The first turn of a request whose values are protected: the model names the
// facts its answer will rest on before it answers, so that it answers with
// them rather than around them - a live run (25 Sep 2026) never asked, and
// hedged "if July 1, 2026 is the increase date" instead.
inline constexpr const char *kFactsStepInstruction =
	"SINGULARITY FACTS STEP: Before you answer, list every fact about protected values your answer will depend on - which "
	"date falls in which period, which amount is larger, differences, sums, ratios, durations, a figure scaled by a "
	"rate - as typed questions in localFacts.questions, and nothing else; an empty list when it depends on none. Singularity "
	"works them out on the lawyer's computer, and then you give your answer with them.";

// Russian case and gender statements the model once wrote itself. No longer
// sent - restoration writes each name in the form its place needs
// (PapinianGuard/RestorationGrammar) - but a stored answer that has them is
// still written out: {{case р [[E5]]}}, {{gender [[E5]]: именуемый|именуемая|именуемое}}.
inline constexpr const char *kRussianGrammarInstruction =
	"In Russian, a protected name that is not in the nominative is written {{case \xD1\x80 [[E5]]}} - the "
	"case as \xD1\x80, \xD0\xB4, \xD0\xB2, \xD1\x82 or \xD0\xBF - and a word that must agree with a protected person or company is "
	"written {{gender [[E5]]: \xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x83\xD0\xB5\xD0\xBC\xD1\x8B\xD0\xB9|\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x83\xD0\xB5\xD0\xBC\xD0\xB0\xD1\x8F|\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x83\xD0\xB5\xD0\xBC\xD0\xBE\xD0\xB5}} (masculine|feminine|neuter): Singularity "
	"writes the right forms, in answers and in documents alike.";

// True when `text` asks for any arithmetic - or writes a statement tag that
// opens none ("{{= … " never closed, an {{else}} without its {{if}}), which
// validation must see.
[[nodiscard]] bool has_statements(const std::string &text);

// Each protected placeholder standing in a statement's arithmetic - never in
// the words it chooses between - written ⟦[[E1]]⟧ (a part, ⟦[[E5.first]]⟧,
// the same way), so the value restored in its place stays one operand however
// it is written ("$2,800,000, 3000000" is two operands, not four). A
// statement nested in a value's arithmetic is arithmetic too. Applied to the
// provider's text before restoration.
[[nodiscard]] std::string mark_statement_operands(const std::string &provider_text);

// mark_statement_operands for a string of a model's response standing at
// `json_locator`. A localFacts question's expression is a calculation however
// it is written: "[[E3]] - [[E4]]" at "/localFacts/questions/0/expression" is
// read as {{= [[E3]] - [[E4]]}}, so its operands come back ⟦…⟧ like any
// statement's - restored bare, "$3,000,000 - $2,800,000" is no expression -
// and so are the expressions of a response document a string carries (the
// research protocol's final text).
[[nodiscard]] std::string mark_response_operands(const std::string &provider_text, const std::string &json_locator);

// A statement that could not be worked out, as written, and why - naming
// its operands as the statement writes them.
struct StatementFailure {
	std::string statement;
	std::string reason;
};

struct CheckedText {
	std::string text;
	std::vector<StatementFailure> failures;
};

// `text` with every statement worked out, and every statement that could not
// be. What cannot be worked out is never guessed and never explained in the
// text: a value with one operand is that operand as it is written, any other
// value and a condition are left out, and a tag that opens no statement is
// taken away - the failure is for the caller, who corrects it or asks. A
// reason never writes a figure worked out from protected values outside ⟦ ⟧.
// Each result is written in the language of the words on its statement's
// line (a bilingual contract keeps both), and Russian unit words after a count
// ("{{= …}} дня") agree with it.
[[nodiscard]] CheckedText compute_checked(const std::string &text);

// `text` as the lawyer reads it: compute_checked's text.
[[nodiscard]] std::string compute(const std::string &text);

// Facts a model asked for about protected values, each question a statement
// ({{= expression}} or {{if condition}}) whose operands are restored ⟦…⟧.
// One line per question, "<n>. <question> → <answer>": true or false; "not
// determined: <why>"; a quantity or a date as ⟦its value⟧ with its kind, so
// that protection sends a new symbol of that type in its place; or "could not
// be worked out: <why>". A value is never written outside ⟦ ⟧, and inside
// them only in a writing that reads back as exactly that value and period:
// what has none (1 / 3, an amount per day) is refused with how to ask. An
// amount of money is what is paid: one finer than its currency's cents
// ($61,333.33 * 1.035) is rounded half up to them, and its kind says so.
[[nodiscard]] std::string answer_local_facts(const std::vector<std::string> &questions);

// One question a model asks about protected values, typed: an operation over
// its operands - protected values as they come back restored, or the model's
// own literals ("3%", "12 months", "1.03").
struct FactQuestion {
	// compare, difference, sum, ratio, product, round (a value and its places;
	// an amount of money alone is rounded to the cent), before, after, same_day,
	// within, overlaps, duration_between, add_period, next_occurrence,
	// same_day_of_year, expression.
	std::string op;
	std::vector<std::string> operands;
	// A duration's unit: days, business days, weeks, months, years; "" for none.
	std::string unit;
	// op "expression" only: a statement, {{= …}}, or the calculation alone.
	std::string expression;
	// "governing" (business days under the rule that governs them), "calendar"
	// (calendar days) or "" - for between, deadline, next_business_day and
	// day_class (PapinianGuard/ProtectedOperations).
	std::string rule;
};

// The same answers, for typed questions, one line each: "<n>. <op> ⟦a⟧, ⟦b⟧ →
// <answer>". A relation is said in words over the operands ("⟦a⟧ is before
// ⟦b⟧", "⟦a⟧ is greater than ⟦b⟧"), or "not determined: <why>"; a derived
// quantity, date or duration is ⟦its value⟧ with its kind, which protection
// sends as a new symbol the model can write straight into its answer. Every
// operand and every value stands inside ⟦ ⟧, so protection sends a symbol for
// each - the same symbol a protected value already has.
[[nodiscard]] std::string answer_typed_facts(const std::vector<FactQuestion> &questions);

// ---------------------------------------------------------------------------
// For the operations executor (PapinianGuard/ProtectedOperations), which works
// on typed values rather than on restored text.

// A writing of `value` that SemanticAlgebra reads back as exactly that value -
// what goes inside ⟦ ⟧ so an expression works on the value itself - or
// nothing when it has none (an amount per day, 1 / 3).
[[nodiscard]] std::optional<std::string> exact_writing_of(const protected_api::papinian::semantic::Value &value);

// An expression over ⟦writing⟧ operands worked out exactly. A value is kept
// only when it has an exact writing - so a symbol made for it stands for it -
// and an amount of money is rounded half up to what is paid, and says so.
// `failure` is a closed code, never a sentence that could carry a value:
// "undetermined" (the operands leave it open: a date's precision),
// "currency_mismatch", "not_a_quantity", "division_by_zero", "per_period" (an
// amount owed for each day, which no symbol carries), "no_exact_writing",
// "not_worked_out" (anything else).
struct WorkedValue {
	std::optional<protected_api::papinian::semantic::Value> value;
	bool rounded = false;
	std::string failure;
};
[[nodiscard]] WorkedValue work_out(const std::string &expression,
	protected_api::papinian::semantic::Language language = protected_api::papinian::semantic::Language::English);

} // namespace singularity::context::computed_statements
