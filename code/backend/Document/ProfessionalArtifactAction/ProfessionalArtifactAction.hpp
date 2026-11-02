#pragma once

#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "Document/Types/DocumentChangeDescription.hpp"
#include "Document/Types/DocumentPatchTypes.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentVersionStore/DocumentVersionStore.hpp"
#include "Document/XlsxRowInsertion/XlsxRowInsertion.hpp"
#include "Document/Types/ProfessionalReview.hpp"
#include "Matter/Types/Matter.hpp"
#include "Professional/StructuredResults/StructuredResults.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace singularity::documents {

// NoAction means "just answer, no side effect" - always a legal choice.
// StructuredRecordFillTemplate/StructuredRecordUpdateExisting are the two
// DirectDocumentWork operations, expressed here so the SAME single
// generation that can propose a generic artifact action can also propose
// structured-record work; MatterRequestExecution.cpp routes those two kinds
// into DirectDocumentWork's own existing, unchanged apply pipeline (see
// professional_response_output_contract()'s own comment below for why they
// share one schema instead of requiring a second model call to pick between
// them). AdvancedWorkflow is the model's own declaration that this request
// genuinely needs ConnectedDocumentAction's richer, multi-turn, evidence/
// methodology-aware choreography (multi-document evidence agenda, coverage
// tracking, table-map compaction, iterative professional drafting,
// independent review) - never chosen by Singularity pre-generation, and only
// ever activated when a real structural candidate for it was resolved (see
// MatterRequestExecution.cpp's own wire_connected_document_action_request()).
enum class ArtifactActionKind {
	NoAction, CreateContent, AppendContent, ReplaceContent, UpdateCells,
	StructuredRecordFillTemplate, StructuredRecordUpdateExisting, AdvancedWorkflow,
	// Work ON a document the Project already has, said the way a lawyer says
	// it: this passage becomes that, this paragraph goes, this row joins the
	// table, this warning is written so it can be seen. One action carries
	// however many of those a single professional instruction needs, and Singularity
	// applies them as one change or not at all (DocumentPatch).
	EditDocument,
};

struct ArtifactCellUpdate {
	std::string sheet;
	std::string cell_ref;
	std::string expected_old_value;
	std::string new_value;
};

// A real Project cell/region the model looked at and explicitly chose NOT to
// write, with its own stated reason - ported from ConnectedDocumentAction's
// skippedRegions/skip-semantics property (a live gpt-4o-mini acceptance run
// proved a model forced to act on every visible candidate with no way to
// decline one eventually invents work nobody asked for). Persisted into
// provenance (AppliedDocumentActionEvidence), never into the document.
struct ArtifactSkippedCandidate {
	std::string location;
	std::string reason;
};

struct ProfessionalArtifactProposal {
	std::string title;
	std::string summary;
	ArtifactActionKind action = ArtifactActionKind::CreateContent;
	// CreateContent only: the model's own declared format (a professional
	// judgment - a memorandum is naturally docx, an extraction naturally csv -
	// never guessed by Singularity from the user's wording). Validated against
	// ArtifactCapabilityRegistry's real create capability before use.
	DocumentFormat format = DocumentFormat::unknown;
	std::string content;
	std::string match_text;
	std::string replacement_text;
	std::vector<std::string> columns;
	std::vector<std::vector<std::string>> rows;
	// UpdateCells in a workbook: a newValue that begins with "=" is a formula,
	// written as the formula it is (xlsx_formula_stored).
	std::vector<ArtifactCellUpdate> cells;
	// UpdateCells in a workbook: whole rows inserted before any cell is written,
	// each numbered as the sheet stands once they are all in - the numbering
	// every cell and formula of the change is written in. A value the model gave
	// a new row is one of `cells`, expected empty.
	std::vector<XlsxRowInsertion> inserted_rows;
	// UpdateCells in a workbook: whole rows taken out the way Excel's Delete Row
	// takes them, each numbered as the sheet stands now, before anything of the
	// change is made - every other row and cell of the change is named as the
	// sheet stands once the change is made. Always shown to the lawyer, with
	// every formula that would no longer find what it reads.
	std::vector<XlsxRowDeletion> deleted_rows;
	// UpdateCells in a workbook: new, empty sheets after the last tab, in this
	// order. What they hold is written as cells on them.
	std::vector<std::string> added_sheets;
	std::vector<ArtifactSkippedCandidate> skipped_candidates;
	// EditDocument only: what the model asks to be done to the bound document.
	// The same contract Singularity validates and writes with everywhere else
	// (Document/Types/DocumentPatchTypes.hpp) - a model never sees a second
	// vocabulary for the same act.
	std::vector<DocumentPatchOperation> operations;
	// The supplied-source handle ("DOC_3") of the existing artifact this
	// proposal is against, when the model itself names it - see
	// ArtifactTargetHandle. Empty for create_content and whenever the user
	// bound the target instead.
	std::string target_handle;
	// create_content only: what the new document IS, declared by the model so
	// Singularity can name the file itself (artifact_file_name below) instead of
	// copying a heading into the filesystem. document_kind is a stable machine
	// value ("amendment", "memorandum", ... or "other"); subject is what the
	// document is about in a few words, in the document's own language;
	// based_on_handle is the supplied handle ("DOC_2") of the Project document
	// it amends, summarises or answers; language is the language the document
	// is written in ("en", "ru"). Empty when the model gave none.
	std::string document_kind;
	std::string subject;
	std::string based_on_handle;
	std::string language;
	// The lawyer asked to see this change or new document before the file is
	// touched ("show me before changing it"). The model declares it from the
	// request; Singularity then asks for approval for this one action even under
	// Full access. It never grants anything: Ask first asks regardless.
	bool ask_first = false;
	// The lawyer's own words that ask for this change, quoted by the model
	// from their message. Where they are missing, or are not words the lawyer
	// wrote in this request, the change waits for the lawyer whatever the
	// Project's action mode: a document is changed without a card only when
	// the lawyer can be shown to have asked for it.
	std::string requested_by;
	// Ported from ConnectedDocumentAction's EvidenceObjective sufficiency
	// declaration, scoped down to this bounded single-shot protocol: THE
	// MODEL OWNS the judgment of whether the supplied sources were enough to
	// produce this content. Meaningful only when the plan actually has
	// sources (has_sources below) - see professional_artifact_review_
	// instruction(), which is the independent check on this self-declaration.
	bool evidence_sufficient = true;
	std::vector<std::string> evidence_gaps;
	// What Singularity worked out in this proposal (title, summary, content,
	// replacementText, table cells, each operation's words), by the field it
	// stands in and the text it became, as the model wrote it - statements and
	// their ⟦operands⟧. Two fields that came to the same text keep each its own
	// statement. The review and a later correction read the proposal as
	// written, so a figure worked out from protected values never leaves this
	// computer; only the document and the lawyer receive the result. Empty
	// where nothing was worked out.
	std::map<std::string, std::string> as_written;
	// Every statement in it Singularity could not work out, and why: a proposal
	// that still has one is never written, since the figure it needs is not
	// there.
	std::vector<context::computed_statements::StatementFailure> statement_failures;
};

