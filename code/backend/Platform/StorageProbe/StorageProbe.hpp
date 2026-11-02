#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace singularity::platform {

// Writes `probe_file` (`bytes` of incompressible data, flushed to the
// device), reads it back past the OS file cache, deletes it, and returns the
// measured read rate - the storage path a model load actually takes, not a
// re-read of pages the write just cached. std::nullopt when the platform
// could not bypass its cache or any step failed: an unmeasured rate is never
// replaced by a cached-read one.
std::optional<std::uint64_t> measure_uncached_read_bytes_per_second(
	const std::filesystem::path &probe_file, std::uint64_t bytes);

} // namespace singularity::platform
