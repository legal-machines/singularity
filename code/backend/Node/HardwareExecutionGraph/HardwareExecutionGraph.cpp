#include "Node/HardwareExecutionGraph/HardwareExecutionGraph.hpp"

#include "LocalModel/ExecutionCompatibility/ExecutionCompatibility.hpp"

#include <algorithm>

namespace singularity::hardware {

HardwareExecutionGraph build_hardware_execution_graph(
		const HardwareCapabilities &capabilities,
		const HardwareCalibration &calibration) {
	HardwareExecutionGraph graph;
	graph.memory_pressure = capabilities.memory_pressure;
	graph.memory_topology = capabilities.unified_memory
		? MemoryTopology::Unified
		: (capabilities.accelerator_memory_bytes ? MemoryTopology::Discrete : MemoryTopology::Unknown);

	ExecutionNode cpu;
	cpu.id = "cpu";
	cpu.kind = ExecutionNodeKind::Cpu;
	cpu.identity = capabilities.cpu_identity;
	cpu.bandwidth_bytes_per_second = calibration.memory_bandwidth_bytes_per_second;
	cpu.execution_giga_operations_per_second = calibration.cpu_execution_giga_operations_per_second;
	cpu.capabilities = capabilities.cpu_capabilities;
	graph.nodes.push_back(std::move(cpu));

	StateResource system_memory;
	system_memory.id = capabilities.unified_memory ? "memory:unified" : "memory:host";
	const std::string system_memory_id = system_memory.id;
	system_memory.kind = capabilities.unified_memory
		? StateResourceKind::UnifiedMemory : StateResourceKind::HostMemory;
	system_memory.capacity_bytes = capabilities.total_system_memory_bytes;
	system_memory.available_bytes = capabilities.available_system_memory_bytes;
	system_memory.safe_working_set_bytes = runtime_memory_budget_bytes(capabilities);
	system_memory.bandwidth_bytes_per_second = calibration.memory_bandwidth_bytes_per_second;
	system_memory.attached_compute_node_ids.push_back("cpu");

	if (capabilities.supports(ExecutionBackend::AppleAccelerator) ||
		capabilities.supports(ExecutionBackend::Cuda) ||
		capabilities.supports(ExecutionBackend::DirectMl)) {
		ExecutionNode accelerator;
		accelerator.id = "accelerator:0";
		accelerator.kind = ExecutionNodeKind::Accelerator;
		accelerator.identity = capabilities.accelerator_identity;
		accelerator.capacity_bytes = capabilities.accelerator_memory_bytes;
		accelerator.available_bytes = capabilities.accelerator_memory_budget_bytes;
		accelerator.safe_working_set_bytes = capabilities.accelerator_memory_budget_bytes;
		accelerator.current_allocated_bytes = capabilities.accelerator_current_allocated_bytes;
		accelerator.bandwidth_bytes_per_second =
			calibration.accelerator_memory_bandwidth_bytes_per_second;
		accelerator.execution_giga_operations_per_second =
			calibration.accelerator_execution_giga_operations_per_second;
		accelerator.command_overhead_microseconds =
			calibration.accelerator_command_overhead_microseconds;
		if (capabilities.supports_accelerator_residency_sets)
			accelerator.capabilities.push_back("explicit_residency_sets");
		if (capabilities.supports_accelerator_storage_io)
			accelerator.capabilities.push_back("storage_io");
		graph.nodes.push_back(std::move(accelerator));
		if (capabilities.unified_memory) {
			system_memory.current_allocated_bytes = capabilities.accelerator_current_allocated_bytes;
			system_memory.attached_compute_node_ids.push_back("accelerator:0");
			graph.contention_domains.push_back(ContentionDomain{
				"contention:shared-memory", ContentionDomainKind::SharedMemoryBandwidth,
				{"cpu", "accelerator:0", system_memory.id},
				calibration.memory_bandwidth_bytes_per_second,
				"isolated measured capacity; simultaneous CPU/Metal effect requires real-model evidence"});
		} else {
			StateResource accelerator_memory;
			accelerator_memory.id = "memory:accelerator:0";
			accelerator_memory.kind = StateResourceKind::AcceleratorMemory;
			accelerator_memory.capacity_bytes = capabilities.accelerator_memory_bytes;
			accelerator_memory.safe_working_set_bytes = capabilities.accelerator_memory_budget_bytes;
			accelerator_memory.current_allocated_bytes = capabilities.accelerator_current_allocated_bytes;
			accelerator_memory.attached_compute_node_ids.push_back("accelerator:0");
			graph.state_resources.push_back(std::move(accelerator_memory));
		}

		ExecutionLink compute_link;
		compute_link.source_node_id = "cpu";
		compute_link.destination_node_id = "accelerator:0";
		if (capabilities.unified_memory) {
			compute_link.kind = ExecutionLinkKind::UnifiedMemoryAccess;
			compute_link.semantics = "shared_physical_memory_no_discrete_transfer";
			compute_link.bandwidth_bytes_per_second = calibration.memory_bandwidth_bytes_per_second;
		} else {
			compute_link.kind = ExecutionLinkKind::Interconnect;
			compute_link.semantics = calibration.host_to_accelerator_semantics;
			compute_link.bandwidth_bytes_per_second =
				calibration.host_to_accelerator_bandwidth_bytes_per_second;
		}
		graph.links.push_back(std::move(compute_link));
		graph.contention_domains.push_back(ContentionDomain{
			"contention:accelerator-working-set", ContentionDomainKind::AcceleratorWorkingSet,
			{"accelerator:0", capabilities.unified_memory ? system_memory.id : "memory:accelerator:0"},
			std::nullopt, "working-set pressure is observed at runtime"});
		graph.contention_domains.push_back(ContentionDomain{
			"contention:dispatch-sync", ContentionDomainKind::DispatchSynchronization,
			{"cpu", "accelerator:0"}, std::nullopt,
			"dispatch, synchronization and merge are measured by the selected runtime strategy"});
	}

	StateResource storage;
	storage.id = "storage:model";
	storage.kind = StateResourceKind::Storage;
	storage.bandwidth_bytes_per_second = calibration.storage_read_bytes_per_second;
	graph.state_resources.push_back(std::move(storage));
	graph.links.push_back(ExecutionLink{
		"storage:model", "cpu", ExecutionLinkKind::StorageRead,
		calibration.storage_read_bytes_per_second, "measured_uncached_local_read"});
	graph.contention_domains.push_back(ContentionDomain{
		"contention:storage-io", ContentionDomainKind::StorageIo,
		{"storage:model", system_memory_id}, calibration.storage_read_bytes_per_second,
		"measured_uncached_local_read"});
	graph.state_resources.push_back(std::move(system_memory));
	graph.contention_domains.push_back(ContentionDomain{
		"contention:responsiveness", ContentionDomainKind::SystemResponsiveness,
		{"cpu", "accelerator:0"}, std::nullopt,
		"bounded event-loop delay is measured during real-model execution"});
	return graph;
}

const char *to_string(ExecutionNodeKind kind) {
	switch (kind) {
		case ExecutionNodeKind::Cpu: return "cpu";
		case ExecutionNodeKind::Accelerator: return "accelerator";
	}
	return "unknown";
}

const char *to_string(StateResourceKind kind) {
	switch (kind) {
		case StateResourceKind::HostMemory: return "host_memory";
		case StateResourceKind::UnifiedMemory: return "unified_memory";
		case StateResourceKind::AcceleratorMemory: return "accelerator_memory";
		case StateResourceKind::Storage: return "storage";
	}
	return "unknown";
}

const char *to_string(ContentionDomainKind kind) {
	switch (kind) {
		case ContentionDomainKind::SharedMemoryBandwidth: return "shared_memory_bandwidth";
		case ContentionDomainKind::AcceleratorWorkingSet: return "accelerator_working_set";
		case ContentionDomainKind::StorageIo: return "storage_io";
		case ContentionDomainKind::DispatchSynchronization: return "dispatch_synchronization";
		case ContentionDomainKind::SystemResponsiveness: return "system_responsiveness";
		case ContentionDomainKind::Thermal: return "thermal";
	}
	return "unknown";
}

const char *to_string(MemoryTopology topology) {
	switch (topology) {
		case MemoryTopology::Unknown: return "unknown";
		case MemoryTopology::Unified: return "unified";
		case MemoryTopology::Discrete: return "discrete";
	}
	return "unknown";
}

const char *to_string(ExecutionLinkKind kind) {
	switch (kind) {
		case ExecutionLinkKind::UnifiedMemoryAccess: return "unified_memory_access";
		case ExecutionLinkKind::Interconnect: return "interconnect";
		case ExecutionLinkKind::StorageRead: return "storage_read";
	}
	return "unknown";
}

} // namespace singularity::hardware
