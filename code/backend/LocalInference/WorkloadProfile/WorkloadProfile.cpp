#include "LocalInference/WorkloadProfile/WorkloadProfile.hpp"

namespace singularity::local_inference {

WorkloadProfile derive_workload_profile(const WorkloadOperation &operation) {
	WorkloadProfile profile;
	profile.prompt_tokens = operation.prompt_tokens;
	profile.expected_generation_tokens = operation.expected_generation_tokens;
	profile.concurrency = operation.concurrency == 0 ? 1 : operation.concurrency;
	profile.reusable_prefix_available = operation.reusable_prefix_available;
	profile.model_already_loaded = operation.model_already_loaded;
	profile.intent = operation.user_waiting && profile.concurrency == 1
		? WorkloadIntent::Interactive : WorkloadIntent::Sustained;
	profile.priority = profile.intent == WorkloadIntent::Interactive
		? WorkloadPriority::Latency : WorkloadPriority::Throughput;
	if (profile.expected_generation_tokens) {
		if (profile.prompt_tokens > *profile.expected_generation_tokens * 4)
			profile.phase_bias = WorkloadPhaseBias::PrefillHeavy;
		else if (*profile.expected_generation_tokens > profile.prompt_tokens * 4)
			profile.phase_bias = WorkloadPhaseBias::DecodeHeavy;
	}
	return profile;
}

const char *to_string(WorkloadIntent intent) {
	return intent == WorkloadIntent::Interactive ? "interactive" : "sustained";
}

const char *to_string(WorkloadPriority priority) {
	switch (priority) {
		case WorkloadPriority::Latency: return "latency";
		case WorkloadPriority::Balanced: return "balanced";
		case WorkloadPriority::Throughput: return "throughput";
	}
	return "balanced";
}

const char *to_string(WorkloadPhaseBias bias) {
	switch (bias) {
		case WorkloadPhaseBias::Balanced: return "balanced";
		case WorkloadPhaseBias::PrefillHeavy: return "prefill_heavy";
		case WorkloadPhaseBias::DecodeHeavy: return "decode_heavy";
	}
	return "balanced";
}

} // namespace singularity::local_inference
