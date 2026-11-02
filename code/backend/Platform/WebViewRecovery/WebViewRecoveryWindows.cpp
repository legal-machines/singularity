#include "Platform/WebViewRecovery/WebViewRecovery.hpp"

#include <windows.h>

// WebView2.h needs objbase.h's `interface`, which WIN32_LEAN_AND_MEAN leaves out.
#include <objbase.h>

#include <WebView2.h>

#include "Platform/WebViewCom/WebViewCom.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace singularity::platform {

void install_web_view_recovery(void *web_view_controller, std::function<void()> on_browser_lost) {
  auto *controller = static_cast<ICoreWebView2Controller *>(web_view_controller);
  if (controller == nullptr) return;
  ICoreWebView2 *web_view = nullptr;
  if (FAILED(controller->get_CoreWebView2(&web_view)) || web_view == nullptr)
    throw std::runtime_error("The window could not watch its web view for failures");
  auto lost = std::make_shared<std::function<void()>>(std::move(on_browser_lost));
  auto told = std::make_shared<std::atomic<bool>>(false);
  EventRegistrationToken token{};
  const HRESULT added = web_view->add_ProcessFailed(
      webview_com::make_handler<ICoreWebView2ProcessFailedEventHandler>(
          [lost, told](ICoreWebView2 *sender, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            if (FAILED(args->get_ProcessFailedKind(&kind))) return S_OK;
            switch (kind) {
            case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
            case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
              std::cerr << "Singularity's page stopped (WebView2 renderer); it is reloaded.\n";
              sender->Reload();
              break;
            case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
              if (!told->exchange(true)) {
                std::cerr << "Singularity's web view is gone (WebView2 browser process exited).\n";
                if (*lost) (*lost)();
              }
              break;
            default:
              // The GPU process (restarted by WebView2), a frame's renderer
              // (its page stays), utility processes: nothing to do.
              break;
            }
            return S_OK;
          })
          .Get(),
      &token);
  web_view->Release();
  if (FAILED(added)) throw std::runtime_error("The window could not watch its web view for failures");
}

} // namespace singularity::platform
