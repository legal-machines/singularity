#pragma once

#include "Document/Types/DocumentActionEvidence.hpp"
#include "Document/Types/DocumentLocator.hpp"
#include "Document/Types/DocumentRoleBinding.hpp"
#include "Document/Types/DocumentSemantics.hpp"
#include "Document/Types/ProfessionalReview.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "ExternalResearch/TurnContract/TurnContract.hpp"
#include "ExternalResearch/WorkingSet/WorkingSet.hpp"
#include "Matter/Types/Matter.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Tool/Types/ToolTypes.hpp"

#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace singularity::documents {

enum class DocumentDraftItemState { Pending, NeedsResearch, NeedsCorrection, Accepted };

struct DocumentDraftItem {
	DocumentDraftItemState state = DocumentDraftItemState::Pending;
	std::optional<nlohmann::json> accepted_action;
	int proposal_attempts = 0;
	std::optional<std::string> last_issue_category;
	std::optional<std::string> last_issue_message;
	std::size_t research_search_calls_at_request = 0;
	std::size_t research_open_calls_at_request = 0;
	std::optional<std::string> requested_research_evidence_id;
};

struct RequiredOutputObligation {
	std::string id;
	std::string description;
	std::string item_handle;
	std::string field_handle;
};

// THE MODEL OWNS WHAT EVIDENCE IS RELEVANT. SINGULARITY OWNS THE COVERAGE STATE OF
// THAT EVIDENCE. An EvidenceObjective is the model's own declaration, made via
// document.inspect's own optional evidenceObjective argument the first time it
// inspects a table it judges relevant to some professional question, that a
// specific bounded table is the structural region a specific question (its own
// free-text description) depends on. Exactly one objective binds to exactly
// one table - this keeps coverage bookkeeping unambiguous; a question that
// genuinely spans more than one table is simply two objectives, one per table.
// EXHAUSTIVE means the model itself judges that question answerable only by
// having looked at every row (completeness, enumeration, negative-evidence
// "what is/was not present" questions); TARGETED means an ordinary bounded
// factual read is enough. Never inferred by Singularity from task text or
// language - see connected_document_inspect_model_tool_definition()'s own
// instruction text, which is model GUIDANCE, never a C++ keyword match.
enum class EvidenceCoverageMode { Exhaustive, Targeted };

// A half-open [start, end) row range this operation has actually shown the
// model, in the bound table's own real row order. Always runtime-computed
// (from document.inspect's own already-clamped rowStart/rowCount), never
// model-authored - the model cannot claim coverage it was not actually given.
struct EvidenceCoverageRange {
	std::size_t start_row = 0;
	std::size_t end_row = 0;
};

struct EvidenceObjective {
	// Model-chosen short stable id (e.g. "EO1"), stable across every later
	// document.inspect call that continues work on the same question.
	std::string id;
	// Model-owned: what professional question this objective is trying to
	// establish. Free text - Singularity never parses or matches it.
	std::string description;
	// Runtime-owned, resolved from the table this objective was first bound
	// to (see request_connected_document_table_inspection()) - never settable
	// by the model directly.
	std::string document_id;
	std::string table_handle;
	// Immutable after the objective's first declaration (a mode flip mid-
	// review would make "coverage complete" retroactively ambiguous).
	EvidenceCoverageMode coverage_mode = EvidenceCoverageMode::Targeted;
	// Runtime-owned, merged/normalized on every document.inspect call that
	// names this objective's own table - see merge_evidence_coverage_range().
	std::vector<EvidenceCoverageRange> inspected_ranges;
	// Model-owned running list, replaced wholesale on every update that
	// supplies it - the model's own open professional questions about this
	// objective, preserved outside its own context the same way
	// inspection_working_note already is.
	std::vector<std::string> unresolved_questions;
	// TARGETED-only explicit closure (see document.inspect's own
	// evidenceObjectiveClosure argument) - the model's own acknowledgement
	// that it is intentionally stopping short of full coverage. Meaningless
	// for EXHAUSTIVE, whose completeness is structural, never narrative.
	bool closed = false;
	std::optional<std::string> closure_reason;
	std::vector<std::string> scope_limitations;
};

enum class DocumentFieldDisposition { Write, LeaveUnchanged };

