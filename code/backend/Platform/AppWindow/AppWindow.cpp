#include "Platform/AppWindow/AppWindow.hpp"

#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/FileDrop/FileDrop.hpp"
#include "Platform/WindowNavigation/WindowNavigation.hpp"
#if defined(_WIN32)
#include "Platform/WebViewData/WebViewData.hpp"
#include "Platform/WebViewRecovery/WebViewRecovery.hpp"
#endif

#if defined(__APPLE__)
#include "Platform/MacOSChrome/MacOSChrome.hpp"
#endif

#include <webview/webview.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include "Platform/WebViewCom/WebViewCom.hpp"
#elif defined(__linux__)
#include <glib-unix.h>
#include <gtk/gtk.h>

#include <csignal>
#endif

namespace {

/*
 * AppWindow sizing semantics
 * --------------------------
 *
 * Every size exposed through AppWindow describes the application's content
 * area in logical pixels.
 *
 * It does not describe:
 *
 *   - physical Retina pixels;
 *   - the complete decorated native window frame;
 *   - devicePixelRatio-scaled pixels.
 *
 * Therefore:
 *
 *   AppWindow 1080 x 720
 *
 * means:
 *
 *   frontend content area 1080 x 720
 *
 * On macOS the complete NSWindow frame may be slightly different because
 * AppKit owns native window chrome.
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
 * Width and height must either both be zero or both be positive.
 *
 *
 * Onboarding
 * ----------
 *
 * The onboarding window can temporarily be locked using:
 *
 *   initial = 1080 x 720
 *   minimum = 1080 x 720
 *   maximum = 1080 x 720
 *
 * We deliberately do not use WEBVIEW_HINT_FIXED.
 *
 * The underlying native window remains resizable. The equal minimum and
 * maximum dimensions temporarily prevent resizing.
 *
 * This allows the same native window to become freely resizable after
 * onboarding without recreating the window.
 */

/*
 * Some platform APIs require a concrete maximum dimension rather than a
 * representation of infinity.
 *
 * One million logical pixels is effectively unlimited for a desktop window
 * while remaining safely inside ordinary integer ranges.
 */
constexpr int practical_unbounded_dimension = 1000000;

void validate_current_size(int width, int height) {
  if (width <= 0 || height <= 0) {
    throw std::invalid_argument(
        "Window size must have a positive width and height");
  }
}

void validate_size_constraint(
    int width,
    int height,
    const char* constraint_name) {
  if (width < 0 || height < 0) {
    throw std::invalid_argument(
        std::string(constraint_name) +
        " window size cannot contain negative dimensions");
  }

  const bool width_is_zero = width == 0;
  const bool height_is_zero = height == 0;

  if (width_is_zero != height_is_zero) {
    throw std::invalid_argument(
        std::string(constraint_name) +
        " window size must use either two positive dimensions or (0, 0)");
  }
}

int normalized_maximum_dimension(int value) {
  return value == 0
             ? practical_unbounded_dimension
             : value;
}

#if defined(__APPLE__)

using ObjectiveCObject = void*;

/*
 * NSSize compatibility type
 * -------------------------
 *
 * On 64-bit macOS CGFloat is a double, so NSSize consists of two doubles.
 *
 * We intentionally use the Objective-C runtime here instead of importing
 * AppKit into the public C++ window abstraction.
 */
struct NativeSize {
  double width;
  double height;
};

NativeSize native_size(double width, double height) {
  return NativeSize{
      width,
      height};
}

NativeSize native_size(int width, int height) {
  return native_size(
      static_cast<double>(width),
      static_cast<double>(height));
}

/*
 * Generic Objective-C message helper.
 *
 * This keeps native AppKit interaction isolated inside the macOS
 * implementation without introducing Objective-C++ into the rest of the
 * application.
 */
template <typename Return, typename... Args>
Return send_message(
    ObjectiveCObject receiver,
    const char* selector,
    Args... args) {
  const auto function =
      reinterpret_cast<
          Return (*)(ObjectiveCObject, SEL, Args...)>(
          objc_msgSend);

  return function(
      receiver,
      sel_registerName(selector),
      args...);
}

ObjectiveCObject object_class(const char* name) {
  return reinterpret_cast<ObjectiveCObject>(
      objc_getClass(name));
}

ObjectiveCObject ns_url(const std::string& url) {
  const auto string =
      send_message<ObjectiveCObject>(
          object_class("NSString"),
          "stringWithUTF8String:",
          url.c_str());

  return send_message<ObjectiveCObject>(
      object_class("NSURL"),
      "URLWithString:",
      string);
}

/*
 * Native macOS sizing
 * -------------------
 *
 * Do not replace these functions with webview::set_size().
 *
 * webview's Cocoa backend currently interprets:
 *
 *   WEBVIEW_HINT_NONE
 *       as NSWindow frame size
 *
 * while:
 *
 *   WEBVIEW_HINT_MIN
 *   WEBVIEW_HINT_MAX
 *
 * operate on NSWindow content size.
 *
 * Mixing the two coordinate systems caused the original issue where an
 * apparently fixed 1080 x 720 onboarding window could still be enlarged by
 * approximately the titlebar height.
 *
 * webview::set_size() also rebuilds the NSWindow style mask. That could
 * remove FullSizeContentView after the integrated titlebar has already been
 * configured.
 *
 * AppWindow therefore owns native content sizing directly on macOS.
 */

void set_macos_content_size(
    ObjectiveCObject window,
    int width,
    int height) {
  send_message<void>(
      window,
      "setContentSize:",
      native_size(width, height));
}

void set_macos_minimum_content_size(
    ObjectiveCObject window,
    int width,
    int height) {
  send_message<void>(
      window,
      "setContentMinSize:",
      native_size(width, height));
}

void set_macos_maximum_content_size(
    ObjectiveCObject window,
    int width,
    int height) {
  const int normalized_width =
      normalized_maximum_dimension(width);

  const int normalized_height =
      normalized_maximum_dimension(height);

  send_message<void>(
      window,
      "setContentMaxSize:",
      native_size(
          normalized_width,
          normalized_height));
}

/*
 * Standard macOS traffic-light buttons
 * ------------------------------------
 *
 * NSWindowButton values:
 *
 *   0 = Close
 *   1 = Miniaturize
 *   2 = Zoom
 *
 * We keep the numeric definitions local so the project does not need to
 * import AppKit headers merely to control the standard buttons.
 */
constexpr long macos_close_button = 0;
constexpr long macos_miniaturize_button = 1;
constexpr long macos_zoom_button = 2;

ObjectiveCObject macos_standard_window_button(
    ObjectiveCObject window,
    long button_type) {
  return send_message<ObjectiveCObject>(
      window,
      "standardWindowButton:",
      button_type);
}

void configure_macos_standard_window_button(
    ObjectiveCObject window,
    long button_type,
    bool enabled) {
  const auto button =
      macos_standard_window_button(
          window,
          button_type);

  if (button == nullptr) {
    return;
  }

  /*
   * Keep native traffic lights visible.
   *
   * platform::configure_macos_chrome() may reposition or otherwise style
   * them, but AppWindow owns whether the controls are available.
   */
  send_message<void>(
      button,
      "setHidden:",
      false);

  send_message<void>(
      button,
      "setEnabled:",
      enabled);
}

/*
 * Custom macOS drag region
 * ------------------------
 *
 * The WKWebView renders underneath the transparent titlebar.
 *
 * This NSView covers the dedicated draggable region at the top and forwards
 * mouse-down events to NSWindow's native window-dragging mechanism.
 */

void drag_region_mouse_down(
    ObjectiveCObject view,
    SEL,
    ObjectiveCObject event) {
  const auto window =
      send_message<ObjectiveCObject>(
          view,
          "window");

  if (window != nullptr) {
    send_message<void>(
        window,
        "performWindowDragWithEvent:",
        event);
  }
}

bool drag_region_can_move_window(
    ObjectiveCObject,
    SEL) {
  return true;
}

/*
 * The page's own controls inside the drag region
 * ----------------------------------------------
 *
 * The drag region lies over the top of the page, so a control the page puts
 * there would never be pressed: the region takes the press and drags the
 * window. The page therefore tells the window where those controls are
 * (AppWindow::set_titlebar_controls), and the region steps aside exactly
 * there: hitTest: answers nil inside a control's rectangle, AppKit carries on
 * to the WKWebView underneath, and everywhere else the strip still drags.
 *
 * Read and written on the UI thread only, like every other AppKit state here.
 */
struct NativePoint {
  double x;
  double y;
};

std::vector<singularity::app::AppWindow::TitlebarControl>& titlebar_controls() {
  static std::vector<singularity::app::AppWindow::TitlebarControl> controls;
  return controls;
}

/*
 * The region is flipped so that its own coordinates run from its top-left
 * corner, which is also the top-left corner of the page.
 */
bool drag_region_is_flipped(
    ObjectiveCObject,
    SEL) {
  return true;
}

ObjectiveCObject drag_region_hit_test(
    ObjectiveCObject view,
    SEL selector,
    NativePoint point) {
  /*
   * NSView's own answer first: nil when the point is outside the region.
   * The superclass is named, not looked up from the instance, so a
   * dynamically created subclass can never send this back to itself.
   */
  objc_super parent{
      reinterpret_cast<id>(view),
      reinterpret_cast<Class>(
          objc_getClass("NSView"))};

  const auto parent_hit_test =
      reinterpret_cast<
          ObjectiveCObject (*)(objc_super*, SEL, NativePoint)>(
          objc_msgSendSuper);

  const auto hit =
      parent_hit_test(
          &parent,
          selector,
          point);

  if (hit == nullptr) {
    return nullptr;
  }

  const auto local =
      send_message<NativePoint>(
          view,
          "convertPoint:fromView:",
          point,
          send_message<ObjectiveCObject>(
              view,
              "superview"));

  for (const auto& control : titlebar_controls()) {
    if (local.x >= control.x &&
        local.x < control.x + control.width &&
        local.y >= control.y &&
        local.y < control.y + control.height) {
      return nullptr;
    }
  }

  return hit;
}

Class drag_region_class() {
  constexpr const char* class_name =
      "NativeAppWindowDragRegion";

  if (const auto existing =
          objc_lookUpClass(class_name)) {
    return existing;
  }

  const auto base =
      reinterpret_cast<Class>(
          objc_getClass("NSView"));

  const auto created =
      objc_allocateClassPair(
          base,
          class_name,
          0);

  if (created == nullptr) {
    throw std::runtime_error(
        "Could not create the macOS drag-region class");
  }

  class_addMethod(
      created,
      sel_registerName("mouseDown:"),
      reinterpret_cast<IMP>(
          &drag_region_mouse_down),
      "v@:@");

  class_addMethod(
      created,
      sel_registerName("mouseDownCanMoveWindow"),
      reinterpret_cast<IMP>(
          &drag_region_can_move_window),
      "B@:");

  class_addMethod(
      created,
      sel_registerName("isFlipped"),
      reinterpret_cast<IMP>(
          &drag_region_is_flipped),
      "B@:");

  class_addMethod(
      created,
      sel_registerName("hitTest:"),
      reinterpret_cast<IMP>(
          &drag_region_hit_test),
      "@@:{CGPoint=dd}");

  objc_registerClassPair(created);

  return created;
}

void install_drag_region(
    ObjectiveCObject content_view) {
  // The traffic lights' strip (MacOSChrome's window_strip_height, and
  // --titlebar-height on macOS in code/frontend/styles/tokens.css).
  constexpr double drag_region_height = 62.0;

  const auto drag_view =
      send_message<ObjectiveCObject>(
          reinterpret_cast<ObjectiveCObject>(
              drag_region_class()),
          "new");

  send_message<void>(
      drag_view,
      "setTranslatesAutoresizingMaskIntoConstraints:",
      false);

  send_message<void>(
      content_view,
      "addSubview:",
      drag_view);

  const auto top =
      send_message<ObjectiveCObject>(
          send_message<ObjectiveCObject>(
              drag_view,
              "topAnchor"),
          "constraintEqualToAnchor:",
          send_message<ObjectiveCObject>(
              content_view,
              "topAnchor"));

  const auto leading =
      send_message<ObjectiveCObject>(
          send_message<ObjectiveCObject>(
              drag_view,
              "leadingAnchor"),
          "constraintEqualToAnchor:",
          send_message<ObjectiveCObject>(
              content_view,
              "leadingAnchor"));

  const auto trailing =
      send_message<ObjectiveCObject>(
          send_message<ObjectiveCObject>(
              drag_view,
              "trailingAnchor"),
          "constraintEqualToAnchor:",
          send_message<ObjectiveCObject>(
              content_view,
              "trailingAnchor"));

  const auto height =
      send_message<ObjectiveCObject>(
          send_message<ObjectiveCObject>(
              drag_view,
              "heightAnchor"),
          "constraintEqualToConstant:",
          drag_region_height);

  const ObjectiveCObject constraints[]{
      top,
      leading,
      trailing,
      height};

  const auto constraint_array =
      send_message<ObjectiveCObject>(
          object_class("NSArray"),
          "arrayWithObjects:count:",
          constraints,
          sizeof(constraints) /
              sizeof(constraints[0]));

  send_message<void>(
      object_class("NSLayoutConstraint"),
      "activateConstraints:",
      constraint_array);

  /*
   * addSubview retains the drag view. Release the ownership obtained from
   * "new" after the view has been attached to the hierarchy.
   */
  send_message<void>(
      drag_view,
      "release");
}

#endif

#if defined(_WIN32)
// Names WebView2's data folder to webview before the window's webview is made:
// this computer's application data, never the roaming profile
// (Platform/WebViewData, D-040). False when the folder cannot be readied;
// webview's own choice then stands.
bool name_web_view_data_folder() {
  const auto folder = singularity::platform::ready_web_view_data_directory();
  if (folder.empty()) return false;
  webview::mswebview2_user_data_folder() = folder.wstring();
  return true;
}
#endif

} // namespace

