#include "LocalInference/ModelInspector/ModelInspector.hpp"

#include "LocalInference/RuntimeClosure/RuntimeClosure.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace singularity::local_inference {
namespace {

enum class GgufValueType : std::uint32_t {
	Uint8 = 0,
	Int8 = 1,
	Uint16 = 2,
	Int16 = 3,
	Uint32 = 4,
	Int32 = 5,
	Float32 = 6,
	Bool = 7,
	String = 8,
	Array = 9,
	Uint64 = 10,
	Int64 = 11,
	Float64 = 12,
};

struct MetadataValue {
	std::optional<std::uint64_t> unsigned_value;
	std::optional<double> number_value;
	std::optional<std::string> string_value;
};

struct TensorDescriptor {
	std::string name;
	std::vector<std::uint64_t> dimensions;
	std::uint32_t type = 0;
	std::uint64_t offset = 0;
};

struct GgmlTypeLayout {
	const char *name;
	std::uint64_t block_elements;
	std::uint64_t block_bytes;
};

std::optional<std::uint64_t> multiply_checked(std::uint64_t first, std::uint64_t second);

template <typename T>
T read_value(std::ifstream &input) {
	T value{};
	input.read(reinterpret_cast<char*>(&value), sizeof(value));
	if (!input) throw std::runtime_error("The model metadata is truncated.");
	return value;
}

std::string read_string(std::ifstream &input) {
	const std::uint64_t size = read_value<std::uint64_t>(input);
	if (size > 128ULL * 1024 * 1024) {
		throw std::runtime_error("The model metadata contains an invalid string size.");
	}
	std::string value(static_cast<std::size_t>(size), '\0');
	input.read(value.data(), static_cast<std::streamsize>(size));
	if (!input) throw std::runtime_error("The model metadata is truncated.");
	return value;
}

std::uint64_t scalar_size(GgufValueType type) {
	switch (type) {
		case GgufValueType::Uint8:
		case GgufValueType::Int8:
		case GgufValueType::Bool:
			return 1;
		case GgufValueType::Uint16:
		case GgufValueType::Int16:
			return 2;
		case GgufValueType::Uint32:
		case GgufValueType::Int32:
		case GgufValueType::Float32:
			return 4;
		case GgufValueType::Uint64:
		case GgufValueType::Int64:
		case GgufValueType::Float64:
			return 8;
		case GgufValueType::String:
		case GgufValueType::Array:
			break;
	}
	throw std::runtime_error("The model metadata contains an invalid scalar type.");
}

void skip_bytes(std::ifstream &input, std::uint64_t size) {
	if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
		throw std::runtime_error("The model metadata contains an invalid value size.");
	}
	input.seekg(static_cast<std::streamoff>(size), std::ios::cur);
	if (!input) throw std::runtime_error("The model metadata is truncated.");
}

void skip_value(std::ifstream &input, GgufValueType type) {
	if (type == GgufValueType::String) {
		(void)read_string(input);
		return;
	}
	if (type == GgufValueType::Array) {
		const GgufValueType element_type = static_cast<GgufValueType>(read_value<std::uint32_t>(input));
		const std::uint64_t count = read_value<std::uint64_t>(input);
		if (count > 100'000'000ULL) throw std::runtime_error("The model metadata contains an invalid array size.");
		if (element_type == GgufValueType::String || element_type == GgufValueType::Array) {
			for (std::uint64_t index = 0; index < count; ++index) skip_value(input, element_type);
			return;
		}
		const std::uint64_t size = scalar_size(element_type);
		if (count > std::numeric_limits<std::uint64_t>::max() / size) {
			throw std::runtime_error("The model metadata contains an invalid array size.");
		}
		skip_bytes(input, count * size);
		return;
	}
	skip_bytes(input, scalar_size(type));
}