struct DocumentFieldDispositionBinding {
	std::string item_handle;
	std::string field_handle;
	DocumentFieldDisposition disposition = DocumentFieldDisposition::Write;
	std::optional<std::string> reason;
};

// ReviewIssueCategory/ProfessionalReviewVerdict/ProfessionalReviewIssue/
// ProfessionalReviewResult now live in Document/Types/ProfessionalReview.hpp
// (included above) so ProfessionalArtifactAction's own, simpler review step
// can share the exact same verdict/issue contract - including the JSON shape
// ChatConversation.cpp's professional_review_from_task() already projects to
// the frontend - instead of a second, competing one. Every reference on
// ProfessionalReviewIssue is optional (an issue need not implicate a single
// objective/table/field to be real), but submit_connected_document_
// professional_review() below still requires at least one populated
// reference on every NeedsMoreEvidence/NeedsRedraft issue - see its own
// comment - since a verdict Singularity cannot route anywhere specific cannot
// bound its own correction cycle either.

// THE RUNTIME OWNS THE AGENDA. THE MODEL OWNS THE JUDGMENT. One Project
// document the MODEL itself declared (via define_connected_document_evidence_
// agenda()) is required source evidence it must inspect at least once before
// drafting into ANY of the operation's candidate documents may proceed - see
// is_item_collapsed()'s own extended comment in ConnectedDocumentAction.cpp.
// A named Project document can be a required source, a destination, a
// template/example or optional context; which role each one actually plays is
// legal/document judgment, never a blanket "every other named document is
// evidence" backend inference (CORE PRODUCT LAW). Persisted on
// DocumentActionDraft (not ConnectedDocumentActionPlan) precisely because it
// is model-declared state, not something prepare_connected_document_action()
// can deterministically recompute from the instruction text alone the way the
// old name-counting heuristic could.
struct RequiredEvidenceObligation {
	std::string document_id;
	std::string document_name;
};

