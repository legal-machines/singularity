#pragma once

#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "Routing/Types/ModelTarget.hpp"
#include "Routing/ProtectedInference/ProtectedInference.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "Provider/Types/InputPictures.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBudget.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "LocalModel/ModelLifecycle/ModelLifecycle.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"
#include "Tool/Types/ToolTypes.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "ExternalResearch/TurnContract/TurnContract.hpp"
#include "Routing/Observability/ExecutionProgress.hpp"

#include <algorithm>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace singularity::routing {

// The single process-wide, hash-pinned, lazily-loaded Papinian Guard every
// real inference request runs through (see execute() below). Exposed so any
// other caller that wants to demonstrate what Protected API actually does to
// text - e.g. the Settings/onboarding preview - runs the exact same engine
// instead of a separate, lesser stand-in.
const protected_api::papinian::PapinianGuard &production_guard();

// A fresh 24-hex-character request-scoped namespace (Master Spec Sec.44/45),
// exposed for the same reason as production_guard() above.
std::string generate_request_namespace();

// Tracks whether Papinian's semantic model has finished loading. NativeBridge
// submits papinian_warmup_tracker().run_once() to its BackgroundExecutor once
// at app launch instead of waiting for the first real request to trigger
// VerifiedGliner2Runtime's lazy load (see gliner2_runtime.cpp) - this makes a
// broken/missing model package fail fast and visibly instead of on whatever
// request happens to touch Protected API first. Purely additive: does not
// change the fail-closed guarantee already enforced inside protect() itself.
ModelWarmupTracker &papinian_warmup_tracker();
// How long the last finished warm-up of the installed Papinian version took
// on this computer, read from its benchmark record when the running warm-up
// began - what a loading status reports as expected. std::nullopt on the
// first warm-up of a model version here, or before a warm-up has begun.
std::optional<std::uint64_t> papinian_expected_warm_up_ms();
// The launch sequence NativeBridge runs off the UI thread: an installed
// package is warmed up at once (the warm-up verifies it) and synced after; a
// missing one is fetched first. A package the sync had to repair is loaded
// again. Never throws; outcomes are in the tracker and package status.
void prepare_papinian_model_at_launch();
// User-facing lifecycle controls. load clears a previous sticky runtime
// failure before retrying the newly installed package; unload releases the
// engine while leaving the verified files installed.
void load_papinian_model();
void unload_papinian_model();
// Protected operations call this before touching the guard so an explicitly
// unloaded model cannot be loaded again as an accidental inference side effect.
void require_papinian_model_ready(const protected_api::Policy &policy);
// True while the launch warm-up (or a retry after repair) is still loading
// the model. A read that only reports readiness - a Privacy overview, a
// Document list's own preprocessing pass, the composer's protection state -
// checks this first and reports "loading" instead of calling
// require_papinian_model_ready(), which would join the warm-up and hold a
// bridge worker for the whole load. Real protected requests keep waiting
// for the guard through require_papinian_model_ready(), unchanged.
bool is_papinian_model_loading();

// Real, measured evidence from Papinian's most recent warm-up (whichever
// backend actually ran it) compared against that backend's own previously
// recorded baseline - std::nullopt before the very first warm-up of this
// process has completed. See LocalModel/ResourceBenchmark/ResourceBenchmark.hpp's
// run_resource_benchmark()/record_and_compare_benchmark(). Mirrors exactly
// what production_local_model_registry()'s live state already carries for
// this model, exposed directly for callers that only need Papinian's.
std::optional<hardware::BenchmarkComparison> papinian_last_resource_benchmark();

// The catalogue of local AI modules this build knows about - one real entry
// (Papinian NER) today, but a general LocalModelRegistry so a future module
// (e.g. Gaius) is just another registered descriptor, not a UI redesign.
// Exposed for the papinian.getModelStatus/localModels.list native bridge
// handlers; not consulted by protect()/execute() themselves.
hardware::LocalModelRegistry &production_local_model_registry();

// The local model to ask for small housekeeping on this Mac (a chat's name, the
// Project's memory): the generative model that is already loaded if there is
// one, otherwise the smallest one installed - which the governed path then loads.
// Papinian's own extraction encoder cannot write and is never a candidate.
// std::nullopt when no generative model is installed.
[[nodiscard]] std::optional<std::string> preferred_local_generative_model();

// Node-owned package id -> exact installed artifact path supplied to the Singularity
// adapter. The public local runtime intentionally exposes model ids but not
// backing paths; package ownership remains in Node and is the deletion truth.
std::map<std::string, std::filesystem::path> production_generative_install_paths();

