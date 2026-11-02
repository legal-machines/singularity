#include "Platform/WindowNavigation/WindowNavigation.hpp"
#include "Platform/WindowNavigation/WindowNavigationWindows.hpp"

#include <windows.h>

// WebView2.h needs objbase.h's `interface`, which WIN32_LEAN_AND_MEAN leaves
// out; the webview library includes WebView2.h after it.
#include <objbase.h>

#include <webview/webview.h>

#include "Platform/WebViewCom/WebViewCom.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

// The window's navigation policy and native bridge gate on Windows (WebView2),
// the counterpart of WindowNavigationMacOS.cpp: the same decisions
// (WindowNavigation.cpp), taken where WebView2 offers them.
//
// - Navigation: NavigationStarting (the window's own document, server
//   redirects included) and FrameNavigationStarting (a frame inside it) are
//   cancelled unless window_may_show() allows the address; NewWindowRequested
//   never opens a second window (it would get the bridge too).
// - Messages: WebView2 raises ICoreWebView2::WebMessageReceived only for the
//   window's top-level document (a frame's own postMessage goes to
//   ICoreWebView2Frame, which nothing here subscribes to). The webview
//   library's handler for it (webview 0.12, detail::webview2_com_handler)
//   passes every message to the bound native methods unread; the gate puts
//   message_may_reach_native() in front of it, so only the window's own page
//   reaches them, whatever let the window show another.

namespace singularity::platform {
namespace {

std::mutex &window_prefix_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<std::string> &window_prefixes() {
  static std::vector<std::string> prefixes;
  return prefixes;
}

std::string utf8_of(const wchar_t *text) {
  if (text == nullptr || *text == L'\0') return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};
  std::string result(static_cast<std::size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
  return result;
}

int hex_value(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  return -1;
}

// A file URL as Windows names the file: percent escapes decoded, and case
// folded, since NTFS paths ignore case. Singularity writes its own file URLs
// (AppPaths file_url) with escapes WebView2 may spell differently, and
// WebView2 reports a drive letter in either case; compared this way, the
// window's own pages match their folder and nothing else does. A decoded ".."
// is still caught by window_may_show(), which refuses any URL that climbs.
// Other URLs are compared as WebView2 reports them.
std::string comparable(const std::string &url) {
  if (url.size() < 7 || _strnicmp(url.c_str(), "file://", 7) != 0) return url;
  std::string decoded;
  decoded.reserve(url.size());
  for (std::size_t at = 0; at < url.size(); ++at) {
    if (url[at] == '%' && at + 2 < url.size() && hex_value(url[at + 1]) >= 0 && hex_value(url[at + 2]) >= 0) {
      decoded.push_back(static_cast<char>(hex_value(url[at + 1]) * 16 + hex_value(url[at + 2])));
      at += 2;
    } else {
      decoded.push_back(url[at]);
    }
  }
  for (char &character : decoded)
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  return decoded;
}

std::vector<std::string> remembered_prefixes() {
  const std::lock_guard<std::mutex> lock(window_prefix_mutex());
  std::vector<std::string> prefixes;
  for (const std::string &prefix : window_prefixes()) prefixes.push_back(comparable(prefix));
  return prefixes;
}

// Said once a process per kind, and never with what was refused in full: a
// page retrying in a loop must not fill the log.
void report_once(std::atomic<bool> &reported, const std::string &what) {
  if (reported.exchange(true)) return;
  std::cerr << "Singularity " << what << ". Later ones are not reported.\n";
}

std::atomic<bool> refused_navigation{false};
std::atomic<bool> refused_frame{false};
std::atomic<bool> refused_window{false};
std::atomic<bool> refused_message{false};

// The scheme, host and port of a URL, as the gate compares origins: 0 for a
// file URL and for a scheme's default port.
void origin_parts(const std::string &url, std::string &protocol, std::string &host, int &port) {
  protocol.clear();
  host.clear();
  port = 0;
  const std::size_t colon = url.find(':');
  if (colon == std::string::npos) return;
  protocol = url.substr(0, colon);
  for (char &character : protocol)
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  if (protocol == "file" || url.compare(colon, 3, "://") != 0) return;
  std::string authority = url.substr(colon + 3, url.find_first_of("/?#", colon + 3) - (colon + 3));
  const std::size_t user_end = authority.rfind('@');
  if (user_end != std::string::npos) authority = authority.substr(user_end + 1);
  const std::size_t port_colon = authority.rfind(':');
  const std::size_t bracket = authority.rfind(']');
  const bool has_port = port_colon != std::string::npos && (bracket == std::string::npos || port_colon > bracket);
  host = has_port ? authority.substr(0, port_colon) : authority;
  if (has_port) {
    const std::string digits = authority.substr(port_colon + 1);
    const bool numeric = !digits.empty() && digits.size() <= 5
        && std::all_of(digits.begin(), digits.end(), [](char digit) { return digit >= '0' && digit <= '9'; });
    port = numeric ? std::stoi(digits) : -1;
    if ((protocol == "http" && port == 80) || (protocol == "https" && port == 443)) port = 0;
  }
}

// ICoreWebView2WebMessageReceivedEventHandler::Invoke, as the webview
// library's handler implements it.
using MessageInvoke = HRESULT(STDMETHODCALLTYPE *)(
    ICoreWebView2WebMessageReceivedEventHandler *, ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *);
MessageInvoke library_message_invoke = nullptr;

// The library's Invoke with the gate in front: a message from any document but
// the window's own page is dropped before the library reads it.
HRESULT STDMETHODCALLTYPE gated_message(ICoreWebView2WebMessageReceivedEventHandler *handler, ICoreWebView2 *sender,
    ICoreWebView2WebMessageReceivedEventArgs *args) {
  if (!is_from_window_page(args)) {
    report_once(refused_message, "refused a message to its native bridge from a page that is not the window's own");
    return S_OK;
  }
  return library_message_invoke(handler, sender, args);
}

} // namespace

bool is_from_window_page(ICoreWebView2WebMessageReceivedEventArgs *args) {
  LPWSTR source = nullptr;
  const bool described = args != nullptr && SUCCEEDED(args->get_Source(&source)) && source != nullptr;
  const std::string url = described ? utf8_of(source) : std::string();
  if (source != nullptr) CoTaskMemFree(source);
  std::string protocol;
  std::string host;
  int port = 0;
  origin_parts(url, protocol, host, port);
  return described && message_may_reach_native(true, comparable(url), protocol, comparable(host), port, remembered_prefixes());
}

void allow_window_prefix(const std::string &prefix) {
  if (prefix.empty()) return;
  const std::lock_guard<std::mutex> lock(window_prefix_mutex());
  auto &prefixes = window_prefixes();
  if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) prefixes.push_back(prefix);
}

