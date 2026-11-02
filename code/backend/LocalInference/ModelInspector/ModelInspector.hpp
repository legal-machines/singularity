#pragma once

#include "LocalModel/ExecutionBackend/ExecutionBackend.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::local_inference {

enum class ModelFormat {
	Unknown,
	Gguf,
};

enum class ModelArchitecture {
	Unknown,
	Dense,
	MixtureOfExperts,
};

struct AttentionProfile {
	std::optional<std::uint64_t> context_length;
	std::optional<std::uint64_t> embedding_length;
	std::optional<std::uint64_t> head_count;
	std::optional<std::uint64_t> key_value_head_count;
	std::optional<std::uint64_t> key_length;
	std::optional<std::uint64_t> value_length;
	std::optional<std::uint64_t> sliding_window;
	std::optional<double> rope_frequency_base;
};

struct ExpertTensorSource {
	std::uint64_t layer = 0;
	std::uint64_t expert = 0;
	std::string role;
	std::uint32_t tensor_type = 0;
	std::string tensor_type_name;
	std::string layout;
	std::uint64_t file_offset = 0;
	std::uint64_t byte_size = 0;
};

struct ExpertProfile {
	std::optional<std::uint64_t> expert_count;
	std::optional<std::uint64_t> active_experts_per_token;
	std::optional<std::uint64_t> expert_state_bytes;
	std::optional<std::uint64_t> non_expert_state_bytes;
	std::optional<std::uint64_t> estimated_bytes_per_expert;
	std::vector<ExpertTensorSource> tensor_sources;
	bool exact_tensor_layout = false;
};

// Exact tensor bytes grouped by where llama.cpp places them under partial
// offload: whole repeating blocks (blk.N.*, the unit of layer offload, with
// the routed-expert tensors inside each block counted separately), the
// token embedding (always kept in host memory) and the head - every other
// non-block tensor, output projection and final norm included - which joins
// the accelerator only when every block does. A tied output projection
// re-uses the token embedding, which the runtime then duplicates onto the
// accelerator, so the head counts it again. `exact` is false when any
// tensor's size could not be derived from its type and shape, or the blocks
// are not exactly 0..block_count-1; a planner must then not offload by
// placement at all.
struct PlacementProfile {
	std::vector<std::uint64_t> block_bytes;
	std::vector<std::uint64_t> block_expert_bytes;
	std::uint64_t token_embedding_bytes = 0;
	std::uint64_t head_bytes = 0;
	bool exact = false;
};

struct ModelExecutionProfile {
	ModelFormat format = ModelFormat::Unknown;
	ModelArchitecture architecture = ModelArchitecture::Unknown;
	std::string format_version;
	std::string architecture_id;
	std::string display_name;
	std::string artifact_identity;
	std::string precision;
	std::optional<std::string> quantization;
	std::optional<std::uint64_t> layer_count;
	std::optional<std::uint64_t> parameter_count;
	std::uint64_t model_state_bytes = 0;
	std::uint64_t tensor_count = 0;
	AttentionProfile attention;
	ExpertProfile experts;
	std::optional<std::uint64_t> estimated_kv_bytes_per_token;
	PlacementProfile placement;
	std::vector<std::string> supported_operators;
	std::vector<hardware::ExecutionBackend> supported_backends;
	bool supports_hybrid_placement = false;
	bool supports_prefix_state_reuse = false;
	bool supports_dynamic_expert_residency = false;
	bool is_supported = false;
	std::string support_reason;
};

// Reads only the GGUF header, metadata and tensor descriptors. Tensor data is
// never mapped or loaded, so compatibility and memory feasibility can be
// decided before a potentially unsafe runtime load.
ModelExecutionProfile inspect_model(const std::filesystem::path &path);

const char *to_string(ModelFormat format);
const char *to_string(ModelArchitecture architecture);

} // namespace singularity::local_inference
