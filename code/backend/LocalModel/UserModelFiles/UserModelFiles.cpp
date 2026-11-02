#include "LocalModel/UserModelFiles/UserModelFiles.hpp"

#include "Platform/AppPaths/AppPaths.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <system_error>

namespace singularity::local_models {
namespace {

std::string lowercased(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

bool is_model_file_name(const std::filesystem::path &file) {
	return lowercased(file.extension().string()) == ".gguf";
}

} // namespace

std::filesystem::path generative_model_directory() {
	return platform::application_data_directory() / "Models" / "generative";
}

std::string linked_model_id(const std::filesystem::path &file) {
	return "file:" + file.filename().string();
}

std::vector<LinkedModelFile> linked_model_files() {
	std::vector<LinkedModelFile> result;
	std::error_code error;
	const std::filesystem::path directory = generative_model_directory();
	if (!std::filesystem::is_directory(directory, error)) return result;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory, error)) {
		std::error_code linkError;
		if (!std::filesystem::is_symlink(entry.path(), linkError) || linkError) continue;
		const std::filesystem::path target = std::filesystem::read_symlink(entry.path(), linkError);
		if (linkError) continue;
		LinkedModelFile linked;
		linked.id = linked_model_id(entry.path());
		linked.display_name = entry.path().stem().string();
		linked.file = target;
		linked.link = entry.path();
		std::error_code sizeError;
		const auto size = std::filesystem::file_size(target, sizeError);
		linked.bytes = sizeError ? 0 : static_cast<std::uint64_t>(size);
		result.push_back(std::move(linked));
	}
	std::sort(result.begin(), result.end(),
		[](const LinkedModelFile &left, const LinkedModelFile &right) { return left.id < right.id; });
	return result;
}

LinkedModelFile link_model_file(const std::filesystem::path &file) {
	std::error_code error;
	if (!std::filesystem::is_regular_file(file, error) || error)
		throw std::runtime_error("That is not a file Singularity can read.");
	if (!is_model_file_name(file))
		throw std::runtime_error("Choose a model file in GGUF format (.gguf).");
	const auto size = std::filesystem::file_size(file, error);
	if (error || size == 0) throw std::runtime_error("That model file is empty or could not be read.");

	const std::filesystem::path directory = generative_model_directory();
	std::filesystem::create_directories(directory, error);
	const std::filesystem::path link = directory / file.filename();
	const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, error);
	const std::filesystem::path target = error ? file : canonical;

	std::error_code linkError;
	if (std::filesystem::exists(std::filesystem::symlink_status(link, linkError))) {
		if (std::filesystem::is_symlink(link, linkError)) {
			const std::filesystem::path current = std::filesystem::read_symlink(link, linkError);
			if (!linkError && current == target) {
				LinkedModelFile existing;
				existing.id = linked_model_id(link);
				existing.display_name = link.stem().string();
				existing.file = target;
				existing.link = link;
				existing.bytes = static_cast<std::uint64_t>(size);
				return existing;
			}
			throw std::runtime_error("Another model file with this name is already in use. Rename the file, or "
				"forget the one Singularity already uses.");
		}
		// A real file of that name is a model Singularity installed itself.
		throw std::runtime_error("A model with this file name is already installed in Singularity.");
	}

	std::filesystem::create_symlink(target, link, linkError);
	if (linkError) throw std::runtime_error("Singularity could not use that model file: " + linkError.message());

	LinkedModelFile linked;
	linked.id = linked_model_id(link);
	linked.display_name = link.stem().string();
	linked.file = target;
	linked.link = link;
	linked.bytes = static_cast<std::uint64_t>(size);
	return linked;
}

void forget_model_file(const std::string &id) {
	for (const LinkedModelFile &linked : linked_model_files()) {
		if (linked.id != id) continue;
		std::error_code error;
		// Only ever the link: the person's own model file is never removed.
		if (!std::filesystem::is_symlink(linked.link, error) || error)
			throw std::runtime_error("This model was not chosen from a file, so there is nothing to forget.");
		std::filesystem::remove(linked.link, error);
		if (error) throw std::runtime_error("Singularity could not stop using that model file: " + error.message());
		return;
	}
	throw std::runtime_error("This model was not chosen from a file, so there is nothing to forget.");
}

} // namespace singularity::local_models
