#include "LocalInference/GenerativeRuntime/GenerativeRuntime.hpp"

#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/ChildProcess/ChildProcess.hpp"
#include "Platform/LoopbackSocket/LoopbackSocket.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace singularity::local_inference {
namespace {

using Json = nlohmann::json;

// Where this build keeps llama-server: inside a macOS bundle,
// Contents/Resources/llama.cpp (Contents/MacOS/<app> -> up two ->
// Contents/Resources); everywhere else, and for the test and command-line
// programs the build stages it beside, llama.cpp/ beside the executable.
std::filesystem::path installed_runtime_directory() {
	const auto server = platform::executable_file_name("llama-server");
	const auto executable_directory = platform::executable_path().parent_path();
	const auto bundle_relative = executable_directory.parent_path() / "Resources" / "llama.cpp";
	if (std::filesystem::exists(bundle_relative / server)) return bundle_relative;
	return executable_directory / "llama.cpp";
}

double number_or_zero(const Json &value, const char *key) {
	return value.contains(key) && value.at(key).is_number() ? value.at(key).get<double>() : 0.0;
}

// The request every call to the runtime's server sends: the per-launch key,
// JSON, and the caller's stop flag.
platform::LoopbackHttpRequest runtime_request(const std::string &method, const std::string &target,
		const std::string &api_key, std::string body, std::chrono::seconds timeout,
		const std::atomic<bool> *stop = nullptr) {
	platform::LoopbackHttpRequest request;
	request.method = method;
	request.target = target;
	request.headers = {{"Authorization", "Bearer " + api_key}};
	if (!body.empty()) request.headers.emplace_back("Content-Type", "application/json");
	request.body = std::move(body);
	request.timeout = timeout;
	request.stop = stop;
	return request;
}

// The real-context-accounting primitive of bound_output_by_remaining_context
// and of measure() - genuinely non-generating. POST
// /v1/chat/completions/input_tokens is the mode-matched sibling of the real
// /v1/chat/completions route: its handler runs the same chat-parameter
// parsing the generation handler runs on the exact same request body
// (messages/response_format - see build_generation_request_body(), shared
// with generate()), then the same tokenizer, and returns {"input_tokens": N}
// with no token decoded. A hand-tuned safety margin, or a one-token
// generation probe, would be exactly the made-up number this boundary must
// never use.
struct RealRequestMeasurement {
	std::uint64_t prompt_tokens = 0;
	std::uint64_t context_capacity_tokens = 0;
};
RealRequestMeasurement measure_real_generation_request(unsigned short port, const std::string &api_key,
		const GenerativeRequest &request, bool supports_output_constraint, std::uint64_t context_capacity_tokens) {
	const auto response = platform::loopback_http(port, runtime_request("POST",
		"/v1/chat/completions/input_tokens", api_key,
		build_generation_request_body(request, supports_output_constraint), std::chrono::seconds(30),
		request.stop.get()));
	if (response.status < 200 || response.status >= 300)
		throw std::runtime_error("The local inference runtime could not measure the real request's input token "
			"count (httpStatus=" + std::to_string(response.status) + ").");
	const Json result = Json::parse(response.body);
	if (!result.contains("input_tokens") || !result.at("input_tokens").is_number())
		throw std::runtime_error(
			"The local inference runtime's /v1/chat/completions/input_tokens response had no real input_tokens count.");
	RealRequestMeasurement measurement;
	measurement.context_capacity_tokens = context_capacity_tokens;
	measurement.prompt_tokens = static_cast<std::uint64_t>(result.at("input_tokens").get<std::int64_t>());
	return measurement;
}

// The real remaining-context ceiling for one bound_output_by_remaining_context
// call: the real loaded --ctx-size minus the real (template-inclusive,
// server-measured) prompt token count - no other term. std::nullopt when the
// prompt alone already consumes the whole context; the caller then reports an
// honest physical_context_exhausted result instead of sending a request that
// could never have succeeded.
std::optional<std::uint64_t> compute_context_bounded_output_ceiling(
		std::uint64_t context_window_tokens, std::uint64_t real_prompt_tokens) {
	if (real_prompt_tokens >= context_window_tokens) return std::nullopt;
	return context_window_tokens - real_prompt_tokens;
}

bool write_private_number(const std::filesystem::path &path, std::uint64_t value) {
	return platform::write_private_file(path, std::to_string(value) + "\n");
}

} // namespace