namespace singularity::app {

class AppWindow::Impl final {
public:
  explicit Impl(bool debug)
      : view(debug, nullptr) {}

#if defined(_WIN32)
  // Named before the view below is made, which starts WebView2 in it.
  bool web_view_data_named = name_web_view_data_folder();
#endif
  webview::webview view;

  /*
   * Stored geometry
   * ---------------
   *
   * AppWindow keeps its requested geometry independently of webview.
   *
   * This allows us to reapply content dimensions after native chrome changes
   * and lets the same window transition cleanly between onboarding and the
   * normal application workspace.
   */

  bool has_current_size = false;
  int current_width = 0;
  int current_height = 0;

  bool has_minimum_size = false;
  int minimum_width = 0;
  int minimum_height = 0;

  bool has_maximum_size = false;
  int maximum_width = 0;
  int maximum_height = 0;

  /*
   * use_integrated_titlebar() is one-time initialization.
   *
   * This guard prevents accidental duplicate drag regions or duplicate
   * platform-specific frame hooks.
   */
  bool integrated_titlebar_installed = false;

#if defined(__APPLE__)

  ObjectiveCObject macos_window() {
    auto native_window =
        view.window();

    native_window.ensure_ok();

    return native_window.value();
  }

  /*
   * Returns true only when both minimum and maximum are active and describe
   * exactly the same content size.
   *
   * That is our onboarding "temporarily fixed" state.
   */
  bool macos_size_is_locked() const {
    return
        has_minimum_size &&
        minimum_width > 0 &&
        minimum_height > 0 &&
        has_maximum_size &&
        maximum_width > 0 &&
        maximum_height > 0 &&
        minimum_width == maximum_width &&
        minimum_height == maximum_height;
  }

