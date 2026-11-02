#pragma once

#include "LocalInference/ExecutionPlanner/ExecutionPlanner.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform {
class OwnedChildProcess;
}

namespace singularity::local_inference {

enum class StateBoundary {
	UserMessage,
	AssistantTurn,
	ToolCall,
	ToolResult,
	DocumentInsertion,
	ContextBlock,
	ConsiliumHandoff,
};

struct GenerativeRequest {
	struct OutputConstraint {
		std::string schema_name;
		std::string json_schema;
		bool required = false;
	};

	std::string prompt;
	std::uint64_t maximum_output_tokens = 128;
	std::optional<OutputConstraint> output_constraint;
	std::optional<StateBoundary> stable_prefix_boundary;
	std::optional<std::string> stable_prefix_id;
	// Wall-clock ceiling for the completion request. std::nullopt selects
	// the adapter's own default (see LlamaCppRuntimeAdapter::generate()).
	std::optional<int> timeout_seconds;
	// When true, `maximum_output_tokens` above is IGNORED and
	// LlamaCppRuntimeAdapter::generate() instead computes the real remaining
	// room in this model's own loaded context window (real prompt tokens,
	// measured via llama-server's own mode-matched, non-generating
	// POST /v1/chat/completions/input_tokens, subtracted from the real
	// loaded --ctx-size) and requests that instead - i.e. "let a bounded,
	// grammar/schema-constrained generation run until it semantically
	// completes (or the model's own EOS), never stop it at a caller-guessed
	// token count." Context shift stays disabled regardless (see start()'s
	// own --no-context-shift) - this can only ever result in either a normal
	// completion or an honest, explicitly reported physical exhaustion
	// (GenerativeResult::physical_context_exhausted), never a silently
	// shifted/dropped prompt.
	bool bound_output_by_remaining_context = false;
	// Set by another thread - a person's Stop - to end this request while it
	// runs: the connection to the runtime closes, which the runtime observes
	// and answers by cancelling the generation, and the call throws.
	std::shared_ptr<const std::atomic<bool>> stop;
};

struct GenerativeResult {
	std::string text;
	std::uint64_t prompt_tokens = 0;
	// Tokens actually evaluated by this request. This can be smaller than
	// prompt_tokens when the runtime reuses a prefix; throughput must use this
	// runtime-reported denominator rather than total request usage.
	std::uint64_t evaluated_prompt_tokens = 0;
	std::uint64_t generated_tokens = 0;
	double prompt_ms = 0.0;
	double generation_ms = 0.0;
	bool prefix_cache_used = false;
	std::string finish_reason;
	// Opaque presentation/diagnostic payload assembled by the owning service
	// from the runtime's measured resource and route-trace surfaces. It does
	// not participate in execution or planning.
	std::string telemetry_json;
	// Set (only ever alongside finish_reason=="length") when this generation
	// ran under GenerativeRequest::bound_output_by_remaining_context and
	// still could not finish within this model's real, measured remaining
	// context - a genuine physical boundary, not a caller-chosen ceiling
	// being hit, and never the product of llama.cpp silently shifting/
	// dropping earlier context to keep going (context shift stays disabled -
	// see LlamaCppRuntimeAdapter::start()). A caller that never sets
	// bound_output_by_remaining_context never sees this become true.
	bool physical_context_exhausted = false;
};

// A real, physically-measured fact about a GenerativeRequest, without
// generating a response - see IGenerativeRuntimeAdapter::measure(). Every
// field comes from the same tokenizer/chat-template/runtime accounting a real
// generate() call uses; none of it is a chars-per-token estimate.
struct GenerativeRequestMeasurement {
	// The real prompt token count this exact request would evaluate,
	// including chat-template wrapping - the same number a real generate()
	// call's own GenerativeResult::prompt_tokens would report.
	std::uint64_t prompt_tokens = 0;
	// This model's real loaded --ctx-size, granted at load time.
	std::uint64_t context_capacity_tokens = 0;
	// context_capacity_tokens - prompt_tokens, floored at 0 - the real room
	// physically left for output once this exact prompt is evaluated.
	std::uint64_t physical_remaining_tokens = 0;
	// True iff prompt_tokens < context_capacity_tokens.
	bool fits = false;
};

// One exact target-model routing decision. The runtime trace carries no
// architecture or model-name assumptions: layer, token ordinal and top-k are
// reported by the executing backend.
struct RouteTraceEntry {
	std::uint64_t decision_sequence = 0;
	std::uint64_t token = 0;
	std::int32_t layer = -1;
	std::vector<std::int32_t> exact_routed_ids;
	std::uint64_t top_k = 0;
	std::uint64_t decision_timestamp_nanoseconds = 0;
	std::vector<bool> hard_resident_before;
	// The file-cache state of pread-backed expert sources is not exposed by the
	// runtime. nullopt is UNKNOWN, never an inferred false.
	std::optional<std::vector<bool>> soft_resident_estimate_before;
	std::uint64_t state_ready_timestamp_nanoseconds = 0;
	std::uint64_t execution_timestamp_nanoseconds = 0;
};

// Every value is optional by design: an unavailable physical quantity stays
// UNKNOWN instead of being silently converted to zero.
struct RuntimeResourceTelemetry {
	std::optional<std::uint64_t> current_resident_memory_bytes;
	std::optional<std::uint64_t> lifetime_peak_memory_bytes;
	std::optional<std::uint64_t> expert_cache_capacity;
	// Bytes addressable through currently active expert slots. A logical
	// runtime allocation metric, not a claim about physical residency.
	std::optional<std::uint64_t> expert_active_slot_bytes;
	std::optional<std::uint64_t> metal_current_allocated_bytes;
	std::optional<std::uint64_t> metal_recommended_working_set_bytes;
	std::optional<std::uint64_t> expert_cache_hits;
	std::optional<std::uint64_t> expert_cache_misses;
	std::optional<std::uint64_t> routed_expert_rows;
	std::optional<std::uint64_t> cpu_executed_expert_rows;
	std::optional<std::uint64_t> gpu_resident_expert_rows;
	std::optional<std::uint64_t> gpu_admitted_expert_rows;
	std::optional<std::uint64_t> routing_accounting_violations;
	std::optional<std::uint64_t> expert_evictions;
	std::optional<std::uint64_t> expert_bytes_loaded;
	// Exact bytes copied into accelerator expert slots: data movement, not a
	// claim that storage I/O occurred.
	std::optional<std::uint64_t> expert_admission_bytes;
	std::optional<std::uint64_t> expert_storage_load_nanoseconds;
	std::optional<std::uint64_t> prefill_expert_cache_hits;
	std::optional<std::uint64_t> prefill_expert_cache_misses;
	std::optional<std::uint64_t> prefill_expert_bytes_loaded;
	std::optional<std::uint64_t> decode_expert_cache_hits;
	std::optional<std::uint64_t> decode_expert_cache_misses;
	std::optional<std::uint64_t> decode_expert_bytes_loaded;
	std::optional<std::uint64_t> cpu_branch_nanoseconds;
	std::optional<std::uint64_t> metal_branch_wall_nanoseconds;
	std::optional<std::uint64_t> actual_simultaneous_execution_nanoseconds;
	std::optional<std::uint64_t> branch_critical_path_nanoseconds;
	std::optional<std::uint64_t> logical_bytes_requested;
	std::optional<std::uint64_t> api_read_bytes;
	std::optional<std::uint64_t> peak_transient_in_flight_bytes;
	std::optional<std::uint64_t> process_page_ins;
	std::optional<std::uint64_t> process_disk_read_bytes;
	std::vector<RouteTraceEntry> route_trace;
	bool route_trace_overflow = false;
};

// The runtime a loaded model runs in. The installed one is llama.cpp's
// llama-server, but neither its command line nor its HTTP payload escapes
// this boundary; a test stands in for it through the same contract.
class IGenerativeRuntimeAdapter {
public:
	virtual ~IGenerativeRuntimeAdapter() = default;
	virtual void start(const std::filesystem::path &model_path, const AdaptiveExecutionPlan &plan) = 0;
	virtual GenerativeResult generate(const GenerativeRequest &request) = 0;
	// Non-generating: measures this exact request through the same request
	// preparation generate() uses (real chat-template rendering, real
	// tokenizer) without decoding a single model token. The capacity oracle
	// uses it to decide what a working set may admit before a generation.
	virtual GenerativeRequestMeasurement measure(const GenerativeRequest &request) = 0;
	virtual bool supports_json_schema_output_constraint() const noexcept { return false; }
	virtual void adapt_to_memory_pressure(hardware::MemoryPressureLevel pressure) = 0;
	virtual RuntimeResourceTelemetry resource_telemetry() const noexcept = 0;
	virtual void stop() noexcept = 0;
	virtual bool running() const noexcept = 0;
};

class LlamaCppRuntimeAdapter final : public IGenerativeRuntimeAdapter {
public:
	// `log_directory` receives llama-server.log: Singularity's own data
	// folder, never another program's.
	explicit LlamaCppRuntimeAdapter(std::filesystem::path log_directory);
	~LlamaCppRuntimeAdapter() override;
	void start(const std::filesystem::path &model_path, const AdaptiveExecutionPlan &plan) override;
	GenerativeResult generate(const GenerativeRequest &request) override;
	GenerativeRequestMeasurement measure(const GenerativeRequest &request) override;
	bool supports_json_schema_output_constraint() const noexcept override;
	void adapt_to_memory_pressure(hardware::MemoryPressureLevel pressure) override;
	RuntimeResourceTelemetry resource_telemetry() const noexcept override;
	void stop() noexcept override;
	bool running() const noexcept override;
	const std::filesystem::path &runtime_directory() const noexcept;

private:
	std::filesystem::path runtime_directory_;
	std::filesystem::path log_directory_;
	std::filesystem::path slot_save_directory_;
	std::filesystem::path moe_control_path_;
	std::filesystem::path moe_telemetry_path_;
	std::filesystem::path moe_route_trace_path_;
	// The llama-server worker, owned for its whole life: it can never outlive
	// this process and never receives the terminal's Ctrl-C - see
	// Platform/ChildProcess for how each OS provides that guarantee. Guarded
	// by process_mutex_: the model's live state asks whether it runs from any
	// thread, while a load or an unload starts or ends it.
	mutable std::mutex process_mutex_;
	std::unique_ptr<platform::OwnedChildProcess> process_;
	unsigned short port_ = 0;
	std::string api_key_;
	// The real --ctx-size this running server was launched with, so
	// generate() can compute a real remaining-context ceiling for
	// GenerativeRequest::bound_output_by_remaining_context without asking.
	std::uint64_t context_window_tokens_ = 0;
	std::optional<std::string> last_prefix_id_;
	std::optional<StateBoundary> last_prefix_boundary_;
	std::uint64_t expert_cache_capacity_ = 0;
	std::uint64_t minimum_expert_cache_capacity_ = 0;
};

const char *to_string(StateBoundary boundary);

// The folder name every per-launch runtime folder of this process starts
// with (followed by the process id), so a folder a crashed Singularity left
// behind can be recognised and removed (Platform/StaleRuntimeReaper).
inline constexpr const char *kRuntimeSlotFolderPrefix = "local-inference-slots-";

// Bound each Metal prefill graph independently of the logical context/batch.
// This also covers fully resident MoE plans, whose strategy is DenseAccelerator.
std::uint64_t prompt_micro_batch_tokens(const AdaptiveExecutionPlan &plan);

// The lowest expert_cache_capacity that would ever be safe to run THIS
// plan's fixed --ubatch-size at: active_experts_per_token * ubatch, the
// exact bound the Metal MoE offloader enforces (aborting the whole process
// if violated). NOT consulted by any live control-file write - see
// decide_pressure_response() for why a live capacity resize is disabled.
// Kept as tested infrastructure for when that is fixed, and because it
// documents the invariant prompt_micro_batch_tokens() must never violate.
std::uint64_t pressure_safe_expert_cache_capacity(const AdaptiveExecutionPlan &plan);

// What a live memory-pressure signal does to an already-running plan. Pure,
// so the decision LlamaCppRuntimeAdapter::adapt_to_memory_pressure() applies
// is testable without a live server.
//
// It deliberately has no live expert-cache resize: real-model reproduction
// showed that any live resize through llama-server's control-file path -
// whatever the target value or ubatch - triggers a Metal command-buffer
// watchdog timeout on the next decode and poisons the backend
// (compute_error). The KV-erase under Warning was verified safe in
// isolation. Until that runtime defect is root-caused, the only safe policy
// is never to resize live; Critical pressure stays safe because it ends the
// process rather than resizing it.
enum class PressureAction {
	Continue,    // Nothing to change (Normal/Warning: no live resize is attempted).
	FailClosed,  // Critical: stop, never run unsafely.
};

struct PressureDecision {
	PressureAction action = PressureAction::Continue;
};

PressureDecision decide_pressure_response(
	hardware::MemoryPressureLevel pressure,
	std::uint64_t expert_cache_capacity);

// The one serialization path used by generate() and measure() and their
// deterministic tests. `supports_output_constraint` is supplied by the
// runtime adapter so a required constraint fails before any request is sent
// instead of degrading to ordinary generation.
std::string build_generation_request_body(const GenerativeRequest &request,
	bool supports_output_constraint);
// A rejection from the runtime, described by bounded metadata only - never
// the prompt or the response text.
std::string safe_generation_rejection_message(const GenerativeRequest &request,
	int http_status, const std::string &response_body, std::size_t request_bytes);

} // namespace singularity::local_inference
