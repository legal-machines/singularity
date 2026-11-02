#pragma once

// DirectDocumentWork: the simple, source-bound "one substantive model call"
// happy path for a professional document task that does not need iterative
// multi-document research - the product path that live acceptance runs with
// real frontier models (a spouse-consent risk in a design-partner-style task)
// showed the old multi-turn tool-choreography (document.evidence.define -> document.inspect ->
// workingEvidenceNote -> document.outputs.define -> document.draft.upsert)
// is too brittle for: a real, reproducible GPT-5-family failure mode
// (workingEvidenceNote silently dropped from a repeated document.inspect
// call) blocked an otherwise-correct, well-reasoned completion twice.
//
// The professional contract does not change - only the physical execution
// shape does:
//
//   Project documents -> resolve relevant context -> resolve safe edit
//   surface -> Papinian -> ONE structured model call -> deterministic DOCX
//   mutation -> optional review -> saved Work Product
//
// Singularity owns: target document, mutation intent (FILL_TEMPLATE/
// UPDATE_EXISTING/INSERT_NEW), the template/insertion location, and the
// physical DOCX mutation. The model owns only professional content
// (ProfessionalDocumentResult below) - it never sees or chooses internal
// OOXML coordinates or arbitrary existing-row handles.

#include "Document/Types/DocumentPatchTypes.hpp"
#include "Document/Types/DocumentSemantics.hpp"
#include "Matter/Types/Matter.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

enum class EditSurfaceMode { FillTemplate, UpdateExisting, InsertNew };

[[nodiscard]] inline const char *to_string(EditSurfaceMode mode) noexcept {
	switch (mode) {
		case EditSurfaceMode::FillTemplate: return "FILL_TEMPLATE";
		case EditSurfaceMode::UpdateExisting: return "UPDATE_EXISTING";
		case EditSurfaceMode::InsertNew: return "INSERT_NEW";
	}
	return "FILL_TEMPLATE";
}

// One generic "professional record block" discovered structurally in a
// target document: a title/banner row (DocumentCellRole::TitleOrBanner,
// optionally carrying a DocumentCellRole::MetadataOrSeverity cell alongside
// it), immediately followed by one or more DocumentCellRole::HeaderLabel-
// role columns whose real body cells (DocumentCellRole::Body) this struct
// exposes. Detected purely from Office-Structure-Law role evidence - the
// same formal-formatting-only signal every other document action in this
// codebase already relies on - never from column header TEXT content, so
// this works for any document using this common "banner + labeled body"
// block convention, in any language. Proven structurally against the real
// design-partner document this session: a "Риск 1.9"-style empty block is exactly
// one banner row (title "Общество — [Наименование риска]" + severity cell)
// followed by a header row ("Описание риска"/"Рекомендации") and a body row,
// then a second header/body pair ("Дополнительная информация").
struct DocumentBlockSurface {
	// "" when this block was discovered directly against a single known
	// table/document (every existing call site before the real Chat->
	// DirectDocumentWork product routing below) - populated only by
	// resolve_direct_document_work_task_plan(), which discovers blocks
	// across every candidate Project document and must remember which one
	// each block actually came from to build the real document.patch.apply
	// call afterward.
	std::string document_id;
	std::string table_id;
	std::string banner_row_key;
	// The banner row's own STABLE structural position (DocumentTableRow::
	// row_index) - unlike banner_row_key above (content-derived: the row's
	// own first-cell text, or a "row-<n>" fallback - see DocxExtractor.cpp),
	// this never changes when document.patch.apply overwrites that row's own
	// title cell. Real product bug found and fixed this session: professional-
	// object lineage (MatterTask::direct_document_work_banner_row_index)
	// MUST use this field, never banner_row_key, or a follow-up can never
	// find its own prior turn's block again once that block's title has
	// actually been written (the row's content key by definition just
	// changed to the new title). banner_row_key itself keeps its original
	// job unchanged: build_fill_template_operations()'s live, current-content
	// patch addressing, and resolve_edit_surface()'s own explicit-update-
	// intent content lookup.
	std::size_t banner_row_index = 0;
	std::string banner_title_text;    // the banner's own title cell text, verbatim
	std::string banner_metadata_text; // the banner's own severity/metadata cell text, verbatim ("" if none)
	// A block's occupancy is decided from its real body fields alone (see
	// all_fields_are_placeholder() below) - a template's own DEFAULT title/
	// severity text (e.g. the design partner's own real "Средний" sitting in an
	// otherwise-empty block) is presentation scaffolding, never evidence the
	// block itself is already used, and never a constraint on what the
	// model may professionally conclude for it (this session's own real
	// product correction: a template default is not a structural
	// constraint). These are therefore populated whenever the corresponding
	// banner cell exists AT ALL - never gated on the cell's own current text
	// - so a caller may always write real content into an AVAILABLE block's
	// title/severity; the real safety guarantee remains that a caller may
	// only ever propose this for a block whose all_fields_are_placeholder()
	// it already checked, and that document.patch.apply's own expected-old-
	// value match still fails closed against any live document that has
	// changed since.
	std::string banner_title_column_key;    // "" only when this block's banner has no title cell at all
	std::string banner_metadata_column_key; // "" only when this block's banner has no metadata/severity cell at all
	// The metadata/severity cell's own real, current OOXML shading (see
	// DocumentTableCell's own shading_* fields) - "" when the cell declares
	// none. document_native_severity_presentation() reads this, from every
	// discovered block regardless of occupancy, to learn each real severity
	// word's own real color from the document itself.
	std::string banner_metadata_shading_fill_hex;
	std::string banner_metadata_shading_theme_fill;
	std::string banner_metadata_shading_theme_fill_shade;

