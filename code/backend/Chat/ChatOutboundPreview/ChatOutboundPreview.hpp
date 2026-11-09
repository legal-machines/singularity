#pragma once

#include "Chat/ChatOperationSetup/ChatOperationSetup.hpp"
#include "Chat/ChatSourceResolver/ChatSourceResolver.hpp"
#include "Routing/Types/ModelTarget.hpp"
#include "Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::chat {

// One pending, not-yet-sent Chat request exactly as the composer currently
// holds it. Composition state only - nothing here is persisted by this
// component, and none of it is a second copy of backend truth: every field
// is either the user's own unsent input or an explicit reference to
// something the Phase 21/22 resolvers below own.
struct ChatRequestComposition {
  std::string project_id;
  std::optional<std::string> chat_id;
  std::string prompt;
  // Explicit Phase 21 backend truth - never inferred from whether
  // selected_document_ids happens to be empty (ChatSourceResolver.hpp).
  SourcePolicyMode source_policy_mode = SourcePolicyMode::Relevant;
  std::vector<std::string> selected_document_ids;
	// A source selection grants read scope only. This separate identity is
	// present only when the caller explicitly binds an existing artifact as a
	// possible update destination.
	std::optional<std::string> target_document_id;
  std::optional<std::string> legal_task_preset_id;
  std::optional<int> legal_task_preset_version;
  // Applies to the Phase 22 backend-resolved primary result role only, for
  // this one prospective operation - never persisted anywhere
  // (ChatOperationSetup.hpp).
  std::optional<routing::ModelTarget> one_time_primary_model_override;
  // Set when this request re-runs an earlier Chat task (Retry, Run with another
  // model, Edit and continue): the new attempt joins that task's retry chain
  // and never protects less than it did.
  std::optional<std::string> retry_of_task_id;
	// The composer can put a question from the model to the lawyer and send the
	// answer back while the request runs (Tool/LawyerQuestion). Set by the app's
	// Chat for a send and its preview alike; a caller that cannot answer - the
	// console, a test - leaves it off, and the model is never offered a question
	// nobody would answer.
	bool can_ask_lawyer = false;
	// Nobody watches this request as it runs: an AI colleague's own work. A
	// question or a web search to approve keeps its place and the request
	// returns at once, waiting for the decision there, so it never holds a
	// thread another piece of work needs.
	bool unattended = false;
	// An agent's own run in its thread (Agent/AgentProduction): the agent
	// began it - its schedule, a change it noticed - not a person.
	bool agent_initiated = false;
	// The agent answering, when it answers outside its own thread - a thread
	// where someone named it: its id, its name and its brief, which the request
	// carries as the chat's own instructions would.
	std::string agent_id;
	std::string agent_name;
	std::string agent_brief;
	// Who asked, when it is not the person at this computer: a colleague who
	// named the agent in a thread (MatterTask::author_*).
	std::string author_actor_id;
	std::string author_name;
	// The person's message this request answers, when someone named the agent.
	std::string answers_message_id;
	// An agent's own run (Agent/AgentProduction) - this process's own truth,
	// never read from a request or a Project's .lm: the operation is the
	// agent's, and every change it would make waits for the lawyer
	// (Tool/OperationAuthority).
	bool agent_run = false;
	// The request works only on this computer: an agent set to (its policy,
	// Agent/AgentProduction). ChatExecution also keeps on this computer every
	// request carrying on such an operation, and one in such an agent's thread.
	bool local_only = false;
	// The operation this request carries on: the one Retry, Run with another
	// model or Edit and continue runs again, or the one Send again replaces. It
	// never runs with more authority than that one had on this computer.
	std::string continues_operation_id;
};