struct InferenceRequest {
  std::string prompt;
	// The pictures the member attached, sent with every model call this request
	// makes as image inputs; none for a request that carries words only.
	std::shared_ptr<const std::vector<providers::InputPicture>> pictures;
	// The prompt in parts, when the caller gives it so (a colleague's talk):
	// sent as those parts, each protected on its own (ModelVisibleTurn.hpp);
	// `prompt` is then their joined text, for what is measured of it.
	std::vector<std::string> prompt_parts;
	// Singularity's own instruction beside the parts (a colleague's talk's
	// rules): the turn's machine-owned instruction, never scored or rewritten
	// as data (ModelVisibleTurn.hpp).
	std::string prompt_instruction;
	// How long a reasoning model thinks on this request, where it takes it:
	// a short think for a conversational answer.
	providers::ReasoningEffort reasoning_effort = providers::ReasoningEffort::Default;
	// A colleague's talk: its protection legend speaks a conversation's words
	// (ModelVisibleTurn::is_conversation).
	bool is_conversation = false;
	// The bounded product reason for this request's first real model call.
	// Every execution boundary supplies one (the default is the ordinary main
	// answer), and the executor emits it before dispatch. Later loop turns own
	// their more specific research/repair/correction purposes.
	observability::CallPurpose call_purpose = observability::CallPurpose::SubstantiveGeneration;
  std::optional<std::string> workspace_id;
  // Overrides LocalNodeSetup::default_model for this one request, when set.
  std::optional<ModelTarget> model_target;
	// Operation-level generation budget. Context automation needs enough room
	// to close its strict JSON object; ordinary calls keep the established
	// backend defaults when this is unset.
	std::optional<std::uint64_t> maximum_output_tokens;
  // Overrides the live LocalNodeSetup::protected_api_policy for this one
  // request, when set. Existing purely so Retry (Master Spec Sec.66) can
  // pin a request to the Protected API mode the original task actually ran
  // under instead of silently picking up a policy change made since - see
  // matters::MatterTask::protected_api_mode and native_bridge.cpp's
  // retry_matter_task(). Every other caller leaves this unset and gets the
  // current live policy, unchanged from before this field existed.
  std::optional<protected_api::Policy> protected_api_policy_override;
	// Same Project identity authority used by ContextItem.entityId. These
	// hints annotate the ephemeral restoration map only; they never become
	// provider-visible placeholder text.
	std::vector<ProjectEntityProtectionHint> project_entity_hints;
	// Trusted Singularity scope for a model/tool loop. The model never constructs
	// or alters this value; ordinary no-tool inference simply leaves it unset.
	std::optional<tools::ToolExecutionContext> tool_execution_context;
	// The operation the request belongs to, for a request with no tool context:
	// its authority record (Tool/OperationAuthority) says where the work may run,
	// and the executor reads it whatever model the caller chose
	// (is_local_only_work, Routing/InferenceExecutor/LocalOnlyWork.hpp).
	struct AuthorityOperation {
		std::string project_id;
		std::string operation_id;
	};
	std::optional<AuthorityOperation> authority_operation;
	research::ResearchMode research_mode = research::ResearchMode::Auto;
	// Optional domain-owned final-text syntax requirement. The research loop
	// may request one format correction; domain semantics and mutations remain
	// outside inference. Transient request state, never a persisted callback.
	std::function<bool(const std::string &)> accepts_research_final_text;
	// Optional domain-owned semantic preflight. A returned reason keeps the
	// same model inside the same bounded ResearchToolLoop so it can open a
	// discovered source or revise an unsupported claim before any mutation.
	std::function<std::optional<research::FinalTextIssue>(const std::string &,
		const std::vector<research::ExternalEvidenceReference> &)> research_final_text_issue;
	std::string research_final_text_instruction;
	// Model-native research escalation: the typed external research request a
	// validated final text declares (web.search's own argument object), if any.
	// See ResearchToolLoop.hpp's finalTextResearchRequest.
	std::function<std::optional<nlohmann::json>(const std::string &)> research_request_from_final_text;
	// Model-native Project inspection: the read-only Project tool calls a
	// validated final text's own request for more of the Project comes to
	// ([{"toolId", "arguments"}], empty when it asks for none), and how many such
	// requests one call may serve. See ResearchToolLoop.hpp's
	// finalTextProjectEvidenceCalls: the reads run through ToolBroker
	// (capability project.read) and are answered by
	// tool_execution_context->project_evidence.
	std::function<nlohmann::json(const std::string &)> project_evidence_calls_from_final_text;
	std::size_t maximum_project_evidence_requests = 0;
	// A question to the lawyer in the middle of the run: the lawyerQuestion a
	// validated final text carries, when it asks anything, and how many more
	// questions this call may still put. See ResearchToolLoop.hpp's
	// finalTextLawyerQuestion: the question waits in ToolBroker (lawyer.ask)
	// for the lawyer's answer, and the same operation continues with it.
	std::function<std::optional<nlohmann::json>(const std::string &)> lawyer_question_from_final_text;
	std::size_t maximum_lawyer_questions = 0;
	// Facts on demand: the questions about protected values a validated final
	// text asks Singularity to work out on this computer (its localFacts), empty
	// when it asks none, and how many rounds of them one call may answer. See
	// ResearchToolLoop.hpp's finalTextLocalFacts: the answers are relations,
	// or new symbols for derived values - never a value itself.
	std::function<std::vector<context::computed_statements::FactQuestion>(const std::string &)> local_facts_from_final_text;
	std::size_t maximum_local_fact_rounds = 0;
	// The facts step: the contract and instruction of a first turn in which the
	// model names the facts about protected values its answer will rest on,
	// answered here before the answer turn (ResearchToolLoop.hpp's
	// localFactsStepContract). Sent only where the call really goes out
	// protected - a provider's model under a protection policy; a model on this
	// computer reads the values themselves.
	std::optional<providers::GenerationOutputContract> local_facts_step_contract;
	std::string local_facts_step_instruction;
	// Told each round of local facts the call answered, as the model read it
	// (ResearchToolLoop.hpp's localFactsAnswered): the task keeps them for its
	// later calls, so a review or a redraft reads each derived symbol with what
	// it was worked out from. The values inside the answers' marks are the
	// lawyer's: process memory only, never serialized.
	std::function<void(const std::string &)> local_facts_answered;
	// One task's protection identity across all of its calls - the draft, its
	// professional review, the corrections and every loop turn - so one value
	// is one symbol in every call of the task. Unset, execute() keeps a table
	// of its own for the one call. Process memory only, never serialized.
	std::shared_ptr<protected_api::papinian::OperationProtectedSymbolTable> operation_symbols;
	std::optional<providers::GenerationOutputContract> research_final_output_contract;
	// Optional operation-local incremental work tool. ResearchToolLoop owns
	// scheduling/bounds; the domain callback owns validation and draft state.
	std::optional<providers::ModelToolDefinition> operation_model_tool;
	std::string operation_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &)> operation_tool_handler;
	std::optional<providers::ModelToolDefinition> operation_research_request_model_tool;
	std::string operation_research_request_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_research_request_handler;
	std::function<void(const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_research_completed_handler;
	// document.inspect: a third operation-tool slot, structurally identical
	// to operation_research_request_* above, that reveals one collapsed
	// document table's real structural items instead of transitioning to web
	// research - see ConnectedDocumentAction.cpp's compact-context path,
	// used only when the resolved model's real capacity could not fit the
	// full Project structure up front. Left unset by every ordinary/
	// full-context request, exactly like the research-request slot was
	// before Phase 24.1D wired it in.
	std::optional<providers::ModelToolDefinition> operation_inspect_model_tool;
	std::string operation_inspect_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_inspect_handler;
	// Domain-owned lossless structural compaction for a typed operation turn.
	// ResearchToolLoop invokes it only after the complete provider-facing turn
	// is known not to fit and repeats until the turn fits or no smaller unit
	// exists. It never summarizes or drops semantic evidence itself.
	std::function<bool()> operation_context_compaction_handler;
	// Model-authored semantic mapping from the user's requested deliverables to
	// runtime-issued editable Body handles. Once accepted, the domain persists
	// and enforces the typed agenda; inference never decides completion itself.
	std::optional<providers::ModelToolDefinition> operation_output_agenda_model_tool;
	std::string operation_output_agenda_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_output_agenda_handler;
	// Model-driven declaration of which named candidate document(s), if any,
	// are required source evidence (THE MODEL OWNS THE JUDGMENT) - see
	// ConnectedDocumentAction.cpp's define_connected_document_evidence_agenda().
	std::optional<providers::ModelToolDefinition> operation_evidence_agenda_model_tool;
	std::string operation_evidence_agenda_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_evidence_agenda_handler;
	// Advances one named document's own table-map page (THE RUNTIME OWNS HOW
	// MANY FIT, NEVER WHICH MATTERS) - see ConnectedDocumentAction.cpp's
	// request_connected_document_table_map_next_page().
	std::optional<providers::ModelToolDefinition> operation_map_next_model_tool;
	std::string operation_map_next_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_map_next_handler;
	// The Professional Completion Challenge's own typed verdict submission
	// (THE MODEL OWNS THE JUDGMENT of professional completeness; SINGULARITY OWNS
	// ROUTING THE VERDICT) - see ConnectedDocumentAction.cpp's submit_
	// connected_document_professional_review().
	std::optional<providers::ModelToolDefinition> operation_professional_review_model_tool;
	std::string operation_professional_review_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_professional_review_handler;
	// Reveals one currently-collapsed methodology slide's own full text (THE
	// MODEL OWNS WHICH SLIDE IT NEEDS; SINGULARITY OWNS KEEPING THE REST BOUNDED)
	// - see ConnectedDocumentAction.cpp's reveal_connected_document_
	// methodology().
	std::optional<providers::ModelToolDefinition> operation_methodology_reveal_model_tool;
	std::string operation_methodology_reveal_canonical_tool_id;
	std::function<std::pair<bool, std::string>(const nlohmann::json &,
		const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &)> operation_methodology_reveal_handler;
	// Rebuilds each disposable model turn from canonical operation state. The
	// returned string is the full next prompt; accepted payloads remain local.
	std::function<std::string(const std::vector<research::ExternalEvidenceReference> &,
		const std::optional<research::FinalTextIssue> &)> operation_context_compiler;
	// The authoritative per-turn projection. ResearchToolLoop compiles context,
	// tools, choice policy, fallback grammar, handles and validation from this
	// same immutable snapshot; no boundary independently guesses the phase.
	std::function<research::OperationStateProjection(
		const std::vector<research::ExternalEvidenceReference> &,
		const std::optional<research::FinalTextIssue> &)> operation_state_compiler;
	// Called at safe state transitions so the enclosing canonical MatterTask
	// can checkpoint draft/evidence without persisting Papinian mappings. The
	// third argument is what the provider has already billed for this operation,
	// so a crash or a quit between turns never loses usage that was really
	// charged.
	std::function<void(const std::vector<research::ExternalEvidenceReference> &,
		const research::OperationExecutionProgress &,
		const providers::UsageMetadata &)> operation_checkpoint_handler;
	std::vector<research::ExternalEvidenceReference> operation_initial_external_evidence;
	research::OperationExecutionProgress operation_initial_progress;
	std::string operation_completion_text;
	// The protected computation protocol (DESIGN4 §1.1, §6.2). The tables of
	// the documents a prompt names, read whole on this computer (Routing/
	// ProtectedTableSource), for the local table model; and the budgets the
	// protocol runs under. Both unset: the protocol is not offered and the
	// request runs exactly as before it existed.
	std::function<std::vector<protected_api::papinian::tables::TableInput>(const std::vector<std::string> &)>
		protected_table_source;
	std::optional<protected_api::papinian::compute::ComputeBudgetLimits> compute_budget;
};