struct DocumentActionDraft {
	std::uint64_t revision = 1;
	std::map<std::string, DocumentDraftItem> items;
	// Persisted completion compatibility signal. Explicit final=true still sets
	// it, and the runtime also derives it once every structured WRITE obligation
	// in the model-finalized output agenda has an accepted delta. The agenda,
	// not this model hint, is canonical professional scope; ProfessionalReview
	// remains the independent completion gate.
	bool finished = false;
	// Table handles (see DocumentTableSummary/ConnectedDocumentActionPlan::
	// table_handles below) the model has already revealed via document.inspect
	// - only meaningful when the owning plan's compact_context_mode is true.
	// Persisted across a save/reload exactly like every other draft field
	// (see connected_document_draft_checkpoint_json()) so a resumed operation
	// never has to re-inspect a table it already earned visibility into.
	std::set<std::string> expanded_table_ids;
	// Methodology slide handles (see ConnectedDocumentActionPlan::
	// methodology_handles below) the model has already semantically inspected
	// via document.methodology.reveal - the durable/cumulative record of a real
	// methodology deck's own slides. A reveal is the model's explicit
	// dependency selection, so membership is cumulative and never removed by
	// a physical projection heuristic. Persisted across a
	// save/reload exactly like expanded_table_ids so a resumed operation
	// never treats an inspected slide as unseen.
	//
	// Membership here is permanent and is the source for both citation
	// eligibility and current model visibility. Physical capacity is measured
	// against the exact prepared request later; it never silently drops one of
	// these model-selected professional dependencies.
	std::set<std::string> revealed_methodology_handles;
	// Stable reveal order retained for checkpoint compatibility and forensic
	// audit. It does not override the semantic membership above or define a
	// separate visibility window.
	std::vector<std::string> revealed_methodology_recency;
	// THE MODEL OWNS THE JUDGMENT of evidence SUFFICIENCY, never Singularity: a
	// document_id the model has explicitly told Singularity (via document.
	// inspect's own evidenceSufficient argument, set only on a call that
	// itself inspects one of that document's own tables - see
	// request_connected_document_table_inspection()) it judges its review of
	// that document complete enough to proceed. Inspecting a table is proof
	// the region was looked at; it is never, by itself, proof the model
	// considers its review sufficient - see document_evidence_satisfied(),
	// which requires both.
	std::set<std::string> evidence_declared_sufficient;
	// EVIDENCE SUFFICIENCY IS NOT A BOOLEAN WITHOUT COVERAGE. Keyed by the
	// model's own EvidenceObjective::id - see document_evidence_coverage_
	// complete(), which document_evidence_satisfied() now also requires for
	// every objective bound to a document before that document can be
	// sufficient at all. A document with no declared objective has no
	// coverage obligation beyond today's unchanged inspect-and-declare check;
	// this is purely additive, model-opt-in state.
	std::map<std::string, EvidenceObjective> evidence_objectives;
	// tableHandle -> the one EvidenceObjective::id bound to it, the runtime's
	// own O(1) index into evidence_objectives above - see request_connected_
	// document_table_inspection()'s own comment on the one-objective-per-table
	// invariant.
	std::map<std::string, std::string> table_evidence_objective;
	// A compact model receives one lossless structural row window at a time.
	// Inspection identity remains cumulative in expanded_table_ids, while this
	// disposable frontier prevents previously inspected table bytes from being
	// copied into every later prompt. The model may preserve its own semantic
	// judgment in the bounded, evidence-linked working note below.
	std::optional<std::string> active_table_id;
	std::size_t active_row_start = 0;
	std::size_t active_row_count = 0;
	std::string inspection_working_note;
	std::set<std::string> inspection_working_note_table_ids;
	// The model establishes semantic deliverables once, against finite Body
	// handles issued by Singularity. From that point the runtime owns this agenda
	// and completion remains illegal until every mapped output has an accepted
	// semantic delta. Empty means the planning frontier is still unresolved.
	bool output_agenda_defined = false;
	std::vector<RequiredOutputObligation> required_outputs;
	// document.outputs.define selects one or more bounded structural regions
	// (issued item handles) and gives every editable field in each region an
	// explicit model-authored disposition. RequiredOutputObligation remains the
	// WRITE subset; LEAVE_UNCHANGED fields remain canonical and unmodified.
	// Keeping the selected region and dispositions beside the output agenda is
	// what lets Singularity own completeness without inferring what any visible
	// label means for the professional task.
	std::vector<std::string> selected_output_region_handles;
	std::vector<DocumentFieldDispositionBinding> field_dispositions;
	// A CANDIDATE region the model explicitly judged is NOT part of this
	// request's own required deliverables (document.outputs.define's own
	// "skippedRegions") - distinct from selected_output_region_handles above
	// (candidate examined and accepted) in exactly the way "another
	// candidate is visible" must never be conflated with "another required
	// output remains": a live acceptance run against openai/gpt-4o-mini
	// proved a model forced to act on every visible candidate with no way to
	// decline one will eventually select it anyway, corrupting the agenda
	// with work the user never asked for. Considered exactly like a selected
	// region for visibility purposes (never re-shown, never re-offered) but
	// carries no output/field disposition of its own and is never drafted.
	std::vector<std::string> skipped_output_region_handles;
	// THE MODEL OWNS THE JUDGMENT of which named candidate document(s), if
	// any, are required source evidence - see define_connected_document_
	// evidence_agenda(). Only meaningful when the owning plan's
	// requires_evidence_agenda is true (more than one candidate document was
	// explicitly named); false/empty otherwise, exactly like output_agenda_
	// defined/required_outputs above. May stay empty even after being defined
	// if the model judges no other named document requires review.
	bool evidence_agenda_defined = false;
	std::vector<RequiredEvidenceObligation> required_evidence;
	// Table-map pagination: one level ABOVE the row-window mechanism above
	// (document -> bounded table-map page -> table -> bounded row window),
	// for the same reason the row window exists - a real design-partner
	// acceptance run proved a document with many real tables (e.g. a dozen
	// risk cards) can exceed a small local model's real turn budget just from
	// their compact structural summaries, even with zero row content shown.
	// A per-document cursor (not one global cursor) since more than one
	// document's map may be visible at once (e.g. once required-evidence
	// grounding is satisfied, every candidate document's own map becomes
	// relevant again together). No model-authored note is required here
	// (unlike the row window's own workingEvidenceNote): a table-map entry is
	// purely structural navigation evidence - a stable handle, real row
	// count, and whatever title/header text Office Structure Law already
	// classified - never body content, so nothing substantive is lost by
	// paging on. Every table stays individually reachable via document.
	// inspect (its handle remains valid there regardless of which table-map
	// page is currently shown) for as long as it has appeared on any page.
	std::map<std::string, std::size_t> table_map_page_start;
	// Shared across every document's map, mirroring active_row_count's own
	// halving-on-compaction pattern - a per-document page size would need
	// independent compaction bookkeeping for no real benefit, since only
	// currently-actionable documents ever have a page to begin with.
	// std::numeric_limits<std::size_t>::max() means "not yet capped" (today's
	// unchanged, everything-on-one-page behavior for any document whose real
	// table count already fits); compact_connected_document_inspection_
	// window() is the only place this is ever reduced.
	std::size_t table_map_page_size = std::numeric_limits<std::size_t>::max();
	// The Professional Completion Challenge's own last verdict, once the draft
	// is otherwise structurally complete - see connected_document_operation_
	// frontier()'s own use of this (draft_is_complete() alone no longer reaches
	// OperationWorkFrontier::Complete; a fresh Pass at the CURRENT revision is
	// also required). std::nullopt means no review has run yet for the current
	// draft state.
	std::optional<ProfessionalReviewResult> professional_review;
	// The draft's own `revision` at the moment professional_review was
	// recorded - a Pass whose revision no longer matches the draft's current
	// revision is stale (something changed since) and no longer counts,
	// exactly like a stale accepted proposal anywhere else in this file.
	std::uint64_t professional_review_reviewed_revision = 0;
	// Bounds the review/correct/re-review loop independently of (and usually
	// well inside) the operation's own generic tool-call budget - see
	// submit_connected_document_professional_review()'s own comment: a third
	// consecutive non-Pass verdict fails the whole operation closed rather
	// than committing an incomplete professional document.
	std::size_t professional_review_cycles_used = 0;
};

