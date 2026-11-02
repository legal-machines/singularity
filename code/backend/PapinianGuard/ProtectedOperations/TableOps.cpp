#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"
#include "PapinianGuard/ProtectedOperations/TemporalOps.hpp"
#include "PapinianGuard/ProtectedTables/FormulaEval.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The table family (DESIGN4 §2.5, §6): filter, count, group, aggregate,
// sum_column, average_column, min_column, max_column, derive_column, union,
// intersect, minus, lookup, progressive, cell, list_rows, recalc,
// describe_tables - each a typer and an evaluator over ProtectedTables,
// registered through register_family.
//
// What the model learns from a table op is decided before anything is read:
// a row set is an opaque [[Rows1]] whatever its members; a sum, a count and a
// minimum are new symbols; what can depend on values - a bound, an empty set,
// the rows listed, a partition, a band, whether a saved total still holds - is
// an observation, charged by the pre-charge from the request and the table's
// structure alone (DESIGN4 §8.3). Class-S errors name types, codes and
// letters, never a value.
namespace singularity::protected_api::papinian::compute {
namespace {

namespace tables = singularity::protected_api::papinian::tables;
using semantic::ExactRational;
using semantic::Kind;
using semantic::Rational;
using semantic::Value;
namespace temporal = singularity::context::temporal;

// ---------------------------------------------------------------------------
// Answers

OpAnswer error_answer(Code code, std::size_t argument = 0, std::string hint = {}, std::string expected = {},
		std::string got = {}) {
	OpAnswer answer;
	answer.status = code == Code::UnsafeLiteral || code == Code::ConstantNotInMaterial ? OpStatus::Refused : OpStatus::Error;
	answer.code = code;
	answer.error.code = code;
	answer.error.argument = argument;
	answer.error.hint = std::move(hint);
	answer.error.expected = std::move(expected);
	answer.error.got = std::move(got);
	return answer;
}

OpAnswer unknown_answer(Code reason) {
	OpAnswer answer;
	answer.status = OpStatus::Unknown;
	answer.code = reason;
	answer.certainty.kind = Certainty::Kind::Unknown;
	answer.certainty.reason = reason;
	return answer;
}

std::optional<ResultType> type_error(TypeError &error, Code code, std::size_t argument, std::string expected = {},
		std::string got = {}) {
	error.code = code;
	error.argument = argument;
	error.expected = std::move(expected);
	error.got = std::move(got);
	return std::nullopt;
}

ResultType result_of(ArgType type, bool relation = false) {
	ResultType result;
	result.type = std::move(type);
	result.relation = relation;
	return result;
}

// ---------------------------------------------------------------------------
// Types

bool numeric(const ArgType &type) {
	switch (type.kind) {
		case TypeKind::Money:
		case TypeKind::Number:
		case TypeKind::Count:
		case TypeKind::Percentage:
		case TypeKind::Rate:
		case TypeKind::Duration:
			return true;
		default:
			return false;
	}
}

bool ordered(const ArgType &type) { return numeric(type) || type.kind == TypeKind::Date; }

bool set_like(const ArgType &type) { return type.kind == TypeKind::RowSet || type.kind == TypeKind::Table; }

std::optional<ArgType> element_of(const ArgType &column) {
	if (column.kind != TypeKind::Column) return std::nullopt;
	return tables::parse_type(column.element);
}

// A sum over a column owed per period is the total (DESIGN4 §2.5).
ArgType total_of(ArgType element) {
	if (element.kind == TypeKind::Money) element.basis = semantic::RateBasis::None;
	return element;
}

ArgType count_type() {
	ArgType type;
	type.kind = TypeKind::Count;
	return type;
}

ArgType word_type() {
	ArgType type;
	type.kind = TypeKind::Word;
	return type;
}

bool same_table(const ArgType &a, const ArgType &b) { return a.table.empty() || b.table.empty() || a.table == b.table; }

// ---------------------------------------------------------------------------
// Typers (DESIGN4 §2.5, the table rows)

std::optional<ResultType> type_filter(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() != 1) return type_error(error, Code::Arity, 0);
	if (!set_like(args[0])) return type_error(error, Code::TypeMismatch, 1, "TABLE or ROW_SET", written(args[0]));
	ArgType type = args[0];
	type.kind = TypeKind::RowSet;
	return result_of(type);
}

std::optional<ResultType> type_count(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() != 1) return type_error(error, Code::Arity, 0);
	if (!set_like(args[0]) && args[0].kind != TypeKind::GroupSet)
		return type_error(error, Code::TypeMismatch, 1, "ROW_SET or GROUP_SET", written(args[0]));
	return result_of(count_type());
}

std::optional<ResultType> type_column_aggregate(const std::vector<ArgType> &args, TypeError &error, bool orderedOnly,
		bool total, bool average) {
	if (args.size() != 2) return type_error(error, Code::Arity, 0);
	if (!set_like(args[0])) return type_error(error, Code::TypeMismatch, 1, "ROW_SET", written(args[0]));
	const std::optional<ArgType> element = element_of(args[1]);
	if (!element) return type_error(error, Code::TypeMismatch, 2, "COLUMN", written(args[1]));
	if (!same_table(args[0], args[1])) return type_error(error, Code::CrossTable, 2);
	if (orderedOnly ? !ordered(*element) : !numeric(*element))
		return type_error(error, Code::TypeMismatch, 2, orderedOnly ? "COLUMN(ordered)" : "COLUMN(numeric)", written(args[1]));
	ArgType result = total ? total_of(*element) : *element;
	if (average && result.kind == TypeKind::Count) result.kind = TypeKind::Number;
	return result_of(result);
}

std::optional<ResultType> type_sum_column(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	return type_column_aggregate(args, error, false, true, false);
}

std::optional<ResultType> type_average_column(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	return type_column_aggregate(args, error, false, false, true);
}

std::optional<ResultType> type_min_max_column(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	return type_column_aggregate(args, error, true, false, false);
}

std::optional<ResultType> type_derive_column(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() < 3) return type_error(error, Code::Arity, 0);
	if (!set_like(args[0])) return type_error(error, Code::TypeMismatch, 1, "ROW_SET", written(args[0]));
	// The cells' type is the named op's own typing, worked out by the
	// evaluator over the columns' element types.
	ArgType column;
	column.kind = TypeKind::Column;
	column.table = args[0].table;
	return result_of(column);
}

bool date_unit(const std::optional<std::string> &unit) {
	return unit && (*unit == "months" || *unit == "quarters" || *unit == "years");
}

std::optional<ResultType> type_group(const std::vector<ArgType> &args, const OpOptions &options, TypeError &error) {
	if (args.size() != 2) return type_error(error, Code::Arity, 0);
	if (!set_like(args[0])) return type_error(error, Code::TypeMismatch, 1, "ROW_SET", written(args[0]));
	const std::optional<ArgType> key = element_of(args[1]);
	if (!key) return type_error(error, Code::TypeMismatch, 2, "COLUMN", written(args[1]));
	if (!same_table(args[0], args[1])) return type_error(error, Code::CrossTable, 2);
	if (key->kind == TypeKind::Date) {
		if (!date_unit(options.unit)) return type_error(error, Code::UnitRequired, 2);
	} else if (key->kind != TypeKind::Entity && key->kind != TypeKind::Word) {
		return type_error(error, Code::GroupKeyType, 2, "COLUMN(ENTITY, WORD or DATE)", written(args[1]));
	}
	ArgType type;
	type.kind = TypeKind::GroupSet;
	type.table = args[0].table;
	type.key = written(*key);
	return result_of(type);
}

std::optional<ResultType> type_aggregate(const std::vector<ArgType> &args, const OpOptions &options, TypeError &error) {
	if (args.size() != 2) return type_error(error, Code::Arity, 0);
	if (args[0].kind != TypeKind::GroupSet) return type_error(error, Code::TypeMismatch, 1, "GROUP_SET", written(args[0]));
	const std::optional<ArgType> element = element_of(args[1]);
	if (!element) return type_error(error, Code::TypeMismatch, 2, "COLUMN", written(args[1]));
	if (!same_table(args[0], args[1])) return type_error(error, Code::CrossTable, 2);
	const std::string fn = options.fn.value_or("");
	if (fn == "count") return result_of(count_type());
	if (fn == "sum" || fn == "average") {
		if (!numeric(*element)) return type_error(error, Code::TypeMismatch, 2, "COLUMN(numeric)", written(args[1]));
		ArgType result = fn == "sum" ? total_of(*element) : *element;
		if (fn == "average" && result.kind == TypeKind::Count) result.kind = TypeKind::Number;
		return result_of(result);
	}
	if (fn == "min" || fn == "max") {
		if (!ordered(*element)) return type_error(error, Code::TypeMismatch, 2, "COLUMN(ordered)", written(args[1]));
		return result_of(*element);
	}
	return type_error(error, Code::Arity, 0, "options.fn sum|count|min|max|average");
}

std::optional<ResultType> type_set_algebra(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() != 2) return type_error(error, Code::Arity, 0);
	for (std::size_t index = 0; index < 2; ++index)
		if (!set_like(args[index])) return type_error(error, Code::TypeMismatch, index + 1, "ROW_SET", written(args[index]));
	if (!same_table(args[0], args[1])) return type_error(error, Code::CrossTable, 2);
	ArgType type = args[0];
	type.kind = TypeKind::RowSet;
	return result_of(type);
}

std::optional<ResultType> type_lookup(const std::vector<ArgType> &args, const OpOptions &options, TypeError &error) {
	if (options.mode && *options.mode != "le" && *options.mode != "ge")
		return type_error(error, Code::TypeMismatch, 0, "mode le|ge", *options.mode);
	if (args.size() == 2 && args[0].kind == TypeKind::Schedule) {
		const std::optional<ArgType> key = tables::parse_type(args[0].key);
		const bool proportion = key && (key->kind == TypeKind::Percentage || key->kind == TypeKind::Rate)
			&& (args[1].kind == TypeKind::Percentage || args[1].kind == TypeKind::Rate);
		if (key && key->kind != args[1].kind && !proportion)
			return type_error(error, Code::TypeMismatch, 2, written(*key), written(args[1]));
		const std::optional<ArgType> value = tables::parse_type(args[0].element);
		if (!value) return type_error(error, Code::TypeMismatch, 1, "SCHEDULE", written(args[0]));
		return result_of(*value);
	}
	if (args.size() != 4) return type_error(error, Code::Arity, 0);
	if (args[0].kind != TypeKind::Table) return type_error(error, Code::TypeMismatch, 1, "TABLE", written(args[0]));
	const std::optional<ArgType> key = element_of(args[1]);
	const std::optional<ArgType> result = element_of(args[3]);
	if (!key) return type_error(error, Code::TypeMismatch, 2, "COLUMN", written(args[1]));
	if (!result) return type_error(error, Code::TypeMismatch, 4, "COLUMN", written(args[3]));
	if (!same_table(args[0], args[1]) || !same_table(args[0], args[3])) return type_error(error, Code::CrossTable, 2);
	if (!ordered(*key)) return type_error(error, Code::TypeMismatch, 2, "COLUMN(ordered)", written(args[1]));
	if (key->kind != args[2].kind && !(numeric(*key) && numeric(args[2]) && key->kind != TypeKind::Money
			&& args[2].kind != TypeKind::Money))
		return type_error(error, Code::TypeMismatch, 3, written(*key), written(args[2]));
	return result_of(*result);
}

std::optional<ResultType> type_progressive(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() != 2) return type_error(error, Code::Arity, 0);
	if (args[1].kind != TypeKind::Schedule) return type_error(error, Code::TypeMismatch, 2, "SCHEDULE", written(args[1]));
	if (args[0].kind != TypeKind::Money && args[0].kind != TypeKind::Number && args[0].kind != TypeKind::Count)
		return type_error(error, Code::TypeMismatch, 1, "MONEY or NUMBER", written(args[0]));
	const std::optional<ArgType> rate = tables::parse_type(args[1].element);
	if (!rate || (rate->kind != TypeKind::Percentage && rate->kind != TypeKind::Rate))
		return type_error(error, Code::TypeMismatch, 2, "SCHEDULE(→PERCENTAGE)", written(args[1]));
	ArgType result = args[0];
	if (result.kind == TypeKind::Count) result.kind = TypeKind::Number;
	return result_of(result);
}

// A cell of words named where a figure is wanted (live EN2: "cell(... B12)"
// asked seven times of a cell reading "Specified warranty", told only its op).
constexpr const char *kWordsCellHint = "that cell holds words, not a figure: name the cell of the figure itself - its "
	"row and a column whose type in the tables block is an amount, a rate, a date or a count";

std::optional<ResultType> type_cell(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.empty() || args.size() > 2) return type_error(error, Code::Arity, 0);
	if (args.size() == 2 && args[0].kind != TypeKind::Table)
		return type_error(error, Code::TypeMismatch, 1, "TABLE", written(args[0]));
	const ArgType &cell = args.back();
	if (!numeric(cell) && cell.kind != TypeKind::Date && cell.kind != TypeKind::Boolean) {
		type_error(error, Code::TypeMismatch, args.size(), "NUMBER", written(cell));
		error.hint = kWordsCellHint;
		return std::nullopt;
	}
	return result_of(cell);
}

std::optional<ResultType> type_list_rows(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() != 1) return type_error(error, Code::Arity, 0);
	if (!set_like(args[0])) return type_error(error, Code::TypeMismatch, 1, "ROW_SET", written(args[0]));
	return result_of(word_type(), true);
}

std::optional<ResultType> type_recalc(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.empty() || args.size() > 2) return type_error(error, Code::Arity, 0);
	const ArgType &cell = args.back();
	if (!numeric(cell) && cell.kind != TypeKind::Date) {
		type_error(error, Code::TypeMismatch, args.size(), "NUMBER", written(cell));
		error.hint = kWordsCellHint;
		return std::nullopt;
	}
	return result_of(cell);
}

std::optional<ResultType> type_describe_tables(const std::vector<ArgType> &args, const OpOptions &, TypeError &error) {
	if (args.size() != 1) return type_error(error, Code::Arity, 0);
	if (args[0].kind != TypeKind::Word || args[0].element != "document")
		return type_error(error, Code::TypeMismatch, 1, "DOC_n", written(args[0]));
	return result_of(word_type(), true);
}

