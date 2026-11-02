#pragma once

#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Routing/Types/ModelVisibleTurn.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::research {

// Domain state is projected once per turn from the canonical backend draft.
// The projection is disposable: checkpoints persist the typed draft/evidence
// state needed to recreate it, never this prompt or a provider grammar.
// Output planning ran out of regions with none selected: every table region of
// every candidate document was offered and the model declined each one. The
// request asks for no document change. A logic_error for every caller that
// does not handle it; the AdvancedWorkflow dispatch does, and answers in the
// chat from the generation it already has.
struct OutputPlanningFoundNoRegion final : std::logic_error {
	using std::logic_error::logic_error;
};

enum class OperationWorkFrontier {
	PlanningEvidence, PlanningOutputs, Drafting, Correction, NeedsResearch, ProfessionalReview, Complete
};

inline const char *to_string(OperationWorkFrontier value) noexcept {
	switch (value) {
		case OperationWorkFrontier::PlanningEvidence: return "planning_evidence";
		case OperationWorkFrontier::PlanningOutputs: return "planning_outputs";
		case OperationWorkFrontier::Drafting: return "drafting";
		case OperationWorkFrontier::Correction: return "correction";
		case OperationWorkFrontier::NeedsResearch: return "needs_research";
		case OperationWorkFrontier::ProfessionalReview: return "professional_review";
		case OperationWorkFrontier::Complete: return "complete";
	}
	return "drafting";
}

struct OperationStateProjection {
	std::string operation_id;
	std::uint64_t revision = 0;
	OperationWorkFrontier frontier = OperationWorkFrontier::Drafting;
	routing::TurnDataPlane data_plane;
	std::vector<std::string> item_handles;
	std::vector<std::string> field_handles;
	std::map<std::string, std::vector<std::string>> project_evidence_handles_by_item;
	// A visible item (in item_handles) is not always yet a draftable one - see
	// ConnectedDocumentAction.cpp's is_item_collapsed()/document_grounding_
	// complete_for_drafting() for why those are two separate questions once
	// more than one document is simultaneously required evidence. This map's
	// own keys are therefore always a subset of item_handles, populated for
	// an item only once drafting into it may actually be accepted - see
	// compile_turn_contract()'s own use of this map (rather than item_handles)
	// to decide whether anything is draftable at all this turn.
	std::map<std::string, std::vector<std::string>> editable_field_handles_by_item;
	std::vector<std::string> methodology_handles;
	// The subset of methodology_handles NOT yet semantically revealed via document.
	// methodology.reveal (see DocumentActionDraft::revealed_methodology_
	// handles) - used solely to enum-constrain that tool's own methodology
	// Handle argument and to decide whether to expose it at all, the same
	// pattern as inspectable_table_handles/table_map_next_document_handles.
	// Empty (and the tool unexposed) whenever every real methodology slide is
	// already revealed, even if some revealed text has left the bounded current
	// projection, or the operation has no methodology deck at all.
	std::vector<std::string> methodology_reveal_candidate_handles;
	std::vector<std::string> supportable_external_handles;
	std::vector<std::string> research_candidate_handles;
	// Document/table handles the model may still request to have expanded
	// into real structural items via document.inspect (see
	// ConnectedDocumentAction.cpp's compact-context path) - never populated
	// when the operation's full structure already fit and was exposed
	// directly, so this stays empty (and the tool stays unexposed below) for
	// every pre-existing caller.
	std::vector<std::string> inspectable_table_handles;
	// Handles ("D1", "D2"...) of the real candidate documents, populated only
	// when an explicit required-evidence declaration is possible for this
	// operation (see ConnectedDocumentActionPlan::requires_evidence_agenda) -
	// used solely to enum-constrain document.evidence.define's own schema, so
	// the model can only ever name a document Singularity already resolved and
	// structurally read. Handles, never names: a schema is not transformed by
	// the Papinian boundary, so it must never carry Project data.
	std::vector<std::string> candidate_document_handles;
	// Handles of the documents whose own table map (see
	// ConnectedDocumentAction.cpp's table-map pagination) does not yet list
	// every one of its own tables that currently require inspection - used
	// solely to enum-constrain document.map.next's documentHandle argument and
	// to decide whether to expose it at all.
	std::vector<std::string> table_map_next_document_handles;
	// Real EvidenceObjective ids only (see ConnectedDocumentAction.hpp), used
	// solely to enum-constrain document.review.submit's own optional
	// evidenceObjectiveId so a NeedsMoreEvidence verdict can only ever name an
	// objective Singularity actually knows how to route back to - never one the
	// model invented. Empty whenever no objective has been declared yet.
	std::vector<std::string> evidence_objective_ids;
	std::optional<std::string> requested_research_source_handle;
	// Meaningful only for NeedsResearch. They are part of the domain's typed
	// checkpoint so restart can tell old research from the newly requested
	// discovery/retrieval cycle without replaying a transcript.
	std::size_t research_search_calls_at_request = 0;
	std::size_t research_open_calls_at_request = 0;
	// True once at least one document.outputs.define call already
	// accumulated a region (final=false), before the agenda is finalized -
	// see DocumentActionDraft::selected_output_region_handles' own comment
	// on cross-call accumulation. item_handles alone is not enough to decide
	// whether document.outputs.define should stay offered: a real operation
	// can legitimately reach a turn where every currently-visible region is
	// already selected and nothing new remains to inspect (item_handles and
	// inspectable_table_handles both empty), yet the agenda is still not
	// finalized because the model has not yet sent final=true - the tool
	// must stay reachable so it can submit that closing, possibly-empty
	// call, or the operation dead-ends with no exposed capability at all
	// (a real live-API run against openai/gpt-4o-mini via OpenRouter hit
	// this exact case).
	bool output_agenda_can_finalize = false;
};

enum class TurnFrontier {
	ResearchDiscovery, ResearchRetrieval, EvidencePlanning, OutputPlanning, Drafting, Correction,
	ProfessionalReview, Complete
};

inline const char *to_string(TurnFrontier value) noexcept {
	switch (value) {
		case TurnFrontier::ResearchDiscovery: return "research_discovery";
		case TurnFrontier::ResearchRetrieval: return "research_retrieval";
		case TurnFrontier::EvidencePlanning: return "evidence_planning";
		case TurnFrontier::OutputPlanning: return "output_planning";
		case TurnFrontier::Drafting: return "drafting";
		case TurnFrontier::Correction: return "correction";
		case TurnFrontier::ProfessionalReview: return "professional_review";
		case TurnFrontier::Complete: return "complete";
	}
	return "complete";
}

struct TurnHandleWhitelist {
	std::vector<std::string> item_handles;
	std::vector<std::string> field_handles;
	std::map<std::string, std::vector<std::string>> project_evidence_handles_by_item;
	std::map<std::string, std::vector<std::string>> editable_field_handles_by_item;
	std::vector<std::string> methodology_handles;
	std::vector<std::string> source_handles;
	std::vector<std::string> supportable_external_handles;
	std::vector<std::string> research_candidate_handles;
	std::vector<std::string> inspectable_table_handles;

	[[nodiscard]] bool contains_item(const std::string &handle) const {
		return std::find(item_handles.begin(), item_handles.end(), handle) != item_handles.end();
	}
	[[nodiscard]] bool contains_source(const std::string &handle) const {
		return std::find(source_handles.begin(), source_handles.end(), handle) != source_handles.end();
	}
};

// One immutable contract drives every model-facing/control boundary for one
// operation revision. The duplicated phase stamps are deliberate audit
// guards: compile_turn_contract() is the sole constructor and validation can
// assert that context, actions, tools, grammar and validator did not drift.
struct TurnContract {
	std::string operation_id;
	std::uint64_t operation_revision = 0;
	std::uint64_t turn_contract_revision = 0;
	TurnFrontier phase = TurnFrontier::Complete;
	TurnFrontier context_phase = TurnFrontier::Complete;
	TurnFrontier allowed_actions_phase = TurnFrontier::Complete;
	TurnFrontier tool_surface_phase = TurnFrontier::Complete;
	TurnFrontier fallback_grammar_phase = TurnFrontier::Complete;
	TurnFrontier validator_phase = TurnFrontier::Complete;
	routing::TurnControlPlane control_plane;
	routing::TurnDataPlane data_plane;
	std::vector<std::string> allowed_actions;
	std::vector<providers::ModelToolDefinition> exposed_tools;
	providers::ToolChoicePolicy tool_choice = providers::ToolChoicePolicy::Required;
	std::optional<std::string> forced_tool_wire_name;
	std::optional<providers::GenerationOutputContract> fallback_output_contract;
	routing::ResponseRestorationContract response_restoration;
	TurnHandleWhitelist handles;