MetadataValue read_metadata_value(std::ifstream &input, GgufValueType type) {
	MetadataValue result;
	if (type == GgufValueType::String) {
		result.string_value = read_string(input);
		return result;
	}
	switch (type) {
		case GgufValueType::Uint8: result.unsigned_value = read_value<std::uint8_t>(input); return result;
		case GgufValueType::Uint16: result.unsigned_value = read_value<std::uint16_t>(input); return result;
		case GgufValueType::Uint32: result.unsigned_value = read_value<std::uint32_t>(input); return result;
		case GgufValueType::Uint64: result.unsigned_value = read_value<std::uint64_t>(input); return result;
		case GgufValueType::Float32: result.number_value = read_value<float>(input); return result;
		case GgufValueType::Float64: result.number_value = read_value<double>(input); return result;
		case GgufValueType::Int8: {
			const std::int8_t value = read_value<std::int8_t>(input);
			if (value >= 0) result.unsigned_value = static_cast<std::uint64_t>(value);
			return result;
		}
		case GgufValueType::Int16: {
			const std::int16_t value = read_value<std::int16_t>(input);
			if (value >= 0) result.unsigned_value = static_cast<std::uint64_t>(value);
			return result;
		}
		case GgufValueType::Int32: {
			const std::int32_t value = read_value<std::int32_t>(input);
			if (value >= 0) result.unsigned_value = static_cast<std::uint64_t>(value);
			return result;
		}
		case GgufValueType::Int64: {
			const std::int64_t value = read_value<std::int64_t>(input);
			if (value >= 0) result.unsigned_value = static_cast<std::uint64_t>(value);
			return result;
		}
		default:
			skip_value(input, type);
			return result;
	}
}

std::optional<std::uint64_t> metadata_uint(const std::map<std::string, MetadataValue> &metadata,
											const std::string &key) {
	const auto found = metadata.find(key);
	if (found == metadata.end()) return std::nullopt;
	return found->second.unsigned_value;
}

std::string metadata_string(const std::map<std::string, MetadataValue> &metadata,
								const std::string &key) {
	const auto found = metadata.find(key);
	if (found == metadata.end() || !found->second.string_value) return "";
	return *found->second.string_value;
}

std::optional<double> metadata_number(const std::map<std::string, MetadataValue> &metadata,
										const std::string &key) {
	const auto found = metadata.find(key);
	if (found == metadata.end()) return std::nullopt;
	if (found->second.number_value) return found->second.number_value;
	return found->second.unsigned_value
		? std::optional<double>(static_cast<double>(*found->second.unsigned_value)) : std::nullopt;
}

std::string quantization_name(std::uint64_t file_type) {
	switch (file_type) {
		case 0: return "F32";
		case 1: return "F16";
		case 2: return "Q4_0";
		case 3: return "Q4_1";
		case 7: return "Q8_0";
		case 8: return "Q5_0";
		case 9: return "Q5_1";
		case 10: return "Q2_K";
		case 11: return "Q3_K_S";
		case 12: return "Q3_K_M";
		case 13: return "Q3_K_L";
		case 14: return "Q4_K_S";
		case 15: return "Q4_K_M";
		case 16: return "Q5_K_S";
		case 17: return "Q5_K_M";
		case 18: return "Q6_K";
		case 19: return "IQ2_XXS";
		case 20: return "IQ2_XS";
		case 21: return "IQ3_XXS";
		case 22: return "IQ1_S";
		case 23: return "IQ4_NL";
		case 24: return "IQ3_S";
		case 25: return "IQ3_M";
		case 26: return "IQ2_S";
		case 27: return "IQ2_M";
		case 28: return "IQ4_XS";
		case 29: return "IQ1_M";
		case 30: return "BF16";
		default: return "GGUF type " + std::to_string(file_type);
	}
}

std::optional<GgmlTypeLayout> ggml_type_layout(std::uint32_t type) {
	switch (type) {
		case 0: return GgmlTypeLayout{"f32", 1, 4};
		case 1: return GgmlTypeLayout{"f16", 1, 2};
		case 2: return GgmlTypeLayout{"q4_0", 32, 18};
		case 3: return GgmlTypeLayout{"q4_1", 32, 20};
		case 6: return GgmlTypeLayout{"q5_0", 32, 22};
		case 7: return GgmlTypeLayout{"q5_1", 32, 24};
		case 8: return GgmlTypeLayout{"q8_0", 32, 34};
		case 9: return GgmlTypeLayout{"q8_1", 32, 40};
		case 10: return GgmlTypeLayout{"q2_K", 256, 84};
		case 11: return GgmlTypeLayout{"q3_K", 256, 110};
		case 12: return GgmlTypeLayout{"q4_K", 256, 144};
		case 13: return GgmlTypeLayout{"q5_K", 256, 176};
		case 14: return GgmlTypeLayout{"q6_K", 256, 210};
		case 15: return GgmlTypeLayout{"q8_K", 256, 292};
		case 24: return GgmlTypeLayout{"i8", 1, 1};
		case 25: return GgmlTypeLayout{"i16", 1, 2};
		case 26: return GgmlTypeLayout{"i32", 1, 4};
		case 27: return GgmlTypeLayout{"i64", 1, 8};
		case 28: return GgmlTypeLayout{"f64", 1, 8};
		case 30: return GgmlTypeLayout{"bf16", 1, 2};
		default: return std::nullopt;
	}
}