// ---------------------------------------------------------------------------
// Resolving the operands an evaluator reads

// An operand as a table op reads it: the executor resolves table arguments
// through tables::resolve_arg; one it left unresolved is resolved here.
Operand settled(const Operand &given, const OpContext &context) {
	if (given.table != nullptr || given.rows || given.groups || given.schedule != nullptr || context.table == nullptr)
		return given;
	Operand resolved = given;
	TypeError ignored;
	ignored.code = Code::None;
	Operand fresh;
	if (tables::resolve_arg(*context.table, given.arg, fresh, ignored) && ignored.code == Code::None) {
		fresh.value = fresh.value ? fresh.value : given.value;
		if (fresh.fact_key.empty()) fresh.fact_key = given.fact_key;
		if (fresh.roots.empty()) fresh.roots = given.roots;
		fresh.row = fresh.row != nullptr ? fresh.row : given.row;
		fresh.derived = fresh.derived != nullptr ? fresh.derived : given.derived;
		return fresh;
	}
	return resolved;
}

tables::ProtectedTable *mutable_table(OpContext &context, const std::string &handle) {
	return context.table == nullptr ? nullptr : tables::find_table(*context.table, handle);
}

// The set a set-like operand stands for: its rows, or every body row of a table.
std::optional<tables::RowSet> set_of(const Operand &operand) {
	if (operand.rows) return operand.rows;
	if (operand.table == nullptr) return std::nullopt;
	tables::RowSet all;
	all.table = operand.table->handle;
	for (const tables::TableRow &row : operand.table->rows)
		if (row.kind == tables::RowKind::Body) all.certain.push_back(row.number);
	all.structural = true;
	all.base_rows = all.certain;
	all.base_size = all.certain.size();
	all.canonical = "table:" + tables::place_key(*operand.table);
	return all;
}

// A column operand: a letter of the table, or a derived column (@label).
struct ColumnRef {
	const tables::ProtectedTable *table = nullptr;
	const tables::TableColumn *column = nullptr;
	const tables::DerivedColumn *derived = nullptr;
	std::string name;
	std::optional<ArgType> element;
};

ColumnRef column_ref(const Operand &operand) {
	ColumnRef ref;
	ref.table = operand.table;
	if (ref.table == nullptr) return ref;
	if (operand.arg.kind == ArgKind::Column) {
		ref.column = tables::column_of(*ref.table, operand.arg.column);
		ref.name = operand.arg.column;
		if (ref.column != nullptr) ref.element = tables::parse_type(ref.column->type);
	} else if (operand.arg.kind == ArgKind::Ref || operand.arg.kind == ArgKind::Symbol) {
		const std::string label = operand.arg.kind == ArgKind::Ref ? "@" + operand.arg.label : operand.arg.symbol;
		ref.derived = tables::derived_column(*ref.table, label);
		ref.name = label;
		if (ref.derived != nullptr) ref.element = tables::parse_type(ref.derived->type);
	}
	return ref;
}

// One row's value in a column, and the fact keys it rests on. Nothing for a
// blank; `undetermined` when a derived row could not be worked out.
struct RowValue {
	std::optional<Value> value;
	std::set<std::string> roots;
	// Of `roots`, those every row shares (a derived column's scalar operands):
	// a filter ranks them among the rows rather than reading them per row.
	std::set<std::string> operand_roots;
	std::vector<RelationSource> sources;
	bool blank = true;
	bool undetermined = false;
	const tables::TableCell *cell = nullptr;
};

RowValue value_at(const ColumnRef &ref, std::size_t row) {
	RowValue read;
	if (ref.table == nullptr) return read;
	if (ref.derived != nullptr) {
		if (const auto found = ref.derived->values.find(row); found != ref.derived->values.end()) {
			read.value = found->second;
			read.blank = false;
		}
		read.undetermined = ref.derived->undetermined.count(row) != 0;
		if (const auto roots = ref.derived->roots.find(row); roots != ref.derived->roots.end()) read.roots = roots->second;
		// Whether the row has a value at all rests on the set it was worked out
		// over: that set's cells and operands are this row's roots too.
		read.roots.insert(ref.derived->base_cell_roots.begin(), ref.derived->base_cell_roots.end());
		read.roots.insert(ref.derived->operand_roots.begin(), ref.derived->operand_roots.end());
		read.operand_roots = ref.derived->operand_roots;
		if (const auto sources = ref.derived->sources.find(row); sources != ref.derived->sources.end()) read.sources = sources->second;
		return read;
	}
	if (ref.column == nullptr) return read;
	const tables::TableCell *cell = tables::cell_of(*ref.table, row, ref.name);
	if (cell == nullptr) return read;
	read.cell = cell;
	read.roots.insert(tables::cell_root(*ref.table, *cell));
	read.sources.push_back(cell->source);
	if (cell->value) {
		read.value = cell->value;
		read.blank = false;
	} else if (!cell->text.empty() || cell->kind == tables::CellKind::Error) {
		read.blank = false;
	}
	return read;
}

// Whether the rows `set` holds were already disclosed (list_rows answered for
// it): a figure worked out over those rows then says nothing more about what
// decided them, and owes none of the set's own roots again - an implied
// relation is free (owner, 27 Sep 2026). Live EN1: listing the audit window
// spent the notice date's budget, and every total over the window after it -
// the underpayment against the 5% threshold - was refused.
bool membership_disclosed(const tables::RowSet &set, const OpContext &context) {
	return context.ledger != nullptr && !set.canonical.empty() && context.ledger->answered("list_rows(" + set.canonical + ")");
}

std::vector<std::size_t> members(const tables::RowSet &set) {
	std::vector<std::size_t> rows = set.certain;
	rows.insert(rows.end(), set.undetermined.begin(), set.undetermined.end());
	std::sort(rows.begin(), rows.end());
	rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
	return rows;
}

// The rows a set's predicates were read over, decided without a value (see
// RowSet::base_rows); a set built before it was kept falls back to its members.
std::vector<std::size_t> base_of(const tables::RowSet &set) {
	if (!set.base_rows.empty()) return set.base_rows;
	return members(set);
}

bool contains(const std::vector<std::size_t> &rows, std::size_t row) {
	return std::find(rows.begin(), rows.end(), row) != rows.end();
}

void add_roots(std::vector<std::string> &into, const std::set<std::string> &roots) {
	for (const std::string &root : roots)
		if (std::find(into.begin(), into.end(), root) == into.end()) into.push_back(root);
}

// ---------------------------------------------------------------------------
// Exact sums over cells (ExactRational, DESIGN4 §3.2)

std::optional<ExactRational> exact_of(const Value &value) {
	switch (value.kind) {
		case Kind::Money:
		case Kind::Number:
		case Kind::Percentage:
		case Kind::Duration:
		case Kind::Count:
			return ExactRational::from(value.amount);
		default:
			return std::nullopt;
	}
}

// A value of `type` holding `amount`, or nothing when the amount does not fit
// what a value holds (it never happens inside the static bounds).
std::optional<Value> value_of(const ArgType &type, const ExactRational &amount, int decimals = 0) {
	const std::optional<Rational> fitted = amount.to_rational();
	if (!fitted) return std::nullopt;
	switch (type.kind) {
		case TypeKind::Money: {
			Value money = semantic::make_money(*fitted, type.currency == "?" ? std::string() : type.currency, decimals);
			money.basis = type.basis;
			return money;
		}
		case TypeKind::Percentage:
		case TypeKind::Rate: return semantic::make_percentage(*fitted, type.basis);
		case TypeKind::Duration: return semantic::make_duration(*fitted, type.unit);
		case TypeKind::Count: {
			Value count = semantic::make_number(*fitted);
			count.kind = Kind::Count;
			return count;
		}
		default: return semantic::make_number(*fitted);
	}
}

std::string sign_word(const ExactRational &amount) {
	const int sign = amount.sign();
	return sign == 0 ? "ZERO" : sign > 0 ? "POSITIVE" : "NEGATIVE";
}

bool revealed(const OpOptions &options) { return !options.reveal || *options.reveal; }

// ---------------------------------------------------------------------------
// filter (DESIGN4 §6.5)

enum class Truth { True, False, Undetermined };

Truth from(semantic::Truth truth) {
	return truth == semantic::Truth::True ? Truth::True : truth == semantic::Truth::False ? Truth::False : Truth::Undetermined;
}

Truth negate(Truth truth) { return truth == Truth::True ? Truth::False : truth == Truth::False ? Truth::True : truth; }

// ASCII and Cyrillic letters in lower case.
std::string fold(const std::string &text) {
	std::string result;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte < 0x80) {
			result += static_cast<char>(std::tolower(byte));
			continue;
		}
		if (index + 1 < text.size() && byte == 0xD0) {
			const unsigned char next = static_cast<unsigned char>(text[index + 1]);
			if (next >= 0x90 && next <= 0x9F) { result += static_cast<char>(0xD0); result += static_cast<char>(next + 0x20); ++index; continue; }
			if (next >= 0xA0 && next <= 0xAF) { result += static_cast<char>(0xD1); result += static_cast<char>(next - 0x20); ++index; continue; }
		}
		result += static_cast<char>(byte);
	}
	return result;
}

bool word_byte(char character) {
	const unsigned char byte = static_cast<unsigned char>(character);
	return std::isalnum(byte) != 0 || byte >= 0x80;
}

// Whether `wanted` (folded) stands in `folded` as a whole word or words.
bool whole_word(const std::string &folded, const std::string &wanted) {
	if (wanted.empty()) return false;
	for (std::size_t at = folded.find(wanted); at != std::string::npos; at = folded.find(wanted, at + 1)) {
		const std::size_t end = at + wanted.size();
		if ((at == 0 || !word_byte(folded[at - 1])) && (end >= folded.size() || !word_byte(folded[end]))) return true;
	}
	return false;
}

// Whether `wanted` (folded) is a text the lawyer's material this call carries
// shows openly as a cell of `table`'s column `letter`: a workbook line of that
// sheet sent unmasked ("Row 7: A7 = General"); or, where the material carries
// no line of that column, words standing whole in a lawyer-material span.
// Decided from the sent text alone, never from a local cell: a guess the
// material does not show is refused whatever the table holds (DESIGN4 §6.5,
// §8.4 L2).
// The label as the table itself holds it (local), when the model wrote one
// the material shows for this column: the shown cell's own text - its words
// may carry symbols ("CY2 ([[Date49]])") the cell holds as written (live EN1,
// 26 Sep 2026: compared as sent, it matched no row and summed to $0). Words
// shown only in the material are their own label.
// A label without the gloss in parentheses after it: "CY2 (2025)" is the
// label "CY2" (live EN1, 26 Sep 2026: "C eq CY2" found no cell).
std::string without_gloss(const std::string &folded) {
	if (folded.empty() || folded.back() != ')') return folded;
	const std::size_t open = folded.rfind(" (");
	if (open == std::string::npos || open == 0) return folded;
	return folded.substr(0, open);
}

std::optional<std::string> text_shown(const OpContext &context, const tables::ProtectedTable &table,
		const std::string &letter, const std::string &wanted) {
	std::vector<std::string> sources;
	std::vector<std::string> material;
	for (const PayloadSegment &segment : context.segments) {
		if (segment.start >= context.sent.size()) continue;
		const bool source = segment.kind == PayloadSegment::Kind::Source;
		if (!source && segment.kind != PayloadSegment::Kind::Request && segment.kind != PayloadSegment::Kind::LawyerAnswer)
			continue;
		std::string part = context.sent.substr(segment.start, segment.length);
		if (source) sources.push_back(part);
		material.push_back(std::move(part));
	}
	bool columnShown = false;
	for (const tables::PassageSheet &sheet : tables::passage_sheets(sources, 64, kRowsPerTable, 1)) {
		if (sheet.name.empty() || sheet.name != table.sheet) continue;
		for (const tables::PassageRow &row : sheet.rows)
			for (const tables::PassageCell &cell : row.cells) {
				if (cell.column != letter) continue;
				columnShown = true;
				const std::string shown = fold(cell.raw);
				if (shown != wanted && without_gloss(shown) != wanted) continue;
				// The label as written, without a gloss the model left out.
				if (const tables::TableCell *held = tables::cell_of(table, row.number, letter); held != nullptr && !held->text.empty())
					return shown == wanted ? held->text : without_gloss(fold(held->text));
				return cell.raw;
			}
	}
	if (columnShown) return std::nullopt;
	for (const std::string &part : material)
		if (whole_word(fold(part), wanted)) return wanted;
	return std::nullopt;
}

// What an atom's value is: a symbol's fact, a label's, another column of the
// same table, or a constant as written.
struct AtomValue {
	enum class Kind { None, Scalar, Column, Text };
	Kind kind = Kind::None;
	std::optional<Value> value;
	std::string symbol;
	std::string fact_key;
	std::set<std::string> roots;
	ColumnRef column;
	std::string text;
	// "fk:..." / "c:..." / "col:B": how the canonical form names it.
	std::string canonical;
};

bool sent(const OpContext &context, const std::string &symbol) {
	if (context.table == nullptr) return false;
	return context.table->sent_symbols.count(symbol) != 0 || context.table->derived_facts.count(symbol) != 0;
}

// The value a symbol stands for: its registry row's, or the derived fact's.
bool scalar_of(const OpContext &context, const std::string &symbol, AtomValue &into) {
	if (context.table != nullptr)
		if (const auto derived = context.table->derived_facts.find(symbol); derived != context.table->derived_facts.end()) {
			into.value = derived->second.value;
			into.fact_key = derived->second.fact_key;
			into.roots = derived->second.roots;
			into.symbol = symbol;
			into.kind = AtomValue::Kind::Scalar;
			into.canonical = "fk:" + into.fact_key;
			return true;
		}
	if (!context.row_of) return false;
	const RequestMappingEntry *row = context.row_of(symbol);
	if (row == nullptr) return false;
	into.symbol = symbol;
	into.kind = AtomValue::Kind::Scalar;
	into.value = row->value;
	into.fact_key = row->fact_key;
	if (!row->fact_key.empty()) into.roots.insert(row->fact_key);
	into.canonical = row->fact_key.empty() ? "sym:" + symbol : "fk:" + row->fact_key;
	return true;
}

