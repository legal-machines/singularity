#pragma once

#include "PapinianGuard/ProtectedTables/ProtectedTableTypes.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <optional>
#include <string>
#include <vector>

// Singularity's own evaluator of a workbook formula (DESIGN4 §6.6, `recalc`), over
// the local table model and in exact numbers: cell references and ranges of
// the same sheet, + - * /, unary minus, brackets, SUM, MIN, MAX, AVERAGE,
// COUNT and SUBTOTAL (1-11 counting hidden rows, 101-111 skipping them; a
// nested SUBTOTAL is ignored). Anything else is not evaluable. Seam
// declaration (lane S); lane T writes the body.
namespace singularity::protected_api::papinian::tables {

struct FormulaOutcome {
	// Value: worked out. NotEvaluable: the formula does not parse or uses a
	// function outside the closed set (depends on its text only).
	// EvaluationFailed: a value made it fail (#DIV/0!, text in a referenced cell).
	enum class Kind { Value, NotEvaluable, EvaluationFailed };
	Kind kind = Kind::NotEvaluable;
	std::optional<semantic::ExactRational> value;
	// The cells it read ("B4", ...), for provenance and the pre-charge.
	std::vector<std::string> inputs;
};

[[nodiscard]] FormulaOutcome recalc(const ProtectedTable &table, const TableCell &cell);

// The cells `cell`'s formula reads, and those their own formulas read, from
// the formulas' text alone - never from a value: what a recalc is charged to
// (DESIGN4 §8.3). Empty when the formula is not evaluable.
[[nodiscard]] std::vector<std::string> formula_inputs(const ProtectedTable &table, const TableCell &cell);

} // namespace singularity::protected_api::papinian::tables
