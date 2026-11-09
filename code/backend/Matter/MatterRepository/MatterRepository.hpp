#pragma once

#include "Context/ContextRetriever/ContextRetriever.hpp"
#include "Matter/Types/Matter.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::matters {

nlohmann::json serialize_matter_storage(const Matter &matter);
Matter deserialize_matter_storage(const nlohmann::json &value);

// Collaboration conflict alternatives use the same durable Context item
// representation as Project storage. Keep its schema parsing centralized.
nlohmann::json serialize_context_item_storage(const context::ContextItem &item);
context::ContextItem deserialize_context_item_storage(const nlohmann::json &value);

std::string generate_matter_id();
std::string generate_document_id();
std::string generate_document_event_id();
std::string generate_task_id();
std::string generate_review_table_id();
std::string generate_review_column_id();
std::string generate_review_run_id();
std::string generate_chat_id();
std::string generate_chat_operation_id();
std::string generate_finding_id();
std::string generate_obligation_id();
std::string generate_event_id();
std::string generate_change_id();
std::string generate_note_id();
std::string generate_issue_id();
std::string generate_work_product_id();
std::string generate_work_product_type_id();
std::string generate_work_product_revision_id();
std::string generate_consilium_configuration_id();
std::string generate_consilium_participant_id();
std::string generate_consilium_stage_id();
std::string generate_consilium_operation_id();
std::string generate_consilium_group_id();
std::string generate_workflow_run_id();

// Master Spec Sec.26 promotion: builds a new Issue from `finding` - `title`/
// `description`/`source_refs` copied as-is (every entry in `source_refs` was
// already validated Supplied against the exact request context snapshot
// that produced the finding - never re-derived from current Matter state),
// status starts Open, `id`/`created_at` are caller-supplied (see
// generate_issue_id()/workspaces::current_timestamp()) so this stays pure
// and testable, `updated_at` starts equal to `created_at`. Does not read or
// modify `finding`/its owning MatterTask. Repeated promotion of the same
// Finding is not prevented or deduplicated here - each call yields one more
// Issue, the same add-only behavior already established for
// MatterDocument/MatterNote.
Issue promote_finding_to_issue(const Finding &finding, const std::string &matter_id,
                               const std::string &id, const std::string &created_at);

// Every task in `tasks` that is a Consilium Member of `group_id` - the
// independent answers a synthesis is built over. Excludes any Synthesis
// task in the same group, so a synthesis can never itself become an input
// to another synthesis. Preserves `tasks`' own order (creation order).
std::vector<MatterTask> consilium_members(const std::vector<MatterTask> &tasks,
                                          const std::string &group_id);

// Builds the deterministic prompt text for "Synthesize with..." (Sec.68): a
// literal, verbatim transcript of the original task and every member's own
// provider/model and response, followed by a fixed instruction to reconcile
// them and call out material disagreement rather than silently picking one
// answer. Never summarizes or edits a member's response_text - the
// synthesis model sees exactly what each member actually said, and this
// text is sent through the same Papinian Guard pass as any other prompt
// (see execute_task_attempt()), so anything protected in a member's answer
// is protected again here. `members` must be non-empty and every entry must
// have a response_text (a caller must filter to successful members first).
// A deliberation turn: the objective plus the answers already given in this
// run, for a participant whose configured stage puts it AFTER them. This is
// what a Sequential stage means - a challenger or reviewer seat is only
// meaningful if it can actually see what it is challenging or reviewing.
// `participant_role` is the seat's own configured professional role, stated so
// the model answers from that seat rather than restating the panel.
std::string build_consilium_deliberation_prompt(const std::string &objective,
                                                const std::vector<MatterTask> &answered,
                                                const std::string &participant_role);

std::string build_consilium_synthesis_prompt(const std::string &original_prompt,
                                              const std::vector<MatterTask> &members);

// Master Spec Sec.24: what "a selected result / Matter note" means in V1 -
// a successful MatterTask's own response, or a Note. Issues are
// deliberately not an export source here: Sec.24's own export bullet names
// only "a selected result / Matter note", and Sec.26 never mentions
// exporting an Issue either.
enum class ExportSourceType { Task, Note, WorkProduct };

// Parses a wire "sourceType" string. Fails closed (nullopt) on anything but
// exactly "task"/"note"/"work_product", the same fail-closed convention as
// issue_status_from_string().
[[nodiscard]] std::optional<ExportSourceType> export_source_type_from_string(const std::string &value) noexcept;

// Master Spec Sec.24 local export: the exact local text a "Copy"/"Add to
// Notes" action already exposes for `source_id` - a successful MatterTask's
// `response_text` verbatim, a Note's `content` verbatim, or a text-bearing
// Work Product's own current content. No rendering, no
// re-derivation, no revalidation against current Matter state. Throws when
// `source_id` does not resolve to an exportable object in `matter` - an
// unknown id, a deleted note, or a task that does not exist or never
// succeeded (nothing else has a durable "answer" to export).
[[nodiscard]] std::string resolve_export_content(const Matter &matter, ExportSourceType source_type,
                                                  const std::string &source_id);