// Reads an atom's value (`written`) against the column it is compared with.
// Returns Code::None, or the code refusing it.
Code atom_value(const OpContext &context, const tables::ProtectedTable &table, const ColumnRef &column,
		const std::string &written, AtomValue &into) {
	if (written.empty()) return Code::BadPredicate;
	// Another column of the same table: "[[Table1]].C", or a derived "@label".
	if (written.rfind("[[", 0) == 0) {
		const std::size_t close = written.find("]]");
		if (close == std::string::npos) return Code::BadPredicate;
		const std::string handle = written.substr(2, close - 2);
		if (close + 2 < written.size() && written[close + 2] == '.') {
			if (handle != table.handle) return Code::CrossTable;
			const std::string letter = written.substr(close + 3);
			if (letter.empty() || letter.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
				return Code::BadPredicate;
			const tables::TableColumn *other = tables::column_of(table, letter);
			if (other == nullptr || std::find(table.described_columns.begin(), table.described_columns.end(), letter)
					== table.described_columns.end())
				return Code::UnknownColumn;
			into.kind = AtomValue::Kind::Column;
			into.column.table = &table;
			into.column.column = other;
			into.column.name = letter;
			into.column.element = tables::parse_type(other->type);
			into.canonical = "col:" + letter;
			return Code::None;
		}
		const std::string symbol = written.substr(0, close + 2);
		if (!sent(context, symbol)) return Code::UnknownSymbol;
		if (!scalar_of(context, symbol, into)) return Code::UnknownSymbol;
		return Code::None;
	}
	if (written.front() == '@') {
		if (const tables::DerivedColumn *derived = tables::derived_column(table, written)) {
			into.kind = AtomValue::Kind::Column;
			into.column.table = &table;
			into.column.derived = derived;
			into.column.name = written;
			into.column.element = tables::parse_type(derived->type);
			into.canonical = "col:" + written;
			return Code::None;
		}
		if (context.table == nullptr) return Code::UnknownLabel;
		const auto bound = context.table->compute_labels.find(written.substr(1));
		if (bound == context.table->compute_labels.end()) return Code::UnknownLabel;
		if (!scalar_of(context, bound->second, into)) return Code::UnknownLabel;
		return Code::None;
	}
	// A constant: never an amount or a date (DESIGN4 §8.1 R2) but the typed
	// zero; text only as a cell of that column the lawyer's material shows.
	const std::string trimmed = written;
	if (trimmed == "0") {
		into.kind = AtomValue::Kind::Scalar;
		into.canonical = "c:0";
		if (column.element && column.element->kind == TypeKind::Money)
			into.value = semantic::make_money(Rational(0), column.element->currency == "?" ? std::string() : column.element->currency);
		else if (column.element && (column.element->kind == TypeKind::Percentage || column.element->kind == TypeKind::Rate))
			into.value = semantic::make_percentage(Rational(0));
		else if (column.element && column.element->kind == TypeKind::Duration)
			into.value = semantic::make_duration(Rational(0), column.element->unit);
		else
			into.value = semantic::make_number(Rational(0));
		return Code::None;
	}
	const bool textColumn = column.element
		&& (column.element->kind == TypeKind::Word || column.element->kind == TypeKind::Entity);
	if (textColumn) {
		// The text analogue of grounding: a cell of this column, shown openly in
		// the lawyer's material this call carries - judged from the sent text.
		std::string clean = trimmed;
		while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) clean.pop_back();
		clean.erase(0, std::min(clean.find_first_not_of(" \t"), clean.size()));
		const std::string wanted = fold(clean);
		const std::optional<std::string> label = wanted.empty() ? std::nullopt : text_shown(context, table, column.name, wanted);
		if (!label) return Code::ConstantNotInMaterial;
		into.kind = AtomValue::Kind::Text;
		into.text = *label;
		into.canonical = "t:" + wanted;
		return Code::None;
	}
	std::optional<Value> constant = semantic::parse_value(trimmed);
	if (!constant) return Code::BadPredicate;
	// A period against a date column is the op's shape, not its value: said
	// before grounding (class S).
	if (constant->kind == Kind::Duration && column.element && column.element->kind == TypeKind::Date) return Code::TypeMismatch;
	if (constant->kind == Kind::Money || constant->kind == Kind::Date || constant->kind == Kind::DayOfYear)
		return Code::UnsafeLiteral;
	// Grounded in the lawyer's material, or one of the safe constants.
	// The executor's safe constants (Admission.cpp safe_constant), as value keys.
	static const std::set<std::string> safe{"NUM:0", "NUM:1", "NUM:2", "NUM:3", "NUM:4", "NUM:5", "NUM:6", "NUM:7", "NUM:8",
		"NUM:9", "NUM:10", "NUM:11", "NUM:12", "NUM:24", "NUM:60", "NUM:100", "NUM:360",
		"NUM:365", "NUM:366", "NUM:1440"};
	const std::string key = semantic::value_key(*constant);
	bool grounded = safe.count(key) != 0;
	if (!grounded)
		for (const ConstantUse &use : operations::ground_constants(context.sent, context.segments))
			if (use.value_key == key) grounded = true;
	if (!grounded) return Code::ConstantNotInMaterial;
	into.kind = AtomValue::Kind::Scalar;
	into.value = constant;
	into.canonical = "c:" + key;
	return Code::None;
}

// The tests each type of column admits (DESIGN4 §6.5).
// What a date column tested against a period is told (live, 26 Sep 2026: a
// window "within [[Date8]] and 24 months" has no direction).
const std::string kDateNotPeriod = "a date column is tested against dates: within takes a date and a signed period, or two "
	"dates";

bool test_fits(const std::string &test, const ArgType &element) {
	static const std::set<std::string> numericTests{"eq", "ne", "gt", "lt", "ge", "le"};
	static const std::set<std::string> dateTests{"before", "after", "on_or_before", "on_or_after", "within", "outside",
		"eq", "ne"};
	if (test == "blank" || test == "not_blank" || test == "hidden" || test == "visible") return true;
	if (numeric(element)) return numericTests.count(test) != 0;
	if (element.kind == TypeKind::Date) return dateTests.count(test) != 0;
	return test == "eq" || test == "ne";
}

bool kinds_fit(const ArgType &element, const Value &value) {
	switch (element.kind) {
		case TypeKind::Money: return value.kind == Kind::Money || (value.kind == Kind::Number && value.amount.is_zero());
		case TypeKind::Percentage:
		case TypeKind::Rate: return value.kind == Kind::Percentage;
		case TypeKind::Date: return value.kind == Kind::Date;
		case TypeKind::Duration: return value.kind == Kind::Duration;
		case TypeKind::Number:
		case TypeKind::Count: return value.kind == Kind::Number || value.kind == Kind::Count;
		case TypeKind::Boolean: return value.kind == Kind::Boolean;
		default: return false;
	}
}

std::string kind_word(const Value &value) {
	switch (value.kind) {
		case Kind::Money: return "MONEY(" + (value.currency.empty() ? std::string("?") : value.currency) + ")";
		case Kind::Percentage: return "PERCENTAGE";
		case Kind::Date: return "DATE";
		case Kind::Duration: return "DURATION";
		case Kind::Boolean: return "BOOLEAN";
		case Kind::Count: return "COUNT";
		default: return "NUMBER";
	}
}

Truth compare_values(const Value &cell, const std::string &test, const Value &value, const std::optional<Value> &second) {
	try {
		if (test == "before") return from(semantic::before(cell, value).truth);
		if (test == "after") return from(semantic::after(cell, value).truth);
		if (test == "on_or_before") return negate(from(semantic::after(cell, value).truth));
		if (test == "on_or_after") return negate(from(semantic::before(cell, value).truth));
		if (test == "within" || test == "outside") {
			if (!second) return Truth::Undetermined;
			const Truth inside = from(semantic::within(cell, value, *second).truth);
			return test == "within" ? inside : negate(inside);
		}
		semantic::Comparator comparator = semantic::Comparator::Equal;
		if (test == "ne") comparator = semantic::Comparator::NotEqual;
		else if (test == "gt") comparator = semantic::Comparator::Greater;
		else if (test == "lt") comparator = semantic::Comparator::Less;
		else if (test == "ge") comparator = semantic::Comparator::GreaterOrEqual;
		else if (test == "le") comparator = semantic::Comparator::LessOrEqual;
		if (cell.kind == Kind::Money && value.kind == Kind::Number && value.amount.is_zero()) {
			Value zero = semantic::make_money(Rational(0), cell.currency);
			zero.basis = cell.basis;
			return from(semantic::compare(cell, comparator, zero).truth);
		}
		// A figure owed per period against one of the same period: compared as
		// amounts (the types already match).
		Value left = cell;
		Value right = value;
		if (left.kind == Kind::Money && right.kind == Kind::Money) left.basis = right.basis = semantic::RateBasis::None;
		if (left.kind == Kind::Count) left.kind = Kind::Number;
		if (right.kind == Kind::Count) right.kind = Kind::Number;
		return from(semantic::compare(left, comparator, right).truth);
	} catch (const semantic::AlgebraError &) {
		return Truth::Undetermined;
	}
}

struct Atom {
	WhereAtom written;
	ColumnRef column;
	AtomValue value;
	AtomValue value2;
	bool structural = false;
};

// Checks and reads every atom of a filter (class S: types, letters, codes).
std::optional<OpAnswer> read_atoms(const OpContext &context, const tables::ProtectedTable &table, std::vector<Atom> &atoms) {
	if (context.node == nullptr || !context.node->where) return std::nullopt;
	if (context.node->where->size() > kMaxWhereAtoms) return error_answer(Code::BadPredicate, 0, "at most 8 conditions");
	std::size_t index = 0;
	for (const WhereAtom &given : *context.node->where) {
		++index;
		Atom atom;
		atom.written = given;
		const WhereAtom &atomWritten = given;
		static const std::set<std::string> tests{"eq", "ne", "gt", "lt", "ge", "le", "before", "after", "on_or_before",
			"on_or_after", "within", "outside", "blank", "not_blank", "hidden", "visible"};
		if (tests.count(atomWritten.test) == 0) return error_answer(Code::BadPredicate, index);
		atom.structural = atomWritten.test == "hidden" || atomWritten.test == "visible";
		if (!atom.structural || !atomWritten.column.empty()) {
			if (atomWritten.column.empty()) return error_answer(Code::BadPredicate, index);
			if (atomWritten.column.front() == '@') {
				atom.column.table = &table;
				atom.column.derived = tables::derived_column(table, atomWritten.column);
				atom.column.name = atomWritten.column;
				if (atom.column.derived == nullptr) return error_answer(Code::UnknownLabel, index);
				atom.column.element = tables::parse_type(atom.column.derived->type);
			} else {
				atom.column.table = &table;
				atom.column.column = tables::column_of(table, atomWritten.column);
				atom.column.name = atomWritten.column;
				if (atom.column.column == nullptr
						|| std::find(table.described_columns.begin(), table.described_columns.end(), atomWritten.column)
							== table.described_columns.end())
					return error_answer(Code::UnknownColumn, index, "a column is named by a letter the tables block lists");
				atom.column.element = tables::parse_type(atom.column.column->type);
			}
			if (!atom.column.element) return error_answer(Code::BadPredicate, index);
		}
		if (atom.structural || atomWritten.test == "blank" || atomWritten.test == "not_blank") {
			if (atomWritten.value || atomWritten.value2) return error_answer(Code::BadPredicate, index, "this test takes no value");
			atoms.push_back(std::move(atom));
			continue;
		}
		if (!test_fits(atomWritten.test, *atom.column.element))
			return error_answer(Code::BadPredicate, index, "this test does not apply to a column of this type", {},
				written(*atom.column.element));
		if (!atomWritten.value) return error_answer(Code::BadPredicate, index, "the test needs a value");
		const auto refused = [&](Code code) {
			return error_answer(code, index,
				code == Code::TypeMismatch && atom.column.element->kind == TypeKind::Date ? kDateNotPeriod : std::string());
		};
		const Code code = atom_value(context, table, atom.column, *atomWritten.value, atom.value);
		if (code != Code::None) return refused(code);
		if (atomWritten.test == "within" || atomWritten.test == "outside") {
			if (!atomWritten.value2) return error_answer(Code::BadPredicate, index, "within takes a start and an end");
			// A window from a date by a signed period - the one form within takes
			// everywhere (kSignThePeriod): "[[Date8]]" and "-24 months" is the 24
			// months back from it.
			std::string periodWritten = *atomWritten.value2;
			while (!periodWritten.empty() && periodWritten.front() == ' ') periodWritten.erase(periodWritten.begin());
			const bool signedPeriod = written_signed(periodWritten);
			bool negative = false;
			std::string magnitude = periodWritten;
			if (signedPeriod) {
				negative = magnitude.front() != '+';
				magnitude.erase(0, magnitude.front() == '-' || magnitude.front() == '+' ? 1 : 3);
			}
			std::optional<Value> period = semantic::parse_value(magnitude);
			if (atom.column.element->kind == TypeKind::Date && period && period->kind == Kind::Duration) {
				if (!signedPeriod) return error_answer(Code::TypeMismatch, index, kSignThePeriod);
				// The executor's safe counts (Admission.cpp safe_constant), or the
				// period as the material writes it.
				static const std::set<long long> safeCounts{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 24, 60, 100, 360, 365, 366};
				bool grounded = period->amount.is_integer()
					&& safeCounts.count(static_cast<long long>(period->amount.numerator())) != 0;
				if (!grounded)
					for (const ConstantUse &use : operations::ground_constants(context.sent, context.segments))
						if (use.value_key == semantic::value_key(*period)) grounded = true;
				if (!grounded) return error_answer(Code::ConstantNotInMaterial, index);
				if (atom.value.kind != AtomValue::Kind::Scalar || !atom.value.value) return error_answer(Code::TypeMismatch, index, kSignThePeriod);
				Value step = *period;
				if (negative) step.amount = Rational(0) - step.amount;
				const std::optional<Value> other = date_moved_by(*atom.value.value, step);
				if (!other) return error_answer(Code::TypeMismatch, index, "the window's date is one day: a day, not a month or a quarter");
				AtomValue end = atom.value;
				end.value = other;
				end.symbol.clear();
				end.fact_key.clear();
				end.canonical = atom.value.canonical + (negative ? "-" : "+") + semantic::value_key(*period);
				if (negative) {
					atom.value2 = atom.value;
					atom.value = end;
				} else {
					atom.value2 = end;
				}
			} else {
				const Code second = atom_value(context, table, atom.column, *atomWritten.value2, atom.value2);
				if (second != Code::None) return refused(second);
			}
		} else if (atomWritten.value2) {
			return error_answer(Code::BadPredicate, index, "this test takes one value");
		}
		// Types, from the declared types alone.
		for (const AtomValue *value : {&atom.value, &atom.value2}) {
			if (value->kind == AtomValue::Kind::Scalar && value->value && !kinds_fit(*atom.column.element, *value->value))
				return error_answer(Code::TypeMismatch, index,
					atom.column.element->kind == TypeKind::Date && value->value->kind == Kind::Duration ? kDateNotPeriod : std::string(),
					written(*atom.column.element), kind_word(*value->value));
			if (value->kind == AtomValue::Kind::Scalar && value->value && value->value->kind == Kind::Money
					&& atom.column.element->kind == TypeKind::Money && !value->value->currency.empty()
					&& atom.column.element->currency != "?" && value->value->currency != atom.column.element->currency)
				return error_answer(Code::CurrencyMismatch, index);
			if (value->kind == AtomValue::Kind::Column && value->column.element
					&& value->column.element->kind != atom.column.element->kind)
				return error_answer(Code::TypeMismatch, index, {}, written(*atom.column.element),
					written(*value->column.element));
		}
		atoms.push_back(std::move(atom));
	}
	return std::nullopt;
}

