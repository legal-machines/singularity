#pragma once

#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The wire protocol of protected computation (DESIGN4 §1.5, §1.9, §2.1-§2.3):
// the one JSON Schema both carriers use, the strict parse of a model's
// `{"ops":[...]}` in the symbol plane, the argument grammar, the compute
// instruction with its signature lines, and the lines and the results block
// the model reads.
//
// Everything here is pure: it reads what the model wrote and what the executor
// answered, and writes Singularity's own words. It never sees a value - an answer
// carries symbols, types, relation words and closed codes, and a line is built
// from those alone (DESIGN4 §8.4 L1, L2).
namespace singularity::protected_api::papinian::compute {

// Carrier N (DESIGN4 §1.2): the native operation tool's wire name and its
// canonical id. Its arguments are the `{"ops":[...]}` object and stay in the
// symbol plane on every path - never restored, never figure-checked.
inline constexpr const char *kComputeToolWireName = "lm_compute";
inline constexpr const char *kComputeCanonicalToolId = "papinian.compute";
// Whether `name` is carrier N's wire name or canonical id.
[[nodiscard]] bool is_compute_tool(std::string_view name);

// The instruction a protected call carries in place of
// computed_statements::kInstruction (DESIGN4 §1.9), signatures included: the
// model writes its operations in localFacts.ops (carrier F), calls lm_compute
// (carrier N, native) or replies with its textual envelope (carrier N,
// textual).
[[nodiscard]] const std::string &compute_instruction();
[[nodiscard]] const std::string &compute_instruction(Carrier carrier);
// The signature lines alone (DESIGN4 §1.9): the `description` of `op` in the
// carrier-N schema.
[[nodiscard]] const std::string &signature_lines();
// The signature line of one op ("apply_rate(amount, rate[, duration]; mode=
// share|increase|decrease)"), what an arity or type_mismatch error echoes;
// empty for an op no signature names.
[[nodiscard]] std::string signature_of(std::string_view op);
// Every op the schema's enum admits, in its order.
[[nodiscard]] const std::vector<std::string> &op_names();

// The `{"ops":[...]}` schema (DESIGN4 §2.1) as JSON text: carrier F wraps it
// as `localFacts: object|null`, carrier N is `lm_compute`'s parameters. No
// recursion, no oneOf, no pattern, no format; every property required.
[[nodiscard]] std::string ops_schema_json();
// The same schema with the signature lines as the `op` description (carrier
// N, where no instruction travels beside the tool).
[[nodiscard]] std::string ops_tool_schema_json();

// A reply's ops, parsed strictly and checked locally (§2.1: ids, labels, why,
// places, the op enum, the where and options enums). `code` is BadJson when
// the object does not parse at all (the whole round is one error). `ops` holds
// every op the reply wrote, in order; `checks[i]` is the local error of
// `ops[i]` (none when it passed), and `errors` names the same failures by op
// id. An op that failed a local check is never executed.
struct ParsedOps {
	Code code = Code::None;
	std::vector<OpNode> ops;
	std::vector<std::pair<std::string, TypeError>> errors;
	std::vector<std::optional<TypeError>> checks;
};
[[nodiscard]] ParsedOps parse_ops(const std::string &ops_json);

// One argument by the grammar of §2.2; nothing, with `error`, when it is not
// one (a column by name is `ambiguous_operand`, a broken symbol
// `unknown_symbol`, a malformed reference `unknown_label`).
[[nodiscard]] std::optional<Arg> parse_arg(std::string_view written, TypeError &error);

// Whether `label` is a bind name under §2.1's rule: [a-z][a-z0-9_]{0,31}, and no
// run of three or more figures other than one four-figure year.
[[nodiscard]] bool valid_label(std::string_view label);

// The op as parsed, in the symbol plane: op(args;where;options). The key a
// repeated request is recognised by, and what the disclosure lists as asked.
[[nodiscard]] std::string request_canonical(const OpNode &node);

// Carrier F (DESIGN4 §1.2): the `{"ops":[...]}` object a model's final JSON
// carries in localFacts, as JSON text - what the loop hands the executor.
// Nothing when the final carries no operation (localFacts null, or an empty
// list). A textual final ({"type":"final","text":"{...}"}) is read through its
// text. Read from the provider's own bytes (D13), a word or a fence around the
// object is tolerated; `strict` - for text restoration has already been over -
// takes only an object that is the whole text (and a textual final whose text
// is one), since restoration keeps localFacts in the symbol plane only there.
[[nodiscard]] std::optional<std::string> ops_in_final_text(const std::string &final_text, bool strict = false);

// VERBATIM SPANS (DESIGN4 §8.7, D13). The model's own words sent back to it -
// an echo of its Project search, the prompts and options of its question to
// the lawyer - are its provider bytes exactly, never restored and protected
// again (which would show which of its guesses matched a protected value).
// In the loop's text they stand between ⟪verbatim⟫ and ⟪/verbatim⟫, and the
// bytes are registered on the operation table (`register_verbatim`).
// Protection sends a registered span unchanged (machine-owned: no detection,
// no mapping, no validation); a span whose bytes were never registered - text
// that only looks like one - loses its marks and is protected like any other
// text (`neutralize_verbatim`). Only source, request and lawyer-answer
// segments ground a constant, and a span is never one of them.
inline constexpr std::string_view kVerbatimOpen = "\xE2\x9F\xAAverbatim\xE2\x9F\xAB";
inline constexpr std::string_view kVerbatimClose = "\xE2\x9F\xAA/verbatim\xE2\x9F\xAB";
struct VerbatimSpan {
	// The whole span, marks included, and the bytes between them.
	std::size_t from = 0;
	std::size_t to = 0;
	std::size_t bytes_from = 0;
	std::size_t bytes_to = 0;
};
// Every span of `text`, left to right, never nested.
[[nodiscard]] std::vector<VerbatimSpan> verbatim_spans(std::string_view text);
// `bytes` registered on `table` for this operation, and the span that carries
// them.
[[nodiscard]] std::string register_verbatim(OperationProtectedSymbolTable &table, const std::string &bytes);
// `text` with each span whose bytes `table` registered kept, and every other
// span's marks taken out (its bytes stay, as ordinary text). With no table,
// every span's marks are taken out: what an unprotected send, a durable pause
// or the lawyer reads.
[[nodiscard]] std::string neutralize_verbatim(const std::string &text, const OperationProtectedSymbolTable *table);
// The same over every string of a JSON value (a turn's data plane).
void neutralize_verbatim(nlohmann::json &value, const OperationProtectedSymbolTable *table);
// Protection that reads one text whole (the scripted twin of a provider
// call): each span cut out and left as an empty pair of marks, then put back
// in order once the rest is protected. `paste_verbatim` throws when the
// protected text no longer holds the empty pairs as they were.
struct VerbatimCut {
	std::string without;
	std::vector<std::string> spans;
};
[[nodiscard]] VerbatimCut cut_verbatim(const std::string &text);
[[nodiscard]] std::string paste_verbatim(const std::string &text, const VerbatimCut &cut);

// The loop's prompt as PromptAssembler's sections, whichever form it has: a
// sectioned prompt as it is; a model-visible turn envelope (an operation
// turn's `{"controlPlane":...,"dataPlane":{"payload":...}}`, JSON-escaped, with
// what the loop appended after it) rebuilt from its data plane - the request's
// instructions, material and current request as their sections, the rest of
// the payload as Singularity's own block after them, and the appended text after
// that - so an operation's segments, rules and request read the same material
// on every carrier.
[[nodiscard]] std::string data_plane_text(const std::string &prompt);

// The symbol each name of this task stands for (bind -> "[[Amount12]]"): a
// derivation writes an earlier result by its symbol, never by the model's own
// name for it (DESIGN4 §2.7, D13).
using LabelSymbols = std::map<std::string, std::string>;

// How an op was worked out, as its line writes it after ":=": the op over its
// arguments as the model wrote them (symbols, handles, column letters,
// constants in quotes; an @name as the symbol it names), its where atoms and
// its option values - "apply_rate([[Amount3]], \"4%\", increase)". Symbol
// plane; carries no value Singularity worked out.
[[nodiscard]] std::string derivation_of(const OpNode &node, const LabelSymbols &labels = {});

// One op's line exactly as the model reads it (DESIGN4 §1.5), from the op as
// parsed and the executor's answer: "q1 ok [[Amount12]] MONEY(USD/month)
// base_rent_2026 := apply_rate([[Amount3]], \"4%\", increase)". Built from
// Singularity's own templates (Context/ControlText): the op id, status, code,
// symbols, types, names and argument positions are control text; the
// derivation and a hint are data. Ends with a newline. `repeated`: the same
// request failed the same way in an earlier round (DESIGN4 §10).
[[nodiscard]] std::string result_line(const OpNode &node, const OpAnswer &answer, bool repeated = false,
	const LabelSymbols &labels = {});

// A line for an op that never reached the executor: a local check, a budget.
[[nodiscard]] std::string refusal_line(const OpNode &node, const TypeError &error);

// The results block of one round (§1.5) as the model reads it (`local`
// false) or as the loop's prompt carries it (`local` true). The two are the
// same text: a line writes each result by the symbol this operation's table
// issued, and the table is shared by every call of the attempt.
[[nodiscard]] std::string results_block(const std::vector<OpRecord> &records, std::size_t round, std::size_t rounds,
	std::size_t ops_left, bool local);

// Singularity's line when no more operations can be worked out for this answer
// (DESIGN4 §1.4 ComputeClosed).
[[nodiscard]] std::string closed_text();

// Singularity's line when the completeness gate gives an answer whose rounds are
// spent one more, for what the gate names.
[[nodiscard]] std::string reopened_text();

// The labels a resumed call could not rebuild (DESIGN4 §1.10), one line each.
[[nodiscard]] std::string labels_lost_text(const std::vector<std::string> &labels);

// The record of one op (DESIGN4 §2.3), without the fields only the executor
// knows (charges, fact keys): op_id "c<call>.r<round>.<qid>", carrier, the
// request as parsed, status, code, results and the line as sent.
[[nodiscard]] OpRecord record_of(const OpNode &node, const OpAnswer &answer, const std::string &line,
	std::size_t call, std::size_t round, std::size_t model_turn, Carrier carrier);

// What each part of a payload is (DESIGN4 §8.2): a PromptAssembler section by
// its name - SOURCE_MATERIAL and PROJECT_CONTEXT the lawyer's material, one
// segment per passage; CURRENT_REQUEST and the lawyer's standing instructions
// the request; HISTORY what a model wrote; Singularity's own sections control -
// and, in what the loop appended after them, a lawyer's answer, Project
// evidence read at the model's request (material), Singularity's templates
// (control) and everything else Singularity's own block (a results block, a page
// the model opened). Only source, request and lawyer-answer segments ground a
// constant; text that is none of these grounds nothing.
[[nodiscard]] std::vector<PayloadSegment> payload_segments(const std::string &payload);

// The pause snapshot (DESIGN4 §1.10) as the pause's loop JSON keeps it, and
// back: the op log over fact keys, the names, the task's counts and the keys
// already answered. Value-free; nothing in it is a restoration mapping.
[[nodiscard]] std::string snapshot_json(const ComputeSnapshot &snapshot);
[[nodiscard]] std::optional<ComputeSnapshot> snapshot_from_json(const std::string &json);

// Runs `ops` over the operation's table (operations::execute_ops, lane X).
using ExecuteOps = std::function<std::vector<OpAnswer>(const std::vector<OpNode> &, OpContext &)>;

// One round of operations, from either carrier (DESIGN4 §1.1 serveCompute,
// §1.3, §1.4, §10): parse; answer every op that fails a local check, the
// task's budgets or repeats a request that failed the same way before
// without executing it; execute the rest in order; write each op's line and
// record (op log), the names it bound, and the results block. A round in which
// no op was answered ok, bounded or unknown is a repair round. Never throws
// for what a model asks.
// The completeness gate (owner, 27 Sep 2026): whether a final answer leaves a
// quantitative, temporal or table-dependent conclusion unresolved - or rests on
// a table total a hidden row sits inside, with no operation over that table -
// while local operations could settle it. It does not solve the task: it names
// only the answer's own symbols, the tables and the operation family. Nothing
// when the answer is complete as far as Singularity can tell. With no operation
// left for this answer (`operations_open` false), what an operation would
// settle goes to the lawyer as well.
[[nodiscard]] std::optional<std::string> completeness_repair(const std::string &final_text,
	const OperationProtectedSymbolTable &table, bool lawyer_question_offered = false, bool operations_open = true);

[[nodiscard]] ComputeRoundOutput serve_round(const ComputeRoundInput &input, OperationProtectedSymbolTable &table,
	OpContext context, const ComputeBudgetLimits &limits, const ExecuteOps &execute);

} // namespace singularity::protected_api::papinian::compute
