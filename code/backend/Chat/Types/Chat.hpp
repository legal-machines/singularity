#pragma once

#include "Context/Types/ProjectContext.hpp"
#include "Context/QuoteVerification/QuoteVerification.hpp"
#include "Context/TemporalContext/TemporalContext.hpp"
#include "Document/Types/DocumentActionEvidence.hpp"
#include "Document/Types/DocumentRoleBinding.hpp"
#include "Document/Types/DocumentOperationCheckpoint.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Tool/Types/ToolTypes.hpp"
#include "WorkProduct/Types/WorkProducts.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matters {

enum class ConsiliumRole { Member, Synthesis };
[[nodiscard]] const char *to_string(ConsiliumRole role) noexcept;
[[nodiscard]] std::optional<ConsiliumRole> consilium_role_from_string(const std::string &value) noexcept;

enum class TaskStatus { Success, Error };

enum class ChatTitleAuthority { Default, Automatic, User };
[[nodiscard]] const char *to_string(ChatTitleAuthority authority) noexcept;
[[nodiscard]] ChatTitleAuthority chat_title_authority_from_string(const std::string &value) noexcept;

// One explicit AI Setup role difference at Project or Chat level (Phase 06,
// AI_SETUP_CONTRACT.md). Stored as plain strings, like MatterTask's own
// provider_id/model_id, so Matter/Chat persistence never depends on the
// Routing domain - `role` is one of routing::to_string(AiSetupRole),
// validated at the Bridge boundary. A level's override list holds only the
// roles it actually customized; everything else stays inherited, never a
// copied full configuration.
struct AiSetupRoleOverride {
  std::string role;
  std::string provider_id;
  std::string model_id;
};

// A Chat the person has explicitly compacted ("/compact", chat.compact). It
// stands in for the turns it covers when the next request is assembled, so a
// long conversation keeps working without carrying every turn in full.
//
// Derived from this Chat's OWN canonical operations - never written by a model.
// A compaction that asked a model to summarise the conversation would be a new
// place for facts to be invented, and the one thing a compacted Chat must never
// do is remember something that did not happen. Everything here is quoted or
// counted from records Singularity already holds, which is also why it costs no
// provider call and behaves identically on a frontier model and a local one.
//
// Compaction never writes Project Context: what a conversation established
// belongs to the conversation until a person promotes it.
struct ChatCompaction {
  std::string created_at;
  std::string created_by_actor_id;
  // The turns this record stands in for. Anything later is still carried as
  // itself, so work done after compacting is never summarised away.
  std::vector<std::string> covered_task_ids;
  // The record as PromptAssembler receives it: every covered turn, the newest
  // few in full and the older ones by what they asked and what became of it.
  std::string record;
  // The same conversation at its smallest - every covered turn by its brief
  // record alone - for a window that cannot hold the one above. Nothing is
  // dropped from it: every turn is still named. Empty only for a compaction
  // written before this existed, where `record` stands for both.
  std::string brief_record;
  // Kept structured as well as in the text, so nothing has to be re-parsed out
  // of prose to know what the conversation still depends on.
  std::vector<std::string> cited_document_ids;
  std::vector<std::string> work_product_ids;
  std::vector<std::string> artifact_document_ids;
  std::vector<std::string> unresolved;
};

// The stored value that once meant "this Chat writes a new document"
// (MatterChat::working_document_id). Still accepted and kept, so older Chats
// read unchanged; it binds nothing.
inline constexpr const char *kNewDocumentWorkingTarget = "new-document";

