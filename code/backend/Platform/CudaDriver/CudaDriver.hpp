#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform::cuda {

// The installed NVIDIA driver, reached through its own stable driver API
// (nvcuda.dll / libcuda.so.1) loaded at run time - so the same code serves
// Windows and Linux, and a machine without an NVIDIA driver simply reports
// no devices. Nothing here needs the CUDA toolkit on the user's machine.

struct Device {
	int ordinal = 0;
	std::string name;
	std::uint64_t total_memory_bytes = 0;
	int compute_capability_major = 0;
	int compute_capability_minor = 0;
	// Shares system memory with the CPU (a Tegra/Jetson-class device) rather
	// than owning dedicated memory.
	bool integrated = false;
	// "0000:01:00.0" - the device's PCI address, how other APIs (NVML, DXGI
	// through the LUID below) name the same physical device.
	std::string pci_bus_id;
	// Windows only: the adapter LUID DXGI reports for the same device.
	std::optional<std::array<std::uint8_t, 8>> luid;
};

struct DriverInfo {
	// The CUDA version the driver implements, e.g. 13020 for 13.2.
	int driver_version = 0;
	std::vector<Device> devices;
};

// Reads the driver and its devices without creating a context or touching
// device memory. std::nullopt when no NVIDIA driver is installed or it
// reports no usable device.
std::optional<DriverInfo> query_driver();

// A short, model-independent measurement on one device - the same
// element-wise multiply-add over 16 Mi floats the Metal calibration runs, so
// the two report comparable quantities - plus the host-to-device copy rate
// from pinned memory that only a discrete device has. Unmeasured values stay
// empty and `unavailable_reason` says why.
struct DeviceMeasurement {
	std::optional<std::uint64_t> memory_bandwidth_bytes_per_second;
	std::optional<double> execution_giga_operations_per_second;
	std::optional<double> command_overhead_microseconds;
	std::optional<std::uint64_t> host_to_device_bytes_per_second;
	std::string unavailable_reason;
};

DeviceMeasurement measure_device(int ordinal);

// Memory currently free on the device, board-wide, as the NVIDIA management
// library reports it (no context is created). std::nullopt when that
// library is not installed or does not know the device.
std::optional<std::uint64_t> free_memory_bytes(const Device &device);

} // namespace singularity::platform::cuda
