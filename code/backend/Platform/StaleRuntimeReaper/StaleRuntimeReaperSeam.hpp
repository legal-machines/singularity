#pragma once

#include "Platform/StaleRuntimeReaper/StaleRuntimeReaper.hpp"

#include <cstddef>
#include <filesystem>

namespace singularity::platform::stale_runtime {

// reap_stale_runtimes() with the bundled runtime and the Singularity program
// named by the caller instead of found beside this executable. A test can run
// it against stand-in programs of its own, since no test ever starts the real
// runtime. Windows (StaleRuntimeReaper.cpp).
std::size_t reap_stale_runtimes_for(const std::filesystem::path &runtime,
	const std::filesystem::path &singularity);

} // namespace singularity::platform::stale_runtime