struct MatterChat {
  std::string id;
  std::string project_id;
  std::string title;
  ChatTitleAuthority title_authority = ChatTitleAuthority::Default;
  std::string created_at;
  std::string updated_at;
  long long revision = 1;
  std::string created_by_actor_id;
  std::string updated_by_actor_id;
  std::string operation_id;
  bool archived = false;
  // A deleted Chat is a tombstone, never an erased record: another device may
  // have sent a turn into it before the deletion arrived, and that turn must
  // stay reachable rather than disappearing with the Chat. A Chat with work
  // that arrived after its deletion is listed again so a person can restore
  // it (see Chat/ChatActions).
  std::string deleted_at;
  std::string deleted_by_actor_id;
  // Set once this Chat has been compacted; replaced by a later compaction.
  std::optional<ChatCompaction> compaction;
  std::vector<AiSetupRoleOverride> ai_setup_overrides;
  // The real, persisted starting selection this Chat was created/opened
  // with (Phase 09, "Ask in Chat" - phase-09-documents-actions-source-peek.md).
  // Canonical Document IDs only - never file paths or copied content.
  // Genuinely round-trips through .singularity-workspace persistence (see
  // ChatActions.test.cpp's selection-round-trip case). This is a real
  // consumed source policy, not merely a stored display value: any request
  // naming this Chat's id that supplies no selectedDocumentIds of its own
  // inherits this exact selection into that request's own
  // MatterTask::selected_document_ids (below) and, from there, into real
  // local context retrieval - Routing/MatterRequestExecution.cpp's
  // task_attempt_spec_from_params() does the inheritance,
  // resolve_task_context_for_testing() is the proof this actually reaches
  // ContextRetriever, not just persistence (see
  // MatterRequestExecution.test.cpp's item1/item2/item3). An explicit
  // selectedDocumentIds on one request still overrides this default outright
  // (item2). What does not exist yet: any freeform-message composer that
  // lets a user type directly into an existing Chat - a later Chat phase
  // adds that UI, and it gets this same inheritance for free by simply
  // passing this Chat's id, exactly like Phase 09's own Documents actions do.
  std::vector<std::string> selected_document_ids;
  // Phase 27's restart-surviving draft (unsent composer text). Backend/`.lm`
  // persisted so a React-only copy is never the sole durable copy - never
  // sent automatically, and never visible from another Chat (it round-trips
  // with this exact Chat's own id). A plain last-write-wins field: it does
  // not bump `revision`, since ordinary debounced autosave is not the kind
  // of structural change rename/AI Setup's optimistic lock protects.
  std::string draft_text;
  // A document a lawyer once chose for this Chat to work on, kept as it was
  // stored so older Projects read unchanged. It no longer binds anything: the
  // request says which document is to change, and the model names it among
  // the documents it was given or found (MatterRequestExecution.cpp's request
  // target). Like draft_text, it does not bump `revision`.
  std::string working_document_id;
	// An agent's thread (Agent/): the one conversation a lawyer has with that
	// agent, where it introduces itself, reports what it found and answers
	// what it is asked. `instructions` are the chat's own standing
	// instructions - for an agent's thread, its brief: who it is and the work
	// it stands for - and every request in the chat carries them, as it
	// carries the Matter's. Empty for an ordinary chat.
	std::string agent_id;
	std::string instructions;
};

// Phase 27's source-problem Error (phase-27-chat-lifecycle-observe.md's
// "Errors" - "Board Minutes.docx couldn't be read. / Review document"). A
// real, honest projection of a document this attempt genuinely could not
// read while assembling Relevant Project sources - never a guess from
// selectedDocumentIds, never a parsed error string. `reason` is the same
// real human-readable text the Documents page already shows for this exact
// document (MatterDocument::attention_detail), never a second wording.
struct UnavailableSourceDocument {
  std::string document_id;
  std::string document_name;
  std::string reason;
};

// A read-only link to the existing canonical AI operation. Chat storage never
// contains a prompt/response transcript or copies provenance/usage/source
// evidence; it only groups the MatterTask that already owns those facts.
// A deleted Chat that new work reached anyway - listed so the person can
// restore it instead of losing that work silently.
struct ChatTaskLink {
  std::string task_id;
  std::string chat_id;
};

struct ChatList {
  std::string project_id;
  std::string project_name;
  std::vector<MatterChat> chats;
  std::vector<ChatTaskLink> task_links;
  // Whether this Project currently has real, non-blank Project instructions
  // (Matter::instructions) - the Context inspector's own "Instructions"
  // fact (phase-26-chat-inspector-workflows.md). Never the instructions text
  // itself: normal Chat never needs to display it, only whether it is active.
  bool project_instructions_active = false;
};

// Immutable, privacy-safe execution fact attached to the canonical
// MatterTask. Provider bodies, model reasoning, document text, Papinian
// restoration mappings and tool arguments never belong here.
struct AgentOperationEvent {
	std::string id;
	std::uint64_t sequence = 0;
	std::string type;
	std::string created_at;
	std::uint64_t operation_revision = 0;
	std::string message;
	std::optional<std::string> subject_id;
	std::optional<std::string> outcome;
};