struct InferenceResult {
  std::string text;
  std::string provider_id;
  std::string model_id;
	std::string execution_kind;
	std::string runtime_id;
	std::string runtime_version;
	std::string runtime_source_commit;
	std::string runtime_platform;
	std::string runtime_backend;
	std::string runtime_model_id;
	std::string runtime_telemetry_json;
  // Whether Protected API's policy was engaged for this request (its mode
  // was not Off) - not whether anything was actually found to replace, a
  // prompt with nothing sensitive in it is still routed under the active
  // policy.
  bool protected_api_applied = false;
  protected_api::papinian::PrivacyMetadata privacy_metadata;
  providers::UsageMetadata usage;
  std::optional<std::string> finish_reason;
	// Set (only ever alongside finish_reason=="length"/"max_tokens") when this
	// turn was compiled with ModelVisibleTurn::request_semantic_completion and
	// still genuinely exhausted the real physical boundary (local: local inference's
	// own real remaining-context measurement; cloud: the provider's
	// own documented maximum completion allowance) - a true physical
	// boundary, never a semantic verdict. False for every turn that did not
	// request semantic completion (finish_reason=="length" keeps its
	// pre-existing meaning: the caller's own configured ceiling was reached).
	bool physical_context_exhausted = false;
	// Set when the turn asked for computable values
	// (ModelVisibleTurn::needs_computable_values) and protection left none in
	// what would have been sent: no provider call was made, `text` is empty and
	// nothing was used or recorded as sent.
	bool provider_call_skipped = false;
	// Real, authoritative per-generation physical timing/reuse facts from the
	// LOCAL provider only (local inference's own real llama.cpp timings - see
	// local_inference::GenerativeResult's own comment) - never estimated in Singularity,
	// never populated for a cloud provider. std::nullopt when this
	// execution did not go through the local provider at all (never
	// fabricated as "measured"). Forensic/observability use only -
	// nothing in ResearchToolLoop.hpp's own turn/objective/dependency
	// decisions may ever read these.
	std::optional<double> local_prompt_ms;
	std::optional<double> local_generation_ms;
	std::optional<std::uint64_t> local_evaluated_prompt_tokens;
	std::optional<bool> local_prefix_cache_used;
	// Every amount or date restoration wrote into `text`, with the fact it is
	// (ProtectedGenerationResult::fact_anchors): offsets and digests only.
	std::vector<protected_api::papinian::FactAnchor> fact_anchors;
	// The answer check of a protected generation (ProtectedGenerationResult::
	// untraced_figures): how many figures its text writes that no symbol,
	// source or local operation gives, or results other than an operation's.
	// A count only.
	std::size_t untraced_figures = 0;
	// What the lawyer is told beside the answer, never in it (DESIGN3 9.8): the
	// figures still untraced when the final answer was taken - after its one
	// correction, or when the correction could not be sent - and those a
	// written tool call carried. A count only; MatterRequestExecution turns it
	// into the chat's status line (ControlText FigureUnresolved).
	std::size_t unresolved_figures = 0;
	// The final answer contract of the last protected generation (DESIGN4 §9;
	// ProtectedGenerationResult::answer_contract): what failed, by code and
	// symbol, and what the lawyer's answer left out or shows as a gap. The
	// status line for figures left out is withheld_figures_status().
	AnswerContractFindings answer_contract;
	std::vector<tools::ToolCallEvidence> tool_call_evidence;
	std::vector<research::ExternalEvidenceReference> external_evidence;
	// Canonical JSON of every model-declared external research request whose
	// web.search actually executed in this generation (model-native research
	// escalation) - so an open request on the final can be told apart from
	// one that was served (see MatterRequestExecution.cpp's limitation rule).
	std::vector<std::string> fulfilled_research_requests;
	// Real provider-native tool calls this one generation turn returned (see
	// providers::GenerationResult::tool_calls, threaded through
	// ProtectedGenerationResult), argument string leaves already restored
	// when Papinian masked something. Transient, internal-loop state only -
	// meaningful solely as ResearchToolLoop.hpp's ResearchGeneration return
	// value inside run_research_tool_loop(); every InferenceResult that loop
	// actually returns to a caller outside itself has this cleared before
	// return (a raw restored tool-call argument may briefly hold real,
	// unminimized Project values in memory and must never reach a persisted
	// MatterTask). Empty for every ordinary, non-research execute() call.
	std::vector<providers::ModelToolCall> tool_calls;
};