	[[nodiscard]] bool phases_are_atomic() const noexcept {
		return phase == context_phase && phase == allowed_actions_phase
			&& phase == tool_surface_phase && phase == fallback_grammar_phase
			&& phase == validator_phase;
	}
	[[nodiscard]] bool allows(const std::string &canonical_action) const {
		return std::find(allowed_actions.begin(), allowed_actions.end(), canonical_action)
			!= allowed_actions.end();
	}
	[[nodiscard]] nlohmann::json safe_metadata() const {
		return {{"operationId", operation_id}, {"operationRevision", operation_revision},
			{"turnContractRevision", turn_contract_revision}, {"phase", to_string(phase)},
			{"allowedActions", allowed_actions},
			{"itemHandles", handles.item_handles}, {"sourceHandles", handles.source_handles}};
	}
};

struct TurnContractInput {
	OperationStateProjection operation;
	ResearchMode research_mode = ResearchMode::Auto;
	OperationExecutionProgress progress;
	providers::ModelToolDefinition search_tool;
	providers::ModelToolDefinition open_tool;
	providers::ModelToolDefinition draft_upsert_tool;
	providers::ModelToolDefinition draft_request_research_tool;
	// Default-constructed (empty wire_name) for every caller that never sets
	// OperationStateProjection::inspectable_table_handles - see
	// compile_turn_contract() below, which only ever exposes this tool when
	// that list is non-empty, so an operation with no compact/collapsed
	// tables never surfaces it.
	providers::ModelToolDefinition draft_inspect_tool;
	providers::ModelToolDefinition output_agenda_tool;
	// Default-constructed (empty wire_name) for every caller that never sets
	// OperationStateProjection::candidate_document_handles - see compile_turn_
	// contract() below, which only ever reaches OperationWorkFrontier::
	// PlanningEvidence when ConnectedDocumentAction populated that list, so an
	// operation with only one (or no) named candidate document never surfaces
	// this tool at all.
	providers::ModelToolDefinition evidence_agenda_tool;
	// Default-constructed (empty wire_name) for every caller that never sets
	// OperationStateProjection::table_map_next_document_handles - see compile_
	// turn_contract() below, which only ever exposes this tool when that list
	// is non-empty, so an operation where every actionable document's table
	// map already lists every table never surfaces it at all.
	providers::ModelToolDefinition table_map_next_tool;
	// Default-constructed (empty wire_name) for every caller that never sets
	// OperationWorkFrontier::ProfessionalReview - see compile_turn_contract()
	// below, which only ever reaches that phase once a caller wires this tool.
	providers::ModelToolDefinition professional_review_tool;
	// Default-constructed (empty wire_name) for every caller that never sets
	// OperationStateProjection::methodology_reveal_candidate_handles - see
	// compile_turn_contract() below, which only ever exposes this tool when
	// that list is non-empty, so an operation with no methodology deck (or
	// one already fully revealed) never surfaces it at all.
	providers::ModelToolDefinition methodology_reveal_tool;
	std::string draft_upsert_action = "document.draft.upsert";
	std::string draft_request_research_action = "document.draft.request_research";
	std::string draft_inspect_action = "document.inspect";
	std::string output_agenda_action = "document.outputs.define";
	std::string evidence_agenda_action = "document.evidence.define";
	std::string table_map_next_action = "document.map.next";
	std::string professional_review_action = "document.review.submit";
	std::string methodology_reveal_action = "document.methodology.reveal";
};

inline providers::ModelToolDefinition constrain_item_handles(
		providers::ModelToolDefinition tool, const std::vector<std::string> &handles) {
	if (tool.parameters_schema.contains("properties")
			&& tool.parameters_schema["properties"].contains("itemHandle"))
		tool.parameters_schema["properties"]["itemHandle"]["enum"] = handles;
	return tool;
}

inline providers::ModelToolDefinition constrain_source_handles(
		providers::ModelToolDefinition tool, const std::vector<std::string> &handles) {
	if (tool.parameters_schema.contains("properties")
			&& tool.parameters_schema["properties"].contains("sourceId"))
		tool.parameters_schema["properties"]["sourceId"]["enum"] = handles;
	return tool;
}

inline void constrain_array_string_items(nlohmann::json &arraySchema,
		const std::vector<std::string> &handles) {
	if (handles.empty()) {
		arraySchema["maxItems"] = 0;
		return;
	}
	arraySchema["items"]["enum"] = handles;
	// A real citation array can reference the same real handle more than
	// once (e.g. two rationales both resting on the same project cell), so
	// this bounds the array's real maximum SHAPE (never fewer distinct
	// handles than actually exist) for phase_aware_reserved_output_tokens()'s
	// own Drafting/Correction derivation below - not a uniqueItems constraint,
	// which would wrongly forbid a legitimate repeat.
	arraySchema["maxItems"] = handles.size();
}

inline providers::ModelToolDefinition constrain_draft_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	nlohmann::json alternatives = nlohmann::json::array();
	std::vector<std::string> eligibleItems;
	std::set<std::string> unionProjectEvidence;
	std::set<std::string> unionEditableFields;
	std::vector<std::string> revealedMethodologyHandles;
	for (const std::string &handle : operation.methodology_handles) {
		if (std::find(operation.methodology_reveal_candidate_handles.begin(),
				operation.methodology_reveal_candidate_handles.end(), handle)
				== operation.methodology_reveal_candidate_handles.end())
			revealedMethodologyHandles.push_back(handle);
	}
	for (const std::string &itemHandle : operation.item_handles) {
		const auto project = operation.project_evidence_handles_by_item.find(itemHandle);
		const auto editable = operation.editable_field_handles_by_item.find(itemHandle);
		if (project == operation.project_evidence_handles_by_item.end()
				|| editable == operation.editable_field_handles_by_item.end()
				|| editable->second.empty())
			continue;
		eligibleItems.push_back(itemHandle);
		unionProjectEvidence.insert(project->second.begin(), project->second.end());
		unionEditableFields.insert(editable->second.begin(), editable->second.end());
		auto branch = tool.parameters_schema;
		auto &properties = branch["properties"];
		properties["itemHandle"]["enum"] = nlohmann::json::array({itemHandle});
		constrain_array_string_items(properties["projectEvidence"], project->second);
		constrain_array_string_items(properties["externalEvidence"],
			operation.supportable_external_handles);
		// Only an ALREADY-REVEALED slide (see document.methodology.reveal /
		// DocumentActionDraft::revealed_methodology_handles) may be cited: a
		// collapsed slide's own content was never actually shown to the
		// model, so citing its handle here would let it assert a rule it
		// never read. Real methodology decks can run well past what a single
		// enum comfortably lists, so this is also real steering weight kept
		// bounded by the same reveal discipline that already keeps state
		// bounded, not the operation's total slide count.
			if (revealedMethodologyHandles.empty()) {
			properties["methodologyEvidence"]["maxItems"] = 0;
		} else {
			properties["methodologyEvidence"]["items"]["properties"]["handle"]["enum"] =
				revealedMethodologyHandles;
			properties["methodologyEvidence"]["maxItems"] = revealedMethodologyHandles.size();
		}
		properties["updates"]["items"]["properties"]["fieldHandle"]["enum"] = editable->second;
		// Real bound for phase_aware_reserved_output_tokens()'s own Drafting/
		// Correction derivation: one item can never legitimately submit more
		// WRITE updates than it actually has editable fields (upsert_
		// connected_document_draft_item() already independently requires
		// submittedWriteFields == expectedWriteFields, a real subset).
		properties["updates"]["maxItems"] = editable->second.size();
		alternatives.push_back(std::move(branch));
	}
	if (alternatives.empty())
		throw std::logic_error("The current document draft has no item with an editable capability.");
	if (alternatives.size() == 1) {
		tool.parameters_schema = {{"oneOf", std::move(alternatives)}};
		return tool;
	}
	// CORE PRODUCT LAW correction fallout: once a request may legitimately
	// have many real candidate items (every structural row is now a
	// candidate - see prepare_connected_document_action()'s own comment),
	// this oneOf routinely has more than one alternative. A root-level oneOf
	// with more than one alternative cannot be faithfully represented as the
	// single flat object schema native tool calling requires (see
	// ProviderRegistry.cpp's lower_tool_schema_for_native_calling(), which
	// correctly refuses to guess at one) - a real design-partner acceptance
	// run's own OpenRouter/gpt-5.1 call proved this reachable the moment
	// per-item branches stopped collapsing to exactly one. Flatten to one
	// branch whose itemHandle/projectEvidence/fieldHandle enums span the
	// union of every eligible item instead of one item each -
	// upsert_connected_document_draft_item() already independently
	// re-validates that a submitted fieldHandle actually belongs to the
	// submitted itemHandle at accept time, so this loses only the syntactic
	// (schema-level) cross-item exclusivity guarantee, never the semantic one.
	auto &properties = tool.parameters_schema["properties"];
	properties["itemHandle"]["enum"] = eligibleItems;
	constrain_array_string_items(properties["projectEvidence"],
		std::vector<std::string>(unionProjectEvidence.begin(), unionProjectEvidence.end()));
	constrain_array_string_items(properties["externalEvidence"], operation.supportable_external_handles);
	if (revealedMethodologyHandles.empty()) {
		properties["methodologyEvidence"]["maxItems"] = 0;
	} else {
		properties["methodologyEvidence"]["items"]["properties"]["handle"]["enum"] =
			revealedMethodologyHandles;
		properties["methodologyEvidence"]["maxItems"] = revealedMethodologyHandles.size();
	}
	properties["updates"]["items"]["properties"]["fieldHandle"]["enum"] =
		std::vector<std::string>(unionEditableFields.begin(), unionEditableFields.end());
	// A single call still targets exactly one itemHandle, so its own real
	// max is one item's editable field count, never the flattened union
	// across every eligible item - but per-item counts are not carried this
	// far once flattened, so the union remains a safe (if slightly loose)
	// upper bound for phase_aware_reserved_output_tokens()'s own derivation.
	properties["updates"]["maxItems"] = unionEditableFields.size();
	return tool;
}

