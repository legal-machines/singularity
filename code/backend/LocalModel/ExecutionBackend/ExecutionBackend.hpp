#pragma once

#include <string>

namespace singularity::hardware {

// A concrete inference execution path a local model's runtime can actually
// run on. This is deliberately narrower than "every backend ONNX Runtime
// could theoretically support" - it only names backends this codebase knows
// how to reason about today. Adding a new one (e.g. Vulkan, ROCm) means
// adding a case here and to HardwareCapabilities/ExecutionPlanner, not
// changing every caller.
enum class ExecutionBackend {
  Cpu,
  // Apple's on-device accelerator path (CoreML, routing to ANE/GPU as CoreML
  // itself decides) - the only accelerator this build's ONNX Runtime bundle
  // actually carries today (see native_gliner2's lib_v2.rs provider list).
  AppleAccelerator,
  Cuda,
  DirectMl,
};

const char *to_string(ExecutionBackend backend);
ExecutionBackend execution_backend_from_string(const std::string &value);

} // namespace singularity::hardware