  /*
   * Keep macOS traffic lights synchronized with current window constraints.
   *
   * Close
   *     Always available.
   *
   * Minimize
   *     Always available.
   *
   * Zoom
   *     Disabled only while min == max.
   *
   * Once onboarding constraints are relaxed, the green Zoom button becomes
   * active automatically.
   */
  void refresh_macos_window_controls() {
    const auto window =
        macos_window();

    configure_macos_standard_window_button(
        window,
        macos_close_button,
        true);

    configure_macos_standard_window_button(
        window,
        macos_miniaturize_button,
        true);

    configure_macos_standard_window_button(
        window,
        macos_zoom_button,
        !macos_size_is_locked());
  }

  /*
   * Reapply geometry after changing NSWindow chrome.
   *
   * Changing styleMask can change the relationship between NSWindow's frame
   * and its content area.
   *
   * Applying the requested content dimensions again after the integrated
   * titlebar has been configured guarantees that frontend 1080 x 720 stays
   * exactly 1080 x 720.
   */
  void reapply_macos_geometry_after_chrome_change() {
    const auto window =
        macos_window();

    if (has_current_size) {
      set_macos_content_size(
          window,
          current_width,
          current_height);
    }

    if (has_minimum_size) {
      set_macos_minimum_content_size(
          window,
          minimum_width,
          minimum_height);
    }

    if (has_maximum_size) {
      set_macos_maximum_content_size(
          window,
          maximum_width,
          maximum_height);
    }

    /*
     * Chrome configuration normally occurs during startup.
     *
     * Centering here ensures that the final post-titlebar frame, rather than
     * an intermediate frame, is centered on screen.
     */
    if (has_current_size) {
      send_message<void>(
          window,
          "center");
    }
  }

#endif

#if defined(_WIN32)

  /*
   * The caption in the page (D-049).
   *
   * Windows draws no caption strip above the page. The window keeps its
   * standard style, so Snap, Win+arrows, Alt+Space and the taskbar treat it
   * as any other window, but its client area takes the caption's place
   * (WM_NCCALCSIZE in constrained_window_procedure). Through WebView2's
   * support for the CSS app-region, the page's header rows drag the window,
   * a double-click there maximizes it and a right-click opens its system
   * menu. The page draws minimize, maximize and close at the right of its
   * header, and they run the window's own commands (run_caption_command).
   * The side and bottom borders resize the window as before; an input-only
   * strip along the top edge, above the web view, takes the place of the
   * caption's top border.
   *
   * A WebView2 too old to let the page drag its window keeps the standard
   * caption (install_standard_caption), so the window can always be moved.
   */
  void install_integrated_frame(
      HWND window) {
    native_window = window;

    if (!let_page_drag_window()) {
      install_standard_caption();
      return;
    }

    caption_in_page = true;

    SetWindowPos(
        native_window,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED |
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOZORDER |
            SWP_NOACTIVATE);

    // The content keeps the size it was given before the caption left.
    drop_caption_allowance();

    install_top_edge();
  }

  // The caption is the page's: set once the window is framed for it.
  bool caption_in_page = false;

  /*
   * WebView2 lets the page's app-region areas drag the window. The setting
   * holds from the next navigation, and the first page has not loaded yet
   * (ApplicationMain installs the frame before it navigates).
   */
  bool let_page_drag_window() {
    auto controller = view.browser_controller();

    if (!controller.ok()) {
      return false;
    }

    auto* web_view_controller =
        static_cast<ICoreWebView2Controller*>(controller.value());

    Microsoft::WRL::ComPtr<ICoreWebView2> web_view;
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    Microsoft::WRL::ComPtr<ICoreWebView2Settings9> regions;

    return web_view_controller != nullptr &&
        SUCCEEDED(web_view_controller->get_CoreWebView2(&web_view)) &&
        web_view &&
        SUCCEEDED(web_view->get_Settings(&settings)) &&
        settings &&
        SUCCEEDED(settings.As(&regions)) &&
        SUCCEEDED(regions->put_IsNonClientRegionSupportEnabled(TRUE));
  }

  // How thick Windows makes a resizable window's border at its DPI: the
  // depth of the strip that resizes the window from its top edge.
  static int resize_border(HWND window) {
    const UINT dpi = GetDpiForWindow(window);

    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  }

  /*
   * The height Windows reserves above the content for its caption and top
   * border, as AdjustWindowRectExForDpi gives it. webview sizes the window
   * with it; with the caption in the page the window holds no such strip.
   */
  static int caption_allowance(HWND window) {
    RECT frame{0, 0, 0, 0};

    AdjustWindowRectExForDpi(
        &frame,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
        FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
        GetDpiForWindow(window));

    return -frame.top;
  }

  // Shrinks a window that webview sized for a standard caption to the content
  // it was asked for.
  void drop_caption_allowance() const {
    if (!caption_in_page ||
        native_window == nullptr ||
        IsZoomed(native_window) ||
        IsIconic(native_window)) {
      return;
    }

    RECT frame{};

    if (!GetWindowRect(native_window, &frame)) {
      return;
    }

    SetWindowPos(
        native_window,
        nullptr,
        0,
        0,
        frame.right - frame.left,
        frame.bottom - frame.top - caption_allowance(native_window),
        SWP_NOMOVE |
            SWP_NOZORDER |
            SWP_NOACTIVATE);
  }

