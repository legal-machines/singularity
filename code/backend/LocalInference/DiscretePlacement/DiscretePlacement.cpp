#include "LocalInference/DiscretePlacement/DiscretePlacement.hpp"

#include <numeric>

namespace singularity::local_inference {
namespace {

bool usable(const PlacementProfile &placement) {
	return placement.exact && !placement.block_bytes.empty() &&
		placement.block_expert_bytes.size() == placement.block_bytes.size();
}

std::uint64_t total(const std::vector<std::uint64_t> &values) {
	return std::accumulate(values.begin(), values.end(), std::uint64_t{0});
}

std::uint64_t model_bytes(const PlacementProfile &placement) {
	return total(placement.block_bytes) + placement.token_embedding_bytes + placement.head_bytes;
}

} // namespace

std::optional<DenseAcceleratorSplit> plan_dense_accelerator_split(
		const PlacementProfile &placement, std::uint64_t kv_bytes,
		std::uint64_t accelerator_budget_bytes) {
	if (!usable(placement)) return std::nullopt;
	DenseAcceleratorSplit split;
	split.total_blocks = placement.block_bytes.size();
	const std::uint64_t kv_per_block = kv_bytes / split.total_blocks;
	std::uint64_t used = kDiscreteAcceleratorRuntimeReserveBytes;
	if (used <= accelerator_budget_bytes) {
		for (std::size_t index = placement.block_bytes.size(); index-- > 0;) {
			const std::uint64_t needed = placement.block_bytes[index] + kv_per_block;
			if (needed > accelerator_budget_bytes - used) break;
			used += needed;
			++split.accelerator_blocks;
		}
		if (split.accelerator_blocks == split.total_blocks &&
			placement.head_bytes <= accelerator_budget_bytes - used) {
			used += placement.head_bytes;
			split.head_on_accelerator = true;
		}
	}
	split.accelerator_bytes = split.accelerator_blocks == 0 ? 0 : used;
	const std::uint64_t offloaded_model_bytes = split.accelerator_bytes == 0 ? 0
		: split.accelerator_bytes - kDiscreteAcceleratorRuntimeReserveBytes - kv_per_block * split.accelerator_blocks;
	split.host_bytes = model_bytes(placement) - offloaded_model_bytes +
		kv_per_block * (split.total_blocks - split.accelerator_blocks);
	return split;
}

std::optional<ExpertHostSplit> plan_expert_host_split(
		const PlacementProfile &placement, std::uint64_t kv_bytes,
		std::uint64_t accelerator_budget_bytes) {
	if (!usable(placement)) return std::nullopt;
	const std::uint64_t expert_bytes = total(placement.block_expert_bytes);
	const std::uint64_t non_expert_block_bytes = total(placement.block_bytes) - expert_bytes;
	// Everything but the experts is always on the accelerator in this
	// placement; if that alone does not fit there is no expert split at all.
	const std::uint64_t fixed = kDiscreteAcceleratorRuntimeReserveBytes + non_expert_block_bytes +
		placement.head_bytes + kv_bytes;
	if (fixed > accelerator_budget_bytes) return std::nullopt;

	ExpertHostSplit split;
	split.total_blocks = placement.block_bytes.size();
	std::uint64_t accelerator_experts = expert_bytes;
	// Move experts to the host from the first block onwards until the rest
	// fits; zero host blocks means the whole model fits the accelerator.
	while (accelerator_experts > accelerator_budget_bytes - fixed && split.host_expert_blocks < split.total_blocks) {
		accelerator_experts -= placement.block_expert_bytes[static_cast<std::size_t>(split.host_expert_blocks)];
		++split.host_expert_blocks;
	}
	if (accelerator_experts > accelerator_budget_bytes - fixed) return std::nullopt;
	split.accelerator_bytes = fixed + accelerator_experts;
	split.host_bytes = placement.token_embedding_bytes + (expert_bytes - accelerator_experts);
	return split;
}

} // namespace singularity::local_inference
