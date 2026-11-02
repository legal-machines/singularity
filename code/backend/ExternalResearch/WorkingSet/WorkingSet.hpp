#pragma once

// The phase-general Working-Set contract (see this migration's own
// architecture note: token count is not a semantic completion condition, and
// capacity constrains EXECUTION, never RELEVANCE). Domain code (Document/
// ConnectedDocumentAction.cpp today) owns the Semantic Dependency Resolver -
// what the current ProfessionalObjective actually needs, and what (if
// anything) would genuinely help beyond that - this header owns only the
// shared shape of that result and the tri-state physical-fit contract every
// frontier checks it against. There is deliberately NO generic "keep adding
// candidates until capacity says stop" algorithm here: a candidate belongs
// in a WorkingSetSpec only because the resolver found a real professional
// reason for it (SemanticDependency below always carries one), never because
// physical room happened to remain - see plan_working_set()'s own comment
// for why it is a plain concatenation, not a filling loop. It is entirely
// legitimate, and expected for this first pass, for `candidates` to be empty
// - do not invent optional context merely to exercise this field.
//
// This header intentionally does NOT hardcode what a SemanticUnit "is" for
// any frontier - never "item", never "region". Which canonical objects form
// one indivisible group is a current domain-validator/persistence fact
// (Document/ConnectedDocumentAction.cpp's own upsert/addRegion invariants
// today), not a generic architectural law - see this migration's own
// classification of that atomicity question as UNKNOWN/current-compatibility-
// boundary, not proven, for both Drafting's item and OutputPlanning's region.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::research {

// Identifies one canonical object addressable in durable state (a field, an
// item, a region, an evidence record, a methodology slide, ...). `kind` is
// domain-defined text this header never interprets.
struct SemanticUnitRef {
	std::string kind;
	std::string handle;
};

// One unit of canonical state as it may appear in a model-visible
// projection. `detail` is domain-defined JSON.
struct SemanticUnit {
	SemanticUnitRef ref;
	nlohmann::json detail;
};

// A real professional reason one SemanticUnit belongs alongside another -
// never physical proximity alone (see this migration's own note: document/
// canonical order is at most a weak secondary tiebreaker, never itself a
// reason). `relation` is a short, domain-defined, stable label (e.g.
// "evidence_required_by_objective", "explicit_cross_reference",
// "unresolved_review_issue") - a real, nameable professional relationship
// the resolver actually found in canonical state, not a guess.
struct SemanticDependency {
	SemanticUnitRef from;
	SemanticUnitRef to;
	std::string relation;
	std::string reason;
};

// The current professional action Singularity is compiling a turn for.
// `seed` is the canonical object(s) the action is directly about (e.g. one
// pending Drafting item, one selectable OutputPlanning region) - resolved by
// domain code from durable state (plan.draft), never chosen for capacity
// reasons.
struct ProfessionalObjective {
	std::string frontier; // research::OperationWorkFrontier's own to_string()
	std::vector<SemanticUnitRef> seed;
};

// required: the dependency-closed minimum sufficient set for `objective` -
// the seed plus every SemanticDependency target the resolver found. Never
// trimmed for capacity reasons; if its own prepared request does not
// physically fit (see CapacityMeasurement below), that is an escalation
// (compact the input projection, decompose the objective if the current
// domain validator allows it, or an honest failure) - never a silent drop.
// candidates: additional units the resolver found a real SemanticDependency
// reason to offer, already decided by domain logic - legitimately empty.
// This type never grows or shrinks itself; it is the resolver's finished
// answer, produced entirely before any CapacityMeasurement exists.
struct WorkingSetSpec {
	ProfessionalObjective objective;
	std::vector<SemanticUnit> required;
	std::vector<SemanticUnit> candidates;
	std::vector<SemanticDependency> dependencies;
};

// The actual model-visible content for one turn - required, then every
// candidate the resolver already decided to include. Plain concatenation:
// see plan_working_set()'s own comment for why there is no capacity-driven
// filtering here.
struct WorkingSetProjection {
	std::vector<SemanticUnit> admitted;
};

inline WorkingSetProjection plan_working_set(const WorkingSetSpec &spec) {
	WorkingSetProjection projection;
	projection.admitted = spec.required;
	projection.admitted.insert(projection.admitted.end(), spec.candidates.begin(), spec.candidates.end());
	return projection;
}

// A real, physically-measured fact about a candidate prepared request -
// never a chars-per-token estimate standing in for one. Deliberately
// tri-state (see this migration's own note): the absence of an authoritative
// measurement capability is a real fact (Unknown), never silently treated as
// InputFits. InputFits/InputDoesNotFit answer only "does the prepared INPUT
// physically enter this model's context" - neither one is a claim that the
// resulting generation will semantically complete before physical context
// is exhausted; that is a separate, later fact (see InferenceResult::
// physical_context_exhausted).
enum class CapacityFit { InputFits, InputDoesNotFit, Unknown };

// `source` names which real capability produced this measurement (or why
// none did) - never blank. Concrete values used today: "local_exact" (local inference's
// own non-generating input-token measurement),
// "no_preflight_measurement" (no authoritative capability for this
// provider/mode - CapacityFit is always Unknown in that case).
struct CapacityMeasurement {
	CapacityFit fit = CapacityFit::Unknown;
	std::optional<std::uint64_t> measured_input_tokens;
	std::optional<std::uint64_t> physical_capacity_tokens;
	std::optional<std::uint64_t> physical_remaining_tokens;
	std::string source;
};

// Given the real provider-facing text a candidate prepared request would
// send, reports its real CapacityMeasurement. Implementations: local (local inference's
// measure(), sharing the exact
// canonical request-preparation path real generation uses - zero decoded
// tokens), or a provider-specific authoritative input-count capability when
// one exists. An empty callback (no capability negotiated for this
// provider/mode) means every call site must treat every measurement as
// Unknown - never fabricate InputFits.
using CapacityMeasurer = std::function<CapacityMeasurement(const std::string &candidateProviderFacingInput)>;

} // namespace singularity::research