inline providers::ModelToolDefinition constrain_research_request_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	if (operation.item_handles.empty())
		throw std::logic_error("The current document draft has no item that can request research.");
	// sourceHandle's own enum is already identical across every item (see
	// operation.research_candidate_handles, an operation-wide list, never
	// per-item) - only itemHandle actually varies, so this needs no oneOf at
	// all. See constrain_draft_capabilities()'s own comment: a root-level
	// oneOf with more than one alternative cannot be faithfully represented
	// as the single flat object schema native tool calling requires.
	tool.parameters_schema["properties"]["itemHandle"]["enum"] = operation.item_handles;
	tool.parameters_schema["properties"]["sourceHandle"]["enum"] = operation.research_candidate_handles;
	return tool;
}

// document.inspect: reveals one collapsed table's real structural items - see
// ConnectedDocumentAction.cpp's compact-context path, used only when the full
// Project structure did not fit the resolved model's real capacity. Only
// called when operation.inspectable_table_handles is non-empty (see
// compile_turn_contract() below), matching constrain_draft_capabilities()/
// constrain_research_request_capabilities()'s own throw-on-empty convention
// would be wrong here since an already-fully-expanded operation should
// simply never expose this tool at all, not fail to compile one it does.
inline providers::ModelToolDefinition constrain_inspect_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	if (tool.parameters_schema.contains("properties")
			&& tool.parameters_schema["properties"].contains("tableHandle"))
		tool.parameters_schema["properties"]["tableHandle"]["enum"] = operation.inspectable_table_handles;
	// PlanningOutputs is pure structural region selection, never the drafting-
	// time evidence citation document.draft.upsert's own methodologyEvidence/
	// renderEvidence arguments reference by evidenceObjective id (see that
	// tool's own definition and constrain_draft_capabilities()) - so paying
	// for the full evidenceObjective/evidenceObjectiveClosure tracking sub-
	// schema on every OutputPlanning turn is currently-unusable weight, the
	// same category this session already deferred for methodology/value/
	// externalEvidence. evidenceSufficient/workingEvidenceNote/
	// workingEvidenceTables stay available: required-evidence grounding (a
	// real document declared via document.evidence.define) can still need to
	// be satisfied during OutputPlanning before drafting into a different
	// document may begin - only the professional-question-tracking pair is
	// deferred. The handler already treats an absent key the same as an
	// explicit null (this tool definition's own comment on OpenAI-strict-
	// mode optionality), so a caller that never sends either key needs no
	// handler change.
	if (operation.frontier == OperationWorkFrontier::PlanningOutputs
			&& tool.parameters_schema.contains("properties") && tool.parameters_schema.contains("required")) {
		auto &properties = tool.parameters_schema["properties"];
		properties.erase("evidenceObjective");
		properties.erase("evidenceObjectiveClosure");
		nlohmann::json filteredRequired = nlohmann::json::array();
		for (const auto &entry : tool.parameters_schema["required"]) {
			const std::string name = entry.get<std::string>();
			if (name != "evidenceObjective" && name != "evidenceObjectiveClosure") filteredRequired.push_back(name);
		}
		tool.parameters_schema["required"] = std::move(filteredRequired);
	}
	return tool;
}

// document.methodology.reveal: enum-constrains methodologyHandles' own array
// items to exactly the slides still collapsed (operation.methodology_reveal_
// candidate_handles), and bounds how many can be named in one call to the
// real remaining candidate count - see ConnectedDocumentAction.cpp's
// reveal_connected_document_methodology()'s own comment on why reveals are
// cumulative, never a disposable window, and on why a real governed
// acceptance run proved revealing an entire deck one slide per call cost
// many avoidable sequential round trips - the model may now reveal every
// slide it expects to need in ONE call.
inline providers::ModelToolDefinition constrain_methodology_reveal_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	if (tool.parameters_schema.contains("properties")
			&& tool.parameters_schema["properties"].contains("methodologyHandles")) {
		auto &property = tool.parameters_schema["properties"]["methodologyHandles"];
		if (property.contains("items")) property["items"]["enum"] = operation.methodology_reveal_candidate_handles;
		property["maxItems"] = operation.methodology_reveal_candidate_handles.size();
	}
	return tool;
}

// document.map.next: advances one named document's own table-map page - see
// ConnectedDocumentAction.cpp's table-map pagination. Only called when
// operation.table_map_next_document_handles is non-empty (see compile_turn_
// contract() below); enum-constrains documentHandle to exactly the documents
// that currently have a real next page, so the model can never request one
// that does not exist or is already fully shown.
inline providers::ModelToolDefinition constrain_table_map_next_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	if (tool.parameters_schema.contains("properties")
			&& tool.parameters_schema["properties"].contains("documentHandle"))
		tool.parameters_schema["properties"]["documentHandle"]["enum"] = operation.table_map_next_document_handles;
	return tool;
}