// Structural (never semantic) facts Singularity resolves BEFORE generation about
// what a generic create/update action could possibly target - see
// resolve_professional_artifact_candidates(). Creating a new artifact is
// always structurally available whenever the Project has a folder; updating
// an existing one is available ONLY when the request carries a separate,
// explicit target identity whose format supports update. Source selection
// never supplies that identity (and "the only writable file happens to
// exist" never does either).
// Which of these, if either, the request actually calls for is the MODEL's
// own judgment from the SAME generation this struct's own model_instruction
// is folded into - never decided here.
// One existing artifact the MODEL may name as its write target, under the
// handle this operation already supplied it under ("DOC_3" - the same handle
// vocabulary a citation is written in).
//
// A write target is still never inferred. It is bound by the user, or named
// by the model - and a named one is validated exactly the way a citation is:
// against the handles this operation actually issued. A handle Singularity did
// not supply is not a target, just as a fabricated marker is not a source
// (Master Spec Sec.22). Read access still selects nothing by itself: the
// model must say which document the request asks to change, and the user's
// action mode still decides whether that change asks before it is written.
struct ArtifactTargetHandle {
	std::string handle;
	std::string document_id;
	DocumentFormat format = DocumentFormat::unknown;
};

struct ProfessionalArtifactCandidates {
	bool can_create = false;
	std::optional<std::string> existing_target_document_id;
	DocumentFormat existing_target_format = DocumentFormat::unknown;
	// LM-authored capability text only - never Project content, so it can be
	// protected as instruction text (see ProtectedInference's backend-authored
	// sections).
	std::string model_instruction;
	// The care a careful drafter takes beyond the rules above - an enforceable
	// form, the legal risk said in the answer. LM-authored like the rest, but
	// offered only where the model's window has room for it: the first text a
	// tight window leaves out (MatterRequestExecution), never a reason to refuse.
	std::string drafting_guidance;
	// The artifactAction shape in words - every key its schema requires, read
	// from that schema. A turn that carries the strict response schema already
	// shows the model the shape; a turn that offers research tools cannot carry
	// it (the transport never combines tools with a strict response format), so
	// only such a turn is given this sentence. Empty when no artifact action is
	// available.
	std::string artifact_shape_instruction;
	// Project content describing the candidates (the bound target's bounded
	// structural preview) - rendered in the prompt's CANDIDATE_CONTEXT data
	// section, which receives full semantic protection.
	std::string candidate_context;
	// See ProfessionalArtifactActionPlan::requires_professional_review's own
	// comment - true whenever real Project source evidence was supplied.
	bool has_sources = false;
	std::size_t source_count = 0;
	// The existing artifacts the model may name (offer_nameable_targets()).
	// Empty until this operation's supplied set is known.
	std::vector<ArtifactTargetHandle> nameable_targets;
};

// Offers `targets` as nameable write targets and returns the capability
// sentence that says so - LM-authored instruction text (handles and formats
// only, never Project content). Called once this operation's supplied set is
// known; returns an empty string when there is nothing to offer or the user
// already bound a target.
[[nodiscard]] std::string offer_nameable_targets(ProfessionalArtifactCandidates &candidates,
	std::vector<ArtifactTargetHandle> targets);

