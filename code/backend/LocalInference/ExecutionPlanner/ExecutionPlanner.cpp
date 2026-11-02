#include "LocalInference/ExecutionPlanner/ExecutionPlanner.hpp"

#include "LocalInference/DenseExecutionStrategy/DenseExecutionStrategy.hpp"
#include "LocalInference/MoEExecutionStrategy/MoEExecutionStrategy.hpp"

#include <limits>

namespace singularity::local_inference {

const char *to_string(ExecutionStrategy strategy) {
	switch (strategy) {
		case ExecutionStrategy::DenseAccelerator: return "dense_accelerator";
		case ExecutionStrategy::DenseCpu: return "dense_cpu";
		case ExecutionStrategy::DenseHybrid: return "dense_hybrid";
		case ExecutionStrategy::SparseExpertResidency: return "sparse_expert_residency";
		case ExecutionStrategy::SparseHeterogeneous: return "sparse_heterogeneous";
		case ExecutionStrategy::SparseCpu: return "sparse_cpu";
		case ExecutionStrategy::SparseHybrid: return "sparse_hybrid";
	}
	return "unknown";
}

const char *to_string(ExecutionFeasibility feasibility) {
	switch (feasibility) {
		case ExecutionFeasibility::Recommended: return "recommended";
		case ExecutionFeasibility::ReducedPerformance: return "reduced_performance";
		case ExecutionFeasibility::NotSafelyRunnable: return "not_safely_runnable";
		case ExecutionFeasibility::Unsupported: return "unsupported";
	}
	return "unsupported";
}

const char *to_string(AdaptiveMemoryConsumerKind kind) {
	switch (kind) {
		case AdaptiveMemoryConsumerKind::ModelState: return "model_state";
		case AdaptiveMemoryConsumerKind::ExpertResidency: return "expert_residency";
		case AdaptiveMemoryConsumerKind::KvState: return "kv_state";
		case AdaptiveMemoryConsumerKind::PrefixCheckpoint: return "prefix_checkpoint";
		case AdaptiveMemoryConsumerKind::Scratch: return "scratch";
		case AdaptiveMemoryConsumerKind::Prefetch: return "prefetch";
	}
	return "unknown";
}

const char *to_string(AdaptiveMemoryLifetime lifetime) {
	switch (lifetime) {
		case AdaptiveMemoryLifetime::Model: return "model";
		case AdaptiveMemoryLifetime::Session: return "session";
		case AdaptiveMemoryLifetime::Request: return "request";
		case AdaptiveMemoryLifetime::Temporary: return "temporary";
	}
	return "unknown";
}

const char *to_string(AdaptiveMemoryPool pool) {
	switch (pool) {
		case AdaptiveMemoryPool::Host: return "host";
		case AdaptiveMemoryPool::Accelerator: return "accelerator";
	}
	return "unknown";
}

AdaptiveMemoryFootprint memory_footprint(const AdaptiveExecutionPlan &plan) {
	AdaptiveMemoryFootprint footprint;
	for (const auto &consumer : plan.memory_consumers) {
		auto &total = consumer.pool == AdaptiveMemoryPool::Accelerator
			? footprint.accelerator_bytes : footprint.host_bytes;
		total = consumer.bytes > std::numeric_limits<std::uint64_t>::max() - total
			? std::numeric_limits<std::uint64_t>::max() : total + consumer.bytes;
	}
	return footprint;
}

AdaptiveExecutionPlan plan_model_execution(
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

	if (!model.is_supported) {
		plan.reason = model.support_reason.empty() ? "The model format or architecture is unsupported." : model.support_reason;
		return plan;
	}
	if (model.architecture == ModelArchitecture::Dense) {
		return plan_dense_execution(model, hardware, calibration, requested_context_tokens, forced_backend);
	}
	if (model.architecture == ModelArchitecture::MixtureOfExperts) {
		return plan_moe_execution(model, hardware, calibration, requested_context_tokens, forced_backend);
	}
	plan.reason = "The model architecture is unknown; Singularity will not guess an execution strategy.";
	return plan;
}

} // namespace singularity::local_inference
