#pragma once

#include "Platform/Types/ParentWindow.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace singularity::app {

/*
 * AppWindow
 * =========
 *
 * Cross-platform native application window backed by webview.
 *
 * Window size semantics
 * ---------------------
 *
 * All dimensions passed through this interface describe the application's
 * content area in logical pixels.
 *
 * They do not describe:
 *
 *   - physical Retina pixels;
 *   - the complete decorated native window frame;
 *   - CSS device pixels after devicePixelRatio scaling.
 *
 * Therefore:
 *
 *   set_initial_size(1080, 720)
 *
 * means that the frontend receives a logical content area of 1080 x 720.
 *
 * This distinction is especially important on macOS, where the complete
 * NSWindow frame may be larger because of native window chrome.
 *
 *
 * Typical lifecycle
 * -----------------
 *
 * Onboarding:
 *
 *   window.set_initial_size(1080, 720);
 *   window.set_minimum_size(1080, 720);
 *   window.set_maximum_size(1080, 720);
 *
 * This effectively locks the onboarding window at 1080 x 720 while keeping
 * the underlying native window resizable.
 *
 *
 * Main application:
 *
 *   window.clear_size_constraints();
 *   window.set_minimum_size(900, 650);
 *   window.set_size(1440, 900);
 *
 * The same native window can therefore transition from a fixed onboarding
 * layout into a normal freely resizable desktop application without being
 * destroyed or recreated.
 *
 *
 * Constraint convention
 * ---------------------
 *
 * set_minimum_size(0, 0)
 *     Removes the custom minimum-size constraint.
 *
 * set_maximum_size(0, 0)
 *     Removes the custom maximum-size constraint.
 *
 * clear_size_constraints()
 *     Removes both constraints.
 *
 * Width and height supplied to a size constraint must either both be zero
 * or both be positive.
 */
class AppWindow final {
public:
  using Binding =
      std::function<void(const std::string &request_id,
                         const std::string &request)>;

  /*
   * Creates the native window and its webview.
   *
   * debug = true enables webview debugging/developer functionality where
   * supported by the underlying platform backend.
   */
  explicit AppWindow(bool debug = false);

  ~AppWindow();

  /*
   * AppWindow owns native window state and must have exactly one owner.
   *
   * Copying and moving are disabled deliberately because the underlying
   * native window, callbacks and platform-specific state contain references
   * to the owning AppWindow::Impl instance.
   */
  AppWindow(const AppWindow &) = delete;
  AppWindow &operator=(const AppWindow &) = delete;
  AppWindow(AppWindow &&) = delete;
  AppWindow &operator=(AppWindow &&) = delete;

  /*
   * Native bridge
   * -------------
   *
   * Exposes a named native callback to JavaScript running inside the
   * application webview.
   */
  void bind(const std::string &name, Binding binding);

  /*
   * Schedules callback execution on the webview/native UI thread.
   */
  void dispatch(std::function<void()> callback);

  /*
   * Executes JavaScript in the currently loaded page.
   */
  void evaluate(const std::string &javascript);

  /*
   * Installs JavaScript that should be initialized for webview content.
   */
  void initialize(const std::string &javascript);

  /*
   * Navigation
   * ----------
   */

  /*
   * Navigates the webview to an ordinary URL.
   */
  void navigate(const std::string &url);

  /*
   * Loads a packaged local frontend.
   *
   * read_access_url identifies the directory that the local document may
   * read from.
   *
   * On macOS this is needed by WKWebView so that packaged Vite JS, CSS and
   * other adjacent assets can be loaded from disk.
   */
  void navigate_local_file(const std::string &file_url,
                           const std::string &read_access_url);

  /*
   * Resolves a pending native binding request.
   */
  void resolve(const std::string &request_id,
               int status,
               const std::string &json_result);

  /*
   * Starts the native application event loop.
   *
   * Normally called once after window configuration and frontend navigation
   * are complete.
   */
  void run();

  /*
   * The system asks the application to end, other than by closing its window:
   *
   *   macOS:   the Quit item, Cmd+Q, the Dock, an AppleScript quit, logging
   *            out or shutting down;
   *   Windows: signing out, shutting down or restarting, or an installer's
   *            Restart Manager (WM_ENDSESSION);
   *   Linux:   SIGTERM, SIGINT or SIGHUP, as the session ends among others.
   *
   * `quit` is called once, on the window's thread, and must end the process;
   * the system waits meanwhile with the window still live. Closing the window
   * stays the ordinary way to quit: run() returns.
   */
  void on_quit_request(std::function<void()> quit);

  /*
   * Window geometry
   * ---------------
   */

  /*
   * Sets the initial logical content size.
   *
   * This should normally be used during application startup.
   *
   * The implementation is nevertheless safe to call again later, which is
   * why set_size() below can use the same implementation.
   *
   * width and height must both be positive.
   */
  void set_initial_size(int width, int height);