// The handle of the one nameable Project document `answer` cites, when every
// source marker in it cites that one document; empty otherwise. An amendment
// whose answer cites only the agreement has said which agreement it amends,
// though its basedOn was left empty. This names a new file; it grants nothing.
[[nodiscard]] std::string sole_cited_nameable_handle(const ProfessionalArtifactCandidates &candidates,
	const std::string &answer);

// `available_context_window_tokens` is the resolved model's own capacity, and
// decides how much of the bound document is put in front of it. A document
// that fits is shown in full and said to be in full; one that does not is
// shown as far as it goes and says so. With no known capacity, the small
// fixed reading Singularity used before capacity was consulted at all.
[[nodiscard]] ProfessionalArtifactCandidates resolve_professional_artifact_candidates(
	const std::vector<std::string> &selected_document_ids,
	const std::optional<std::string> &explicit_target_document_id,
	const matters::Matter &project,
	std::optional<std::size_t> available_context_window_tokens = std::nullopt);

struct ProfessionalArtifactActionPlan {
	bool creates_new_artifact = false;
	// True when the target was named by the model rather than bound by the
	// user - recorded, so the Project's own record of the change always says
	// who chose the document it was written into.
	bool target_named_by_model = false;
	DocumentFormat format = DocumentFormat::unknown;
	std::string relative_path;
	std::optional<std::string> target_document_id;
	std::optional<int> expected_version;
	std::optional<std::string> expected_fingerprint;
	ArtifactActionKind required_action = ArtifactActionKind::CreateContent;
	// True whenever this action draws on real Project source evidence (see
	// ProfessionalArtifactCandidates::has_sources above) - the exact condition
	// that justifies running the independent Professional Review gate below.
	// A trivial localized edit with no cited sources (e.g. "shorten this
	// paragraph") never sets this and never pays for a review turn - see
	// Section 8/48 ("simple localized updates must remain simple").
	bool requires_professional_review = false;
	std::size_t source_count = 0;
	// The writings that name one and the same party, as the Project knows them
	// (Tool/Types/ToolTypes.hpp's own project_identity_writings). A change a
	// model anchors on one writing resolves against the document's own writing.
	std::vector<std::vector<std::string>> identity_writings;
};

// Builds a concrete, target-resolved plan from what the MODEL actually
// proposed (proposal.action/format) against what was structurally resolved
// beforehand (candidates) - fails closed (throws, never guesses) if the
// model's declared action has no matching structural candidate, e.g. it
// proposed update_cells with no existing_target_document_id resolved. This
// is where "the model proposes, Singularity validates" becomes concrete: the
// model's OWN choice among the candidates it was told about is authoritative
// for WHAT to do; Singularity is authoritative for whether that choice is even
// possible given real, current Project state.
// The file name Singularity gives a new document: what it is, what it is written
// against and what it is about, in the document's own language - "Amendment to
// Northwind Master Services Agreement – Contractual Penalty.docx". Deterministic,
// short (the stem is at most kMaxArtifactFileNameCharacters characters, cut at a
// word, never inside a character), filesystem-safe on every platform, and free
// of repeated words. A kind Singularity has no vocabulary for, or a language it has
// none in, is named from the subject, and failing that from `fallback_title`.
inline constexpr std::size_t kMaxArtifactFileNameCharacters = 80;
[[nodiscard]] std::string artifact_file_name(const std::string &document_kind, const std::string &based_on_name,
	const std::string &subject, const std::string &language, DocumentFormat format,
	const std::string &fallback_title);

[[nodiscard]] ProfessionalArtifactActionPlan build_professional_artifact_plan(
	const ProfessionalArtifactCandidates &candidates, const ProfessionalArtifactProposal &proposal,
	const matters::Matter &project);

// The name a new document was approved under, as the path it is written to:
// the approved document is created under exactly the name the lawyer saw,
// whatever Singularity's naming became since. A plain file name of the plan's own
// format only, never renamed: a name taken since the approval is refused when
// the document is written (a conflict to prepare again). Empty when the
// recorded name cannot be one.
[[nodiscard]] std::string approved_new_document_path(const matters::Matter &project,
	const std::string &approved_name, DocumentFormat format);

// One correction, then the lawyer: a draft the review still questions after one
// repair goes to them as a proposal with the review's concern beside it, never
// written on its own. Two rounds cost a live amendment nine minutes and $0.36
// of GPT-5 calls re-litigating a term only the lawyer could decide (24 Sep 2026);
// the owner's rule is one bounded repair.
inline constexpr std::size_t kMaxProfessionalArtifactReviewCycles = 1;

// The change a proposal makes, as a person reads one before approving it and
// again after it was written: what kind ("create", "append", "replace",
// "cells", "edit"), the text that goes, the text that comes - and, when rows go
// into a table, the columns they fit and the rows themselves, so the change can
// be shown the way the document now shows it. An "edit" also lists every one of
// its steps (DocumentChangeDescription.hpp): a deletion, a cell, a row that goes
// is shown as itself, never folded into "the text that comes".
struct ArtifactChange {
	std::string kind;
	std::string removed_text;
	std::string added_text;
	std::vector<std::string> columns;
	std::vector<std::vector<std::string>> rows;
	std::vector<DocumentChangeStep> operations;
};

