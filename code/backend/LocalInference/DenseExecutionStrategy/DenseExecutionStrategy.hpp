#pragma once

#include "LocalInference/ExecutionPlanner/ExecutionPlanner.hpp"

namespace singularity::local_inference {

// Architecture-specific dense-transformer planning. The generic scheduler
// dispatches here only after the inspector has truthfully classified the
// artifact as dense.
AdaptiveExecutionPlan plan_dense_execution(
	const ModelExecutionProfile &model,
	const hardware::HardwareCapabilities &hardware,
	const hardware::HardwareCalibration &calibration,
	std::uint64_t requested_context_tokens,
	std::optional<hardware::ExecutionBackend> forced_backend = std::nullopt);

} // namespace singularity::local_inference
