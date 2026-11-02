#pragma once

#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"

#include <cstddef>
#include <string>
#include <vector>

// The final answer contract (DESIGN4 §9, owner §10): before any protected
// symbol of a model's final text - or of any string field of an artifact or a
// tool call - is restored for the lawyer, every quantitative symbol is checked:
// it exists and belongs to this operation, it was sourced or deterministically
// derived, its provenance is complete, it fits the words around it, a bound is
// written as a bound, and an equation the text claims is one Singularity recorded.
// A raw figure the lawyer's material does not write is a violation too. Pure:
// structural checks only - no value is ever evaluated here, so what a check
// says never depends on whether a claim is true.
//
// Declared by the seam (lane S); lane V writes the checks. Until then check()
// finds nothing and withhold() changes nothing: today's figure check keeps
// running where it runs.
namespace singularity::protected_api::papinian {
struct OperationProtectedSymbolTable;
} // namespace singularity::protected_api::papinian

namespace singularity::protected_api::papinian::answer_contract {

// One violation: its closed code (unknown_symbol, not_sourced,
// derivation_failed, provenance_incomplete, type_in_context, bound_as_exact,
// bound_direction, claim_not_computed, sign_unobserved, raw_figure,
// statement_not_allowed; soft: compute_not_attempted, unchecked_cross_source),
// the symbol it concerns ("" for a raw figure), its byte offset and the JSON
// pointer of the field. Never a value.
struct ContractViolation {
	std::string code;
	std::string symbol;
	std::size_t offset = 0;
	std::string json_pointer;
	// A nudge, given once, never a failure.
	bool soft = false;
	// A figure the model wrote outside any symbol (raw_figure): as it wrote
	// it, so the repair can name it. The model's own words; never a value it
	// was not given.
	std::string written;
};

struct ContractResult {
	std::vector<ContractViolation> violations;
	std::size_t quantitative_symbols = 0;

	[[nodiscard]] bool clean() const {
		for (const ContractViolation &violation : violations)
			if (!violation.soft) return false;
		return true;
	}
};

[[nodiscard]] ContractResult check(const std::string &protected_text, const std::string &json_pointer,
	const OperationProtectedSymbolTable &table, const std::string &sent_prompt, semantic::Language language);

// Keeps what `sent_prompt` gave the model of the lawyer's material (its source
// segments and the lawyer's own answers) in `table`, once per passage: a
// figure any call of the attempt sent openly grounds the attempt's answers.
void remember_sent_material(OperationProtectedSymbolTable &table, const std::string &sent_prompt);

// Where an unverifiable figure would have stood (DESIGN4 §9.2): in the chat
// answer the sentence carrying it is left out; in a document the figure alone
// becomes a neutral bracketed gap in the document's language.
enum class Surface { Chat, Artifact };

struct Withheld {
	std::string text;
	std::size_t figures = 0;
};

[[nodiscard]] Withheld withhold(const std::string &protected_text, const std::vector<ContractViolation> &violations,
	Surface surface, semantic::Language language);

} // namespace singularity::protected_api::papinian::answer_contract
