#include "Platform/WindowNavigation/WindowNavigation.hpp"

#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace singularity::platform {
namespace {

using ObjectiveCObject = void *;

template <typename Return, typename... Args>
Return send_message(ObjectiveCObject receiver, const char *selector, Args... args) {
  const auto function = reinterpret_cast<Return (*)(ObjectiveCObject, SEL, Args...)>(objc_msgSend);
  return function(receiver, sel_registerName(selector), args...);
}

ObjectiveCObject object_class(const char *name) {
  return reinterpret_cast<ObjectiveCObject>(objc_getClass(name));
}


// The prefixes the window was loaded from; one window per process.
std::mutex &window_prefix_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<std::string> &window_prefixes() {
  static std::vector<std::string> prefixes;
  return prefixes;
}

std::vector<std::string> remembered_prefixes() {
  const std::lock_guard<std::mutex> lock(window_prefix_mutex());
  return window_prefixes();
}

} // namespace

void allow_window_prefix(const std::string &prefix) {
  if (prefix.empty()) return;
  const std::lock_guard<std::mutex> lock(window_prefix_mutex());
  auto &prefixes = window_prefixes();
  if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) prefixes.push_back(prefix);
}

namespace {

std::string utf8_of(ObjectiveCObject string) {
  if (string == nullptr) return {};
  const char *bytes = send_message<const char *>(string, "UTF8String");
  return bytes == nullptr ? std::string() : std::string(bytes);
}

} // namespace

// WKNavigationDelegate: every navigation of the window is decided here. The
// window's own pages are shown; nothing else is - WebKit shows a dropped link or
// a link a rendered document opens by navigating the window, and that page
// would get every native method the window's scripts can call.
void install_navigation_policy(ObjectiveCObject web_view) {
  constexpr const char *class_name = "SingularityNavigationDelegate";
  Class delegate_class = objc_lookUpClass(class_name);
  if (delegate_class == nullptr) {
    delegate_class = objc_allocateClassPair(objc_getClass("NSObject"), class_name, 0);
    class_addProtocol(delegate_class, objc_getProtocol("WKNavigationDelegate"));
    class_addMethod(delegate_class, sel_registerName("webView:decidePolicyForNavigationAction:decisionHandler:"),
        reinterpret_cast<IMP>(+[](ObjectiveCObject, SEL, ObjectiveCObject, ObjectiveCObject action,
            ObjectiveCObject decision_handler) {
          const ObjectiveCObject request = send_message<ObjectiveCObject>(action, "request");
          const ObjectiveCObject address = request == nullptr ? nullptr : send_message<ObjectiveCObject>(request, "URL");
          const std::string url =
              address == nullptr ? std::string() : utf8_of(send_message<ObjectiveCObject>(address, "absoluteString"));
          const ObjectiveCObject frame = send_message<ObjectiveCObject>(action, "targetFrame");
          const bool main_frame = frame == nullptr || send_message<BOOL>(frame, "isMainFrame");
          // WebKit asks again for every server redirect, with where it leads.
          long long policy = window_may_show(url, remembered_prefixes(), main_frame) ? 1 : 0;  // WKNavigationActionPolicy
          const ObjectiveCObject signature = send_message<ObjectiveCObject>(
              object_class("NSMethodSignature"), "signatureWithObjCTypes:", "v@?q");
          const ObjectiveCObject invocation = send_message<ObjectiveCObject>(
              object_class("NSInvocation"), "invocationWithMethodSignature:", signature);
          send_message<void>(invocation, "setTarget:", decision_handler);
          send_message<void>(invocation, "setArgument:atIndex:", static_cast<void *>(&policy), 1L);
          send_message<void>(invocation, "invoke");
        }),
        "v@:@@@?");
    objc_registerClassPair(delegate_class);
  }
  // The web view holds its delegate weakly: this one lives with the process.
  static ObjectiveCObject delegate = send_message<ObjectiveCObject>(
      reinterpret_cast<ObjectiveCObject>(delegate_class), "new");
  send_message<void>(web_view, "setNavigationDelegate:", delegate);
}

