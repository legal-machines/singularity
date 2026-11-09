#pragma once

// One model call on behalf of an operation. It is sent only through
// routing::execute, which reads the operation's authority record (M1b); a stop
// is honoured before it is sent and while it is out; its failure has one class;
// its reply is checked against the form the call declares, when it declares
// one. It builds no prompt, keeps no Matter state, runs no tool and decides
// nothing a reply means: its caller does, and keeps its own records and words.

#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/ModelCall/ModelCall.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::inference {

struct ModelCall {
	// Its operation names the authority record the executor reads.
	CallIdentity identity;
	routing::observability::CallPurpose purpose = routing::observability::CallPurpose::SubstantiveGeneration;
	// Resolved by the workflow, from its own policy.
	routing::ModelTarget model;
	std::optional<std::string> workspace_id;
	std::string prompt;
	std::vector<std::string> prompt_parts;
	std::string prompt_instruction;
	std::shared_ptr<const std::vector<providers::InputPicture>> pictures;
	// Only ever narrows what the model may write.
	std::optional<std::uint64_t> maximum_output_tokens;
	providers::ReasoningEffort reasoning_effort = providers::ReasoningEffort::Default;
	bool is_conversation = false;
	// The Matter's identities, for protection when the call leaves this computer.
	std::vector<routing::ProjectEntityProtectionHint> entity_hints;
	std::shared_ptr<protected_api::papinian::OperationProtectedSymbolTable> symbols;
	// The shape asked of the model, sent with the call.
	std::optional<providers::GenerationOutputContract> output_contract;
	// The form its reply must take; none: the text comes back as it came, for
	// the caller to read.
	std::optional<ReplyContract> reply;
	const std::atomic<bool> *stopped = nullptr;
};

struct CallOutcome {
	// The reply, or why there is none: its class, a provider's code when a
	// provider failed, and the failure's own words, for the caller's record and
	// the words it gives a member.
	std::optional<Reply> reply;
	std::optional<FailureClass> failure;
	std::optional<providers::TaskErrorCode> code;
	std::string message;
	// Whether the executor was asked at all: a call stopped before it began is not.
	bool is_sent = false;
	routing::ModelTarget model;
	// What the executor gave back when it answered - the text, what it used,
	// how it was protected - for the caller's own record, whether or not the
	// reply met its form.
	std::optional<routing::InferenceResult> result;
	// What a failed attempt still reported using, and why it ended.
	std::optional<providers::UsageMetadata> failed_usage;
	std::optional<std::string> failed_finish_reason;
	// The protection the request went out under (routing::protection_mode_of).
	std::string protection_mode;
	long long latency_ms = 0;
};

// The call made, once. A computer whose saved credentials are locked throws
// platform::CredentialUnavailable to the caller, whose work waits for the unlock.
CallOutcome call_model(const ModelCall &call);

} // namespace singularity::inference
