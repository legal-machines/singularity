#pragma once

#include <functional>
#include <memory>
#include <string>

namespace singularity::app {
class AppWindow;
}

namespace singularity::bridge {

class NativeBridge final {
public:
	explicit NativeBridge(app::AppWindow &window, bool start_background_services = true);
  ~NativeBridge();

  NativeBridge(const NativeBridge &) = delete;
  NativeBridge &operator=(const NativeBridge &) = delete;

  void emit(const std::string &event_name, const std::string &json_payload);
  void install();

	// Invokes the exact registered worker handler used by window.__nativeInvoke
	// and returns its JSON result. This is the machine-usable native contract
	// entry used by packaged diagnostics/acceptance; UI-thread methods remain
	// available only through the live app window.
	std::string invoke_worker(const std::string &method,
	                          const std::string &json_params);

	// A call that arrives by another carrier than the window: Singularity's
	// Office connection (Office/OfficeConnection, docs/NATIVE_TRANSPORT.md).
	// The same handler table, the same pools and the same error envelope as a
	// window call (TRN-4); a method that needs the window is refused (TRN-3).
	// Never throws.
	struct CarriedAnswer {
		// 0 resolved, 1 rejected, as the window resolves a call.
		int status = 0;
		std::string json;
	};
	CarriedAnswer answer_carried_call(const std::string &method, const std::string &json_params);

	// Every event the window receives, for another carrier too; the handle
	// removes it. A listener is called on the thread that published the event
	// and must return at once.
	int add_event_listener(std::function<void(const std::string &name, const std::string &json_payload)> listener);
	void remove_event_listener(int handle);

	// The window's own page script (window.nativeApp), for a page another
	// carrier serves.
	std::string page_script() const;

	// Singularity is closing. It says so first (Platform/AppLifecycle), so
	// nothing new begins anywhere. Every operation still running is invalidated
	// and recorded as interrupted (so a later launch finds the truth, and
	// anything waiting for an approval that will never come stops waiting),
	// work an AI colleague was doing is kept to be begun again, queued work
	// nobody is waiting for is dropped, the workers are joined, and last every
	// folder watch ends. Safe to call more than once, from any thread: a later
	// call returns at once. Never throws.
	void shutdown() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace singularity::bridge
