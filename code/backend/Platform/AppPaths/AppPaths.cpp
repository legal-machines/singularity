#include "Platform/AppPaths/AppPaths.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include "Platform/WindowsText/WindowsText.hpp"
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace singularity::platform {
namespace {

std::string percent_encode_path(const std::string &value) {
  std::ostringstream encoded;
  encoded << std::uppercase << std::hex;
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    const bool allowed =
        (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
        byte == '_' || byte == '~' || byte == '/' || byte == ':';
    if (allowed) {
      encoded << character;
    } else {
      encoded << '%' << std::setw(2) << std::setfill('0')
              << static_cast<int>(byte);
    }
  }
  return encoded.str();
}

} // namespace

std::filesystem::path path_from_utf8(std::string_view utf8) {
  // C++17's UTF-8 constructor: the text is decoded as UTF-8 on every
  // platform, independent of the process code page.
  return std::filesystem::u8path(utf8.begin(), utf8.end());
}

std::string path_to_utf8(const std::filesystem::path &path) { return path.u8string(); }

std::optional<std::string> environment_variable(const char *name) {
#if defined(_WIN32)
  // The wide environment holds every value exactly; the narrow one a
  // process sees depends on its code page.
  const std::wstring wide_name = windows::utf8_to_wide(name);
  const DWORD required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (required <= 1) return std::nullopt;
  std::wstring value(required, L'\0');
  const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), required);
  if (written == 0 || written >= required) return std::nullopt;
  value.resize(written);
  return windows::wide_to_utf8(value);
#else
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
#endif
}

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string executable_file_name(std::string_view program) {
#if defined(_WIN32)
  return std::string(program) + ".exe";
#else
  return std::string(program);
#endif
}

std::filesystem::path executable_path() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    throw std::runtime_error("Unable to resolve executable path");
  }
  return std::filesystem::weakly_canonical(buffer.data());
#elif defined(_WIN32)
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "Unable to resolve executable path");
    }
    if (length < buffer.size() - 1) {
      return std::filesystem::path(std::wstring(buffer.data(), length));
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__linux__)
  std::vector<char> buffer(1024);
  for (;;) {
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "Unable to resolve executable path");
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      return std::filesystem::path(
          std::string(buffer.data(), static_cast<std::size_t>(length)));
    }
    buffer.resize(buffer.size() * 2);
  }
#else
#error "Only macOS, Windows and Linux are supported"
#endif
}

std::filesystem::path frontend_entry_path() {
#if !defined(NDEBUG)
  // Development builds only: a release build never loads HTML from an
  // environment-chosen directory, because that page receives the full native
  // bridge (documents, credentials-backed providers, file actions).
  if (const auto *override_path = std::getenv("NATIVE_APP_FRONTEND_DIST")) {
    return std::filesystem::path(override_path) / "index.html";
  }
#endif

  const auto executable_directory = executable_path().parent_path();
#if defined(__APPLE__)
  const auto bundle_entry = executable_directory.parent_path() / "Resources" /
                            "frontend-dist" / "index.html";
  if (std::filesystem::exists(bundle_entry)) {
    return bundle_entry;
  }
#endif

  const auto adjacent_entry =
      executable_directory / "frontend-dist" / "index.html";
  if (std::filesystem::exists(adjacent_entry)) {
    return adjacent_entry;
  }

#if defined(__linux__)
  const auto installed_entry = executable_directory.parent_path() / "share" /
                               "singularity" / "frontend-dist" /
                               "index.html";
  if (std::filesystem::exists(installed_entry)) {
    return installed_entry;
  }
#endif

  return adjacent_entry;
}

std::string file_url(const std::filesystem::path &path) {
  auto generic_path = std::filesystem::absolute(path).generic_u8string();
#if defined(_WIN32)
  if (generic_path.empty() || generic_path.front() != '/') {
    generic_path.insert(generic_path.begin(), '/');
  }
#endif
  return "file://" + percent_encode_path(generic_path);
}

const char *contract_platform_name() {
#if defined(__APPLE__)
  return "darwin";
#elif defined(_WIN32)
  return "win32";
#else
  return "linux";
#endif
}

std::optional<std::filesystem::path> home_directory() {
#if defined(_WIN32)
  const auto *home_env = "USERPROFILE";
#else
  const auto *home_env = "HOME";
#endif
  const auto *home = std::getenv(home_env);
  if (home == nullptr || *home == '\0') return std::nullopt;
  return std::filesystem::path(home);
}

std::filesystem::path default_workspace_directory() {
  const std::optional<std::filesystem::path> home = home_directory();
  if (!home) {
    // No resolvable home directory (unusual, sandboxed or misconfigured
    // environment) - fall back to the current working directory rather
    // than throwing, since this is only ever used to pre-fill a picker.
    return std::filesystem::current_path() / NATIVE_APP_DATA_NAME;
  }
  return *home / "Documents" / NATIVE_APP_DATA_NAME;
}

std::optional<std::filesystem::path> bundled_sample_matter_directory() {
  const auto executable_directory = executable_path().parent_path();
  std::vector<std::filesystem::path> places;
#if defined(__APPLE__)
  places.push_back(executable_directory.parent_path() / "Resources" / "sample-matter");
#endif
  places.push_back(executable_directory / "sample-matter");
#if defined(__linux__)
  places.push_back(executable_directory.parent_path() / "share" / NATIVE_APP_SLUG / "sample-matter");
#endif
  for (const auto &place : places) {
    std::error_code error;
    if (!std::filesystem::is_directory(place, error)) continue;
    for (const auto &entry : std::filesystem::directory_iterator(place, error)) {
      if (entry.is_directory(error) && entry.path().filename().u8string().rfind('.', 0) != 0) return entry.path();
    }
  }
  return std::nullopt;
}

std::filesystem::path application_data_directory() {
#if defined(_WIN32)
  // LOCALAPPDATA first: it is what the user's session names, and what a test
  // that moves the profile sets (AppPathsForTesting.hpp). The shell's Known
  // Folder answers only when it is unset, before the profile-relative guess.
  if (const auto local_app_data = environment_variable("LOCALAPPDATA")) {
    return path_from_utf8(*local_app_data) / NATIVE_APP_DATA_NAME;
  }
  PWSTR known = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known)) &&
      known != nullptr) {
    const std::filesystem::path directory(known);
    CoTaskMemFree(known);
    return directory / NATIVE_APP_DATA_NAME;
  }
  if (known != nullptr) CoTaskMemFree(known);
  const auto *home_env = "USERPROFILE";
#elif defined(__APPLE__)
  const auto *home_env = "HOME";
#else
  if (const auto *xdg_data_home = std::getenv("XDG_DATA_HOME")) {
    if (*xdg_data_home != '\0') {
      return std::filesystem::path(xdg_data_home) / NATIVE_APP_SLUG;
    }
  }
  const auto *home_env = "HOME";
#endif
  const auto *home = std::getenv(home_env);
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("Unable to resolve application data directory");
  }
#if defined(__APPLE__)
  return std::filesystem::path(home) / "Library" / "Application Support" /
         NATIVE_APP_DATA_NAME;
#elif defined(_WIN32)
  return std::filesystem::path(home) / "AppData" / "Local" /
         NATIVE_APP_DATA_NAME;
#else
  return std::filesystem::path(home) / ".local" / "share" /
         NATIVE_APP_SLUG;
#endif
}

} // namespace singularity::platform