// Non-substantive structural identity of one collapsed table - what a model
// needs to decide WHETHER to inspect a table, never what it would find
// inside it. title_or_banner_text/header_labels come only from cells
// DocxExtractor.cpp already classified as DocumentCellRole::TitleOrBanner/
// HeaderLabel (generic OOXML formatting evidence, never text content
// matching - see Office Structure Law), so this never leaks a body/
// metadata cell's substantive value ahead of an explicit document.inspect.
struct DocumentTableSummary {
	std::string document_id;
	std::string table_id;
	std::size_t row_count = 0;
	std::optional<std::string> title_or_banner_text;
	std::vector<std::string> header_labels;
};

// One other mutation-capable Project document a model may choose to target
// instead of the plan's own default (see ConnectedDocumentActionPlan::
// other_candidate_documents below) - CORE PRODUCT LAW: which document a
// request is actually about is legal/document judgment, the model's, never
// decided by Singularity matching the instruction's text against a filename.
struct DocumentActionCandidate {
	std::string document_id;
	std::string document_name;
	std::string fingerprint;
	int version = 0;
	DocumentSemanticModel model;
};

// A task-agnostic action plan resolved from explicit document references and
// the user's instruction. Models receive names, roles and structure only;
// trusted identities and state guards remain backend-owned.
struct ConnectedDocumentActionPlan {
	std::string target_document_id;
	std::string target_document_name;
	std::string target_fingerprint;
	int target_version = 0;
	DocumentSemanticModel target_model;
	// Populated only when more than one Project document could plausibly be
	// this request's real target (see prepare_connected_document_action()):
	// every field/item handle below already spans target_model AND every
	// model here, each locator correctly tagged with its own document_id, so
	// the model may propose against whichever one its own reading of the
	// Project actually supports - Singularity never pre-picks among them.
	std::vector<DocumentActionCandidate> other_candidate_documents;
	std::optional<std::string> methodology_document_id;
	std::optional<std::string> methodology_document_name;
	std::optional<std::string> methodology_fingerprint;
	std::optional<DocumentSemanticModel> methodology_model;
	DocumentRoleBindingSet roles;
	bool requires_retrieved_external_evidence = false;
	std::map<std::string, DocumentLocator> field_handles;
	// Runtime-derived Office metadata for each issued field. A Body field's
	// visible label is inherited positionally from the nearest preceding
	// HeaderLabel cell in the same table column; it is never inferred from the
	// task or from legal vocabulary. Placeholder state is equally structural
	// and is exposed so the model need not rediscover template geometry from
	// raw DOCX text.
	std::map<std::string, std::string> field_labels;
	std::map<std::string, bool> field_placeholder_states;
	std::map<std::string, std::vector<std::string>> item_field_handles;
	std::map<std::string, std::vector<std::string>> item_editable_field_handles;
	std::map<std::string, DocumentLocator> methodology_handles;
	// Every real table across every candidate document, keyed by a stable
	// "T<n>" handle - populated unconditionally (see prepare_connected_
	// document_action()) regardless of compact_context_mode, since the
	// table->item association below is needed to decide what to withhold
	// whether or not withholding actually happens.
	std::map<std::string, DocumentTableSummary> table_handles;
	std::map<std::string, std::string> item_table_handle;
	std::map<std::string, std::size_t> item_table_row_index;
	// Decided once, up front, from the resolved model's real capacity (see
	// Routing/InferenceExecutor/InferenceExecutor.hpp's
	// estimate_model_context_window_tokens() and prepare_connected_document_
	// action()'s own budget argument) - CORE PRODUCT LAW/capability-first
	// context delivery: when the full Project structure fits, this stays
	// false and every item is exposed exactly as before this field existed.
	// When it does not fit, table-based items stay hidden from model-visible
	// turn state (compile_connected_document_turn_data()/compile_connected_
	// document_operation_state()) until the model reveals them one table at
	// a time via document.inspect - see DocumentActionDraft::expanded_table_ids.
	//
	// SCOPE, confirmed by a real source-bound consequence audit (every read
	// site: is_item_collapsed()/table_uses_disposable_window()/
	// table_requires_inspection(), compact_connected_document_inspection_
	// window(), the methodology-reveal-first gate, and this turn's own
	// instruction prose): this field is PHYSICAL EXECUTION PLAN machinery
	// only - how many raw rows/tables are delivered per turn, and whether a
	// document.inspect/methodology.reveal step must run first before a row's
	// real content is visible. It NEVER decides, and every read site above
	// was re-verified to never decide: which document is required, which
	// item/region is this turn's ProfessionalObjective (Document/
	// ConnectedDocumentAction.cpp's own Pass 1/Pass 2 resolve that
	// identically regardless of this field - see WorkingSet.hpp's own top
	// comment), semantic atomicity, or dependency closure. The SAME
	// canonical Matter/request/ProfessionalObjective produces the SAME
	// professional truth whether this is true or false; only the number of
	// physical turns and whether a reveal step is needed first can differ.
	// Kept under this name (not renamed to something like
	// disposable_delivery_mode) despite that narrower, now-confirmed scope:
	// a rename would touch every read site, every test fixture that sets it,
	// and every comment above for a pure clarity gain with no behavior
	// change - flagged here instead so a future reader has the real answer
	// without needing to re-derive it.
	bool compact_context_mode = false;
	// Agent-agenda enforcement (THE RUNTIME OWNS THE AGENDA): a structural
	// fact only - true when the instruction explicitly named more than one
	// candidate document. This never decides WHICH of them are actually
	// required source evidence (CORE PRODUCT LAW: that is the model's
	// judgment - see DocumentActionDraft::required_evidence/evidence_agenda_
	// defined, populated only by define_connected_document_evidence_agenda()).
	// It only decides whether that declaration step is mandatory before any
	// item becomes visible - see is_item_collapsed()'s own comment. False for
	// every single-document (or nothing-named) request, unchanged from before
	// this field existed.
	bool requires_evidence_agenda = false;
	std::shared_ptr<DocumentActionDraft> draft = std::make_shared<DocumentActionDraft>();
	std::string model_instruction;
};