	struct Field {
		std::string header_label; // the real, visible header text, verbatim - never hardcoded
		std::string row_key;
		std::string column_key;
		std::string current_text;
		bool is_placeholder = false;
	};
	std::vector<Field> fields;

	[[nodiscard]] bool all_fields_are_placeholder() const noexcept {
		if (fields.empty()) return false;
		for (const auto &field : fields)
			if (!field.is_placeholder) return false;
		return true;
	}
	[[nodiscard]] const Field *field_for_header(const std::string &headerLabel) const noexcept {
		for (const auto &field : fields)
			if (field.header_label == headerLabel) return &field;
		return nullptr;
	}
};

// Discovers every "banner + labeled body" block in one table, purely from
// structural role evidence (see DocumentBlockSurface's own comment). A table
// that does not match this shape at all (no banner row, or no HeaderLabel-
// role column) simply contributes zero candidates - this is a real, but not
// universal, document convention.
[[nodiscard]] std::vector<DocumentBlockSurface> discover_document_block_surfaces(
		const DocumentTable &table);

struct EditSurfaceDecision {
	EditSurfaceMode mode = EditSurfaceMode::InsertNew;
	std::optional<DocumentBlockSurface> target;
	// Set only for InsertNew: an existing, real, populated block whose
	// STRUCTURE (never its content) a real implementation would clone to
	// synthesize a new one. std::nullopt when even that has no candidate,
	// which the caller must treat as a hard failure, never a guess.
	std::optional<DocumentBlockSurface> clone_source;
	std::string reason;
};

// Routing precedence (product-specified): a compatible, genuinely empty
// template block always wins over everything else; only once no such block
// exists does an explicit "update this named existing block" intent get to
// select an already-populated block; only once neither applies does
// InsertNew ever run.
//
// Selection is occupancy-only - it is NOT severity-aware, and never re-runs
// once a severity judgment is known: this session's own real product
// correction established that a template's own default severity display
// (e.g. the design partner's own real "Средний" sitting in an otherwise fully-
// placeholder block) is presentation scaffolding, not a structural
// constraint on what the model may professionally conclude. Selecting
// "the area Singularity will let the model populate" and deciding "what that
// area's own severity value/presentation ends up being" are two different
// decisions; searching for a second, differently-labeled template merely
// because the model's own severity differs from the first candidate's
// default is exactly the unnecessary complexity this correction removes -
// document_native_severity_presentation() below is what makes the CHOSEN
// block's own final presentation correct, regardless of which available
// candidate was chosen.
//
// HARD INVARIANT, enforced structurally (never a prompt hope): this
// function can NEVER return UpdateExisting/InsertNew pointing at a
// populated block unless updateIntentBlockKey names that EXACT block's
// banner_row_key. This is the direct, structural fix for the real, live-
// observed failure mode this session (a model - and separately this same
// codebase's old tool-choreography path - targeting an unrelated real
// "bankruptcy" risk instead of an empty template): that failure mode is now
// a structural impossibility here, not a matter of prompt discipline.
[[nodiscard]] EditSurfaceDecision resolve_edit_surface(
		const std::vector<DocumentBlockSurface> &candidates,
		const std::string &updateIntentBlockKey = {});

