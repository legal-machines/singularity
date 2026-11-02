#pragma once

#include "Context/BusinessCalendar/BusinessCalendar.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBudget.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTableTypes.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "PapinianGuard/Taxonomy/Taxonomy.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The protected computation protocol's own types (DESIGN4, Papinian Cycle 4:
// "the cloud model chooses what it needs to compute; Singularity executes it
// locally; the cloud receives only symbolic results and relations").
//
// The model asks, in a strict structured request, for typed operations over
// symbols ("subtract [[Amount1]] [[Amount2]], bind profit"); Singularity checks each
// against the type system and the admission rules, works it out on this
// computer over the real values, and answers with a new symbol, a relation word
// or a closed error - never a value. What is kept here is the request as it was
// parsed (OpNode, Arg), the record of each operation (OpRecord), each value
// worked out (DerivedFact), the observation budget (ObservationLedger), and the
// dispatch interface each family of operations registers through
// (register_family): arithmetic (ArithmeticOps.cpp), temporal (TemporalOps.cpp)
// and tables (TableOps.cpp), one table of typers and evaluators behind one
// executor (ProtectedOperations).
//
// Local only: nothing here is sent as it is, and nothing here is a restoration
// mapping - a DerivedFact holds a value only in process memory; an OpRecord, a
// ComputeSnapshot and the ledger's counts hold symbols, fact keys, codes and
// counts, never a value.
namespace singularity::protected_api::papinian {
// PapinianGuardService.hpp: the operation's table and its rows. Declared, not
// included: that header includes this one.
struct OperationProtectedSymbolTable;
struct RequestMappingEntry;
} // namespace singularity::protected_api::papinian