[[nodiscard]] ArtifactChange describe_artifact_change(const ProfessionalArtifactProposal &proposal);
// The same change as the lawyer reads it against the document as it stands:
// each step narrowed to the words that differ and placed in its paragraph,
// section and page.
[[nodiscard]] ArtifactChange describe_artifact_change_in(const ProfessionalArtifactProposal &proposal,
	DocumentFormat format, const std::filesystem::path &target);
[[nodiscard]] nlohmann::json artifact_change_json(const ArtifactChange &change);
[[nodiscard]] ArtifactChange artifact_change_from_json(const nlohmann::json &value);

struct ProfessionalArtifactActionResult {
	std::string response_text;
	std::string document_id;
	std::string title;
	DocumentFormat format = DocumentFormat::unknown;
	int resulting_version = 0;
	tools::ToolCallEvidence tool_call_evidence;
	// The change as written, with the columns of the table it went into.
	ArtifactChange change;
};

class ArtifactActionApprovalRequired final : public std::runtime_error {
public:
	ArtifactActionApprovalRequired(tools::ToolApprovalRequest request,
			tools::ToolCallEvidence evidence)
		: std::runtime_error("The artifact action is waiting for approval."),
		  request(std::move(request)), evidence(std::move(evidence)) {}
	tools::ToolApprovalRequest request;
	tools::ToolCallEvidence evidence;
};

// The tool refused the change - its own validator found it could not be made,
// or its execution failed - in the words the lawyer reads, with the refusal
// as tool evidence so the operation's record says what refused it and why.
class ArtifactActionRefused final : public std::runtime_error {
public:
	ArtifactActionRefused(const std::string &message, tools::ToolCallEvidence evidence)
		: std::runtime_error(message), evidence(std::move(evidence)) {}
	tools::ToolCallEvidence evidence;
};

// The unified, provider-independent professional-response contract: every
// substantive Chat generation (everything except the deterministic
// ProductState echo) uses this ONE schema, always. `response` is always the
// user-facing answer; `actionType` is the model's OWN declared judgment of
// whether any side effect is warranted at all, chosen from exactly the kinds
// `candidates` structurally supports (communicated in prose via
// model_instruction, not by shrinking the JSON schema itself - the schema
// stays fixed/static regardless of which candidates exist, so Singularity's
// post-generation validation, never the grammar, is what enforces "only a
// real candidate may be chosen"). `artifactAction` carries the generic
// create/append/replace/update_cells fields (only meaningful for those
// actionTypes); `structuredRecord` reuses DirectDocumentWork's own existing
// ProfessionalDocumentResult schema verbatim (see DirectDocumentWork.hpp's
// own professional_document_result_schema(), embedded here rather than
// duplicated) so a StructuredRecordFillTemplate/UpdateExisting choice can be
// hand ed straight to DirectDocumentWork's unchanged apply pipeline. One
// call, one schema, one place the model expresses what it wants to do.
// `structuredResults` is present in the contract only for the typed results
// the resolved objective's own legal task actually produces - see
// Professional/StructuredResults/StructuredResults.hpp. A request that
// produces none never carries the field at all.
// `structuredRecordWorkAvailable` says whether a DirectDocumentWork candidate
// actually resolved for this request. When it did not, the two structured-
// record action kinds are withdrawn from the contract entirely: with no target
// document, table or block, choosing one can only fail, and a withdrawn choice
// is a schema fact the model cannot argue with.
// What this one request can actually do to the Project. The contract offers
// nothing outside it: a choice the request cannot execute can only fail, and
// every branch of a strict schema is paid for on every request - by a local
// model decoding against its grammar, and by a provider compiling it (two of
// them refuse a grammar this contract's full size). The defaults are the whole
// contract, for a caller that has not resolved what the request can do.
struct ProfessionalResponseScope {
	// The Project has a folder a new document can be written into.
	bool can_create = true;
	// A document is bound as this request's update target.
	bool has_update_target = true;
	// ConnectedDocumentAction resolved a candidate for this request.
	bool advanced_workflow_available = true;
	// The operation can read more of the Project for the model while it works
	// (Context/ProjectEvidence): the contract then carries the model's own
	// request for it. Off by default - offered only where it is answered.
	bool project_evidence_available = false;
	// The operation can pause for the lawyer's answer to a question and continue
	// with it (Tool/LawyerQuestion): the contract then carries the model's own
	// question. Off by default - offered only where a lawyer is there to answer.
	bool lawyer_question_available = false;
	// The model reads protected values as symbols and may ask Singularity facts
	// about them (Context/ComputedStatements' answer_local_facts): the contract
	// then carries its localFacts questions. Off by default - offered only
	// where protection is on and the model is not on this computer.
	bool local_facts_available = false;
	// With local_facts_available: the model's operations are the protected
	// computation protocol's (DESIGN4 §1.2 carrier F) - localFacts is then
	// {"ops": [...]} (ComputeProtocol's ops schema), never questions.
	bool compute_protocol_available = false;
};