// The canonical OutputPlanning bounded-response shape, shared verbatim by
// the decision-window sizing below and by ResearchToolLoop.hpp's own
// phase_aware_reserved_output_tokens() (which reserves the real output room
// for whatever window this actually returns) - ONE set of numbers, never two
// independently-maintained copies.
//
// A real Phase 1-28 local-only acceptance run against the real installed
// Qwen3-30B-A3B hit finish_reason="length" even for a single bounded region.
// A source audit found two independent, compounding causes, not one: (1)
// connected_document_output_agenda_model_tool_definition()'s "outputs"/
// "regions"/(per-region)"fieldDispositions" arrays had no maxItems at all -
// nothing stopped a constrained decode from legally emitting far more array
// entries than the turn's own decision window ever intended, regardless of
// how tightly each individual entry's own fields were bounded (this is now
// fixed at the call site that actually knows the real windowed counts -
// constrain_output_agenda_capabilities() below); (2) the per-unit constants
// here were calibrated against an "observed moderate, non-abusive" real
// sample (41-45 measured tokens for one reason, 77 for one description),
// never against this schema's own maxLength:400 ceiling - i.e. a plausible-
// looking but not actually conservative estimate.
//
// kOutputPlanningEnvelopeTokens covers the bare {outputs:[],
// regions:[{regionHandle,fieldDispositions:[]}],skippedRegions:[],final}
// envelope (measured 27, kept with margin).
constexpr std::size_t kOutputPlanningEnvelopeTokens = 60;
// kOutputPlanningPerFieldTokens is a CONSERVATIVE RESERVATION ESTIMATE, not a
// mathematically proven bound - it still depends on an assumed chars-per-
// token ratio, not the real tokenizer, and must be read that way. It reuses
// the exact same conservative ratio ResearchToolLoop.hpp's Drafting/
// Correction reservation already uses for ITS OWN maxLength-bounded fields
// (kPerWriteFieldTokens: "1500 chars / 2" - i.e. 2 chars/token, chosen there
// specifically because it is pessimistic for multilingual/legal prose, not
// because it is exact), applied here to this schema's own real ceiling
// instead of an observed sample: description(maxLength 400)/2 = 200 tokens,
// reason(maxLength 400)/2 = 200 tokens, should one visible field become a
// full write (one fieldDisposition entry AND one output entry - the
// runtime-validator-matching "every visible field could become write"
// cardinality assumption the previous calibration already correctly made,
// now paired with a correctly-conservative length assumption too), plus a
// generous flat allowance for the small fixed per-object JSON overhead
// (fieldHandle/itemHandle/disposition/braces/quotes/commas) rather than a
// tightly counted one. This raises the previous 160 substantially; that is
// the intended effect (this constant was demonstrably too small in real use,
// not merely differently biased), not a regression to be minimized.
constexpr std::size_t kOutputPlanningPerFieldTokens = 200 + 200 + 100;

inline providers::ModelToolDefinition constrain_output_agenda_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	// operation.item_handles is ALREADY exactly this turn's dependency-closed
	// semantic execution wave, resolved capacity-blind by compile_connected_document_
	// operation_state() (Document/ConnectedDocumentAction.cpp, reading back
	// compile_connected_document_turn_data()'s own selection - see WorkingSet.hpp's
	// top comment) - never a fixed/derived field-count window computed a
	// second time here. It is never a fixed first-N slice. Already-considered regions are excluded upstream
	// (is_output_region_considered()), so the objective naturally advances to
	// the next unresolved region turn over turn with no separate bookkeeping
	// needed here. TOKEN COUNT IS NOT A SEMANTIC COMPLETION CONDITION, and
	// capacity does not decide this grouping either: whether the resulting
	// prepared request physically fits is a separate, later question
	// (ResearchToolLoop.hpp's real CapacityMeasurement-backed admission
	// check), not something this function or its caller resolves by varying
	// how many regions it exposes.
	const std::vector<std::string> &windowedItems = operation.item_handles;
	std::vector<std::string> editableFields;
	for (const std::string &handle : windowedItems) {
		const auto found = operation.editable_field_handles_by_item.find(handle);
		if (found != operation.editable_field_handles_by_item.end())
			editableFields.insert(editableFields.end(), found->second.begin(), found->second.end());
	}
	// Nothing currently visible left to select OR SKIP is only a dead end if
	// there is also nothing already accumulated to finalize - see
	// OperationStateProjection::output_agenda_can_finalize's own comment. In
	// that finalize-only case, an empty item_handles correctly leaves every
	// enum below empty too, so the model's only legal call here is final=true
	// with empty outputs/regions - never a fabricated region.
	// A windowed region with zero editable (Body-role) fields is NOT a dead
	// end on its own - skippedRegions below is enum-constrained to
	// windowedItems, not editableFields, precisely so a real, purely
	// structural row (a header/title/metadata-only row that legitimately
	// resolved to its own item, real DD_report.docx risk-table shape
	// confirmed by chat_execution_tests' item13) can still be explicitly
	// skipped and the objective can advance to the next region, with no
	// output ever required from it. Treating "this one visible region has
	// nothing to WRITE" as equivalent to "nothing is visible at all" was a
	// real defect this exact fixture caught: it turned a legitimate one-
	// region-per-turn skip into a hard, unrecoverable throw.
	if (operation.item_handles.empty() && !operation.output_agenda_can_finalize)
		throw std::logic_error("The current document operation has no visible region for output planning.");
	// "outputs" itemHandle deliberately carries no enum here: it must equal a
	// region selected in the SAME call, which is not knowable ahead of
	// generation (define_connected_document_output_agenda()'s own
	// "invalid_output_target" check enforces it at runtime regardless).
	// fieldHandle DOES get the same real editableFields enum fieldDispositions.
	// fieldHandle already carries below - a real Phase 1-28 local-only
	// acceptance run against the actual installed Qwen3-30B-A3B proved the
	// gap concretely: the model named a real field belonging to a DIFFERENT
	// region as an output's fieldHandle (structurally valid JSON, semantically
	// wrong), was correctly rejected by invalid_output_target at runtime, and
	// had to spend one entire extra real generation turn (~150s of local
	// inference) recovering. The enum cannot fully prevent every such
	// cross-region mismatch (it spans every currently visible item's own
	// editable fields, not just the region(s) this exact call selects - the
	// runtime check stays necessary regardless), but it does foreclose the
	// larger, strictly worse class of a wholly hallucinated/nonexistent
	// fieldHandle for the same near-zero token cost fieldDispositions already
	// pays for the identical list.
	// A windowed region with ZERO editable fields (a real header/title row,
	// same DD_report.docx shape as this function's own comment above) can
	// NEVER be validly selected via "regions" at all - the "region" object's
	// own schema requires fieldDispositions.minItems=1 (a real region-level
	// disposition list, never an empty placeholder), but that region's own
	// editable-field count is 0, so any "regions" entry naming it produces a
	// self-contradictory sub-schema (minItems=1 vs. this exact region's own
	// real maxItems=0 below) that a constrained decoder can still emit an
	// empty array against instead of failing closed - a real governed local
	// Qwen3-30B-A3B acceptance run observed exactly this: the model named
	// the real header region in "regions" with an empty fieldDispositions,
	// correctly rejected by define_connected_document_output_agenda()'s own
	// runtime check, but only after paying a full extra generation turn to
	// recover, when the schema itself should have foreclosed the attempt.
	// The only valid path for such a region was always "skippedRegions"
	// (enum-constrained to the FULL windowedItems just below, unchanged -
	// skipping any visible candidate, editable or not, is always legal) -
	// this narrows "regions"' own regionHandle enum (and its sibling
	// maxItems bound) to exactly the windowed regions that could ever
	// legally appear there, never all of them.
	std::vector<std::string> selectableRegions;
	for (const std::string &handle : windowedItems) {
		const auto found = operation.editable_field_handles_by_item.find(handle);
		if (found != operation.editable_field_handles_by_item.end() && !found->second.empty())
			selectableRegions.push_back(handle);
	}
	auto &properties = tool.parameters_schema["properties"];
	properties["regions"]["items"]["properties"]["regionHandle"]["enum"] = selectableRegions;
	properties["regions"]["items"]["properties"]["fieldDispositions"]["items"]
		["properties"]["fieldHandle"]["enum"] = editableFields;
	if (properties.contains("outputs") && properties["outputs"].contains("items")
			&& properties["outputs"]["items"]["properties"].contains("fieldHandle"))
		properties["outputs"]["items"]["properties"]["fieldHandle"]["enum"] = editableFields;
	if (properties.contains("skippedRegions") && properties["skippedRegions"].contains("items"))
		properties["skippedRegions"]["items"]["enum"] = windowedItems;
	// Real cardinality bounds, mirroring constrain_draft_capabilities()'s own
	// "updates".maxItems precedent (TurnContract.hpp - "one item can never
	// legitimately submit more WRITE updates than it actually has editable
	// fields"): every scalar field in this schema was already bounded
	// (maxLength on description/reason, additionalProperties:false, enums
	// above), but nothing previously bounded how many array ENTRIES a call
	// could submit - a source audit found this is the one real gap between
	// this function and its sibling, and it is a real degree of freedom a
	// constrained decoder can still legally exploit (repeated/near-duplicate
	// entries, or more entries than the turn's own decision window ever
	// exposed) without violating any per-entry constraint. A region can
	// never legitimately need more fieldDispositions than the whole windowed
	// set's own editable field count (this is the same safe-but-loose
	// union bound constrain_draft_capabilities() itself accepts once more
	// than one item is eligible, not a per-region-tight bound); "outputs"
	// can never legitimately exceed one entry per windowed editable field;
	// "regions"/"skippedRegions" can never exceed the windowed region count.
	properties["regions"]["maxItems"] = selectableRegions.size();
	properties["regions"]["items"]["properties"]["fieldDispositions"]["maxItems"] = editableFields.size();
	if (properties.contains("outputs")) properties["outputs"]["maxItems"] = editableFields.size();
	if (properties.contains("skippedRegions")) properties["skippedRegions"]["maxItems"] = windowedItems.size();
	return tool;
}

