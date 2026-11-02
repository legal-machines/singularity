#include "Platform/WebViewData/WebViewData.hpp"

#include "Platform/AppPaths/AppPaths.hpp"

#include <exception>
#include <system_error>

namespace singularity::platform {

std::filesystem::path web_view_data_directory() {
  return application_data_directory() / "WebView2";
}

std::filesystem::path ready_web_view_data_directory() {
  try {
    const std::filesystem::path local = web_view_data_directory();
    std::error_code error;
    std::filesystem::create_directories(local, error);
    if (error || !std::filesystem::is_directory(local, error)) return {};
    return local;
  } catch (const std::exception&) {
    return {};
  }
}

} // namespace singularity::platform