Truth atom_truth(const tables::ProtectedTable &table, const Atom &atom, std::size_t rowNumber) {
	const tables::TableRow *row = tables::row_of(table, rowNumber);
	if (row == nullptr) return Truth::False;
	if (atom.written.test == "hidden") return row->hidden ? Truth::True : Truth::False;
	if (atom.written.test == "visible") return row->hidden ? Truth::False : Truth::True;
	const RowValue cell = value_at(atom.column, rowNumber);
	if (atom.written.test == "blank") return cell.undetermined ? Truth::Undetermined : cell.blank ? Truth::True : Truth::False;
	if (atom.written.test == "not_blank") return cell.undetermined ? Truth::Undetermined : cell.blank ? Truth::False : Truth::True;
	if (atom.value.kind == AtomValue::Kind::Text || (atom.value.kind == AtomValue::Kind::Scalar && !atom.value.value)) {
		// A text or an entity column: the cell's words, or its symbol. A blank
		// cell is no label: never equal to one (live EN1: "CY2" over a column
		// with blank rows was charged as a bound).
		if (cell.cell == nullptr) {
			if (cell.undetermined) return Truth::Undetermined;
			if (atom.written.test == "eq") return Truth::False;
			if (atom.written.test == "ne") return Truth::True;
			return Truth::Undetermined;
		}
		bool equal = false;
		if (atom.value.kind == AtomValue::Kind::Text) {
			const std::string held = fold(cell.cell->text);
			const std::string label = fold(atom.value.text);
			equal = held == label || without_gloss(held) == label;
		}
		else equal = !cell.cell->placeholder.empty() && cell.cell->placeholder == atom.value.symbol;
		if (atom.written.test == "eq") return equal ? Truth::True : Truth::False;
		if (atom.written.test == "ne") return equal ? Truth::False : Truth::True;
		return Truth::Undetermined;
	}
	if (!cell.value) return Truth::Undetermined;
	std::optional<Value> value = atom.value.value;
	if (atom.value.kind == AtomValue::Kind::Column) value = value_at(atom.value.column, rowNumber).value;
	if (!value) return Truth::Undetermined;
	std::optional<Value> second = atom.value2.value;
	if (atom.value2.kind == AtomValue::Kind::Column) second = value_at(atom.value2.column, rowNumber).value;
	// A cell that holds a figure but no date - a rate or a threshold in a
	// block of parameters below the dated rows - meets no date test, inside a
	// window or outside it: it is not in the set, not undetermined (live EN1:
	// four such rows were reported as undetermined beside the quarters).
	static const std::set<std::string> temporal{"before", "after", "on_or_before", "on_or_after", "within", "outside"};
	if (temporal.count(atom.written.test) != 0 && value->kind == Kind::Date && cell.value->kind != Kind::Date
			&& cell.value->kind != Kind::DayOfYear)
		return Truth::False;
	return compare_values(*cell.value, atom.written.test, *value, second);
}

// Whether a comparison over this atom's column may be undetermined, from its
// declared type alone: a partial date, a blank cell, a derived row not worked
// out.
bool atom_may_be_undetermined(const Atom &atom) {
	if (atom.structural) return false;
	if (atom.written.test == "blank" || atom.written.test == "not_blank") return true;
	if (atom.column.derived != nullptr) return true;
	// A blank cell of a text column is decided (never equal to a label).
	const bool labelTest = atom.value.kind == AtomValue::Kind::Text
		&& (atom.written.test == "eq" || atom.written.test == "ne");
	if (atom.column.column != nullptr && atom.column.column->sparse && !labelTest) return true;
	if (atom.column.element && atom.column.element->kind == TypeKind::Date) {
		if (atom.column.element->precision != "day") return true;
		for (const AtomValue *value : {&atom.value, &atom.value2})
			if (value->value && value->value->kind == Kind::Date && value->value->date.precision != temporal::Precision::Day)
				return true;
	}
	return false;
}

// A filter's predicate as what it decides, never as it was written: a column
// by its letter, the conditions in one order, and a window between two days as
// its two ends - "within [[Date68]]..[[Date21]]" is "on_or_after [[Date68]]"
// and "on_or_before [[Date21]]". The same rows asked the same way are known
// once they are listed, whatever the wording (live EN1 c22: the window listed
// in one task, written as "within" in the next, was charged its dates again
// and the underpayment rate against the threshold was refused).
std::string atoms_canonical(const std::vector<Atom> &atoms) {
	std::vector<std::string> conditions;
	for (const Atom &atom : atoms) {
		const std::string column = atom.column.column != nullptr ? atom.column.column->letter : atom.written.column;
		const auto condition = [&](const std::string &test, const AtomValue *value, const AtomValue *value2) {
			std::string text = column + " " + test;
			if (value != nullptr && value->kind != AtomValue::Kind::None) text += " " + value->canonical;
			if (value2 != nullptr && value2->kind != AtomValue::Kind::None) text += " " + value2->canonical;
			conditions.push_back(text);
		};
		// Both ends a single day: then every cell, whole day or month, blank or
		// no date at all, meets the window exactly when it meets both ends.
		const auto one_day = [](const AtomValue &end) {
			return end.value && end.value->kind == Kind::Date && end.value->date.precision == temporal::Precision::Day;
		};
		if (atom.written.test == "within" && one_day(atom.value) && one_day(atom.value2)) {
			condition("on_or_after", &atom.value, nullptr);
			condition("on_or_before", &atom.value2, nullptr);
		} else {
			condition(atom.written.test, &atom.value, &atom.value2);
		}
	}
	std::sort(conditions.begin(), conditions.end());
	conditions.erase(std::unique(conditions.begin(), conditions.end()), conditions.end());
	std::string canonical;
	for (const std::string &condition : conditions) canonical += (canonical.empty() ? "" : "&") + condition;
	return canonical;
}

OpAnswer evaluate_filter(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.size() != 1) return error_answer(Code::Arity);
	const Operand base = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(base);
	if (!set || base.table == nullptr) return error_answer(Code::TypeMismatch, 1, {}, "TABLE or ROW_SET");
	const tables::ProtectedTable &table = *base.table;
	std::vector<Atom> atoms;
	if (std::optional<OpAnswer> refused = read_atoms(context, table, atoms)) return *refused;
	tables::RowSet result;
	result.table = table.handle;
	result.structural = set->structural;
	result.may_be_bounded = set->may_be_bounded;
	result.roots = set->roots;
	result.cell_roots = set->cell_roots;
	result.operand_roots = set->operand_roots;
	result.group_keys = set->group_keys;
	const std::vector<std::size_t> rows = members(*set);
	// The base the predicates are read over, and list_rows' N: the members of
	// a structural set (the tables block states them), else the base the set
	// itself was read over - never a membership a value decided (D4).
	const std::vector<std::size_t> readOver = set->structural ? rows : base_of(*set);
	result.base_rows = readOver;
	result.base_size = readOver.size();
	std::set<std::string> roots(set->roots.begin(), set->roots.end());
	// A cell read row by row is one membership bit; a root every row shares (a
	// derived column's scalar operand) is ranked among the rows.
	const auto read_roots = [&](const RowValue &read) {
		std::set<std::string> cells;
		for (const std::string &root : read.roots)
			if (read.operand_roots.count(root) == 0) cells.insert(root);
		add_roots(result.cell_roots, cells);
		add_roots(result.operand_roots, read.operand_roots);
		roots.insert(read.roots.begin(), read.roots.end());
	};
	const auto derived_keys = [&](const ColumnRef &column) {
		if (column.derived == nullptr) return;
		for (const std::string &key : column.derived->base_group_keys)
			if (std::find(result.group_keys.begin(), result.group_keys.end(), key) == result.group_keys.end())
				result.group_keys.push_back(key);
	};
	for (const Atom &atom : atoms) {
		if (atom.structural) continue;
		result.structural = false;
		result.may_be_bounded = result.may_be_bounded || atom_may_be_undetermined(atom);
		const std::string key = atom.column.derived != nullptr ? "derived:" + tables::place_key(table) + ":" + atom.column.name
			: tables::group_key(table, atom.column.name);
		if (std::find(result.group_keys.begin(), result.group_keys.end(), key) == result.group_keys.end())
			result.group_keys.push_back(key);
		derived_keys(atom.column);
		// Every cell a predicate read over the base, and its operands.
		for (const std::size_t row : readOver) {
			read_roots(value_at(atom.column, row));
			for (const AtomValue *value : {&atom.value, &atom.value2})
				if (value->kind == AtomValue::Kind::Column) read_roots(value_at(value->column, row));
		}
		for (const AtomValue *value : {&atom.value, &atom.value2}) {
			add_roots(result.operand_roots, value->roots);
			roots.insert(value->roots.begin(), value->roots.end());
			if (value->kind == AtomValue::Kind::Column) derived_keys(value->column);
		}
	}
	// A root ranked is not also a membership bit.
	result.cell_roots.erase(std::remove_if(result.cell_roots.begin(), result.cell_roots.end(), [&](const std::string &root) {
		return std::find(result.operand_roots.begin(), result.operand_roots.end(), root) != result.operand_roots.end();
	}), result.cell_roots.end());
	for (const std::size_t row : rows) {
		Truth truth = contains(set->undetermined, row) ? Truth::Undetermined : Truth::True;
		for (const Atom &atom : atoms) {
			const Truth own = atom_truth(table, atom, row);
			if (own == Truth::False) { truth = Truth::False; break; }
			if (own == Truth::Undetermined) truth = Truth::Undetermined;
		}
		if (truth == Truth::True) result.certain.push_back(row);
		else if (truth == Truth::Undetermined) result.undetermined.push_back(row);
	}
	result.roots.assign(roots.begin(), roots.end());
	result.canonical = "filter(" + set->canonical + ";" + atoms_canonical(atoms) + ")";
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	ArgType type;
	type.kind = TypeKind::RowSet;
	type.table = table.handle;
	answer.result = result_of(type);
	answer.roots = roots;
	answer.rows = result;
	if (context.node != nullptr && context.node->bind)
		if (tables::ProtectedTable *owner = mutable_table(context, table.handle))
			owner->row_sets["@" + *context.node->bind] = result;
	return answer;
}

// ---------------------------------------------------------------------------
// count, union / intersect / minus

OpAnswer count_value(std::size_t certain, std::size_t undetermined) {
	OpAnswer answer;
	answer.result = result_of(count_type());
	const auto make = [](std::size_t count) {
		Value value = semantic::make_number(Rational(static_cast<long long>(count)));
		value.kind = Kind::Count;
		return value;
	};
	if (undetermined == 0) {
		answer.status = OpStatus::Ok;
		answer.value = make(certain);
		return answer;
	}
	answer.status = OpStatus::Bounded;
	answer.lo = make(certain);
	answer.hi = make(certain + undetermined);
	answer.certainty.kind = Certainty::Kind::Bounded;
	return answer;
}

// A count's provenance (DESIGN4 §6.7): each counted row, by its first cell.
std::vector<RelationSource> row_sources(const tables::ProtectedTable *table, const std::vector<std::size_t> &rows) {
	std::vector<RelationSource> sources;
	if (table == nullptr) return sources;
	for (const std::size_t number : rows)
		if (const tables::TableRow *row = tables::row_of(*table, number))
			for (const tables::TableCell &cell : row->cells)
				if (cell.value || !cell.text.empty()) {
					sources.push_back(cell.source);
					break;
				}
	return sources;
}

// The words a group's key is written back to the model in, and its period
// issued as a derived date where the key is a calendar period.
std::string key_written(const tables::GroupSet &groups, std::size_t index, OpContext &context) {
	if (index < groups.key_words.size() && !groups.key_words[index].empty()) return groups.key_words[index];
	if (index < groups.key_periods.size() && groups.key_periods[index] && context.issue) {
		DerivedFact fact;
		fact.op = "group_key";
		fact.ast = "group_key(" + groups.canonical + "," + std::to_string(index + 1) + ")";
		fact.value = *groups.key_periods[index];
		fact.type = "DATE(" + groups.unit.substr(0, groups.unit.size() - 1) + ")";
		fact.depth = 1;
		fact.task = context.task;
		for (const std::string &root : groups.groups[index].second.cell_roots) fact.roots.insert(root);
		if (context.node != nullptr) fact.subproblem = context.node->why;
		return context.issue(fact);
	}
	return "group " + std::to_string(index + 1);
}