// document.evidence.define: only ever exposed at OperationWorkFrontier::
// PlanningEvidence, which ConnectedDocumentAction.cpp only ever reaches when
// more than one candidate document was explicitly named (see
// ConnectedDocumentActionPlan::requires_evidence_agenda). Enum-constrains the
// declarable names to the real candidate documents this operation already
// resolved and structurally read - THE MODEL OWNS THE JUDGMENT of which of
// them (if any) are required source evidence, never Singularity.
inline providers::ModelToolDefinition constrain_evidence_agenda_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	if (tool.parameters_schema.contains("properties")
			&& tool.parameters_schema["properties"].contains("requiredSourceDocumentHandles"))
		constrain_array_string_items(tool.parameters_schema["properties"]["requiredSourceDocumentHandles"],
			operation.candidate_document_handles);
	return tool;
}

// document.review.submit: only ever exposed at OperationWorkFrontier::
// ProfessionalReview - see ConnectedDocumentAction.hpp's own comment on why
// this second gate exists. Only evidenceObjectiveId is enum-constrained (it
// drives backend routing on a NeedsMoreEvidence verdict - a hallucinated id
// would silently fail to route anywhere); tableHandle/fieldHandle references
// are validated against real plan state inside the handler itself instead,
// the same pattern document.draft.upsert's own fieldHandle already uses once
// more than one item is eligible (see constrain_draft_capabilities()).
inline providers::ModelToolDefinition constrain_professional_review_capabilities(
		providers::ModelToolDefinition tool, const OperationStateProjection &operation) {
	auto &properties = tool.parameters_schema["properties"];
	if (properties.contains("issues") && properties["issues"].contains("items")) {
		auto &issueProperties = properties["issues"]["items"]["properties"];
		if (issueProperties.contains("evidenceObjectiveId")) {
			// A nullable scalar (never "not provided" under OpenAI strict mode -
			// see connected_document_professional_review_model_tool_definition()'s
			// own comment), so its own enum must include null alongside every
			// real known objective id, exactly like an empty operation.evidence_
			// objective_ids correctly leaves only null selectable.
			nlohmann::json allowed = nlohmann::json::array({nullptr});
			for (const std::string &id : operation.evidence_objective_ids) allowed.push_back(id);
			issueProperties["evidenceObjectiveId"]["enum"] = std::move(allowed);
		}
	}
	return tool;
}

inline nlohmann::json fallback_tool_call_schema(const std::string &action,
		const nlohmann::json &argumentsSchema) {
	return {{"type", "object"}, {"properties", {
		{"type", {{"type", "string"}, {"enum", nlohmann::json::array({"tool_call"})}}},
		{"tool", {{"type", "string"}, {"enum", nlohmann::json::array({action})}}},
		{"arguments", argumentsSchema}}},
		{"required", nlohmann::json::array({"type", "tool", "arguments"})},
		{"additionalProperties", false}};
}

struct CompiledFallbackContract {
	providers::GenerationOutputContract provider_contract;
	routing::ResponseRestorationContract response_restoration;
};

inline CompiledFallbackContract compile_fallback_contract(
		const TurnContract &contract, const TurnContractInput &input) {
	nlohmann::json alternatives = nlohmann::json::array();
	routing::ResponseRestorationContract restoration;
	auto append = [&](const nlohmann::json &schema, nlohmann::json dataPlaneShape) {
		alternatives.push_back(schema);
		restoration.structured_variants.push_back({schema, std::move(dataPlaneShape)});
	};
	if (contract.phase == TurnFrontier::ResearchDiscovery) {
		const auto &tool = contract.exposed_tools.front();
		append(fallback_tool_call_schema("web.search", tool.parameters_schema),
			{{"properties", {{"arguments", tool.response_data_plane_shape}}}});
	} else if (contract.phase == TurnFrontier::ResearchRetrieval) {
		const auto &tool = contract.exposed_tools.front();
		append(fallback_tool_call_schema("web.open", tool.parameters_schema),
			{{"properties", {{"arguments", tool.response_data_plane_shape}}}});
	} else if (contract.phase == TurnFrontier::EvidencePlanning) {
		const auto &tool = contract.exposed_tools.front();
		append(fallback_tool_call_schema(input.evidence_agenda_action, tool.parameters_schema),
			{{"properties", {{"arguments", tool.response_data_plane_shape}}}});
	} else if (contract.phase == TurnFrontier::ProfessionalReview) {
		const auto &tool = contract.exposed_tools.front();
		append(fallback_tool_call_schema(input.professional_review_action, tool.parameters_schema),
			{{"properties", {{"arguments", tool.response_data_plane_shape}}}});
	} else if (contract.phase == TurnFrontier::Drafting
			|| contract.phase == TurnFrontier::Correction
			|| contract.phase == TurnFrontier::OutputPlanning) {
		const auto upsert = std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
			[&](const auto &tool) { return tool.wire_name == input.draft_upsert_tool.wire_name; });
		const auto request = std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
			[&](const auto &tool) { return tool.wire_name == input.draft_request_research_tool.wire_name; });
		const auto inspect = !input.draft_inspect_tool.wire_name.empty()
			? std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
				[&](const auto &tool) { return tool.wire_name == input.draft_inspect_tool.wire_name; })
			: contract.exposed_tools.end();
		const auto mapNext = !input.table_map_next_tool.wire_name.empty()
			? std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
				[&](const auto &tool) { return tool.wire_name == input.table_map_next_tool.wire_name; })
			: contract.exposed_tools.end();
		const auto outputAgenda = !input.output_agenda_tool.wire_name.empty()
			? std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
				[&](const auto &tool) { return tool.wire_name == input.output_agenda_tool.wire_name; })
			: contract.exposed_tools.end();
		const auto methodologyReveal = !input.methodology_reveal_tool.wire_name.empty()
			? std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
				[&](const auto &tool) { return tool.wire_name == input.methodology_reveal_tool.wire_name; })
			: contract.exposed_tools.end();
		if (outputAgenda != contract.exposed_tools.end())
			append(fallback_tool_call_schema(input.output_agenda_action, outputAgenda->parameters_schema),
				{{"properties", {{"arguments", outputAgenda->response_data_plane_shape}}}});
		if (upsert != contract.exposed_tools.end())
			append(fallback_tool_call_schema(input.draft_upsert_action, upsert->parameters_schema),
				{{"properties", {{"arguments", upsert->response_data_plane_shape}}}});
		if (request != contract.exposed_tools.end())
			append(fallback_tool_call_schema(input.draft_request_research_action, request->parameters_schema),
				{{"properties", {{"arguments", request->response_data_plane_shape}}}});
		if (inspect != contract.exposed_tools.end())
			append(fallback_tool_call_schema(input.draft_inspect_action, inspect->parameters_schema),
				{{"properties", {{"arguments", inspect->response_data_plane_shape}}}});
		if (mapNext != contract.exposed_tools.end())
			append(fallback_tool_call_schema(input.table_map_next_action, mapNext->parameters_schema),
				{{"properties", {{"arguments", mapNext->response_data_plane_shape}}}});
		if (methodologyReveal != contract.exposed_tools.end())
			append(fallback_tool_call_schema(input.methodology_reveal_action, methodologyReveal->parameters_schema),
				{{"properties", {{"arguments", methodologyReveal->response_data_plane_shape}}}});
		// THE MODEL OWNS WHEN RESEARCH IS DONE (compile_turn_contract()'s own
		// resolved Unknown/research-floor-vs-ceiling comment above): once
		// research_mode==Required additively appends web.search/web.open to
		// exposed_tools alongside this phase's own primary actions, the
		// fallback/textual protocol's own schema-matching and response-
		// restoration variants must recognize both, exactly like the native
		// protocol already does by construction (nativeControlForCurrentContract()
		// reads exposed_tools directly) - a fallback-protocol model that
		// legitimately wants a second, professionally distinct search must
		// never have its own well-formed {"type":"tool_call","tool":
		// "web.search",...} response rejected as "did not match the current
		// turn contract" merely because this branch never looked for it.
		const auto search = !input.search_tool.wire_name.empty()
			? std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
				[&](const auto &tool) { return tool.wire_name == input.search_tool.wire_name; })
			: contract.exposed_tools.end();
		const auto open = !input.open_tool.wire_name.empty()
			? std::find_if(contract.exposed_tools.begin(), contract.exposed_tools.end(),
				[&](const auto &tool) { return tool.wire_name == input.open_tool.wire_name; })
			: contract.exposed_tools.end();
		if (search != contract.exposed_tools.end())
			append(fallback_tool_call_schema("web.search", search->parameters_schema),
				{{"properties", {{"arguments", search->response_data_plane_shape}}}});
		if (open != contract.exposed_tools.end())
			append(fallback_tool_call_schema("web.open", open->parameters_schema),
				{{"properties", {{"arguments", open->response_data_plane_shape}}}});
		// Multi-delta is a bounded array of independent upserts. Research
		// transition requests remain single so a generation cannot mix a state
		// transition with sibling mutations. Only worth exposing at all when
		// more than one item is actually visible this turn: with exactly one
		// (item13's own real per-row incremental drafting turn - a full
		// second copy of upsert's own schema, ~790 tokens, measured paid on
		// every single-item Drafting turn regardless), its own "calls" array
		// maxItems would already be capped at 1 - a batch of one item
		// achieves nothing a plain single document.draft.upsert call does not
		// already do, so this alternative would be pure currently-unusable
		// weight for that turn.
		if (upsert != contract.exposed_tools.end() && contract.handles.item_handles.size() > 1) {
			nlohmann::json multiDeltaDataPlaneShape = nlohmann::json::object();
			multiDeltaDataPlaneShape["properties"]["calls"]["items"]["properties"]["arguments"] =
				upsert->response_data_plane_shape;
			append({
				{"type", "object"}, {"properties", {
					{"type", {{"type", "string"}, {"enum", nlohmann::json::array({"tool_calls"})}}},
					{"calls", {{"type", "array"}, {"minItems", 1},
						{"maxItems", contract.handles.item_handles.size()},
						{"items", {{"type", "object"}, {"properties", {
							{"tool", {{"type", "string"}, {"enum", nlohmann::json::array({input.draft_upsert_action})}}},
							{"arguments", upsert->parameters_schema}}},
							{"required", nlohmann::json::array({"tool", "arguments"})},
							{"additionalProperties", false}}}}}}},
				{"required", nlohmann::json::array({"type", "calls"})}, {"additionalProperties", false}},
				std::move(multiDeltaDataPlaneShape));
		}
	}
	return {{std::string("singularity_turn_") + to_string(contract.phase),
		{{"type", "object"}, {"anyOf", std::move(alternatives)}}}, std::move(restoration)};
}

