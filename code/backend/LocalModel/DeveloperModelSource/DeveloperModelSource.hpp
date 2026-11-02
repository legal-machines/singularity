#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::local_models {

// A Debug build's own copy of a pinned model package, so Singularity can be put
// through product acceptance on a fresh profile before Legal Machines accounts
// exist: the protection model is installed from a folder on this computer
// instead of being downloaded with a signed-in session.
//
// Compiled in only when the build defines SINGULARITY_DEVELOPER_MODEL_SOURCE=1,
// which deploy/backend/CMakeLists.txt does for the Debug configuration alone.
// A Release build has no developer source at all: it resolves no folder, reads
// no environment variable for it and installs nothing from it.
//
// Where the copy is looked for, in this order:
//   1. $SINGULARITY_DEV_MODEL_DIR/<model-id>/<version>/, or the folder
//      $SINGULARITY_DEV_MODEL_DIR itself when it holds the package directly;
//   2. <repository>/deploy/.dev-models/<model-id>/<version>/ (git-ignored,
//      never committed and never bundled).
//
// Nothing is trusted because of where it lies: every file is checked against
// the same pinned SHA-256 the production package is checked against.

// One file a pinned package must contain, and its exact digest.
struct PinnedModelFile {
	std::filesystem::path relative_path;
	std::string sha256;
};

enum class DeveloperInstallStage {
	Copying,
	Verifying,
};

// Told how far an install has come: the bytes copied, then the bytes checked,
// each of the package's whole size. Called on the installing thread, only with
// figures the work has reached.
using DeveloperInstallProgress =
	std::function<void(DeveloperInstallStage stage, std::uint64_t done_bytes, std::uint64_t total_bytes)>;

// Whether this build carries the developer model source at all.
bool developer_model_source_compiled_in();

// The folder holding this build's developer copy of `model_id` at `version`,
// when this build has a developer source and such a folder exists.
std::optional<std::filesystem::path> developer_model_directory(const std::string &model_id,
                                                               const std::string &version);

// Installs the pinned files of `source` as `destination`: each file is cloned
// (copied where the file system cannot clone) into a staging folder beside
// `destination`, every staged file is checked against its pinned digest, and
// only then does the staging folder take `destination`'s place in one rename.
// Any missing file, any file that is a link, any digest that differs and any
// failure on the way leaves `destination` exactly as it was and throws
// std::runtime_error. Throws at once in a build without a developer source.
void install_from_developer_source(const std::filesystem::path &source,
                                   const std::vector<PinnedModelFile> &files,
                                   const std::filesystem::path &destination,
                                   const DeveloperInstallProgress &progress = {});

} // namespace singularity::local_models
