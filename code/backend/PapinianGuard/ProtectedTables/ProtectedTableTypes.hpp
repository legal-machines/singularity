#pragma once

#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The local table model of the protected computation protocol (DESIGN4 §6.1):
// a spreadsheet, a Word table or a CSV of the lawyer's material, held on this
// computer whole - every row, hidden ones included, with its formulas and the
// values it was saved with - and addressed by the model only through a handle
// ([[Table1]]), a column letter and a row number. Data only: the operation
// table (OperationProtectedSymbolTable::tables) keeps these for the life of
// one operation; ProtectedTables builds them and the table family of
// ProtectedOperations computes over them. Never serialized, never sent.
namespace singularity::protected_api::papinian::tables {

// What a cell holds, as the file stores it: a formula's cell is the kind of the
// value it was saved with.
enum class CellKind { Empty, Number, Text, Date, Boolean, Error };

struct TableCell {
	// "B12", its row and column (1-based, as the sheet numbers them).
	std::string ref;
	std::size_t row = 0;
	std::size_t column = 0;
	CellKind kind = CellKind::Empty;
	// Parsed with the column's hints (currency, percent display, date).
	std::optional<semantic::Value> value;
	// What the cell shows (local).
	std::string text;
	// "=B10+B11", "=SUBTOTAL(109,B4:B18)"; empty for a plain value.
	std::string formula;
	// The value the file was saved with, when it carries one.
	std::optional<std::string> cached;
	bool hidden_row = false;
	bool hidden_column = false;
	// The registry row this cell's fact is; empty for structure.
	std::string fact_key;
	std::string placeholder;
	// The workbook reading's passage and the cell ("Ledger!B12").
	RelationSource source;
};

enum class RowKind { Header, Body, Total, Subtotal, Note, Blank };

struct TableRow {
	std::size_t number = 0;
	bool hidden = false;
	RowKind kind = RowKind::Body;
	// The first text cell of the row, as the sheet writes it (local).
	std::string label;
	std::vector<TableCell> cells;
};

struct ColumnFormula {
	std::string pattern;
	std::vector<std::string> inputs;
	std::string op;
	bool consistent = false;
};

struct TableColumn {
	std::string letter;
	// The header's words (local; sent only as a data span, DESIGN4 §6.4).
	std::string header;
	// Closed role class (DESIGN4 §5.2) or "".
	std::string role_class;
	// As the legend writes it (DESIGN4 §2.4): "MONEY(USD/month)", "DATE(month)".
	std::string type;
	std::string currency;
	bool hidden = false;
	std::optional<ColumnFormula> formula;
	// A body cell of it is blank: a comparison over it may be undetermined, so
	// what is summed over a set filtered on it may be a bound (DESIGN4 §6.5).
	bool sparse = false;
};

// A column the model worked out row by row (`derive_column`, DESIGN4 §2.5):
// virtual, addressed as @label, its cells never in the file.
struct DerivedColumn {
	std::string label;
	// The op each row ran ("subtract") and the operands as written.
	std::string op;
	std::vector<std::string> operands;
	// As the legend writes its cells' type.
	std::string type;
	// Row number -> value; a row whose value could not be worked out is absent
	// from `values` and listed in `undetermined`.
	std::map<std::size_t, semantic::Value> values;
	std::set<std::size_t> undetermined;
	// Row number -> the fact keys its value rests on (cells and operands).
	std::map<std::size_t, std::set<std::string>> roots;
	// Roots every row shares (the scalar operands, and the predicate operands
	// of the set it was worked out over): ranked among the rows when a filter
	// over it is listed (DESIGN4 §6.5).
	std::set<std::string> operand_roots;
	// The cells the set it was worked out over was filtered by: whether a row
	// has a value at all depends on them (one membership bit each).
	std::set<std::string> base_cell_roots;
	// The rows those predicates were read over (value-independent), and their
	// column keys.
	std::vector<std::size_t> base_rows;
	std::vector<std::string> base_group_keys;
	// Row number -> the cells its value was worked out from (DESIGN4 §6.7).
	std::map<std::size_t, std::vector<RelationSource>> sources;
	// The divisor, when the op divides by it, as a canonical key (DESIGN4 §8.3).
	std::string divisor;
};

// A set of rows of one table (DESIGN4 §6.5): the rows certainly in it and the
// rows whose membership could not be decided (a partial date, a blank cell).
// Opaque to the model ([[Rows1]]); its roots are every cell a predicate read
// over the base set, and every predicate operand.
struct RowSet {
	std::string table;
	std::vector<std::size_t> certain;
	std::vector<std::size_t> undetermined;
	std::vector<std::string> roots;
	// Built only from hidden/visible atoms and set operations over such sets:
	// the tables block states its size, so a count of it is structural.
	bool structural = false;
	// The roots apart: the cells a predicate read (one membership bit each
	// when listed) and the predicate operands (their rank among the cells).
	std::vector<std::string> cell_roots;
	std::vector<std::string> operand_roots;
	// The public grouping keys of the columns a predicate read (DESIGN4 §8.3).
	std::vector<std::string> group_keys;
	// The rows the predicates were read over, decided without reading a value:
	// the table's body rows, or the members of a structural set it was
	// filtered from - never the members of a set a value decided (DESIGN4 D4).
	// Its size is list_rows' N.
	std::vector<std::size_t> base_rows;
	std::size_t base_size = 0;
	// Whether an aggregate over it may be a bound, decided from the predicates'
	// types alone (a date atom on a partial date, a blank atom, a sparse
	// column): what makes its status an observation (DESIGN4 §8.3).
	bool may_be_bounded = false;
	// The canonical request that built it (symbol plane), for keys.
	std::string canonical;
};

// Rows of one table partitioned by a key column (DESIGN4 §2.5 `group`), in
// first-row order.
struct GroupSet {
	std::string table;
	std::string key_column;
	std::vector<std::pair<std::string, RowSet>> groups;
	// Per group, in the same order: how the key is written back to the model -
	// a symbol ([[Org3]]), a cell's words (a data span), or a period's ordinal
	// from the anchor ("year 2") - and, for a calendar period, its dates.
	std::vector<std::string> key_words;
	std::vector<std::optional<semantic::Value>> key_periods;
	// Date keys: the unit and the anchor's fact key, if any.
	std::string unit;
	std::string anchor;
	// Every member row's key cell (charged when the partition is listed).
	std::vector<std::string> key_roots;
	std::string key_group_key;
	bool date_key = false;
	std::string canonical;
};

struct ProtectedTable {
	// "Table1": sent as [[Table1]].
	std::string handle;
	std::string document_id;
	// How the prompt names the document ("DOC_3"), when the caller knows it.
	std::string document_handle;
	std::string title;
	std::string sheet;
	bool sheet_hidden = false;
	std::size_t header_row = 0;
	std::vector<TableColumn> columns;
	std::vector<TableRow> rows;
	// Row number and its formula summary ("SUM of rows 4-18").
	std::vector<std::pair<std::size_t, std::string>> total_rows;
	// Whether the tables block describes it (DESIGN4 §6.2, D14): a sheet the
	// retrieved evidence touched, or one `describe_tables` described.
	bool described = false;
	// The columns the block describes (letters); others are `unknown_column`.
	std::vector<std::string> described_columns;
	// The key column (letters) and its period, when every body row is one
	// distinct month, quarter or year: the money columns are then owed per
	// period (MONEY(USD/month), DESIGN4 §6.3).
	std::string key_column;
	std::string period;
	// More rows than the model may hold (kRowsPerTable): no op reads it.
	bool too_large = false;
	// What the model worked out over it: sets and groups by symbol ("Rows1")
	// and by label ("@rows_2026"), derived columns by label.
	std::map<std::string, RowSet> row_sets;
	std::map<std::string, GroupSet> group_sets;
	std::vector<DerivedColumn> derived_columns;
};

// A band schedule (DESIGN4 §2.4, §6.6): ordered thresholds, each band mapped
// to a value - built from a table or from bands enumerated in prose.
struct ScheduleBand {
	// The band's lower and upper thresholds as symbols or grounded constants
	// (value keys), empty when open-ended; whether each end is inclusive.
	std::string lower;
	std::string upper;
	bool lower_inclusive = true;
	bool upper_inclusive = false;
	// The band's value: a symbol or a grounded constant's value key.
	std::string value;
	// The same, worked out locally, and the fact keys they are (empty for a
	// constant the material writes openly).
	std::optional<semantic::Value> lower_value;
	std::optional<semantic::Value> upper_value;
	std::optional<semantic::Value> band_value;
	std::string lower_fact;
	std::string upper_fact;
	std::string value_fact;
};

struct ProtectedSchedule {
	// "Schedule1": sent as [[Schedule1]].
	std::string handle;
	std::string document_id;
	// The words that name it (local; a data span when sent).
	std::string title;
	std::string location;
	// Type of the thresholds and of the band values (DESIGN4 §2.4 spelling).
	std::string key_type;
	std::string value_type;
	std::vector<ScheduleBand> bands;
	RelationSource source;
};

} // namespace singularity::protected_api::papinian::tables