  /*
   * Singularity opens in the middle of the screen it is on, above the
   * taskbar (the owner, 2026-10-31). The frame a person sees, without the
   * invisible borders Windows keeps around a window for resizing, is
   * centred in its monitor's work area. A window larger than that area keeps
   * its top left corner inside it, so its header stays in reach. A maximized
   * or minimized window stays where Windows put it.
   */
  void center_on_screen() {
    HWND window = native_window;

    if (window == nullptr) {
      auto from_view = view.window();
      if (!from_view.ok()) return;
      window = static_cast<HWND>(from_view.value());
    }

    if (window == nullptr || IsZoomed(window) || IsIconic(window)) {
      return;
    }

    RECT frame{};

    if (!GetWindowRect(window, &frame)) {
      return;
    }

    RECT seen = frame;

    if (FAILED(DwmGetWindowAttribute(
            window,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &seen,
            sizeof(seen)))) {
      seen = frame;
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);

    if (!GetMonitorInfoW(
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
            &monitor)) {
      return;
    }

    const RECT& area = monitor.rcWork;
    const LONG left = area.left +
        (std::max)(0L, (area.right - area.left - (seen.right - seen.left)) / 2);
    const LONG top = area.top +
        (std::max)(0L, (area.bottom - area.top - (seen.bottom - seen.top)) / 2);

    SetWindowPos(
        window,
        nullptr,
        left - (seen.left - frame.left),
        top - (seen.top - frame.top),
        0,
        0,
        SWP_NOSIZE |
            SWP_NOZORDER |
            SWP_NOACTIVATE);
  }

  /*
   * The client area of a window whose caption is the page's: what Windows
   * gives a framed window, less nothing at the top. A maximized window hangs
   * its borders past the monitor, so its top border is taken off; and since
   * it then covers the whole monitor, two rows of pixels are left wherever
   * an auto-hidden taskbar waits, or the taskbar could never show.
   */
  static void fit_client_to_page_caption(
      HWND window,
      RECT& client,
      LONG window_top) {
    client.top = window_top;

    if (!IsZoomed(window)) {
      return;
    }

    client.top += resize_border(window);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);

    if (!GetMonitorInfoW(
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
            &monitor)) {
      return;
    }

    // ABM_GETAUTOHIDEBAREX (Windows 8 and later).
    constexpr DWORD auto_hide_bar_on_monitor = 0x0000000b;
    constexpr LONG kept_for_taskbar = 2;

    const UINT edges[] = {ABE_TOP, ABE_BOTTOM, ABE_LEFT, ABE_RIGHT};

    for (const UINT edge : edges) {
      APPBARDATA bar{};
      bar.cbSize = sizeof(bar);
      bar.uEdge = edge;
      bar.rc = monitor.rcMonitor;

      if (SHAppBarMessage(auto_hide_bar_on_monitor, &bar) == 0) {
        continue;
      }

      if (edge == ABE_TOP) {
        client.top += kept_for_taskbar;
      } else if (edge == ABE_BOTTOM) {
        client.bottom -= kept_for_taskbar;
      } else if (edge == ABE_LEFT) {
        client.left += kept_for_taskbar;
      } else {
        client.right -= kept_for_taskbar;
      }
    }
  }

  /*
   * The top edge that resizes the window.
   *
   * The web view covers the whole client area, so the strip that resizes
   * the window from its top is a child window above it. It is input only: a
   * layered window with no surface of its own shows nothing, and the page
   * shows through it. It answers as the top border and its corners, hands
   * the press to the window, which resizes as from its own border, and is
   * hidden while the window is maximized.
   */
  static constexpr const wchar_t* top_edge_class = L"Singularity.TopEdge";

  HWND top_edge = nullptr;

  static LRESULT CALLBACK top_edge_procedure(
      HWND strip,
      UINT message,
      WPARAM wparam,
      LPARAM lparam) {
    if (message == WM_NCHITTEST) {
      RECT bounds{};
      GetWindowRect(strip, &bounds);

      const int x = GET_X_LPARAM(lparam);
      const int corner = 2 * resize_border(strip);

      if (x < bounds.left + corner) {
        return HTTOPLEFT;
      }

      if (x >= bounds.right - corner) {
        return HTTOPRIGHT;
      }

      return HTTOP;
    }

    if (message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK) {
      return SendMessageW(GetParent(strip), message, wparam, lparam);
    }

    return DefWindowProcW(strip, message, wparam, lparam);
  }

  void install_top_edge() {
    const auto instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW strip_class{};
    strip_class.cbSize = sizeof(strip_class);
    strip_class.style = CS_DBLCLKS;
    strip_class.lpfnWndProc = &top_edge_procedure;
    strip_class.hInstance = instance;
    strip_class.lpszClassName = top_edge_class;

    // A second window of the process finds the class already registered.
    RegisterClassExW(&strip_class);

    top_edge = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
        top_edge_class,
        L"",
        WS_CHILD,
        0,
        0,
        0,
        0,
        native_window,
        nullptr,
        instance,
        nullptr);

    // A layered window takes part in hit-testing once its opacity is set;
    // without a surface it still draws nothing.
    if (top_edge != nullptr) {
      SetLayeredWindowAttributes(top_edge, 0, 255, LWA_ALPHA);
    }

