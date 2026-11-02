#include "LocalModel/ExecutionBackend/ExecutionBackend.hpp"

#include <stdexcept>

namespace singularity::hardware {

const char *to_string(ExecutionBackend backend) {
  switch (backend) {
    case ExecutionBackend::Cpu:
      return "cpu";
    case ExecutionBackend::AppleAccelerator:
      return "apple_accelerator";
    case ExecutionBackend::Cuda:
      return "cuda";
    case ExecutionBackend::DirectMl:
      return "directml";
  }
  throw std::invalid_argument("unknown ExecutionBackend");
}

ExecutionBackend execution_backend_from_string(const std::string &value) {
  if (value == "cpu") return ExecutionBackend::Cpu;
  if (value == "apple_accelerator") return ExecutionBackend::AppleAccelerator;
  if (value == "cuda") return ExecutionBackend::Cuda;
  if (value == "directml") return ExecutionBackend::DirectMl;
  throw std::invalid_argument("unknown execution backend: " + value);
}

} // namespace singularity::hardware
