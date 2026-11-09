#pragma once

#include "Chat/Types/Chat.hpp"
#include "Matter/Types/Matter.hpp"
#include "Context/QuoteVerification/QuoteVerification.hpp"
#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "WorkProduct/Types/WorkProducts.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::chat {

// One clickable citation in a Chat result, resolved entirely by the backend
// from the operation's own persisted evidence
// (matters::MatterTask::source_references). `number` is what the reader
// sees; document_id/chunk_id are the canonical identity Source Peek resolves
// against, and are the exact identity this operation actually cited - never
// a fresh lookup against whatever the Project contains now.
struct ChatCitation {
  int number = 0;
	std::string source_kind = "project";
  std::string document_id;
  std::string chunk_id;
  std::string document_name;
  std::optional<std::size_t> page;
  std::optional<std::string> heading;
  // Human-readable local label built from the fields above only - present
  // parts only, never an invented page or heading.
  std::string label;
	std::optional<std::string> evidence_id;
	std::optional<std::string> url;
	std::optional<std::string> domain;
	std::optional<std::string> source_class;
	std::string excerpt;
};

// A Chat result's prose, pre-split around its citations by the backend so a
// renderer never sees, parses or searches for internal marker syntax.
struct ChatResponseSegment {
  std::string text;
  int citation_number = 0;
};

// Phase 25's own real, restrained execution facts for one result - never a
// second telemetry system, just the exact operation-provenance fields
// MatterTask already records, projected for display. Monetary evidence
// mirrors UsageMetadata's own rule: present only when the provider supplied
// a complete amount/currency/classification/methodology set.
struct ChatExecutionSummary {
  std::string primary_role;
  std::optional<std::string> primary_provider_id;
  std::optional<std::string> primary_model_id;
  std::string execution_location;
  bool protection_applied = false;
  std::size_t source_reference_count = 0;
  std::optional<long long> input_tokens;
  std::optional<long long> output_tokens;
  std::optional<long long> total_tokens;
  std::optional<long long> cost_micros;
  std::optional<std::string> cost_currency;
  // True when one of the turn's tools paid for a hosted model call (web
  // research), whose usage is counted above. A local turn that did so is not
  // free, even when that call reported no price.
  bool paid_external_calls = false;
  // Phase 27's safe historical Routing Log metadata (RoutingLog/RoutingLog.hpp's
  // RoutingLogEntry, matched by this exact attempt's operation_id - never a
  // second telemetry write). Present only when a routing-log entry for this
  // attempt still exists; routing_reason stays std::nullopt until real
  // Automatic routing is built (see RoutingLogEntry's own comment). Never the
  // raw historical outbound payload - that is never persisted for UI
  // convenience.
  std::optional<long long> latency_ms;
  std::optional<std::string> routing_reason;
};

// The exact, real Project document a connected document operation
// physically mutated (Document/DocumentPatchTool - Phase 24.1x). Its mere
// presence is the honest "this result is a Draft against a real document"
// signal; nothing here is inferred from response text.
struct ChatDocumentChange {
  std::string document_id;
  int resulting_version = 0;
  // Populated ONLY for the generic ProfessionalArtifactAction path (create_
  // content/append_content/replace_content/update_cells) - std::nullopt for
  // DirectDocumentWork and AdvancedWorkflow/ConnectedDocumentAction results,
  // which keep their own existing specialized presentation (direct_document_
  // work_title, professional_review, etc.) rather than this generic one.
  // `change_kind` is exactly the real, backend-decided "created" or
  // "updated" (matters::MatterTask::artifact_change_kind) - never guessed
  // frontend-side from whether resulting_version happens to be 1.
  std::optional<std::string> artifact_title;
  std::optional<std::string> artifact_format;
  std::optional<std::string> artifact_change_kind;
  // The change as written (documents::ArtifactChange as JSON text), when the
  // task kept it: what went, what came, rows with their columns.
  std::optional<std::string> artifact_change;
};

// Phase 24.1x's own typed Professional Completion Challenge, projected only
// when its recorded revision still matches the draft's CURRENT revision - a
// Pass recorded for an earlier revision is stale and is never projected as
// current. std::nullopt covers both "not a connected document
// operation" and "review still pending for this revision" - Phase 25 shows
// nothing rather than a fabricated status either way.
struct ChatProfessionalReviewIssue {
  std::string category;
  std::string message;
  std::string suggested_next_action;
};