    place_top_edge();
  }

  void place_top_edge() const {
    if (top_edge == nullptr) {
      return;
    }

    RECT client{};

    if (IsZoomed(native_window) ||
        IsIconic(native_window) ||
        !GetClientRect(native_window, &client)) {
      ShowWindow(top_edge, SW_HIDE);
      return;
    }

    SetWindowPos(
        top_edge,
        HWND_TOP,
        0,
        0,
        client.right - client.left,
        resize_border(native_window),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }

  /*
   * The standard caption, kept where the page cannot carry it.
   *
   * Windows keeps its own caption, so its minimize, maximize and close
   * buttons, Snap Layouts, dragging, double-click and the system menu all
   * stay native. Like the GTK header bar on Linux it shows neither the
   * application title nor the icon, and it takes the page's white -
   * Windows 11 colours the caption, earlier versions keep their own.
   */
  void install_standard_caption() {
    WTA_OPTIONS caption_options{};
    caption_options.dwFlags =
        WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;
    caption_options.dwMask =
        WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;

    SetWindowThemeAttribute(
        native_window,
        WTA_NONCLIENT,
        &caption_options,
        sizeof(caption_options));

    // DWMWA_CAPTION_COLOR (Windows 11 SDK); --background in
    // code/frontend/styles/base.css.
    constexpr DWORD caption_color_attribute = 35;
    const COLORREF canvas = RGB(0xff, 0xff, 0xff);

    DwmSetWindowAttribute(
        native_window,
        caption_color_attribute,
        &canvas,
        sizeof(canvas));

    SetWindowPos(
        native_window,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED |
            SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOZORDER |
            SWP_NOACTIVATE);
  }

  HWND native_window{};

  // Set once WebView2's browser process is gone (Platform/WebViewRecovery):
  // nothing calls into the web view again, the person is told, and the window
  // closes in order.
  std::atomic<bool> web_view_lost{false};

  void on_web_view_lost() {
    if (web_view_lost.exchange(true)) return;
    HWND window = native_window;
    if (window == nullptr) {
      auto from_view = view.window();
      if (from_view.ok()) window = static_cast<HWND>(from_view.value());
    }
    MessageBoxW(window,
        L"Singularity's window stopped after a problem with this computer's graphics or its web view. "
        L"Your work is kept. Singularity will close now; open it again to continue.",
        L"Singularity", MB_OK | MB_ICONWARNING);
    if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
  }

  /*
   * Size constraints on Windows.
   *
   * webview hands a minimum or a maximum to Windows exactly as given, as the
   * whole window's size in physical pixels. AppWindow's sizes are the
   * content's logical pixels ("AppWindow sizing semantics" above), so the
   * window answers WM_GETMINMAXINFO itself: each limit is scaled by the
   * window's DPI and grown by its frame, measured again at every resize, so
   * it holds at any monitor's scale. Without a limit, Windows' own stands.
   *
   * The same procedure frames the window for the caption in the page
   * (WM_NCCALCSIZE), keeps the top edge's strip along the top as the window
   * changes size, and keeps the content's size when the window moves to a
   * monitor of another scale, where webview sizes it for a standard caption.
   */
  static constexpr const wchar_t* constrained_window_property = L"Singularity.AppWindow";

  WNDPROC previous_window_procedure = nullptr;

  POINT window_frame_for_content(HWND window, int width, int height) const {
    const UINT dpi = GetDpiForWindow(window);
    RECT frame{
        0,
        0,
        MulDiv(width, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
        MulDiv(height, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
    AdjustWindowRectExForDpi(
        &frame,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
        FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
        dpi);
    // With the caption in the page, nothing stands above the content.
    if (caption_in_page) frame.top = 0;
    return POINT{frame.right - frame.left, frame.bottom - frame.top};
  }

  void constrain(
      HWND window,
      MINMAXINFO& limits,
      const MINMAXINFO& system) const {
    limits.ptMinTrackSize =
        has_minimum_size && minimum_width > 0
            ? window_frame_for_content(window, minimum_width, minimum_height)
            : system.ptMinTrackSize;
    if (has_maximum_size && maximum_width > 0) {
      limits.ptMaxTrackSize = window_frame_for_content(window, maximum_width, maximum_height);
      limits.ptMaxSize = limits.ptMaxTrackSize;
    } else {
      limits.ptMaxTrackSize = system.ptMaxTrackSize;
      limits.ptMaxSize = system.ptMaxSize;
    }
  }

  static LRESULT CALLBACK constrained_window_procedure(
      HWND window,
      UINT message,
      WPARAM wparam,
      LPARAM lparam) {
    auto* self = static_cast<Impl*>(GetPropW(window, constrained_window_property));
    if (self == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    const auto previous = self->previous_window_procedure;
    if (message == WM_GETMINMAXINFO) {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      const MINMAXINFO system = *limits;
      const auto result = CallWindowProcW(previous, window, message, wparam, lparam);
      self->constrain(window, *limits, system);
      return result;
    }
    if (message == WM_NCCALCSIZE && self->caption_in_page) {
      // Windows works out the borders; the client area keeps the top.
      RECT& client = wparam == TRUE
          ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
          : *reinterpret_cast<RECT*>(lparam);
      const LONG window_top = client.top;
      CallWindowProcW(previous, window, message, wparam, lparam);
      fit_client_to_page_caption(window, client, window_top);
      return 0;
    }
    if (message == WM_SIZE && self->caption_in_page) {
      const auto result = CallWindowProcW(previous, window, message, wparam, lparam);
      self->place_top_edge();
      return result;
    }
    if (message == WM_DPICHANGED && self->caption_in_page) {
      const auto result = CallWindowProcW(previous, window, message, wparam, lparam);
      self->drop_caption_allowance();
      return result;
    }
    if (message == WM_ENDSESSION && wparam == TRUE && self->quit_requested) self->end_session(window);
    if (message == WM_NCDESTROY) {
      SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
      RemovePropW(window, constrained_window_property);
    }
    return CallWindowProcW(previous, window, message, wparam, lparam);
  }

  /*
   * The session ends: signing out, shutting down or restarting, or an
   * installer's Restart Manager closing the app. Windows may end the process
   * as soon as WM_ENDSESSION returns, so the quit runs now
   * (AppWindow::on_quit_request) and this waits for it, still answering the
   * window's messages, since the quit ends the process itself. Should Windows
   * show who it waits on, it says why.
   */
  std::function<void()> quit_requested;
  std::atomic<bool> quit_started{false};

  [[noreturn]] void end_session(HWND window) {
    ShutdownBlockReasonCreate(window, L"Singularity is recording where its work stands.");
    if (!quit_started.exchange(true)) quit_requested();
    for (;;) {
      MSG pending;
      while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&pending);
        DispatchMessageW(&pending);
      }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
  }

  void install_size_constraints(HWND window) {
    SetPropW(window, constrained_window_property, this);
    previous_window_procedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        window,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&constrained_window_procedure)));
  }

#endif

#if defined(__linux__)

  /*
   * Singularity opens in the middle of the screen it is on (the owner,
   * 2026-10-31). GTK centres a window as it shows it, once asked to
   * (use_integrated_titlebar shows it again at its final size); a window
   * already on screen is moved to the middle of its monitor's work area for
   * its new size. Where the display server places windows itself, as on
   * Wayland, a move is a request it may ignore.
   */
  void center_on_screen() {
    auto native_window = view.window();

    if (!native_window.ok() || native_window.value() == nullptr) {
      return;
    }

    auto* window = GTK_WINDOW(native_window.value());
    auto* widget = GTK_WIDGET(window);

    gtk_window_set_position(window, GTK_WIN_POS_CENTER);

    if (!has_current_size ||
        !gtk_widget_get_mapped(widget) ||
        gtk_window_is_maximized(window)) {
      return;
    }

    GdkDisplay* display = gtk_widget_get_display(widget);
    GdkWindow* surface = gtk_widget_get_window(widget);
    GdkMonitor* monitor = surface != nullptr
        ? gdk_display_get_monitor_at_window(display, surface)
        : nullptr;

    if (monitor == nullptr) {
      monitor = gdk_display_get_primary_monitor(display);
    }

    if (monitor == nullptr) {
      monitor = gdk_display_get_monitor(display, 0);
    }

    if (monitor == nullptr) {
      return;
    }

    GdkRectangle area{};
    gdk_monitor_get_workarea(monitor, &area);

    // The header bar stands above the content (use_integrated_titlebar).
    int height = current_height;
    GtkWidget* titlebar = gtk_window_get_titlebar(window);

    if (titlebar != nullptr && gtk_widget_get_visible(titlebar)) {
      height += gtk_widget_get_allocated_height(titlebar);
    }

    gtk_window_move(
        window,
        area.x + std::max(0, (area.width - current_width) / 2),
        area.y + std::max(0, (area.height - height) / 2));
  }

#endif
};


