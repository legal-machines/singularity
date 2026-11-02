#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"

#include "Platform/SecureRandom/SecureRandom.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace singularity::workspaces {

std::string current_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t as_time_t = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &as_time_t);
#else
  gmtime_r(&as_time_t, &utc_tm);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

// 16 digits that are 64 bits from the system's secure source
// (Platform/SecureRandom).
std::string generate_workspace_id() {
  return "ws_" + platform::secure_random_hex(8);
}

std::string derive_display_name(const std::filesystem::path &path) {
  const auto name = path.filename().u8string();
  if (!name.empty()) {
    return std::string(name.begin(), name.end());
  }
  // A root path (e.g. "/" or "C:\\") has no filename component.
  return path.u8string().empty() ? std::string("Workspace")
                                 : std::string(path.u8string().begin(), path.u8string().end());
}

void validate_directory(const std::filesystem::path &path) {
  std::error_code is_directory_error;
  if (!std::filesystem::is_directory(path, is_directory_error)) {
    throw std::runtime_error(
        "Could not access this folder. Choose another folder or check its permissions.");
  }

  const auto probe_name =
      ".singularity-workspace-write-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto probe_path = path / probe_name;

  {
    std::ofstream probe_file(probe_path, std::ios::trunc);
    if (!probe_file) {
      throw std::runtime_error(
          "Could not access this folder. Choose another folder or check its permissions.");
    }
  }

  std::error_code remove_error;
  std::filesystem::remove(probe_path, remove_error);
}

bool directory_available(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error);
}

} // namespace singularity::workspaces