// Resolves the same production workspace/model/provider/credential/privacy
// prerequisites as execute(), without generating or sending anything. Domain
// orchestrators use this to expose truthful availability without duplicating
// routing, credential, local-runtime, or Papinian policy.
void require_execution_ready(const InferenceRequest &request);

// The protection a request to `provider_id` runs under, as its routing record
// must say it even when it fails: "off" for a model on this computer, whose
// turns never leave it, else the mode of the policy execute() holds it to -
// the request's own (a Retry pins it), else this computer's.
std::string protection_mode_of(const std::string &provider_id, const InferenceRequest &request);

// Read-only capability query for a model target, independent of execute()'s
// full resolution (credentials, Protected API policy, tool capability
// flags). Exists so a caller that must decide BEFORE building an
// InferenceRequest how much real capacity a resolved model has - see
// MatterRequestExecution.cpp's document-inspection context-plan decision,
// which must know this ahead of prepare_connected_document_action() so it
// never has to duplicate execute()'s own provider/model lookup or the local
// Singularity fallback catalog (ExternalResearch/ContextBudget.hpp's
// known_model_context_window_tokens()). `target` follows the same
// nullopt-means-current-default-reasoning-assignment convention as
// InferenceRequest::model_target. Returns std::nullopt only when neither a
// discovered value nor a known fallback exists for this exact target -
// callers must never guess a number themselves in that case.
[[nodiscard]] std::optional<std::size_t> estimate_model_context_window_tokens(
	const std::optional<ModelTarget> &target);

