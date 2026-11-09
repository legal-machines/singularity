#pragma once

#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The executor core of the protected computation protocol (DESIGN4 §3, §8):
// typing, admission, normalisation, the observation ledger and the dispatch
// over the registered families. Declared by the seam (lane S) as the contract
// the other lanes call; lane X writes the bodies (TypeSystem.cpp,
// Admission.cpp, Normaliser.cpp, ObservationLedger.cpp, ProtectedOperations.cpp)
// and the header-inline wiring below.
//
// Link rule: the .cpp files of this protocol are compiled into every build
// that reads operations, including builds without PapinianGuardService.cpp.
// Their non-inline code reaches the guard (issue_derived_fact, issue_cell_fact,
// row_of_marker, apply_frozen_mapping) only through OpContext's callbacks,
// which the header-inline compute_context() below wires - exactly as
// operation_context() does for ProtectedOperations today.
namespace singularity::protected_api::papinian::operations {

// ---------------------------------------------------------------------------
// Typing (TypeSystem.cpp).

// The result type of `op` over `arguments` (DESIGN4 §2.5), through the
// registered family's typer; nothing, with `error`, when the operands do not fit.
[[nodiscard]] std::optional<compute::ResultType> type_of(std::string_view op,
	const std::vector<compute::ArgType> &arguments, const compute::OpOptions &options, compute::TypeError &error);

// The static type of a registry row: from its value, its role repair
// (DESIGN4 §5.3) and its entity type - never from the value's magnitude.
[[nodiscard]] compute::ArgType arg_type_of(const RequestMappingEntry &row);

// The static type of a value: its kind, currency, period, unit and precision.
[[nodiscard]] compute::ArgType type_of_value(const semantic::Value &value);

// ---------------------------------------------------------------------------
// Admission and normalisation (Admission.cpp, Normaliser.cpp; DESIGN4 §8.1).

// An expression's canonical form over its hidden root facts.
struct CanonicalForm {
	std::string key;
	// No hidden root: its value is computable from the payload (R10).
	bool known = false;
	// Affine in its one hidden dimensionless root (R6 case c).
	bool affine = false;
	std::set<std::string> hidden_roots;
	std::size_t monomials = 0;
};

[[nodiscard]] std::optional<CanonicalForm> normalise(const compute::OpNode &node,
	const std::vector<compute::Operand> &operands, const compute::OpContext &context, compute::Code &error);

// An operand as a canonical key names it (a fact key, a constant's value key,
// a table handle) - never a hidden value.
[[nodiscard]] std::string operand_key(const compute::Operand &operand);

// Whether a root fact is one the model may know without the lawyer's material
// (origin External: a page the model itself asked for, DESIGN4 §8.1 R11).
[[nodiscard]] bool external_root(const std::string &root, const compute::OpContext &context);

// The verdict on one operation before evaluation: admitted (Code::None) or a
// class-S code with its argument; and, for an observation, its charge.
struct Admission {
	compute::Code code = compute::Code::None;
	std::size_t argument = 0;
	std::optional<compute::ObservationCharge> charge;
	std::vector<compute::ConstantUse> constants;
};

[[nodiscard]] Admission admit(const compute::OpNode &node, const std::vector<compute::Operand> &operands,
	const compute::OpContext &context);

// The figures the lawyer's material writes openly in a payload - source,
// request and lawyer-answer segments only (DESIGN4 §8.2) - by value key.
[[nodiscard]] std::vector<compute::ConstantUse> ground_constants(const std::string &payload,
	const std::vector<compute::PayloadSegment> &segments);

// The figure the lawyer's material writes for `constant`, as the material
// types it (a rate with the period its own clause gives it), or nothing.
[[nodiscard]] std::optional<semantic::Value> material_figure(const semantic::Value &constant,
	const compute::OpContext &context);

// A project search, inspect or web query the model wrote (DESIGN4 §8.7):
// Code::None when it may run, QueryLiteral otherwise. Depends only on the
// sent text. Where a figure or a word the material does not write is why,
// `unwritten` takes it, so the model can be told which.
[[nodiscard]] compute::Code admit_query(const std::string &query, const std::string &payload,
	const std::vector<compute::PayloadSegment> &segments, bool web, std::string *unwritten = nullptr);

// Whether `text` writes something restoration would fill in with a protected
// value: a bracket, letters, digits - "[[Org1]]", "[Org1]", "[ Person 2 ]" -
// in any of the writings PapinianGuard::restore reads, whatever symbols the
// request issued. Nothing so written may reach a search engine.
[[nodiscard]] bool writes_a_symbol(const std::string &text);

// ---------------------------------------------------------------------------
// Execution (ProtectedOperations.cpp).

// Each op of one round, in order: typed, admitted, charged, evaluated, its
// result issued as a derived fact - or answered with its closed code. Never
// throws for what a model asks.
[[nodiscard]] std::vector<compute::OpAnswer> execute_ops(const std::vector<compute::OpNode> &ops,
	compute::OpContext &context);

// The registry's entity type for a value Singularity reads or works out: what
// its symbol's prefix and the category rules go by.
inline EntityType entity_type_of_value(const semantic::Value &value) {
	switch (value.kind) {
		case semantic::Kind::Money: return EntityType::MoneyAmount;
		case semantic::Kind::Percentage: return EntityType::Percentage;
		case semantic::Kind::Date:
		case semantic::Kind::DayOfYear: return EntityType::Date;
		default: return EntityType::Number;
	}
}

// Singularity's own wiring of an OpContext over the guard's table (header-inline,
// so builds without the guard link).
inline compute::OpContext compute_context(const PapinianGuard &guard, OperationProtectedSymbolTable &table,
		const Policy &policy) {
	compute::OpContext wired;
	wired.table = &table;
	wired.ledger = &table.observations;
	wired.row_of = [&table](std::string_view marker) { return row_of_marker(table, marker); };
	wired.issue = [&guard, &table](compute::DerivedFact &fact) { return guard.issue_derived_fact(table, fact); };
	// A cell no prompt carried: a Source fact of that cell, stated where the
	// workbook reading states it.
	wired.issue_cell = [&guard, &table](const tables::ProtectedTable &, const tables::TableCell &cell) -> std::string {
		if (!cell.value) return std::string();
		FactProvenance provenance;
		provenance.origin = FactProvenance::Origin::Source;
		provenance.passage_sha256 = cell.source.passage_sha256;
		provenance.offset = cell.source.offset;
		provenance.length = cell.source.length;
		provenance.cell = cell.source.cell.empty() ? cell.ref : cell.source.cell;
		return guard.issue_cell_fact(table, *cell.value, entity_type_of_value(*cell.value), provenance);
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
	return wired;
}

// The research loop's compute executor - parse, execute_ops() above, render -
// is lane K's glue in ProtectedComputation/ComputeBinding.hpp, not here: X
// owns execution, K binds it (DESIGN4 §13.2 wave 2).

} // namespace singularity::protected_api::papinian::operations