[[nodiscard]] providers::GenerationOutputContract professional_response_output_contract(
	professional::StructuredResultKinds structuredResults = professional::StructuredResultKind::None,
	bool structuredRecordWorkAvailable = true,
	ProfessionalResponseScope scope = {});

// Replaces every internal source handle in the text this proposal would write
// (content, each cell's newValue, table cells) with a lawyer-facing label from
// current Project metadata: the document's current name (its Project-relative
// path when two supplied documents share a name), page/heading when known,
// "(no longer available)" for a source that is gone - escaped for the
// artifact's format and never an absolute path. Words that become the text of
// a legal instrument or of an edit - a new Word document that is not written
// as analysis (memorandum, summary, comparison, issues list), replacementText
// and every edit operation's words - carry no citation at all: their handles
// are taken out, never named. So do the cells of a table (a workbook's cells,
// a new table's rows), except a cell that is nothing but its sources or stands
// in a column whose header names sources, and a table inside a document
// written as analysis. A Word or Excel file never receives the
// "[unverified source reference]" note. Returns the references it resolved;
// they keep the internal tokens as provenance.
[[nodiscard]] std::vector<context::ResolvedSourceReference> render_artifact_source_labels(
	ProfessionalArtifactProposal &proposal, DocumentFormat format,
	const std::vector<context::ResolvedSourceReference> &basis, const matters::Matter &project);
// The same rendering for any other professional text committed as plain text
// (e.g. a DirectDocumentWork structured record's fields written into its
// target document's cells): every valid handle becomes its plain current
// document label; returns the resolved references as provenance.
// A source cited after a value, set apart in parentheses; exposed for tests.
[[nodiscard]] std::string parenthesize_trailing_citations(const std::string &text);
[[nodiscard]] std::vector<context::ResolvedSourceReference> render_plain_source_labels(
	const std::vector<std::string *> &texts, const std::vector<context::ResolvedSourceReference> &basis,
	const matters::Matter &project);

struct ProfessionalResponseProposal {
	std::string response;
	// What this conversation is about, in a few words - offered by the model so
	// a chat is named for its subject rather than for the first line typed into
	// it. Absent when the model gave none.
	std::optional<std::string> chat_title;
	ArtifactActionKind action_type = ArtifactActionKind::NoAction;
	std::optional<ProfessionalArtifactProposal> artifact_action;
	// The lawyer's own words asking for a structured record or an advanced
	// workflow, quoted by the model at the top of its answer (the document
	// actions carry theirs in artifactAction.requestedBy). Empty when it quoted
	// none. The approval floor checks it against what the lawyer wrote.
	std::string requested_by;
	// Raw JSON text of the nested "structuredRecord" object, present only for
	// the two StructuredRecord* action types - handed verbatim to
	// documents::parse_professional_document_result() by the caller
	// (MatterRequestExecution.cpp), which already owns that dependency.
	std::optional<std::string> structured_record_json;
	// Raw JSON text of the nested "structuredResults" object (findings and
	// obligations), present only when the model actually produced any. Typed
	// professional results are independent of actionType: an answer that
	// proposes no side effect at all still carries what it found. Parsed by
	// the caller (MatterRequestExecution.cpp), which already owns the
	// source-marker registry every citation here is validated against.
	std::optional<std::string> structured_results_json;
	// Raw JSON text of the universal "workProduct" document
	// (WorkProduct/WorkProductDocument), when the model returned one.
	std::optional<std::string> work_product_json;
	// The model's own typed declaration that the objective needs external
	// evidence the supplied Project material cannot provide: web.search's own
	// argument object as raw JSON, absent when no research is requested.
	std::optional<std::string> external_research_request_json;
	// The model's own request to read more of the Project before it answers -
	// {"search": ..., "read": [...], "list": ...} as raw JSON, absent when the
	// supplied material is enough (Context/ProjectEvidence answers it).
	std::optional<std::string> project_evidence_request_json;
	// The model's own question to the lawyer - {"title": ..., "questions": [...]}
	// as raw JSON, absent when it has none (Tool/LawyerQuestion puts it).
	std::optional<std::string> lawyer_question_json;
	// The model's own questions about protected values - {"questions": [...]},
	// each a statement for Singularity to work out - as raw JSON, absent when it
	// asks none (ResearchToolLoop answers them).
	std::optional<std::string> local_facts_json;
};

// The questions a reply's localFacts asks, each read as a typed question
// (Context/ComputedStatements' FactQuestion) - a professional_response's, or
// the facts step's own {"localFacts": {"questions": [...]}}. A question the
// model wrote as a plain statement, as it once did, is op "expression"; what
// is no question is dropped. Empty when it asks none.
[[nodiscard]] std::vector<context::computed_statements::FactQuestion> professional_response_local_facts(
	const std::string &model_response);

// The localFacts object a contract carries: {"questions": [...]}, each question
// {"op", "operands", "unit", "expression"} - strict, every property required,
// the ones that do not apply null.
[[nodiscard]] nlohmann::json local_facts_schema();

