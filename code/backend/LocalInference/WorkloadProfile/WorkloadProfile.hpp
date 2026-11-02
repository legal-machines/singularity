#pragma once

#include <cstdint>
#include <optional>

namespace singularity::local_inference {

enum class WorkloadIntent {
	Interactive,
	Sustained,
};

enum class WorkloadPriority {
	Latency,
	Balanced,
	Throughput,
};

enum class WorkloadPhaseBias {
	Balanced,
	PrefillHeavy,
	DecodeHeavy,
};

struct WorkloadOperation {
	std::uint64_t prompt_tokens = 0;
	std::optional<std::uint64_t> expected_generation_tokens;
	std::uint32_t concurrency = 1;
	bool user_waiting = true;
	bool reusable_prefix_available = false;
	bool model_already_loaded = false;
};

struct WorkloadProfile {
	WorkloadIntent intent = WorkloadIntent::Interactive;
	WorkloadPriority priority = WorkloadPriority::Balanced;
	WorkloadPhaseBias phase_bias = WorkloadPhaseBias::Balanced;
	std::uint64_t prompt_tokens = 0;
	std::optional<std::uint64_t> expected_generation_tokens;
	std::uint32_t concurrency = 1;
	bool reusable_prefix_available = false;
	bool model_already_loaded = false;
};

WorkloadProfile derive_workload_profile(const WorkloadOperation &operation);
const char *to_string(WorkloadIntent intent);
const char *to_string(WorkloadPriority priority);
const char *to_string(WorkloadPhaseBias bias);

} // namespace singularity::local_inference
