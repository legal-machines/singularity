// web_view_recovery_probe [--no-recovery]
//
// Real WebView2, in a window that is never shown (it takes no focus): the
// window shows a page, then this probe ends the page's renderer process, and
// later WebView2's browser process, as a graphics driver reset or a runtime
// update can. With the window's recovery installed (WebViewRecoveryWindows.cpp)
// the page is reloaded after its renderer is gone, and the window is told when
// its browser process is gone. --no-recovery shows a window left blank and
// never told. Exits non-zero on any failure.

#include "Platform/WebViewRecovery/WebViewRecovery.hpp"

#include <windows.h>

#include <objbase.h>
#include <webview/webview.h>
#include <wrl/event.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

std::atomic<int> navigations{0};
std::atomic<bool> told_browser_lost{false};

// Ends every process of `kind` WebView2 runs for this window's environment.
int end_processes(ICoreWebView2 *web_view, COREWEBVIEW2_PROCESS_KIND kind) {
  ICoreWebView2_2 *second = nullptr;
  if (FAILED(web_view->QueryInterface(IID_PPV_ARGS(&second))) || second == nullptr) return 0;
  ICoreWebView2Environment *environment = nullptr;
  second->get_Environment(&environment);
  second->Release();
  if (environment == nullptr) return 0;
  ICoreWebView2Environment8 *eighth = nullptr;
  environment->QueryInterface(IID_PPV_ARGS(&eighth));
  environment->Release();
  if (eighth == nullptr) return 0;
  ICoreWebView2ProcessInfoCollection *infos = nullptr;
  eighth->GetProcessInfos(&infos);
  eighth->Release();
  if (infos == nullptr) return 0;
  UINT32 count = 0;
  infos->get_Count(&count);
  int ended = 0;
  for (UINT32 index = 0; index < count; ++index) {
    ICoreWebView2ProcessInfo *info = nullptr;
    if (FAILED(infos->GetValueAtIndex(index, &info)) || info == nullptr) continue;
    COREWEBVIEW2_PROCESS_KIND found{};
    INT32 process_id = 0;
    info->get_Kind(&found);
    info->get_ProcessId(&process_id);
    info->Release();
    if (found != kind) continue;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process_id));
    if (process != nullptr && TerminateProcess(process, 1)) ++ended;
    if (process != nullptr) CloseHandle(process);
  }
  infos->Release();
  return ended;
}

int run_probe(bool recovery) {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"SingularityRecoveryProbe";
  RegisterClassW(&window_class);
  HWND hidden = CreateWindowExW(0, window_class.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600,
      nullptr, nullptr, window_class.hInstance, nullptr);

  int failures = 0;
  const auto row = [&failures](bool ok, const char *what) {
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
    if (!ok) ++failures;
  };
  {
    webview::webview view(false, hidden);
    auto *controller = static_cast<ICoreWebView2Controller *>(view.browser_controller().value());
    ICoreWebView2 *web_view = nullptr;
    controller->get_CoreWebView2(&web_view);
    EventRegistrationToken token{};
    web_view->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
              ++navigations;
              return S_OK;
            })
            .Get(),
        &token);
    if (recovery)
      singularity::platform::install_web_view_recovery(controller, [] { told_browser_lost = true; });
    view.set_html("<!doctype html><p>probe</p>");

    std::atomic<int> renderers_ended{0};
    std::atomic<int> browsers_ended{0};
    std::atomic<int> before_reload{0};
    std::atomic<int> after_reload{0};
    std::thread steps([&] {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      view.dispatch([&] {
        before_reload = navigations.load();
        renderers_ended = end_processes(web_view, COREWEBVIEW2_PROCESS_KIND_RENDERER);
      });
      std::this_thread::sleep_for(std::chrono::seconds(4));
      view.dispatch([&] {
        after_reload = navigations.load();
        UINT32 browser = 0;
        web_view->get_BrowserProcessId(&browser);
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, browser);
        if (process != nullptr && TerminateProcess(process, 1)) browsers_ended = 1;
        if (process != nullptr) CloseHandle(process);
      });
      std::this_thread::sleep_for(std::chrono::seconds(4));
      view.dispatch([&view] { view.terminate(); });
    });
    view.run();
    steps.join();
    web_view->Release();

    std::cout << "recovery " << (recovery ? "on" : "off") << "; renderers ended " << renderers_ended.load()
              << ", browser ended " << browsers_ended.load() << "\n";
    row(renderers_ended > 0, "the page's renderer process was ended");
    row(after_reload > before_reload, "a page whose renderer is gone is shown again (reloaded)");
    row(told_browser_lost.load(), "the window is told when its browser process is gone");
  }
  DestroyWindow(hidden);
  return failures;
}

} // namespace

int main(int argc, char **argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool recovery = !(argc > 1 && std::strcmp(argv[1], "--no-recovery") == 0);
  try {
    return run_probe(recovery) == 0 ? 0 : 1;
  } catch (const std::exception &error) {
    std::cout << "FAIL the probe stopped: " << error.what() << '\n';
    return 1;
  }
}