// The facts step's own small contract: {"localFacts": {"questions": [...]}},
// required, an empty list allowed - what a model answers first, before its
// answer, when the values it reads are protected.
[[nodiscard]] providers::GenerationOutputContract local_facts_step_output_contract();

// A professional_response as a person reads it: every field on its own lines,
// labelled by where it stands (response, actionType, artifactAction.content,
// artifactAction.table.rows, artifactAction.cells[0].newValue, ...), its text
// exactly as written with real line breaks - never escaped JSON, where "\n"
// would stand against the value after it. Text that is not a JSON object is
// returned as it is.
[[nodiscard]] std::string readable_professional_response(const std::string &model_response);

[[nodiscard]] std::optional<ProfessionalResponseProposal> parse_professional_response_proposal(
	const std::string &model_response);

[[nodiscard]] nlohmann::json professional_artifact_tool_arguments(
	const ProfessionalArtifactActionPlan &plan, const ProfessionalArtifactProposal &proposal);

// The approval policy of artifact.action.apply beside the Project's action
// mode, read from the tool's own arguments. True when this one action waits
// for the lawyer in every mode: they asked to see it first (askFirst), or it
// quotes no words of theirs asking for it (requestedBy).
[[nodiscard]] bool professional_artifact_action_always_asks(const nlohmann::json &arguments);
// How far the action reaches, for "Apply routine changes": routine when it
// only adds to or tidies one document (a new document, text or rows added,
// formatting set, a misspelt word corrected, values entered in a workbook's
// cells), elevated when it changes what the document already says - a
// passage or row deleted, a passage emptied or reworded
// (tools::rewrites_substance), a cell cleared or zeroed
// (tools::takes_out_substance) - or cannot be read.
[[nodiscard]] tools::ToolEffectClass professional_artifact_action_effect(const nlohmann::json &arguments);

// The independent Professional Review gate, ported from ConnectedDocumentAction's
// Professional Completion Challenge and reusing its exact verdict/issue
// vocabulary (Document/Types/ProfessionalReview.hpp) - a fresh, bounded
// evaluator turn judging the proposal that was just drafted, never the
// drafting turn's own self-assessment (a real design-partner acceptance run
// proved self-assessment during drafting is not reliable). Only ever
// constructed when plan.requires_professional_review is true.
// With `reviewed_document_ids`, the review's sourceDispositions is an object
// holding exactly one entry per reviewed source, each required: a provider
// that enforces the schema cannot return a pass that skips a source, so the
// review is never asked a second time for what it left out (a live run paid a
// whole second review call for exactly that, 24 Sep 2026). Without them, the
// open array form.
[[nodiscard]] providers::GenerationOutputContract professional_artifact_review_output_contract(
	const std::vector<std::string> &reviewed_document_ids = {});
[[nodiscard]] std::optional<ProfessionalReviewResult> parse_professional_artifact_review(
	const std::string &model_response);
// `available_sources` is every real Project source document actually in this
// request's own context (canonical document id, real name) - never inferred
// from proposal.content, never a filename-only identity. THE MODEL decides
// each one's disposition (used_as_evidence/reviewed_no_material_evidence/
// methodology_or_instruction/irrelevant_to_objective, Document/Types/
// ProfessionalReview.hpp); Singularity only ever tells it what sources exist and
// requires SOME disposition for each - never that every source must actually
// contribute content. See ProfessionalReviewSourceDisposition's own comment.
// `runtime_limitation`: a limitation Singularity itself declared for this
// operation (external research the model requested could not run) - stated
// to the reviewer as a runtime fact, not a drafting defect a redraft could
// cure, so review judges whether the draft discloses it honestly.
[[nodiscard]] std::string professional_artifact_review_instruction(
	const ProfessionalArtifactActionPlan &plan, const ProfessionalArtifactProposal &proposal,
	const std::vector<std::pair<std::string, std::string>> &available_sources,
	const std::vector<research::ExternalEvidenceReference> &external_evidence = {},
	const std::optional<std::string> &runtime_limitation = std::nullopt,
	const std::optional<std::string> &review_completion_note = std::nullopt);
// The same review split along Papinian's typed ownership, for the review's
// canonical evidence prompt (PromptAssembler): the capability instruction is
// machine-owned text only (role, disposition vocabulary, judging rules, output
// format) for the CAPABILITY section, and the request carries everything that
// is Project or proposal data (the user's objective, the proposal including
// any table or cell change, the available sources with their provider-facing
// handles, external evidence, a declared runtime limitation, a completion
// note) for CURRENT_REQUEST. The reviewer then judges the proposal against the
// passages in SOURCE_MATERIAL under the same capacity and coverage contract as
// the drafter.
struct ProfessionalReviewSource {
	std::string document_id;
	std::string name;
	// "DOC_n" in the review prompt's SOURCE_MATERIAL; empty when none of the
	// document's passages is in that prompt's marker universe.
	std::string handle;
};
[[nodiscard]] std::string professional_artifact_review_capability_instruction(
	const ProfessionalArtifactActionPlan &plan);
