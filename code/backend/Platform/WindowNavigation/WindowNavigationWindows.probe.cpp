// window_navigation_webview2_probe [--no-gate] [--no-policy] [--round file|dev]
//
// Real WebView2, in a window that is never shown (it takes no focus): the
// window loads a test page the way the app loads its frontend - round "file"
// from a temporary file:// folder (the packaged app), round "dev" from
// http://127.0.0.1:<port>/ (the development server) - with the navigation
// policy and the message gate installed as in the app
// (WindowNavigationWindows.cpp). Small HTTP servers in this process answer on
// 127.0.0.1: the dev round's own origin, and another origin's page.
//
// The window's page calls a bound native method ("probe") from its main frame,
// posts the library's message from an about:srcdoc iframe, asks for a second
// window, and then sends the window to the other origin, whose page calls the
// same method. WebView2 gives every top-level document the library's bindings,
// so without the policy and the gate that page reaches native code.
//
// Rows: the window's own main frame must arrive; the iframe, the other origin
// and a second window must not. --no-gate and --no-policy show what reaches
// native code without each. Exits non-zero on any failure. Nothing leaves
// 127.0.0.1.

#include "Platform/WindowNavigation/WindowNavigation.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <objbase.h>
#include <webview/webview.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

namespace platform = singularity::platform;

std::mutex received_mutex;
std::set<std::string> received;

std::string file_url_of(const std::filesystem::path &path) {
  std::string generic = std::filesystem::absolute(path).generic_u8string();
  std::string url = "file:///";
  for (const char character : generic) {
    const auto byte = static_cast<unsigned char>(character);
    const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
        || byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '/' || byte == ':';
    if (plain) {
      url.push_back(character);
    } else {
      char escape[4];
      std::snprintf(escape, sizeof(escape), "%%%02X", byte);
      url += escape;
    }
  }
  return url;
}

// Answers every request on 127.0.0.1:<port> with `page`, until stopped.
class OtherOrigin {
public:
  explicit OtherOrigin(std::string page) : page_(std::move(page)) {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener_, reinterpret_cast<const sockaddr *>(&address), static_cast<int>(sizeof(address)));
    listen(listener_, 8);
    int size = static_cast<int>(sizeof(address));
    getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] {
      for (;;) {
        const SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        // WebView2 may open a connection it never sends a request on (a
        // navigation the policy cancelled): never wait on it for long.
        const DWORD wait_ms = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&wait_ms),
            static_cast<int>(sizeof(wait_ms)));
        char request[4096];
        (void)recv(client, request, sizeof(request), 0);
        const std::string answer = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
            + std::to_string(page_.size()) + "\r\nConnection: close\r\n\r\n" + page_;
        send(client, answer.data(), static_cast<int>(answer.size()), 0);
        shutdown(client, SD_SEND);
        closesocket(client);
      }
    });
  }
  ~OtherOrigin() {
    closesocket(listener_);
    if (thread_.joinable()) thread_.join();
    WSACleanup();
  }
  unsigned short port() const { return port_; }

private:
  std::string page_;
  SOCKET listener_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::thread thread_;
};

} // namespace

int run_probe(int argc, char **argv);

int main(int argc, char **argv) {
  // The webview library initialises COM only for a window it makes itself;
  // this window is the probe's own.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    return run_probe(argc, argv);
  } catch (const std::exception &error) {
    std::cout << "FAIL the probe stopped: " << error.what() << '\n';
    return 1;
  }
}