namespace singularity::protected_api::papinian::compute {

namespace business_calendar = singularity::context::business_calendar;

// ---------------------------------------------------------------------------
// Closed codes (DESIGN4 §2.6, §3.2, §1.10). A class-S code depends only on the
// request, the declared types, the sent text and the budget counters; a
// class-O outcome depends on values and is always charged before evaluation.

enum class Code {
	None,
	BadJson,
	BadId,
	DuplicateId,
	BadLabel,
	BindReused,
	UnknownOp,
	Arity,
	UnknownSymbol,
	UnknownLabel,
	DependencyFailed,
	TypeMismatch,
	CurrencyMismatch,
	BasisMismatch,
	PerPeriod,
	UnitRequired,
	CalendarRequired,
	UnknownCalendar,
	RuleConflict,
	DefinitionNotFound,
	RuleUnknown,
	UnsafeLiteral,
	ConstantNotInMaterial,
	ConstantOnly,
	ConstantDepth,
	TooManyConstants,
	SameOperand,
	RoundedObservation,
	ExternalOperand,
	GroupKeyType,
	QueryLiteral,
	DepthExceeded,
	TooManyOps,
	RoundBudget,
	TaskBudget,
	DisclosureBudget,
	NotEvaluable,
	AmbiguousOperand,
	UnknownColumn,
	BadPredicate,
	TableTooLarge,
	CrossTable,
	WithheldByPolicy,
	PrecisionInsufficient,
	DivisionByZero,
	EmptySet,
	EvaluationFailed,
	// UNKNOWN reasons that are not errors of the request.
	BoundedOperand,
	MaterialChanged,
};

enum class CodeClass { Static, Observation };

// "bad_json", "type_mismatch" ... as the results block writes them.
[[nodiscard]] const char *to_string(Code code);
[[nodiscard]] std::optional<Code> code_from_string(std::string_view text);
// Class S or O (DESIGN4 §2.6); DependencyFailed takes its cause's class and is
// Static here.
[[nodiscard]] CodeClass class_of(Code code);

// How one operation was answered (DESIGN4 §1.5): a new symbol or a relation
// word (ok), a bound, an honest unknown, an error of the request, a refusal
// of the admission rules, or not served (past a budget).
enum class OpStatus { Ok, Bounded, Unknown, Error, Refused, NotServed };
[[nodiscard]] const char *to_string(OpStatus status);

// Which carrier brought the request (DESIGN4 §1.2).
enum class Carrier { FinalField, NativeTool, TextualTool };
[[nodiscard]] const char *to_string(Carrier carrier);

// ---------------------------------------------------------------------------
// The request as the model wrote it (DESIGN4 §2.1), in the SYMBOL plane:
// parsed from the provider's own bytes and never restored.

struct WhereAtom {
	std::string column;
	std::string test;
	std::optional<std::string> value;
	std::optional<std::string> value2;
};

struct OpOptions {
	std::optional<std::string> unit;
	std::optional<std::string> calendar;
	std::optional<int> places;
	std::optional<std::string> rounding;
	std::optional<std::string> fn;
	std::optional<std::string> mode;
	std::optional<bool> reveal;
	std::optional<std::string> anchor;
};

struct OpNode {
	std::string id;
	std::string op;
	std::vector<std::string> args;
	std::optional<std::vector<WhereAtom>> where;
	std::optional<OpOptions> options;
	std::optional<std::string> bind;
	std::optional<std::string> expect;
	std::string why;
};

// One argument read by the grammar of DESIGN4 §2.2.
enum class ArgKind { Symbol, Ref, Table, Column, Row, CellRef, Calendar, Schedule, Document, Constant };

struct Arg {
	ArgKind kind = ArgKind::Constant;
	// Exactly as written.
	std::string written;
	// Symbol: "[[Amount3]]" (canonical), with `part` for "[[Person5.first]]".
	// Table, Column, Row, CellRef: the table's handle ("Table1") in `symbol`.
	// Calendar: "[[Calendar1]]", "governing" or "calendar_days". Schedule:
	// "[[Schedule1]]". Document: "DOC_3".
	std::string symbol;
	std::string part;
	// Ref: the label after "@".
	std::string label;
	// Column and CellRef: the column letters; Row and CellRef: the row number.
	std::string column;
	std::size_t row = 0;
	// Constant: the text as written ("4%", "10 business days", "0").
	std::string constant;
};

// ---------------------------------------------------------------------------
// Types (DESIGN4 §2.4).

enum class TypeKind {
	Money,
	Number,
	Count,
	Percentage,
	Rate,
	Date,
	Duration,
	Boolean,
	Order,
	Sign,
	Schedule,
	Entity,
	Table,
	Column,
	Row,
	RowSet,
	GroupSet,
	Calendar,
	// day_class's closed word, list_rows's labels: a word, never a symbol.
	Word,
};

// The static type of an operand or a result, from declared types only - never
// from a value (DESIGN4 §8.4 L1).
struct ArgType {
	TypeKind kind = TypeKind::Number;
	// Money: the ISO code, "?" when unknown.
	std::string currency;
	// Money: the period an amount is owed for (MONEY(USD/month)); Rate: the
	// period a proportion is for; None otherwise.
	semantic::RateBasis basis = semantic::RateBasis::None;
	// Duration: its unit.
	semantic::DurationUnit unit = semantic::DurationUnit::Day;
	// Date: "day", "month", "quarter", "year", "day of year", "range".
	std::string precision;
	// Entity: "person", "organisation", "place", "identifier".
	std::string entity;
	// Column(T), Schedule(K->T), GroupSet(K): the element types as §2.4 writes them.
	std::string element;
	std::string key;
	// RowSet, Row, Column, GroupSet: the owning table's handle.
	std::string table;
	// Built through a calendar-anchored operation (DESIGN4 §7.2).
	bool calendar_anchored = false;
};

// As the legend and the results block write it: "MONEY(USD/month)", "RATE",
// "DATE(month)", "DURATION(business days)", "ROW_SET([[Table1]])".
[[nodiscard]] std::string written(const ArgType &type);

// What an operation returns: a type, and whether the answer is a relation word
// (TRUE/FALSE, LESS/EQUAL/GREATER, a sign) rather than a new symbol.
struct ResultType {
	ArgType type;
	bool relation = false;
};

// A deterministic type error (class S), naming types, never values.
struct TypeError {
	Code code = Code::TypeMismatch;
	// 1-based; 0 when it concerns the operation as a whole.
	std::size_t argument = 0;
	std::string expected;
	std::string got;
	std::string hint;
};

// ---------------------------------------------------------------------------
// Derived facts (DESIGN4 §4.1).

struct Certainty {
	enum class Kind { Exact, Bounded, Unknown };
	Kind kind = Kind::Exact;
	// Bounded: the fact keys of the two ends (each a derived fact of its own).
	std::string lo_fact_key;
	std::string hi_fact_key;
	// Unknown: why.
	Code reason = Code::None;
};

enum class Grounding { Material, Safe, Request, Lawyer };
[[nodiscard]] const char *to_string(Grounding grounding);

// One constant an operation used, and where the lawyer's material writes it.
struct ConstantUse {
	std::string value_key;
	Grounding grounding = Grounding::Safe;
	std::string passage_sha256;
	std::size_t offset = 0;
	std::size_t length = 0;
};

struct DerivedFact {
	// [[Amount12]]: request-scoped, never persisted.
	std::string placeholder;
	// fact_key_of(provenance): stable across the calls of the task.
	std::string fact_key;
	// OpRecord::op_id, or "planner:<relation id>" for a preload.
	std::string op_id;
	std::string op;
	// Canonical AST over operand FACT KEYS and constants' value keys:
	// "apply_rate(fk:3a91...,c:PCT:1/25,increase)".
	std::string ast;
	std::vector<std::string> operand_symbols;
	std::vector<std::string> operand_fact_keys;
	std::vector<ConstantUse> constants;
	// "MONEY(USD/month)" (DESIGN4 §2.4).
	std::string type;
	// The exact value (local only).
	semantic::Value value;
	Certainty certainty;
	// The model's bind, or the planner's role.
	std::string label;
	// Inherited role class when the op preserves it.
	std::string role;
	// Fact keys of every non-derived fact reached transitively, and where each
	// is stated.
	std::set<std::string> roots;
	std::vector<RelationSource> root_sources;
	// The operation (MatterTask id) and the model's why.
	std::string task;
	std::string subproblem;
	std::string value_sensitivity;
	std::string relation_sensitivity;
	unsigned depth = 0;
	unsigned constant_count = 0;
	// Contains round() (DESIGN4 §8.1 R8).
	bool scale_breaking = false;
	// Its value is a figure the lawyer's material writes openly (a band's rate
	// a lookup chose): no writing of it is masked on its account.
	bool stated_openly = false;
	enum class Disclosure { Derivation, ResultOnly };
	Disclosure disclosure = Disclosure::Derivation;
};

// ---------------------------------------------------------------------------
// The record of one operation (DESIGN4 §2.3): what the disclosure lists as
// "local operations requested" and "symbolic results returned". Never a value.

struct OpRecord {
	// "c<call>.r<round>.<qid>": unique in the operation.
	std::string op_id;
	std::size_t call = 0;
	std::size_t round = 0;
	std::size_t model_turn = 0;
	// "final_field", "native_tool" or "textual_tool".
	std::string carrier;
	// The op as parsed, symbol plane: op(args;where;options).
	std::string request_canonical;
	std::vector<std::string> operand_symbols;
	std::vector<std::string> operand_types;
	std::string expected_type;
	std::string declared_label;
	std::string justification;
	// ok | bounded | unknown | error | refused | not_served
	std::string status;
	std::string code;
	std::vector<std::string> results;
	// The line exactly as the model reads it (symbol plane).
	std::string result_line;
	// The relation word (TRUE, LESS ...) and the sign the answer gave, as the
	// executor wrote them: what this task already knows of an order.
	std::string relation;
	std::string sign;
	// Decided at admission from the AST (pre-charge), never from the outcome.
	bool observation = false;
	std::string observation_key;
	std::vector<std::string> charged_roots;
	std::vector<std::string> charged_group_keys;
	std::vector<std::string> operand_fact_keys;
	// The OutboundCall that carried result_line (set when that call is noted).
	std::string received_by_call;
};

// ---------------------------------------------------------------------------
// Observations and their budget (DESIGN4 §8.3). The matter-lifetime counts
// are loaded from and saved to the Matter (its "observationLedger"); counts,
// canonical keys and the order the relations told, over fact keys only - no
// value.

struct ObservationCounts {
	std::map<std::string, std::size_t> by_root;
	std::map<std::string, std::size_t> by_calendar_root;
	std::map<std::string, std::size_t> by_group_key;
	// Canonical observation keys already answered: asked again, free.
	std::set<std::string> answered;
	// Canonical divisors whose zero test was spent (one per life).
	std::set<std::string> zero_divisors;
	// How much of each root's count a preload spent (the rest was asked for).
	std::map<std::string, std::size_t> preload_by_root;
	// What the relations told so far say of two facts' order, over their keys:
	// "lt|a|b" (a is below or before b), "le|a|b" (a is not above or after b).
	// What they imply is known, and asking it again is free (owner, 27 Sep 2026).
	std::set<std::string> order_edges;
};

// Who asks for a disclosure (owner, 28 Sep 2026): an operation the lawyer's
// task needs - asked by the model, or by a completeness repair - comes first;
// a preload (the relations planner, the legend's timeline) only spends what
// is left beyond kPreload*, and never the last of it.
enum class DisclosurePriority { OnDemand, Preload };

// What one task disclosed and held back, for the benchmark report and the
// operation's events: counts, fact keys (digests, never values) and codes.
struct DisclosureDiagnostics {
	std::size_t preload_proposed = 0;
	std::size_t preload_sent = 0;
	// Sent free: told before, or implied by what was told.
	std::size_t preload_free = 0;
	// Not sent: no intent or fact of the task asks for it.
	std::size_t preload_skipped_irrelevant = 0;
	// Not sent: it would have spent capacity an operation may need.
	std::size_t preload_skipped_reserve = 0;
	std::size_t on_demand_charged = 0;
	std::size_t on_demand_free = 0;
	std::size_t on_demand_refused = 0;
	// Each disclosure_budget refusal: "q6 root 4c72f439fa12e8be 4/4 (preload 2)".
	std::vector<std::string> refusals;
	// Roots this task brought to the lifetime limit.
	std::set<std::string> roots_at_limit;
	// The preload keys already counted: every call of a task plans again, and
	// a line is counted once, where it was first proposed.
	std::set<std::string> preload_seen;
};

// What one operation is charged, decided from its AST before evaluation.
struct ObservationCharge {
	std::string key;
	std::vector<std::string> roots;
	std::vector<std::string> calendar_roots;
	std::vector<std::string> group_keys;
	std::vector<std::string> zero_divisors;
	// Charges per root beyond one (list_rows, lookups: ceil(log2(n+1))).
	std::size_t weight = 1;
};

// The ledger's logic is ProtectedOperations/ObservationLedger.cpp's.
struct ObservationLedger {
	// For the life of the matter.
	ObservationCounts matter;
	// For this task.
	std::size_t task_observations = 0;
	std::map<std::string, std::size_t> task_by_root;
	std::map<std::string, std::size_t> task_by_group_key;
	DisclosureDiagnostics diagnostics;
	// What a preload must leave of every root: the heaviest single charge an
	// operation of this task could need of one - ranking a bound among the
	// rows of the attempt's longest table (list_rows, ceil(log2(N+1))) - and at
	// least what kPreloadObservationsPerRoot leaves (live EN1 c24: the timeline
	// took two of the notice date's four, and listing the 24-month window,
	// which ranks it among thirteen rows at once, was refused).
	std::size_t preload_keep = kObservationsPerRoot - kPreloadObservationsPerRoot;