inline TurnContract compile_turn_contract(const TurnContractInput &input) {
	TurnContract result;
	result.operation_id = input.operation.operation_id;
	result.operation_revision = input.operation.revision;
	result.turn_contract_revision = input.operation.revision;
	result.data_plane = input.operation.data_plane;
	result.handles.item_handles = input.operation.item_handles;
	result.handles.field_handles = input.operation.field_handles;
	result.handles.project_evidence_handles_by_item = input.operation.project_evidence_handles_by_item;
	result.handles.editable_field_handles_by_item = input.operation.editable_field_handles_by_item;
	result.handles.methodology_handles = input.operation.methodology_handles;
	result.handles.supportable_external_handles = input.operation.supportable_external_handles;
	result.handles.research_candidate_handles = input.operation.research_candidate_handles;
	result.handles.inspectable_table_handles = input.operation.inspectable_table_handles;
	result.handles.source_handles = input.operation.supportable_external_handles;
	result.handles.source_handles.insert(result.handles.source_handles.end(),
		input.operation.research_candidate_handles.begin(), input.operation.research_candidate_handles.end());

	const bool reopenedResearch = input.operation.frontier == OperationWorkFrontier::NeedsResearch;
	const std::size_t searchBaseline = reopenedResearch
		? input.operation.research_search_calls_at_request : 0;
	const std::size_t openBaseline = reopenedResearch
		? input.operation.research_open_calls_at_request : 0;
	const bool initialResearchOutstanding = input.research_mode == ResearchMode::Required
		&& input.progress.open_calls_used == 0;
	const bool researchOutstanding = reopenedResearch || initialResearchOutstanding;
	if (reopenedResearch && input.operation.requested_research_source_handle) {
		result.phase = TurnFrontier::ResearchRetrieval;
		result.allowed_actions = {"web.open"};
		result.exposed_tools = {constrain_source_handles(input.open_tool,
			std::vector<std::string>{*input.operation.requested_research_source_handle})};
	} else if (researchOutstanding && (input.progress.search_calls_used <= searchBaseline
			|| input.operation.research_candidate_handles.empty())) {
		result.phase = TurnFrontier::ResearchDiscovery;
		result.allowed_actions = {"web.search"};
		result.exposed_tools = {input.search_tool};
	} else if (researchOutstanding && input.progress.open_calls_used <= openBaseline) {
		result.phase = TurnFrontier::ResearchRetrieval;
		result.allowed_actions = {"web.open"};
		result.exposed_tools = {constrain_source_handles(input.open_tool,
			input.operation.research_candidate_handles)};
	} else if (input.operation.frontier == OperationWorkFrontier::Complete) {
		result.phase = TurnFrontier::Complete;
	} else if (input.operation.frontier == OperationWorkFrontier::ProfessionalReview) {
		result.phase = TurnFrontier::ProfessionalReview;
		result.allowed_actions = {input.professional_review_action};
		result.exposed_tools = {constrain_professional_review_capabilities(
			input.professional_review_tool, input.operation)};
	} else if (input.operation.frontier == OperationWorkFrontier::PlanningEvidence) {
		result.phase = TurnFrontier::EvidencePlanning;
		result.allowed_actions = {input.evidence_agenda_action};
		result.exposed_tools = {constrain_evidence_agenda_capabilities(input.evidence_agenda_tool, input.operation)};
	} else if (input.operation.frontier == OperationWorkFrontier::PlanningOutputs) {
		result.phase = TurnFrontier::OutputPlanning;
		// Stays reachable once something is already accumulated even with
		// nothing new currently selectable - see OperationStateProjection::
		// output_agenda_can_finalize's own comment: the model's only real
		// remaining action is to close the agenda with final=true.
		if (!input.operation.item_handles.empty() || input.operation.output_agenda_can_finalize) {
			result.allowed_actions = {input.output_agenda_action};
			result.exposed_tools = {constrain_output_agenda_capabilities(input.output_agenda_tool, input.operation)};
		}
		if (!input.operation.inspectable_table_handles.empty()) {
			result.exposed_tools.push_back(constrain_inspect_capabilities(
				input.draft_inspect_tool, input.operation));
			result.allowed_actions.push_back(input.draft_inspect_action);
		}
		if (!input.operation.table_map_next_document_handles.empty()) {
			result.exposed_tools.push_back(constrain_table_map_next_capabilities(
				input.table_map_next_tool, input.operation));
			result.allowed_actions.push_back(input.table_map_next_action);
		}
		// Every region of every candidate document has been decided and none was
		// selected: the model found nothing in the Project's documents that this
		// request calls for changing. That is a real outcome, not an internal
		// fault - typed so the operation can deliver the answer it already has
		// instead of failing (see OutputPlanningFoundNoRegion).
		if (result.exposed_tools.empty())
			throw OutputPlanningFoundNoRegion(
				"No region of any Project document was selected for this request, so there is nothing to change.");
	} else {
		result.phase = input.operation.frontier == OperationWorkFrontier::Correction
			? TurnFrontier::Correction : TurnFrontier::Drafting;
		// Compact-context mode (see ConnectedDocumentActionPlan::
		// compact_context_mode) can legitimately leave zero items exposed
		// before any document.inspect call - every real candidate is still
		// hidden behind a collapsed table. A required-evidence operation with
		// more than one required document can equally leave every visible
		// item with zero editable fields (a real document may be shown for
		// review - see ConnectedDocumentAction.cpp's is_item_collapsed() -
		// before every required document is grounded for drafting - see its
		// document_grounding_complete_for_drafting()), which is the same
		// "nothing draftable yet" state by a different real cause.
		// constrain_draft_capabilities() throws when it finds no item with a
		// real editable field (a correct invariant for every other caller,
		// where that means something is genuinely wrong) so it must simply
		// not be called in either of these specific cases: the model is
		// steered to document.inspect only, never handed a broken/empty
		// upsert tool. editable_field_handles_by_item (not item_handles) is
		// the right test - its own keys are already a subset of item_handles
		// populated only once grounding allows drafting at all (see
		// OperationStateProjection::editable_field_handles_by_item's own
		// comment), so this single check correctly covers both causes.
		if (input.operation.editable_field_handles_by_item.empty()
				&& !input.operation.inspectable_table_handles.empty()) {
			result.allowed_actions = {input.draft_inspect_action};
			result.exposed_tools = {constrain_inspect_capabilities(input.draft_inspect_tool, input.operation)};
			if (!input.operation.table_map_next_document_handles.empty()) {
				result.exposed_tools.push_back(constrain_table_map_next_capabilities(
					input.table_map_next_tool, input.operation));
				result.allowed_actions.push_back(input.table_map_next_action);
			}
		} else if (!input.operation.methodology_handles.empty()
				&& input.operation.methodology_reveal_candidate_handles.size()
					== input.operation.methodology_handles.size()) {
			// Sequenced exactly like ResearchDiscovery/ResearchRetrieval above:
			// a real methodology deck governs how this same turn's own
			// draft.upsert must assess its target field (see the golden
			// request's own "согласно правилам оценки рисков в файле ..." -
			// the delegated methodology IS the professional grading rule), so
			// a turn with NOTHING yet revealed genuinely has only one honest
			// next action - reveal one slide - never a real choice between
			// that and drafting blind. Exposing document.draft.upsert's own
			// full schema (and document.draft.request_research's) on a turn
			// that offers nothing a well-formed draft could actually rest on
			// is currently-unusable weight, exactly the category constrain_
			// inspect_capabilities()/the OutputPlanning evidenceObjective gate
			// above already exist for - a real, substantial fraction of a
			// small local model's own genuinely oversized Drafting turn (see
			// item13's own investigation). Once at least one slide has been
			// revealed (methodology_reveal_candidate_handles has shrunk),
			// every later turn returns to today's combined behavior below -
			// this only ever delays the model's very first upsert by one
			// bounded reveal turn, never blocks it, and never re-triggers
			// once real progress exists.
			result.allowed_actions = {input.methodology_reveal_action};
			result.exposed_tools = {constrain_methodology_reveal_capabilities(
				input.methodology_reveal_tool, input.operation)};
			if (!input.operation.inspectable_table_handles.empty()) {
				result.exposed_tools.push_back(constrain_inspect_capabilities(
					input.draft_inspect_tool, input.operation));
				result.allowed_actions.push_back(input.draft_inspect_action);
			}
			if (!input.operation.table_map_next_document_handles.empty()) {
				result.exposed_tools.push_back(constrain_table_map_next_capabilities(
					input.table_map_next_tool, input.operation));
				result.allowed_actions.push_back(input.table_map_next_action);
			}
		} else {
			result.allowed_actions = {input.draft_upsert_action, input.draft_request_research_action};
			result.exposed_tools = {constrain_draft_capabilities(input.draft_upsert_tool, input.operation)};
			if (!input.operation.research_candidate_handles.empty()) {
				result.exposed_tools.push_back(constrain_research_request_capabilities(
					input.draft_request_research_tool, input.operation));
			} else {
				result.allowed_actions = {input.draft_upsert_action};
			}
			if (!input.operation.inspectable_table_handles.empty()) {
				result.exposed_tools.push_back(constrain_inspect_capabilities(
					input.draft_inspect_tool, input.operation));
				result.allowed_actions.push_back(input.draft_inspect_action);
			}
			if (!input.operation.table_map_next_document_handles.empty()) {
				result.exposed_tools.push_back(constrain_table_map_next_capabilities(
					input.table_map_next_tool, input.operation));
				result.allowed_actions.push_back(input.table_map_next_action);
			}
			if (!input.operation.methodology_reveal_candidate_handles.empty()) {
				result.exposed_tools.push_back(constrain_methodology_reveal_capabilities(
					input.methodology_reveal_tool, input.operation));
				result.allowed_actions.push_back(input.methodology_reveal_action);
			}
		}
	}

	// THE MODEL OWNS WHEN RESEARCH IS DONE: ResearchMode::Required is a real
	// professional floor (at least one search+open before drafting, enforced
	// by researchOutstanding above) - never a ceiling that permanently closes
	// research the instant that floor is first crossed. The original design
	// treated progress.open_calls_used==0 as the ENTIRE "is research still
	// outstanding" question, so the very first accepted web.open silently
	// made every later web.search "out_of_contract_action" forever,
	// rejecting a second,
	// professionally distinct research question as though it were a repeat
	// of the first. A real Gemini 2.5 Flash/OpenRouter acceptance scenario
	// (captured_http_replay.cpp's own two-distinct-query fault-injection
	// fixture: "аспект риска А" and "аспект риска Б" - two real, different
	// sub-questions the same request legitimately raises) proved this
	// concretely. Appended - never substituted - once the operation has
	// moved past the initial research phases into ongoing work, so the
	// model always retains the option to research again alongside whatever
	// this turn's own primary phase already offers; nothing here ever
	// forces another search, and nothing here weakens the floor above (a
	// reopened NeedsResearch turn keeps its own narrow, single-source
	// exposure unchanged - see the reopenedResearch branch's own comment).
	if (input.research_mode == ResearchMode::Required && !reopenedResearch
			&& (result.phase == TurnFrontier::OutputPlanning
				|| result.phase == TurnFrontier::Drafting
				|| result.phase == TurnFrontier::Correction)) {
		result.allowed_actions.push_back("web.search");
		result.exposed_tools.push_back(input.search_tool);
		if (!input.operation.research_candidate_handles.empty()) {
			result.allowed_actions.push_back("web.open");
			result.exposed_tools.push_back(constrain_source_handles(
				input.open_tool, input.operation.research_candidate_handles));
		}
	}

	result.context_phase = result.phase;
	result.allowed_actions_phase = result.phase;
	result.tool_surface_phase = result.phase;
	result.fallback_grammar_phase = result.phase;
	result.validator_phase = result.phase;
	if (result.phase != TurnFrontier::Complete) {
		result.tool_choice = result.exposed_tools.size() == 1
			? providers::ToolChoicePolicy::ForceNamed : providers::ToolChoicePolicy::Required;
		if (result.exposed_tools.size() == 1)
			result.forced_tool_wire_name = result.exposed_tools.front().wire_name;
		auto fallback = compile_fallback_contract(result, input);
		result.fallback_output_contract = std::move(fallback.provider_contract);
		result.response_restoration = std::move(fallback.response_restoration);
	} else {
		// Complete exposes no tool at all (see its own branch above) - the
		// model's own expected response here is exactly the short plain-
		// language completion sentence MatterRequestExecution.cpp's own
		// research_final_text_instruction asks for, never a further tool
		// call or structured payload. A live GPT-5.1 acceptance run against
		// a real strict-mode provider proved the previous default (false,
		// meaning "reject any unstructured text") a genuine dead end: the
		// model did exactly what it was told, submitted its last accepted
		// draft.upsert with final=true, correctly reached Complete, then
		// gave the requested plain completion sentence - which Singularity's own
		// response-restoration layer then refused as an unrecognized shape,
		// failing the entire otherwise-successful operation.
		result.response_restoration.unstructured_text_is_data = true;
	}
	result.control_plane.value = {
		{"protocol", "singularity.turn_contract.v1"},
		{"operationId", result.operation_id},
		{"operationRevision", result.operation_revision},
		{"turnContractRevision", result.turn_contract_revision},
		{"currentFrontier", to_string(result.phase)},
		{"allowedActions", result.allowed_actions},
		// "items"/"fields"/"projectEvidenceByItem"/"methodology" used to
		// repeat here verbatim every turn - result.handles' own purpose is
		// solely to feed the constrain_*_capabilities() enum-building above
		// (nothing downstream ever reads this control-plane copy back - see
		// its own grep-verified absence from every response-handling path),
		// and every one of those four is already the exact same real handle
		// set the model sees per-field in "state.targets" and per-slide in
		// "state.methodology" whenever those are populated (every frontier
		// that has a real editable/methodology handle to offer at all) -
		// paying to also list each one here a second, structurally different
		// way added nothing a model turn could act on. "editableFieldsByItem"/
		// "sources"/"supportableExternal"/"researchCandidates"/
		// "inspectableTables" remain: none of those is otherwise consolidated
		// anywhere else in one place the model can read directly.
		{"allowedHandles", {
			{"editableFieldsByItem", result.handles.editable_field_handles_by_item},
			{"sources", result.handles.source_handles},
			{"supportableExternal", result.handles.supportable_external_handles},
			{"researchCandidates", result.handles.research_candidate_handles},
			{"inspectableTables", result.handles.inspectable_table_handles}}},
		{"toolNames", nlohmann::json::array()},
		{"toolChoice", result.tool_choice == providers::ToolChoicePolicy::ForceNamed ? "force_named"
			: result.tool_choice == providers::ToolChoicePolicy::Auto ? "auto" : "required"},
		{"forcedToolName", result.forced_tool_wire_name ? nlohmann::json(*result.forced_tool_wire_name)
			: nlohmann::json(nullptr)},
		{"fallbackSchemaId", result.fallback_output_contract
			? nlohmann::json(result.fallback_output_contract->schema_name) : nlohmann::json(nullptr)},
		{"budget", {{"modelTurnsUsed", input.progress.model_turns_used},
			{"successfulToolCalls", input.progress.successful_tool_calls_used},
			{"searchCallsUsed", input.progress.search_calls_used},
			{"openCallsUsed", input.progress.open_calls_used}}},
	};
	for (const auto &tool : result.exposed_tools)
		result.control_plane.value["toolNames"].push_back(tool.wire_name);
	return result;
}

