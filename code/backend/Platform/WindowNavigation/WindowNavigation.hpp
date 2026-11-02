#pragma once

#include <string>
#include <vector>

namespace singularity::platform {

/*
 * Whether the window may show `url`: about:blank, or a page under one of the
 * prefixes it was loaded from - the packaged frontend's folder, the development
 * server's origin. A sub-frame may also show what a page builds itself
 * (about:srcdoc, blob:, data:). Any other site - a link dropped on the window, a
 * page a rendered document points at, where a server redirect leads - never gets
 * the window, whose scripts can call every native method.
 */
bool window_may_show(const std::string &url, const std::vector<std::string> &allowed_prefixes, bool main_frame);

/*
 * Whether a message a page posts to the native bridge may reach it: it comes
 * from the window's main frame, showing a page under one of the prefixes
 * (`url`, the frame's own address), and that frame's security origin is
 * exactly the prefix's - scheme "file" with no host and port 0 for the packaged
 * frontend's folder; the scheme, host and port of the development server. A
 * frame inside the page - about:srcdoc, data:, blob:, sandboxed, another
 * origin's - never does, nor does a main-frame page of any other origin or
 * folder, whatever let the window show it. `port` is the port WebKit reports,
 * which is 0 for a file URL and for a scheme's default port.
 */
bool message_may_reach_native(bool main_frame, const std::string &url, const std::string &protocol,
    const std::string &host, int port, const std::vector<std::string> &allowed_prefixes);

/*
 * The prefix under which the window may show pages, from a URL it was loaded
 * from: the folder of a file URL, the origin of an http(s) URL. Empty when the
 * URL is neither.
 */
std::string window_prefix_of(const std::string &url, bool directory);

// The prefix under which the window may now show pages (window_prefix_of()),
// remembered for every later navigation. One window per process.
void allow_window_prefix(const std::string &prefix);

// Makes every navigation of `web_view` ask window_may_show() first, with the
// prefixes allow_window_prefix() remembered: on macOS a WKWebView; on Windows
// the window's ICoreWebView2Controller, whose new windows are refused too.
void install_navigation_policy(void *web_view);

/*
 * Puts message_may_reach_native() in front of the webview library's native
 * bridge. On macOS the library hands every message any frame posts to
 * window.webkit.messageHandlers.__webview__ to the bound native methods
 * (-[WebviewWKScriptMessageHandler userContentController:didReceiveScriptMessage:]);
 * on Windows, every message the top-level document posts to
 * window.chrome.webview (detail::webview2_com_handler's WebMessageReceived
 * Invoke). With the gate, only text from the window's own page is passed on,
 * and everything else is dropped unread. Returns false, after saying why on
 * std::cerr, when the library's handler is not the one the gate was written
 * for - the window must then load no page. Safe to call more than once. macOS
 * and Windows.
 */
bool install_message_gate();

/*
 * Linux (WebKitGTK, WindowNavigationLinux.cpp). install_navigation_policy()
 * above takes the window's WebKitWebView there; a new window is refused too.
 *
 * install_message_gate(web_view) puts message_may_reach_native() in front of
 * the webview library's native bridge on the WebKitWebView `web_view`. The
 * library receives every message any frame posts to
 * window.webkit.messageHandlers.__webview__
 * ("script-message-received::__webview__"), and WebKitGTK says nothing of the
 * frame that posted it. The gate takes that name out of the pages' script
 * world: the page's calls reach native code only through an isolated world of
 * the gate's own, where only the gate's forwarder runs, in the top frame, and
 * says what that frame's document really is. Returns false, after saying why
 * on std::cerr, when the library's handler is not the one the gate was written
 * for - the window must then load no page. The library's bind() removes every
 * user script, the gate's too: call it again after each bind(); it adds its
 * script back once and changes nothing else.
 */
bool install_message_gate(void *web_view);

// The script that hands the window page's calls to the gate on Linux. The
// window adds it right after the webview library's own script
// (webview::webview::init), before any binding.
const char *message_gate_page_script();

} // namespace singularity::platform