// What one attempt's request carried of Project Context and of its
// conversation's earlier turns (by Context item id and task id) - the record of
// the coverage statement that request made to the model, never re-derived later.
// What one Project document contributed to a request: how many of its
// retrieved passages the request actually carried, and whether the person
// selected it explicitly rather than Singularity choosing it by relevance.
struct SourceCoverage {
	std::string document_id;
	std::size_t supplied = 0;
	std::size_t retrieved = 0;
	bool explicitly_selected = false;
};

// A question the model put to the lawyer in the middle of this operation
// (Tool/LawyerQuestion), and what became of it. The question is kept exactly
// as the lawyer was shown it - already restored, never the protected form the
// model wrote - and the answer as the lawyer gave it: the same kind of text
// the operation already keeps as its request and its answer. Written only by
// the operation's own executing thread.
struct LawyerQuestionRecord {
	// The approval id the question was asked under.
	std::string id;
	std::optional<std::string> title;
	// [{id, prompt, note, choice, allowsOwnAnswer, options: [{id, label, note}]}]
	nlohmann::json questions = nlohmann::json::array();
	// {questionId: {optionIds, text}}, or null while it is open and when the
	// lawyer left the decision to Singularity.
	nlohmann::json answers = nullptr;
	// "asked", "answered" or "left_to_model".
	std::string state = "asked";
	std::string asked_at;
	std::optional<std::string> answered_at;
};

struct RequestCoverage {
	std::size_t context_items = 0;
	std::vector<std::string> omitted_context_item_ids;
	std::size_t conversation_turns = 0;
	std::vector<std::string> turns_in_full;
	std::vector<std::string> turns_as_records;
	std::vector<std::string> omitted_turns;
	std::vector<SourceCoverage> sources;
	// True when the passages came from relevance retrieval: the request read a
	// sample of the Project, never the whole of it.
	bool relevance_selection = false;
	std::size_t relevance_candidate_documents = 0;
};

// A value Singularity worked out on this computer at the model's request and
// restored into an answer (Papinian Cycle 4, DESIGN4 §4.5): kept by the fact
// it is, so a later turn of the chat sends it as the same derived fact, with
// its name and how it was worked out. A fact key, a type, the value's key (the
// value is already the lawyer's own local text in the answer), the model's
// name for it, its AST over fact keys, and the facts it rests on. Never a
// symbol, never a restoration mapping.
struct PersistedDerivedFact {
  std::string fact_key;
  std::string type;
  std::string value_key;
  std::string label;
  std::string ast;
  std::vector<std::string> roots;
};

// How many charged observations the model has had about each hidden fact of
// the matter, for its whole life (DESIGN4 §8.3, §4.5): counts over fact keys
// and public grouping keys, the canonical keys already answered, how much of
// each count a preload spent, and the order the relations told over fact keys
// ("lt|a|b": what the provider was already told, kept so what it implies is
// free). No value.
struct PersistedObservationLedger {
  std::map<std::string, std::size_t> by_root;
  std::map<std::string, std::size_t> by_calendar_root;
  std::map<std::string, std::size_t> by_group_key;
  std::vector<std::string> answered;
  std::vector<std::string> zero_divisors;
  std::map<std::string, std::size_t> preload_by_root;
  std::vector<std::string> order_edges;

  [[nodiscard]] bool empty() const noexcept {
    return by_root.empty() && by_calendar_root.empty() && by_group_key.empty() && answered.empty() &&
           zero_divisors.empty() && preload_by_root.empty() && order_edges.empty();
  }
};

// Where a file Singularity wrote carries each fact it restored, and which spans
// of it the model wrote (DESIGN4 §4.5), keyed in the Matter by the SHA-256 of
// the file's extracted text: read back unchanged, the file is protected from
// these anchors rather than detected afresh. Offsets, fact keys and value
// keys only.
struct WrittenFileFact {
  std::size_t start = 0;
  std::size_t length = 0;
  std::string fact_key;
  std::string kind;
  std::string value_key;
};

struct WrittenFileSpan {
  std::size_t start = 0;
  std::size_t length = 0;
};

struct WrittenFileAnchors {
  std::vector<WrittenFileFact> facts;
  std::vector<WrittenFileSpan> model_spans;
};