// A real severity label's own real, document-native presentation (cell
// shading), LEARNED from every block in `candidates` that already carries
// both a recognizable severity word and shading - whether that block is
// itself available or already populated (a populated block's own severity
// styling is just as authoritative a source of "what does this document's
// own Low/Medium/High/Informational look like" as an available one's
// default). Never an invented color: a semantic severity value this
// function has never seen a real shading example for returns std::nullopt,
// and the caller leaves that cell's existing shading untouched rather than
// guess.
struct SeverityPresentation {
	std::string fill_hex;
	std::string theme_fill;
	std::string theme_fill_shade;
};
[[nodiscard]] std::optional<SeverityPresentation> document_native_severity_presentation(
		const std::vector<DocumentBlockSurface> &candidates, const std::string &canonicalSeverity);

// The document's own authoritative example of how it writes this kind of
// record: one real, already-populated block from the SAME document, LEARNED
// from `candidates` exactly as document_native_severity_presentation() above
// learns severity colour - never an invented or generic template. Returns the
// most completely filled populated block (the most informative example), and
// std::nullopt when the document contains no populated block at all, so a
// document with nothing to imitate yet is never given a fabricated house
// style.
//
// Why this exists: candidate_context previously showed the model only the
// free template's FIELD LABELS while simultaneously instructing it to "write
// it in the language and professional register of the record's own field
// labels and existing entries" - it was told to match existing entries it was
// never actually shown. A real design-partner run reproduced the firm's own
// register only because the lawyer's own prompt said "follow the examples
// file". `excludeBannerRowKey` omits a block already rendered in full
// elsewhere (the Chat's own previous record), so the same text is never sent
// twice.
[[nodiscard]] std::optional<DocumentBlockSurface> document_native_record_exemplar(
		const std::vector<DocumentBlockSurface> &candidates,
		const std::string &excludeBannerRowKey = {});

// The provider-neutral, professional-decisions-only output contract. No
// orchestration ceremony (no workingEvidenceNote/skippedRegions/frontier/
// final flags, no internal handles) - this file's own top comment explains
// why: Singularity owns targeting and mutation mechanics, the model owns only
// this content.
[[nodiscard]] providers::GenerationOutputContract professional_document_result_output_contract();
// The raw schema fragment behind professional_document_result_output_contract()
// above, exposed so ProfessionalArtifactAction.cpp's own unified professional-
// response contract can embed it as a nested "structuredRecord" object
// verbatim - one schema, reused, never a second hand-copied definition that
// could silently drift from this one.
[[nodiscard]] nlohmann::json professional_document_result_schema();

struct ProfessionalDocumentResult {
	std::string title;
	std::string severity; // one of "high"/"medium"/"low"/"informational"
	std::vector<std::string> description;
	std::vector<std::string> recommendations;
	std::optional<std::string> additional_information;
	std::vector<std::string> source_support;
	// Follow-up continuity (Beta ship-path requirement): true only when the
	// model itself judges this request to be a modification of the SAME
	// professional object it (or a prior turn in this same Chat) already
	// produced - never decided by Singularity re-parsing text or guessing from
	// keywords (CORE PRODUCT LAW: the model owns professional judgment, Singularity
	// owns state). Defaults false - the SAFE direction on any doubt,
	// since false only ever risks creating a new entry in a free slot,
	// never overwriting real existing content: resolve_direct_document_work_
	// task_plan() only ever offers a real update TARGET when this exact
	// Chat's own most recent DirectDocumentWork task is still resolvable, and
	// build_fill_template_operations() writes into that real target either
	// way - true merely selects WHICH real target this call already resolved
	// Singularity-side, never an arbitrary one the model names.
	bool updates_previous_object = false;
};

[[nodiscard]] std::optional<ProfessionalDocumentResult> parse_professional_document_result(
		const std::string &modelResponseText);

// Singularity's own deterministic Chat-facing summary of an accepted
// ProfessionalDocumentResult - the model produces the professional content
// exactly once (title/severity/description/recommendations/
// additionalInformation/sourceSupport); it is never asked to also write a
// second free-form restatement of the same analysis for chat display (that
// used to be this contract's own "answer" field, removed because it
// duplicated the structured fields' content and could independently exceed
// its own length ceiling - see this session's own real 2003-vs-2000-character
// acceptance failure). Presentation is a Chat/UI concern Singularity owns
// entirely, generic across every DirectDocumentWork-supported professional
// document object - no document/task-specific wording. `usedUpdateTarget`
// must be the ACTUAL selection outcome (SelectedDirectDocumentWorkTarget::
// used_update_target), never the model's own updatesPreviousObject claim in
// isolation - see that struct's own comment for why.
// `severityLabel` is the register's own word for this severity (see
// document_native_severity_label()); empty keeps the schema's code.
[[nodiscard]] std::string describe_direct_document_work_result(
		const ProfessionalDocumentResult &content, bool usedUpdateTarget, const std::string &severityLabel = std::string());

