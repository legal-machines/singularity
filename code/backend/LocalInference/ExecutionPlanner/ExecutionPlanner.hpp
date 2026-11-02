#pragma once

#include "LocalInference/ModelInspector/ModelInspector.hpp"
#include "LocalModel/ExecutionBackend/ExecutionBackend.hpp"
#include "LocalModel/ExecutionCompatibility/ExecutionCompatibility.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "Node/HardwareCalibration/HardwareCalibration.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::local_inference {

// How one generative model is executed on this machine: the planner layer on
// top of the shared compatibility policy (LocalModel/ExecutionCompatibility),
// whose memory budget every plan is held to. A plan is built from the model's
// inspected facts, the machine's current resource signals and its measured
// calibration - never from a model's name.

enum class ExecutionStrategy {
	DenseAccelerator,
	DenseCpu,
	// Discrete accelerator: the last accelerator_block_count blocks (with
	// their KV state) in accelerator memory, the rest in host memory on CPU.
	DenseHybrid,
	SparseExpertResidency,
	SparseHeterogeneous,
	SparseCpu,
	// Discrete accelerator, MoE: every non-expert tensor, all KV state and
	// the routed experts of the later blocks in accelerator memory; the
	// routed experts of the first host_expert_block_count blocks in host
	// memory, executed by the CPU. Exact - the same weights and arithmetic -
	// only the placement differs.
	SparseHybrid,
};

enum class ExecutionFeasibility {
	Recommended,
	ReducedPerformance,
	NotSafelyRunnable,
	Unsupported,
};

enum class AdaptiveMemoryConsumerKind {
	ModelState,
	ExpertResidency,
	KvState,
	PrefixCheckpoint,
	Scratch,
	Prefetch,
};

enum class AdaptiveMemoryLifetime { Model, Session, Request, Temporary };

// The physical pool a consumer's bytes occupy. Unified memory has one pool,
// the host's; a discrete accelerator has its own beside it, and a byte lives
// in exactly one of the two.
enum class AdaptiveMemoryPool { Host, Accelerator };

struct AdaptiveMemoryConsumer {
	AdaptiveMemoryConsumerKind kind = AdaptiveMemoryConsumerKind::ModelState;
	std::uint64_t bytes = 0;
	bool reclaimable = false;
	std::uint64_t rebuild_cost_nanoseconds = 0;
	std::optional<double> expected_reuse_value;
	AdaptiveMemoryLifetime lifetime = AdaptiveMemoryLifetime::Model;
	std::string owner;
	AdaptiveMemoryPool pool = AdaptiveMemoryPool::Host;
};

// What one plan occupies in each pool: the sum of its consumers per pool.
// On unified memory everything is host memory.
struct AdaptiveMemoryFootprint {
	std::uint64_t host_bytes = 0;
	std::uint64_t accelerator_bytes = 0;
};

struct AdaptiveExecutionPlan {
	ExecutionFeasibility feasibility = ExecutionFeasibility::Unsupported;
	std::optional<ExecutionStrategy> strategy;
	std::optional<hardware::ExecutionBackend> backend;
	std::uint64_t requested_context_tokens = 0;
	std::uint64_t estimated_model_bytes = 0;
	std::uint64_t estimated_state_bytes = 0;
	std::uint64_t host_memory_budget_bytes = 0;
	std::optional<std::uint64_t> accelerator_memory_budget_bytes;
	bool double_buffering = false;
	bool prefix_state_reuse = false;
	std::optional<std::uint64_t> expert_cache_bytes;
	std::optional<std::uint64_t> expert_cache_capacity;
	std::optional<std::uint64_t> minimum_expert_cache_capacity;
	// The largest expert_cache_capacity that still fits the safe execution
	// budget computed for THIS plan (memory- and pressure-aware; recomputed
	// on every load). Present only for Metal MoE strategies. Candidate
	// generation may propose a wider cache up to this ceiling, but must never
	// exceed it - see GenerativeModelService.cpp's "wide" MoE candidate.
	std::optional<std::uint64_t> maximum_safe_expert_cache_capacity;
	// How many prompt tokens the runtime evaluates per Metal microbatch/graph
	// for this plan. A real, measurable, safety-bound execution-plan
	// dimension - not a runtime-adapter implementation detail - because for
	// the Metal MoE offloader (SparseExpertResidency), a too-large value is
	// not merely slow but a hard correctness violation: the offloader aborts
	// the whole runtime process if one microbatch routes to more distinct
	// experts than expert_cache_capacity holds. std::nullopt means "use the
	// runtime's pinned llama.cpp default" (512, or the SparseHeterogeneous
	// routed-work bound - see GenerativeRuntime.cpp).
	std::optional<std::uint64_t> prompt_micro_batch_tokens;
	// Present only after the planner selected the Apple unified-memory
	// heterogeneous strategy. These are measured inputs for fine-grained
	// routed-work assignment, not a second coarse planner.
	std::optional<double> cpu_execution_giga_operations_per_second;
	std::optional<double> accelerator_execution_giga_operations_per_second;
	std::optional<double> expert_admission_gigabytes_per_second;
	std::optional<std::uint64_t> accelerator_dispatch_nanoseconds;
	std::optional<std::uint64_t> accelerator_synchronization_nanoseconds;
	std::optional<std::uint64_t> merge_nanoseconds_per_row;
	// The coarse plan defines the legal envelope. The fine unified-memory
	// scheduler may choose less CPU work but may never exceed this worker budget.
	std::optional<std::uint32_t> cpu_worker_budget;
	std::optional<double> target_cpu_work_fraction;
	bool concurrent_cpu_accelerator = false;
	// Discrete-accelerator placement (DenseHybrid / SparseHybrid), from the
	// model's exact per-block sizes - see LocalInference/DiscretePlacement.
	// accelerator_block_count counts blocks on the accelerator;
	// accelerator_head is true when the head joined them (only possible with
	// every block). host_expert_block_count counts blocks whose routed
	// experts stay in host memory.
	std::optional<std::uint64_t> accelerator_block_count;
	bool accelerator_head = false;
	std::optional<std::uint64_t> host_expert_block_count;
	std::vector<AdaptiveMemoryConsumer> memory_consumers;
	std::string plan_identity;
	std::string reason;
};

// Builds a model-name-independent execution plan from inspected model facts,
// current resource signals and measured calibration. It never asks the
// runtime to perform a placement mechanism the runtime does not have.
AdaptiveExecutionPlan plan_model_execution(
	const ModelExecutionProfile &model,
	const hardware::HardwareCapabilities &hardware,
	const hardware::HardwareCalibration &calibration,
	std::uint64_t requested_context_tokens,
	std::optional<hardware::ExecutionBackend> forced_backend = std::nullopt);

const char *to_string(ExecutionStrategy strategy);
const char *to_string(ExecutionFeasibility feasibility);
const char *to_string(AdaptiveMemoryConsumerKind kind);
const char *to_string(AdaptiveMemoryLifetime lifetime);
const char *to_string(AdaptiveMemoryPool pool);

// The plan's consumers summed per pool, saturating rather than wrapping.
AdaptiveMemoryFootprint memory_footprint(const AdaptiveExecutionPlan &plan);

} // namespace singularity::local_inference
