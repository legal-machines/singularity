#pragma once

#include <filesystem>

namespace singularity::platform {

// Where WebView2 keeps the window's browser data on Windows: its cache and the
// page's own storage. That is this computer's application data
// (application_data_directory), in "WebView2", where Microsoft advises an app
// to keep it: with the rest of the app's data, never in the roaming profile,
// which may sit on a network drive. macOS's WKWebView and Linux's WebKitGTK
// keep theirs in the system's local folders. Windows (WebViewDataWindows.cpp).
std::filesystem::path web_view_data_directory();

// Readies that folder before the window's webview is created, which then uses
// it (AppWindow.cpp). Returns the folder, or empty when it cannot be resolved;
// webview's own choice then stands.
std::filesystem::path ready_web_view_data_directory();

} // namespace singularity::platform
