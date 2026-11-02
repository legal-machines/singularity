#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace singularity::local_models {

// A model file the person already keeps on this Mac, made usable by Singularity
// without copying tens of gigabytes: Singularity links it into the one folder Singularity
// discovers models from, and local inference then runs it the same way as
// any other local model. Singularity never downloads a
// model of this size itself (owner decision 2026-09-15), and never moves,
// copies or changes the person's own file - forgetting one removes only the
// link Singularity made.
struct LinkedModelFile {
	// Stable id this model is known by everywhere else (Model Library card,
	// AI Setup assignment): "file:" plus the file's own name.
	std::string id;
	std::string display_name;
	// Where the person keeps the model.
	std::filesystem::path file;
	// The link inside Singularity's own model folder - the only thing Singularity
	// created and the only thing forgetting removes.
	std::filesystem::path link;
	std::uint64_t bytes = 0;
};

// The folder Singularity discovers local models from.
[[nodiscard]] std::filesystem::path generative_model_directory();

// Every model file a person has chosen, with its current real location. A link
// whose file is gone is still listed, with `bytes` 0, so it can be forgotten.
[[nodiscard]] std::vector<LinkedModelFile> linked_model_files();

// Makes `file` usable as a local model. Throws with a plain-language message
// when the file is not a usable model file, or when another file of the same
// name is already in use. Choosing the same file again simply returns it.
LinkedModelFile link_model_file(const std::filesystem::path &file);

// Removes only Singularity's own link for `id`; the person's file stays where it
// is. Throws when `id` is not a chosen model file.
void forget_model_file(const std::string &id);

// The id a chosen file is known by ("file:<name>").
[[nodiscard]] std::string linked_model_id(const std::filesystem::path &file);

} // namespace singularity::local_models