// The complete professional record as a Markdown memo for a retained Work
// Product: title, severity, every description paragraph, the numbered
// recommendations and any additional information. Section labels follow the
// record's own language (Russian when the title is Cyrillic). Source-support
// handles are never included - the Work Product carries its source
// references separately, in professional form.
[[nodiscard]] std::string direct_document_work_memo(
		const ProfessionalDocumentResult &content, const std::string &severityLabel = std::string());

// Builds the exact document.patch.apply-shaped operations array (see
// DocumentPatchTool.cpp's own operation_from_json()) that fill `target`'s
// own real fields from `content` - never inventing a locator, never writing
// to a field this exact target does not itself expose. Field mapping is
// header-label-driven (a small, documented EN/RU keyword table in the .cpp,
// falling back to position for a plain two-column description/
// recommendation shape) - not hardcoded to any one document.
//
// Writes description, recommendations, and (when present) additional
// information into their own real body fields. ALSO writes title and
// severity into the banner's own title/metadata cells whenever `target`
// exposes them (banner_title_column_key/banner_metadata_column_key non-
// empty) - a template default value there is real, current text, but is
// replaceable scaffolding, never an existing professional conclusion (see
// DocumentBlockSurface's own comment); the hard safety guarantee is that
// `target` itself was only ever proposed by resolve_edit_surface() after
// confirming this whole block's real body fields are ALL placeholders.
// `severityPresentation`, when supplied, additionally requests a real
// document-native shading change on the severity cell alongside its text -
// see document_native_severity_presentation()'s own comment for where a
// real one comes from; omitted (std::nullopt) leaves the cell's existing
// shading untouched.
//
// Returns std::nullopt only when `target` cannot host `content` at all (a
// genuine incompatibility - e.g. no description-shaped field exists),
// never silently dropping a field content actually supplied.
// `registerBlocks` are the register's own discovered blocks (the plan's
// candidates): the severity word written into the target is the one this
// register itself already uses, so an English register never receives another
// language's wording.
[[nodiscard]] std::optional<nlohmann::json> build_fill_template_operations(
		const std::string &documentId, const DocumentBlockSurface &target,
		const ProfessionalDocumentResult &content, const std::string &rationaleId,
		const std::optional<SeverityPresentation> &severityPresentation = std::nullopt,
		const std::vector<DocumentBlockSurface> &registerBlocks = {});

// The document's own word for a canonical severity (see the .cpp).
[[nodiscard]] std::string document_native_severity_label(const std::vector<DocumentBlockSurface> &candidates,
		const std::string &currentMetadataText, const std::string &canonicalSeverity);

// ---------------------------------------------------------------------
// Real Chat -> DirectDocumentWork product routing (the "Arrow #1" wiring:
// this is what lets a real Chat message reach this whole file through the
// NORMAL product path - MatterRequestExecution.cpp's prepare_task_request()
// - rather than only through the phase24_1d acceptance harness). Routing is
// product-semantic, never hardcoded to any one document/filename/prompt.
// ---------------------------------------------------------------------

// Everything one real task attempt needs to run DirectDocumentWork's one
// substantive model call, resolved once, up front, from the real Project -
// mirrors ConnectedDocumentActionPlan's own role as prepare_task_request()'s
// other real routing plan.
struct DirectDocumentWorkTaskPlan {
	// The genuinely free FILL_TEMPLATE surface, when one exists - std::nullopt
	// once every real template in every candidate document is already
	// populated (a real, expected steady state for an active Chat/Project).
	std::optional<std::string> new_work_document_id;
	std::optional<DocumentBlockSurface> new_work_target;
	// The SAME real block this exact Chat's own most recent DirectDocumentWork
	// task (see MatterTask::direct_document_work_table_id/banner_row_key)
	// still resolves to, live-rediscovered from the real current document
	// (never a stale cached copy) - std::nullopt when this Chat has no prior
	// DirectDocumentWork task, or that task's own block can no longer be
	// found (document changed externally, etc.). This is the ONLY target
	// UPDATE_EXISTING may ever select - never an arbitrary populated block a
	// model merely names.
	std::optional<std::string> update_document_id;
	std::optional<DocumentBlockSurface> update_target;
	// Every block discovered across every candidate Project document, not
	// just the chosen target(s) - document_native_severity_presentation()
	// needs the full set (an available OR populated block can be the one real
	// source of a given severity's document-native color), exactly as the
	// real live acceptance evidence for this file already established.
	std::vector<DocumentBlockSurface> candidates;
	// Whether "no free template" really means none exists anywhere (every
	// Project document was in scope), rather than only none inside an explicit
	// selection. It words the refusal; a record the model declares as new is
	// never written over the previous record either way.
	bool update_fallback_allowed = false;
	// The structured-record candidate exactly as the model sees it: the free
	// template's own field labels when one exists and this Chat's prior record
	// when there is one - structure only. It never carries source material:
	// the request's canonical SOURCE_MATERIAL (the user's source selection,
	// capacity-first, with source markers) is the evidence for this candidate
	// exactly as for every other capability, and the candidate's existence
	// never replaces or redirects that request.
	std::string candidate_context;
};

