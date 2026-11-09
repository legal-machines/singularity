#include "Platform/AppWindow/AppWindow.hpp"
#include "Platform/Types/WindowLayout.hpp"
#include "Bridge/NativeBridge/NativeBridge.hpp"
#include "Bridge/NativeBridge/NativeError.hpp"
#include "Node/AppUpdate/AppUpdate.hpp"
#include "Office/OfficeService/OfficeService.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(__APPLE__)
#include "Platform/MacOSChrome/MacOSChrome.hpp"
#endif

namespace {

// Development frontend URLs are restricted to localhost by default.
// A remote development server may be explicitly enabled with:
// NATIVE_APP_ALLOW_REMOTE_DEV_SERVER=1
bool is_allowed_development_url(const std::string &url) {
  if (url.rfind("http://127.0.0.1:", 0) == 0 ||
      url.rfind("http://localhost:", 0) == 0) {
    return true;
  }

  const auto *allow_remote =
      std::getenv("NATIVE_APP_ALLOW_REMOTE_DEV_SERVER");

  return allow_remote != nullptr &&
         std::string(allow_remote) == "1";
}

// Whether the window opens on onboarding: until onboarding is complete, as
// Settings record it. The page asks onboarding.getStatus, which reads the
// same record, so the window's size and the page always agree. In a
// development build NATIVE_APP_ONBOARDING=1 begins onboarding afresh and
// NATIVE_APP_ONBOARDING=0 marks it complete, by that same record.
bool opens_on_onboarding() {
  try {
    const singularity::persistence::PersistenceLock lock(
        singularity::persistence::persistence_mutex());
    auto settings = singularity::platform::load_app_settings();
#if !defined(NDEBUG)
    if (const char *flag = std::getenv("NATIVE_APP_ONBOARDING")) {
      const std::string value(flag);
      if ((value == "1" || value == "0") &&
          settings.onboarding_completed != (value == "0")) {
        settings.onboarding_completed = value == "0";
        singularity::platform::save_app_settings(settings);
      }
    }
#endif
    return !settings.onboarding_completed;
  } catch (const std::exception &error) {
    // Settings that cannot be read never keep a member out: the window opens
    // at its main size, and the page, whose own read fails too, shows the app.
    std::cerr << NATIVE_APP_DISPLAY_NAME
              << ": onboarding state could not be read: " << error.what()
              << '\n';
    return false;
  }
}

// How long Singularity waits, as it closes, for its work to record where it
// stood.
constexpr std::chrono::seconds kClosingPatience{5};

// Singularity ends here, whichever way it was asked to: its window closed, or
// on macOS the Quit item, the Dock, an AppleScript quit or logging out.
// shutdown records what was running and joins the workers, for
// kClosingPatience at most, and the process then ends without static
// destructors: a thread still finishing - a local generation, a document being
// read - never finds its objects destroyed. The local runtime ends with this
// process (Platform/ChildProcess). A version of Singularity readied to install
// is handed over last, once the work has stopped. A second caller waits here
// until the first ends it.
[[noreturn]] void end_application(singularity::bridge::NativeBridge &bridge) {
  static std::once_flag once;
  std::call_once(once, [&bridge] {
    auto finished = std::make_shared<std::promise<void>>();
    std::future<void> shutDown = finished->get_future();
    std::thread([&bridge, finished] {
      // Word, Excel and Outlook stop reaching it first.
      singularity::office::stop_office();
      bridge.shutdown();
      finished->set_value();
    }).detach();
    (void)shutDown.wait_for(kClosingPatience);
    singularity::node::update::finish_on_exit();
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    std::_Exit(0);
  });
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

int run_application() {
#if defined(NDEBUG)
  // Disable WebView developer tools in release builds.
  constexpr bool enable_webview_debug = false;
#else
  // Keep WebView developer tools available in debug builds.
  constexpr bool enable_webview_debug = true;
#endif

  singularity::app::AppWindow window(enable_webview_debug);

  // Set the native application window title.
  window.set_title(NATIVE_APP_DISPLAY_NAME);

  /*
   * Onboarding's fixed 1080 by 720 window until onboarding is complete, the
   * main geometry after it. onboarding.complete moves the running window
   * from the one to the other (Bridge/NativeBridge).
   */
  const bool onboarding_window = opens_on_onboarding();
  if (onboarding_window) {
    constexpr auto size = singularity::app::kOnboardingWindowSize;
    window.set_initial_size(size.width, size.height);
    window.set_minimum_size(size.width, size.height);
    window.set_maximum_size(size.width, size.height);
  } else {
    window.set_initial_size(singularity::app::kMainWindowInitialSize.width,
                            singularity::app::kMainWindowInitialSize.height);
    window.set_minimum_size(singularity::app::kMainWindowMinimumSize.width,
                            singularity::app::kMainWindowMinimumSize.height);
  }

  /*
   * Configure the platform-specific integrated titlebar.
   *
   * On macOS this keeps the native traffic lights while allowing the
   * WebView content to extend underneath the transparent titlebar.
   *
   * This is intentionally called after the size constraints have been set.
   */
  window.use_integrated_titlebar();

  // Apply the packaged application icon where the platform requires it.
  window.use_application_icon();

  /*
   * Install the native bridge before loading the frontend so that native
   * bindings are available as soon as the application UI starts running.
   */
  singularity::bridge::NativeBridge bridge(window);
  bridge.install();

  // Singularity in Word, Excel and Outlook: the Office connection carries the
  // same Native API as the window, and serves the panes again when the person
  // connected Office on this computer.
  {
    singularity::office::Carrier carrier;
    carrier.call = [&bridge](const std::string &method, const std::string &params) {
      const auto answer = bridge.answer_carried_call(method, params);
      return std::make_pair(answer.status, answer.json);
    };
    carrier.subscribe = [&bridge](std::function<void(const std::string &, const std::string &)> listener) {
      return bridge.add_event_listener(std::move(listener));
    };
    carrier.unsubscribe = [&bridge](int handle) { bridge.remove_event_listener(handle); };
    carrier.page_script = bridge.page_script();
    singularity::office::install_carrier(std::move(carrier));
    std::thread([] { singularity::office::start_office_if_connected(); }).detach();
  }

  /*
   * Development mode:
   *
   * If NATIVE_APP_DEV_SERVER_URL is present, load the frontend from the
   * local development server, normally Vite.
   */
  // Development builds only: a release build always loads its packaged
  // frontend, never a page from a development server environment variable,
  // because that page receives the full native bridge.
#if defined(NDEBUG)
  const char *development_url = nullptr;
#else
  const char *development_url = std::getenv("NATIVE_APP_DEV_SERVER_URL");
#endif
  if (development_url != nullptr) {
    const std::string url(development_url);

    if (!is_allowed_development_url(url)) {
      throw std::runtime_error(
          "Remote dev server is blocked. Use loopback or explicitly set "
          "NATIVE_APP_ALLOW_REMOTE_DEV_SERVER=1");
    }

    std::cout << NATIVE_APP_DISPLAY_NAME
              << " frontend: "
              << url
              << '\n';

    window.navigate(url);
  } else {
    /*
     * Production mode:
     *
     * Load the packaged Vite frontend from frontend-dist.
     */
    const auto entry =
        singularity::platform::frontend_entry_path();

    if (!std::filesystem::is_regular_file(entry)) {
      throw std::runtime_error(
          "frontend-dist/index.html was not found: " +
          entry.string());
    }

    const auto url =
        singularity::platform::file_url(entry);

    std::cout << NATIVE_APP_DISPLAY_NAME
              << " frontend: "
              << url
              << '\n';

    /*
     * On macOS WKWebView needs explicit read access to the directory
     * containing the packaged HTML, JS, CSS and other frontend assets.
     *
     * Other platforms fall back to normal navigation inside AppWindow.
     */
    window.navigate_local_file(
        url,
        singularity::platform::file_url(entry.parent_path()));
  }

  // A quit the system asks of the application ends it the same way as
  // closing the window, on macOS, Windows and Linux: on macOS Cmd+Q, the Dock,
  // AppleScript or logging out, while AppKit waits with the window still live;
  // on Windows logging off, shutting down or restarting; on Linux SIGTERM,
  // SIGINT or SIGHUP, the session ending among them.
  window.on_quit_request([&bridge] {
    std::thread([&bridge] { end_application(bridge); }).detach();
  });

  // Enter the native application event loop.
  window.run();

  // The window is gone: nothing may keep running invisibly. Operations still
  // in flight are recorded as interrupted and queued work is dropped before
  // this process exits. On macOS the closed window leaves the screen first:
  // the wait below holds the main thread, which would otherwise keep it shown
  // until the process ends.
#if defined(__APPLE__)
  singularity::platform::hide_application_windows();
#endif
  end_application(bridge);
}

#if !defined(NDEBUG)
// Development and acceptance tools only (deploy/frontend/native-dev-host.mjs,
// deploy/scripts/native_contract_parity_acceptance.py): one bridge method, run
// in this second process. A release build has none of it.
int run_native_call(const std::string &method, const std::string &params) {
	try {
		singularity::app::AppWindow window(false);
		singularity::bridge::NativeBridge bridge(window, false);
		std::cout << bridge.invoke_worker(method, params) << '\n';
		return 0;
	} catch (...) {
		// The same NativeError envelope the window's bridge rejects with.
		std::cerr << singularity::bridge::nativeErrorJson(singularity::bridge::nativeErrorOf(std::current_exception())).dump()
		          << '\n';
		return 2;
	}
}
#endif

} // namespace

#if defined(_WIN32)

// Windows GUI entry point.
// Using WinMain prevents an additional console window from appearing.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else

// Standard entry point for macOS and Linux.
int main(int argc, char **argv) {
#endif
  try {
#if !defined(_WIN32)
	if (argc == 4 && std::string(argv[1]) == "--native-call") {
#if defined(NDEBUG)
		// A release build runs no bridge method for another process: only the
		// window's own page reaches the bridge, so nothing outside it - an AI
		// app with a shell among them - can approve a draft, change a Matter's
		// mode or remove its data.
		std::cerr << NATIVE_APP_DISPLAY_NAME << ": this build does not run bridge methods from the command line.\n";
		return 2;
#else
		return run_native_call(argv[2], argv[3]);
#endif
	}
#endif
    return run_application();
  } catch (const std::exception &error) {
    std::cerr
        << NATIVE_APP_DISPLAY_NAME
        << ": "
        << error.what()
        << '\n';

    return 1;
  }
}
