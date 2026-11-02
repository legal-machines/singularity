#pragma once

#include "Platform/HttpClient/HttpClient.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace singularity::platform {

// How long a download waits for its next bytes before it fails as stalled
// (https_download_file: "a connection that stops delivering anything for a
// minute fails sooner").
inline constexpr int kDownloadStallSeconds = 60;

// The download path of https_download_file without its HTTPS-only check, with
// the stall limit named by the caller: what lets a test run the very code that
// downloads a model package against a server of its own on 127.0.0.1. The
// product calls https_download_file only. Windows (HttpClientWindows.cpp).
int download_to_file(const std::string &url, const std::vector<HttpHeader> &headers,
                     const std::filesystem::path &destination, int timeout_seconds,
                     const DownloadProgress &progress, int stall_seconds);

} // namespace singularity::platform