	// Whether `key` was answered before, in this task or the matter.
	[[nodiscard]] bool answered(const std::string &key) const;
	// Whether what was told already settles how fact `a` stands to fact `b`
	// for `relation` ("compare", "less_than", "before", "equal" ...: the op's
	// name; "compare" for a three-way order).
	[[nodiscard]] bool implied(const std::string &a, const std::string &b, const std::string &relation) const;
	// Keeps what a relation told ("lt|a|b", "le|a|b").
	void learn(const std::vector<std::string> &edges);
	// Code::None when `charge` fits every limit - for a preload, every limit
	// less the capacity kept for operations; DisclosureBudget otherwise.
	[[nodiscard]] Code admit(const ObservationCharge &charge, DisclosurePriority priority = DisclosurePriority::OnDemand) const;
	// Pays `charge` (before evaluation) and records its key.
	void charge(const ObservationCharge &charge, DisclosurePriority priority = DisclosurePriority::OnDemand);
	// Which limit refuses `charge`, in fact keys and counts: "root <key> 4/4
	// (preload 2)", "task 40/40", "group <key> 12/12", "calendar <key> 4/4".
	[[nodiscard]] std::string refusal(const ObservationCharge &charge,
		DisclosurePriority priority = DisclosurePriority::OnDemand) const;
};

// The order a relation answered ok tells of its two facts, as ledger edges
// ("lt|a|b", "le|a|b"): compare, subtract's sign, greater_than ... same_time.
[[nodiscard]] std::vector<std::string> order_edges(const OpRecord &record);

// The task's disclosure diagnostics as event details ("disclosure..." keys):
// counts, fact-key digests and codes, never a value.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> disclosure_details(const ObservationLedger &ledger);

// ---------------------------------------------------------------------------
// Calendars (DESIGN4 §7.3): each business-day rule the material states, as a
// handle the model names ([[Calendar1]]). Not a fact: no value hides behind it.

struct CalendarHandle {
	// "Calendar1".
	std::string handle;
	std::optional<business_calendar::BusinessDayRule> rule;
	// The line the calendars block lists, closed words only.
	std::string line;
	// Whether it governs the documents it names, and which those are.
	bool governing = false;
	std::vector<std::string> documents;
};

// ---------------------------------------------------------------------------
// Payload segments (DESIGN4 §8.2): what kind of text each part of a payload
// is. Only source, request and lawyer_answer ground a constant.

struct PayloadSegment {
	enum class Kind { Source, Request, LawyerAnswer, ModelAuthored, LmBlock, Control };
	Kind kind = Kind::Source;
	std::size_t start = 0;
	std::size_t length = 0;
	// The passage it is, when it is one.
	std::string passage_sha256;
};
[[nodiscard]] const char *to_string(PayloadSegment::Kind kind);

// ---------------------------------------------------------------------------
// The dispatch interface (DESIGN4 §3.1, §13.1): each family registers its
// operations - a typer and an evaluator each - and the executor dispatches.

// One resolved operand, as an evaluator reads it.
struct Operand {
	Arg arg;
	ArgType type;
	// The registry row a symbol names, or the derived fact it is.
	const RequestMappingEntry *row = nullptr;
	const DerivedFact *derived = nullptr;
	// The exact value (local), when the operand has one.
	std::optional<semantic::Value> value;
	std::string fact_key;
	std::set<std::string> roots;
	unsigned depth = 0;
	// A table, a row set, a group set, a schedule or a calendar.
	const tables::ProtectedTable *table = nullptr;
	std::optional<tables::RowSet> rows;
	std::optional<tables::GroupSet> groups;
	const tables::ProtectedSchedule *schedule = nullptr;
	const CalendarHandle *calendar = nullptr;
};

// What an evaluator gives back. Values stay here: the executor issues the
// symbols and writes the lines.
struct OpAnswer {
	OpStatus status = OpStatus::Ok;
	Code code = Code::None;
	TypeError error;
	ResultType result;
	// ok with a symbol: the value; bounded: both ends.
	std::optional<semantic::Value> value;
	std::optional<semantic::Value> lo;
	std::optional<semantic::Value> hi;
	Certainty certainty;
	// ok with a relation: "TRUE", "LESS", "POSITIVE" ...; a subtraction's sign.
	std::string relation;
	std::string sign;
	std::optional<tables::RowSet> rows;
	std::optional<tables::GroupSet> groups;
	// aggregate: key -> value, in first-row order.
	std::vector<std::pair<std::string, semantic::Value>> pairs;
	// Closed words a line carries (day_class, bounded wording, calendar note).
	std::string words;
	std::set<std::string> roots;
	std::vector<RelationSource> root_sources;
	// Issued by the executor.
	std::vector<std::string> results;
	std::string line;
	std::string local_line;
	OutboundRelation record;
};

// What an operation runs with: the operation's table, how to issue a derived
// fact, the sent segments, the ledger, the rules and Singularity's own words.
struct OpContext {
	OperationProtectedSymbolTable *table = nullptr;
	// The op being run (its where-atoms, id and why).
	const OpNode *node = nullptr;
	std::function<const RequestMappingEntry *(std::string_view)> row_of;
	// Issues a derived fact into the table (PapinianGuard::issue_derived_fact)
	// and returns its symbol.
	std::function<std::string(DerivedFact &)> issue;
	// The symbol of a table cell's fact (PapinianGuard::issue_cell_fact).
	std::function<std::string(const tables::ProtectedTable &, const tables::TableCell &)> issue_cell;
	std::function<std::string(const RequestMappingEntry &)> category;
	std::function<std::string(const RequestMappingEntry &)> origin;
	std::function<RelationSensitivity(const RelationFacts &)> classify;
	std::function<bool(const RelationFacts &)> permit;
	std::function<std::string(const RequestMappingEntry &)> party_of;
	// The payload as sent (symbols only) and what each part of it is.
	std::string sent;
	std::vector<PayloadSegment> segments;
	ObservationLedger *ledger = nullptr;
	business_calendar::GoverningRules rules;
	std::function<business_calendar::GoverningRules(const std::vector<std::string> &)> rules_for;
	std::vector<std::string> raw_passages;
	semantic::Language language = semantic::Language::English;
	std::function<std::string(singularity::context::control_text::Template,
		const std::vector<singularity::context::control_text::Slot> &)> render;
	// Where the request came from.
	std::size_t call = 0;
	std::size_t round = 0;
	std::size_t model_turn = 0;
	Carrier carrier = Carrier::FinalField;
	std::string task;
};

using Typer = std::optional<ResultType> (*)(const std::vector<ArgType> &, const OpOptions &, TypeError &);
using Evaluator = OpAnswer (*)(const std::vector<Operand> &, const OpOptions &, OpContext &);
// The charge an operation pays before evaluation (DESIGN4 §8.3), decided from
// its operands' declared types, roots and the AST; empty when it observes
// nothing.
using PreCharge = std::optional<ObservationCharge> (*)(const std::vector<Operand> &, const OpOptions &,
	const OpContext &);

// Never: always a symbol (class S). Always: every outcome is a relation or a
// status (class O). Conditional: decided by the pre-charge (a subtraction's
// sign unless reveal:false; a bounded status).
enum class ObservationClass { Never, Always, Conditional };

struct OpEntry {
	std::string name;
	Typer typer = nullptr;
	Evaluator evaluator = nullptr;
	ObservationClass observation = ObservationClass::Never;
	PreCharge pre_charge = nullptr;
};

// Registers `entries` under `family`; a later registration of the same name
// replaces the earlier one. Thread-safe.
void register_family(std::string_view family, std::vector<OpEntry> entries);
// The entry for an op name, or nullptr.
[[nodiscard]] const OpEntry *find_op(std::string_view op);
// Every registered family's name, in registration order.
[[nodiscard]] std::vector<std::string> registered_families();
// Registers the three families once (idempotent): arithmetic, temporal, tables.
void ensure_families_registered();

// Each family's registration, defined in its own file.
void register_arithmetic_family();  // ProtectedOperations/ArithmeticOps.cpp (lane X)
void register_temporal_family();    // ProtectedOperations/TemporalOps.cpp (lane C)
void register_table_family();       // ProtectedOperations/TableOps.cpp (lane T)

// ---------------------------------------------------------------------------
// Pause and resume (DESIGN4 §1.10): value-free, in the pause's loop JSON.

struct SnapshotOp {
	std::string op_id;
	std::string request_canonical;
	std::vector<std::string> operand_fact_keys;
	std::string status;
	std::string code;
	std::string declared_label;
	std::vector<std::string> result_fact_keys;
};

struct ComputeSnapshot {
	std::vector<SnapshotOp> op_log;
	// bind label -> fact key.
	std::map<std::string, std::string> labels;
	// The task's counts (the matter's are persisted in the Matter).
	std::size_t task_observations = 0;
	std::map<std::string, std::size_t> task_by_root;
	std::map<std::string, std::size_t> task_by_group_key;
	std::set<std::string> answered;
	std::size_t compute_rounds_used = 0;
	std::size_t ops_used = 0;
	std::size_t repair_rounds_used = 0;
	std::string provider_chain;
};

// ProtectedOperations/ObservationLedger.cpp (lane X).
[[nodiscard]] ComputeSnapshot snapshot(const OperationProtectedSymbolTable &table);
// Re-executes the snapshot's op log locally over the re-read registry; counts
// are restored, never recomputed. Returns the labels that could not be
// rebuilt (the material changed).
std::vector<std::string> restore_snapshot(OperationProtectedSymbolTable &table, const ComputeSnapshot &snapshot,
	OpContext &context);

// ---------------------------------------------------------------------------
// One round as the research loop hands it to the compute executor
// (ResearchToolLoop's `computeExecutor`), and what comes back.

struct ComputeRoundInput {
	// The `{"ops":[...]}` object exactly as the provider wrote it (symbol
	// plane, never restored).
	std::string ops_json;
	Carrier carrier = Carrier::FinalField;
	std::size_t call = 0;
	std::size_t round = 0;
	std::size_t model_turn = 0;
	// The loop's prompt as it now stands (local text).
	std::string prompt;
	// The completeness check before a final answer is taken (owner, 27 Sep
	// 2026): nothing is executed; `final_text` is that answer as the provider
	// wrote it (symbol plane), and a typed repair comes back in local_block.
	bool completeness_check = false;
	std::string final_text;
	// Whether this answer may still ask the lawyer on the question card
	// (lawyerQuestion): where it may not, what Singularity may not disclose is put
	// to the lawyer in the answer itself.
	bool lawyer_question_offered = false;
	// Whether an operation the answer still needs can be worked out for it:
	// else what one would settle goes to the lawyer.
	bool operations_open = true;
};

struct ComputeRoundOutput {
	// The results block as the loop's prompt carries it (local anchors).
	std::string local_block;
	std::size_t ops = 0;
	std::size_t ok = 0;
	std::size_t bounded = 0;
	std::size_t unknown = 0;
	std::size_t error = 0;
	std::size_t refused = 0;
	std::size_t observations = 0;
	// Whether any op was answered ok, bounded or unknown (else a repair round).
	bool answered = false;
	// The call or task budget is spent.
	bool closed = false;
	// The task's disclosure diagnostics after this round (disclosure_details).
	std::vector<std::pair<std::string, std::string>> disclosure;
};

} // namespace singularity::protected_api::papinian::compute
