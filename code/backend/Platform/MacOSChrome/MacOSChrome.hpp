#pragma once

#include <functional>

namespace singularity::platform {

void configure_macos_chrome(void *window_handle, void *webview_handle,
                            const char *application_name);

// The application is asked to quit: the Quit item, Cmd+Q, the Dock, an
// AppleScript quit, logging out or shutting down. `quit` is called once, on
// the window's thread, and must end the process; AppKit waits meanwhile
// (NSTerminateLater) with the window still live, so `quit` returns at once and
// does its work elsewhere. Without it AppKit would call exit() from inside its
// run loop, and Singularity would never record what was running (Beta 1 #55).
// Called after the window exists: its application delegate is then set.
// The windows leave the screen before `quit` is called (hide_application_windows).
void install_quit_handler(std::function<void()> quit);

// Takes every window of Singularity off the screen at once, before the
// application spends up to its closing patience recording where its work
// stands (Platform/ApplicationMain): a quit or a closed window is seen to
// happen when it is asked for (the owner, 2026-11-01: "Cmd+Q and Cmd+W do not
// work" - the window stood frozen for five seconds). Call on the main thread.
void hide_application_windows();

} // namespace singularity::platform