OpAnswer evaluate_count(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.size() != 1) return error_answer(Code::Arity);
	const Operand operand = settled(given[0], context);
	if (operand.groups) {
		const tables::GroupSet &groups = *operand.groups;
		OpAnswer answer;
		answer.status = OpStatus::Ok;
		answer.result = result_of(count_type());
		for (std::size_t index = 0; index < groups.groups.size() && answer.pairs.size() < kListedRows; ++index) {
			const tables::RowSet &members = groups.groups[index].second;
			if (!members.undetermined.empty()) return unknown_answer(Code::PrecisionInsufficient);
			Value count = semantic::make_number(Rational(static_cast<long long>(members.certain.size())));
			count.kind = Kind::Count;
			answer.pairs.emplace_back(key_written(groups, index, context), count);
			const std::vector<RelationSource> sources = row_sources(operand.table, members.certain);
			answer.root_sources.insert(answer.root_sources.end(), sources.begin(), sources.end());
		}
		for (const auto &[key, members] : groups.groups) {
			static_cast<void>(key);
			answer.roots.insert(members.roots.begin(), members.roots.end());
		}
		answer.roots.insert(groups.key_roots.begin(), groups.key_roots.end());
		return answer;
	}
	const std::optional<tables::RowSet> set = set_of(operand);
	if (!set) return error_answer(Code::TypeMismatch, 1, {}, "ROW_SET or GROUP_SET");
	OpAnswer answer = count_value(set->certain.size(), set->undetermined.size());
	if (!membership_disclosed(*set, context)) answer.roots.insert(set->roots.begin(), set->roots.end());
	answer.root_sources = row_sources(operand.table, members(*set));
	return answer;
}

enum class SetOp { Union, Intersect, Minus };

template <SetOp op>
OpAnswer evaluate_set_algebra(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.size() != 2) return error_answer(Code::Arity);
	const Operand first = settled(given[0], context);
	const Operand second = settled(given[1], context);
	const std::optional<tables::RowSet> a = set_of(first);
	const std::optional<tables::RowSet> b = set_of(second);
	if (!a) return error_answer(Code::TypeMismatch, 1, {}, "ROW_SET");
	if (!b) return error_answer(Code::TypeMismatch, 2, {}, "ROW_SET");
	if (a->table != b->table) return error_answer(Code::CrossTable, 2);
	const std::vector<std::size_t> aAll = members(*a);
	const std::vector<std::size_t> bAll = members(*b);
	tables::RowSet result;
	result.table = a->table;
	std::set<std::size_t> certain;
	std::set<std::size_t> maybe;
	for (const std::size_t row : aAll) {
		const bool inA = contains(a->certain, row);
		const bool inB = contains(b->certain, row);
		const bool maybeB = contains(bAll, row);
		if constexpr (op == SetOp::Union) {
			if (inA || inB) certain.insert(row); else maybe.insert(row);
		} else if constexpr (op == SetOp::Intersect) {
			if (!maybeB) continue;
			if (inA && inB) certain.insert(row); else maybe.insert(row);
		} else {
			if (inB) continue;
			if (inA && !maybeB) certain.insert(row); else maybe.insert(row);
		}
	}
	if constexpr (op == SetOp::Union)
		for (const std::size_t row : bAll) {
			if (contains(aAll, row)) continue;
			if (contains(b->certain, row)) certain.insert(row); else maybe.insert(row);
		}
	result.certain.assign(certain.begin(), certain.end());
	for (const std::size_t row : maybe)
		if (certain.count(row) == 0) result.undetermined.push_back(row);
	result.structural = a->structural && b->structural;
	result.may_be_bounded = a->may_be_bounded || b->may_be_bounded;
	std::set<std::string> roots(a->roots.begin(), a->roots.end());
	roots.insert(b->roots.begin(), b->roots.end());
	result.roots.assign(roots.begin(), roots.end());
	result.cell_roots = a->cell_roots;
	for (const std::string &root : b->cell_roots)
		if (std::find(result.cell_roots.begin(), result.cell_roots.end(), root) == result.cell_roots.end())
			result.cell_roots.push_back(root);
	result.operand_roots = a->operand_roots;
	for (const std::string &root : b->operand_roots)
		if (std::find(result.operand_roots.begin(), result.operand_roots.end(), root) == result.operand_roots.end())
			result.operand_roots.push_back(root);
	result.group_keys = a->group_keys;
	for (const std::string &key : b->group_keys)
		if (std::find(result.group_keys.begin(), result.group_keys.end(), key) == result.group_keys.end())
			result.group_keys.push_back(key);
	std::set<std::size_t> base;
	for (const tables::RowSet *side : {&*a, &*b}) {
		const std::vector<std::size_t> own = side->structural ? members(*side) : base_of(*side);
		base.insert(own.begin(), own.end());
	}
	result.base_rows.assign(base.begin(), base.end());
	result.base_size = result.base_rows.size();
	const char *name = op == SetOp::Union ? "union" : op == SetOp::Intersect ? "intersect" : "minus";
	result.canonical = std::string(name) + "(" + a->canonical + "," + b->canonical + ")";
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	ArgType type;
	type.kind = TypeKind::RowSet;
	type.table = result.table;
	answer.result = result_of(type);
	answer.roots = roots;
	answer.rows = result;
	if (context.node != nullptr && context.node->bind)
		if (tables::ProtectedTable *owner = mutable_table(context, result.table)) owner->row_sets["@" + *context.node->bind] = result;
	return answer;
}

// ---------------------------------------------------------------------------
// sum_column, average_column, min_column, max_column

// A column whose rows hold other kinds below its amounts - a block of
// parameters under a list of acts (the real R1 register): what to total instead.
constexpr const char *kMixedColumnHint = "some rows of that column hold other figures - dates, days, rates - beside its "
	"amounts: total only the rows of amounts - filter them first (a test on another column tells them apart, such as "
	"not_blank on a date column), or add their cells";

struct Collected {
	std::vector<std::pair<std::size_t, Value>> certain;
	std::vector<std::pair<std::size_t, Value>> undetermined;
	std::set<std::string> roots;
	std::vector<RelationSource> sources;
	bool failed = false;
};

Collected collect(const tables::RowSet &set, const ColumnRef &column, const OpContext &context) {
	Collected collected;
	if (!membership_disclosed(set, context)) collected.roots.insert(set.roots.begin(), set.roots.end());
	for (const std::size_t row : members(set)) {
		const RowValue read = value_at(column, row);
		collected.roots.insert(read.roots.begin(), read.roots.end());
		collected.sources.insert(collected.sources.end(), read.sources.begin(), read.sources.end());
		const bool undetermined = contains(set.undetermined, row) || read.undetermined;
		if (!read.value) {
			if (read.undetermined) collected.undetermined.emplace_back(row, Value{});
			continue;
		}
		(undetermined ? collected.undetermined : collected.certain).emplace_back(row, *read.value);
	}
	return collected;
}

std::optional<OpAnswer> column_operands(const std::vector<Operand> &given, OpContext &context, tables::RowSet &set,
		ColumnRef &column) {
	if (given.size() != 2) return error_answer(Code::Arity);
	const Operand first = settled(given[0], context);
	const Operand second = settled(given[1], context);
	const std::optional<tables::RowSet> rows = set_of(first);
	if (!rows) return error_answer(Code::TypeMismatch, 1, {}, "ROW_SET");
	column = column_ref(second);
	if (column.table == nullptr || (column.column == nullptr && column.derived == nullptr) || !column.element)
		return error_answer(Code::UnknownColumn, 2);
	if (column.table->handle != rows->table) return error_answer(Code::CrossTable, 2);
	set = *rows;
	return std::nullopt;
}

OpAnswer sum_answer(const Collected &collected, const ArgType &type, const OpOptions &options, bool signed_difference) {
	OpAnswer answer;
	answer.result = result_of(type);
	answer.roots = collected.roots;
	answer.root_sources = collected.sources;
	ExactRational certain;
	int decimals = 0;
	for (const auto &[row, value] : collected.certain) {
		static_cast<void>(row);
		const std::optional<ExactRational> amount = exact_of(value);
		if (!amount) return error_answer(Code::TypeMismatch, 2, kMixedColumnHint, written(type), kind_word(value));
		certain = certain + *amount;
		decimals = std::max(decimals, value.source_decimals);
	}
	if (collected.undetermined.empty()) {
		const std::optional<Value> value = value_of(type, certain, decimals);
		if (!value) return error_answer(Code::DepthExceeded);
		answer.status = OpStatus::Ok;
		answer.value = value;
		if (signed_difference && revealed(options)) answer.sign = sign_word(certain);
		return answer;
	}
	// Rows whose membership is undetermined bound a sum of figures that are
	// not negative; anything else is not known (DESIGN4 §6.5).
	ExactRational extra;
	for (const auto &[row, value] : collected.undetermined) {
		static_cast<void>(row);
		const std::optional<ExactRational> amount = value.kind == Kind::Number && value.amount.is_zero() && value.currency.empty()
			? std::optional<ExactRational>() : exact_of(value);
		if (!amount || amount->sign() < 0) return unknown_answer(Code::PrecisionInsufficient);
		extra = extra + *amount;
	}
	for (const auto &[row, value] : collected.certain) {
		static_cast<void>(row);
		if (value.amount.sign() < 0) return unknown_answer(Code::PrecisionInsufficient);
	}
	const std::optional<Value> lo = value_of(type, certain, decimals);
	const std::optional<Value> hi = value_of(type, certain + extra, decimals);
	if (!lo || !hi) return error_answer(Code::DepthExceeded);
	answer.status = OpStatus::Bounded;
	answer.lo = lo;
	answer.hi = hi;
	answer.certainty.kind = Certainty::Kind::Bounded;
	return answer;
}

// Whether a column is a difference row by row: its sum carries a sign.
bool difference_column(const ColumnRef &column) {
	return column.derived != nullptr && column.derived->op == "subtract";
}

OpAnswer evaluate_sum_column(const std::vector<Operand> &given, const OpOptions &options, OpContext &context) {
	tables::RowSet set;
	ColumnRef column;
	if (std::optional<OpAnswer> refused = column_operands(given, context, set, column)) return *refused;
	if (!numeric(*column.element)) return error_answer(Code::TypeMismatch, 2, {}, "COLUMN(numeric)");
	return sum_answer(collect(set, column, context), total_of(*column.element), options, difference_column(column));
}

OpAnswer evaluate_average_column(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	tables::RowSet set;
	ColumnRef column;
	if (std::optional<OpAnswer> refused = column_operands(given, context, set, column)) return *refused;
	if (!numeric(*column.element)) return error_answer(Code::TypeMismatch, 2, {}, "COLUMN(numeric)");
	const Collected collected = collect(set, column, context);
	if (!collected.undetermined.empty()) return unknown_answer(Code::PrecisionInsufficient);
	if (collected.certain.empty()) return unknown_answer(Code::EmptySet);
	ExactRational total;
	int decimals = 0;
	for (const auto &[row, value] : collected.certain) {
		static_cast<void>(row);
		const std::optional<ExactRational> amount = exact_of(value);
		if (!amount) return error_answer(Code::TypeMismatch, 2);
		total = total + *amount;
		decimals = std::max(decimals, value.source_decimals);
	}
	ArgType type = *column.element;
	if (type.kind == TypeKind::Count) type.kind = TypeKind::Number;
	const ExactRational average = total / ExactRational(semantic::BigInt(static_cast<long long>(collected.certain.size())));
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	answer.result = result_of(type);
	answer.value = value_of(type, average, decimals);
	if (!answer.value) return error_answer(Code::DepthExceeded);
	answer.roots = collected.roots;
	answer.root_sources = collected.sources;
	return answer;
}

template <bool smallest>
OpAnswer evaluate_extreme_column(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	tables::RowSet set;
	ColumnRef column;
	if (std::optional<OpAnswer> refused = column_operands(given, context, set, column)) return *refused;
	if (!ordered(*column.element)) return error_answer(Code::TypeMismatch, 2, {}, "COLUMN(ordered)");
	const Collected collected = collect(set, column, context);
	if (!collected.undetermined.empty()) return unknown_answer(Code::PrecisionInsufficient);
	if (collected.certain.empty()) return unknown_answer(Code::EmptySet);
	const Value *best = &collected.certain.front().second;
	for (const auto &[row, value] : collected.certain) {
		static_cast<void>(row);
		try {
			Value left = value;
			Value right = *best;
			if (left.kind == Kind::Money) left.basis = right.basis = semantic::RateBasis::None;
			const semantic::Relation relation = semantic::compare(left,
				smallest ? semantic::Comparator::Less : semantic::Comparator::Greater, right);
			if (relation.truth == semantic::Truth::True) best = &value;
			else if (relation.truth == semantic::Truth::Undetermined) return unknown_answer(Code::PrecisionInsufficient);
		} catch (const semantic::AlgebraError &) {
			return error_answer(Code::TypeMismatch, 2);
		}
	}
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	answer.result = result_of(*column.element);
	answer.value = *best;
	answer.roots = collected.roots;
	answer.root_sources = collected.sources;
	return answer;
}

// ---------------------------------------------------------------------------
// derive_column