// A half-open row range this operation actually inspected, in the bound
// table's own real row order (Document/ConnectedDocumentAction.hpp's
// EvidenceCoverageRange, projected verbatim) - real runtime-computed
// evidence, never a fabricated percentage.
struct ChatEvidenceCoverageRange {
  std::size_t start_row = 0;
  std::size_t end_row = 0;
};

struct ChatEvidenceObjective {
  std::string id;
  std::string description;
  std::string table_handle;
  std::string coverage_mode;
  std::vector<ChatEvidenceCoverageRange> inspected_ranges;
  bool closed = false;
};

struct ChatProfessionalReview {
  std::string verdict;
  std::uint64_t reviewed_revision = 0;
  std::vector<ChatProfessionalReviewIssue> issues;
  std::vector<ChatEvidenceObjective> evidence_objectives;
};

// One completed operation of a Chat, projected for display. It is a
// projection, not a record: every field here is read from the canonical
// MatterTask that already owns it, and nothing in this struct is stored
// anywhere.
struct ChatTurn {
  std::string task_id;
  std::string created_at;
  // The user's own request, exactly as the operation recorded it.
  std::string prompt;
  // In an agent's thread (MatterChat::agent_id): the agent the turn belongs
  // to; `is_agent_message` for the agent's own words, with no request before
  // them; `agent_initiated` for work the agent began itself.
  std::string agent_id;
  bool is_agent_message = false;
  bool agent_initiated = false;
  // A person's words to their colleagues (no request, no answer), who wrote
  // the turn, and whom it names (MatterTask::author_*, mentions).
  bool is_member_message = false;
  std::string author_actor_id;
  std::string author_name;
  std::vector<std::string> mentions;
  // For a person's message: whose it is, as this computer can tell
  // (Chat/MessageProvenance) - "this_computer", "confirmed", "unconfirmed",
  // "revoked", "unverified" (changed after it was signed) or "unsigned". Empty
  // for every other turn.
  std::string message_provenance;
  // The person's message an agent's turn answers (MatterTask::answers_message_id).
  std::string answers_message_id;
  // What a message carries, whoever wrote it (MatterTask::quote_message_id,
  // attachment_document_ids, reactions), and its edit and deletion; whether
  // the person at this computer may change it - its author, on the computer
  // that wrote it, the same rule for every member.
  std::string quote_message_id;
  std::vector<std::string> attachment_document_ids;
  std::map<std::string, std::vector<std::string>> reactions;
  // When a person's message reached each other member (MatterTask::received).
  std::map<std::string, std::string> received;
  std::string edited_at;
  std::string deleted_at;
  // What else it hands in (MatterTask::not_checked, ask_options,
  // answer_option), and a thread's closing or reopening it records
  // (MatterTask::thread_event).
  std::string not_checked;
  std::vector<std::string> ask_options;
  std::string answer_option;
  std::string thread_event;
  // The place in a document it is about (MatterTask::place).
  matters::DocumentPlace place;
  bool can_change = false;
  // "completed", "failed" or "stopped" - the last of which is the durable
  // MatterTask::stopped_by_user lifecycle fact, so an operation the user
  // stopped never returns as ordinary successful work after a reload.
  std::string status;
  std::vector<ChatResponseSegment> response_segments;
  std::vector<ChatCitation> citations;
  // Every quotation this answer put in quotation marks, checked against the
  // passage it cited (matters::MatterTask::quoted_passages). A reader is told
  // which words genuinely stand in the documents and which could not be found
  // there, rather than checking each one by hand.
  std::vector<context::QuotedPassage> quoted_passages;
  // Marker-shaped spans in the response that could not be resolved to
  // material this operation actually supplied. Counted, never rendered.
  std::size_t unresolved_reference_count = 0;
  // "none" (the response cited nothing), "checked" (every reference it
  // cited points at material actually supplied to the model for this
  // operation) or "partial" (at least one could not be resolved). "checked"
  // is deliberately not a claim that Singularity verified the legal conclusion -
  // see phase-24's "Source language".
  std::string source_reference_status;
  std::string legal_task_preset_id;
  int legal_task_preset_version = 1;
  // Derived from this operation's own recorded selection: a request that
  // named documents ran under Selected documents, one that named none ran
  // under Relevant Project sources.
  std::string source_policy_mode;
  std::vector<std::string> selected_document_ids;
  // The model that actually produced this result, from operation
  // provenance - never re-resolved from current AI Setup.
  std::optional<std::string> provider_id;
  std::optional<std::string> model_id;
  std::string execution_kind;
  std::string protected_api_mode;
  // The execution fact: whether Papinian Guard's policy was genuinely
  // engaged for this operation. Local inference records false even under a
  // Standard outbound policy, so a local answer can never display external
  // protection semantics.
  bool protected_api_applied = false;
	std::string research_mode = "auto";
  // PrivacyMetadata::response_integrity for this operation.
  std::string response_integrity;
  // A figure the answer (or a draft it wrote) still states with no symbol,
  // source or local calculation behind it after its one correction (DESIGN3
  // 9.8 d): the operation's "answer.figure_unresolved" event, in Singularity's own
  // words (ControlText FigureUnresolvedOne/Many), for the answer's status line
  // - never the work product. "" when every figure is traced.
  std::string unresolved_figure_notice;
  // Phase 26's on-demand Protection detail (PrivacyMetadata::protected_count/
  // category_counts, projected verbatim) - never the restoration mapping
  // itself, which stays request-memory-only and is never placed here.
  std::size_t protected_value_count = 0;
  std::map<std::string, std::size_t> protected_category_counts;
  std::optional<std::string> error;
  std::optional<std::string> error_code;
  // Phase 25's real structured legal objects - direct projections of the
  // exact MatterTask::findings/obligations/events/changes this operation
  // already recorded (WorkProduct/Types/WorkProducts.hpp). Empty means the
  // backend found none, never "not yet implemented" - the natural-language
  // response above remains the truthful fallback either way.
  std::vector<matters::Finding> findings;
  std::vector<matters::Obligation> obligations;
  std::vector<matters::Event> events;
  std::vector<matters::Change> changes;
  std::optional<ChatDocumentChange> document_change;
  // The change the lawyer rejected, as it was proposed; nothing of it written.
  std::optional<tools::ToolApprovalRequest> rejected_change;
  // Every question Singularity put to the lawyer while this operation ran, as they
  // were shown it and as they answered it (MatterTask::lawyer_questions). One
  // the turn ended without an answer to reads "unanswered".
  std::vector<matters::LawyerQuestionRecord> questions;
  std::optional<ChatExecutionSummary> execution_summary;
  std::optional<ChatProfessionalReview> professional_review;
  // A durable Work Product this exact operation retained (MatterTask::
  // result_work_product_ids/direct_document_work_title, projected verbatim -
  // never a frontend-generated surrogate). Empty means this operation never
  // retained one, never "not yet implemented" - the plain response above
  // remains the truthful result either way. direct_document_work_title is
  // the professional record's own title, for a calm result label without
  // re-deriving one from response text.
  std::vector<std::string> result_work_product_ids;
  // The document review this turn belongs to (MatterTask::document_review_id).
  std::string document_review_id;
  std::optional<std::string> direct_document_work_title;
  std::optional<std::string> direct_document_work_severity;
  std::optional<bool> direct_document_work_updated;
  // The complete entry as written into the register, as Markdown.
  std::optional<std::string> direct_document_work_memo;
  std::optional<std::string> work_product_document;
  // Phase 26's real Consilium/workflow operation linkage - direct
  // projections of MatterTask::consilium_group_id/consilium_role/
  // workflow_run_id/workflow_id/workflow_version/workflow_step_id, so a Chat
  // conversation can honestly group a Consilium run or a Completor workflow
  // run as one operation instead of unrelated turns. workflow_name/
  // workflow_step_name resolve only when the exact workflow/step still
  // exists in the current registry (context::find_workflow_definition) -
  // std::nullopt otherwise, never a stale or guessed label.
  std::optional<std::string> consilium_group_id;
  std::optional<std::string> consilium_role;
  std::optional<std::string> workflow_run_id;
  std::optional<std::string> workflow_id;
  std::optional<int> workflow_version;
  std::optional<std::string> workflow_step_id;
  std::optional<std::string> workflow_name;
  std::optional<std::string> workflow_step_name;
  // MatterTask::context_budget_excluded_material, projected verbatim -
  // phase-26's own Context-budget honesty rule (never a byte/token count).
  bool context_budget_omitted_material = false;
  // MatterTask::request_coverage, projected verbatim: what this attempt's
  // request carried of Project Context and of the Chat's earlier turns.
  matters::RequestCoverage request_coverage;
  // Phase 27's source-problem Error - real Relevant-source documents this
  // exact attempt could not read, projected verbatim from MatterTask::
  // unavailable_source_documents. Never populated for the strict Selected
  // documents mode, which fails the whole request closed instead (Chat/
  // ChatSourceResolver/ChatSourceResolver.hpp).
  std::vector<matters::UnavailableSourceDocument> unavailable_source_documents;
};

