#pragma once

#include "Context/QuoteVerification/QuoteVerification.hpp"
#include "Matter/Types/Matter.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Provider/Types/UsageMetadata.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <nlohmann/json_fwd.hpp>
#include <optional>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace singularity::documents {
struct ProfessionalDocumentResult;
}

namespace singularity::routing::matter_request_execution {

matters::Matter run_task(const nlohmann::json &params);

// An operation's checkpoint as this computer keeps it, by digest: its stage,
// its draft and its write-ahead record - all but the lawyer's decision on it,
// which is verified on its own. Noted with every save of the checkpoint
// (Tool/OperationAuthority); a resume from a checkpoint that differs - changed
// in a shared Matter's .lm, or kept on another computer - asks first.
std::string operation_checkpoint_digest(const documents::DocumentOperationCheckpoint &checkpoint);

// The operation works only on this computer (Chat/OperationOrigin): this
// computer recorded it so when it began (Tool/OperationAuthority), the Matter
// states it for one of its tasks - signed or not, a restriction stated only
// keeps work here - or this computer did not begin it and cannot confirm who
// did. Every attempt of it runs on a local model, researches nothing on the
// web, reaches no network with its tools, and its answer is never folded into
// the Matter's memory, which later requests send to the Matter's own models.
bool operation_works_only_here(const matters::Matter &matter, const std::string &operation_id);

// Why work kept here refuses a model outside this computer: it was asked to stay
// here, or this computer cannot confirm who began it.
const char *works_only_here_refusal(const matters::Matter &matter, const std::string &operation_id);

// What a request kept on this computer says when the model it would run on is
// not a local one: nothing was sent.
inline constexpr const char *kWorksOnlyHereModelRefused =
	"This work runs only on this computer, so it was not sent to a model outside it. Nothing was sent. "
	"Choose a model on this computer in Models, then run it again.";

// The same, for work another computer began that this computer cannot confirm
// as its colleague's: who began it, and how it was allowed to run, cannot be
// read from the shared Matter alone.
inline constexpr const char *kOriginUnconfirmedModelRefused =
	"This work began on another computer, and this computer cannot confirm who began it or how it was allowed "
	"to run, so it stays here. Nothing was sent. Confirm that colleague's computer in Team, or choose a model "
	"on this computer in Models, then run it again.";

// Told of every request that was answered successfully, after its result is
// saved and outside the persistence lock: (project id, task id). One listener;
// the application sets it when it starts (the Project's memory keeper), and a
// test or the console simply has none. A listener's failure never reaches the
// request.
using AnsweredObserver = std::function<void(const std::string &project_id, const std::string &task_id)>;
void set_answered_observer(AnsweredObserver observer);

// The one model call that picks a request's documents from a list
// (Context/SourcePicker): the application installs it, because it owns the
// model call - the executing model, reached through Papinian Guard like the
// request itself. With none installed (tests, the console, previews) a
// request that names no document is served by relevance over the data room,
// as before. A picker's failure never reaches the request: it is served as
// before too.
struct SourcePickCall {
	std::string project_id;
	std::string operation_id;
	ModelTarget model;
	std::string prompt;
	providers::GenerationOutputContract contract;
	// The attempt's own protection policy. A re-run never protects less than
	// it did, and the pick is part of the run: it goes out under the same
	// policy as the request it serves, never the weaker live setting.
	std::optional<protected_api::Policy> protected_api_policy;
};
struct SourcePickReply {
	std::string text;
	providers::UsageMetadata usage;
};
using SourcePicker = std::function<SourcePickReply(const SourcePickCall &)>;
void set_source_picker(SourcePicker picker);
// Test-only: called on the running thread the moment a change's approval has
// been put to the lawyer and recorded, before the run returns - where a
// lawyer's quick decision lands (Beta 1 #125, LIFE-1). Never set in the app.
using ApprovalFiledHook = std::function<void(const std::string &project_id, const std::string &operation_id,
	const std::string &approval_id)>;
void set_approval_filed_hook_for_testing(ApprovalFiledHook hook);
nlohmann::json preview_outbound_request(const nlohmann::json &params);
nlohmann::json reveal_protected_occurrence(const nlohmann::json &params);
nlohmann::json close_outbound_preview(const nlohmann::json &params);
matters::Matter retry_task(const nlohmann::json &params);
// Phase 27's "Edit and continue" (Message revision) - never rewrites the
// original user turn. Creates a new attempt in the same retry chain
// (matters::task_group_id()) as `taskId`, honestly a later real operation:
// an omitted "prompt" reuses the original text (a pure source-policy
// revision, e.g. Context limit's "Use Project default"/"Choose fewer
// sources"); an omitted "sourcePolicyMode"/"selectedDocumentIds" reuses the
// original selection. Refuses a Consilium member or workflow step task -
// those chains stay Phase 26/29's own ownership.
matters::Matter revise_task(const nlohmann::json &params);
matters::Matter run_consilium(const nlohmann::json &params);
matters::Matter synthesize_consilium(const nlohmann::json &params);
matters::Matter run_workflow(const nlohmann::json &params);

// Test-only proof surface for Phase 09's "Ask in Chat" - see
// resolve_task_context_for_testing() in the .cpp for exactly what real
// production pipeline this runs (and where it deliberately stops, before
// any network/local-model call).
struct ResolvedTaskContextForTesting {
	// The final selected_document_ids a real task attempt would carry -
	// already resolved through the Chat-inheritance fallback when `params`
	// supplies none of its own.
	std::vector<std::string> selected_document_ids;
	// The distinct Document IDs whose real extracted content chunks actually
	// reached prompt assembly (sorted for deterministic comparison) - the
	// proof that resolution above was not merely stored, but consumed by
	// real local context retrieval (matters::retrieve_document_context()).
	std::vector<std::string> distinct_context_document_ids;
	// The exact canonical Context values admitted by PromptAssembler's real
	// request budget for this operation.
	std::vector<context::ContextItemSnapshot> context_snapshot;
	// The real, fully assembled prompt text this attempt would actually send -
	// exposed only so a test can prove a genuine product boundary (e.g. that
	// another Chat's own history never appears here) by searching for real
	// content, never by re-deriving history selection through a second path.
	std::string assembled_prompt;
	// PreparedTaskRequest::context_budget_excluded_material, exposed only for
	// tests - the same real fact MatterTask::context_budget_excluded_material
	// (and Chat's own contextBudgetOmittedMaterial projection) records.
	bool context_budget_excluded_material = false;
	// What the request carried of Project Context and earlier turns.
	matters::RequestCoverage request_coverage;
	// Every document the request's AdvancedWorkflow candidate may read or change
	// (its target first), empty when no candidate resolved.
	std::vector<std::string> advanced_workflow_document_ids;
	// The schema the request's final answer is held to, and what the request
	// does with what the model writes over protected values: how many rounds
	// of its localFacts questions Singularity answers, and whether every statement
	// in its answer is checked before the answer is taken.
	nlohmann::json final_output_schema;
	std::size_t maximum_local_fact_rounds = 0;
	bool checks_statements = false;
	// The facts step's contract, when the request's first turn asks the model
	// which facts about protected values its answer rests on (null when not).
	nlohmann::json local_facts_step_schema;
};

// The one rule every writer of a canonical operation record follows when it
// saves a task another writer may have changed meanwhile: lifecycle events
// merge, a newer lifecycle state is kept, and a person's recorded approval
// decision is never erased by a writer that does not carry it. Exposed so the
// rule itself can be proven directly, exactly as the executor applies it.
void preserve_concurrent_operation_state(const matters::MatterTask &current, matters::MatterTask &replacement);

// How one operation's usage accumulates across every call it pays for - the
// research turns, the review, the correction, the resumed continuation. What an
// operation cost is the cost of all of them: a call that produced real usage
// but no price, or a price in another currency, makes the total a part of the
// cost rather than the cost, and it is never reported as "actual". Exposed so
// that rule can be proven directly, exactly as every caller applies it.
[[nodiscard]] providers::UsageMetadata accumulate_operation_usage(
	const providers::UsageMetadata &accumulated, const providers::UsageMetadata &addition);

// The model's answer as the lawyer reads it once its corrections are spent:
// every figure it asks for ({{= …}}, {{if …}}) that Singularity still cannot work
// out is left out of it, and one calm sentence after it says which - by the
// words beside it - and why, in the answer's language and in plain words:
// never the wording of the calculation, which is the model's. What works out
// stays as the model wrote it, so the answer is kept with its statements and
// protected operands. An answer with nothing left out is returned as it is.
// Exposed so the rule can be proven directly, exactly as every caller applies it.
[[nodiscard]] std::string answer_with_figures_left_out(const std::string &answer);
// The same, with the answer's fact anchors (restoration's, in `answer`'s
// coordinates) carried by position onto what is kept: one inside a figure
// left out goes with it, every other one stays on its own occurrence.
[[nodiscard]] std::string answer_with_figures_left_out(const std::string &answer,
	std::vector<protected_api::papinian::FactAnchor> &anchors);
// The model's answer under the note that its change was not placed: its
// sentences about an approval still to come taken out, its fact anchors
// carried by position the same way.
[[nodiscard]] std::string answer_without_pending_approval_talk(const std::string &answer,
	std::vector<protected_api::papinian::FactAnchor> &anchors);

// A structured record (DirectDocumentWork) as its document receives it: every
// statement in it worked out, and no operand mark left. When one cannot be,
// `record` is left as it was and the one plain sentence the lawyer reads
// first is returned - that nothing was written, and why: such a record is
// never written. Exposed so the rule can be proven directly.
[[nodiscard]] std::optional<std::string> work_out_record(documents::ProfessionalDocumentResult &record);

// An answer that put a true quotation beside the wrong citation, with the
// citation of the passage the words really stand in written right after the
// quotation's closing mark - unless it already stands there - so the citation
// the lawyer opens beside the quotation is where the words are. The model's
// own citation stays for the rest of its sentence. `quoted` is what
// context::verify_quotations() found (only its re-bound quotations are
// touched); `references` are the operation's recorded sources, and a
// quotation whose passage has no reference there, or whose marker the
// operation also records for another passage, is left as written.
[[nodiscard]] std::string cite_rebound_quotations(const std::string &response,
	const std::vector<context::QuotedPassage> &quoted,
	const std::vector<context::ResolvedSourceReference> &references);

// Why a change this request proposes waits for the lawyer whatever the
// Project's action mode, read from the lawyer's own words in it - their
// instruction and every answer they typed to its questions - never from the
// model's account of them: "asked_first" when they asked to see changes first
// (tools::asks_to_see_changes_first), "not_requested" when `requested_by` - the
// words the proposal says asked for it - is not theirs (tools::quotes_lawyer),
// empty when the change may go ahead as the mode allows. Exposed so the floor
// can be proven directly, exactly as the executor applies it.
[[nodiscard]] std::string approval_floor(const matters::MatterTask &task, const std::string &requested_by);

// True when `instruction` only confirms the proposal it follows and asks for
// nothing new - "Go ahead and create the amendment.", "Apply it.", "Да,
// создавай." - so the change the lawyer was just shown is put to them again
// instead of being drafted anew. Every word of it must be a word of
// confirmation, a word with no request in it ("please", "the", "it") or a word
// the proposal itself already uses (`proposal_words`: its request, title,
// summary and file name); one word more - any figure at all, "but", "only", a
// new term - and the instruction is a new request. So is one that sends the
// change somewhere else (`destination`): a separate document for a change to a
// document, another of the Project's documents, or an existing document for a
// change that makes a new one ("Add it to the MSA."). Exposed so the reading
// can be proven directly, exactly as the executor applies it.
struct ProposalDestination {
	// The change makes a new document, under `new_document_name`; otherwise it
	// changes the document named `target_document_name`.
	bool creates_new_document = false;
	std::string new_document_name;
	std::string target_document_name;
	// The names of every other document in the Project.
	std::vector<std::string> other_document_names;
};
[[nodiscard]] bool only_confirms_proposal(const std::string &instruction, const std::string &proposal_words,
		const ProposalDestination &destination = {});

ResolvedTaskContextForTesting resolve_task_context_for_testing(const nlohmann::json& params,
		const std::string& prompt, const std::filesystem::path& workspace_path, const matters::Matter& matter);

} // namespace singularity::routing::matter_request_execution