OpAnswer evaluate_derive_column(const std::vector<Operand> &given, const OpOptions &options, OpContext &context) {
	if (given.size() < 3) return error_answer(Code::Arity);
	const Operand base = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(base);
	if (!set || base.table == nullptr) return error_answer(Code::TypeMismatch, 1, {}, "ROW_SET");
	const std::string opName = !given[1].arg.constant.empty() ? given[1].arg.constant : given[1].arg.written;
	static const std::set<std::string> rowOps{"add", "subtract", "multiply", "divide", "ratio", "percentage_of",
		"apply_rate", "abs", "positive_part", "min", "max", "round", "percentage_change", "start_of", "end_of", "days_in",
		"add_duration", "duration_between"};
	if (rowOps.count(opName) == 0) return error_answer(Code::UnknownOp, 2, "a row op: add, subtract, multiply, divide ...");
	const OpEntry *entry = find_op(opName);
	if (entry == nullptr || entry->typer == nullptr || entry->evaluator == nullptr) return error_answer(Code::UnknownOp, 2);
	// Each operand: a column of this table (read row by row), or a scalar.
	std::vector<ColumnRef> columns(given.size());
	std::vector<Operand> scalars(given.size());
	std::vector<ArgType> types;
	std::set<std::string> operandRoots;
	std::string divisor;
	std::vector<std::string> writtenOperands;
	for (std::size_t index = 2; index < given.size(); ++index) {
		const Operand operand = settled(given[index], context);
		writtenOperands.push_back(operand.arg.written);
		if (operand.arg.kind == ArgKind::Column || (operand.type.kind == TypeKind::Column)) {
			columns[index] = column_ref(operand);
			if (columns[index].table == nullptr || !columns[index].element) return error_answer(Code::UnknownColumn, index + 1);
			if (columns[index].table->handle != set->table) return error_answer(Code::CrossTable, index + 1);
			types.push_back(*columns[index].element);
			if (index == 3 && (opName == "divide" || opName == "ratio")) divisor = "col:" + set->table + "." + columns[index].name;
			continue;
		}
		if (operand.table != nullptr || operand.rows || operand.groups)
			return error_answer(Code::TypeMismatch, index + 1, {}, "a column or a figure", written(operand.type));
		scalars[index] = operand;
		types.push_back(operand.type);
		operandRoots.insert(operand.roots.begin(), operand.roots.end());
		if (index == 3 && (opName == "divide" || opName == "ratio"))
			divisor = operand.fact_key.empty() ? operand.arg.written : "fk:" + operand.fact_key;
	}
	TypeError typeError;
	const std::optional<ResultType> rowType = entry->typer(types, options, typeError);
	if (!rowType) {
		OpAnswer answer = error_answer(typeError.code == Code::None ? Code::TypeMismatch : typeError.code,
			typeError.argument == 0 ? 0 : typeError.argument + 2, typeError.hint, typeError.expected, typeError.got);
		return answer;
	}
	tables::DerivedColumn derived;
	derived.label = context.node != nullptr && context.node->bind ? *context.node->bind
		: (context.node != nullptr ? context.node->id : std::string("derived"));
	derived.op = opName;
	derived.operands = writtenOperands;
	derived.type = written(rowType->type);
	derived.operand_roots = operandRoots;
	// Which rows have a value rests on the set it is worked out over - unless
	// its rows were already disclosed.
	const bool disclosedRows = membership_disclosed(*set, context);
	if (!disclosedRows) {
		derived.operand_roots.insert(set->operand_roots.begin(), set->operand_roots.end());
		derived.base_cell_roots.insert(set->cell_roots.begin(), set->cell_roots.end());
	}
	derived.base_rows = set->structural ? members(*set) : base_of(*set);
	derived.base_group_keys = set->group_keys;
	derived.divisor = divisor;
	std::set<std::string> roots;
	if (!disclosedRows) roots.insert(set->roots.begin(), set->roots.end());
	roots.insert(operandRoots.begin(), operandRoots.end());
	for (const std::size_t row : members(*set)) {
		std::vector<Operand> rowOperands;
		std::set<std::string> rowRoots;
		bool missing = false;
		for (std::size_t index = 2; index < given.size(); ++index) {
			if (columns[index].table == nullptr) {
				rowOperands.push_back(scalars[index]);
				continue;
			}
			const RowValue read = value_at(columns[index], row);
			rowRoots.insert(read.roots.begin(), read.roots.end());
			derived.operand_roots.insert(read.operand_roots.begin(), read.operand_roots.end());
			std::vector<RelationSource> &sources = derived.sources[row];
			sources.insert(sources.end(), read.sources.begin(), read.sources.end());
			if (!read.value) {
				missing = true;
				continue;
			}
			Operand cell;
			cell.arg.kind = ArgKind::CellRef;
			cell.arg.symbol = set->table;
			cell.arg.column = columns[index].name;
			cell.arg.row = row;
			cell.arg.written = "[[" + set->table + "]]." + columns[index].name + std::to_string(row);
			cell.type = *columns[index].element;
			cell.value = read.value;
			cell.roots = read.roots;
			if (read.cell != nullptr) cell.fact_key = tables::cell_root(*columns[index].table, *read.cell);
			rowOperands.push_back(std::move(cell));
		}
		roots.insert(rowRoots.begin(), rowRoots.end());
		derived.roots[row] = rowRoots;
		if (missing) {
			derived.undetermined.insert(row);
			continue;
		}
		OpAnswer rowAnswer;
		try {
			rowAnswer = entry->evaluator(rowOperands, options, context);
		} catch (const std::exception &) {
			rowAnswer.status = OpStatus::Unknown;
		}
		if (rowAnswer.status == OpStatus::Ok && rowAnswer.value) derived.values[row] = *rowAnswer.value;
		else derived.undetermined.insert(row);
	}
	tables::ProtectedTable *owner = mutable_table(context, set->table);
	if (owner == nullptr) return error_answer(Code::UnknownSymbol, 1);
	owner->derived_columns.erase(std::remove_if(owner->derived_columns.begin(), owner->derived_columns.end(),
		[&](const tables::DerivedColumn &existing) { return existing.label == derived.label; }), owner->derived_columns.end());
	owner->derived_columns.push_back(std::move(derived));
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	ArgType column;
	column.kind = TypeKind::Column;
	column.table = set->table;
	column.element = written(rowType->type);
	answer.result = result_of(column);
	answer.roots = roots;
	return answer;
}

// ---------------------------------------------------------------------------
// group, aggregate

long long add_months(long long day, long long months) {
	// Civil arithmetic: the same day of the month, clamped to its end.
	long long days = day + 719468;
	const long long era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(days - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long year = static_cast<long long>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	unsigned date = doy - (153 * mp + 2) / 5 + 1;
	unsigned month = mp < 10 ? mp + 3 : mp - 9;
	if (month <= 2) ++year;
	long long total = year * 12 + (month - 1) + months;
	year = total >= 0 ? total / 12 : (total - 11) / 12;
	month = static_cast<unsigned>(total - year * 12 + 1);
	static const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned last = lengths[month - 1];
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) last = 29;
	date = std::min(date, last);
	long long y = year - (month <= 2 ? 1 : 0);
	const long long e = (y >= 0 ? y : y - 399) / 400;
	const unsigned ye = static_cast<unsigned>(y - e * 400);
	const unsigned dy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + date - 1;
	const unsigned de = ye * 365 + ye / 4 - ye / 100 + dy;
	return e * 146097 + static_cast<long long>(de) - 719468;
}

long long unit_months(const std::string &unit) { return unit == "years" ? 12 : unit == "quarters" ? 3 : 1; }

// The period index of `day` counted from `anchor` in units of `months`.
long long period_index(long long day, long long anchor, long long months) {
	long long index = 0;
	if (day >= anchor) {
		while (add_months(anchor, (index + 1) * months) <= day && index < 100000) ++index;
	} else {
		while (add_months(anchor, index * months) > day && index > -100000) --index;
	}
	return index;
}

std::string unit_singular(const std::string &unit) {
	return unit == "years" ? "year" : unit == "quarters" ? "quarter" : "month";
}

OpAnswer evaluate_group(const std::vector<Operand> &given, const OpOptions &options, OpContext &context) {
	if (given.size() != 2) return error_answer(Code::Arity);
	const Operand base = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(base);
	if (!set || base.table == nullptr) return error_answer(Code::TypeMismatch, 1, {}, "ROW_SET");
	const ColumnRef key = column_ref(settled(given[1], context));
	if (key.table == nullptr || key.column == nullptr || !key.element) return error_answer(Code::UnknownColumn, 2);
	if (key.table->handle != set->table) return error_answer(Code::CrossTable, 2);
	const bool dateKey = key.element->kind == TypeKind::Date;
	if (!dateKey && key.element->kind != TypeKind::Entity && key.element->kind != TypeKind::Word)
		return error_answer(Code::GroupKeyType, 2);
	if (dateKey && !date_unit(options.unit)) return error_answer(Code::UnitRequired, 2);
	std::optional<long long> anchor;
	std::string anchorKey;
	if (options.anchor) {
		AtomValue anchorValue;
		const std::string &written = *options.anchor;
		bool found = false;
		if (!written.empty() && written.front() == '@' && context.table != nullptr) {
			const auto bound = context.table->compute_labels.find(written.substr(1));
			found = bound != context.table->compute_labels.end() && scalar_of(context, bound->second, anchorValue);
		} else if (sent(context, written)) {
			found = scalar_of(context, written, anchorValue);
		}
		if (!found || !anchorValue.value || anchorValue.value->kind != Kind::Date) return error_answer(Code::UnknownSymbol, 0, "anchor");
		anchor = anchorValue.value->date.first_day;
		anchorKey = anchorValue.fact_key;
	}
	tables::GroupSet groups;
	groups.table = set->table;
	groups.key_column = key.name;
	groups.date_key = dateKey;
	groups.unit = options.unit.value_or("");
	groups.anchor = anchorKey;
	groups.key_group_key = tables::group_key(*key.table, key.name);
	// The partition is charged to the key cells of the whole base, not of the
	// members a value chose (D4).
	const std::vector<std::size_t> readOver = set->structural ? members(*set) : base_of(*set);
	std::vector<std::string> baseKeyRoots;
	for (const std::size_t row : readOver) add_roots(baseKeyRoots, value_at(key, row).roots);
	add_roots(groups.key_roots, std::set<std::string>(baseKeyRoots.begin(), baseKeyRoots.end()));
	std::map<std::string, std::size_t> index;
	for (const std::size_t row : members(*set)) {
		const RowValue read = value_at(key, row);
		std::string keyText;
		std::string words;
		std::optional<Value> period;
		bool undetermined = contains(set->undetermined, row);
		if (dateKey) {
			if (!read.value || read.value->kind != Kind::Date) {
				undetermined = true;
				keyText = "?";
			} else {
				const long long first = read.value->date.first_day;
				const long long last = read.value->date.last_day;
				const long long months = unit_months(groups.unit);
				if (anchor) {
					const long long at = period_index(first, *anchor, months);
					if (period_index(last, *anchor, months) != at) undetermined = true;
					keyText = std::to_string(at);
					words = unit_singular(groups.unit) + " " + std::to_string(at + 1);
				} else {
					// Calendar periods: counted from January 1970.
					const auto calendar = [&](long long day) { return period_index(day, 0, months); };
					const long long at = calendar(first);
					if (calendar(last) != at) undetermined = true;
					keyText = std::to_string(at);
					const long long startDay = add_months(0, at * months);
					const long long endDay = add_months(0, (at + 1) * months) - 1;
					temporal::TemporalValue interval{startDay, endDay,
						months == 12 ? temporal::Precision::Year
						: months == 3 ? temporal::Precision::Quarter : temporal::Precision::Month};
					period = semantic::make_date(interval);
				}
			}
		} else {
			const tables::TableCell *cell = read.cell;
			if (cell == nullptr || fold(cell->text).empty()) {
				keyText = "";
				words = "(blank)";
			} else if (!cell->placeholder.empty()) {
				keyText = cell->placeholder;
				words = cell->placeholder;
			} else {
				keyText = fold(cell->text);
				words = cell->text;
			}
		}
		auto found = index.find(keyText);
		if (found == index.end()) {
			found = index.emplace(keyText, groups.groups.size()).first;
			tables::RowSet members;
			members.table = set->table;
			members.canonical = set->canonical + "#" + std::to_string(groups.groups.size() + 1);
			members.roots = set->roots;
			members.cell_roots = set->cell_roots;
			add_roots(members.cell_roots, std::set<std::string>(baseKeyRoots.begin(), baseKeyRoots.end()));
			members.operand_roots = set->operand_roots;
			members.group_keys = set->group_keys;
			if (std::find(members.group_keys.begin(), members.group_keys.end(), groups.key_group_key) == members.group_keys.end())
				members.group_keys.push_back(groups.key_group_key);
			members.base_rows = readOver;
			members.base_size = readOver.size();
			members.may_be_bounded = set->may_be_bounded;
			groups.groups.emplace_back(keyText, members);
			groups.key_words.push_back(words);
			groups.key_periods.push_back(period);
		}
		tables::RowSet &into = groups.groups[found->second].second;
		(undetermined ? into.undetermined : into.certain).push_back(row);
	}
	groups.canonical = "group(" + set->canonical + "," + key.name + "," + groups.unit + "," + anchorKey + ")";
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	ArgType type;
	type.kind = TypeKind::GroupSet;
	type.table = set->table;
	type.key = written(*key.element);
	answer.result = result_of(type);
	answer.roots.insert(set->roots.begin(), set->roots.end());
	answer.roots.insert(groups.key_roots.begin(), groups.key_roots.end());
	answer.groups = groups;
	if (context.node != nullptr && context.node->bind)
		if (tables::ProtectedTable *owner = mutable_table(context, set->table)) owner->group_sets["@" + *context.node->bind] = groups;
	return answer;
}

OpAnswer evaluate_aggregate(const std::vector<Operand> &given, const OpOptions &options, OpContext &context) {
	if (given.size() != 2) return error_answer(Code::Arity);
	const Operand first = settled(given[0], context);
	if (!first.groups) return error_answer(Code::TypeMismatch, 1, {}, "GROUP_SET");
	const tables::GroupSet &groups = *first.groups;
	const ColumnRef column = column_ref(settled(given[1], context));
	if (column.table == nullptr || !column.element) return error_answer(Code::UnknownColumn, 2);
	if (column.table->handle != groups.table) return error_answer(Code::CrossTable, 2);
	const std::string fn = options.fn.value_or("");
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	std::optional<ArgType> type;
	for (std::size_t index = 0; index < groups.groups.size(); ++index) {
		const tables::RowSet &members = groups.groups[index].second;
		if (!members.undetermined.empty()) return unknown_answer(Code::PrecisionInsufficient);
		std::vector<Operand> operands(2);
		operands[0].rows = members;
		operands[0].table = column.table;
		operands[1] = given[1];
		operands[1].table = column.table;
		OpAnswer one;
		if (fn == "count") {
			one = count_value(members.certain.size(), 0);
			one.root_sources = row_sources(column.table, members.certain);
		}
		else if (fn == "sum") one = evaluate_sum_column(operands, options, context);
		else if (fn == "average") one = evaluate_average_column(operands, options, context);
		else if (fn == "min") one = evaluate_extreme_column<true>(operands, options, context);
		else if (fn == "max") one = evaluate_extreme_column<false>(operands, options, context);
		else return error_answer(Code::Arity, 0, "options.fn sum|count|min|max|average");
		if (one.status == OpStatus::Error || one.status == OpStatus::Refused) return one;
		if (one.status != OpStatus::Ok || !one.value) return unknown_answer(one.code == Code::None ? Code::PrecisionInsufficient : one.code);
		type = one.result.type;
		answer.roots.insert(one.roots.begin(), one.roots.end());
		answer.root_sources.insert(answer.root_sources.end(), one.root_sources.begin(), one.root_sources.end());
		if (answer.pairs.size() < kListedRows) answer.pairs.emplace_back(key_written(groups, index, context), *one.value);
	}
	answer.roots.insert(groups.key_roots.begin(), groups.key_roots.end());
	answer.result = result_of(type ? *type : total_of(*column.element));
	return answer;
}