void install_navigation_policy(void *web_view_controller) {
  auto *controller = static_cast<ICoreWebView2Controller *>(web_view_controller);
  if (controller == nullptr) return;
  ICoreWebView2 *web_view = nullptr;
  if (FAILED(controller->get_CoreWebView2(&web_view)) || web_view == nullptr)
    throw std::runtime_error("The window's navigation policy could not be installed, so no page is loaded");
  using webview_com::make_handler;
  EventRegistrationToken token{};
  // The window's own document, and every server redirect of it.
  const HRESULT navigation = web_view->add_NavigationStarting(
      make_handler<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
            LPWSTR uri = nullptr;
            const std::string url = SUCCEEDED(args->get_Uri(&uri)) ? utf8_of(uri) : std::string();
            if (uri != nullptr) CoTaskMemFree(uri);
            if (!window_may_show(comparable(url), remembered_prefixes(), true)) {
              args->put_Cancel(TRUE);
              report_once(refused_navigation, "kept its window from showing a page that is not its own");
            }
            return S_OK;
          })
          .Get(),
      &token);
  // A frame inside the window's page.
  const HRESULT frames = web_view->add_FrameNavigationStarting(
      make_handler<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
            LPWSTR uri = nullptr;
            const std::string url = SUCCEEDED(args->get_Uri(&uri)) ? utf8_of(uri) : std::string();
            if (uri != nullptr) CoTaskMemFree(uri);
            if (!window_may_show(comparable(url), remembered_prefixes(), false)) {
              args->put_Cancel(TRUE);
              report_once(refused_frame, "kept a frame of its window from showing a page that is not its own");
            }
            return S_OK;
          })
          .Get(),
      &token);
  // window.open and target="_blank": no second window, which would get the bridge.
  const HRESULT windows = web_view->add_NewWindowRequested(
      make_handler<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
            args->put_Handled(TRUE);
            report_once(refused_window, "kept its page from opening a second window");
            return S_OK;
          })
          .Get(),
      &token);
  web_view->Release();
  if (FAILED(navigation) || FAILED(frames) || FAILED(windows))
    throw std::runtime_error("The window's navigation policy could not be installed, so no page is loaded");
}

bool install_message_gate() {
  static std::mutex mutex;
  const std::lock_guard<std::mutex> lock(mutex);
  // Every webview2_com_handler shares one table of methods for its
  // ICoreWebView2WebMessageReceivedEventHandler part, the window's own handler
  // included: an instance made here, never registered with WebView2, gives
  // that table. Slot 3, after IUnknown's three, is Invoke.
  webview::detail::webview2_com_handler probe(nullptr, [](const std::string &) {},
      [](ICoreWebView2Controller *, ICoreWebView2 *) {});
  auto *handler = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(&probe);
  void **table = *reinterpret_cast<void ***>(handler);
  constexpr std::size_t invoke_slot = 3;
  void *const gate = reinterpret_cast<void *>(&gated_message);
  if (table == nullptr || table[invoke_slot] == nullptr) {
    std::cerr << "Singularity cannot guard its native bridge: the webview library's message handler is not "
                 "where Platform/WindowNavigation expects it. Review it before using this webview library.\n";
    return false;
  }
  if (table[invoke_slot] == gate) return true;
  DWORD previous = 0;
  if (!VirtualProtect(&table[invoke_slot], sizeof(void *), PAGE_READWRITE, &previous)) {
    std::cerr << "Singularity cannot guard its native bridge: its message handler could not be replaced.\n";
    return false;
  }
  library_message_invoke = reinterpret_cast<MessageInvoke>(table[invoke_slot]);
  table[invoke_slot] = gate;
  DWORD restored = 0;
  VirtualProtect(&table[invoke_slot], sizeof(void *), previous, &restored);
  FlushInstructionCache(GetCurrentProcess(), &table[invoke_slot], sizeof(void *));
  return true;
}

} // namespace singularity::platform