// The single canonical prepared-request handoff for a Chat operation: the
// real Phase 21 source resolution, the real Phase 22 operation setup, and
// the exact params the EXISTING canonical request-preparation path consumes
// (Routing/MatterRequestExecution.hpp - the same
// task_attempt_spec_from_params()/prepare_task_request() pair a real send
// runs through). There is deliberately no second Chat request assembler:
// Phase 23's preview and Phase 24's execution both start here, so a preview
// can never describe a request shape execution would not actually build.
struct PreparedChatRequest {
  ChatSourceResolution sources;
  OperationSetup setup;
	professional::ProfessionalExecutionIntent professional_intent;
  // True when the resolved primary model is a connected provider rather
  // than a local model - the one honest basis for saying anything leaves
  // this device, never a UI label.
  bool is_external_destination = false;
  nlohmann::json request_params;
};

// Resolves - never executes, never persists, never runs a model - the exact
// canonical request one pending Chat composition would produce. Phase 21's
// resolve_sources() runs first, so its explicit-mode fail-closed contract
// (Relevant rejects a selection; Selected rejects an empty one and rejects
// any id that is not a genuinely AI-ready document of this Project) governs
// before anything is prepared. Throws with a plain-language message when
// the Phase 22 primary result role has no model assigned at all, rather
// than letting request preparation fall back to some other default.
PreparedChatRequest prepare_chat_request(const ChatRequestComposition &composition);

// The compact, truthful composer state behind Chat's calm trust affordance
// (phase-23's "Normal protection state"). Cheap enough to resolve whenever
// the Chat/legal task/one-time model changes - it runs no retrieval, no
// request preparation and no protection - and deliberately never depends on
// the prompt, so typing never prepares anything.
struct ChatProtectionState {
  // "external" (a connected provider), "local" (nothing leaves this Mac) or
  // "unassigned" (the primary result role has no model yet, so Singularity
  // claims nothing at all).
  std::string destination;
  std::optional<routing::ModelTarget> primary_model;
  std::string primary_result_role;
  // The live Protected API policy mode this request would actually run
  // under ("off"/"standard"/"strict") - the same policy value
  // prepare_task_request() resolves, never a separate protection setting.
  std::string protection_mode;
  // Exactly the predicate a real protected external request enforces
  // (routing::require_papinian_model_ready) - so "Papinian Guard ready" can
  // never be shown for a Guard a real send would refuse to use.
  bool is_protection_ready = false;
  // True while the model is still loading after launch: not ready yet, but
  // not a problem either. A real send arriving in this window waits for the
  // guard; this read does not.
  bool is_protection_loading = false;
  // Plain-language reason, present only when is_protection_ready == false.
  std::string protection_message;
  // Why this chat's work stays on this computer, when it does: "asked",
  // "origin_unconfirmed" or "thread". Then `primary_model` is the local
  // model it really runs on (none when nothing here can run it) and
  // `destination` is "local", whatever AI Setup would choose.
  std::optional<std::string> kept_here;
  // Why it stays here, or the sentence a send stops with when nothing here can
  // run it.
  std::string kept_here_message;
};

// What a request kept on this computer really runs on: why it is kept here,
// the local model it runs on (none when this computer has none it may use),
// and the words for it - why it stays here, or why nothing can run.
struct KeptHereModel {
  std::string reason;
  std::optional<routing::ModelTarget> model;
  std::string message;
};

// Chat/ChatExecution's own decision of whether and where work stays on this
// computer, installed by the application: the composer's state and the send
// then say the same. Without one, nothing is said to stay here.
using KeptHereResolver = std::function<std::optional<KeptHereModel>(const ChatRequestComposition &)>;
void set_kept_here_resolver(KeptHereResolver resolver);

ChatProtectionState resolve_protection_state(const ChatRequestComposition &composition);

// "What leaves this device?" for one pending Chat request: prepares the
// exact request through prepare_chat_request() above, then hands it to the
// EXISTING Phase 11 preview boundary
// (matter_request_execution::preview_outbound_request()), which owns the
// Papinian protection call, the redacted no-original-value occurrence
// summary and the ephemeral native preview session. This function adds only
// Chat-facing facts that are themselves already resolved above - it never
// re-derives a count, a destination or any protected content of its own.
nlohmann::json preview_chat_outbound_request(const ChatRequestComposition &composition);

} // namespace singularity::chat
