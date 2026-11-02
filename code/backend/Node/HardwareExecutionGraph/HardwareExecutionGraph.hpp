#pragma once

#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "Node/HardwareCalibration/HardwareCalibration.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::hardware {

enum class ExecutionNodeKind {
	Cpu,
	Accelerator,
};

enum class StateResourceKind { HostMemory, UnifiedMemory, AcceleratorMemory, Storage };

enum class ContentionDomainKind {
	SharedMemoryBandwidth,
	AcceleratorWorkingSet,
	StorageIo,
	DispatchSynchronization,
	SystemResponsiveness,
	Thermal,
};

enum class MemoryTopology {
	Unknown,
	Unified,
	Discrete,
};

enum class ExecutionLinkKind {
	UnifiedMemoryAccess,
	Interconnect,
	StorageRead,
};

struct ExecutionNode {
	std::string id;
	ExecutionNodeKind kind = ExecutionNodeKind::Cpu;
	std::string identity;
	std::optional<std::uint64_t> capacity_bytes;
	std::optional<std::uint64_t> available_bytes;
	std::optional<std::uint64_t> safe_working_set_bytes;
	std::optional<std::uint64_t> current_allocated_bytes;
	std::optional<std::uint64_t> bandwidth_bytes_per_second;
	std::optional<double> execution_giga_operations_per_second;
	std::optional<double> command_overhead_microseconds;
	std::vector<std::string> capabilities;
};

struct ExecutionLink {
	std::string source_node_id;
	std::string destination_node_id;
	ExecutionLinkKind kind = ExecutionLinkKind::Interconnect;
	std::optional<std::uint64_t> bandwidth_bytes_per_second;
	std::string semantics;
};

struct StateResource {
	std::string id;
	StateResourceKind kind = StateResourceKind::HostMemory;
	std::optional<std::uint64_t> capacity_bytes;
	std::optional<std::uint64_t> available_bytes;
	std::optional<std::uint64_t> safe_working_set_bytes;
	std::optional<std::uint64_t> current_allocated_bytes;
	std::optional<std::uint64_t> bandwidth_bytes_per_second;
	std::vector<std::string> attached_compute_node_ids;
};

// Identifies shared bottlenecks without pretending concurrent performance is
// the sum of isolated measurements. The actual interaction remains evidence.
struct ContentionDomain {
	std::string id;
	ContentionDomainKind kind = ContentionDomainKind::SharedMemoryBandwidth;
	std::vector<std::string> participant_ids;
	std::optional<std::uint64_t> measured_capacity_per_second;
	std::string measurement_semantics;
};

struct HardwareExecutionGraph {
	MemoryTopology memory_topology = MemoryTopology::Unknown;
	MemoryPressureLevel memory_pressure = MemoryPressureLevel::Unknown;
	std::vector<ExecutionNode> nodes;
	std::vector<StateResource> state_resources;
	std::vector<ExecutionLink> links;
	std::vector<ContentionDomain> contention_domains;
};

HardwareExecutionGraph build_hardware_execution_graph(
	const HardwareCapabilities &capabilities,
	const HardwareCalibration &calibration);

const char *to_string(ExecutionNodeKind kind);
const char *to_string(StateResourceKind kind);
const char *to_string(ContentionDomainKind kind);
const char *to_string(MemoryTopology topology);
const char *to_string(ExecutionLinkKind kind);

} // namespace singularity::hardware
