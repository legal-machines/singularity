#include "LocalInference/DenseExecutionStrategy/DenseExecutionStrategy.hpp"

#include "LocalInference/DiscretePlacement/DiscretePlacement.hpp"

#include <algorithm>
#include <limits>

namespace singularity::local_inference {

AdaptiveExecutionPlan plan_dense_execution(
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

	std::uint64_t state_bytes = 256ULL * 1024 * 1024;
	std::uint64_t kv_bytes = 0;
	if (model.estimated_kv_bytes_per_token) {
		if (*model.estimated_kv_bytes_per_token >
			std::numeric_limits<std::uint64_t>::max() / requested_context_tokens) {
			plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
			plan.reason = "The model state requirement exceeds the measurable addressable budget.";
			return plan;
		}
		kv_bytes = *model.estimated_kv_bytes_per_token * requested_context_tokens;
		state_bytes += kv_bytes;
	}
	plan.estimated_state_bytes = state_bytes;
	plan.memory_consumers = {
		{AdaptiveMemoryConsumerKind::ModelState, model.model_state_bytes, false, 0,
			std::nullopt, AdaptiveMemoryLifetime::Model, "loaded-model"},
		{AdaptiveMemoryConsumerKind::KvState, state_bytes, true, 0,
			std::nullopt, AdaptiveMemoryLifetime::Session, "runtime-session"}};
	if (model.model_state_bytes > std::numeric_limits<std::uint64_t>::max() - state_bytes) {
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "This model needs more memory than is safely available right now.";
		return plan;
	}
	// What host memory must hold is decided with the placement below: all of
	// it on the CPU or on unified memory, only the host's part of a discrete
	// placement.

	auto supports_model_backend = [&](hardware::ExecutionBackend backend) {
		return hardware.supports(backend) &&
			std::find(model.supported_backends.begin(), model.supported_backends.end(), backend) !=
			model.supported_backends.end();
	};
	if (forced_backend && !supports_model_backend(*forced_backend)) {
		plan.reason = "The requested execution backend is not supported by this model/runtime on this machine.";
		return plan;
	}

	// The accelerator both the model's runtime and this machine support, in
	// the model's own preference order: Metal, CUDA or any later backend -
	// never a vendor hard-coded here. A forced accelerator is that one.
	std::optional<hardware::ExecutionBackend> accelerator_backend;
	if (forced_backend && *forced_backend != hardware::ExecutionBackend::Cpu) {
		accelerator_backend = forced_backend;
	} else {
		for (const auto backend : model.supported_backends) {
			if (backend != hardware::ExecutionBackend::Cpu && supports_model_backend(backend)) {
				accelerator_backend = backend;
				break;
			}
		}
	}
	const bool accelerator_available = accelerator_backend.has_value();
	const bool accelerator_compute_faster = calibration.accelerator_execution_giga_operations_per_second &&
		(!calibration.cpu_execution_giga_operations_per_second ||
		 *calibration.accelerator_execution_giga_operations_per_second >
			*calibration.cpu_execution_giga_operations_per_second);
	const bool accelerator_bandwidth_faster = calibration.accelerator_memory_bandwidth_bytes_per_second &&
		(!calibration.memory_bandwidth_bytes_per_second ||
		 *calibration.accelerator_memory_bandwidth_bytes_per_second >
			*calibration.memory_bandwidth_bytes_per_second);
	const bool accelerator_measured_faster = accelerator_compute_faster || accelerator_bandwidth_faster;
	// A discrete accelerator owns separate memory: place the model by its
	// exact block sizes (see LocalModel/DiscretePlacement). Unified memory has
	// nothing to split.
	std::optional<DenseAcceleratorSplit> split;
	if (accelerator_available && !hardware.unified_memory && model.supports_hybrid_placement &&
		plan.accelerator_memory_budget_bytes) {
		split = plan_dense_accelerator_split(model.placement, kv_bytes, *plan.accelerator_memory_budget_bytes);
	}
	const bool accelerator_fits = hardware.unified_memory
		? accelerator_available
		: split ? split->head_on_accelerator
		: accelerator_available && plan.accelerator_memory_budget_bytes &&
			model.model_state_bytes + state_bytes <= *plan.accelerator_memory_budget_bytes;
	const bool partial_fit = split && split->accelerator_blocks > 0 && !split->head_on_accelerator;

	hardware::ExecutionBackend selected = hardware::ExecutionBackend::Cpu;
	ExecutionStrategy strategy = ExecutionStrategy::DenseCpu;
	if (forced_backend) {
		selected = *forced_backend;
		if (selected != hardware::ExecutionBackend::Cpu && !accelerator_fits && !partial_fit) {
			plan.reason = "The requested accelerator placement does not safely fit and the runtime has no hybrid path.";
			return plan;
		}
		strategy = selected == hardware::ExecutionBackend::Cpu ? ExecutionStrategy::DenseCpu :
			(accelerator_fits ? ExecutionStrategy::DenseAccelerator : ExecutionStrategy::DenseHybrid);
	} else if (accelerator_fits && accelerator_measured_faster) {
		selected = *accelerator_backend;
		strategy = ExecutionStrategy::DenseAccelerator;
	} else if (partial_fit && accelerator_measured_faster) {
		selected = *accelerator_backend;
		strategy = ExecutionStrategy::DenseHybrid;
	} else if (!supports_model_backend(hardware::ExecutionBackend::Cpu)) {
		plan.reason = "No measured, supported execution path is available on this machine.";
		return plan;
	}
	constexpr std::uint64_t kHostRuntimeStateBytes = 256ULL * 1024 * 1024;
	if (split && selected != hardware::ExecutionBackend::Cpu) {
		plan.accelerator_block_count = split->accelerator_blocks;
		plan.accelerator_head = split->head_on_accelerator;
		// Two separate pools, each byte counted once where it lives: the
		// accelerator holds its blocks, their KV state, the head when every
		// block is there, and the runtime's own reserve; host memory holds the
		// rest of the model, the rest of the KV state and the runtime's state.
		const std::uint64_t accelerator_kv = kv_bytes / split->total_blocks * split->accelerator_blocks;
		const std::uint64_t host_kv = kv_bytes / split->total_blocks * (split->total_blocks - split->accelerator_blocks);
		plan.memory_consumers = {
			{AdaptiveMemoryConsumerKind::ModelState, split->host_bytes - host_kv, false, 0, std::nullopt,
				AdaptiveMemoryLifetime::Model, "host-resident-model-state", AdaptiveMemoryPool::Host},
			{AdaptiveMemoryConsumerKind::Scratch, kHostRuntimeStateBytes, false, 0, std::nullopt,
				AdaptiveMemoryLifetime::Session, "host-runtime-state", AdaptiveMemoryPool::Host}};
		if (host_kv > 0) {
			plan.memory_consumers.push_back({AdaptiveMemoryConsumerKind::KvState, host_kv, true, 0, std::nullopt,
				AdaptiveMemoryLifetime::Session, "runtime-session-host-blocks", AdaptiveMemoryPool::Host});
		}
		if (split->accelerator_blocks > 0) {
			plan.memory_consumers.insert(plan.memory_consumers.end(), {
				{AdaptiveMemoryConsumerKind::KvState, accelerator_kv, true, 0, std::nullopt,
					AdaptiveMemoryLifetime::Session, "runtime-session", AdaptiveMemoryPool::Accelerator},
				{AdaptiveMemoryConsumerKind::Scratch, kDiscreteAcceleratorRuntimeReserveBytes, false, 0, std::nullopt,
					AdaptiveMemoryLifetime::Model, "accelerator-runtime-reserve", AdaptiveMemoryPool::Accelerator},
				{AdaptiveMemoryConsumerKind::ModelState,
					split->accelerator_bytes - kDiscreteAcceleratorRuntimeReserveBytes - accelerator_kv, false, 0,
					std::nullopt, AdaptiveMemoryLifetime::Model, "accelerator-resident-model-state",
					AdaptiveMemoryPool::Accelerator}});
		}
	} else if (selected != hardware::ExecutionBackend::Cpu && !hardware.unified_memory) {
		// No exact placement: the whole model and its KV state go to the
		// discrete accelerator (accelerator_fits checked them against its
		// budget), and host memory holds only the runtime's own state.
		plan.memory_consumers = {
			{AdaptiveMemoryConsumerKind::ModelState, model.model_state_bytes, false, 0, std::nullopt,
				AdaptiveMemoryLifetime::Model, "accelerator-resident-model-state", AdaptiveMemoryPool::Accelerator},
			{AdaptiveMemoryConsumerKind::KvState, kv_bytes, true, 0, std::nullopt,
				AdaptiveMemoryLifetime::Session, "runtime-session", AdaptiveMemoryPool::Accelerator},
			{AdaptiveMemoryConsumerKind::Scratch, kHostRuntimeStateBytes, false, 0, std::nullopt,
				AdaptiveMemoryLifetime::Session, "host-runtime-state", AdaptiveMemoryPool::Host}};
	}
	if (memory_footprint(plan).host_bytes > plan.host_memory_budget_bytes) {
		plan.accelerator_block_count.reset();
		plan.accelerator_head = false;
		plan.feasibility = ExecutionFeasibility::NotSafelyRunnable;
		plan.reason = "This model needs more memory than is safely available right now.";
		return plan;
	}

	plan.backend = selected;
	plan.strategy = strategy;
	plan.cpu_worker_budget = std::max(1U, hardware.physical_cores);
	plan.plan_identity = to_string(strategy);
	if (strategy == ExecutionStrategy::DenseHybrid && plan.accelerator_block_count && split) {
		plan.plan_identity += ":accelerator-blocks=" + std::to_string(*plan.accelerator_block_count) + "/" +
			std::to_string(split->total_blocks);
	}
	plan.feasibility = (strategy == ExecutionStrategy::DenseCpu && accelerator_available) ||
			strategy == ExecutionStrategy::DenseHybrid
		? ExecutionFeasibility::ReducedPerformance : ExecutionFeasibility::Recommended;
	plan.double_buffering = false;
	plan.reason = strategy == ExecutionStrategy::DenseAccelerator
		? (hardware.unified_memory
			? "The inspected model safely fits unified memory and measured accelerator compute or memory bandwidth is faster."
			: "The inspected model safely fits the accelerator's own memory and measured accelerator compute or memory bandwidth is faster.")
		: strategy == ExecutionStrategy::DenseHybrid
			? "The complete model does not fit the accelerator's own memory: its last " +
				std::to_string(plan.accelerator_block_count.value_or(0)) +
				" blocks run there and the rest run on the CPU from host memory."
			: forced_backend
				? "CPU is the requested real fallback/comparison path and fits the safe memory budget."
				: "CPU is the fastest safe supported plan for the current measured resources.";
	return plan;
}

} // namespace singularity::local_inference