AppWindow::AppWindow(bool debug)
    : impl_(
          std::make_unique<Impl>(debug)) {
#if defined(__APPLE__)
  auto widget = impl_->view.widget();
  if (widget.ok()) platform::install_navigation_policy(widget.value());
  // Only the window's own page may reach the native bridge. A window whose
  // every frame could call every native method loads no page at all.
  if (widget.ok() && !platform::install_message_gate())
    throw std::runtime_error("The window's native bridge could not be guarded, so no page is loaded");
#elif defined(_WIN32)
  // The same policy and gate on WebView2 (WindowNavigationWindows.cpp): only
  // the window's own pages are shown, and only they reach the native bridge.
  // A window that cannot be guarded loads no page at all.
  auto controller = impl_->view.browser_controller();
  if (!controller.ok() || !platform::install_message_gate())
    throw std::runtime_error("The window's native bridge could not be guarded, so no page is loaded");
  platform::install_navigation_policy(controller.value());
  platform::install_web_view_recovery(controller.value(), [impl = impl_.get()] { impl->on_web_view_lost(); });
  // Files dragged onto the window reach the page by their paths, as on macOS.
  platform::install_file_drop(controller.value());
  auto window = impl_->view.window();
  window.ensure_ok();
  impl_->install_size_constraints(static_cast<HWND>(window.value()));
#elif defined(__linux__)
  // The same policy and gate on WebKitGTK (WindowNavigationLinux.cpp): only
  // the window's own pages are shown, and only they reach the native bridge.
  // The page's calls go to the gate through its script, added right after the
  // library's own. A window that cannot be guarded loads no page at all.
  auto widget = impl_->view.widget();
  if (!widget.ok() || !platform::install_message_gate(widget.value()))
    throw std::runtime_error("The window's native bridge could not be guarded, so no page is loaded");
  impl_->view.init(platform::message_gate_page_script());
  platform::install_navigation_policy(widget.value());
  // Files dragged onto the window reach the page with their paths (FileDropLinux.cpp).
  platform::install_file_drop(widget.value());
#endif
}

AppWindow::~AppWindow() = default;

void AppWindow::bind(
    const std::string& name,
    Binding binding) {
  impl_->view.bind(
      name,
      [binding = std::move(binding)](
          const std::string& request_id,
          const std::string& request,
          void*) {
        binding(
            request_id,
            request);
      },
      nullptr);
#if defined(__linux__)
  // The library's bind() removes every user script and adds back only its
  // own: the message gate's script goes back in too.
  auto widget = impl_->view.widget();
  if (!widget.ok() || !platform::install_message_gate(widget.value()))
    throw std::runtime_error("The window's native bridge could not be guarded, so no page is loaded");
#endif
}

void AppWindow::on_quit_request(
    std::function<void()> quit) {
#if defined(__APPLE__)
  platform::install_quit_handler(
      std::move(quit));
#elif defined(_WIN32)
  // Called from the window's procedure as the session ends (WM_ENDSESSION).
  impl_->quit_requested = std::move(quit);
#elif defined(__linux__)
  // The system asks the app to end by a signal: SIGTERM when the session ends
  // or a service manager stops it, SIGINT, SIGHUP. GLib delivers it on the
  // main loop, never inside the signal handler, and quit is called once.
  static std::function<void()> requested;
  requested = std::move(quit);
  for (const int signal_number : {SIGTERM, SIGINT, SIGHUP}) {
    g_unix_signal_add(signal_number, [](gpointer) -> gboolean {
      static std::once_flag once;
      std::call_once(once, [] {
        if (requested) requested();
      });
      return G_SOURCE_CONTINUE;
    }, nullptr);
  }
#else
  (void)quit;
#endif
}

void AppWindow::dispatch(
    std::function<void()> callback) {
  impl_->view.dispatch(
      std::move(callback));
}

void AppWindow::evaluate(
    const std::string& javascript) {
#if defined(_WIN32)
  // A web view whose browser process is gone is never called again.
  if (impl_->web_view_lost) return;
#endif
  impl_->view.eval(
      javascript);
}

void AppWindow::initialize(
    const std::string& javascript) {
  impl_->view.init(
      javascript);
}

void AppWindow::navigate(
    const std::string& url) {
#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
  platform::allow_window_prefix(platform::window_prefix_of(url, false));
#endif
  impl_->view.navigate(
      url);
}

void AppWindow::navigate_local_file(
    const std::string& file_url,
    const std::string& read_access_url) {
#if defined(__APPLE__)

  /*
   * Packaged frontend loading on macOS
   * ----------------------------------
   *
   * webview loads ordinary URLs through WKWebView.loadRequest.
   *
   * That can load the HTML file itself but does not grant the page automatic
   * access to adjacent packaged Vite JS and CSS assets.
   *
   * loadFileURL:allowingReadAccessToURL: grants read-only access to the
   * packaged frontend directory.
   */
  auto widget =
      impl_->view.widget();

  widget.ensure_ok();

  platform::allow_window_prefix(platform::window_prefix_of(read_access_url, true));

  const auto pool =
      send_message<ObjectiveCObject>(
          object_class("NSAutoreleasePool"),
          "new");

  send_message<void>(
      widget.value(),
      "loadFileURL:allowingReadAccessToURL:",
      ns_url(file_url),
      ns_url(read_access_url));

  send_message<void>(
      pool,
      "drain");

#else

#if defined(_WIN32) || defined(__linux__)
  // The packaged frontend's folder: the window's pages, and nothing beside it.
  platform::allow_window_prefix(platform::window_prefix_of(read_access_url, true));
#else
  (void)read_access_url;
#endif

  impl_->view.navigate(
      file_url);

#endif
}

void AppWindow::resolve(
    const std::string& request_id,
    int status,
    const std::string& json_result) {
#if defined(_WIN32)
  if (impl_->web_view_lost) return;
#endif
  impl_->view.resolve(
      request_id,
      status,
      json_result);
}

void AppWindow::run() {
  impl_->view.run();
}

/*
 * Current content size
 * --------------------
 *
 * The historical name is set_initial_size(), but the implementation is safe
 * to use again later.
 *
 * AppWindow::set_size(), declared in the header, delegates to this method for
 * runtime window changes.
 *
 *
 * Example onboarding -> workspace transition:
 *
 *   clear_size_constraints();
 *   set_minimum_size(900, 650);
 *   set_size(1440, 900);
 *
 * The old onboarding maximum must be removed before requesting a larger
 * current size.
 */
void AppWindow::set_initial_size(
    int width,
    int height) {
  validate_current_size(
      width,
      height);

  if (impl_->has_minimum_size &&
      impl_->minimum_width > 0 &&
      (width < impl_->minimum_width ||
       height < impl_->minimum_height)) {
    throw std::invalid_argument(
        "Requested window size is smaller than the active minimum size");
  }

  if (impl_->has_maximum_size &&
      impl_->maximum_width > 0 &&
      (width > impl_->maximum_width ||
       height > impl_->maximum_height)) {
    throw std::invalid_argument(
        "Requested window size is larger than the active maximum size");
  }

  impl_->has_current_size = true;
  impl_->current_width = width;
  impl_->current_height = height;

#if defined(__APPLE__)

  /*
   * Do not use:
   *
   *   webview::set_size(..., WEBVIEW_HINT_NONE)
   *
   * here.
   *
   * The Cocoa backend interprets NONE as NSWindow frame size while MIN/MAX
   * are content-size constraints.
   *
   * It can also rebuild styleMask and remove our integrated titlebar flags.
   */
  const auto native_window =
      impl_->macos_window();

  set_macos_content_size(
      native_window,
      width,
      height);

  // Singularity changes from its compact onboarding canvas to a wider workspace
  // exactly once. Re-centering both explicit application sizes keeps the
  // expanded window inside the visible display instead of growing off the
  // right and bottom edges from the onboarding window's old origin.
  send_message<void>(native_window, "center");

#else

  /*
   * Windows and GTK continue using their webview implementations for current
   * window sizing and platform-specific DPI conversion.
   */
  impl_->view.set_size(
      width,
      height,
      WEBVIEW_HINT_NONE);

#if defined(_WIN32)
  // webview sized the window for a standard caption.
  impl_->drop_caption_allowance();
#endif

  // Every size the application gives its window is centred on the screen,
  // the first one included: the window opens in the middle (the owner,
  // 2026-10-31), and a later, larger one stays inside the visible display
  // instead of growing off its right and bottom edges.
  impl_->center_on_screen();

#endif
}