std::optional<std::uint64_t> tensor_data_bytes(const TensorDescriptor &tensor) {
	const auto layout = ggml_type_layout(tensor.type);
	if (!layout || tensor.dimensions.empty() ||
		tensor.dimensions.front() % layout->block_elements != 0) return std::nullopt;
	std::uint64_t size = tensor.dimensions.front() / layout->block_elements;
	if (size > std::numeric_limits<std::uint64_t>::max() / layout->block_bytes) return std::nullopt;
	size *= layout->block_bytes;
	for (std::size_t index = 1; index < tensor.dimensions.size(); ++index) {
		const auto next = multiply_checked(size, tensor.dimensions[index]);
		if (!next) return std::nullopt;
		size = *next;
	}
	return size;
}

std::optional<std::uint64_t> tensor_layer(const std::string &name) {
	if (name.compare(0, 4, "blk.") != 0) return std::nullopt;
	const char *begin = name.data() + 4;
	const char *end = name.data() + name.size();
	std::uint64_t layer = 0;
	const auto parsed = std::from_chars(begin, end, layer);
	if (parsed.ec != std::errc{} || parsed.ptr == end || *parsed.ptr != '.') return std::nullopt;
	return layer;
}

std::string expert_tensor_role(const std::string &name) {
	if (name.find("ffn_gate_up_exps.") != std::string::npos) return "gate_up";
	if (name.find("ffn_gate_exps.") != std::string::npos) return "gate";
	if (name.find("ffn_up_exps.") != std::string::npos) return "up";
	if (name.find("ffn_down_exps.") != std::string::npos) return "down";
	return "";
}

PlacementProfile placement_profile(const std::vector<TensorDescriptor> &tensors,
		std::optional<std::uint64_t> block_count, std::optional<std::uint64_t> expert_count) {
	PlacementProfile placement;
	if (!block_count || *block_count == 0 || *block_count > 100'000) return placement;
	placement.block_bytes.assign(static_cast<std::size_t>(*block_count), 0);
	placement.block_expert_bytes.assign(static_cast<std::size_t>(*block_count), 0);
	std::vector<bool> block_seen(static_cast<std::size_t>(*block_count), false);
	std::optional<std::uint64_t> output_projection_bytes;
	for (const TensorDescriptor &tensor : tensors) {
		const auto bytes = tensor_data_bytes(tensor);
		if (!bytes) return PlacementProfile{};
		if (const auto layer = tensor_layer(tensor.name)) {
			if (*layer >= *block_count) return PlacementProfile{};
			const auto index = static_cast<std::size_t>(*layer);
			placement.block_bytes[index] += *bytes;
			block_seen[index] = true;
			if (expert_count && !expert_tensor_role(tensor.name).empty() && tensor.dimensions.size() >= 3 &&
				tensor.dimensions.back() == *expert_count)
				placement.block_expert_bytes[index] += *bytes;
		} else if (tensor.name == "token_embd.weight") {
			placement.token_embedding_bytes += *bytes;
		} else {
			if (tensor.name == "output.weight") output_projection_bytes = *bytes;
			placement.head_bytes += *bytes;
		}
	}
	if (std::find(block_seen.begin(), block_seen.end(), false) != block_seen.end()) return PlacementProfile{};
	if (!output_projection_bytes) placement.head_bytes += placement.token_embedding_bytes;
	placement.exact = true;
	return placement;
}

void hash_bytes(std::uint64_t &hash, const void *data, std::size_t size) {
	const auto *bytes = static_cast<const unsigned char*>(data);
	for (std::size_t index = 0; index < size; ++index) {
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
	}
}

void hash_string(std::uint64_t &hash, const std::string &value) {
	hash_bytes(hash, value.data(), value.size());
}

bool runtime_supports_architecture(const std::string &architecture) {
	static const std::array<const char*, 18> supported = {
		"llama", "mistral", "mixtral", "qwen2", "qwen3", "gemma", "gemma2", "gemma3",
		"deepseek2", "deepseek3", "phi2", "phi3", "phi4", "starcoder2", "command-r", "gpt2",
		"granite", "qwen3moe",
	};
	return std::find(supported.begin(), supported.end(), architecture) != supported.end();
}

bool runtime_architecture_requires_experts(const std::string &architecture) {
	static const std::array<const char*, 4> mixture_of_experts = {
		"mixtral", "deepseek2", "deepseek3", "qwen3moe",
	};
	return std::find(mixture_of_experts.begin(), mixture_of_experts.end(), architecture) !=
		mixture_of_experts.end();
}

std::optional<std::uint64_t> multiply_checked(std::uint64_t first, std::uint64_t second) {
	if (first == 0 || second == 0) return std::uint64_t{0};
	if (first > std::numeric_limits<std::uint64_t>::max() / second) return std::nullopt;
	return first * second;
}

} // namespace