struct DocumentDraftToolResult {
	bool accepted = false;
	std::string message;
};

struct ConnectedDocumentActionResult {
	std::string response_text;
	AppliedDocumentActionEvidence action_evidence;
	tools::ToolCallEvidence tool_call_evidence;
	std::vector<research::ExternalEvidenceReference> referenced_external_evidence;
};

// Resolve local document structure. Protection belongs to the complete
// outbound request; this plan never owns a restoration map.
// `availableContextWindowTokens` is the resolved model's real capacity (see
// Routing/InferenceExecutor/InferenceExecutor.hpp's
// estimate_model_context_window_tokens(), called by MatterRequestExecution.cpp
// before this, since which model will run is already known there) - std::
// nullopt (the default, matching every existing caller's unchanged
// behavior) means "assume it fits", never "assume it doesn't": a caller that
// cannot yet resolve a model target simply keeps today's full-exposure
// behavior rather than guessing a capacity it does not have.
//
// The request's source boundary bounds every document the plan reads or may
// change: with an explicit `targetDocumentId` that document is the only
// candidate; with a document selection (`selectedDocumentIds`) only selected
// documents are read - named, unnamed or methodology - and a document outside
// it is never read, sent or changed. Without either (Relevant sources) every
// mutation-capable Project document stays a candidate (owner decision BB-18).
[[nodiscard]] std::optional<ConnectedDocumentActionPlan> prepare_connected_document_action(
	const std::string &userInstruction, const matters::Matter &project,
	std::optional<std::size_t> availableContextWindowTokens = std::nullopt,
	const std::vector<std::string> &selectedDocumentIds = {},
	const std::optional<std::string> &targetDocumentId = std::nullopt);