// Writes `content` verbatim as UTF-8 bytes to `path` - the exact write step
// matter.exportText (Bridge/NativeBridge/NativeBridge.cpp) performs once the native Save
// dialog returns a chosen path. Kept here, not in the dialog-heavy native
// bridge, so it is directly unit-testable against a real temporary file.
// Throws std::runtime_error if the file cannot be opened, written or closed
// cleanly - the caller must not report a successful export otherwise.
void write_export_file(const std::filesystem::path &path, const std::string &content);

// The id every attempt of the same logical task shares: `task`'s own id if
// it is the first attempt (root_task_id unset), otherwise its root_task_id.
std::string task_group_id(const MatterTask &task);

// The attempt number a new attempt of the same logical task as `existing`
// should use: the highest `attempt` among every task in `tasks` sharing
// existing's task_group_id(), plus one. Returns 2 the first time a task with
// no prior retries is retried.
int next_attempt_number(const std::vector<MatterTask> &tasks, const MatterTask &existing);

// Every task in `tasks` whose task_group_id() is not `chain_id` - used to
// build prior-task Matter history for a retry/run-with-another-model
// attempt. A retry chain's attempts are independent executions of the same
// preserved task configuration (Master Spec Sec.66/67), not a conversation:
// Attempt 2 of a chain must not receive Attempt 1's own prompt/response as
// history merely because it is already persisted in Matter::tasks by the
// time Attempt 2 runs, and Run with another model must not let one model see
// another model's prior judgment on the same task. A fresh matter.runTask
// attempt has no chain yet (nothing in Matter::tasks can share an id that
// does not exist), so this is only ever applied when building history for a
// retry - see execute_task_attempt() in native_bridge.cpp.
std::vector<MatterTask> exclude_chain(const std::vector<MatterTask> &tasks,
                                      const std::string &chain_id);

// The Matter instructions a new attempt of `original` should use: its own
// captured snapshot when one exists, or `current_matter_instructions` for a
// task that predates snapshot capture (WP5-D) - a documented compatibility
// fallback, since no historical value was ever recorded for it, not a
// fabricated one.
std::optional<std::string> resolve_retry_matter_instructions(
    const MatterTask &original, const std::optional<std::string> &current_matter_instructions);

// Whether `task` already wrote its change to a document: applied evidence,
// or a document operation that reached "committed".
bool has_committed_document_change(const MatterTask &task);

// Whether an implicit Retry (Master Spec Sec.66: same task, same model) is
// honest for `task` - false for a task with nothing to recover from (it
// already succeeded), whose error will just reproduce itself with
// identical inputs (see providers::is_retryable), or whose change was
// already written (a new attempt would only write it twice). A request the
// lawyer stopped can be retried otherwise. Run with another model (Sec.67)
// is available for a successful task too, and redoes a written change in
// place - see is_same_model_target for its own eligibility rule.
bool is_retry_eligible(const MatterTask &task);

// Whether running `task` again against `provider_id`/`model_id` would be a
// no-op disguised as Run with another model (Sec.67: "choose a different
// connected model") - true only when `task` already resolved a model and it
// is exactly this pair.
bool is_same_model_target(const MatterTask &task, const std::string &provider_id,
                          const std::string &model_id);

// "<workspace>/.singularity-workspace/matters.json" - every matter in that workspace,
// together.  Small enough in practice (a few matters, each with a handful
// of documents/tasks) that one file beats one-file-per-matter for this
// iteration; see save_matters().
std::filesystem::path manifest_path(const std::filesystem::path &workspace_path);

// Whether `matter_id` can be a Project's id: letters, digits, '_' and '-',
// at most 128 of them - one name on disk that never reaches outside the folder
// it is joined to. Every id Singularity generates is one ("mt_" + hex); an id
// read from a folder's .lm or sent by the window is checked before it is ever
// joined to a path.
bool is_valid_matter_id(const std::string &matter_id);

// "<workspace>/.singularity-workspace/documents/<matterId>/" - where a matter's imported
// files actually live, named by MatterDocument::stored_name. Throws
// std::invalid_argument for an id that is_valid_matter_id() refuses: joined to
// a path, "/Users/someone" replaces the base and "../.." climbs out of it.
std::filesystem::path documents_directory(const std::filesystem::path &workspace_path,
                                          const std::string &matter_id);

// Where one of a Project's imported copies lives: documents_directory() joined
// with `stored_name` - nothing when the Project's id is not valid or the stored
// name is not one plain file name. A stored name is read from the Project's
// data, which another writer may have edited: "../../x" or "/Users/someone/x"
// names no copy of Singularity's, and is never read, hashed or deleted.
std::optional<std::filesystem::path> managed_document_path(const std::filesystem::path &workspace_path,
                                                           const std::string &matter_id,
                                                           const std::string &stored_name);

// Whether this computer holds Singularity's own copy of a Managed document. A
// Matter's records travel to the firm's other computers, never that copy: on
// them the document is the computer's that added it, not ready here.
bool holds_managed_copy(const std::filesystem::path &workspace_path, const std::string &matter_id,
                        const MatterDocument &document);

