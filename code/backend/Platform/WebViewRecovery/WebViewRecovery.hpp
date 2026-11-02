#pragma once

#include <functional>

namespace singularity::platform {

/*
 * What the window does when WebView2 loses one of its processes
 * (ICoreWebView2::ProcessFailed), which a graphics driver reset, an update of
 * the WebView2 Runtime or memory pressure can cause. Without it the window
 * stayed blank or unresponsive, and Singularity later crashed calling into a
 * web view that was gone (2026-10-21, after a GPU reset).
 *
 * - The page's renderer exited or stopped answering: the page is reloaded; the
 *   frontend reads its state from the native side again, so nothing is lost.
 * - The browser process exited: the web view cannot be used again in this
 *   window. `on_browser_lost` runs once, on the window's thread, so the window
 *   can stop calling into it, tell the person and close in order (work in
 *   flight is recorded as interrupted on the way out).
 * - The GPU process exited: WebView2 restarts it by itself; nothing to do.
 *
 * `web_view_controller` is the window's ICoreWebView2Controller. Windows only.
 */
void install_web_view_recovery(void *web_view_controller, std::function<void()> on_browser_lost);

} // namespace singularity::platform
