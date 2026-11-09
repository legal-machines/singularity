#include "Routing/ModelCall/CallModel.hpp"

#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"
#include "Routing/InferenceExecutor/LocalOnlyWork.hpp"

#include <chrono>
#include <thread>

namespace singularity::inference {

namespace {

// A stop asked while the call is out becomes an aborted connection: a watcher
// beside the call, ended and joined however the call ends.
class StopWatch {
public:
	StopWatch(const std::atomic<bool> *stopped, std::shared_ptr<platform::HttpCancelToken> token) {
		if (stopped == nullptr) return;
		watcher_ = std::thread([this, stopped, token]() {
			while (isDone_.load() == false) {
				if (stopped->load() == true) {
					token->cancel();
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
		});
	}
	~StopWatch() {
		isDone_.store(true);
		if (watcher_.joinable() == true) watcher_.join();
	}
	StopWatch(const StopWatch &) = delete;
	StopWatch &operator=(const StopWatch &) = delete;

private:
	std::atomic<bool> isDone_{false};
	std::thread watcher_;
};

long long millisecondsSince(std::chrono::steady_clock::time_point started) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
}

} // namespace

CallOutcome call_model(const ModelCall &call) {
	CallOutcome outcome;
	outcome.model = call.model;
	if (call.stopped != nullptr && call.stopped->load() == true) {
		outcome.failure = FailureClass::Stopped;
		outcome.message = "The work was stopped before the model was asked.";
		return outcome;
	}

	routing::InferenceRequest request;
	request.call_purpose = call.purpose;
	request.model_target = call.model;
	// Where this work may run is the operation's record, read by the executor.
	request.authority_operation = routing::InferenceRequest::AuthorityOperation{call.identity.operation.matter_id,
		call.identity.operation.operation_id};
	request.workspace_id = call.workspace_id;
	request.maximum_output_tokens = call.maximum_output_tokens;
	request.project_entity_hints = call.entity_hints;
	request.prompt = call.prompt;
	request.pictures = call.pictures;
	request.prompt_parts = call.prompt_parts;
	request.prompt_instruction = call.prompt_instruction;
	request.reasoning_effort = call.reasoning_effort;
	request.is_conversation = call.is_conversation;
	request.research_mode = research::ResearchMode::Off;
	request.research_final_output_contract = call.output_contract;
	request.operation_symbols = call.symbols != nullptr ? call.symbols
		: std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
	if (request.operation_symbols->operation_id.empty() == true) request.operation_symbols->operation_id = call.identity.operation.operation_id;
	outcome.protection_mode = routing::protection_mode_of(call.model.provider_id, request);

	// A stop, or Singularity closing, abandons the call.
	const std::shared_ptr<platform::HttpCancelToken> token = std::make_shared<platform::HttpCancelToken>();
	const platform::lifecycle::CancelOnClosing closingStops(token);
	const auto isStopped = [&]() {
		return token->is_cancelled() == true || (call.stopped != nullptr && call.stopped->load() == true);
	};
	const auto started = std::chrono::steady_clock::now();
	routing::InferenceResult result;
	{
		const StopWatch watch(call.stopped, token);
		try {
			routing::require_execution_ready(request);
			const platform::HttpCancelScope scope(token);
			outcome.is_sent = true;
			result = routing::execute(request);
		} catch (const providers::TaskError &error) {
			outcome.latency_ms = millisecondsSince(started);
			outcome.code = error.code();
			outcome.message = error.what();
			outcome.failure = classify(error.code(), isStopped(), platform::lifecycle::is_closing());
			// The executor's own refusal of a model outside this computer for work
			// kept here: not allowed, rather than a model refused.
			if (error.code() == providers::TaskErrorCode::UnsupportedModel && outcome.message == routing::kLocalOnlyWorkNotSent)
				outcome.failure = FailureClass::NotAllowedHere;
			// A model that declined to answer refused the request: asked again, it
			// declines again.
			if (outcome.failure == FailureClass::Unusable && providers::is_model_refusal(error) == true)
				outcome.failure = FailureClass::Refused;
			if (const auto *unusable = dynamic_cast<const providers::UnusableGeneration *>(&error); unusable != nullptr) {
				outcome.failed_usage = unusable->usage;
				outcome.failed_finish_reason = unusable->finish_reason;
			}
			return outcome;
		} catch (const platform::CredentialUnavailable &) {
			throw;
		} catch (const std::exception &error) {
			// No provider's failure: the request itself was refused, unless it was
			// stopped or Singularity is closing.
			outcome.latency_ms = millisecondsSince(started);
			outcome.message = error.what();
			outcome.failure = platform::lifecycle::is_closing() == true ? FailureClass::Closing
				: isStopped() == true ? FailureClass::Stopped : FailureClass::Refused;
			return outcome;
		}
	}
	outcome.latency_ms = millisecondsSince(started);
	outcome.result = result;
	if (call.reply.has_value() == false) {
		outcome.reply = Reply{result.text, std::nullopt};
		return outcome;
	}
	const ReplyCheck checked = check_reply(result.text, *call.reply);
	if (checked.reply.has_value() == true) {
		outcome.reply = checked.reply;
		return outcome;
	}
	outcome.failure = FailureClass::Unusable;
	outcome.message = "The model's answer could not be used: " + checked.why_unusable + ".";
	return outcome;
}

} // namespace singularity::inference
