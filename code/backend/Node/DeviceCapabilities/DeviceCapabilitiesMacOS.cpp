#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "LocalInference/RuntimeClosure/RuntimeClosure.hpp"
#include "Platform/AcceleratorCalibration/AcceleratorCalibration.hpp"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace singularity::hardware {
namespace {

template <typename T>
std::optional<T> sysctl_value(const char *name) {
  T value{};
  std::size_t size = sizeof(value);
  if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return std::nullopt;
  return value;
}

std::string sysctl_string(const char *name) {
	std::size_t size = 0;
	if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return "";
	std::string value(size, '\0');
	if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return "";
	while (!value.empty() && value.back() == '\0') value.pop_back();
	return value;
}

// Free + inactive pages are the standard macOS "actually reclaimable right
// now" figure (what Activity Monitor's own "Memory Used" subtracts from
// total) - purgeable/wired/active pages are not safely available to a new
// allocation without first evicting real work.
std::optional<std::uint64_t> available_memory_bytes() {
  vm_size_t page_size = 0;
  if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS) return std::nullopt;

  vm_statistics64_data_t stats{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) !=
      KERN_SUCCESS) {
    return std::nullopt;
  }

  const auto free_pages = static_cast<std::uint64_t>(stats.free_count) +
                          static_cast<std::uint64_t>(stats.inactive_count);
  return free_pages * static_cast<std::uint64_t>(page_size);
}

} // namespace

HardwareCapabilities detect_hardware_capabilities() {
  HardwareCapabilities capabilities;

  capabilities.logical_cores =
      static_cast<unsigned>(sysctl_value<int32_t>("hw.logicalcpu").value_or(0));
  capabilities.physical_cores =
      static_cast<unsigned>(sysctl_value<int32_t>("hw.physicalcpu")
                                 .value_or(static_cast<int32_t>(capabilities.logical_cores)));
	capabilities.total_system_memory_bytes = sysctl_value<uint64_t>("hw.memsize").value_or(0);
	capabilities.available_system_memory_bytes = available_memory_bytes();
	capabilities.cpu_identity = sysctl_string("machdep.cpu.brand_string");
	capabilities.hardware_model = sysctl_string("hw.model");
	capabilities.operating_system_identity = "Darwin " + sysctl_string("kern.osrelease");

  // hw.optional.arm64 is the canonical "is this actually Apple Silicon"
  // query - true even for an x86_64 process running under Rosetta, unlike
  // testing the build's own compiled architecture.
	capabilities.apple_silicon = sysctl_value<int32_t>("hw.optional.arm64").value_or(0) != 0;
	capabilities.unified_memory = capabilities.apple_silicon;
	const platform::AcceleratorCapabilities accelerator = platform::inspect_accelerator_capabilities();
	capabilities.accelerator_identity = accelerator.identity;
	capabilities.unified_memory = accelerator.identity.empty()
		? capabilities.unified_memory : accelerator.unified_memory;
	capabilities.accelerator_current_allocated_bytes = accelerator.current_allocated_bytes;
	capabilities.accelerator_recommended_working_set_bytes = accelerator.recommended_working_set_bytes;
	capabilities.accelerator_maximum_transfer_bytes_per_second =
		accelerator.maximum_transfer_bytes_per_second;
	capabilities.supports_accelerator_residency_sets = accelerator.supports_residency_sets;
	capabilities.supports_accelerator_storage_io = accelerator.supports_metal_io;
	if (sysctl_value<int32_t>("hw.optional.neon").value_or(0) != 0) capabilities.cpu_capabilities.push_back("neon");
	if (sysctl_value<int32_t>("hw.optional.arm.FEAT_DotProd").value_or(0) != 0) capabilities.cpu_capabilities.push_back("dotprod");
	capabilities.runtime_identities = {"onnxruntime:1.23.2:coreml,cpu"};
	const local_inference::RuntimeClosureInfo &closure = local_inference::packaged_runtime_closure();
	if (closure.packaged) capabilities.runtime_identities.push_back(closure.identity);
	const int pressure = sysctl_value<int32_t>("kern.memorystatus_vm_pressure_level").value_or(0);
	if (pressure == 1) capabilities.memory_pressure = MemoryPressureLevel::Normal;
	if (pressure == 2) capabilities.memory_pressure = MemoryPressureLevel::Warning;
	if (pressure >= 4) capabilities.memory_pressure = MemoryPressureLevel::Critical;

  // The official macOS ONNX Runtime bundle carries CoreML; the native
  // GLiNER2 bridge always tries CoreML then CPU on Apple targets (see
  // native_gliner2/vendor/gliner2_inference/src/lib_v2.rs). CPU is always a
  // safe fallback once the runtime itself is bundled, which it is on macOS.
  capabilities.available_backends = {ExecutionBackend::AppleAccelerator, ExecutionBackend::Cpu};

  // Apple Silicon has no separate VRAM pool - CoreML draws from the same
  // unified memory as everything else. Intel Macs' CoreML path may use a
  // discrete/integrated GPU with its own VRAM that this build does not
  // probe; leaving this unset there is honest, not a bug.
	if (capabilities.apple_silicon && capabilities.total_system_memory_bytes > 0) {
		capabilities.accelerator_memory_bytes = capabilities.total_system_memory_bytes;
		std::optional<std::uint64_t> metal_budget;
		if (capabilities.accelerator_recommended_working_set_bytes &&
			capabilities.accelerator_current_allocated_bytes) {
			metal_budget = *capabilities.accelerator_recommended_working_set_bytes >
				*capabilities.accelerator_current_allocated_bytes
				? *capabilities.accelerator_recommended_working_set_bytes -
					*capabilities.accelerator_current_allocated_bytes : 0;
		}
		if (metal_budget && capabilities.available_system_memory_bytes) {
			capabilities.accelerator_memory_budget_bytes =
				std::min(*metal_budget, *capabilities.available_system_memory_bytes);
		} else {
			capabilities.accelerator_memory_budget_bytes = metal_budget
				? metal_budget : capabilities.available_system_memory_bytes;
		}
	}

  return capabilities;
}

} // namespace singularity::hardware