// A change to a Project whose data is unavailable or damaged
// (Matter::data_state): nothing was saved. `code()` is the stable reason the
// native bridge reports ("project_data_unavailable", "project_data_damaged").
class ProjectDataError final : public std::runtime_error {
public:
  ProjectDataError(ProjectDataState state, const std::string &message);
  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

// A Matter read to show what it holds - its threads, a conversation: one whose
// data can't be read just now (a write caught midway, a file a sync client
// holds, a folder out of reach) is refused with ProjectDataError, never read
// as one that holds nothing. The window then keeps what it shows and reads it
// again. Every such read goes through this one rule.
void require_readable(const Matter &matter);

// The Matter/Project persistence facade: for a Matter not yet cut over to
// portable .lm truth, reads matters.json exactly as documented below. For one
// that has been (its directory_path already has .lm set up), returns its
// live, canonical .lm state instead - matters.json holds only a locator for
// it (id/directoryPath), never a content snapshot. Every existing domain
// caller (Matter/Activity/Chat/Document/Routing/WorkProduct) already does
// load-modify-save through this function and therefore transparently becomes
// .lm-aware with no call-site change.
//
// matters.json semantics: returns an empty list only when neither a manifest
// nor a recovery copy exists. If the current manifest is invalid and
// `.previous` is valid, moves the damaged bytes to a unique `.corrupt-*`
// sibling, restores the valid copy, and throws a recoverable retry error
// once. Throws std::runtime_error without replacing either source when
// neither copy is readable.
std::vector<Matter> load_matters(const std::filesystem::path &workspace_path);

// The write side of the same facade: a Matter cut over to .lm (its
// directory_path already has .lm set up) is persisted there - as an
// attributed operation against its own merge baseline, never as a whole-file
// overwrite - and matters.json keeps only its locator, never its content.
// Everything else is written to matters.json exactly as documented below.
//
// matters.json semantics: atomic write (unique temp file + platform
// replacement), retaining the prior valid manifest as `.previous`. Throws
// std::runtime_error on failure and refuses to overwrite a corrupt current
// manifest.
void save_matters(const std::filesystem::path &workspace_path, const std::vector<Matter> &matters);

// One Project through the same facade, without reading, diffing or writing
// any other: exactly the entry load_matters() would list for `id` (same data
// states, same .lm hydration), or std::nullopt when the workspace lists no
// such Project. Work that concerns one Project - opening its page, its
// documents, its protection - costs that Project, not the workspace.
std::optional<Matter> load_matter(const std::filesystem::path &workspace_path, const std::string &id);

// Persists one Project exactly as save_matters() would persist it within the
// whole list - the same refusals for unavailable/damaged data, .lm canonical
// for a cut-over Project - and rewrites matters.json only when that
// Project's own entry in it changes. Other Projects are neither read nor
// written.
void save_matter(const std::filesystem::path &workspace_path, const Matter &matter);

// The Project ids matters.json lists, in its order, read from the locators
// alone: no Project's data is opened.
std::vector<std::string> list_matter_ids(const std::filesystem::path &workspace_path);

// Reconciles source bytes with the fingerprint-addressed local extraction
// cache. Returns true when persisted metadata changed.
bool refresh_document_derivatives(const std::filesystem::path &workspace_path,
                                  const std::string &matter_id,
                                  MatterDocument &document);
bool refresh_matter_documents(const std::filesystem::path &workspace_path,
                              Matter &matter);

// Context assembled only from fingerprint-matched, AI-ready local cache
// entries. Call refresh_matter_documents() first so changed source bytes can
// never silently reuse stale text.
std::string build_document_context(
    const std::filesystem::path &workspace_path, const Matter &matter,
    const std::string &current_request = {},
    const std::vector<std::string> &selected_document_ids = {});

// Structured variant retained through PromptAssembler so the request-wide
// budget can distinguish explicit source chunks from retrieved chunks.
// `unavailable_source_documents_out`, when supplied, is appended with every
// real Relevant-mode document this call itself skipped because it could not
// actually be read (Phase 27's source-problem Error) - never the Managed-
// vs-Referenced corpus-scoping exclusion, which is intentional scope, not a
// failure. Left untouched (nullptr) by every caller that does not need it.
// How many passages of an explicitly selected document retrieval returns:
// the fixed relevance window, or every passage for a caller whose prompt is
// bounded by the executing model's resolved capacity (PromptAssembler's
// capacity-first admission).
// RelevanceWithin: the documents were picked for the request (Context/
// SourcePicker), and within them every passage the request genuinely points
// at is a candidate, best first - the same reading a data-room-wide request
// gets, confined to the pick.
enum class SelectedSourceDelivery { RelevanceWindow, EveryPassage, RelevanceWithin };

std::vector<context::RetrievedContextChunk> retrieve_document_context(
    const std::filesystem::path &workspace_path, const Matter &matter,
    const std::string &current_request = {},
    const std::vector<std::string> &selected_document_ids = {},
    std::vector<UnavailableSourceDocument> *unavailable_source_documents_out = nullptr,
    SelectedSourceDelivery delivery = SelectedSourceDelivery::RelevanceWindow);

} // namespace singularity::matters