int run_probe(int argc, char **argv) {
  bool gate = true;
  bool policy = true;
  std::string round = "file";
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--no-gate") == 0) gate = false;
    if (std::strcmp(argv[index], "--no-policy") == 0) policy = false;
    if (std::strcmp(argv[index], "--round") == 0 && index + 1 < argc) round = argv[++index];
  }

  // The other origin's page calls the bound method, as any page given the
  // window would.
  OtherOrigin other("<!doctype html><script>try { window.probe('other-origin'); } catch (e) {}</script>");
  const std::string other_url = "http://127.0.0.1:" + std::to_string(other.port()) + "/";

  // The window's own page.
  const std::string page = std::string("<!doctype html><body><script>\n"
                                       "window.probe('own-main-frame');\n"
                                       "const frame = document.createElement('iframe');\n"
                                       "frame.srcdoc = \"<script>try { window.chrome.webview.postMessage(JSON.stringify("
                                       "{id: 'frame-1', method: 'probe', params: ['iframe']})); } catch (e) {}\"\n"
                                       "  + \" try { window.probe('iframe-binding'); } catch (e) {}<\\/script>\";\n"
                                       "document.body.appendChild(frame);\n"
                                       "try { window.open('")
      + other_url + "'); } catch (e) {}\nsetTimeout(() => { location.href = '" + other_url
      + "'; }, 1500);\n</script></body>\n";

  // Round "file": a folder whose name has a space and a non-ASCII letter, as
  // the packaged app's under C:\Program Files or a user's own name has.
  const auto folder = std::filesystem::temp_directory_path()
      / std::filesystem::u8path("singularity navigation probe \xD0\x96-" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(folder);
  {
    std::ofstream file(folder / "index.html", std::ios::binary);
    file << page;
  }
  // Round "dev": the same page from the window's own origin, as the
  // development server serves the frontend.
  OtherOrigin own(page);
  const std::string own_url = "http://127.0.0.1:" + std::to_string(own.port()) + "/";

  // A window that is never shown: the probe takes no focus.
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"SingularityNavigationProbe";
  RegisterClassW(&window_class);
  HWND hidden = CreateWindowExW(0, window_class.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600,
      nullptr, nullptr, window_class.hInstance, nullptr);

  int failures = 0;
  {
    webview::webview view(false, hidden);
    if (gate && !platform::install_message_gate()) {
      std::cout << "FAIL the message gate could not be installed\n";
      return 1;
    }
    if (policy) {
      auto controller = view.browser_controller();
      if (!controller.ok()) {
        std::cout << "FAIL no WebView2 controller\n";
        return 1;
      }
      platform::install_navigation_policy(controller.value());
    }
    view.bind("probe", [](const std::string &request) -> std::string {
      const std::lock_guard<std::mutex> lock(received_mutex);
      received.insert(request);
      return "";
    });
    if (round == "dev") {
      platform::allow_window_prefix(platform::window_prefix_of(own_url, false));
      view.navigate(own_url);
    } else {
      platform::allow_window_prefix(platform::window_prefix_of(file_url_of(folder), true));
      view.navigate(file_url_of(folder / "index.html"));
    }
    std::thread stopper([&view] {
      std::this_thread::sleep_for(std::chrono::seconds(6));
      view.dispatch([&view] { view.terminate(); });
    });
    view.run();
    stopper.join();
  }
  DestroyWindow(hidden);

  const auto arrived = [](const std::string &who) {
    const std::lock_guard<std::mutex> lock(received_mutex);
    for (const std::string &request : received)
      if (request.find("\"" + who + "\"") != std::string::npos) return true;
    return false;
  };
  const auto row = [&failures](bool ok, const std::string &what) {
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
    if (!ok) ++failures;
  };
  std::cout << "round " << round << ", gate " << (gate ? "on" : "off") << ", policy " << (policy ? "on" : "off") << "\n";
  row(arrived("own-main-frame"), "the window's own main frame reaches native code");
  row(!arrived("iframe"), "a message posted from an iframe does not reach native code");
  row(!arrived("iframe-binding"), "an iframe's binding call does not reach native code");
  row(!arrived("other-origin"), "a page of another origin, in the window or a new one, does not reach native code");

  std::error_code ignored;
  std::filesystem::remove_all(folder, ignored);
  return failures == 0 ? 0 : 1;
}