// ---------------------------------------------------------------------------
// lookup, progressive

bool below(const Value &a, const Value &b, bool orEqual) {
	try {
		Value left = a;
		Value right = b;
		if (left.kind == Kind::Money && right.kind == Kind::Money) left.basis = right.basis = semantic::RateBasis::None;
		const semantic::Relation relation = semantic::compare(left,
			orEqual ? semantic::Comparator::LessOrEqual : semantic::Comparator::Less, right);
		return relation.truth == semantic::Truth::True;
	} catch (const semantic::AlgebraError &) {
		return false;
	}
}

// The band of `schedule` that holds `value` (DESIGN4 §6.6).
const tables::ScheduleBand *band_for(const tables::ProtectedSchedule &schedule, const Value &value, const std::string &mode) {
	const tables::ScheduleBand *chosen = nullptr;
	for (const tables::ScheduleBand &band : schedule.bands) {
		bool inside = true;
		if (band.lower_value) inside = inside && below(*band.lower_value, value, band.lower_inclusive);
		if (band.upper_value) inside = inside && below(value, *band.upper_value, band.upper_inclusive);
		if (inside) {
			chosen = &band;
			if (mode != "ge") break;
		}
	}
	return chosen;
}

OpAnswer evaluate_lookup(const std::vector<Operand> &given, const OpOptions &options, OpContext &context) {
	const std::string mode = options.mode.value_or("le");
	if (given.size() == 2) {
		const Operand schedule = settled(given[0], context);
		if (schedule.schedule == nullptr) return error_answer(Code::TypeMismatch, 1, {}, "SCHEDULE");
		const Operand &value = given[1];
		if (!value.value) return error_answer(Code::TypeMismatch, 2);
		const tables::ScheduleBand *band = band_for(*schedule.schedule, *value.value, mode);
		OpAnswer answer;
		answer.roots = value.roots;
		answer.roots.insert(schedule.roots.begin(), schedule.roots.end());
		if (band == nullptr || !band->band_value) {
			answer = unknown_answer(Code::EmptySet);
			return answer;
		}
		answer.status = OpStatus::Ok;
		const std::optional<ArgType> type = tables::parse_type(schedule.schedule->value_type);
		answer.result = result_of(type ? *type : ArgType{});
		answer.value = band->band_value;
		answer.root_sources.push_back(schedule.schedule->source);
		return answer;
	}
	if (given.size() != 4) return error_answer(Code::Arity);
	const Operand tableOperand = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(tableOperand);
	if (!set || tableOperand.table == nullptr) return error_answer(Code::TypeMismatch, 1, {}, "TABLE");
	const ColumnRef key = column_ref(settled(given[1], context));
	const ColumnRef result = column_ref(settled(given[3], context));
	if (key.table == nullptr || !key.element) return error_answer(Code::UnknownColumn, 2);
	if (result.table == nullptr || !result.element) return error_answer(Code::UnknownColumn, 4);
	const Operand &value = given[2];
	if (!value.value) return error_answer(Code::TypeMismatch, 3);
	OpAnswer answer;
	answer.roots = value.roots;
	std::optional<std::size_t> best;
	std::optional<Value> bestKey;
	// The result rests on every key cell and every result cell of the table,
	// whichever band the value fell in (D4).
	for (const std::size_t row : base_of(*set)) {
		const RowValue own = value_at(result, row);
		answer.roots.insert(own.roots.begin(), own.roots.end());
	}
	for (const std::size_t row : members(*set)) {
		const RowValue read = value_at(key, row);
		answer.roots.insert(read.roots.begin(), read.roots.end());
		if (!read.value) continue;
		const bool fits = mode == "ge" ? below(*value.value, *read.value, true) : below(*read.value, *value.value, true);
		if (!fits) continue;
		const bool better = !bestKey || (mode == "ge" ? below(*read.value, *bestKey, false) : below(*bestKey, *read.value, false));
		if (better) {
			best = row;
			bestKey = read.value;
		}
	}
	if (!best) return unknown_answer(Code::EmptySet);
	const RowValue chosen = value_at(result, *best);
	if (!chosen.value) return unknown_answer(Code::EmptySet);
	answer.root_sources = chosen.sources;
	answer.status = OpStatus::Ok;
	answer.result = result_of(*result.element);
	answer.value = chosen.value;
	return answer;
}

OpAnswer evaluate_progressive(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.size() != 2) return error_answer(Code::Arity);
	const Operand &value = given[0];
	const Operand schedule = settled(given[1], context);
	if (schedule.schedule == nullptr) return error_answer(Code::TypeMismatch, 2, {}, "SCHEDULE");
	if (!value.value) return error_answer(Code::TypeMismatch, 1);
	const std::optional<ExactRational> amount = exact_of(*value.value);
	if (!amount) return error_answer(Code::TypeMismatch, 1);
	ExactRational total;
	for (const tables::ScheduleBand &band : schedule.schedule->bands) {
		if (!band.band_value || band.band_value->kind != Kind::Percentage) return error_answer(Code::TypeMismatch, 2);
		ExactRational lower;
		if (band.lower_value) {
			const std::optional<ExactRational> own = exact_of(*band.lower_value);
			if (!own) return error_answer(Code::TypeMismatch, 2);
			lower = *own;
		}
		ExactRational upper = *amount;
		if (band.upper_value) {
			const std::optional<ExactRational> own = exact_of(*band.upper_value);
			if (!own) return error_answer(Code::TypeMismatch, 2);
			if (*own < upper) upper = *own;
		}
		if (upper > lower) total = total + (upper - lower) * ExactRational::from(band.band_value->amount);
	}
	ArgType type = value.type;
	if (type.kind == TypeKind::Count) type.kind = TypeKind::Number;
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	answer.result = result_of(type);
	answer.value = value_of(type, total, value.value->source_decimals);
	if (answer.value && value.value->kind == Kind::Money) answer.value->currency = value.value->currency;
	if (!answer.value) return error_answer(Code::DepthExceeded);
	answer.roots = value.roots;
	answer.roots.insert(schedule.roots.begin(), schedule.roots.end());
	answer.root_sources.push_back(schedule.schedule->source);
	return answer;
}

// ---------------------------------------------------------------------------
// cell, list_rows, recalc, describe_tables

// The cell an operation names: [[Table1]].B12 (with the table before it or not).
const tables::TableCell *named_cell(const std::vector<Operand> &given, OpContext &context,
		const tables::ProtectedTable *&table) {
	const Operand &last = given.back();
	if (last.arg.kind != ArgKind::CellRef || context.table == nullptr) return nullptr;
	table = last.table != nullptr ? last.table : tables::find_table(*context.table, last.arg.symbol);
	if (table == nullptr) return nullptr;
	return tables::cell_of(*table, last.arg.row, last.arg.column);
}

OpAnswer evaluate_cell(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.empty() || given.size() > 2) return error_answer(Code::Arity);
	const tables::ProtectedTable *table = nullptr;
	const tables::TableCell *cell = named_cell(given, context, table);
	if (cell == nullptr || table == nullptr || !cell->value) return error_answer(Code::UnknownSymbol, given.size());
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	answer.result = result_of(given.back().type);
	answer.value = cell->value;
	answer.roots = {tables::cell_root(*table, *cell)};
	answer.root_sources = {cell->source};
	std::string symbol = cell->placeholder;
	if (symbol.empty() && context.issue_cell) {
		symbol = context.issue_cell(*table, *cell);
		if (tables::ProtectedTable *owner = mutable_table(context, table->handle))
			for (tables::TableRow &row : owner->rows)
				for (tables::TableCell &own : row.cells)
					if (own.ref == cell->ref) own.placeholder = symbol;
	}
	// The cell's own fact: its existing symbol, never a new derived one.
	if (!symbol.empty()) answer.results = {symbol};
	return answer;
}

OpAnswer evaluate_list_rows(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.size() != 1) return error_answer(Code::Arity);
	const Operand operand = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(operand);
	if (!set) return error_answer(Code::TypeMismatch, 1, {}, "ROW_SET");
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	answer.result = result_of(word_type(), true);
	std::vector<std::size_t> listed = set->certain;
	std::sort(listed.begin(), listed.end());
	std::string words;
	std::size_t shown = 0;
	for (const std::size_t row : listed) {
		if (shown == kListedRows) break;
		words += (shown == 0 ? "r" : " r") + std::to_string(row);
		++shown;
	}
	if (listed.empty()) words = "none";
	if (listed.size() > kListedRows) words += " and " + std::to_string(listed.size() - kListedRows) + " more";
	// Rows the predicate could not decide, named as rows - never a bare count,
	// which a model reads as a row number (live EN1: "undetermined: 4" was read
	// as the hidden row 4, and the answer left its quarter open).
	if (!set->undetermined.empty()) {
		std::vector<std::size_t> open = set->undetermined;
		std::sort(open.begin(), open.end());
		words += "; not decided: ";
		for (std::size_t index = 0; index < open.size() && index < kListedRows; ++index)
			words += (index == 0 ? "r" : " r") + std::to_string(open[index]);
		if (open.size() > kListedRows) words += " and " + std::to_string(open.size() - kListedRows) + " more";
	}
	answer.words = words;
	tables::RowSet shownSet = *set;
	shownSet.certain.assign(listed.begin(), listed.begin() + static_cast<std::ptrdiff_t>(shown));
	answer.rows = shownSet;
	answer.roots.insert(set->roots.begin(), set->roots.end());
	return answer;
}

OpAnswer evaluate_recalc(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.empty() || given.size() > 2) return error_answer(Code::Arity);
	const tables::ProtectedTable *table = nullptr;
	const tables::TableCell *cell = named_cell(given, context, table);
	if (cell == nullptr || table == nullptr) return error_answer(Code::UnknownSymbol, given.size());
	if (cell->formula.empty()) return error_answer(Code::NotEvaluable, given.size(), "that cell holds no formula");
	const tables::FormulaOutcome outcome = tables::recalc(*table, *cell);
	OpAnswer answer;
	answer.roots.clear();
	answer.root_sources.clear();
	answer.roots.insert(tables::cell_root(*table, *cell));
	for (const std::string &input : tables::formula_inputs(*table, *cell))
		if (const tables::TableCell *read = tables::cell_of(*table, input)) {
			answer.roots.insert(tables::cell_root(*table, *read));
			answer.root_sources.push_back(read->source);
		}
	if (outcome.kind == tables::FormulaOutcome::Kind::NotEvaluable)
		return error_answer(Code::NotEvaluable, given.size(), "the formula uses what Singularity does not work out");
	if (outcome.kind == tables::FormulaOutcome::Kind::EvaluationFailed) {
		OpAnswer failed = unknown_answer(Code::EvaluationFailed);
		failed.roots = answer.roots;
		return failed;
	}
	const tables::TableColumn *column = tables::column_of(*table, cell->ref.substr(0, cell->ref.find_first_of("0123456789")));
	const std::optional<ArgType> type = column != nullptr ? tables::parse_type(column->type) : std::nullopt;
	ArgType resultType = type ? *type : given.back().type;
	// A total row of a column owed per period holds the total.
	if (const tables::TableRow *row = tables::row_of(*table, cell->row);
			row != nullptr && (row->kind == tables::RowKind::Total || row->kind == tables::RowKind::Subtotal))
		resultType = total_of(resultType);
	answer.status = OpStatus::Ok;
	answer.result = result_of(resultType);
	// The evaluator works in Excel's serial days: a date column's result is
	// the day it names (a fraction of a day is no date: evaluation_failed).
	constexpr long long kSerialOfEpoch = 25569;
	if (resultType.kind == TypeKind::Date) {
		const std::optional<Rational> serial = outcome.value->to_rational();
		if (!serial || !serial->is_integer()) {
			OpAnswer failed = unknown_answer(Code::EvaluationFailed);
			failed.roots = answer.roots;
			return failed;
		}
		const long long day = static_cast<long long>(serial->numerator()) - kSerialOfEpoch;
		answer.value = semantic::make_date(temporal::TemporalValue{day, day, temporal::Precision::Day});
	} else {
		answer.value = value_of(resultType, *outcome.value, cell->value ? cell->value->source_decimals : 0);
	}
	if (!answer.value) return error_answer(Code::DepthExceeded);
	// Whether the value the file was saved with still holds: an observation.
	bool equal = false;
	if (cell->value) {
		if (cell->value->kind == Kind::Date) {
			// The day its value names; a value shown as a month or a year keeps
			// the serial the file saved.
			std::optional<ExactRational> saved;
			if (cell->value->date.first_day == cell->value->date.last_day)
				saved = ExactRational(semantic::BigInt(cell->value->date.first_day + kSerialOfEpoch));
			else if (cell->cached)
				saved = ExactRational::parse_decimal(*cell->cached);
			equal = saved && *saved == *outcome.value;
		}
		else if (const std::optional<ExactRational> saved = exact_of(*cell->value))
			equal = *saved == *outcome.value;
	}
	answer.relation = equal ? "TRUE" : "FALSE";
	return answer;
}

OpAnswer evaluate_describe_tables(const std::vector<Operand> &given, const OpOptions &, OpContext &context) {
	if (given.size() != 1) return error_answer(Code::Arity);
	if (context.table == nullptr) return error_answer(Code::UnknownSymbol, 1);
	const std::string &document = given[0].arg.symbol.empty() ? given[0].arg.written : given[0].arg.symbol;
	std::string words;
	bool known = false;
	for (tables::ProtectedTable &table : context.table->tables) {
		if (table.document_handle != document && table.document_id != document) continue;
		known = true;
		if (table.described && table.described_columns.size() == table.columns.size()) continue;
		table.described = true;
		table.described_columns.clear();
		for (const tables::TableColumn &column : table.columns) table.described_columns.push_back(column.letter);
		words += (words.empty() ? "" : " ") + std::string("[[") + table.handle + "]]";
	}
	if (!known) return error_answer(Code::UnknownSymbol, 1);
	OpAnswer answer;
	answer.status = OpStatus::Ok;
	answer.result = result_of(word_type(), true);
	answer.words = words.empty() ? "already described" : words;
	return answer;
}

