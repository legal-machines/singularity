#include "Node/HardwareCalibration/HardwareCalibration.hpp"

#include "Platform/AcceleratorCalibration/AcceleratorCalibration.hpp"
#include "Platform/StorageProbe/StorageProbe.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <mutex>
#include <sstream>
#include <system_error>
#include <vector>

namespace singularity::hardware {
namespace {

using Json = nlohmann::json;

std::optional<std::uint64_t> measure_memory_bandwidth_bytes_per_second() {
  constexpr std::size_t bufferBytes = 64ULL * 1024 * 1024;
  constexpr int iterations = 8;
  std::vector<char> source(bufferBytes, 'a');
  std::vector<char> destination(bufferBytes, 0);

  volatile char touched = 0;
  const auto started = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    std::memcpy(destination.data(), source.data(), bufferBytes);
    // Touch the result so the optimizer cannot prove the copy is dead and
    // remove it entirely.
    touched = destination[0];
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  (void)touched;

	if (elapsed <= 0.0) return std::nullopt;
	return static_cast<std::uint64_t>((static_cast<double>(bufferBytes) * iterations) / elapsed);
}

std::optional<double> measure_cpu_execution_giga_operations_per_second() {
	constexpr std::size_t matrixSize = 192;
	constexpr int iterations = 3;
	std::vector<float> first(matrixSize * matrixSize, 0.25f);
	std::vector<float> second(matrixSize * matrixSize, 0.5f);
	std::vector<float> result(matrixSize * matrixSize, 0.0f);

	const auto started = std::chrono::steady_clock::now();
	for (int iteration = 0; iteration < iterations; ++iteration) {
		for (std::size_t row = 0; row < matrixSize; ++row) {
			for (std::size_t inner = 0; inner < matrixSize; ++inner) {
				const float value = first[row * matrixSize + inner];
				for (std::size_t column = 0; column < matrixSize; ++column) {
					result[row * matrixSize + column] += value * second[inner * matrixSize + column];
				}
			}
		}
	}
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	volatile float touched = result[0];
	(void)touched;
	if (elapsed <= 0.0) return std::nullopt;
	const double operations = 2.0 * matrixSize * matrixSize * matrixSize * iterations;
	return operations / elapsed / 1'000'000'000.0;
}

std::optional<std::uint64_t> measure_storage_read_bytes_per_second(
		const std::filesystem::path &storage_probe_directory) {
	std::error_code error;
	std::filesystem::create_directories(storage_probe_directory, error);
	if (error) return std::nullopt;
	return platform::measure_uncached_read_bytes_per_second(
		storage_probe_directory / "calibration-probe.tmp", 128ULL * 1024 * 1024);
}

} // namespace

HardwareCalibration measure_hardware_calibration(const std::filesystem::path &storage_probe_directory) {
  HardwareCalibration result;
	result.measured_at_epoch_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
	result.memory_bandwidth_bytes_per_second = measure_memory_bandwidth_bytes_per_second();
	result.cpu_execution_giga_operations_per_second = measure_cpu_execution_giga_operations_per_second();
	result.cpu_throughput_score = result.cpu_execution_giga_operations_per_second.value_or(0.0);
	result.storage_read_bytes_per_second = measure_storage_read_bytes_per_second(storage_probe_directory);
	const platform::AcceleratorCalibration accelerator = platform::measure_accelerator_calibration();
	result.accelerator_memory_bandwidth_bytes_per_second = accelerator.memory_bandwidth_bytes_per_second;
	result.accelerator_execution_giga_operations_per_second = accelerator.execution_giga_operations_per_second;
	result.accelerator_command_overhead_microseconds = accelerator.command_overhead_microseconds;
	result.accelerator_calibration_unavailable_reason = accelerator.unavailable_reason;
	result.host_to_accelerator_bandwidth_bytes_per_second = accelerator.host_to_accelerator_bytes_per_second;
	result.host_to_accelerator_semantics = accelerator.host_to_accelerator_semantics.empty()
		? "unknown" : accelerator.host_to_accelerator_semantics;
	return result;
}

std::string hardware_signature(const HardwareCapabilities &capabilities) {
  std::ostringstream stream;
	stream << capabilities.logical_cores << ':' << capabilities.physical_cores << ':'
			 << capabilities.total_system_memory_bytes << ':' << capabilities.apple_silicon << ':'
			 << capabilities.cpu_identity << ':' << capabilities.hardware_model << ':'
			 << capabilities.accelerator_identity << ':' << capabilities.operating_system_identity;
	for (const ExecutionBackend backend : capabilities.available_backends) {
		stream << ':' << to_string(backend);
	}
	for (const std::string &runtime : capabilities.runtime_identities) stream << ':' << runtime;
	return stream.str();
}

std::optional<HardwareCalibration> read_cached_hardware_calibration(
    const std::filesystem::path &cache_path, const HardwareCapabilities &capabilities) {
  const auto signature = hardware_signature(capabilities);
  try {
    std::ifstream input(cache_path);
    if (!input) return std::nullopt;
    Json cached;
    input >> cached;
    if (!cached.is_object() || cached.value("hardwareSignature", "") != signature) return std::nullopt;
    HardwareCalibration result;
		if (cached.contains("memoryBandwidthBytesPerSecond") && !cached["memoryBandwidthBytesPerSecond"].is_null())
			result.memory_bandwidth_bytes_per_second = cached["memoryBandwidthBytesPerSecond"].get<std::uint64_t>();
		if (cached.contains("storageReadBytesPerSecond") && !cached["storageReadBytesPerSecond"].is_null())
			result.storage_read_bytes_per_second = cached["storageReadBytesPerSecond"].get<std::uint64_t>();
		if (cached.contains("cpuExecutionGigaOperationsPerSecond") && !cached["cpuExecutionGigaOperationsPerSecond"].is_null())
			result.cpu_execution_giga_operations_per_second = cached["cpuExecutionGigaOperationsPerSecond"].get<double>();
		if (cached.contains("acceleratorMemoryBandwidthBytesPerSecond") && !cached["acceleratorMemoryBandwidthBytesPerSecond"].is_null())
			result.accelerator_memory_bandwidth_bytes_per_second = cached["acceleratorMemoryBandwidthBytesPerSecond"].get<std::uint64_t>();
		if (cached.contains("acceleratorExecutionGigaOperationsPerSecond") && !cached["acceleratorExecutionGigaOperationsPerSecond"].is_null())
			result.accelerator_execution_giga_operations_per_second = cached["acceleratorExecutionGigaOperationsPerSecond"].get<double>();
		if (cached.contains("acceleratorCommandOverheadMicroseconds") && !cached["acceleratorCommandOverheadMicroseconds"].is_null())
			result.accelerator_command_overhead_microseconds = cached["acceleratorCommandOverheadMicroseconds"].get<double>();
		if (cached.contains("hostToAcceleratorBandwidthBytesPerSecond") && !cached["hostToAcceleratorBandwidthBytesPerSecond"].is_null())
			result.host_to_accelerator_bandwidth_bytes_per_second = cached["hostToAcceleratorBandwidthBytesPerSecond"].get<std::uint64_t>();
		result.host_to_accelerator_semantics = cached.value("hostToAcceleratorSemantics", "unknown");
		result.accelerator_calibration_unavailable_reason = cached.value("acceleratorCalibrationUnavailableReason", "");
		result.cpu_throughput_score = cached.value("cpuThroughputScore", 0.0);
    result.measured_at_epoch_seconds = cached.value("measuredAtEpochSeconds", 0LL);
    return result;
  } catch (...) {
    // A corrupt/unreadable cache is treated the same as a missing one.
    return std::nullopt;
  }
}

HardwareCalibration cached_hardware_calibration(const std::filesystem::path &cache_path,
                                                const std::filesystem::path &storage_probe_directory,
                                                const HardwareCapabilities &capabilities) {
	static std::mutex calibration_mutex;
	const std::lock_guard<std::mutex> lock(calibration_mutex);
  if (const auto cached = read_cached_hardware_calibration(cache_path, capabilities)) return *cached;
  const auto signature = hardware_signature(capabilities);

	auto fresh = measure_hardware_calibration(storage_probe_directory);
	if (capabilities.unified_memory == true) {
		fresh.host_to_accelerator_semantics = "unified_memory_no_discrete_transfer";
		fresh.host_to_accelerator_bandwidth_bytes_per_second = std::nullopt;
	}

  try {
    std::error_code error;
    std::filesystem::create_directories(cache_path.parent_path(), error);
    std::ofstream output(cache_path, std::ios::trunc);
    if (output) {
		const Json record{
			{"hardwareSignature", signature},
			{"memoryBandwidthBytesPerSecond", fresh.memory_bandwidth_bytes_per_second ? Json(*fresh.memory_bandwidth_bytes_per_second) : Json(nullptr)},
			{"storageReadBytesPerSecond", fresh.storage_read_bytes_per_second ? Json(*fresh.storage_read_bytes_per_second) : Json(nullptr)},
			{"cpuExecutionGigaOperationsPerSecond", fresh.cpu_execution_giga_operations_per_second ? Json(*fresh.cpu_execution_giga_operations_per_second) : Json(nullptr)},
			{"acceleratorMemoryBandwidthBytesPerSecond", fresh.accelerator_memory_bandwidth_bytes_per_second ? Json(*fresh.accelerator_memory_bandwidth_bytes_per_second) : Json(nullptr)},
			{"acceleratorExecutionGigaOperationsPerSecond", fresh.accelerator_execution_giga_operations_per_second ? Json(*fresh.accelerator_execution_giga_operations_per_second) : Json(nullptr)},
			{"acceleratorCommandOverheadMicroseconds", fresh.accelerator_command_overhead_microseconds ? Json(*fresh.accelerator_command_overhead_microseconds) : Json(nullptr)},
			{"hostToAcceleratorBandwidthBytesPerSecond", fresh.host_to_accelerator_bandwidth_bytes_per_second ? Json(*fresh.host_to_accelerator_bandwidth_bytes_per_second) : Json(nullptr)},
			{"hostToAcceleratorSemantics", fresh.host_to_accelerator_semantics},
			{"acceleratorCalibrationUnavailableReason", fresh.accelerator_calibration_unavailable_reason},
          {"cpuThroughputScore", fresh.cpu_throughput_score},
          {"measuredAtEpochSeconds", fresh.measured_at_epoch_seconds},
      };
      output << record.dump(2);
    }
  } catch (...) {
    // Persisting the cache is an optimization, not a correctness
    // requirement - a fresh, unpersisted measurement is still a real one.
  }

  return fresh;
}

} // namespace singularity::hardware