  /*
   * Changes the current logical content size at runtime.
   *
   * This is the preferred name once the application is already running.
   *
   * Example after onboarding:
   *
   *   window.clear_size_constraints();
   *   window.set_minimum_size(900, 650);
   *   window.set_size(1440, 900);
   *
   * This wrapper intentionally delegates to the same native implementation
   * as set_initial_size(), so startup and runtime sizing always use exactly
   * the same coordinate system.
   */
  void set_size(int width, int height) {
    set_initial_size(width, height);
  }

  /*
   * Sets the minimum logical content size.
   *
   * Passing (0, 0) removes the explicit minimum-size constraint.
   */
  void set_minimum_size(int width, int height);

  /*
   * Sets the maximum logical content size.
   *
   * Passing (0, 0) removes the explicit maximum-size constraint.
   *
   * During onboarding, setting the minimum and maximum to the same dimensions
   * effectively locks the window without permanently disabling its native
   * resize capability.
   */
  void set_maximum_size(int width, int height);

  /*
   * Removes application-defined minimum and maximum size constraints.
   *
   * This is intended primarily for the transition from the fixed onboarding
   * window to the normal application workspace.
   *
   * A new minimum size can be installed immediately afterwards if the main
   * application should remain usable only above a certain viewport size.
   */
  void clear_size_constraints() {
    set_minimum_size(0, 0);
    set_maximum_size(0, 0);
  }

  /*
   * Window appearance
   * -----------------
   */

  /*
   * Sets the native window title.
   */
  void set_title(const std::string &title);

  /*
   * Applies the packaged application icon where runtime icon configuration
   * is required by the current platform.
   *
   * On macOS the application bundle provides the icon instead.
   */
  void use_application_icon();

  /*
   * The window native dialogs open over (Platform/NativeDialogs): its HWND on
   * Windows, where a dialog without one belongs to no window, and its NSWindow
   * on macOS. Linux gives none: GTK's dialog is modal to the application
   * already, and a parent there is an X11 window id, which would bring Xlib's
   * macros into this window's code. None, too, before the window exists.
   */
  platform::ParentWindow dialog_parent();

  /*
   * Enables the platform-specific integrated titlebar.
   *
   * macOS:
   *   Keeps native traffic lights while allowing frontend content to extend
   *   underneath a transparent titlebar.
   *
   * Windows:
   *   Puts the caption in the page (D-049): Windows draws no caption strip,
   *   the page's header rows drag the window (CSS app-region) and the page
   *   draws minimize, maximize and close at the right of its header
   *   (frame(), run_caption_command()). The side and bottom borders and a
   *   strip along the top edge still resize the window. Where the installed
   *   WebView2 cannot let a page drag its window, the standard caption stays.
   *
   * Linux:
   *   Uses GTK client-side decorations.
   *
   * The implementation is idempotent and is intended to remain compatible
   * with later runtime window resizing.
   */
  void use_integrated_titlebar();

  /*
   * The window's frame, as the page needs it to draw its header.
   *
   * caption_in_page
   *   True once the page carries the window's caption: it draws minimize,
   *   maximize and close itself. Windows only; macOS and Linux keep their
   *   native window controls.
   *
   * maximized
   *   Whether the window is maximized now, so the page's maximize button
   *   shows restore. Reported where the caption is in the page; false
   *   elsewhere.
   */
  struct Frame {
    bool caption_in_page = false;
    bool maximized = false;
  };

  Frame frame() const;

  /*
   * One of the commands of the window's standard caption buttons, run from
   * the page's own buttons exactly as Windows runs them (WM_SYSCOMMAND):
   * the same animations, Snap state and quit as the native buttons. Close
   * is posted, so it runs after the caller returns.
   *
   * Returns whether the command was taken: false where the window has
   * nothing to do with it (macOS and Linux, whose native controls stay, or a
   * window that cannot be maximized now).
   *
   * Must be called on the UI thread.
   */
  enum class CaptionCommand {
    Minimize,
    Maximize,
    Restore,
    Close,
  };

  bool run_caption_command(CaptionCommand command);

  /*
   * One control the page has placed inside the strip at the top of the
   * window: a rectangle in logical pixels, measured from the top-left corner
   * of the content area (the same space CSS uses).
   */
  struct TitlebarControl {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
  };

  /*
   * Tells the window where the page's own controls are inside the strip that
   * otherwise drags the window.
   *
   * macOS:
   *   The integrated titlebar puts a transparent native drag region over the
   *   top of the page. A press inside one of these rectangles goes to the
   *   page instead; a press anywhere else in the strip still drags the
   *   window. An empty list gives the whole strip back to dragging.
   *
   * Windows and Linux:
   *   No effect: the page already receives every press there. (On Windows
   *   the page marks its drag areas itself, with CSS app-region.)
   *
   * Replaces the previous list. Must be called on the UI thread.
   */
  void set_titlebar_controls(std::vector<TitlebarControl> controls);

private:
  /*
   * Platform-specific implementation.
   *
   * Keeping native WebKit/WebView2/GTK types inside Impl prevents platform
   * headers and implementation details from leaking into the public header.
   */
  class Impl;

  std::unique_ptr<Impl> impl_;
};

} // namespace singularity::app