#include "LocalInference/RuntimeClosure/RuntimeClosure.hpp"

// Defined for this file by deploy/backend/CMakeLists.txt, from
// cmake/LocalInferenceRuntime.cmake's accelerator and llama.cpp build.
#ifndef LOCAL_INFERENCE_ACCELERATOR
#define LOCAL_INFERENCE_ACCELERATOR "none"
#endif
#ifndef LOCAL_INFERENCE_LLAMA_CPP_BUILD
#define LOCAL_INFERENCE_LLAMA_CPP_BUILD "unknown"
#endif

namespace singularity::local_inference {

const RuntimeClosureInfo &packaged_runtime_closure() {
	static const RuntimeClosureInfo info = [] {
		RuntimeClosureInfo result;
		const std::string accelerator = LOCAL_INFERENCE_ACCELERATOR;
		if (accelerator == "none") return result;
		result.packaged = true;
		result.build = LOCAL_INFERENCE_LLAMA_CPP_BUILD;
		if (accelerator == "metal") {
			result.accelerator = hardware::ExecutionBackend::AppleAccelerator;
			result.dynamic_expert_residency = true;
		} else if (accelerator == "cuda") {
			result.accelerator = hardware::ExecutionBackend::Cuda;
		}
		result.partial_offload = result.accelerator.has_value();
		result.identity = "llama.cpp:" + result.build + ":" +
			(result.accelerator ? accelerator + ",cpu" : std::string("cpu"));
		return result;
	}();
	return info;
}

} // namespace singularity::local_inference