// What a model can do, read from the data its provider or Singularity gave - never
// from a table kept in this code: the context it reads, the most it writes in
// one answer (its thinking included), what it charges, and when the figures
// were read. A figure no source gave is unset.
struct ModelCapability {
	std::optional<std::uint64_t> context_window_tokens;
	std::optional<std::uint64_t> max_output_tokens;
	std::optional<double> input_price_per_token;
	std::optional<double> output_price_per_token;
	std::string discovered_at;
	// "provider_reported" (the provider's model list), "local" (local inference's figures
	// for this computer), or "unknown" when no source gave any.
	std::string source = "unknown";
	// Whether the model reads pictures; std::nullopt where nothing says.
	std::optional<bool> takes_pictures;
};

// The capability of `target`: for a provider's model, the figures its model
// list reported when it was last read (Provider/ModelClassification); for a
// local model, the context Singularity grants or plans on this computer, and half
// of it as the most one answer writes (ExternalResearch/ContextBudget's rule).
[[nodiscard]] ModelCapability model_capability(const ModelTarget &target);

// Why web research cannot run right now for an operation with this tool
// context - its research backend's provider is not connected or has no
// credential, or Papinian (which minimizes every research query under a
// protection policy) is unloaded or failed - or std::nullopt when it can.
// Asked while a request is prepared, so an unavailable capability is stated to
// the model and in the result instead of being offered and failing mid-task.
[[nodiscard]] std::optional<std::string> research_unavailable_reason(const tools::ToolExecutionContext &context);

