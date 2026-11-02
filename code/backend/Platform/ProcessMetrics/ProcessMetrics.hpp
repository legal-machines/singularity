#pragma once

#include <cstdint>

namespace singularity::platform {

// This process's real peak resident memory since launch, in bytes, as
// reported by the OS (getrusage on macOS/Linux, GetProcessMemoryInfo on
// Windows) - never estimated. Returns 0 if the platform query fails rather
// than throwing; a benchmark that cannot measure memory should say so, not
// abort the rest of the measurement.
std::uint64_t peak_resident_memory_bytes();

} // namespace singularity::platform