// `drafter_note` is the drafter's own answer to the lawyer beside the
// proposal - where it explains its choices and names the risks. A reviewer
// that never saw it judged liquidated damages drafted for a requested
// "penalty" a departure from the instruction, and the drafter, told to follow
// every point, rewrote an enforceable clause into a void one (live runs, 24
// Sep 2026).
[[nodiscard]] std::string professional_artifact_review_request(const ProfessionalArtifactProposal &proposal,
	const std::string &objective, const std::vector<ProfessionalReviewSource> &available_sources,
	const std::vector<research::ExternalEvidenceReference> &external_evidence = {},
	const std::optional<std::string> &runtime_limitation = std::nullopt,
	const std::optional<std::string> &review_completion_note = std::nullopt,
	const std::string &drafter_note = std::string());
// What the review of a revised proposal is told: what the previous review
// asked for, so it checks that correction instead of searching afresh. A
// fresh second review found new points each round and once asked to undo
// exactly what the first had asked for (live run, 24 Sep 2026).
[[nodiscard]] std::string professional_artifact_revision_review_note(const ProfessionalReviewResult &previous);
// Full redraft - reserved for ProfessionalReviewVerdict::NeedsRedraft (the
// draft itself is structurally/qualitatively wrong, not merely missing
// evidence). Like the targeted completion below it revises rather than
// reconstructs: `prior_response` is the complete previous professional_response
// (every artifactAction field and its source markers) and the action kind of
// `proposal` is kept. NeedsMoreEvidence uses professional_artifact_targeted_evidence_
// completion_instruction() below instead - see execute_task_attempt()'s own
// branch on review->verdict.
// `found_by` says who found the defects - the review, or Singularity's own check
// that the proposed action can be carried out at all (a proposal the tool's
// validator refused before any approval existed).
[[nodiscard]] std::string professional_artifact_correction_instruction(
	const ProfessionalArtifactActionPlan &plan, const ProfessionalReviewResult &review,
	const ProfessionalArtifactProposal &proposal, const std::string &prior_response,
	const std::string &found_by = "An independent professional review found it professionally incomplete:");
// A bounded, targeted completion turn for ProfessionalReviewVerdict::
// NeedsMoreEvidence: the already-accepted draft, the review's own unresolved
// issues, and ONLY the specific source(s) the review itself named (issue.
// document_id) - never the full original Project context/prompt again. The
// model may still conclude a named source has no material evidence for the
// question (a valid, complete resolution - see SourceDisposition's own
// comment); it must never be pressured into inventing a fact merely to
// justify citing a document. `revisited_documents` is {document_id,
// name} for exactly the source(s) execute_task_attempt() resolved from the
// review's own issues; their passages reach the model through the
// continuation's own SOURCE_MATERIAL (same assembly, capacity budget and
// source markers as the first call), which this text follows.
// `prior_response` is the accepted professional_response text itself, so every
// artifactAction payload field (table, matchText/replacementText, cells) the
// continuation must preserve is visible to it.
[[nodiscard]] std::string professional_artifact_targeted_evidence_completion_instruction(
	const ProfessionalArtifactProposal &proposal, const std::string &prior_response,
	const ProfessionalReviewResult &review,
	const std::vector<std::pair<std::string, std::string>> &revisited_documents);
// Canonical provenance JSON for one review verdict, in the exact shape
// ChatConversation.cpp's professional_review_from_task() already knows how
// to project to the frontend (reused unchanged, never a second contract).
[[nodiscard]] nlohmann::json professional_artifact_review_json(
	const ProfessionalReviewResult &review, std::uint64_t reviewed_revision);

// A protected value's symbol that restoration could not put back, still in
// a draft's own words (title, summary, content, replacement text, table cells)
// or the drafter's note when given: what it stood for cannot be told here, so
// it is left for the review to raise. (A phrase restoration wrote twice side
// by side is written once where it is restored - PapinianGuard::restore -
// which alone knows what the model wrote there: a live amendment read "seek
// injunctive relief, injunctive relief or specific performance", 25 Sep 2026.)
[[nodiscard]] std::vector<std::string> unrestored_symbols(const ProfessionalArtifactProposal &proposal,
	const std::string *drafter_note = nullptr);

// A reviewer's words made fit for the lawyer who reads them beside a draft:
// about the document only, never about Singularity's protection of it, in English
// and Russian. "a duplicated protected remedy" reads "a duplicated remedy"
// ("защищённое значение" reads "значение"), a type written as a code
// ("PERSON", "LEGAL_ENTITY") reads as a word in the note's language, and a
// sentence about symbols or placeholders together with what protected or
// restored them is left out when anything else was said. The document's own
// words stay: protected persons and parties, a trademark's symbol, mask works.
[[nodiscard]] std::string plain_words_for_lawyer(const std::string &text);

// A new Word document written whole at `path`: the title, then `content` as a
// document - "#" headings, **bold** and *italics*, "-" and "1." lists as lists
// Word numbers, "|" rows as tables - with US Letter or A4 pages by `language`,
// page numbers at the foot and `author` as its writer (this computer's member
// when empty). Throws, in words, for text a Word document cannot hold.
void write_new_word_document(const std::filesystem::path &path, const std::string &title, const std::string &content,
	const std::string &language, const std::string &author);