const char *to_string(StateBoundary boundary) {
	switch (boundary) {
		case StateBoundary::UserMessage: return "user_message";
		case StateBoundary::AssistantTurn: return "assistant_turn";
		case StateBoundary::ToolCall: return "tool_call";
		case StateBoundary::ToolResult: return "tool_result";
		case StateBoundary::DocumentInsertion: return "document_insertion";
		case StateBoundary::ContextBlock: return "context_block";
		case StateBoundary::ConsiliumHandoff: return "consilium_handoff";
	}
	return "unknown";
}

LlamaCppRuntimeAdapter::LlamaCppRuntimeAdapter(std::filesystem::path log_directory)
	: runtime_directory_(installed_runtime_directory()), log_directory_(std::move(log_directory)) {}

LlamaCppRuntimeAdapter::~LlamaCppRuntimeAdapter() { stop(); }

std::uint64_t prompt_micro_batch_tokens(const AdaptiveExecutionPlan &plan) {
	// The planner (MoEExecutionStrategy.cpp) derives this per plan, from real
	// model structure and the plan's own safety-bounded expert cache
	// capacity. A plan that never populates it (dense/CPU) uses llama.cpp's
	// pinned default.
	return plan.prompt_micro_batch_tokens.value_or(512);
}

std::uint64_t pressure_safe_expert_cache_capacity(const AdaptiveExecutionPlan &plan) {
	const auto minimum = plan.minimum_expert_cache_capacity.value_or(0);
	if (minimum == 0) return 0;
	// minimum == active_experts_per_token for every strategy that populates
	// it (see MoEExecutionStrategy.cpp), so minimum * ubatch is exactly the
	// offloader's own worst-case bound for THIS plan's fixed microbatch.
	return std::max<std::uint64_t>(minimum, minimum * prompt_micro_batch_tokens(plan));
}

PressureDecision decide_pressure_response(
		hardware::MemoryPressureLevel pressure,
		std::uint64_t expert_cache_capacity) {
	// Critical always wins, MoE cache or not, and stays safe because it ends
	// the process rather than resizing it (see GenerativeRuntime.hpp).
	if (pressure == hardware::MemoryPressureLevel::Critical) return {PressureAction::FailClosed};
	(void)expert_cache_capacity; // Reserved for when a live resize is safe to attempt again.
	return {PressureAction::Continue};
}

