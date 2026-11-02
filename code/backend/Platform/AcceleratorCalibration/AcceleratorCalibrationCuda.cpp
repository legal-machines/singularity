#include "Platform/AcceleratorCalibration/AcceleratorCalibration.hpp"

#include "Platform/CudaDriver/CudaDriver.hpp"

#include <algorithm>

namespace singularity::platform {
namespace {

// The device the runtime closure executes on: llama.cpp's CUDA backend
// places a single-device plan on device 0 by default, and local inference
// plans for exactly one accelerator. A dedicated-memory device is preferred over an
// integrated one only when both exist - integrated devices share the CPU's
// memory and gain nothing from offload planning.
std::optional<cuda::Device> execution_device() {
	const auto driver = cuda::query_driver();
	if (!driver || driver->devices.empty()) return std::nullopt;
	const auto discrete = std::find_if(driver->devices.begin(), driver->devices.end(),
		[](const cuda::Device &device) { return !device.integrated; });
	return discrete != driver->devices.end() ? *discrete : driver->devices.front();
}

} // namespace

AcceleratorCapabilities inspect_accelerator_capabilities() {
	AcceleratorCapabilities result;
	const auto device = execution_device();
	if (!device) return result;
	result.identity = device->name;
	result.unified_memory = device->integrated;
	return result;
}

AcceleratorCalibration measure_accelerator_calibration() {
	AcceleratorCalibration result;
	const auto device = execution_device();
	if (!device) {
		result.unavailable_reason = "No NVIDIA device is available.";
		return result;
	}
	const cuda::DeviceMeasurement measured = cuda::measure_device(device->ordinal);
	result.memory_bandwidth_bytes_per_second = measured.memory_bandwidth_bytes_per_second;
	result.execution_giga_operations_per_second = measured.execution_giga_operations_per_second;
	result.command_overhead_microseconds = measured.command_overhead_microseconds;
	if (!device->integrated && measured.host_to_device_bytes_per_second) {
		result.host_to_accelerator_bytes_per_second = measured.host_to_device_bytes_per_second;
		result.host_to_accelerator_semantics = "pinned_host_to_device_copy";
	}
	result.unavailable_reason = measured.unavailable_reason;
	return result;
}

} // namespace singularity::platform