// Whether `instruction` names a Project document by `name` - exposed here
// (defined in ConnectedDocumentAction.cpp, outside its anonymous namespace)
// purely so a focused unit test can prove this pure text match is
// extension-tolerant (a real prompt names a document by its descriptive
// title, never its file extension) without constructing a full Matter/
// Project fixture. This is a non-blocking ranking hint only: the model
// owns which document its instruction is about (CORE PRODUCT LAW - Singularity
// owns state, not semantic judgment), so prepare_connected_document_action()
// never uses this to exclude a document, only to order candidates.
[[nodiscard]] bool instruction_names_document(const std::string &instruction, const std::string &name);

// Syntax only: this never validates authority, evidence or an operation and
// cannot authorize a write. Full validation still happens at apply below.
[[nodiscard]] bool has_document_action_result_shape(const std::string &modelResult);

// Provider-neutral syntax contract for the tool-free final correction turn.
// The backend remains authoritative for locators, evidence provenance,
// arithmetic and write authorization after schema-valid JSON is returned.
[[nodiscard]] const providers::GenerationOutputContract &connected_document_action_output_contract();

// Model-facing incremental proposal tool. Every call may replace exactly one
// draft item; aliases resolve only against this operation's backend-owned plan.
[[nodiscard]] providers::ModelToolDefinition connected_document_draft_model_tool_definition();
[[nodiscard]] providers::ModelToolDefinition connected_document_research_request_model_tool_definition();
// document.inspect: only ever exposed by TurnContract.hpp's compile_turn_
// contract() when plan.compact_context_mode is true and at least one table
// remains collapsed (see OperationStateProjection::inspectable_table_handles).
[[nodiscard]] providers::ModelToolDefinition connected_document_inspect_model_tool_definition();
[[nodiscard]] providers::ModelToolDefinition connected_document_output_agenda_model_tool_definition();
[[nodiscard]] DocumentDraftToolResult define_connected_document_output_agenda(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments);
// document.evidence.define: only ever exposed when plan.requires_evidence_agenda
// is true (more than one candidate document was explicitly named). THE MODEL
// OWNS THE JUDGMENT of which named document(s), if any, are required source
// evidence - Singularity only resolves each declared name against a real
// candidate document (never a filename/keyword heuristic) and enforces the
// result thereafter.
[[nodiscard]] providers::ModelToolDefinition connected_document_evidence_agenda_model_tool_definition();
[[nodiscard]] DocumentDraftToolResult define_connected_document_evidence_agenda(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments);
// document.methodology.reveal: only ever exposed at Drafting/Correction when
// plan.methodology_handles is non-empty and at least one slide remains
// collapsed (see OperationStateProjection::methodology_reveal_candidate_
// handles). Reveals stay cumulative for the whole operation - see
// DocumentActionDraft::revealed_methodology_handles' own comment.
[[nodiscard]] providers::ModelToolDefinition connected_document_methodology_reveal_model_tool_definition();
[[nodiscard]] DocumentDraftToolResult reveal_connected_document_methodology(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments);
[[nodiscard]] DocumentDraftToolResult upsert_connected_document_draft_item(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence);
[[nodiscard]] DocumentDraftToolResult request_connected_document_draft_research(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const research::OperationExecutionProgress &progress);
void complete_connected_document_draft_research(
	const ConnectedDocumentActionPlan &plan,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const research::OperationExecutionProgress &progress);
