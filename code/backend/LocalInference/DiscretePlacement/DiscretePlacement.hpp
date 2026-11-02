#pragma once

#include "LocalInference/ModelInspector/ModelInspector.hpp"

#include <cstdint>
#include <optional>

namespace singularity::local_inference {

// Placement of one model across a discrete accelerator's own memory and host
// memory - the topology of a PCIe GPU, where the two are separate physical
// pools and every byte lives in exactly one of them. (Unified-memory Apple
// silicon never uses this: there is nothing to split.)
//
// The arithmetic follows the runtime's actual placement rules rather than a
// proportional estimate: llama.cpp offloads the LAST n repeating blocks
// together with their KV state, keeps the token embedding in host memory,
// moves the head only when every block is offloaded, and - for an MoE
// model - can keep the routed-expert tensors of the FIRST k blocks in host
// memory while everything else runs on the accelerator. Only exact,
// inspected tensor sizes are used; an inexact PlacementProfile yields no
// placement at all.

// Memory the accelerator needs before any model byte: the driver context,
// math-library workspaces and the runtime's compute buffers. A planning
// reserve (like the host responsiveness reserve), not a measurement.
constexpr std::uint64_t kDiscreteAcceleratorRuntimeReserveBytes = 1024ULL * 1024 * 1024;

struct DenseAcceleratorSplit {
	// Blocks placed on the accelerator (the last `accelerator_blocks` ones).
	std::uint64_t accelerator_blocks = 0;
	std::uint64_t total_blocks = 0;
	// The head joined them (only possible when every block did).
	bool head_on_accelerator = false;
	std::uint64_t accelerator_bytes = 0;
	std::uint64_t host_bytes = 0;
};

// The largest dense placement within `accelerator_budget_bytes`, KV state
// included (`kv_bytes` for the whole context, spread evenly over blocks).
// std::nullopt when the placement profile is inexact.
std::optional<DenseAcceleratorSplit> plan_dense_accelerator_split(
	const PlacementProfile &placement, std::uint64_t kv_bytes,
	std::uint64_t accelerator_budget_bytes);

struct ExpertHostSplit {
	// Blocks whose routed experts stay in host memory and run on the CPU
	// (the first `host_expert_blocks` ones); every other tensor, all KV state
	// and the head are on the accelerator.
	std::uint64_t host_expert_blocks = 0;
	std::uint64_t total_blocks = 0;
	std::uint64_t accelerator_bytes = 0;
	std::uint64_t host_bytes = 0;
};

// The smallest number of host-resident expert blocks that lets everything
// else fit `accelerator_budget_bytes`. std::nullopt when the placement is
// inexact or even all-experts-on-host does not fit (the non-expert weights,
// head and KV state alone exceed the budget).
std::optional<ExpertHostSplit> plan_expert_host_split(
	const PlacementProfile &placement, std::uint64_t kv_bytes,
	std::uint64_t accelerator_budget_bytes);

} // namespace singularity::local_inference
