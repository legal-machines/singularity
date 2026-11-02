#pragma once

// The Windows side of the window's native bridge gate
// (WindowNavigationWindows.cpp), for other Windows code that reads what the
// page posts (Platform/FileDrop).

struct ICoreWebView2WebMessageReceivedEventArgs;

namespace singularity::platform {

// Whether a message WebView2 received comes from the window's own page, as the
// gate in front of the native bridge decides it (message_may_reach_native()
// with the window's remembered prefixes). Anything else is to be dropped
// unread.
bool is_from_window_page(ICoreWebView2WebMessageReceivedEventArgs *args);

} // namespace singularity::platform
