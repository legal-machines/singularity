#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::node {

struct CpuSummary {
	std::optional<std::string> identity;
	std::vector<std::string> capabilities;
  std::optional<int> physical_cores;
  std::optional<int> logical_cores;
  std::optional<std::uint64_t> total_memory_bytes;
  // Real currently-free system memory, not a fixed total - see
  // Node/DeviceCapabilities/DeviceCapabilities.hpp's available_system_memory_bytes.
  std::optional<std::uint64_t> available_memory_bytes;
};

struct AcceleratorSummary {
	std::optional<std::string> identity;
  // e.g. "apple_accelerator" - absent when this machine/build has no
  // execution backend beyond Cpu.
  std::optional<std::string> backend;
	std::optional<std::uint64_t> memory_bytes;
	std::optional<std::uint64_t> memory_budget_bytes;
	bool unified_memory = false;
};

// Real, locally-measured facts (not spec-sheet lookups) - see
// Node/HardwareCalibration/HardwareCalibration.hpp.
struct HardwareCalibrationSummary {
	std::optional<std::uint64_t> memory_bandwidth_bytes_per_second;
	std::optional<std::uint64_t> storage_read_bytes_per_second;
	std::optional<double> cpu_execution_giga_operations_per_second;
	std::optional<std::uint64_t> accelerator_memory_bandwidth_bytes_per_second;
	std::optional<double> accelerator_execution_giga_operations_per_second;
	std::optional<double> accelerator_command_overhead_microseconds;
	std::optional<std::uint64_t> host_to_accelerator_bandwidth_bytes_per_second;
	std::string host_to_accelerator_semantics;
	std::string accelerator_calibration_unavailable_reason;
  double cpu_throughput_score = 0.0;
  long long measured_at_epoch_seconds = 0;
};

struct StorageSummary {
  std::uint64_t total_bytes = 0;
  std::uint64_t local_models_bytes = 0;
  std::uint64_t matter_documents_bytes = 0;
  std::uint64_t matter_state_bytes = 0;
  std::uint64_t indexes_cache_bytes = 0;
};

// This machine's Hardware Profile abstraction (docs/LOCAL_INFERENCE.md).
// Purely about the machine: what any
// registered local model's execution plan is chosen against, never a
// specific model's own facts (see LocalModel/LocalModelRegistry/LocalModelRegistry.hpp's
// ModelExecutionProfile for that side) or its benchmark evidence (see
// LocalModelStatus.live.benchmark).
struct NodeSummary {
  std::optional<std::string> device_name;
	std::string operating_system;
	std::string operating_system_identity;
	std::string hardware_model;
	std::string memory_pressure;
  CpuSummary cpu;
  AcceleratorSummary accelerator;
	std::vector<std::string> available_backends;
	std::vector<std::string> runtime_identities;
  HardwareCalibrationSummary calibration;
  StorageSummary storage;
  std::vector<std::string> loaded_local_model_ids;
};

NodeSummary get_node_summary();

} // namespace singularity::node