// ---------------------------------------------------------------------------
// Pre-charges (DESIGN4 §8.3): from the request and the table's structure,
// before anything is evaluated.

std::size_t bits_for(std::size_t count) {
	std::size_t bits = 0;
	while ((static_cast<std::size_t>(1) << bits) < count + 1) ++bits;
	return bits;
}

std::optional<ObservationCharge> charge_of(const std::string &key, const std::set<std::string> &roots,
		const std::vector<std::string> &groupKeys) {
	ObservationCharge charge;
	charge.key = key;
	charge.roots.assign(roots.begin(), roots.end());
	charge.group_keys = groupKeys;
	return charge;
}

// A sum, a count, an average or an aggregate over a set that may be bounded;
// a minimum, a maximum or an average over one that may be empty; a signed
// difference.
std::optional<ObservationCharge> pre_charge_set_aggregate(const std::vector<Operand> &given, const OpOptions &options,
		const OpContext &context, bool mayBeEmpty) {
	if (given.empty()) return std::nullopt;
	const Operand base = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(base);
	if (!set) return std::nullopt;
	ColumnRef column;
	if (given.size() >= 2) column = column_ref(settled(given[1], context));
	// A blank cell of the column can leave a minimum or an average of a
	// structural set with nothing to read; a derived row not worked out (a
	// blank operand, a row op that failed) makes a sum a bound or unknown.
	const bool sparse = column.column != nullptr && column.column->sparse;
	const bool emptyPossible = mayBeEmpty && (!set->structural || sparse || column.derived != nullptr);
	const bool derivedRows = column.derived != nullptr;
	const bool signedDifference = difference_column(column) && revealed(options);
	if (!set->may_be_bounded && !emptyPossible && !derivedRows && !signedDifference) return std::nullopt;
	std::set<std::string> roots;
	if (!membership_disclosed(*set, context)) roots.insert(set->roots.begin(), set->roots.end());
	std::vector<std::string> groupKeys = set->group_keys;
	if (column.table != nullptr) {
		// Over the base the set was read over, never its members alone (D4).
		for (const std::size_t row : set->structural ? members(*set) : base_of(*set)) {
			const RowValue read = value_at(column, row);
			roots.insert(read.roots.begin(), read.roots.end());
		}
		const std::string key = column.derived != nullptr ? "derived:" + tables::place_key(*column.table) + ":" + column.name
			: tables::group_key(*column.table, column.name);
		if (std::find(groupKeys.begin(), groupKeys.end(), key) == groupKeys.end()) groupKeys.push_back(key);
		if (column.derived != nullptr)
			for (const std::string &base : column.derived->base_group_keys)
				if (std::find(groupKeys.begin(), groupKeys.end(), base) == groupKeys.end()) groupKeys.push_back(base);
	}
	const std::string op = context.node != nullptr ? context.node->op : std::string("aggregate");
	return charge_of(op + "(" + set->canonical + "," + column.name + ")", roots, groupKeys);
}

std::optional<ObservationCharge> pre_charge_count(const std::vector<Operand> &given, const OpOptions &options,
		const OpContext &context) {
	if (!given.empty()) {
		const Operand operand = settled(given[0], context);
		if (operand.groups) {
			// Listing a partition: every member's key cell, and the key column.
			const tables::GroupSet &groups = *operand.groups;
			ObservationCharge charge;
			charge.key = "partition(" + groups.canonical + ")";
			charge.roots = groups.key_roots;
			if (groups.date_key) charge.calendar_roots = groups.key_roots;
			charge.group_keys = {groups.key_group_key};
			return charge;
		}
	}
	return pre_charge_set_aggregate(given, options, context, false);
}

std::optional<ObservationCharge> pre_charge_sum(const std::vector<Operand> &given, const OpOptions &options,
		const OpContext &context) {
	return pre_charge_set_aggregate(given, options, context, false);
}

std::optional<ObservationCharge> pre_charge_nonempty(const std::vector<Operand> &given, const OpOptions &options,
		const OpContext &context) {
	return pre_charge_set_aggregate(given, options, context, true);
}

std::optional<ObservationCharge> pre_charge_aggregate(const std::vector<Operand> &given, const OpOptions &options,
		const OpContext &context) {
	if (given.size() != 2) return std::nullopt;
	const Operand operand = settled(given[0], context);
	if (!operand.groups) return std::nullopt;
	const tables::GroupSet &groups = *operand.groups;
	const ColumnRef column = column_ref(settled(given[1], context));
	ObservationCharge charge;
	charge.key = "partition(" + groups.canonical + ")";
	charge.roots = groups.key_roots;
	if (groups.date_key) charge.calendar_roots = groups.key_roots;
	charge.group_keys = {groups.key_group_key};
	for (const auto &[key, members] : groups.groups) {
		static_cast<void>(key);
		for (const std::string &root : members.roots)
			if (std::find(charge.roots.begin(), charge.roots.end(), root) == charge.roots.end()) charge.roots.push_back(root);
	}
	// Whether each group's figure can be worked out at all can rest on the
	// aggregated cells: a derived row not worked out, a blank cell a minimum or
	// an average reads, a set that may bound. Then they are charged too, over
	// the whole base of the groups.
	const std::string fn = options.fn.value_or("");
	const bool mayFail = (column.derived != nullptr && fn != "count")
		|| (fn != "count" && fn != "sum" && column.column != nullptr && column.column->sparse);
	bool bounded = false;
	for (const auto &[key, members] : groups.groups) {
		static_cast<void>(key);
		bounded = bounded || members.may_be_bounded;
	}
	if (column.table != nullptr && (mayFail || bounded) && !groups.groups.empty()) {
		const tables::RowSet &any = groups.groups.front().second;
		for (const std::size_t row : base_of(any)) {
			const RowValue read = value_at(column, row);
			for (const std::string &root : read.roots)
				if (std::find(charge.roots.begin(), charge.roots.end(), root) == charge.roots.end()) charge.roots.push_back(root);
		}
		const std::string key = column.derived != nullptr ? "derived:" + tables::place_key(*column.table) + ":" + column.name
			: tables::group_key(*column.table, column.name);
		if (std::find(charge.group_keys.begin(), charge.group_keys.end(), key) == charge.group_keys.end())
			charge.group_keys.push_back(key);
	}
	return charge;
}

// list_rows: one membership bit per cell a predicate read, and the rank of
// each other operand among the N rows (ceil(log2(N+1)) each, written as that
// many charges of the root), and each column's group key (DESIGN4 §6.5).
std::optional<ObservationCharge> pre_charge_list_rows(const std::vector<Operand> &given, const OpOptions &,
		const OpContext &context) {
	if (given.size() != 1) return std::nullopt;
	const Operand operand = settled(given[0], context);
	const std::optional<tables::RowSet> set = set_of(operand);
	if (!set) return std::nullopt;
	ObservationCharge charge;
	charge.key = "list_rows(" + set->canonical + ")";
	charge.roots = set->cell_roots;
	const std::size_t rank = bits_for(set->base_size);
	for (const std::string &root : set->operand_roots)
		for (std::size_t times = 0; times < std::max<std::size_t>(rank, 1); ++times) charge.roots.push_back(root);
	charge.group_keys = set->group_keys;
	if (charge.roots.empty() && set->structural) {
		// A set of hidden/visible rows: the tables block already states it.
		return std::nullopt;
	}
	return charge;
}

std::optional<ObservationCharge> pre_charge_recalc(const std::vector<Operand> &given, const OpOptions &,
		const OpContext &context) {
	if (given.empty() || context.table == nullptr) return std::nullopt;
	const Operand &last = given.back();
	const tables::ProtectedTable *table = last.table != nullptr ? last.table : tables::find_table(*context.table, last.arg.symbol);
	if (table == nullptr) return std::nullopt;
	const tables::TableCell *cell = tables::cell_of(*table, last.arg.row, last.arg.column);
	if (cell == nullptr || cell->formula.empty()) return std::nullopt;
	// The cells the formula reads, from its text alone.
	std::set<std::string> roots{tables::cell_root(*table, *cell)};
	std::vector<std::string> groupKeys;
	for (const std::string &input : tables::formula_inputs(*table, *cell))
		if (const tables::TableCell *read = tables::cell_of(*table, input)) {
			roots.insert(tables::cell_root(*table, *read));
			const std::string letters = input.substr(0, input.find_first_of("0123456789"));
			const std::string key = tables::group_key(*table, letters);
			if (std::find(groupKeys.begin(), groupKeys.end(), key) == groupKeys.end()) groupKeys.push_back(key);
		}
	return charge_of("recalc(" + tables::place_key(*table) + ":" + cell->ref + ")", roots, groupKeys);
}

// lookup and progressive: which band a value falls in, and whether any does,
// rest on the value and on every threshold and band value - a schedule's
// facts, or a table's key and result cells. The value's roots are charged
// ceil(log2(bands+1)) times (written as that many repetitions, as list_rows
// does: the band reveals its rank among the thresholds), every threshold and
// band value once (DESIGN4 §6.6, §8.3).
std::optional<ObservationCharge> pre_charge_band(const std::vector<Operand> &given, const OpOptions &,
		const OpContext &context) {
	std::set<std::string> roots;
	std::set<std::string> valueRoots;
	std::size_t bands = 0;
	std::string schedule;
	std::string value;
	const auto take_value = [&](const Operand &operand) {
		valueRoots.insert(operand.roots.begin(), operand.roots.end());
		value = operand.fact_key.empty() ? operand.arg.written : operand.fact_key;
	};
	if (given.size() == 4) {
		// lookup(table, key column, value, result column).
		const Operand table = settled(given[0], context);
		const std::optional<tables::RowSet> set = set_of(table);
		if (set && table.table != nullptr) {
			const std::vector<std::size_t> rows = base_of(*set);
			bands = rows.size();
			schedule = tables::place_key(*table.table);
			for (const std::size_t index : {std::size_t{1}, std::size_t{3}}) {
				const ColumnRef column = column_ref(settled(given[index], context));
				if (column.table == nullptr) continue;
				for (const std::size_t row : rows) {
					const RowValue read = value_at(column, row);
					roots.insert(read.roots.begin(), read.roots.end());
				}
			}
		}
		take_value(given[2]);
	} else {
		for (const Operand &raw : given) {
			const Operand operand = settled(raw, context);
			if (operand.schedule != nullptr) {
				bands = operand.schedule->bands.size();
				schedule = operand.schedule->handle;
				roots.insert(operand.roots.begin(), operand.roots.end());
				for (const tables::ScheduleBand &band : operand.schedule->bands)
					for (const std::string *fact : {&band.lower_fact, &band.upper_fact, &band.value_fact})
						if (!fact->empty()) roots.insert(*fact);
				continue;
			}
			if (operand.value) take_value(operand);
		}
	}
	if (roots.empty() && valueRoots.empty()) return std::nullopt;
	ObservationCharge charge;
	charge.key = "band(" + schedule + "," + value + ")";
	for (const std::string &root : roots)
		if (valueRoots.count(root) == 0) charge.roots.push_back(root);
	const std::size_t rank = std::max<std::size_t>(bits_for(bands), 1);
	for (const std::string &root : valueRoots)
		for (std::size_t times = 0; times < rank; ++times) charge.roots.push_back(root);
	return charge;
}

// A derived column that divides: the divisor's zero allowance.
std::optional<ObservationCharge> pre_charge_derive_column(const std::vector<Operand> &given, const OpOptions &,
		const OpContext &) {
	if (given.size() < 4) return std::nullopt;
	const std::string op = !given[1].arg.constant.empty() ? given[1].arg.constant : given[1].arg.written;
	if (op != "divide" && op != "ratio" && op != "percentage_change") return std::nullopt;
	const Operand &divisor = given[3];
	ObservationCharge charge;
	charge.key = "zero(" + (divisor.fact_key.empty() ? divisor.arg.written : divisor.fact_key) + ")";
	charge.zero_divisors = {charge.key};
	return charge;
}

} // namespace

void register_table_family() {
	register_family("tables", {
		{"filter", type_filter, evaluate_filter, ObservationClass::Never, nullptr},
		{"count", type_count, evaluate_count, ObservationClass::Conditional, pre_charge_count},
		{"sum_column", type_sum_column, evaluate_sum_column, ObservationClass::Conditional, pre_charge_sum},
		{"average_column", type_average_column, evaluate_average_column, ObservationClass::Conditional, pre_charge_nonempty},
		{"min_column", type_min_max_column, evaluate_extreme_column<true>, ObservationClass::Conditional, pre_charge_nonempty},
		{"max_column", type_min_max_column, evaluate_extreme_column<false>, ObservationClass::Conditional, pre_charge_nonempty},
		{"derive_column", type_derive_column, evaluate_derive_column, ObservationClass::Conditional, pre_charge_derive_column},
		{"group", type_group, evaluate_group, ObservationClass::Never, nullptr},
		{"aggregate", type_aggregate, evaluate_aggregate, ObservationClass::Always, pre_charge_aggregate},
		{"union", type_set_algebra, evaluate_set_algebra<SetOp::Union>, ObservationClass::Never, nullptr},
		{"intersect", type_set_algebra, evaluate_set_algebra<SetOp::Intersect>, ObservationClass::Never, nullptr},
		{"minus", type_set_algebra, evaluate_set_algebra<SetOp::Minus>, ObservationClass::Never, nullptr},
		{"lookup", type_lookup, evaluate_lookup, ObservationClass::Always, pre_charge_band},
		{"progressive", type_progressive, evaluate_progressive, ObservationClass::Always, pre_charge_band},
		{"cell", type_cell, evaluate_cell, ObservationClass::Never, nullptr},
		{"list_rows", type_list_rows, evaluate_list_rows, ObservationClass::Always, pre_charge_list_rows},
		{"recalc", type_recalc, evaluate_recalc, ObservationClass::Always, pre_charge_recalc},
		{"describe_tables", type_describe_tables, evaluate_describe_tables, ObservationClass::Never, nullptr},
	});
}

} // namespace singularity::protected_api::papinian::compute