struct ChatConversation {
  std::string project_id;
  std::string chat_id;
  std::string chat_title;
  std::vector<ChatTurn> turns;
};

// The smallest read-only Chat projection over canonical MatterTasks. It
// creates nothing, persists nothing and duplicates nothing: a Chat has no
// transcript, result or provenance store of its own, and this rebuilds the
// whole conversation from the operations that already exist - which is
// exactly why it reconstructs identically in a fresh process with no
// reattachment step. Throws for a Project or Chat that no longer exists.
ChatConversation conversation(const std::string &project_id, const std::string &chat_id);

// Puts every other member's messages on this computer's clock. A message's
// time is its writer's clock; when it reached each member, that member's
// (MatterTask::received). From the two between this member and each other -
// when this member's messages reached them, and theirs reached here - it
// finds how far that member's clock runs from this one's, and moves their
// messages by it: a computer whose clock is hours ahead or behind no longer
// puts its messages after, or before, everyone else's. From one of the two
// alone, only what cannot be is mended: a message never reached anyone before
// it was written. Less than two minutes apart, nothing moves. `viewer` is the
// member at this computer.
void align_member_clocks(std::vector<ChatTurn> &turns, const std::string &viewer);

// A thread's latest message as a list of Matters shows it: who wrote it and
// its words in one line. It is the last message the thread shows: a person's
// message, an AI colleague's, or a model's answer to a person's own request
// - never a deleted message, a closing or reopening, a request still running
// or one that ended without an answer.
struct ChatLatestMessage {
  std::string chat_id;
  std::string task_id;
  std::string at;
  // A person's message: their actor id and name. An AI colleague's: its id
  // and name. A model's answer to a person's own request names no one.
  std::string author_actor_id;
  std::string author_name;
  std::string agent_id;
  // Its words, spaces collapsed into one line of at most 160 letters, its
  // source markers left out.
  std::string text;
  // Who the message names with @: agents by id, people by actor id.
  std::vector<std::string> mentions;
  // The Matter's documents it hands over, for a message of files alone.
  std::size_t attachment_count = 0;
};