// Reveals one collapsed table's real structural items by adding its handle
// to plan.draft->expanded_table_ids. Rejects (accepted=false) an unknown or
// already-expanded tableHandle without mutating anything; matches the
// (bool, string) shape run_research_tool_loop() dispatches identically for
// every operation-tool slot. Never resolves, drafts or reads a cell's
// content itself - it only changes what compile_connected_document_turn_
// data()/compile_connected_document_operation_state() are willing to reveal
// on the NEXT compiled turn.
[[nodiscard]] DocumentDraftToolResult request_connected_document_table_inspection(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments);
// document.map.next: only ever exposed alongside document.inspect (see
// TurnContract.hpp), and only for a document whose current table-map page
// does not yet show every table still requiring inspection. Advances that
// one document's own page cursor; never resolves, drafts or reads a cell's
// content, and never decides which table matters - it only changes which
// slice of that document's own stable table order compile_connected_
// document_turn_data() is willing to reveal on the NEXT compiled turn.
[[nodiscard]] providers::ModelToolDefinition connected_document_map_next_model_tool_definition();
[[nodiscard]] DocumentDraftToolResult request_connected_document_table_map_next_page(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments);
// document.review.submit: only ever exposed at OperationWorkFrontier::
// ProfessionalReview, reached once draft_is_complete() is true but no Pass
// verdict has been recorded for the draft's current revision (see
// connected_document_operation_frontier()). A non-Pass verdict routes the
// operation back to the frontier its own issues implicate (evidence,
// output planning or drafting - see submit_connected_document_professional_
// review()'s own comment); a third consecutive non-Pass verdict fails the
// whole operation closed rather than committing an incomplete document.
[[nodiscard]] providers::ModelToolDefinition connected_document_professional_review_model_tool_definition();
[[nodiscard]] DocumentDraftToolResult submit_connected_document_professional_review(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &arguments);
// The Professional Completion Challenge's own compact, dedicated context -
// deliberately NOT compile_connected_document_turn_context()/turn_data()
// (their accepted item payloads are deliberately absent - see
// ConnectedDocumentActionPlan's own comment - since a drafting turn must
// reason fresh, never just copy its own prior answer forward; the evaluator
// turn is the opposite case, and must see exactly what it is judging). Never
// includes a raw table row dump - only stable handles, the coverage ledger,
// working notes and the proposed field values themselves (Sec.13: smallest
// high-signal context necessary for the next judgment).
[[nodiscard]] nlohmann::json compile_connected_document_review_context(const ConnectedDocumentActionPlan &plan);
// Reduces only the current compact structural row window, or (once that is
// already at its own smallest lossless unit) the current table-map page
// size. Returns false when no smaller lossless unit exists at all, allowing
// the caller to fail before knowingly dispatching an impossible continuation.
[[nodiscard]] bool compact_connected_document_inspection_window(
	const ConnectedDocumentActionPlan &plan);
[[nodiscard]] std::optional<research::FinalTextIssue> connected_document_draft_completion_issue(
	const ConnectedDocumentActionPlan &plan,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence);

// Compiles a disposable, minimum-sufficient model projection from the
// canonical local plan/draft. Accepted item payloads are deliberately absent.
[[nodiscard]] std::string compile_connected_document_turn_context(
	const ConnectedDocumentActionPlan &plan,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const std::optional<research::FinalTextIssue> &issue = std::nullopt);