// Resolves whether one real task attempt should run through DirectDocumentWork
// at all, and if so, the complete plan for it - std::nullopt (never a thrown
// error) for every case where it does not apply, exactly like
// prepare_connected_document_action()'s own "return nullopt when not
// applicable" contract, which this is a sibling of.
//
// `chatId` and `priorTasksInChat` (already filtered to this exact Chat,
// caller's own responsibility - see prepare_task_request()'s own existing
// per-Chat history scoping) let this function discover the most recent prior
// DirectDocumentWork task in THIS Chat, if any, for follow-up continuity -
// see DirectDocumentWorkTaskPlan::update_target's own comment. Omit
// (defaulted) for a caller with no Chat boundary, matching every existing
// caller's unchanged behavior.
//
// SAFE-BY-CONSTRUCTION ROUTING (Beta product decision, Phase 1-28 final
// integration): a plan is only ever returned when EITHER a genuinely empty,
// non-destructive FILL_TEMPLATE surface exists OR this exact Chat's own
// resolvable prior professional object exists (or both) - INSERT_NEW, and
// UPDATE_EXISTING against any block other than this exact Chat's own prior
// object, stay the old connected-document-action path's job (an ambiguous or
// destructive edit target must never auto-route into a one-shot mutation
// with no in-loop model review). Also never returns a plan when the
// instruction explicitly requires external research
// (research::resolve_research_mode(instruction) == Required) -
// DirectDocumentWork is source-bound only. Every condition here is a
// structural fact about the real Project/Chat/instruction, never a guess at
// the instruction's own semantic meaning (CORE PRODUCT LAW: Singularity owns
// structure/state, the model owns professional judgment) - WHICH of
// new_work_target/update_target a given call actually uses is the model's
// own judgment (ProfessionalDocumentResult::updates_previous_object),
// resolved deterministically afterward, never guessed here. A request this
// function declines simply falls through to the old path (or plain
// conversation), exactly as today, never a fabricated DirectDocumentWork
// result.
// With an explicit source selection, structured-record candidates come only
// from the selected documents, the request's explicit target and the document
// this Chat's own most recent structured record lives in (continuity): a
// template in any other Project document never enters the request. Without a
// selection (Relevant), every Project DOCX is a candidate.
[[nodiscard]] std::optional<DirectDocumentWorkTaskPlan> resolve_direct_document_work_task_plan(
		const std::string &instruction, const matters::Matter &project,
		const std::optional<std::string> &chatId = std::nullopt,
		const std::vector<matters::MatterTask> &priorTasksInChat = {},
		const std::vector<std::string> &selectedDocumentIds = {},
		const std::optional<std::string> &targetDocumentId = std::nullopt);

// The one, real selection between a DirectDocumentWorkTaskPlan's two
// candidate targets - extracted so MatterRequestExecution.cpp's real
// production call site and any deterministic/offline acceptance harness
// (which cannot run a real model to produce a ProfessionalDocumentResult)
// share this exact decision, never a second reimplementation of it.
// updatesPreviousObject is the model's own judgment; when only one of the
// two targets actually exists this turn, that one is used regardless of the
// flag (nothing else to write to). Returns std::nullopt only when the plan
// itself has neither target (an invariant resolve_direct_document_work_
// task_plan() already guarantees never happens for a plan it actually
// returned).
struct SelectedDirectDocumentWorkTarget {
	std::string document_id;
	DocumentBlockSurface block;
	// True when this selection resolved to plan.update_target (the SAME real
	// block a prior task in this exact Chat already produced) rather than a
	// freshly-chosen free template - the ACTUAL operation semantics, for
	// callers (e.g. describe_direct_document_work_result()) that must report
	// what really happened rather than repeat the model's own
	// updatesPreviousObject claim, which alone never decides this when only
	// one target exists (see select_direct_document_work_target()'s own
	// comment).
	bool used_update_target = false;
};
[[nodiscard]] std::optional<SelectedDirectDocumentWorkTarget> select_direct_document_work_target(
		const DirectDocumentWorkTaskPlan &plan, bool updatesPreviousObject);

} // namespace singularity::documents
