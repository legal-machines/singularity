#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace singularity::platform {

struct AcceleratorCalibration {
	std::optional<std::uint64_t> memory_bandwidth_bytes_per_second;
	std::optional<double> execution_giga_operations_per_second;
	std::optional<double> command_overhead_microseconds;
	// Host memory to accelerator memory, measured - only a discrete
	// accelerator has such a path. `host_to_accelerator_semantics` names
	// what exactly was measured ("pinned_host_to_device_copy").
	std::optional<std::uint64_t> host_to_accelerator_bytes_per_second;
	std::string host_to_accelerator_semantics;
	std::string unavailable_reason;
};

struct AcceleratorCapabilities {
	std::string identity;
	bool unified_memory = false;
	std::optional<std::uint64_t> current_allocated_bytes;
	std::optional<std::uint64_t> recommended_working_set_bytes;
	std::optional<std::uint64_t> maximum_transfer_bytes_per_second;
	bool supports_residency_sets = false;
	bool supports_metal_io = false;
};

// Runs a short model-independent kernel on a real accelerator when the
// platform adapter exposes one. Unknown stays unknown; CPU timing is never
// relabelled as accelerator evidence. Implemented per accelerator API -
// Metal (Apple), the CUDA driver (Windows and Linux builds whose runtime
// closure targets CUDA) - and as an explicit "unavailable" elsewhere.
AcceleratorCalibration measure_accelerator_calibration();
AcceleratorCapabilities inspect_accelerator_capabilities();

} // namespace singularity::platform