// A new workbook of one sheet written whole at `path` (D-175): `columns` its
// header row, bold and frozen above `rows`, each column as wide as what it
// holds, and a date, a percentage or an amount stored as the number it stands
// for, shown as it was written. Throws, in words, for text a workbook cannot hold.
void write_new_workbook(const std::filesystem::path &path, const std::vector<std::string> &columns,
	const std::vector<std::vector<std::string>> &rows);

void ensure_production_professional_artifact_tool();
void reset_professional_artifact_tool_for_testing();
void recover_prepared_professional_artifact_action(
	const tools::ToolExecutionContext &context, const std::string &prepared_record_json);

// A replacement that repeats the words standing just before or after what it
// replaces takes those words into the replaced passage, so they are written
// once. Returns how many replacements were settled.
std::size_t settle_replacement_overlaps(const ProfessionalArtifactActionPlan &plan,
	ProfessionalArtifactProposal &proposal, const matters::Matter &project);

// A table quoted whole as tab-separated text and written back with values
// changed becomes one replaceCellText per changed value - exactly, or not at
// all. Returns how many quoted-table operations were settled this way.
std::size_t settle_quoted_table_edits(const ProfessionalArtifactActionPlan &plan,
	ProfessionalArtifactProposal &proposal, const matters::Matter &project);

// The tool's own validation of this exact action, as it would run before an
// approval is created - nothing asked, nothing written. A refusal carries the
// lawyer-facing reason and the refusal's evidence; nothing means the action
// can be made against the document as it now stands.
[[nodiscard]] std::optional<ArtifactActionRefused> validate_professional_artifact_action(
	const ProfessionalArtifactActionPlan &plan, const ProfessionalArtifactProposal &proposal,
	const tools::ToolExecutionContext &context);

[[nodiscard]] ProfessionalArtifactActionResult apply_professional_artifact_action(
	const ProfessionalArtifactActionPlan &plan, const ProfessionalArtifactProposal &proposal,
	const tools::ToolExecutionContext &context, bool preapproved = false);

// A workbook's cells changed in a copy of it, never in it: a colleague's revised
// draft of a tracker, handed in beside the original for the lawyer to accept
// (D-115). The change is checked, written and proven exactly as every change to
// a workbook is (no formula or calculated cell overwritten, a number kept a
// number, every other cell, formula, row and sheet as it was), staged and
// committed only where `copy` does not exist yet. Each cell is then read back
// from the copy as Excel shows it.
struct WorkbookCellReading {
	std::string sheet;
	std::string cell;
	std::string before;
	std::string meant;
	// As the copy shows it, read back after it was written.
	std::string shown;
};
// A cell change that was not written, with what the cell shows now and why.
struct WorkbookCellRefusal {
	std::string sheet;
	std::string cell;
	std::string quoted;
	std::string shown;
	std::string reason;
};
struct WorkbookCopyResult {
	bool isWritten = false;
	std::string problem;
	std::filesystem::path path;
	std::string sha256;
	std::vector<WorkbookCellReading> cells;
	// Each change is judged on its own (D-117): one misquoted cell refuses only
	// itself. A number quoted in any writing of the same number ("421",
	// "421.00", "$421.00") is the cell's value; a cell that already holds what
	// the change gives it is left as it is ("Contracts!H2").
	std::vector<WorkbookCellRefusal> refused;
	std::vector<std::string> unchanged;
};
[[nodiscard]] WorkbookCopyResult write_workbook_cells_to_copy(const std::filesystem::path &source,
	const std::filesystem::path &copy, ProfessionalArtifactProposal proposal);

// A workbook as a colleague reads it: each sheet's cells that hold anything, by
// reference, with what Excel shows and the formula behind a calculated cell.
[[nodiscard]] std::string workbook_cells_for_reading(const std::filesystem::path &path, std::size_t most_characters);

// The versions Singularity kept of one Project document before it changed it -
// its original and the latest ones (DocumentVersionStore) - that the lawyer
// can put back. A kept version holding what the document holds now is left
// out: putting it back would change nothing.
struct DocumentVersions {
	std::string document_id;
	std::string document_name;
	int current_version = 0;
	std::string current_fingerprint;
	// Oldest first.
	std::vector<KeptDocumentVersion> kept;
};

[[nodiscard]] DocumentVersions list_document_versions(const std::string &project_id, const std::string &document_id);

struct RestoredDocumentVersion {
	std::string document_id;
	int restored_version = 0;
	int new_version = 0;
	std::string new_fingerprint;
};

// Puts a kept version of a Project document back, through the same commit
// every Singularity write takes: under the document's write lock, only while the
// document still holds `expected_fingerprint` (what the lawyer was looking at),
// closed and writable; the bytes staged and proven identical to the kept
// version; the version it replaces kept in turn, so the restore can itself be
// undone; then one atomic rename, recorded as Singularity's change. Throws
// platform::DocumentWriteConflict, in plain words, when the document is not in
// a state to be written.
[[nodiscard]] RestoredDocumentVersion restore_document_version(const std::string &project_id,
	const std::string &document_id, int version, const std::string &expected_fingerprint);

[[nodiscard]] const char *to_string(ArtifactActionKind kind) noexcept;

} // namespace singularity::documents
