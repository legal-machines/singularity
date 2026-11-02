#pragma once

#include "LocalModel/ExecutionBackend/ExecutionBackend.hpp"

#include <optional>
#include <string>

namespace singularity::local_inference {

// What local inference's runtime - the llama-server this build of Singularity
// carries - can execute. A build fact, resolved once by
// deploy/backend/cmake/LocalInferenceRuntime.cmake from the accelerator the
// runtime was compiled for, never inferred from the operating system's name:
// a new platform or accelerator is a build change there, not an #if scattered
// through model inspection and device detection.
struct RuntimeClosureInfo {
	// A llama-server is part of this build at all.
	bool packaged = false;
	// The accelerator backend compiled into it (Metal -> AppleAccelerator,
	// CUDA -> Cuda). Empty for a CPU-only runtime. CPU is always available
	// when `packaged` is true.
	std::optional<hardware::ExecutionBackend> accelerator;
	// The llama.cpp build the runtime is made from, e.g. "b10516".
	std::string build;
	// "llama.cpp:b10516:cuda,cpu" - the exact runtime, as recorded in
	// hardware signatures and telemetry.
	std::string identity;
	// The pinned MoE patch's dynamic expert residency (its Metal offloader)
	// is compiled in.
	bool dynamic_expert_residency = false;
	// Model layers can be split between accelerator and host memory
	// (llama.cpp's partial layer and expert offload), which a discrete
	// accelerator needs whenever a model is larger than its memory.
	bool partial_offload = false;
};

const RuntimeClosureInfo &packaged_runtime_closure();

} // namespace singularity::local_inference
