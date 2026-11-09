#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Provider/Types/ReasoningEffort.hpp"

namespace singularity::routing {

// The model-visible request has one structural representation. The control
// plane is LM-owned protocol state and is never privacy-transformed. The data
// plane is the only place user/Project/document/evidence text may appear.
struct TurnControlPlane {
	nlohmann::json value = nlohmann::json::object();
};

struct TurnDataPlane {
	nlohmann::json value = nlohmann::json::object();
	nlohmann::json protection_metadata = nullptr;
};

struct StructuredResponseVariant {
	nlohmann::json schema = nlohmann::json::object();
	nlohmann::json data_plane_shape = nlohmann::json::object();
};

struct ResponseRestorationContract {
	bool unstructured_text_is_data = false;
	bool arbitrary_structured_value_is_data = false;
	std::vector<StructuredResponseVariant> structured_variants;
};

struct ModelVisibleTurn {
	TurnControlPlane control;
	TurnDataPlane data;
	ResponseRestorationContract response_restoration;
	// Set only by a phase-aware operation-turn caller (ResearchToolLoop.hpp's
	// own compileOperationTurnContract()/phase_aware_reserved_output_tokens())
	// to replace the executing request's own configured output ceiling for
	// this one turn - see ResearchGeneration's own comment for why the real
	// dispatched request and this loop's predictive capacity fit-check must
	// always agree on the same number. Carried on the turn itself (rather
	// than as a new ResearchGeneration parameter) so every existing
	// generation callable - production and every test double alike - keeps
	// its own unchanged signature and simply never reads a field it does not
	// know about; unset for every non-operation and pre-existing caller,
	// preserving today's behavior exactly. Since the token-count-as-semantic-
	// completion migration, this is no longer sent to a local model as the
	// literal request ceiling when request_semantic_completion (below) is
	// set - it stays in use only as this loop's own predictive input-fit
	// estimate (fits_continuation_budget), never as the number that decides
	// whether a turn is done.
	std::optional<std::size_t> max_output_tokens_override;
	// TOKEN COUNT IS NOT A SEMANTIC COMPLETION CONDITION: when true (every
	// real TurnContract-compiled operation turn - see compileOperationTurnContract()
	// in ResearchToolLoop.hpp), InferenceExecutor.cpp's runGeneration() asks
	// for this turn's semantic/schema completion rather than a caller-guessed
	// output-token ceiling - locally via local inference's bound_output_by_remaining_
	// context (real remaining context, never llama.cpp context
	// shift), and for a cloud provider via the largest safe physical
	// allowance that provider/model actually documents (known_model_
	// maximum_completion_tokens(), ContextBudget.hpp) rather than max_output_
	// tokens_override's own small phase-derived estimate. finish_reason==
	// "length"/"max_tokens" under this flag means a genuine physical
	// boundary, never a completed-vs-incomplete semantic verdict - see
	// InferenceResult::physical_context_exhausted. False for every non-
	// operation and pre-existing caller, preserving their exact prior
	// behavior (their own configured/overridden ceiling still governs, and
	// finish_reason=="length" still means what it always did for them).
	bool request_semantic_completion = false;
	// How long a reasoning model thinks on this one turn. Default leaves the
	// provider's own; a mechanical turn (ResearchToolLoop's facts step) asks
	// for a short think. Sent only to a model that takes it
	// (providers::accepts_reasoning_effort()).
	providers::ReasoningEffort reasoning_effort = providers::ReasoningEffort::Default;
	// A colleague's talk: its protection legend says what a symbol is in a
	// conversation's few words - the same symbols, masking and checks as every
	// turn. The legend every other turn carries asks a model to write a
	// party's name wherever it can, and in a conversation that made it greet
	// the person by name at every message.
	bool is_conversation = false;
	// Set on a turn that exists only to ask Singularity for facts about protected
	// amounts, dates, rates and quantities (the facts step): protection reads
	// the turn first, and when it leaves no such value in what would be sent,
	// the call is not made at all - there is nothing to work out - and the
	// result says so (InferenceResult::provider_call_skipped).
	bool needs_computable_values = false;

	ModelVisibleTurn() = default;
	ModelVisibleTurn(TurnControlPlane controlPlane, TurnDataPlane dataPlane,
			ResponseRestorationContract responseRestoration = {})
		: control(std::move(controlPlane)), data(std::move(dataPlane)),
		  response_restoration(std::move(responseRestoration)) {}
	// Existing non-operation callers become a typed conversational turn at the
	// same boundary; this is a representation adapter, not a second pipeline.
	ModelVisibleTurn(const std::string &prompt)
		: control{{{"protocol", "singularity.conversation.v1"}}}, data{{{"prompt", prompt}}},
		  response_restoration{true, true, {}} {}

	[[nodiscard]] std::string serialize() const {
		nlohmann::json dataPlane{{"payload", data.value}};
		if (!data.protection_metadata.is_null())
			dataPlane["protectionMetadata"] = data.protection_metadata;
		return nlohmann::json{{"controlPlane", control.value}, {"dataPlane", std::move(dataPlane)}}.dump();
	}

	// Transitional source compatibility for deterministic generation lambdas:
	// they observe the exact production serialization, never a second prompt.
	[[nodiscard]] operator std::string() const { return serialize(); }

	[[nodiscard]] static ModelVisibleTurn parse(const std::string &serialized) {
		const auto value = nlohmann::json::parse(serialized, nullptr, false);
		if (!value.is_object() || !value.contains("controlPlane") || !value.at("controlPlane").is_object()
				|| !value.contains("dataPlane") || !value.at("dataPlane").is_object()
				|| !value.at("dataPlane").contains("payload") || !value.at("dataPlane").at("payload").is_object())
			throw std::runtime_error("The model-visible turn envelope is malformed.");
		TurnDataPlane dataPlane;
		dataPlane.value = value.at("dataPlane").at("payload");
		if (value.at("dataPlane").contains("protectionMetadata"))
			dataPlane.protection_metadata = value.at("dataPlane").at("protectionMetadata");
		return {{value.at("controlPlane")}, std::move(dataPlane)};
	}
};

inline ModelVisibleTurn conversational_model_turn(const std::string &prompt) {
	return ModelVisibleTurn(prompt);
}

// The same turn with its prompt in parts, in order: each part is protected on
// its own, so a part an operation has seen before is read from what was found
// in it then, and a provider's cache can end where a part ends (a colleague's
// talk: its brief, its rules, each message - Provider/Types/PromptCache.hpp).
inline ModelVisibleTurn conversational_model_turn(const std::vector<std::string> &parts) {
	ModelVisibleTurn turn(std::string{});
	turn.data.value = nlohmann::json{{"prompt", parts}};
	return turn;
}

// The same, with Singularity's own instruction beside the parts: written as the
// payload's "instruction", which protection passes as LM-owned text - never
// scored, never rewritten by the operation's mapping (ProtectedInference's
// policy_for_typed_path) - and which the model reads before the parts.
inline ModelVisibleTurn conversational_model_turn(const std::string &instruction, const std::vector<std::string> &parts) {
	ModelVisibleTurn turn = conversational_model_turn(parts);
	if (!instruction.empty()) turn.data.value["instruction"] = instruction;
	return turn;
}

} // namespace singularity::routing
