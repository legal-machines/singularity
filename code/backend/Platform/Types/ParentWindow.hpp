#pragma once

namespace singularity::platform {

// The window a native dialog opens over (AppWindow::dialog_parent,
// NativeDialogs): the system keeps the dialog in front of it, and on Windows
// the window takes no input until the dialog closes. `handle` is the window as
// its system names it: an HWND on Windows, an NSWindow on macOS, an X11 window
// id on Linux. `none` leaves a dialog without one, as for a window not shown.
struct ParentWindow {
  enum class System { none, windows, cocoa, x11 };

  System system = System::none;
  void *handle = nullptr;
};

} // namespace singularity::platform