// The latest message of each of the Project's chats that has one, in the
// order of its chats. Reads only the Project it is given: no store, no
// routing log, no document.
std::vector<ChatLatestMessage> latest_messages(const matters::Matter &project);

// What a Chat Source Peek can honestly show for one citation.
struct ChatSourcePeek {
  // "resolved", "changed" or "unavailable" - the existing
  // Document/DocumentSourceResolver verdict, unchanged.
  std::string status;
  std::string document_id;
  std::string chunk_id;
  // The document name this operation actually cited. Kept separate from the
  // resolver's current name so a renamed document degrades honestly instead
  // of silently rewriting history.
  std::string cited_document_name;
  std::string document_name;
  int current_version = 0;
  std::optional<std::size_t> page;
  std::optional<std::string> heading;
  std::string text_preview;
};

// Resolves one citation of one operation back to its local source. Every
// step of the chain is verified against canonical state before any document
// is read: the Project exists, the Chat exists, the MatterTask exists and
// belongs to that Chat, and the requested (documentId, chunkId) pair is one
// this exact operation actually recorded as cited. A reference that does not
// belong to this task is refused rather than resolved from elsewhere.
//
// Version binding needs no new provenance system and deliberately adds
// none: a chunk_id already contains its document's content fingerprint
// (Document/DocumentChunker.hpp), so a document changed since the answer was
// written can no longer produce the cited chunk_id and the existing resolver
// reports Changed instead of quietly showing unrelated current bytes.
ChatSourcePeek resolve_source_reference(const std::string &project_id, const std::string &chat_id,
                                        const std::string &task_id, const std::string &document_id,
                                        const std::string &chunk_id);

} // namespace singularity::chat
