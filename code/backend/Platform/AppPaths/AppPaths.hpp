#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace singularity::platform {

// Text crossing into or out of a path is UTF-8 on every platform.
// std::filesystem::path(const char *) would instead decode with the process
// code page on Windows, so every conversion between UTF-8 text and a path that
// must hold any name goes through these two. path_from_utf8 throws for input
// that is not valid UTF-8 where the system must decode it (Windows).
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path &path);

// An environment variable's value as UTF-8; std::nullopt when it is unset or
// empty.
std::optional<std::string> environment_variable(const char *name);

std::uint64_t current_process_id() noexcept;

// The platform's file name for a program ("llama-server" -> "llama-server.exe"
// on Windows).
std::string executable_file_name(std::string_view program);

std::filesystem::path executable_path();
std::filesystem::path frontend_entry_path();
std::string file_url(const std::filesystem::path &path);
const char *contract_platform_name();

// The platform's own per-user home directory (HOME, or USERPROFILE on
// Windows) - nullopt in an unusual, sandboxed or misconfigured environment
// where it cannot be resolved. Never created or written to here.
std::optional<std::filesystem::path> home_directory();

// Suggested default location for the local Singularity workspace (matters and
// documents), offered during onboarding's "Local workspace" step. Purely
// computed from the platform's home directory - never created here. The
// user may accept it as-is or choose a different folder instead.
std::filesystem::path default_workspace_directory();

// The sample matter this build carries - a folder of fictional US deal
// documents - read only: inside the app bundle's Resources on macOS, beside
// the executable on Windows, under share/ on Linux. The matter's own folder
// (the one directory inside "sample-matter"), or nullopt when this build
// carries none.
std::optional<std::filesystem::path> bundled_sample_matter_directory();

// OS-owned per-user application data. Local model packages are application
// assets, not user documents or Matter data, so they live below this path.
std::filesystem::path application_data_directory();

} // namespace singularity::platform