// `frontier` scopes the instruction text to only the capabilities this exact
// turn actually exposes (see TurnContract.hpp's own tool-exposure gating,
// which this must stay consistent with) - e.g. a PlanningEvidence turn never
// includes document.draft.upsert's own mechanics, since that action is not
// offered until Drafting/Correction. Defaults to Drafting (today's
// unchanged, everything-included behavior) for a caller that does not yet
// know the real current frontier.
//
// This function is CAPACITY-BLIND by design (see WorkingSet.hpp's own top
// comment): it resolves the current ProfessionalObjective's dependency-
// closed minimum sufficient set from canonical state alone - today, at most
// one pending item/region PER real candidate document (never more than one
// per document - a real, source-proven "document structural dependency"
// relation, see constrain_output_agenda_capabilities()'s own grouping
// comment for the live acceptance evidence behind it; the per-document group
// itself is what current domain validation already treats as one atomic
// unit - upsert_connected_document_draft_item()'s submittedWriteFields==
// expectedWriteFields for Drafting/Correction, addRegion()'s complete-
// fieldDispositions requirement for OutputPlanning; see this migration's own
// note classifying that atomicity as a CURRENT COMPATIBILITY BOUNDARY, not a
// proven universal law) - never a capacity-driven window size, and never
// more than one per document merely because pending work remains (unrelated
// pending items of the SAME document are simply ABSENT, not referenced -
// see this migration's own matter-size-invariance requirement). Whether the
// resulting prepared request physically fits is a SEPARATE, later question
// this function has no part in - see ResearchToolLoop.hpp's own real
// CapacityMeasurement-backed admission check.
// `outAdmittedItemHandles`, when non-null, receives exactly which handles
// this call selected as this turn's objective (empty if none is currently
// eligible) - compile_connected_document_operation_state() uses this so its
// own item_handles/editable_field_handles_by_item (which TurnContract.hpp's
// schema-narrowing consumes) can never diverge from what the prompt actually
// shows, without recomputing eligibility twice.
[[nodiscard]] routing::TurnDataPlane compile_connected_document_turn_data(
	const ConnectedDocumentActionPlan &plan,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const std::optional<research::FinalTextIssue> &issue = std::nullopt,
	research::OperationWorkFrontier frontier = research::OperationWorkFrontier::Drafting,
	std::vector<std::string> *outAdmittedItemHandles = nullptr);
[[nodiscard]] research::OperationStateProjection compile_connected_document_operation_state(
	const ConnectedDocumentActionPlan &plan,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const std::optional<research::FinalTextIssue> &issue = std::nullopt);

// Safe checkpoint projection. It contains typed work state and accepted
// semantic deltas, but never Papinian restoration mappings or plaintext
// diagnostics from a rejected response.
[[nodiscard]] nlohmann::json connected_document_draft_checkpoint_json(
	const ConnectedDocumentActionPlan &plan);
void restore_connected_document_draft_checkpoint(
	const ConnectedDocumentActionPlan &plan, const nlohmann::json &checkpoint);

// Full read-only preflight used by the existing ResearchToolLoop before it
// accepts a model's final proposal. Returns a concise correction reason when
// provenance, retrieved-source coverage, deterministic arithmetic, or
// backend-rendered citation placement is incomplete. It never mutates a file.
[[nodiscard]] std::optional<research::FinalTextIssue> connected_document_action_final_issue(
	const ConnectedDocumentActionPlan &plan, const std::string &structuredModelResult,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence);

// AdvancedWorkflow's change is waiting for a person's decision. The same
// contract ProfessionalArtifactAction already uses (ArtifactActionApprovalRequired):
// the decision is durable on the operation's own checkpoint, never held in an
// in-process wait that a quit or a crash destroys - see MatterRequestExecution.cpp's
// own comment on why ToolBroker's pending map must never be the only record.
class ConnectedDocumentActionApprovalRequired final : public std::runtime_error {
public:
	ConnectedDocumentActionApprovalRequired(tools::ToolApprovalRequest request,
			tools::ToolCallEvidence evidence)
		: std::runtime_error("The document change is waiting for approval."),
		  request(std::move(request)), evidence(std::move(evidence)) {}
	tools::ToolApprovalRequest request;
	tools::ToolCallEvidence evidence;
};

// Resolves an untrusted generic proposal against backend document truth,
// constructs a guarded DocumentPatch and submits it only through ToolBroker.
// `preapproved` means this operation's own durable checkpoint already records
// the person's "approved" decision for exactly this change; the caller proves
// the bound target is unchanged before saying so.
[[nodiscard]] ConnectedDocumentActionResult apply_connected_document_action(
	const ConnectedDocumentActionPlan &plan, const std::string &structuredModelResult,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const tools::ToolExecutionContext &context, bool preapproved = false);

[[nodiscard]] ConnectedDocumentActionResult apply_connected_document_action_draft(
	const ConnectedDocumentActionPlan &plan, const std::string &responseText,
	const std::vector<research::ExternalEvidenceReference> &availableExternalEvidence,
	const tools::ToolExecutionContext &context, bool preapproved = false);

} // namespace singularity::documents
