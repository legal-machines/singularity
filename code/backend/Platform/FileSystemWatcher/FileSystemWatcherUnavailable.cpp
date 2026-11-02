#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"

namespace singularity::platform {

bool file_system_watching_supported() noexcept { return false; }

std::unique_ptr<FileSystemWatcher> start_watching(const std::filesystem::path&, FileSystemChangeCallback) {
	return nullptr;
}

// No live watcher exists to force into a degraded state on this platform -
// file_system_watching_supported() above is already permanently false, so
// this is a no-op kept only so this file links against the same
// FileSystemWatcher.hpp contract FileSystemWatcherMacOS.mm implements for
// real (see that file's own override, exercised by MatterActions.test.cpp).
void set_watching_supported_override_for_testing(std::optional<bool>) {}

} // namespace singularity::platform