// One web step of an AI colleague's own work: a search the colleague wrote,
// run through the research tools as a chat's searches are and held to the
// protection its calls are held to - `policy`, and whether its model reads
// only protected text. Its last check before anything leaves refuses a
// [[symbol]], a value this work protected and one of the Matter's identities
// (ResearchTools' prepare_outbound_research_request). It is put to no person
// first: a colleague searches as a colleague does, and a firm whose settings
// allow no web access still refuses it. The broker's attempt, as
// ToolBroker::request gives it.
[[nodiscard]] tools::ToolAttempt colleague_web_step(const tools::ToolExecutionContext &context,
	const tools::ToolIntent &intent, const protected_api::Policy &policy, bool writtenFromProtectedText);

// While an agent that works only on this computer runs its task on this
// thread (Agent/AgentProduction, AgentModelPolicy::LocalOnly: "I work on this
// computer"), no tool of that task reaches the network: web research is never
// offered - research_unavailable_reason() says why, so the request runs
// without it and says so - and execute() withholds every ExternalNetwork tool
// from the request's tool context. Inactive (`active` false) it changes
// nothing. Header-inline: any build may set it without linking this module.
inline constexpr const char *kLocalOnlyWorkResearchUnavailable =
	"This agent works only on this computer, so it does not search the web.";

class ScopedLocalOnlyWork {
public:
	explicit ScopedLocalOnlyWork(bool active = true) : previous_(slot()) {
		if (active) slot() = true;
	}
	~ScopedLocalOnlyWork() { slot() = previous_; }
	ScopedLocalOnlyWork(const ScopedLocalOnlyWork &) = delete;
	ScopedLocalOnlyWork &operator=(const ScopedLocalOnlyWork &) = delete;
	[[nodiscard]] static bool is_active() { return slot(); }

private:
	static bool &slot() {
		thread_local bool value = false;
		return value;
	}
	bool previous_;
};

// The outbound-only counterpart to InferenceResult - Phase 08 WP8-B ("What
// leaves this device?", Master Spec Sec.54). Carries no response text - see
// preview() below, which never lets a request reach the network.
struct PreviewResult {
  std::string provider_id;
  std::string model_id;
  bool protected_api_applied = false;
  protected_api::papinian::PrivacyMetadata privacy_metadata;
  // The exact provider-facing text (already Papinian-masked when
  // protection is active) - identical to what InferenceRequest::prompt
  // would produce as the request's message content for a real execute()
  // call with the same input.
  std::string outbound_text;
  // The exact serialized HTTP request body a real send would have
  // transmitted (Master Spec Sec.92's own "recording transport" language).
  std::string outbound_request_body;
	// The protection `outbound_text` was built with (OutboundPreviewCaptured::
	// protection): the prompt as it leaves and the mapping behind its symbols,
	// for the preview's presentation and its reveal session. Native memory
	// only - never serialized, never returned to the frontend.
	protected_api::papinian::ProtectionResult protection;
};

// The one place a workspace, a default model, a connected provider's
// secret, and Protected API's policy come together to actually run a
// prompt - see docs' pipeline: resolve workspace -> resolve model -> load
// secret -> apply Protected API -> call the provider -> restore -> return.
//
// Throws std::runtime_error with a plain-language message for every
// resolvable failure (no workspace, no default model, model no longer
// offered by its provider, missing secret, provider-side error) - there is
// no partial InferenceResult to return on failure.
InferenceResult execute(const InferenceRequest &request);