/*
 * Minimum content size
 * --------------------
 *
 * Passing (0, 0) removes the explicit minimum constraint.
 *
 * During onboarding minimum == maximum can temporarily prevent resizing
 * while the underlying native window remains a resizable desktop window.
 */
void AppWindow::set_minimum_size(
    int width,
    int height) {
  validate_size_constraint(
      width,
      height,
      "Minimum");

  if (width > 0 &&
      impl_->has_maximum_size &&
      impl_->maximum_width > 0 &&
      (width > impl_->maximum_width ||
       height > impl_->maximum_height)) {
    throw std::invalid_argument(
        "Minimum window size cannot be larger than the active maximum size");
  }

  impl_->has_minimum_size = true;
  impl_->minimum_width = width;
  impl_->minimum_height = height;

#if defined(__APPLE__)

  const auto native_window =
      impl_->macos_window();

  set_macos_minimum_content_size(
      native_window,
      width,
      height);

  /*
   * Constraint changes may change whether Zoom should be available.
   *
   * Close and Minimize remain enabled regardless of resize constraints.
   */
  impl_->refresh_macos_window_controls();

#else

  impl_->view.set_size(
      width,
      height,
      WEBVIEW_HINT_MIN);

#endif
}

/*
 * Maximum content size
 * --------------------
 *
 * Passing (0, 0) removes the practical maximum.
 *
 *
 * Onboarding:
 *
 *   min = 1080 x 720
 *   max = 1080 x 720
 *
 * The window cannot be manually resized and macOS Zoom is disabled.
 *
 *
 * Main workspace:
 *
 *   min = 900 x 650
 *   max = unlimited
 *
 * The window can resize normally and macOS Zoom is automatically enabled.
 */
void AppWindow::set_maximum_size(
    int width,
    int height) {
  validate_size_constraint(
      width,
      height,
      "Maximum");

  if (width > 0 &&
      impl_->has_minimum_size &&
      impl_->minimum_width > 0 &&
      (width < impl_->minimum_width ||
       height < impl_->minimum_height)) {
    throw std::invalid_argument(
        "Maximum window size cannot be smaller than the active minimum size");
  }

  impl_->has_maximum_size = true;
  impl_->maximum_width = width;
  impl_->maximum_height = height;

#if defined(__APPLE__)

  const auto native_window =
      impl_->macos_window();

  set_macos_maximum_content_size(
      native_window,
      width,
      height);

  /*
   * If min == max, only Zoom becomes unavailable.
   *
   * Close and Minimize must continue to work during onboarding.
   */
  impl_->refresh_macos_window_controls();

#else

  /*
   * webview does not expose a dedicated "clear maximum size" API.
   *
   * On platforms using the webview implementation directly, (0, 0) maps to
   * a deliberately huge practical maximum.
   */
  const int native_width =
      normalized_maximum_dimension(width);

  const int native_height =
      normalized_maximum_dimension(height);

  impl_->view.set_size(
      native_width,
      native_height,
      WEBVIEW_HINT_MAX);

#endif
}

void AppWindow::set_title(
    const std::string& title) {
  impl_->view.set_title(
      title);
}

void AppWindow::set_titlebar_controls(
    std::vector<TitlebarControl> controls) {
#if defined(__APPLE__)

  /*
   * Only what can be pressed is kept: a rectangle with no area, or one that
   * is not a finite number, steps aside for nothing.
   */
  std::vector<TitlebarControl> accepted;
  accepted.reserve(controls.size());

  for (const auto& control : controls) {
    if (std::isfinite(control.x) &&
        std::isfinite(control.y) &&
        std::isfinite(control.width) &&
        std::isfinite(control.height) &&
        control.width > 0.0 &&
        control.height > 0.0) {
      accepted.push_back(control);
    }
  }

  titlebar_controls() = std::move(accepted);

#else

  (void)controls;

#endif
}

AppWindow::Frame AppWindow::frame() const {
  Frame state;

#if defined(_WIN32)

  state.caption_in_page = impl_->caption_in_page;
  state.maximized =
      impl_->caption_in_page &&
      impl_->native_window != nullptr &&
      IsZoomed(impl_->native_window) != FALSE;

#endif

  return state;
}

bool AppWindow::run_caption_command(
    CaptionCommand command) {
#if defined(_WIN32)

  const HWND window = impl_->native_window;

  if (window == nullptr || impl_->web_view_lost) {
    return false;
  }

  switch (command) {
  case CaptionCommand::Minimize:
    SendMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    return true;

  case CaptionCommand::Maximize: {
    // A window held to a largest size, or without a maximize box, is not
    // maximized from the page either.
    const bool can_maximize =
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_MAXIMIZEBOX) != 0 &&
        !(impl_->has_maximum_size && impl_->maximum_width > 0);

    if (!can_maximize || IsZoomed(window)) {
      return false;
    }

    SendMessageW(window, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    return true;
  }

  case CaptionCommand::Restore:
    if (!IsZoomed(window)) {
      return false;
    }

    SendMessageW(window, WM_SYSCOMMAND, SC_RESTORE, 0);
    return true;

  case CaptionCommand::Close:
    // Posted: the window closes, as from its own close button, after the
    // page has its answer.
    PostMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
    return true;
  }

  return false;

#else

  // macOS and Linux keep their native window controls; the page draws none.
  (void)command;
  return false;

#endif
}