const char *to_string(ModelFormat format) {
	switch (format) {
		case ModelFormat::Unknown: return "unknown";
		case ModelFormat::Gguf: return "gguf";
	}
	return "unknown";
}

const char *to_string(ModelArchitecture architecture) {
	switch (architecture) {
		case ModelArchitecture::Unknown: return "unknown";
		case ModelArchitecture::Dense: return "dense";
		case ModelArchitecture::MixtureOfExperts: return "mixture_of_experts";
	}
	return "unknown";
}

ModelExecutionProfile inspect_model(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) throw std::runtime_error("The model file could not be opened.");

	char magic[4]{};
	input.read(magic, sizeof(magic));
	if (!input || std::string(magic, sizeof(magic)) != "GGUF") {
		ModelExecutionProfile result;
		result.support_reason = "The model format is unknown or unsupported.";
		return result;
	}

	ModelExecutionProfile result;
	result.format = ModelFormat::Gguf;
	const std::uint32_t version = read_value<std::uint32_t>(input);
	result.format_version = std::to_string(version);
	if (version < 2 || version > 3) {
		result.support_reason = "This GGUF version is not supported by the installed runtime adapter.";
		return result;
	}

	result.tensor_count = read_value<std::uint64_t>(input);
	const std::uint64_t metadata_count = read_value<std::uint64_t>(input);
	if (metadata_count > 1'000'000ULL || result.tensor_count > 10'000'000ULL) {
		throw std::runtime_error("The model metadata contains invalid collection sizes.");
	}

	std::map<std::string, MetadataValue> metadata;
	for (std::uint64_t index = 0; index < metadata_count; ++index) {
		const std::string key = read_string(input);
		const GgufValueType type = static_cast<GgufValueType>(read_value<std::uint32_t>(input));
		metadata.emplace(key, read_metadata_value(input, type));
	}

	result.architecture_id = metadata_string(metadata, "general.architecture");
	result.display_name = metadata_string(metadata, "general.name");
	const std::optional<std::uint64_t> file_type = metadata_uint(metadata, "general.file_type");
	if (file_type) {
		result.precision = quantization_name(*file_type);
		if (*file_type > 1 && *file_type != 30) result.quantization = result.precision;
	}

	const std::string prefix = result.architecture_id.empty() ? "" : result.architecture_id + ".";
	result.layer_count = metadata_uint(metadata, prefix + "block_count");
	result.attention.context_length = metadata_uint(metadata, prefix + "context_length");
	result.attention.embedding_length = metadata_uint(metadata, prefix + "embedding_length");
	result.attention.head_count = metadata_uint(metadata, prefix + "attention.head_count");
	result.attention.key_value_head_count = metadata_uint(metadata, prefix + "attention.head_count_kv");
	result.attention.key_length = metadata_uint(metadata, prefix + "attention.key_length");
	result.attention.value_length = metadata_uint(metadata, prefix + "attention.value_length");
	result.attention.sliding_window = metadata_uint(metadata, prefix + "attention.sliding_window");
	result.attention.rope_frequency_base = metadata_number(metadata, prefix + "rope.freq_base");
	result.experts.expert_count = metadata_uint(metadata, prefix + "expert_count");
	result.experts.active_experts_per_token = metadata_uint(metadata, prefix + "expert_used_count");

	std::uint64_t parameter_count = 0;
	std::vector<TensorDescriptor> tensors;
	tensors.reserve(static_cast<std::size_t>(result.tensor_count));
	for (std::uint64_t index = 0; index < result.tensor_count; ++index) {
		TensorDescriptor tensor;
		tensor.name = read_string(input);
		const std::uint32_t dimension_count = read_value<std::uint32_t>(input);
		if (dimension_count == 0 || dimension_count > 8) {
			throw std::runtime_error("The model contains an invalid tensor descriptor.");
		}
		std::uint64_t tensor_parameters = 1;
		for (std::uint32_t dimension = 0; dimension < dimension_count; ++dimension) {
			const std::uint64_t size = read_value<std::uint64_t>(input);
			tensor.dimensions.push_back(size);
			const std::optional<std::uint64_t> next = multiply_checked(tensor_parameters, size);
			if (!next) throw std::runtime_error("The model contains an invalid tensor size.");
			tensor_parameters = *next;
		}
		tensor.type = read_value<std::uint32_t>(input);
		tensor.offset = read_value<std::uint64_t>(input);
		if (parameter_count > std::numeric_limits<std::uint64_t>::max() - tensor_parameters) {
			throw std::runtime_error("The model parameter count is invalid.");
		}
		parameter_count += tensor_parameters;
		tensors.push_back(std::move(tensor));
	}
	result.parameter_count = parameter_count;

	std::error_code error;
	result.model_state_bytes = std::filesystem::file_size(path, error);
	if (error || result.model_state_bytes == 0) {
		throw std::runtime_error("The model file size could not be measured.");
	}
	std::uint64_t structural_hash = 1469598103934665603ULL;
	hash_string(structural_hash, result.architecture_id);
	hash_bytes(structural_hash, &result.model_state_bytes, sizeof(result.model_state_bytes));
	// Include cheap artifact facts as well as structure. A replacement at the
	// same path with the same tensor schema must not inherit benchmark evidence
	// merely because hashing multi-gigabyte weights at every discovery would be
	// unacceptable startup work.
	const auto modified = std::filesystem::last_write_time(path, error);
	if (!error) {
		const auto modified_ticks = modified.time_since_epoch().count();
		hash_bytes(structural_hash, &modified_ticks, sizeof(modified_ticks));
	}
	for (const TensorDescriptor &tensor : tensors) {
		hash_string(structural_hash, tensor.name);
		hash_bytes(structural_hash, &tensor.type, sizeof(tensor.type));
		hash_bytes(structural_hash, &tensor.offset, sizeof(tensor.offset));
		for (const std::uint64_t dimension : tensor.dimensions) {
			hash_bytes(structural_hash, &dimension, sizeof(dimension));
		}
	}
	std::ostringstream identity;
	identity << "gguf-artifact-fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
		<< structural_hash;
	result.artifact_identity = identity.str();

	if (result.experts.expert_count && *result.experts.expert_count > 0) {
		result.architecture = ModelArchitecture::MixtureOfExperts;
	} else if (runtime_supports_architecture(result.architecture_id) &&
		!runtime_architecture_requires_experts(result.architecture_id)) {
		result.architecture = ModelArchitecture::Dense;
	}

	result.placement = placement_profile(tensors, result.layer_count, result.experts.expert_count);

	if (result.architecture == ModelArchitecture::MixtureOfExperts) {
		const std::uint64_t alignment = metadata_uint(metadata, "general.alignment").value_or(32);
		const auto descriptor_end = static_cast<std::uint64_t>(input.tellg());
		if (alignment == 0 || alignment > 1024 * 1024) {
			throw std::runtime_error("The model metadata contains an invalid tensor alignment.");
		}
		const std::uint64_t data_start = (descriptor_end + alignment - 1) / alignment * alignment;
		std::sort(tensors.begin(), tensors.end(), [](const TensorDescriptor &first,
			const TensorDescriptor &second) { return first.offset < second.offset; });
		std::uint64_t expert_bytes = 0;
		std::size_t expert_tensor_count = 0;
		std::size_t exactly_indexed_expert_tensor_count = 0;
		for (std::size_t index = 0; index < tensors.size(); ++index) {
			const std::uint64_t begin = data_start + tensors[index].offset;
			const std::uint64_t end = index + 1 < tensors.size()
				? data_start + tensors[index + 1].offset : result.model_state_bytes;
			if (begin > result.model_state_bytes || end < begin || end > result.model_state_bytes) {
				throw std::runtime_error("The model contains invalid tensor data offsets.");
			}
			const std::string role = expert_tensor_role(tensors[index].name);
			const bool named_expert_tensor = !role.empty();
			if (named_expert_tensor) ++expert_tensor_count;
			const bool expert_tensor = named_expert_tensor && tensors[index].dimensions.size() >= 3 &&
				tensors[index].dimensions.back() == *result.experts.expert_count;
			const auto exact_tensor_bytes = tensor_data_bytes(tensors[index]);
			if (expert_tensor && exact_tensor_bytes && *exact_tensor_bytes <= end - begin &&
				*exact_tensor_bytes % *result.experts.expert_count == 0) {
				const std::uint64_t bytes_per_expert = *exact_tensor_bytes / *result.experts.expert_count;
				const auto layer = tensor_layer(tensors[index].name);
				const auto layout = ggml_type_layout(tensors[index].type);
				if (layer && layout) {
					for (std::uint64_t expert = 0; expert < *result.experts.expert_count; ++expert) {
						result.experts.tensor_sources.push_back(ExpertTensorSource{
							*layer, expert, role, tensors[index].type, layout->name,
							"ggml_rows_expert_outermost", begin + expert * bytes_per_expert,
							bytes_per_expert});
					}
					++exactly_indexed_expert_tensor_count;
				}
				expert_bytes += *exact_tensor_bytes;
			}
		}
		if (expert_bytes > 0) {
			result.experts.expert_state_bytes = expert_bytes;
			result.experts.non_expert_state_bytes = result.model_state_bytes - expert_bytes;
			result.experts.estimated_bytes_per_expert = expert_bytes / *result.experts.expert_count;
			result.experts.exact_tensor_layout = expert_tensor_count > 0 &&
				exactly_indexed_expert_tensor_count == expert_tensor_count;
		}
	}

	if (result.layer_count && result.attention.key_value_head_count && result.attention.embedding_length
		&& result.attention.head_count && *result.attention.head_count > 0) {
		const std::uint64_t head_length = result.attention.key_length.value_or(
			*result.attention.embedding_length / *result.attention.head_count);
		const std::uint64_t value_length = result.attention.value_length.value_or(head_length);
		const std::optional<std::uint64_t> layer_heads = multiply_checked(*result.layer_count,
			*result.attention.key_value_head_count);
		if (layer_heads) {
			const std::optional<std::uint64_t> elements = multiply_checked(*layer_heads,
				head_length + value_length);
			if (elements) result.estimated_kv_bytes_per_token = multiply_checked(*elements, 2);
		}
	}

	if (!runtime_supports_architecture(result.architecture_id)) {
		result.support_reason = "The installed GGUF runtime adapter does not declare support for architecture '" +
			result.architecture_id + "'.";
		return result;
	}
	if (result.architecture == ModelArchitecture::Unknown || result.architecture_id.empty()) {
		result.support_reason = runtime_architecture_requires_experts(result.architecture_id)
			? "The model is missing expert metadata required by its declared architecture."
			: "The model architecture is unknown; Singularity will not guess from its name.";
		return result;
	}
	if (!result.layer_count || !result.attention.context_length || !result.attention.embedding_length) {
		result.support_reason = "The model is missing execution metadata required for a safe plan.";
		return result;
	}
	if (result.architecture == ModelArchitecture::MixtureOfExperts
		&& (!result.experts.active_experts_per_token || *result.experts.active_experts_per_token == 0)) {
		result.support_reason = "The MoE model is missing routing metadata required for a safe plan.";
		return result;
	}
	if (result.architecture == ModelArchitecture::MixtureOfExperts &&
		(result.experts.expert_state_bytes.value_or(0) == 0 ||
		 result.experts.non_expert_state_bytes.value_or(0) == 0 ||
		 !result.experts.exact_tensor_layout)) {
		result.support_reason = "The MoE tensor layout does not expose separable expert and non-expert state sizes.";
		return result;
	}

	result.supported_operators = {"attention", "feed_forward", "normalization", "token_embedding"};
	if (result.architecture == ModelArchitecture::MixtureOfExperts) {
		result.supported_operators.push_back("expert_routing");
	}
	const RuntimeClosureInfo &closure = packaged_runtime_closure();
	if (!closure.packaged) {
		result.support_reason = "This build does not package a GGUF execution backend.";
		return result;
	}
	if (closure.accelerator) result.supported_backends.push_back(*closure.accelerator);
	result.supported_backends.push_back(hardware::ExecutionBackend::Cpu);
	result.supports_prefix_state_reuse = true;
	result.supports_hybrid_placement = closure.partial_offload && result.placement.exact;
	if (result.architecture == ModelArchitecture::MixtureOfExperts) {
		result.supports_dynamic_expert_residency =
			closure.dynamic_expert_residency && result.experts.exact_tensor_layout;
	}
	result.is_supported = true;
	result.support_reason = "Supported by the installed GGUF runtime adapter.";
	return result;
}

} // namespace singularity::local_inference
