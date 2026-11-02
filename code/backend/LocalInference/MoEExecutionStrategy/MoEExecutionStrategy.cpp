#include "LocalInference/MoEExecutionStrategy/MoEExecutionStrategy.hpp"

#include "LocalInference/DiscretePlacement/DiscretePlacement.hpp"

#include <algorithm>
#include <limits>

namespace singularity::local_inference {
namespace {

std::optional<std::uint64_t> state_bytes_for(const ModelExecutionProfile &model,
	std::uint64_t context_tokens) {
	std::uint64_t state = 256ULL * 1024 * 1024;
	if (!model.estimated_kv_bytes_per_token) return state;
	if (*model.estimated_kv_bytes_per_token >
		(std::numeric_limits<std::uint64_t>::max() - state) / context_tokens) return std::nullopt;
	return state + *model.estimated_kv_bytes_per_token * context_tokens;
}

} // namespace

AdaptiveExecutionPlan plan_moe_execution(
	const ModelExecutionProfile &model,
	const hardware::HardwareCapabilities &hardware,
	const hardware::HardwareCalibration &calibration,
	std::uint64_t requested_context_tokens,
	std::optional<hardware::ExecutionBackend> forced_backend) {
	AdaptiveExecutionPlan plan;
	plan.requested_context_tokens = requested_context_tokens;
	plan.estimated_model_bytes = model.model_state_bytes;
	plan.host_memory_budget_bytes = hardware::runtime_memory_budget_bytes(hardware);
	plan.accelerator_memory_budget_bytes = hardware.accelerator_memory_budget_bytes;
	plan.prefix_state_reuse = model.supports_prefix_state_reuse;
	if (requested_context_tokens == 0 ||
		(model.attention.context_length && requested_context_tokens > *model.attention.context_length)) {
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "The requested context is outside this model's inspected capability.";
		return plan;
	}
	const auto state = state_bytes_for(model, requested_context_tokens);
	if (!state) {
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "The model state requirement exceeds the measurable addressable budget.";
		return plan;
	}
	plan.estimated_state_bytes = *state;
	plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::KvState, *state, true, 0,
		std::nullopt, AdaptiveMemoryLifetime::Session, "runtime-session"});
	const auto expert_count = model.experts.expert_count.value_or(0);
	const auto active = model.experts.active_experts_per_token.value_or(0);
	const auto per_expert = model.experts.estimated_bytes_per_expert.value_or(0);
	const auto non_expert = model.experts.non_expert_state_bytes.value_or(0);
	if (!model.experts.exact_tensor_layout || expert_count == 0 || active == 0 || per_expert == 0 ||
		non_expert == 0) {
		plan.reason = "The model does not expose an exact correctness-preserving expert layout.";
		return plan;
	}
	const bool cpu_supported = hardware.supports(hardware::ExecutionBackend::Cpu) &&
		std::find(model.supported_backends.begin(), model.supported_backends.end(), hardware::ExecutionBackend::Cpu) !=
		model.supported_backends.end();
	const bool metal_supported = hardware.apple_silicon && hardware.unified_memory &&
		hardware.supports(hardware::ExecutionBackend::AppleAccelerator) && model.supports_dynamic_expert_residency;
	if (forced_backend == hardware::ExecutionBackend::Cpu) {
		if (!cpu_supported || model.model_state_bytes > plan.host_memory_budget_bytes ||
			model.model_state_bytes + *state > plan.host_memory_budget_bytes) {
			plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
			plan.reason = "The complete MoE source of truth does not safely fit the CPU execution budget.";
			return plan;
		}
		plan.backend = hardware::ExecutionBackend::Cpu;
		plan.strategy = ExecutionStrategy::SparseCpu;
		plan.cpu_worker_budget = std::max(1U, hardware.physical_cores);
		plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::ModelState,
			model.model_state_bytes, false, 0, std::nullopt,
			AdaptiveMemoryLifetime::Model, "loaded-model"});
		plan.plan_identity = "sparse_cpu:all_experts";
		plan.feasibility = ExecutionFeasibility::ReducedPerformance;
		plan.reason = "CPU is the requested correctness reference and keeps the complete expert pool resident.";
		return plan;
	}
	// A discrete accelerator (its own memory, e.g. a PCIe GPU) with a runtime
	// that can split a model: keep every non-expert tensor, all KV state and
	// as many blocks' routed experts as fit in accelerator memory, and the
	// remaining blocks' experts in host memory on the CPU. The same weights
	// and arithmetic as full residency - placement only.
	std::optional<hardware::ExecutionBackend> discrete_backend;
	if (!hardware.unified_memory && model.supports_hybrid_placement && plan.accelerator_memory_budget_bytes) {
		for (const auto backend : model.supported_backends) {
			if (backend == hardware::ExecutionBackend::Cpu || !hardware.supports(backend)) continue;
			if (forced_backend && *forced_backend != backend) continue;
			discrete_backend = backend;
			break;
		}
	}
	if (discrete_backend) {
		const bool accelerator_measured_faster =
			(calibration.accelerator_execution_giga_operations_per_second &&
				(!calibration.cpu_execution_giga_operations_per_second ||
				 *calibration.accelerator_execution_giga_operations_per_second >
					*calibration.cpu_execution_giga_operations_per_second)) ||
			(calibration.accelerator_memory_bandwidth_bytes_per_second &&
				(!calibration.memory_bandwidth_bytes_per_second ||
				 *calibration.accelerator_memory_bandwidth_bytes_per_second >
					*calibration.memory_bandwidth_bytes_per_second));
		const std::uint64_t kv_bytes = *state - 256ULL * 1024 * 1024;
		const auto split = plan_expert_host_split(model.placement, kv_bytes, *plan.accelerator_memory_budget_bytes);
		if (split && (accelerator_measured_faster || forced_backend)) {
			constexpr std::uint64_t kHostRuntimeStateBytes = 256ULL * 1024 * 1024;
			if (split->host_bytes > plan.host_memory_budget_bytes ||
				kHostRuntimeStateBytes > plan.host_memory_budget_bytes - split->host_bytes) {
				plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
				plan.reason = "The expert weights that must stay in host memory exceed the safe memory budget.";
				return plan;
			}
			plan.backend = *discrete_backend;
			plan.accelerator_block_count = split->total_blocks;
			plan.accelerator_head = true;
			// Two separate pools: the KV state, the runtime's own reserve and
			// every accelerator-resident tensor live in the accelerator's memory;
			// the host-resident experts (with the token embedding) and the
			// runtime's host state in host memory. Each byte is counted once, in
			// the pool it occupies.
			plan.memory_consumers = {
				{AdaptiveMemoryConsumerKind::KvState, kv_bytes, true, 0, std::nullopt, AdaptiveMemoryLifetime::Session,
					"runtime-session", AdaptiveMemoryPool::Accelerator},
				{AdaptiveMemoryConsumerKind::Scratch, kHostRuntimeStateBytes, false, 0, std::nullopt,
					AdaptiveMemoryLifetime::Session, "host-runtime-state", AdaptiveMemoryPool::Host},
				{AdaptiveMemoryConsumerKind::Scratch, kDiscreteAcceleratorRuntimeReserveBytes, false, 0, std::nullopt,
					AdaptiveMemoryLifetime::Model, "accelerator-runtime-reserve", AdaptiveMemoryPool::Accelerator},
				{AdaptiveMemoryConsumerKind::ModelState,
					split->accelerator_bytes - kDiscreteAcceleratorRuntimeReserveBytes - kv_bytes, false, 0, std::nullopt,
					AdaptiveMemoryLifetime::Model, "accelerator-resident-model-state", AdaptiveMemoryPool::Accelerator}};
			if (split->host_expert_blocks == 0) {
				plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::ModelState, split->host_bytes, false, 0,
					std::nullopt, AdaptiveMemoryLifetime::Model, "host-resident-model-state", AdaptiveMemoryPool::Host});
				plan.strategy = ExecutionStrategy::DenseAccelerator;
				plan.plan_identity = to_string(*plan.strategy);
				plan.feasibility = ExecutionFeasibility::Recommended;
				plan.reason = "The complete MoE model safely fits the accelerator's own memory.";
				return plan;
			}
			plan.strategy = ExecutionStrategy::SparseHybrid;
			plan.host_expert_block_count = split->host_expert_blocks;
			plan.cpu_worker_budget = std::max(1U, hardware.physical_cores);
			plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::ExpertResidency, split->host_bytes,
				false, 0, std::nullopt, AdaptiveMemoryLifetime::Model, "host-resident-experts", AdaptiveMemoryPool::Host});
			plan.plan_identity = std::string(to_string(*plan.strategy)) + ":host-expert-blocks=" +
				std::to_string(split->host_expert_blocks) + "/" + std::to_string(split->total_blocks);
			plan.feasibility = ExecutionFeasibility::ReducedPerformance;
			plan.reason = "The complete MoE model does not fit the accelerator's own memory: the routed experts of its first " +
				std::to_string(split->host_expert_blocks) + " of " + std::to_string(split->total_blocks) +
				" blocks run on the CPU from host memory, and everything else runs on the accelerator.";
			return plan;
		}
	}
	if (forced_backend && *forced_backend != hardware::ExecutionBackend::AppleAccelerator) {
		plan.reason = "The requested backend has no installed adaptive MoE strategy.";
		return plan;
	}
	if (!metal_supported) {
		// No accelerator path applies here (no accelerator, no measured
		// advantage, or no fitting placement): the CPU still runs the model
		// exactly when the complete expert pool fits host memory.
		if (cpu_supported && model.model_state_bytes <= plan.host_memory_budget_bytes &&
			*state <= plan.host_memory_budget_bytes - model.model_state_bytes) {
			plan.backend = hardware::ExecutionBackend::Cpu;
			plan.strategy = ExecutionStrategy::SparseCpu;
			plan.cpu_worker_budget = std::max(1U, hardware.physical_cores);
			plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::ModelState,
				model.model_state_bytes, false, 0, std::nullopt,
				AdaptiveMemoryLifetime::Model, "loaded-model"});
			plan.plan_identity = "sparse_cpu:all_experts";
			plan.feasibility = ExecutionFeasibility::ReducedPerformance;
			plan.reason = "No accelerator placement applies on this machine; the CPU runs the model with its complete expert pool resident.";
			return plan;
		}
		plan.reason = "This platform/runtime does not expose correctness-preserving dynamic expert residency.";
		return plan;
	}
	if (non_expert > plan.host_memory_budget_bytes || *state > plan.host_memory_budget_bytes - non_expert) {
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "The non-expert weights and required runtime state exceed the safe memory budget.";
		return plan;
	}
	// On Apple Silicon CPU and Metal share physical memory, but Metal's
	// recommended working set may be the tighter safe execution boundary.
	// Respect both without inventing a discrete-VRAM topology.
	std::uint64_t execution_budget = plan.host_memory_budget_bytes;
	if (plan.accelerator_memory_budget_bytes) {
		execution_budget = std::min(execution_budget, *plan.accelerator_memory_budget_bytes);
	}
	if (non_expert > execution_budget || *state > execution_budget - non_expert) {
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "The non-expert weights and required runtime state exceed the safe Metal working set.";
		return plan;
	}
	const std::uint64_t available_for_experts = execution_budget - non_expert - *state;
	const std::uint64_t maximum_safe_capacity =
		std::min(expert_count, available_for_experts / per_expert);
	if (maximum_safe_capacity < active) {
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "The safe budget cannot hold every expert routed for one token.";
		return plan;
	}
	// Full residency is unambiguously preferable when it fits. Otherwise do
	// not turn every byte of currently-free memory into a permanent cache on
	// first load: begin with two routed working sets (or the largest smaller
	// safe pool), then let cached real-model candidate evidence tune it.
	const std::uint64_t initial_sparse_capacity = active > expert_count / 2
		? expert_count : active * 2;
	const std::uint64_t capacity = maximum_safe_capacity == expert_count
		? expert_count : std::min(maximum_safe_capacity, initial_sparse_capacity);
	plan.backend = hardware::ExecutionBackend::AppleAccelerator;
	plan.expert_cache_capacity = capacity;
	plan.minimum_expert_cache_capacity = active;
	plan.expert_cache_bytes = capacity * per_expert;
	if (capacity < expert_count) plan.maximum_safe_expert_cache_capacity = maximum_safe_capacity;
	plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::ModelState, non_expert, false, 0,
		std::nullopt, AdaptiveMemoryLifetime::Model, "non-expert-model-state"});
	plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::ExpertResidency,
		*plan.expert_cache_bytes, capacity > active, 0, std::nullopt,
		AdaptiveMemoryLifetime::Model, "adaptive-expert-pool"});
	const bool heterogeneous_calibrated =
		calibration.cpu_execution_giga_operations_per_second &&
		calibration.accelerator_execution_giga_operations_per_second &&
		calibration.memory_bandwidth_bytes_per_second &&
		calibration.accelerator_memory_bandwidth_bytes_per_second;
	// The mixed CPU+Metal runtime uses CPU-mapped expert tensors as its source
	// of truth. If inspected all-resident state already exceeds the safe
	// execution budget, use the pinned file-backed runtime that loads only the
	// compact expert working set instead of risking a Metal load-time OOM.
	const bool requires_file_backed_expert_source =
		model.model_state_bytes > plan.host_memory_budget_bytes - *state;
	plan.strategy = capacity < expert_count
		? (heterogeneous_calibrated && !requires_file_backed_expert_source
			? ExecutionStrategy::SparseHeterogeneous
			: ExecutionStrategy::SparseExpertResidency)
		: ExecutionStrategy::DenseAccelerator;
	if (plan.strategy == ExecutionStrategy::SparseHeterogeneous) {
		plan.cpu_worker_budget = std::max(1U, hardware.physical_cores);
		plan.concurrent_cpu_accelerator = true;
		plan.cpu_execution_giga_operations_per_second =
			calibration.cpu_execution_giga_operations_per_second;
		plan.accelerator_execution_giga_operations_per_second =
			calibration.accelerator_execution_giga_operations_per_second;
		// Admission copies exact expert bytes within unified memory. Model that
		// from the slower of the two measured memory paths; never invent a PCIe
		// transfer rate on Apple Silicon.
		if (calibration.memory_bandwidth_bytes_per_second &&
			calibration.accelerator_memory_bandwidth_bytes_per_second) {
			plan.expert_admission_gigabytes_per_second = static_cast<double>(std::min(
				*calibration.memory_bandwidth_bytes_per_second,
				*calibration.accelerator_memory_bandwidth_bytes_per_second)) / 1'000'000'000.0;
		}
		if (calibration.accelerator_command_overhead_microseconds) {
			const auto command_nanoseconds = static_cast<std::uint64_t>(
				*calibration.accelerator_command_overhead_microseconds * 1000.0);
			plan.accelerator_dispatch_nanoseconds = command_nanoseconds;
			plan.accelerator_synchronization_nanoseconds = command_nanoseconds;
		}
		// Large Metal prefill graphs can exceed the command-buffer watchdog even
		// when all weights fit. Bound every microbatch's routed work (active
		// experts per token times tokens in the microbatch) by a fixed budget
		// rather than letting it grow with prompt length; context capacity and
		// routing stay unchanged. This runtime spreads routed compute between
		// CPU and Metal, so it is not bound by expert_cache_capacity itself.
		plan.prompt_micro_batch_tokens = std::max<std::uint64_t>(1, std::min<std::uint64_t>(32, 64 / active));
	} else if (plan.strategy == ExecutionStrategy::SparseExpertResidency) {
		// The pinned Metal MoE offloader (third_party/llama.cpp's
		// b10516-moe-expert-cache.patch, llama_moe_offloader::resolve()) aborts the
		// whole runtime process - GGML_ABORT("moe_offloader: n_slots too small
		// for ubatch") - if one microbatch routes to more distinct experts than
		// expert_cache_capacity holds. Worst case, `ubatch` tokens route to
		// `active` entirely disjoint experts each, so
		// active_experts_per_token * ubatch <= capacity is the only
		// correctness-preserving bound; reproduced live (real crash, real
		// backtrace) at ubatch=32 against a 16-slot cache on Qwen3-30B-A3B.
		// This replaces the previous unconditional ubatch=1, which paid full
		// per-token Metal dispatch/sync overhead on every prompt token
		// regardless of prompt length or cache capacity.
		plan.prompt_micro_batch_tokens = std::max<std::uint64_t>(1, capacity / active);
	}
	plan.plan_identity = std::string(to_string(*plan.strategy)) + ":slots=" + std::to_string(capacity);
	if (plan.prompt_micro_batch_tokens)
		plan.plan_identity += ":ubatch=" + std::to_string(*plan.prompt_micro_batch_tokens);
	if (plan.cpu_worker_budget) plan.plan_identity += ":cpu-workers=" +
		std::to_string(*plan.cpu_worker_budget) + ":split=adaptive";
	plan.feasibility = capacity < expert_count ? ExecutionFeasibility::ReducedPerformance
		: ExecutionFeasibility::Recommended;
	plan.reason = capacity < expert_count
		? (plan.strategy == ExecutionStrategy::SparseHeterogeneous
			? "A measured compact heterogeneous expert pool makes the inspected model safe; routed work is assigned between CPU, resident Metal, and exact admission followed by Metal."
			: (requires_file_backed_expert_source
				? "A compact file-backed Metal expert pool makes the inspected oversized model safe without mapping its complete expert source into the mixed runtime working set."
				: "A compact Metal expert pool makes the inspected model safe; exact measured heterogeneous assignment is unavailable, so the runtime uses its correctness-preserving Metal-only fallback."))
		: "The complete MoE model safely fits unified memory; full residency avoids unnecessary expert I/O.";
	return plan;
}

} // namespace singularity::local_inference