// Who began an operation and what it may do - Tool/OperationAuthority's record
// as it stood when the operation began - signed by the computer that began it
// (Chat/OperationOrigin). Another computer that carries the operation on reads
// its restrictions from here only when the signature holds and the computer is
// confirmed as its person's; otherwise the work stays on that computer.
struct OperationOriginStatement {
  std::string actor_id;
  // "person" or "agent".
  std::string by = "person";
  std::string agent_id;
  // The most it may do without asking (Tool/ToolTypes ProjectActionMode).
  std::string ceiling;
  bool local_only = false;
  std::string public_key;
  std::string signature;
};

// Where in a document of the Matter a message is about, as its author pointed
// at it - in Word, at the clause under the cursor (D-105, Office/): the
// document, the section by its number and heading, and the words there as they
// read when the message was written. Empty for a message about no place, and
// for every message written before places were kept.
struct DocumentPlace {
  std::string document_id;
  std::string section;
  std::string heading;
  std::string quote;
  [[nodiscard]] bool empty() const { return document_id.empty(); }
};

struct MatterTask {
  std::string id;
  std::string created_at;
	// Shared durable AI-operation evidence. Ordinary chat/task requests use
	// "matter_task"; automatic Context uses "context_automation" in this same
	// collection rather than maintaining a parallel audit store. An agent's
	// own message in its thread - introducing itself, telling what a check
	// found - is an "agent_message": no request, no model, only its words.
	std::string operation_type = "matter_task";
	// The agent (Agent/) this turn belongs to: a turn of its thread, or work
	// it did. `agent_initiated` when the agent began it itself - its
	// schedule, a change it noticed, another agent's report - not a person.
	std::string agent_id;
	bool agent_initiated = false;
	// The operation works only on this computer (Tool/OperationAuthority keeps
	// the authority): local models, no network. Kept here too, so the work
	// stays here when this computer's record is lost or another computer
	// carries it on. It can only keep work here, never let it out: a request
	// that carries this operation on stays here whatever this says if this
	// computer recorded it so.
	bool local_only = false;
	// Who wrote the turn, as Singularity knew them when it was written: a
	// person (their actor id and name) - the one who asked, or who wrote to
	// their colleagues ("member_message": a person's words in a thread, no
	// request) - or an agent, by its name. Empty for turns written before it
	// was kept.
	std::string author_actor_id;
	std::string author_name;
	// Who a person's message names (@): agents by their id, people by their
	// actor id. An agent named takes the message as work and answers in the
	// same thread.
	std::vector<std::string> mentions;
	// What a message carries, whoever wrote it, a person or an AI colleague
	// alike: the message of the same thread it replies to and quotes; the
	// Matter's documents it hands over, as files; and how the members reacted
	// to it (an emoji, and the actor ids of who put it). Empty for a message
	// without them, and for every message written before they were kept.
	std::string quote_message_id;
	std::vector<std::string> attachment_document_ids;
	std::map<std::string, std::vector<std::string>> reactions;
	// When a person's message reached each other member of its chat (D-212): by
	// their actor id, the first time a computer of theirs had it, as that
	// computer's window said once it had read it. Empty until one has.
	std::map<std::string, std::string> received;
	// When its author last changed its words, and when its author deleted it:
	// a deleted message keeps its place in the thread, without its words.
	std::string edited_at;
	std::string deleted_at;
	// What else a message hands in, as the prototype's messages do (D-087): a
	// line for what its author did not check, with its files; the answers it
	// offers to a question it asks; and, for a reply that is one of those
	// answers, the answer it gives (it quotes the question). Empty otherwise.
	std::string not_checked;
	std::vector<std::string> ask_options;
	std::string answer_option;
	// The place in a document the message is about (DocumentPlace).
	DocumentPlace place;
	// A thread's closing ("closed", its words the conclusion) or reopening
	// ("reopened"), kept as a record in the thread by the member who did it
	// (D-088). Empty for every other message.
	std::string thread_event;
	// A person's message as the computer that wrote it signed it
	// (Platform/DeviceKey, Chat/MessageProvenance): that computer's public key
	// and its signature over the message's words, author, thread and names.
	// Empty for a message written before messages were signed - such a
	// message never counts as its author's instruction on another computer.
	std::string signer_public_key;
	std::string signature;
	// An operation's origin as the computer that began it signed it. Empty for
	// an operation begun before origins were signed, or by a version that does
	// not sign them.
	std::optional<OperationOriginStatement> origin;
	// The person's message an agent's turn answers (a "member_message" that
	// named it): the thread shows that message once, then the answer.
	std::string answers_message_id;
	std::string operation_role = "reasoning";
	// Stable client/runtime operation identity and durable execution state.
	// Existing records default to completed; an in-flight connected document
	// operation is checkpointed on this same canonical task.
	std::optional<std::string> agent_operation_id;
	std::string operation_execution_state = "completed";
	std::uint64_t operation_revision = 0;
	std::uint64_t turn_contract_revision = 0;
	std::string operation_stage;
	std::string operation_started_at;
	std::string operation_updated_at;
	std::string operation_activity;
	bool operation_cancel_requested = false;
	bool operation_resume_allowed = false;
	// The computer whose Singularity runs this operation, or last ran it
	// (Platform/ActorIdentity device id). In a Matter shared between computers
	// only that one may call it interrupted or resume it while it is unfinished.
	// Empty for operations recorded before it was kept.
	std::string operation_device_id;
	std::optional<tools::ToolApprovalRequest> operation_approval;
	// The change the lawyer rejected, exactly as it was put to them - kept so
	// the chat still shows what was declined, and in which document, after
	// Singularity is opened again. Nothing of it was written.
	std::optional<tools::ToolApprovalRequest> rejected_approval;
	// Every question this operation put to the lawyer while it worked, in the
	// order it asked them.
	std::vector<LawyerQuestionRecord> lawyer_questions;
	std::vector<AgentOperationEvent> operation_events;
	std::optional<documents::DocumentOperationCheckpoint> document_operation_checkpoint;
	std::string ai_setup_source;
	std::optional<std::string> requested_provider_id;
	std::optional<std::string> requested_model_id;
  std::optional<std::string> chat_id;
  std::string prompt;
  std::string legal_task_preset_id = "general";
  int legal_task_preset_version = 1;
  std::string prompt_profile_id = "legal-machines";
  int prompt_profile_version = 1;
  std::vector<std::string> selected_document_ids;
	// Backend-resolved professional effect, distinct from document selection
	// and from ToolBroker permission. These are durable operation facts; old
	// tasks default to ordinary conversation and remain readable.
	std::string professional_capability = "conversation";
	std::vector<std::string> professional_operations;
	std::vector<std::string> professional_source_document_ids;
	std::optional<std::string> professional_target_document_id;
	std::string professional_destination_format = "unknown";
	std::optional<std::string> professional_destination_file_name;
	bool professional_write_intent = false;
  int attempt = 1;
  std::optional<std::string> root_task_id;
  std::optional<std::string> matter_instructions_snapshot;
  bool matter_instructions_snapshot_captured = false;
  std::optional<std::string> response_text;
	// Each amount or date Singularity restored into response_text, by its place in
	// that text and the digest of the fact it is (FactAnchor): the next turn
	// sends it as the same fact of the material, not merely an equal value.
	// Offsets and a digest only - never a symbol, never a mapping; the value
	// itself is already the lawyer's own local text.
	std::vector<protected_api::papinian::FactAnchor> fact_anchors;
	// The values Singularity worked out for this answer at the model's request
	// (PersistedDerivedFact), and the answer as the provider wrote it with each
	// symbol replaced by its fact anchor (⟪fact:key⟫, no value): what a later
	// turn re-sends instead of re-detecting the restored text (DESIGN4 §4.5).
	// Empty for every task written before them.
	std::vector<PersistedDerivedFact> derived_facts;
	std::optional<std::string> protected_template;
	// Carried from the model's answer to the save that follows it, and never
	// persisted: the name it offered for the conversation. run_task() applies
	// it under the Project's own rule (a chat is named once).
	std::optional<std::string> suggested_chat_title;
  std::optional<std::string> provider_id;
  std::optional<std::string> model_id;
	std::string execution_kind;
	std::string runtime_id;
	std::string runtime_version;
	std::string runtime_source_commit;
	std::string runtime_platform;
	std::string runtime_backend;
  std::string runtime_model_id;
  std::string protected_api_mode;
	// Actual execution fact, distinct from the configured policy mode above.
	// Local inference keeps this false even when the Project's outbound policy
	// is Standard; a successful external protected request records true.
	bool protected_api_applied = false;
  protected_api::papinian::PrivacyMetadata privacy_metadata;
	// Each model call the operation made, as its bill line reads it (D-252):
	// its time, provider and model, the provider's own id for the request, the
	// fingerprints of what was sent and what came back, and its own tokens and
	// cost - never a marker, passage or anything that stands for the Matter's
	// content. Kept in the Matter, so every computer of the firm can bill the
	// work whichever ran it; the full record of what left stays in the Routing
	// Log of the computer that sent it. Written from privacy_metadata's calls
	// as the operation is saved, and kept from then on.
	std::vector<protected_api::papinian::OutboundCall> billed_calls;
  TaskStatus status = TaskStatus::Success;
  // Phase 24 request lifecycle: the user pressed Stop for this operation and
  // the cancellation genuinely took effect (Chat/ChatExecution/ChatExecution.hpp).
  // Durable because it must survive a reload - an operation the user stopped
  // may never come back as an ordinary successful answer. It is a lifecycle
  // fact recorded on the one canonical operation record, never a second
  // operation ledger: the response/usage/provenance this attempt really did
  // produce stay exactly as recorded, so nothing is destroyed or rewritten.
  // False for every task written before this field existed, which is
  // correct - those operations were never stopped.
  bool stopped_by_user = false;
  std::optional<providers::TaskErrorCode> error_code;
  std::optional<std::string> error;
  providers::UsageMetadata usage;
  std::vector<context::ResolvedSourceReference> source_references;
  // Every quotation this answer put in quotation marks, checked against the
  // passage it cited (Context/QuoteVerification). A lawyer reading the answer
  // is told which quotations stand in the documents and which do not, instead
  // of checking each one by hand; a verified one carries the exact bytes it
  // stands at, so the document opens at the quotation.
  std::vector<context::QuotedPassage> quoted_passages;
	std::string research_mode = "auto";
	std::vector<research::ExternalEvidenceReference> external_evidence;
  // Immutable operation provenance: the exact Context items that survived
  // request budgeting for this attempt. Later Context edits/deletions never
  // rewrite this snapshot.
  std::vector<context::ContextItemSnapshot> context_snapshot;
	// The one durable tool/action evidence collection for this operation.
	// Arguments and handler payloads are intentionally absent; the digest
	// binds the exact normalized intent without persisting confidential input.
	std::vector<tools::ToolCallEvidence> tool_call_evidence;
	// Phase 24.1 connected document-work truth. Role bindings are the exact
	// operation-time interpretation of the user's named files; generic action
	// evidence binds validated patches and their rationale/provenance to the
	// old/new Document Registry versions.
	std::vector<documents::DocumentRoleAssignment> document_role_bindings;
	std::vector<documents::AppliedDocumentActionEvidence> document_action_evidence;
	// Stable links to the domain mutations produced by this operation. The
	// model response cannot mutate Context without these being created by
	// ContextCuration first.
	std::vector<std::string> result_context_item_ids;
	std::vector<std::string> result_context_suggestion_ids;
	// Same pattern as result_context_item_ids above, for the domain that owns
	// "retained professional result" (WorkProduct/WorkProductActions.hpp's own
	// retain_task_result()) - stable so a fresh process/reload never has to
	// re-derive which Work Product(s) this operation produced.
	std::vector<std::string> result_work_product_ids;
	// The document review (DocumentReview/) this turn belongs to: the lawyer's
	// request that began a version of it, the colleague's message that hands a
	// version in, or the lawyer's decision on one. Empty for every other turn.
	std::string document_review_id;
	// DirectDocumentWork's own professional-object lineage: the exact real
	// block this operation's mutation targeted, using the same document_id/
	// table_id identity DocumentBlockSurface/EditSurfaceDecision already use
	// elsewhere - never a second parallel identity system. Empty for every
	// task that is not a DirectDocumentWork mutation. Lets a later same-Chat
	// follow-up ("shorten the recommendations, keep severity") resolve back
	// to this EXACT structural location deterministically, rather than
	// re-parsing document text and guessing. The banner row itself is
	// identified by its STABLE structural position (DocumentBlockSurface::
	// banner_row_index), never its content-derived banner_row_key - a real
	// bug found and fixed this session: the row's own content key changes
	// the moment this very mutation writes its new title into that row, so a
	// key captured before the write can never match after it.
	std::optional<std::string> direct_document_work_document_id;
	std::optional<std::string> direct_document_work_table_id;
	std::optional<std::size_t> direct_document_work_banner_row_index;
	// The model's own ProfessionalDocumentResult::title, verbatim - carried on
	// the task so a later Work Product retention (work_product::
	// retain_task_result(), which requires a caller-supplied title) never has
	// to re-derive a name from response_text's own free-form answer text.
	std::optional<std::string> direct_document_work_title;
	// The same record's other typed facts, so a surface renders the result
	// from data instead of parsing response_text: the register's own word for
	// its severity, whether the operation added a new entry or updated an
	// existing one, and the complete memo (description, recommendations,
	// additional information) as Markdown - the text a retained Work Product
	// keeps, while the Chat answer stays the concise summary.
	std::optional<std::string> direct_document_work_severity;
	std::optional<bool> direct_document_work_updated;
	std::optional<std::string> direct_document_work_memo;
	// The sanitized universal Work Product document for this answer, as JSON
	// text (WorkProduct/WorkProductDocument) - what Save as Work Product keeps.
	std::optional<std::string> work_product_document;
	// Generic professional artifact outcome. The document id is the existing
	// Document Registry identity, never a parallel artifact registry record.
	std::optional<std::string> artifact_document_id;
	std::optional<std::string> artifact_title;
	std::optional<std::string> artifact_format;
	std::optional<std::string> artifact_change_kind;
	std::optional<int> artifact_resulting_version;
	// The change itself, as written (documents::ArtifactChange as JSON text):
	// what went, what came, and rows with their columns - so the chat can show
	// the change the way the document now shows it.
	std::optional<std::string> artifact_change;
  std::vector<Finding> findings;
  std::vector<Obligation> obligations;
  std::vector<Event> events;
  std::vector<Change> changes;
  std::optional<std::string> consilium_group_id;
  std::optional<ConsiliumRole> consilium_role;
  // Which participant of the saved Consilium Configuration produced this
  // attempt, and the professional role that participant was given ("Lead
  // reviewer", "Challenger", ...). Set only for a run driven by a
  // Configuration; a legacy targets-only run leaves both unset. Without these
  // a reader - and any UI - can only see which MODEL answered, never which
  // seat at the table it was answering from, which is the thing a lawyer
  // actually reasons about when a panel disagrees.
  std::optional<std::string> consilium_participant_id;
  std::optional<std::string> consilium_participant_role;
  // The Configuration and stage this attempt was run under, so a run can be
  // read back as the plan it actually executed rather than a flat list.
  std::optional<std::string> consilium_configuration_id;
  std::optional<std::string> consilium_stage_id;
  std::optional<std::string> consilium_stage_name;
  std::optional<std::string> workflow_run_id;
  std::optional<std::string> workflow_id;
  std::optional<int> workflow_version;
  std::optional<std::string> workflow_step_id;
  // True only when PromptAssembler's real request budget actually excluded at
  // least one whole lower-priority item (source chunk or Project Context
  // item) for this exact attempt (see PreparedTaskRequest::context_budget_
  // excluded_material in MatterRequestExecution.cpp) - never inferred from
  // byte counts client-side. Phase 26's own Context-budget honesty rule: the
  // normal Chat surface never shows a byte/token budget, only this one real
  // fact, and only when it is genuinely true.
  bool context_budget_excluded_material = false;
	// The order Singularity worked out between the documents this request read, as
	// the model was given it (TEMPORAL_CONTEXT, Context/TemporalContext), each
	// relation with the dates it rests on. Local provenance: the dates never
	// left the device.
	std::vector<context::temporal_context::TemporalRelation> temporal_relations;
  RequestCoverage request_coverage;
  // Phase 27's source-problem Error - the real Relevant-source documents
  // this exact attempt could not read (see matters::retrieve_document_
  // context()'s own out-parameter). Never populated for the strict Selected
  // documents mode, which stays fail-closed exactly as ChatSourceResolver.hpp
  // already documents: a missing/unreadable explicit selection refuses the
  // whole request rather than silently narrowing it, so it never reaches a
  // persisted MatterTask this field could describe.
  std::vector<UnavailableSourceDocument> unavailable_source_documents;
};

} // namespace singularity::matters