// Runs through the exact same resolution and protection steps execute()
// does - workspace/model/provider/secret/policy resolution, Papinian Guard,
// the final outbound leak guard - then captures the exact request instead
// of transmitting it (Master Spec Sec.54 "What leaves this device?").
// Throws exactly like execute() for every resolution failure, and for a
// leak-guard block: a request unsafe to send is unsafe to preview, so it
// fails the identical way. Makes no network call under any circumstance.
PreviewResult preview(const InferenceRequest &request);

// The answer check of a protected call (DESIGN3 9.8 b-c), on top of the
// caller's own final-text check: when the caller finds nothing, a final text
// whose generation wrote a figure no symbol, source or local operation gives
// - or restated an operation's result as another symbol - goes back once
// (ResearchToolLoop: correction_key "untraced_figure"), with Singularity's own
// words (ControlText UntracedFigure). `last_untraced` is the count the last
// generation's protection found; it is read when the check runs. An empty
// text (an operation's completion probe) is the caller's alone.
inline std::function<std::optional<research::FinalTextIssue>(const std::string &,
	const std::vector<research::ExternalEvidenceReference> &)>
with_figure_check(std::function<std::optional<research::FinalTextIssue>(const std::string &,
		const std::vector<research::ExternalEvidenceReference> &)> caller, const std::size_t &last_untraced) {
	return [caller = std::move(caller), &last_untraced](const std::string &text,
			const std::vector<research::ExternalEvidenceReference> &evidence) -> std::optional<research::FinalTextIssue> {
		if (caller) {
			std::optional<research::FinalTextIssue> issue = caller(text, evidence);
			if (issue || text.empty()) return issue;
		}
		if (text.empty() || last_untraced == 0) return std::nullopt;
		namespace control = context::control_text;
		research::FinalTextIssue issue;
		issue.kind = research::FinalTextIssueKind::CorrectionOnly;
		issue.correction_key = "untraced_figure";
		issue.message = control::render(control::Template::UntracedFigure,
			{{control::Slot::Kind::Count, std::to_string(std::min<std::size_t>(last_untraced, 999))}});
		return issue;
	};
}

// The repair message of the answer contract (DESIGN4 §9.2), in Singularity's own
// words (ControlText AnswerContract*): the header, one line per check that
// failed - its code, then the symbol it concerns as its local anchor, which
// protection sends as the symbol again - and the footer. A check that failed
// for one symbol twice is one line; a symbol the contract does not name (a
// figure of the model's own, a symbol never given) is never written. Each soft
// nudge is given once per task (`nudged`), alone when nothing else failed.
// Empty when there is nothing to say.
inline std::string answer_contract_message(const AnswerContractFindings &findings, std::set<std::string> &nudged) {
	namespace control = context::control_text;
	std::string lines;
	std::size_t failures = 0;
	std::set<std::pair<std::string, std::string>> listed;
	for (std::size_t index = 0; index < findings.violations.size(); ++index) {
		const auto &violation = findings.violations[index];
		if (violation.soft || !control::fits(control::Slot::Kind::Code, violation.code)) continue;
		const std::string symbol = index < findings.local_symbols.size() ? findings.local_symbols[index] : std::string();
		if (!listed.insert({violation.code, symbol.empty() ? std::to_string(violation.offset) + violation.json_pointer : symbol}).second)
			continue;
		++failures;
		if (failures > 24) continue;
		lines += control::render(control::Template::AnswerContractViolation, {{control::Slot::Kind::Code, violation.code}})
			+ symbol + "\n";
	}
	std::string message;
	if (failures > 0)
		message = control::render(control::Template::AnswerContractHeader,
				{{control::Slot::Kind::Count, std::to_string(std::min<std::size_t>(failures, 999))}})
			+ lines + control::render(control::Template::AnswerContractFooter);
	for (const auto &violation : findings.violations) {
		if (!violation.soft || nudged.count(violation.code) > 0) continue;
		if (violation.code == "compute_not_attempted") {
			nudged.insert(violation.code);
			message += control::render(control::Template::AnswerContractNudgeCompute);
		} else if (violation.code == "unchecked_cross_source") {
			nudged.insert(violation.code);
			message += control::render(control::Template::AnswerContractNudgeCrossSource);
		}
	}
	return message;
}

// The answer contract of a protected call (DESIGN4 §9.2), on top of the
// caller's own final-text check - what replaces with_figure_check: when the
// caller finds nothing, a final text whose generation failed the contract goes
// back with the answer_contract_message (ResearchToolLoop: correction_key
// "answer_contract", bounded by kAnswerContractRepairs; a nudge alone:
// "answer_contract_nudge", given once). `last` is the last generation's
// findings, read when the check runs. An empty text (an operation's
// completion probe) is the caller's alone. When no repair is left, the
// generation's text is already the answer with what failed left out
// (AnswerContractFindings::text_withheld).
inline std::function<std::optional<research::FinalTextIssue>(const std::string &,
	const std::vector<research::ExternalEvidenceReference> &)>