namespace {

// The webview library's class and method that receive every message posted to
// window.webkit.messageHandlers.__webview__ and call the bound native method it
// names (webview 0.12, cocoa_wkwebview_engine::create_script_message_handler).
constexpr const char *library_handler_class = "WebviewWKScriptMessageHandler";
constexpr const char *library_handler_selector = "userContentController:didReceiveScriptMessage:";
// The method a library answering with replies would receive messages through.
constexpr const char *library_reply_selector = "userContentController:didReceiveScriptMessage:replyHandler:";

using MessageHandler = void (*)(ObjectiveCObject, SEL, ObjectiveCObject, ObjectiveCObject);

// The library's own implementation, which the gate passes allowed messages on to.
MessageHandler library_message_handler = nullptr;

// Whether an Objective-C type encoding is void (id, SEL, id, id), with or
// without the frame offsets a compiler writes into it.
bool takes_two_objects(const char *encoding) {
  std::string types;
  for (const char *character = encoding; character != nullptr && *character != '\0'; ++character)
    if (!std::isdigit(static_cast<unsigned char>(*character))) types.push_back(*character);
  return types == "v@:@@";
}

// Said once a process, and never with what the message carried: a page posting
// in a loop must not fill the log.
void report_refused_message(const std::string &reason) {
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) return;
  std::cerr << "Singularity refused a message to its native bridge " << reason
            << ". Later refusals are not reported.\n";
}

// -[WebviewWKScriptMessageHandler userContentController:didReceiveScriptMessage:]
// with the gate in front: the library's own implementation runs only for text
// the window's own page posts from its main frame. Anything else is dropped
// before the library reads it - it would call -UTF8String on any body, and a
// body that is not text (postMessage({})) does not answer that.
void gated_message(ObjectiveCObject handler, SEL selector, ObjectiveCObject controller, ObjectiveCObject message) {
  const ObjectiveCObject frame = message == nullptr ? nullptr : send_message<ObjectiveCObject>(message, "frameInfo");
  const ObjectiveCObject origin = frame == nullptr ? nullptr : send_message<ObjectiveCObject>(frame, "securityOrigin");
  const ObjectiveCObject request = frame == nullptr ? nullptr : send_message<ObjectiveCObject>(frame, "request");
  const ObjectiveCObject address = request == nullptr ? nullptr : send_message<ObjectiveCObject>(request, "URL");
  if (origin == nullptr || address == nullptr) {
    report_refused_message("from a frame WebKit did not describe");
    return;
  }
  const bool main_frame = send_message<BOOL>(frame, "isMainFrame");
  const std::string url = utf8_of(send_message<ObjectiveCObject>(address, "absoluteString"));
  const std::string protocol = utf8_of(send_message<ObjectiveCObject>(origin, "protocol"));
  const std::string host = utf8_of(send_message<ObjectiveCObject>(origin, "host"));
  const long port = send_message<long>(origin, "port");  // NSInteger
  if (!message_may_reach_native(main_frame, url, protocol, host, static_cast<int>(port), remembered_prefixes())) {
    if (main_frame)
      report_refused_message("from a page that is not the window's own (origin " + protocol + "://" + host + ":"
          + std::to_string(port) + ")");
    else
      report_refused_message("from a frame inside the window's page");
    return;
  }
  const ObjectiveCObject body = send_message<ObjectiveCObject>(message, "body");
  if (body == nullptr || !send_message<BOOL>(body, "isKindOfClass:", object_class("NSString"))) {
    report_refused_message("that was not text");
    return;
  }
  if (library_message_handler != nullptr) library_message_handler(handler, selector, controller, message);
}

} // namespace

bool install_message_gate() {
  static std::mutex mutex;
  const std::lock_guard<std::mutex> lock(mutex);
  const Class handler_class = objc_lookUpClass(library_handler_class);
  const Method method = handler_class == nullptr
      ? nullptr
      : class_getInstanceMethod(handler_class, sel_registerName(library_handler_selector));
  // The gate replaces one method. A library that renamed it, changed what it
  // takes, or receives messages through a second method as well would leave
  // every frame free to call every native method: say so, and let the window
  // refuse to load a page, rather than run unguarded.
  if (method == nullptr || !takes_two_objects(method_getTypeEncoding(method))
      || class_getInstanceMethod(handler_class, sel_registerName(library_reply_selector)) != nullptr) {
    std::cerr << "Singularity cannot guard its native bridge: the webview library no longer receives page "
                 "messages through -["
              << library_handler_class << " " << library_handler_selector
              << "] alone. Review Platform/WindowNavigation before using this webview library.\n";
    return false;
  }
  const IMP gate = reinterpret_cast<IMP>(&gated_message);
  if (method_getImplementation(method) == gate) return true;
  library_message_handler = reinterpret_cast<MessageHandler>(method_getImplementation(method));
  method_setImplementation(method, gate);
  return true;
}

} // namespace singularity::platform
