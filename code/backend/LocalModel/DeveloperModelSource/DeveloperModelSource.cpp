#include "LocalModel/DeveloperModelSource/DeveloperModelSource.hpp"

#if defined(SINGULARITY_DEVELOPER_MODEL_SOURCE) && SINGULARITY_DEVELOPER_MODEL_SOURCE
#include "Platform/Sha256/Sha256.hpp"

#include <cstdlib>
#include <random>
#include <system_error>

#if defined(__APPLE__)
#include <sys/clonefile.h>
#endif
#endif

#include <stdexcept>

namespace singularity::local_models {

#if defined(SINGULARITY_DEVELOPER_MODEL_SOURCE) && SINGULARITY_DEVELOPER_MODEL_SOURCE

namespace {

bool is_folder(const std::filesystem::path &path) {
	std::error_code error;
	return std::filesystem::is_directory(path, error) && !error;
}

std::string random_suffix() {
	std::random_device seed;
	return std::to_string((static_cast<std::uint64_t>(seed()) << 32) | seed());
}

// A clone shares the source's blocks until either side is written, so a
// package of hundreds of megabytes is in place at once; another file system
// gets an ordinary copy. Either way the copy is checked afterwards.
void copy_one(const std::filesystem::path &from, const std::filesystem::path &to) {
#if defined(__APPLE__)
	if (::clonefile(from.c_str(), to.c_str(), 0) == 0) return;
#endif
	std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing);
}

// The pinned file's place inside `root`, refused when the manifest would
// reach outside it.
std::filesystem::path confined(const std::filesystem::path &root, const std::filesystem::path &relative) {
	const std::filesystem::path normal = relative.lexically_normal();
	if (normal.empty() || normal.is_absolute() || normal.begin()->string() == "..") {
		throw std::runtime_error("The developer copy lists a file outside its own folder.");
	}
	return root / normal;
}

} // namespace

bool developer_model_source_compiled_in() {
	return true;
}

std::optional<std::filesystem::path> developer_model_directory(const std::string &model_id,
                                                               const std::string &version) {

	const char *overridden = std::getenv("SINGULARITY_DEV_MODEL_DIR");
	if (overridden != nullptr && overridden[0] != '\0') {
		const std::filesystem::path root(overridden);
		const std::filesystem::path nested = root / model_id / version;
		if (is_folder(nested) == true) return nested;
		if (is_folder(root) == true) return root;
		return std::nullopt;
	}

#if defined(SINGULARITY_DEVELOPER_MODEL_ROOT)
	const std::filesystem::path compiled = std::filesystem::path(SINGULARITY_DEVELOPER_MODEL_ROOT) / model_id / version;
	if (is_folder(compiled) == true) return compiled;
#endif
	return std::nullopt;

}

void install_from_developer_source(const std::filesystem::path &source,
                                   const std::vector<PinnedModelFile> &files,
                                   const std::filesystem::path &destination,
                                   const DeveloperInstallProgress &progress) {

	if (files.empty() == true) throw std::runtime_error("The developer copy has no pinned files to install.");

	// Every pinned file is there, as itself: a link could be changed to point
	// at other bytes after it was checked.
	std::uint64_t totalBytes = 0;
	for (const PinnedModelFile &file : files) {
		const std::filesystem::path path = confined(source, file.relative_path);
		std::error_code error;
		const bool isLink = std::filesystem::is_symlink(path, error);
		if (error || isLink == true || std::filesystem::is_regular_file(path, error) == false || error) {
			throw std::runtime_error("The developer copy of this model is missing " + file.relative_path.string() + ".");
		}
		totalBytes += std::filesystem::file_size(path);
	}

	/* - - - - - - */

	std::filesystem::create_directories(destination.parent_path());
	const std::string suffix = random_suffix();
	const std::filesystem::path staging = destination.parent_path() / (".install-developer-" + suffix);
	const std::filesystem::path previous = destination.parent_path() / (".previous-developer-" + suffix);
	std::error_code ignored;

	try {
		std::filesystem::create_directories(staging);
		std::uint64_t copiedBytes = 0;
		if (progress) progress(DeveloperInstallStage::Copying, 0, totalBytes);
		for (const PinnedModelFile &file : files) {
			const std::filesystem::path from = confined(source, file.relative_path);
			const std::filesystem::path to = confined(staging, file.relative_path);
			std::filesystem::create_directories(to.parent_path());
			copy_one(from, to);
			copiedBytes += std::filesystem::file_size(to);
			if (progress) progress(DeveloperInstallStage::Copying, copiedBytes, totalBytes);
		}

		/* - - - - - - */

		// What is checked is the staged copy - the very bytes that are about to
		// become the installed package - never only the source they came from.
		std::uint64_t checkedBytes = 0;
		if (progress) progress(DeveloperInstallStage::Verifying, 0, totalBytes);
		for (const PinnedModelFile &file : files) {
			const std::filesystem::path staged = confined(staging, file.relative_path);
			if (platform::sha256_hex_digest_of_file(staged) != file.sha256) {
				throw std::runtime_error("The developer copy of this model does not match the pinned package ("
					+ file.relative_path.string() + "), so it was not installed.");
			}
			checkedBytes += std::filesystem::file_size(staged);
			if (progress) progress(DeveloperInstallStage::Verifying, checkedBytes, totalBytes);
		}

		/* - - - - - - */

		if (std::filesystem::exists(destination) == true) std::filesystem::rename(destination, previous);
		try {
			std::filesystem::rename(staging, destination);
		} catch (...) {
			if (std::filesystem::exists(previous) == true && std::filesystem::exists(destination) == false) {
				std::filesystem::rename(previous, destination, ignored);
			}
			throw;
		}
		std::filesystem::remove_all(previous, ignored);
	} catch (...) {
		std::filesystem::remove_all(staging, ignored);
		throw;
	}

}

#else

bool developer_model_source_compiled_in() {
	return false;
}

std::optional<std::filesystem::path> developer_model_directory(const std::string &, const std::string &) {
	return std::nullopt;
}

void install_from_developer_source(const std::filesystem::path &,
                                   const std::vector<PinnedModelFile> &,
                                   const std::filesystem::path &,
                                   const DeveloperInstallProgress &) {
	throw std::runtime_error("This build of Singularity has no developer model source.");
}

#endif

} // namespace singularity::local_models