inline routing::ModelVisibleTurn compile_turn_prompt(const TurnContract &contract) {
	if (!contract.phases_are_atomic())
		throw std::logic_error("The operation turn contract phase projections diverged.");
	std::string instruction;
	if (contract.phase == TurnFrontier::ResearchDiscovery)
		instruction = "Return exactly one web.search tool call. Generic final prose is not an operation action.";
	else if (contract.phase == TurnFrontier::ResearchRetrieval)
		instruction = "Return exactly one web.open tool call using an issued S handle. Generic final prose is not an operation action.";
	else if (contract.phase == TurnFrontier::EvidencePlanning) {
		instruction = "Your instruction named more than one Project document. Call document.evidence.define exactly "
			"once to declare which named document(s), if any, are required source material you must inspect before "
			"drafting - not every named document is evidence; one may be your destination, a template, or optional "
			"context. An empty list is a valid answer when none of the others require review. Generic final prose "
			"is not an operation action.";
	} else if (contract.phase == TurnFrontier::OutputPlanning) {
		if (contract.handles.item_handles.empty()) {
			// Reaching this phase with nothing currently selectable is only
			// legal when something was already accumulated and can now be
			// finalized (compile_turn_contract() throws otherwise - see its
			// own comment) - a real live-API run against openai/gpt-4o-mini
			// proved the generic instruction below is not explicit enough
			// for this exact dead end: the model kept resubmitting an
			// already-selected (and correctly rejected) region instead of
			// recognizing there was nothing left to add.
			instruction = "Every currently visible region has already been selected in an earlier "
				"document.outputs.define call this same operation - see \"outputAgendaSelectedSoFar\". There is "
				"nothing new to add. Call document.outputs.define exactly once more with empty \"outputs\" and "
				"\"regions\" arrays and final=true to close the agenda now; drafting begins immediately after.";
		} else {
			// Tightened wording, same facts, for a small local model's own real
			// per-turn budget (see item13's own investigation) - every fact this
			// paragraph states before tightening remains stated.
			instruction = "Map each requested deliverable to its exact editable Body field via document.outputs."
				"define - never a notes/footer field. Select only the region(s) (\"regionHandle\") you are drafting "
				"into. Each selected region's own \"fieldDispositions\" must cover every one of that region's own "
				"editable fields exactly once (never another region/item's field): \"write\" (with a matching "
				"\"outputs\" entry) or \"leave_unchanged\" (with a reason). Fields of a region you did not select "
				"need no disposition. Use document.inspect first if the right region is not yet visible";
			if (contract.allows("document.map.next"))
				instruction += ", or document.map.next if a named document's table map is missing tables (see its "
					"own \"moreTablesAvailable\")";
			instruction += ".";
		}
	} else if (contract.phase == TurnFrontier::ProfessionalReview) {
		instruction = "The draft is otherwise structurally complete. Before it may be committed, review it yourself "
			"as a supervising professional would, using only the evidence objectives, coverage, working notes, "
			"selected region and proposed field values shown - never resend or re-derive from raw table content "
			"you no longer see. Check: (A) could evidence you have not yet covered materially change the "
			"conclusion, and did any exhaustive/inventory/absence question actually receive exhaustive coverage; "
			"(B) does the draft preserve the specific items your own inspection surfaced rather than collapsing "
			"them into an unsupported generic statement; (C) does the draft omit inspected evidence that "
			"materially mitigates, qualifies or contradicts its own conclusion; (D) does the selected template "
			"have an adjacent field a complete work product of this kind would need, and is every leave_unchanged "
			"disposition still defensible now; (E) is the generated prose coherent, free of duplicated/alien "
			"words, cross-field leakage or broken grammar; (F) is every material claim actually supported by "
			"evidence you inspected. Call document.review.submit exactly once with your verdict: \"pass\" only "
			"when none of these are a real concern; otherwise \"needs_more_evidence\" (cite the exact "
			"evidenceObjectiveId/tableHandle), \"needs_output_replan\", or \"needs_redraft\" (cite the exact "
			"fieldHandle), each with the specific issue(s) that must be fixed. Generic final prose is not an "
			"operation action.";
	} else if (contract.phase == TurnFrontier::Drafting || contract.phase == TurnFrontier::Correction) {
		if (!contract.allows("document.draft.upsert") && contract.allows("document.methodology.reveal")) {
			// compile_turn_contract()'s own reveal-first branch: nothing has
			// been revealed yet and the request names a real methodology deck
			// as its own grading rule, so document.draft.upsert is not even
			// offered this turn - see that branch's own comment.
			instruction = "Call document.methodology.reveal exactly once to reveal every methodology slide relevant "
				"to this professional objective in one call - \"methodologyHandles\" is an array and \"methodology\" "
				"lists the not-yet-revealed candidates. Do not call once per slide. Each accepted handle remains "
				"semantically revealed for this operation even after its verbatim text leaves the bounded current "
				"projection. document.draft.upsert becomes available once at least one slide is revealed.";
			if (contract.allows("document.inspect"))
				instruction += " You may instead call document.inspect to reveal the real rows of a collapsed "
					"table before drafting against it.";
			if (contract.allows("document.map.next"))
				instruction += " Or document.map.next to reveal more of a named document's own table map when its "
					"\"moreTablesAvailable\" is true.";
			instruction += " Generic final prose is not an operation action.";
		} else {
			instruction = "Return one document.draft.upsert or a batch of independent document.draft.upsert calls";
			if (contract.allows("document.draft.request_research"))
				instruction += ", or one document.draft.request_research for an issued pending item and research candidate";
			if (contract.allows("document.inspect"))
				instruction += ", or one document.inspect call to reveal the real rows of a collapsed table before drafting against it";
			if (contract.allows("document.map.next"))
				instruction += ", or one document.map.next call to reveal more of a named document's own table map when "
					"its \"moreTablesAvailable\" is true";
			if (contract.allows("document.methodology.reveal"))
				instruction += ", or one document.methodology.reveal call listing every not-yet-revealed methodology "
					"slide relevant to the current professional objective in methodologyHandles - do not call once per "
					"slide; accepted handles remain semantically revealed and visible dependencies";
			instruction += ". Generic final prose is not an operation action.";
		}
	}
	auto data = contract.data_plane;
	data.value["turnInstruction"] = instruction;
	return {contract.control_plane, std::move(data), contract.response_restoration};
}

} // namespace singularity::research