void LlamaCppRuntimeAdapter::start(const std::filesystem::path &model_path, const AdaptiveExecutionPlan &plan) {
	stop();
	if (!plan.strategy || !plan.backend ||
		plan.feasibility == ExecutionFeasibility::Unsupported ||
		plan.feasibility == ExecutionFeasibility::NotSafelyRunnable) {
		throw std::runtime_error(plan.reason.empty() ? "No safe execution plan is available." : plan.reason);
	}
	if (!std::filesystem::is_regular_file(model_path)) {
		throw std::runtime_error("The inspected model artifact is no longer available.");
	}
	const auto server_path = runtime_directory_ / platform::executable_file_name("llama-server");
	if (!std::filesystem::is_regular_file(server_path)) {
		throw std::runtime_error("The pinned local inference runtime is not installed in this build.");
	}
	const bool cpu_plan = *plan.strategy == ExecutionStrategy::DenseCpu ||
		*plan.strategy == ExecutionStrategy::SparseCpu;
	const bool partial_dense = *plan.strategy == ExecutionStrategy::DenseHybrid;
	const bool host_experts = *plan.strategy == ExecutionStrategy::SparseHybrid;
	if ((partial_dense && !plan.accelerator_block_count) || (host_experts && !plan.host_expert_block_count)) {
		throw std::runtime_error("The execution plan does not describe its accelerator placement.");
	}

	const auto available_port = platform::available_loopback_port();
	if (!available_port) throw std::runtime_error("A private loopback port is not available for local AI.");
	port_ = *available_port;
	context_window_tokens_ = plan.requested_context_tokens;
	const std::string port = std::to_string(port_);
	const std::string context = std::to_string(plan.requested_context_tokens);
	// Nothing on the accelerator for a CPU plan; the planner's exact block
	// count when a discrete accelerator holds only part of a dense model;
	// every block otherwise (unified memory, full residency, or an MoE split
	// where only expert tensors stay on the host).
	const std::string gpu_layers = cpu_plan ? "0"
		: partial_dense ? std::to_string(*plan.accelerator_block_count) : "all";
	const std::uint64_t cache_mib = std::max<std::uint64_t>(64,
		std::min<std::uint64_t>(2048, plan.host_memory_budget_bytes / (8ULL * 1024 * 1024)));
	const std::string cache = std::to_string(cache_mib);
	const std::string cpu_workers = std::to_string(std::max<std::uint32_t>(
		1, plan.cpu_worker_budget.value_or(std::max(1U, std::thread::hardware_concurrency()))));
	const bool sparse_experts = *plan.strategy == ExecutionStrategy::SparseExpertResidency;
	const bool heterogeneous_experts = *plan.strategy == ExecutionStrategy::SparseHeterogeneous;
	const std::string expert_slots = std::to_string(plan.expert_cache_capacity.value_or(0));
	const std::string micro_batch = std::to_string(prompt_micro_batch_tokens(plan));
	expert_cache_capacity_ = plan.expert_cache_capacity.value_or(0);
	minimum_expert_cache_capacity_ = plan.minimum_expert_cache_capacity.value_or(0);
	std::random_device random;
	std::ostringstream secret;
	secret << std::hex << std::setfill('0');
	for (int index = 0; index < 4; ++index) secret << std::setw(8) << random();
	api_key_ = secret.str();
	// Private before anything is written into it: the per-launch credential
	// and the expert-control channel below must never be readable by another
	// local user.
	slot_save_directory_ = std::filesystem::temp_directory_path() /
		(std::string(kRuntimeSlotFolderPrefix) + std::to_string(platform::current_process_id()) + "-" +
			api_key_.substr(0, 8));
	if (!platform::create_private_directory(slot_save_directory_)) {
		std::error_code removal_error;
		std::filesystem::remove_all(slot_save_directory_, removal_error);
		slot_save_directory_.clear();
		api_key_.clear();
		throw std::runtime_error("The local inference runtime cache directory could not be secured.");
	}
	if (sparse_experts || heterogeneous_experts) {
		moe_control_path_ = slot_save_directory_ / "expert-control";
		moe_telemetry_path_ = slot_save_directory_ / "expert-telemetry.json";
		moe_route_trace_path_ = slot_save_directory_ / "route-trace.ndjson";
		if (!write_private_number(moe_control_path_, expert_cache_capacity_)) {
			stop();
			throw std::runtime_error("The adaptive expert-cache control could not be initialized.");
		}
	}
	const auto api_key_path = slot_save_directory_ / "api-key";
	if (!platform::write_private_file(api_key_path, api_key_)) {
		std::error_code removal_error;
		std::filesystem::remove_all(slot_save_directory_, removal_error);
		slot_save_directory_.clear();
		api_key_.clear();
		throw std::runtime_error("The local inference runtime credential could not be secured.");
	}
	const auto log_path = log_directory_ / "llama-server.log";
	std::error_code directory_error;
	std::filesystem::create_directories(log_directory_, directory_error);

	platform::ChildProcessSpec spec;
	spec.executable = server_path;
	spec.library_directory = runtime_directory_;
	spec.output_log = log_path;
	// The runtime's own MoE extensions read their controls from these
	// variables (dependencies/llama.cpp's patches).
	if (sparse_experts) {
		spec.environment = {
			{"LOCAL_INFERENCE_MOE_CONTROL_FILE", platform::path_to_utf8(moe_control_path_)},
			{"LOCAL_INFERENCE_MOE_TELEMETRY_FILE", platform::path_to_utf8(moe_telemetry_path_)},
			{"LOCAL_INFERENCE_MOE_ROUTE_TRACE_FILE", platform::path_to_utf8(moe_route_trace_path_)}};
	}
	if (heterogeneous_experts) {
		spec.environment = {
			{"LOCAL_INFERENCE_MOE_HETEROGENEOUS", "1"},
			{"LOCAL_INFERENCE_MOE_HETEROGENEOUS_SLOTS", expert_slots},
			{"LOCAL_INFERENCE_MOE_HETEROGENEOUS_CONTROL", platform::path_to_utf8(moe_control_path_)},
			{"LOCAL_INFERENCE_MOE_HETEROGENEOUS_TELEMETRY", platform::path_to_utf8(moe_telemetry_path_)},
			{"LOCAL_INFERENCE_AI_EXPERT_PROJECTION_COUNT", "3"},
			{"LOCAL_INFERENCE_AI_RUNTIME_PROBE", "1"}};
		auto set_measurement = [&spec](const char *name, const auto &value) {
			if (value) spec.environment.emplace_back(name, std::to_string(*value));
		};
		set_measurement("LOCAL_INFERENCE_AI_CPU_GOPS", plan.cpu_execution_giga_operations_per_second);
		set_measurement("LOCAL_INFERENCE_AI_GPU_GOPS", plan.accelerator_execution_giga_operations_per_second);
		set_measurement("LOCAL_INFERENCE_AI_ADMISSION_GBPS", plan.expert_admission_gigabytes_per_second);
		set_measurement("LOCAL_INFERENCE_AI_DISPATCH_NS", plan.accelerator_dispatch_nanoseconds);
		set_measurement("LOCAL_INFERENCE_AI_SYNCHRONIZATION_NS", plan.accelerator_synchronization_nanoseconds);
		set_measurement("LOCAL_INFERENCE_AI_MERGE_NS_PER_ROW", plan.merge_nanoseconds_per_row);
		if (!plan.concurrent_cpu_accelerator)
			spec.environment.emplace_back("LOCAL_INFERENCE_AI_SERIALIZE_BRANCHES", "1");
		set_measurement("LOCAL_INFERENCE_AI_CPU_WORK_FRACTION", plan.target_cpu_work_fraction);
	}
	// --no-context-shift on every launch path below is load-bearing, not
	// defensive redundancy: this llama-server build already defaults context
	// shift to disabled, but that default is upstream's to change, never
	// something the physical-context-exhaustion contract
	// (GenerativeRequest::bound_output_by_remaining_context,
	// GenerativeResult::physical_context_exhausted) may silently depend on.
	// With it set, a generation that genuinely runs out of real context always
	// stops and reports finish_reason=="length" honestly.
	spec.arguments = {"-m", platform::path_to_utf8(model_path), "--host", "127.0.0.1", "--port", port,
		"--ctx-size", context, "--threads", cpu_workers, "--threads-batch", cpu_workers};
	const std::string slot_save_path = platform::path_to_utf8(slot_save_directory_);
	const std::string api_key_file = platform::path_to_utf8(api_key_path);
	if (sparse_experts) {
		spec.arguments.insert(spec.arguments.end(), {"--gpu-layers", gpu_layers, "--no-warmup",
			"--cache-ram", cache, "--ctx-checkpoints", "32", "--parallel", "1", "--slot-save-path", slot_save_path,
			"--api-key-file", api_key_file, "--moe-n-slots", expert_slots, "--ubatch-size", micro_batch,
			"--load-mode", "none", "--reasoning", "off", "--jinja", "--no-webui", "--no-context-shift"});
	} else if (heterogeneous_experts) {
		spec.arguments.insert(spec.arguments.end(), {"--gpu-layers", "all", "--cpu-moe", "--no-warmup",
			"--cache-ram", cache, "--ctx-checkpoints", "32", "--parallel", "1", "--slot-save-path", slot_save_path,
			"--api-key-file", api_key_file, "--ubatch-size", micro_batch, "--load-mode", "mmap",
			"--reasoning", "off", "--jinja", "--no-webui", "--no-context-shift"});
	} else {
		spec.arguments.insert(spec.arguments.end(), {"--gpu-layers", gpu_layers, "--no-warmup",
			"--cache-ram", cache, "--ctx-checkpoints", "32", "--parallel", "1", "--slot-save-path", slot_save_path,
			"--api-key-file", api_key_file, "--ubatch-size", micro_batch});
		if (host_experts) {
			// The routed experts of the first N blocks stay in host memory and
			// run on the CPU; every other tensor runs on the accelerator.
			spec.arguments.insert(spec.arguments.end(),
				{"--n-cpu-moe", std::to_string(*plan.host_expert_block_count)});
		}
		spec.arguments.insert(spec.arguments.end(), {"--reasoning", "off", "--jinja", "--no-webui",
			"--no-context-shift"});
	}

	// Owned for its whole life (see Platform/ChildProcess): it dies with this
	// process by any means, never receives the terminal's Ctrl-C (an
	// interrupt cancels a generation cooperatively, never the shared
	// inference process), and inherits nothing but its log.
	try {
		auto process = std::make_unique<platform::OwnedChildProcess>(spec);
		const std::lock_guard<std::mutex> lock(process_mutex_);
		process_ = std::move(process);
	} catch (const std::exception &error) {
		stop();
		throw std::runtime_error(std::string("The local inference runtime process could not be started. ") +
			error.what());
	}

	// Large but safely planned local artifacts can take substantially longer
	// than ten seconds to map and initialize. The worker is still polled
	// every 50 ms so failures surface immediately; this is only an upper
	// bound.
	for (int attempt = 0; attempt < 6000; ++attempt) {
		if (!running()) {
			std::optional<std::string> reason;
			{
				const std::lock_guard<std::mutex> lock(process_mutex_);
				if (process_) reason = process_->exit_reason();
			}
			stop();
			throw std::runtime_error(reason
				? "The local inference runtime exited while loading the model (" + *reason + ")."
				: std::string("The local inference runtime exited while loading the model."));
		}
		try {
			const auto response = platform::loopback_http(port_,
				runtime_request("GET", "/health", api_key_, std::string(), std::chrono::seconds(1)));
			if (response.status == 200) return;
		} catch (const platform::LoopbackHttpError &) {
			// Not listening yet, or still loading: asked again shortly.
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	stop();
	throw std::runtime_error("The local model did not become ready within the safe startup limit.");
}

std::string build_generation_request_body(const GenerativeRequest &request,
		bool supports_output_constraint) {
	if (request.prompt.empty()) throw std::invalid_argument("A local generation prompt is required.");
	Json body{{"model", "local"},
		{"messages", Json::array({Json{{"role", "user"}, {"content", request.prompt}}})},
		{"max_tokens", request.maximum_output_tokens}, {"temperature", 0.0}, {"seed", 1},
		{"cache_prompt", true}, {"timings_per_token", true}};
	if (request.output_constraint) {
		if (!supports_output_constraint) {
			throw std::domain_error("The active local runtime does not support hard JSON-Schema output constraints.");
		}
		if (request.output_constraint->schema_name.empty()) {
			throw std::invalid_argument("An output schema name is required when an output constraint is supplied.");
		}
		Json schema;
		try {
			schema = Json::parse(request.output_constraint->json_schema);
		} catch (const Json::parse_error &) {
			throw std::invalid_argument("The output JSON schema is invalid JSON.");
		}
		if (!schema.is_object()) {
			throw std::invalid_argument("The output JSON schema must be a JSON object.");
		}
		body["response_format"] = Json{{"type", "json_schema"},
			{"json_schema", Json{{"name", request.output_constraint->schema_name},
				{"strict", true}, {"schema", std::move(schema)}}}};
	}
	return body.dump();
}

std::string safe_generation_rejection_message(const GenerativeRequest &request,
		int http_status, const std::string &response_body, std::size_t request_bytes) {
	auto safe_token = [](const std::string &value) {
		if (value.empty() || value.size() > 64) return std::string();
		if (!std::all_of(value.begin(), value.end(), [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '.' || character == '_'
				|| character == ':' || character == '-';
		})) return std::string();
		return value;
	};
	std::string error_class;
	std::string searchable = response_body;
	const Json parsed = Json::parse(response_body, nullptr, false);
	if (parsed.is_object() && parsed.contains("error")) {
		const Json &error = parsed.at("error");
		if (error.is_object()) {
			if (error.contains("type") && error.at("type").is_string())
				error_class = safe_token(error.at("type").get<std::string>());
			if (error_class.empty() && error.contains("code") && error.at("code").is_string())
				error_class = safe_token(error.at("code").get<std::string>());
		}
	}
	std::transform(searchable.begin(), searchable.end(), searchable.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	std::string reason = "runtime_rejected";
	if (searchable.find("compute error") != std::string::npos) reason = "compute_error";
	else if (searchable.find("gpu timeout") != std::string::npos
			|| searchable.find("runtime timeout") != std::string::npos) reason = "runtime_timeout";
	else if (searchable.find("out of memory") != std::string::npos) reason = "out_of_memory";
	else if (searchable.find("context window") != std::string::npos
			|| searchable.find("context size") != std::string::npos) reason = "context_capacity";
	else if (searchable.find("json schema") != std::string::npos
			|| searchable.find("grammar") != std::string::npos) reason = "output_constraint";

	const std::string schema_name = request.output_constraint
		? safe_token(request.output_constraint->schema_name) : std::string();
	const std::size_t schema_bytes = request.output_constraint
		? request.output_constraint->json_schema.size() : 0;
	return "The local inference runtime rejected the generation request. "
		"runtime=llama.cpp backend=local httpStatus=" + std::to_string(http_status)
		+ " errorClass=" + (error_class.empty() ? std::string("unavailable") : error_class)
		+ " reason=" + reason
		+ " schema=" + (schema_name.empty() ? std::string("none") : schema_name)
		+ " schemaBytes=" + std::to_string(schema_bytes)
		+ " promptBytes=" + std::to_string(request.prompt.size())
		+ " requestBytes=" + std::to_string(request_bytes)
		+ " maxOutputTokens=" + std::to_string(request.maximum_output_tokens) + ".";
}

GenerativeResult LlamaCppRuntimeAdapter::generate(const GenerativeRequest &request) {
	if (!running()) throw std::runtime_error("The local generative model is not running.");
	// bound_output_by_remaining_context replaces "the caller guessed a small
	// ceiling" with "measure the real remaining room and ask for that" - the
	// request sent downstream still has an ordinary numeric
	// maximum_output_tokens; only where that number comes from changes.
	GenerativeRequest effective_request = request;
	if (request.bound_output_by_remaining_context) {
		const std::uint64_t real_prompt_tokens = measure_real_generation_request(
			port_, api_key_, request, supports_json_schema_output_constraint(), context_window_tokens_).prompt_tokens;
		const auto ceiling = compute_context_bounded_output_ceiling(context_window_tokens_, real_prompt_tokens);
		if (!ceiling) {
			GenerativeResult exhausted;
			exhausted.prompt_tokens = real_prompt_tokens;
			exhausted.evaluated_prompt_tokens = real_prompt_tokens;
			exhausted.finish_reason = "length";
			exhausted.physical_context_exhausted = true;
			return exhausted;
		}
		effective_request.maximum_output_tokens = *ceiling;
	}
	const std::string encoded_body = build_generation_request_body(
		effective_request, supports_json_schema_output_constraint());
	const std::size_t encoded_bytes = encoded_body.size();
	const auto response = platform::loopback_http(port_, runtime_request("POST", "/v1/chat/completions",
		api_key_, encoded_body, std::chrono::seconds(request.timeout_seconds.value_or(300)), request.stop.get()));
	if (response.status < 200 || response.status >= 300) {
		throw std::runtime_error(safe_generation_rejection_message(
			effective_request, response.status, response.body, encoded_bytes));
	}
	const Json result = Json::parse(response.body);
	const bool semantic_checkpoint_match = request.stable_prefix_id && request.stable_prefix_boundary &&
		last_prefix_id_ == request.stable_prefix_id &&
		last_prefix_boundary_ == request.stable_prefix_boundary;
	GenerativeResult output;
	output.text = result.at("choices").at(0).at("message").value("content", "");
	output.finish_reason = result.at("choices").at(0).value("finish_reason", "");
	if (result.contains("usage")) {
		output.prompt_tokens = result.at("usage").value("prompt_tokens", std::uint64_t{0});
		output.generated_tokens = result.at("usage").value("completion_tokens", std::uint64_t{0});
		if (result.at("usage").contains("prompt_tokens_details")) {
			output.prefix_cache_used = result.at("usage").at("prompt_tokens_details")
				.value("cached_tokens", std::uint64_t{0}) > 0;
		}
	}
	if (result.contains("timings")) {
		output.evaluated_prompt_tokens = static_cast<std::uint64_t>(
			number_or_zero(result.at("timings"), "prompt_n"));
		output.prompt_ms = number_or_zero(result.at("timings"), "prompt_ms");
		output.generation_ms = number_or_zero(result.at("timings"), "predicted_ms");
		output.prefix_cache_used = number_or_zero(result.at("timings"), "cache_n") > 0.0 &&
			semantic_checkpoint_match;
	}
	if (output.evaluated_prompt_tokens == 0) output.evaluated_prompt_tokens = output.prompt_tokens;
	output.prefix_cache_used = output.prefix_cache_used && semantic_checkpoint_match;
	// physical_context_exhausted is only meaningful under bound_output_by_
	// remaining_context: effective_request.maximum_output_tokens WAS the real
	// measured remaining context then, so "length" can only mean the
	// generation genuinely ran out of room. A caller that never sets the
	// flag keeps finish_reason=="length"'s ordinary meaning.
	output.physical_context_exhausted = request.bound_output_by_remaining_context
		&& output.finish_reason == "length";
	last_prefix_id_ = request.stable_prefix_id;
	last_prefix_boundary_ = request.stable_prefix_boundary;
	return output;
}

GenerativeRequestMeasurement LlamaCppRuntimeAdapter::measure(const GenerativeRequest &request) {
	if (!running()) throw std::runtime_error("The local generative model is not running.");
	const RealRequestMeasurement real = measure_real_generation_request(
		port_, api_key_, request, supports_json_schema_output_constraint(), context_window_tokens_);
	GenerativeRequestMeasurement measurement;
	measurement.prompt_tokens = real.prompt_tokens;
	measurement.context_capacity_tokens = real.context_capacity_tokens;
	measurement.physical_remaining_tokens = real.prompt_tokens >= real.context_capacity_tokens
		? 0 : real.context_capacity_tokens - real.prompt_tokens;
	measurement.fits = real.prompt_tokens < real.context_capacity_tokens;
	return measurement;
}

bool LlamaCppRuntimeAdapter::supports_json_schema_output_constraint() const noexcept {
	return true;
}

void LlamaCppRuntimeAdapter::adapt_to_memory_pressure(hardware::MemoryPressureLevel pressure) {
	if (!running()) return;
	// Warning's KV-erase is unrelated to the MoE capacity decision below (it
	// applies even to plans with no expert cache at all) and has its own
	// fail-closed reason to stop, so it stays a direct side effect here.
	if (pressure == hardware::MemoryPressureLevel::Warning) {
		try {
			const auto response = platform::loopback_http(port_, runtime_request("POST", "/slots/0?action=erase",
				api_key_, "{}", std::chrono::seconds(5)));
			if (response.status < 200 || response.status >= 300) {
				stop();
				return;
			}
			last_prefix_id_.reset();
			last_prefix_boundary_.reset();
		} catch (...) {
			stop();
			return;
		}
	}
	const auto decision = decide_pressure_response(pressure, expert_cache_capacity_);
	switch (decision.action) {
		case PressureAction::FailClosed:
			// stop() releases the worker, so running() becomes false and every
			// later generate() on this adapter throws; the owning service sees
			// that and the next request loads the model from scratch (fresh
			// detection, plan and admission), never resuming this process.
			stop();
			return;
		case PressureAction::Continue:
			return;
	}
}

RuntimeResourceTelemetry LlamaCppRuntimeAdapter::resource_telemetry() const noexcept {
	RuntimeResourceTelemetry telemetry;
	{
		const std::lock_guard<std::mutex> lock(process_mutex_);
		const platform::ChildProcessResourceUsage usage = process_ ? process_->resource_usage()
			: platform::ChildProcessResourceUsage{};
		telemetry.current_resident_memory_bytes = usage.resident_memory_bytes;
		telemetry.lifetime_peak_memory_bytes = usage.peak_resident_memory_bytes;
		telemetry.process_page_ins = usage.page_ins;
		telemetry.process_disk_read_bytes = usage.storage_read_bytes;
	}
	if (!moe_telemetry_path_.empty()) {
		// The runtime publishes fabric counters from its bounded background
		// control loop every 100 ms, never from an expert hot path. Waiting
		// beyond one interval keeps a fresh request from being compared with
		// a stale file.
		std::this_thread::sleep_for(std::chrono::milliseconds(125));
		try {
			std::ifstream input(moe_telemetry_path_);
			Json expert;
			input >> expert;
			auto read_u64 = [&](const char *key) -> std::optional<std::uint64_t> {
				return expert.contains(key) && expert.at(key).is_number_unsigned()
					? std::optional<std::uint64_t>(expert.at(key).get<std::uint64_t>()) : std::nullopt;
			};
			telemetry.expert_cache_capacity = read_u64("expert_cache_capacity");
			telemetry.expert_active_slot_bytes = read_u64("active_slot_bytes");
			telemetry.metal_current_allocated_bytes = read_u64("metal_current_allocated_bytes");
			telemetry.metal_recommended_working_set_bytes = read_u64("metal_recommended_working_set_bytes");
			telemetry.expert_cache_hits = read_u64("cache_hits");
			telemetry.expert_cache_misses = read_u64("cache_misses");
			telemetry.routed_expert_rows = read_u64("routed_rows");
			telemetry.cpu_executed_expert_rows = read_u64("cpu_rows");
			telemetry.gpu_resident_expert_rows = read_u64("gpu_resident_rows");
			telemetry.gpu_admitted_expert_rows = read_u64("gpu_admitted_rows");
			telemetry.routing_accounting_violations = read_u64("routing_accounting_violations");
			telemetry.expert_evictions = read_u64("expert_evictions");
			telemetry.expert_bytes_loaded = read_u64("bytes_loaded");
			telemetry.expert_admission_bytes = read_u64("admitted_bytes");
			telemetry.expert_storage_load_nanoseconds = read_u64("storage_load_ns");
			telemetry.logical_bytes_requested = read_u64("logical_bytes_requested");
			telemetry.api_read_bytes = read_u64("api_read_bytes");
			telemetry.peak_transient_in_flight_bytes = read_u64("peak_transient_in_flight_bytes");
			telemetry.prefill_expert_cache_hits = read_u64("prefill_cache_hits");
			telemetry.prefill_expert_cache_misses = read_u64("prefill_cache_misses");
			telemetry.prefill_expert_bytes_loaded = read_u64("prefill_bytes_loaded");
			telemetry.decode_expert_cache_hits = read_u64("decode_cache_hits");
			telemetry.decode_expert_cache_misses = read_u64("decode_cache_misses");
			telemetry.decode_expert_bytes_loaded = read_u64("decode_bytes_loaded");
			telemetry.cpu_branch_nanoseconds = read_u64("cpu_branch_nanoseconds");
			telemetry.metal_branch_wall_nanoseconds = read_u64("metal_branch_wall_nanoseconds");
			telemetry.actual_simultaneous_execution_nanoseconds = read_u64("actual_simultaneous_execution_nanoseconds");
			telemetry.branch_critical_path_nanoseconds = read_u64("branch_critical_path_nanoseconds");
		} catch (...) {
		}
	}
	if (!moe_route_trace_path_.empty()) {
		try {
			std::ifstream input(moe_route_trace_path_);
			std::string line;
			while (std::getline(input, line)) {
				if (line.empty()) continue;
				const Json entry = Json::parse(line);
				if (entry.value("type", std::string{}) == "overflow") {
					telemetry.route_trace_overflow = true;
					continue;
				}
				RouteTraceEntry route;
				route.decision_sequence = entry.at("decision_sequence").get<std::uint64_t>();
				route.token = entry.at("token").get<std::uint64_t>();
				route.layer = entry.at("layer").get<std::int32_t>();
				route.exact_routed_ids = entry.at("exact_routed_ids").get<std::vector<std::int32_t>>();
				route.top_k = entry.at("top_k").get<std::uint64_t>();
				route.decision_timestamp_nanoseconds =
					entry.at("decision_timestamp_nanoseconds").get<std::uint64_t>();
				route.hard_resident_before = entry.at("hard_resident_before").get<std::vector<bool>>();
				route.state_ready_timestamp_nanoseconds =
					entry.at("state_ready_timestamp_nanoseconds").get<std::uint64_t>();
				route.execution_timestamp_nanoseconds =
					entry.at("execution_timestamp_nanoseconds").get<std::uint64_t>();
				telemetry.route_trace.push_back(std::move(route));
			}
		} catch (...) {
			telemetry.route_trace.clear();
			telemetry.route_trace_overflow = true;
		}
	}
	return telemetry;
}

void LlamaCppRuntimeAdapter::stop() noexcept {
	{
		const std::lock_guard<std::mutex> lock(process_mutex_);
		if (process_) {
			process_->terminate();
			process_.reset();
		}
	}
	port_ = 0;
	context_window_tokens_ = 0;
	last_prefix_id_.reset();
	last_prefix_boundary_.reset();
	api_key_.clear();
	moe_control_path_.clear();
	moe_telemetry_path_.clear();
	moe_route_trace_path_.clear();
	expert_cache_capacity_ = 0;
	minimum_expert_cache_capacity_ = 0;
	if (!slot_save_directory_.empty()) {
		std::error_code removal_error;
		std::filesystem::remove_all(slot_save_directory_, removal_error);
		slot_save_directory_.clear();
	}
}

bool LlamaCppRuntimeAdapter::running() const noexcept {
	// A real liveness probe of the worker, never a remembered state: a worker
	// that died on its own (out of memory, an assertion abort) is not
	// running - generate() then reloads it and the live state reports it
	// unloaded with no grant.
	const std::lock_guard<std::mutex> lock(process_mutex_);
	return process_ && process_->running();
}

const std::filesystem::path &LlamaCppRuntimeAdapter::runtime_directory() const noexcept {
	return runtime_directory_;
}

} // namespace singularity::local_inference