void AppWindow::use_application_icon() {
#if defined(__APPLE__)

  /*
   * Finder, Dock and the application switcher obtain the application icon
   * from the macOS application bundle.
   *
   * No runtime icon configuration is required.
   */

#elif defined(_WIN32)

#if NATIVE_APP_HAS_ICON

  auto native_window =
      impl_->view.window();

  native_window.ensure_ok();

  const auto instance =
      GetModuleHandleW(nullptr);

  const auto large_icon =
      reinterpret_cast<HICON>(
          LoadImageW(
              instance,
              MAKEINTRESOURCEW(101),
              IMAGE_ICON,
              GetSystemMetrics(SM_CXICON),
              GetSystemMetrics(SM_CYICON),
              LR_DEFAULTCOLOR));

  const auto small_icon =
      reinterpret_cast<HICON>(
          LoadImageW(
              instance,
              MAKEINTRESOURCEW(101),
              IMAGE_ICON,
              GetSystemMetrics(SM_CXSMICON),
              GetSystemMetrics(SM_CYSMICON),
              LR_DEFAULTCOLOR));

  if (large_icon != nullptr) {
    SendMessageW(
        static_cast<HWND>(
            native_window.value()),
        WM_SETICON,
        ICON_BIG,
        reinterpret_cast<LPARAM>(
            large_icon));
  }

  if (small_icon != nullptr) {
    SendMessageW(
        static_cast<HWND>(
            native_window.value()),
        WM_SETICON,
        ICON_SMALL,
        reinterpret_cast<LPARAM>(
            small_icon));
  }

#endif

#elif defined(__linux__)

#if NATIVE_APP_HAS_ICON

  auto native_window =
      impl_->view.window();

  native_window.ensure_ok();

  const auto icon_path =
      platform::executable_path()
          .parent_path() /
      "app-icon.png";

  GError* error = nullptr;

  if (!gtk_window_set_icon_from_file(
          GTK_WINDOW(
              native_window.value()),
          icon_path.string().c_str(),
          &error)) {
    if (error != nullptr) {
      g_error_free(error);
    }

    gtk_window_set_icon_name(
        GTK_WINDOW(
            native_window.value()),
        NATIVE_APP_SLUG);
  }

#endif

#endif
}

platform::ParentWindow AppWindow::dialog_parent() {
#if defined(_WIN32) || defined(__APPLE__)
  auto native_window = impl_->view.window();
  if (!native_window.ok() || native_window.value() == nullptr) return {};
#if defined(_WIN32)
  return {platform::ParentWindow::System::windows, native_window.value()};
#else
  return {platform::ParentWindow::System::cocoa, native_window.value()};
#endif
#else
  return {};
#endif
}

void AppWindow::use_integrated_titlebar() {
  /*
   * Native chrome installation is one-time initialization.
   *
   * Without this guard, repeated calls could install duplicate drag regions
   * or duplicate platform-specific frame handlers.
   */
  if (impl_->integrated_titlebar_installed) {
    return;
  }

#if defined(__APPLE__)

  /*
   * NSWindowStyleMask values
   * ------------------------
   *
   * These values are defined by AppKit:
   *
   *   Titled              = 1 << 0
   *   Closable            = 1 << 1
   *   Miniaturizable      = 1 << 2
   *   Resizable           = 1 << 3
   *   FullSizeContentView = 1 << 15
   *
   * All normal desktop-window capabilities are explicitly preserved here.
   *
   * In particular, equal min/max dimensions must never remove Close or
   * Minimize.
   */
  constexpr unsigned long titled =
      1UL << 0U;

  constexpr unsigned long closable =
      1UL << 1U;

  constexpr unsigned long miniaturizable =
      1UL << 2U;

  constexpr unsigned long resizable =
      1UL << 3U;

  constexpr unsigned long full_size_content_view =
      1UL << 15U;

  constexpr long hidden_title = 1;
  constexpr long no_titlebar_separator = 0;

  auto native_window =
      impl_->view.window();

  native_window.ensure_ok();

  const auto existing_style_mask =
      send_message<unsigned long>(
          native_window.value(),
          "styleMask");

  /*
   * Preserve whatever flags the native window already has, then explicitly
   * guarantee the capabilities Singularity requires.
   *
   * Resizable remains part of styleMask even during onboarding.
   *
   * The onboarding min/max constraints temporarily prevent resizing. Once
   * those constraints are removed, resizing therefore works immediately
   * without another styleMask mutation.
   */
  const auto required_style_mask =
      existing_style_mask |
      titled |
      closable |
      miniaturizable |
      resizable |
      full_size_content_view;

  send_message<void>(
      native_window.value(),
      "setStyleMask:",
      required_style_mask);

  /*
   * Keep the native titlebar machinery and traffic lights while allowing
   * frontend content to render through the titlebar area.
   */
  send_message<void>(
      native_window.value(),
      "setTitleVisibility:",
      hidden_title);

  send_message<void>(
      native_window.value(),
      "setTitlebarAppearsTransparent:",
      true);

  send_message<void>(
      native_window.value(),
      "setTitlebarSeparatorStyle:",
      no_titlebar_separator);

  send_message<void>(
      native_window.value(),
      "setMovableByWindowBackground:",
      true);

  auto widget =
      impl_->view.widget();

  widget.ensure_ok();

  /*
   * Add the transparent native drag region above the frontend.
   */
  install_drag_region(
      widget.value());

  /*
   * Additional Singularity-specific macOS chrome configuration may reposition
   * the traffic lights and configure the WKWebView/titlebar relationship.
   */
  platform::configure_macos_chrome(
      native_window.value(),
      widget.value(),
      NATIVE_APP_DISPLAY_NAME);

  /*
   * configure_macos_chrome() is allowed to change visual placement but
   * AppWindow owns the behavioral state of the traffic lights.
   *
   * Refresh them after platform chrome configuration so visual customization
   * cannot accidentally leave Close or Minimize disabled.
   */
  impl_->refresh_macos_window_controls();

  /*
   * Reapply the requested geometry after every style/titlebar mutation.
   *
   * This prevents the titlebar from changing the CSS-visible dimensions and
   * fixes the original "slightly smaller than 1080 x 720" startup issue.
   */
  impl_->reapply_macos_geometry_after_chrome_change();

  /*
   * AppKit may reconsider standard button state when window constraints are
   * changed.
   *
   * Run the synchronization once more after geometry has been restored.
   *
   * Final onboarding state:
   *
   *   Close      enabled
   *   Minimize   enabled
   *   Zoom       disabled
   *   Resize     blocked by min == max
   */
  impl_->refresh_macos_window_controls();

#elif defined(_WIN32)

  auto native_window =
      impl_->view.window();

  native_window.ensure_ok();

  impl_->install_integrated_frame(
      static_cast<HWND>(
          native_window.value()));

  // The frame lost its caption's height: centred again at its final size.
  impl_->center_on_screen();

#elif defined(__linux__)

  /*
   * GTK client-side decorations retain the current desktop environment's
   * native controls and drag behavior without displaying the normal
   * application title.
   */
  auto native_window =
      impl_->view.window();

  native_window.ensure_ok();

  auto* window =
      GTK_WINDOW(
          native_window.value());

  auto* widget =
      GTK_WIDGET(window);

  /*
   * Hide the window while replacing its titlebar so intermediate GTK layout
   * states are not shown to the user.
   */
  gtk_widget_hide(
      widget);

  auto* header =
      GTK_HEADER_BAR(
          gtk_header_bar_new());

  gtk_header_bar_set_title(
      header,
      "");

  gtk_header_bar_set_has_subtitle(
      header,
      FALSE);

  gtk_header_bar_set_show_close_button(
      header,
      TRUE);

  gtk_style_context_add_class(
      gtk_widget_get_style_context(
          GTK_WIDGET(header)),
      GTK_STYLE_CLASS_FLAT);

  gtk_window_set_titlebar(
      window,
      GTK_WIDGET(header));

  // Shown again, the window is placed again: in the middle of the screen,
  // at its final size with the header bar.
  gtk_window_set_position(
      window,
      GTK_WIN_POS_CENTER);

  gtk_widget_show_all(
      widget);

#endif

  impl_->integrated_titlebar_installed = true;
}

} // namespace singularity::app