with_answer_contract(std::function<std::optional<research::FinalTextIssue>(const std::string &,
		const std::vector<research::ExternalEvidenceReference> &)> caller, const AnswerContractFindings &last) {
	auto nudged = std::make_shared<std::set<std::string>>();
	return [caller = std::move(caller), &last, nudged](const std::string &text,
			const std::vector<research::ExternalEvidenceReference> &evidence) -> std::optional<research::FinalTextIssue> {
		if (caller) {
			std::optional<research::FinalTextIssue> issue = caller(text, evidence);
			if (issue || text.empty()) return issue;
		}
		if (text.empty()) return std::nullopt;
		std::string message = answer_contract_message(last, *nudged);
		if (message.empty()) return std::nullopt;
		research::FinalTextIssue issue;
		issue.kind = research::FinalTextIssueKind::CorrectionOnly;
		issue.correction_key = last.failures > 0 ? "answer_contract" : "answer_contract_nudge";
		issue.message = std::move(message);
		return issue;
	};
}

// The chat's status line for figures an answer left out (DESIGN4 §9.2;
// ControlText FigureWithheld*): outside the work product, never a figure.
// Empty for none.
inline std::string withheld_figures_status(std::size_t figures) {
	namespace control = context::control_text;
	if (figures == 0) return {};
	if (figures == 1) return control::render(control::Template::FigureWithheldOne);
	return control::render(control::Template::FigureWithheldMany,
		{{control::Slot::Kind::Count, std::to_string(std::min<std::size_t>(figures, 999))}});
}

// A PromptAssembler prompt with the content of its instruction sections taken
// out - Singularity's own words, which protection never reads as data
// (ProtectedInference's semantic_leaf_spans: SYSTEM, TASK, CAPABILITY,
// SOURCE_COVERAGE, TEMPORAL_CONTEXT, REQUEST_COVERAGE) - and what was taken,
// to write back unchanged. The statement instruction's examples of the
// symbols Singularity issues ([[Amount3]]) are neither symbols of the request nor
// figures it writes openly.
struct InstructionSections {
	std::string without;
	std::vector<std::pair<std::string, std::string>> contents;  // section name -> its content
};

inline InstructionSections without_instruction_sections(const std::string &prompt) {
	static constexpr const char *kInstructionSections[] = {"SYSTEM", "TASK", "CAPABILITY", "SOURCE_COVERAGE",
		"TEMPORAL_CONTEXT", "REQUEST_COVERAGE"};
	InstructionSections sections;
	sections.without = prompt;
	for (const char *name : kInstructionSections) {
		const std::string opening = std::string("<<<LM_SECTION:") + name + ">>>\n";
		const std::string closing = std::string("\n<<<LM_END_SECTION:") + name + ">>>";
		const std::size_t open = sections.without.find(opening);
		if (open == std::string::npos) continue;
		const std::size_t from = open + opening.size();
		const std::size_t close = sections.without.find(closing, from);
		if (close == std::string::npos) continue;
		sections.contents.emplace_back(name, sections.without.substr(from, close - from));
		sections.without.erase(from, close - from);
	}
	return sections;
}

inline std::string with_instruction_sections(std::string text, const InstructionSections &sections) {
	for (const auto &[name, content] : sections.contents) {
		const std::string empty = "<<<LM_SECTION:" + name + ">>>\n\n<<<LM_END_SECTION:" + name + ">>>";
		const std::size_t at = text.find(empty);
		if (at == std::string::npos) throw std::runtime_error("the " + name + " section was lost");
		text.insert(at + name.size() + 18, content);
	}
	return text;
}

// The localFacts executor of a protected call, reading the loop's prompt as
// protection does: its data, never Singularity's instructions - where an example
// symbol would read as one the table never issued, and an example figure
// ("3%") as one the request writes openly.
inline std::function<std::string(const std::vector<context::computed_statements::FactQuestion> &, const std::string &)>
over_data_plane(std::function<std::string(const std::vector<context::computed_statements::FactQuestion> &,
		const std::string &)> executor) {
	if (!executor) return executor;
	return [executor = std::move(executor)](const std::vector<context::computed_statements::FactQuestion> &questions,
			const std::string &prompt) { return executor(questions, without_instruction_sections(prompt).without); };
}

} // namespace singularity::routing
