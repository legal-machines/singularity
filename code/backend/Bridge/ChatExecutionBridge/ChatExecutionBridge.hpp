#pragma once

#include <nlohmann/json_fwd.hpp>

// Phase 24's Chat execution and result surface. It lives beside - not
// inside - Bridge/ChatBridge for the same reason Bridge/ChatPreviewBridge
// does: these are the Chat wire methods that reach the canonical request
// path (Routing/MatterRequestExecution and, through it, the provider/
// local-model runtime) and the Documents source resolver, and Chat's
// ordinary list/create/rename/source/setup bridge stays free of that build
// footprint.
namespace singularity::bridge::chat_execution_bridge {

// chat.send - one real backend operation for the composer's CURRENT state.
// Parses the same validated composition Phase 22/23 use, then hands it to
// chat::run_chat_request(), which prepares the request again through Phase
// 23's prepare_chat_request() and executes it through the existing
// canonical MatterRequestExecution path. It never trusts a previously
// prepared preview, and reconstructs no provider, model, source or
// protection decision of its own.
nlohmann::json send(const nlohmann::json &params);
nlohmann::json resume_operation(const nlohmann::json &params);

// chat.stopOperation - a cooperative, backend-owned cancellation request
// against a real running operation. Registered with the UI-thread execution
// policy so it stays deliverable while workers are occupied; it updates the
// live stop flag and the same canonical MatterTask lifecycle record.
nlohmann::json stop_operation(const nlohmann::json &params);

// chat.operationState - the real lifecycle of one operation. Same UI-thread
// policy and the same reason.
nlohmann::json operation_state(const nlohmann::json &params);

// chat.listOperations - every unfinished operation of one Project, after
// reconciling anything a vanished process left behind (see
// chat::list_operations). This is how work interrupted by a crash or a quit
// becomes findable again instead of being lost in a Chat nobody remembers.
nlohmann::json list_operations(const nlohmann::json &params);

nlohmann::json action_permission(const nlohmann::json &params);
nlohmann::json save_action_permission(const nlohmann::json &params);
nlohmann::json respond_tool_approval(const nlohmann::json &params);

// chat.answerQuestion - the lawyer's answer to the question a running
// operation put to them, or null to leave its decisions to Singularity. The
// operation waits for it in this process and continues the same run with it,
// so it is registered on the UI thread like chat.respondToolApproval: a worker
// call could queue behind the very run it is meant to wake.
nlohmann::json answer_question(const nlohmann::json &params);

// chat.conversation - the read-only Chat projection over canonical
// MatterTasks (Chat/ChatConversation/ChatConversation.hpp). Creates no
// second transcript or result store.
nlohmann::json conversation(const nlohmann::json &params);

// chat.resolveSourceReference - Source Peek. Verifies the citation against
// the canonical Project/Chat/MatterTask chain, then resolves it through the
// existing Phase 09 local document source resolver.
nlohmann::json resolve_source_reference(const nlohmann::json &params);

} // namespace singularity::bridge::chat_execution_bridge
