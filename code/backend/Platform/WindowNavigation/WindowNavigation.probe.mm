// window_navigation_webkit_probe [--no-gate] [--no-policy] [--round file|dev]
//
// Real WebKit, no window: WKWebViews that are in no window load a test page the
// way the app loads its frontend - from a temporary file:// folder (the packaged
// frontend, round "file") or from http://127.0.0.1:<port>/ (the development
// server, round "dev"). A small HTTP server in this process answers on two
// loopback ports: the window's own and another origin.
//
// The page, and every frame and page it reaches, posts to
// window.webkit.messageHandlers.__webview__: the main frame (text and a
// {x:1} body), an about:srcdoc, data:, blob:, sandboxed, same-folder and
// cross-origin iframe, and main-frame navigations to data:, blob:, another
// origin, a file outside the folder and a 302 redirect to another origin - and,
// as controls, to the window's own pages. The handler under "__webview__" is a
// stand-in registered under the webview library's own class name and selector
// (WebviewWKScriptMessageHandler, userContentController:didReceiveScriptMessage:)
// that, like the library's, receives every message and reads its body with
// -UTF8String. A second handler, which no gate guards, witnesses which scripts
// ran, in which frame and with which origin.
//
// By default the message gate (install_message_gate) and the navigation policy
// (install_navigation_policy) are installed, as in the app: only the window's
// own main-frame text may reach the stand-in. --no-gate shows which callers
// reached it before the gate; --no-policy shows the gate alone. Without --round,
// both rounds run, each in its own process, since the window's prefixes last a
// process as in the app. Prints a PASS/FAIL row per caller; exits non-zero on
// any failure. Nothing is shown on screen and nothing leaves 127.0.0.1.

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "Platform/WindowNavigation/WindowNavigation.hpp"

#include <arpa/inet.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <objc/runtime.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

namespace platform = singularity::platform;

constexpr const char *library_handler_class = "WebviewWKScriptMessageHandler";

// A message that reached the stand-in for the library's handler.
struct Arrival {
  std::string label;
  std::string raised;  // the exception the library's -UTF8String call raised, if any
};

// What a frame's script told the witness, which no gate guards: that it ran,
// and where.
struct Witness {
  std::string label;
  bool main_frame = false;
  std::string protocol;
  std::string host;
  long port = 0;
  std::string url;
};

std::vector<Arrival> arrivals;
std::vector<Witness> witnesses;

std::string text(NSString *value) {
  return value == nil ? std::string() : std::string(value.UTF8String);
}

// The caller a message came from: the "id" of a JSON text body (the form the
// library's own script posts), of a dictionary body, or the bare text.
std::string label_of(id body) {
  if ([body isKindOfClass:[NSString class]]) {
    id parsed = [NSJSONSerialization JSONObjectWithData:[(NSString *)body dataUsingEncoding:NSUTF8StringEncoding]
                                                options:0
                                                  error:nil];
    if ([parsed isKindOfClass:[NSDictionary class]] && [parsed[@"id"] isKindOfClass:[NSString class]])
      return text(parsed[@"id"]);
    return text(body);
  }
  if ([body isKindOfClass:[NSDictionary class]] && [body[@"id"] isKindOfClass:[NSString class]]) return text(body[@"id"]);
  return "?";
}

// The stand-in's -userContentController:didReceiveScriptMessage:. Like the
// library's, it is invoked for every message and reads the body with
// -UTF8String; here the exception a body that is not text raises is recorded -
// in the app nothing catches it and the process ends.
void stand_in_received(id, SEL, id, WKScriptMessage *message) {
  Arrival arrival;
  arrival.label = label_of(message.body);
  @try {
    (void)[message.body UTF8String];
  } @catch (NSException *exception) {
    arrival.raised = text(exception.name);
  }
  arrivals.push_back(arrival);
}

void stand_in_replied(id, SEL, id, WKScriptMessage *, id) {}

} // namespace

@interface LMProbeWitness : NSObject <WKScriptMessageHandler>
@end

@implementation LMProbeWitness
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
  Witness witness;
  witness.label = [message.body isKindOfClass:[NSString class]] ? text(message.body) : "?";
  witness.main_frame = message.frameInfo.isMainFrame;
  witness.protocol = text(message.frameInfo.securityOrigin.protocol);
  witness.host = text(message.frameInfo.securityOrigin.host);
  witness.port = message.frameInfo.securityOrigin.port;
  witness.url = text(message.frameInfo.request.URL.absoluteString);
  witnesses.push_back(witness);
}
@end

namespace {

// ---------------------------------------------------------------------------
// The pages.

// Posts `label` to the native bridge as the library's own script does (JSON
// text), then to the witness.
const char *const post_script = R"JS(
function lmProbePost(label) {
  var handlers = window.webkit && window.webkit.messageHandlers;
  try { handlers.__webview__.postMessage(JSON.stringify({id: label, method: 'probe', params: '[]'})); } catch (error) {}
  try { handlers.lmProbeWitness.postMessage(label); } catch (error) {}
}
function lmProbePage(script) {
  return '<!doctype html><meta charset="utf-8"><script>' + lmProbePost.toString() + '\n' + script + '<\/script>';
}
)JS";

// The window's page: the main frame posts, then builds every kind of frame.
const char *const frames_page = R"JS(
lmProbePost('main');
try { window.webkit.messageHandlers.__webview__.postMessage({id: 'main-object', x: 1}); } catch (error) {}
try { window.webkit.messageHandlers.lmProbeWitness.postMessage('main-object'); } catch (error) {}
location.hash = '/project/mt_1/overview';
lmProbePost('main-after-route');
function frame(attributes) {
  var element = document.createElement('iframe');
  Object.keys(attributes).forEach(function (name) { element.setAttribute(name, attributes[name]); });
  document.body.appendChild(element);
}
var viaParent = "try { parent.webkit.messageHandlers.__webview__.postMessage(JSON.stringify({id: LABEL, method: 'probe', params: '[]'})); } catch (error) {}\n"
  + "window.webkit.messageHandlers.lmProbeWitness.postMessage(LABEL);";
frame({srcdoc: lmProbePage("lmProbePost('srcdoc')")});
frame({src: 'data:text/html;charset=utf-8,' + encodeURIComponent(lmProbePage("lmProbePost('data')"))});
frame({src: URL.createObjectURL(new Blob([lmProbePage("lmProbePost('blob')")], {type: 'text/html'}))});
frame({sandbox: 'allow-scripts', src: 'frame.html#sandboxed'});
frame({src: 'frame.html#same-origin-frame'});
frame({src: 'http://127.0.0.1:OTHER_PORT/frame.html#cross-origin-frame'});
frame({sandbox: 'allow-scripts', srcdoc: lmProbePage(viaParent.split('LABEL').join("'sandboxed-via-parent'"))});
frame({srcdoc: lmProbePage(viaParent.split('LABEL').join("'srcdoc-via-parent'"))});
)JS";

// A page that sends the main frame to the place its #fragment names.
const char *const navigation_page = R"JS(
var target = decodeURIComponent(location.hash.slice(1));
lmProbePost('start:' + target);
var go = {
  'nav-data': function () { location.href = 'data:text/html;charset=utf-8,' + encodeURIComponent(lmProbePage("lmProbePost('nav-data')")); },
  'nav-blob': function () { location.href = URL.createObjectURL(new Blob([lmProbePage("lmProbePost('nav-blob')")], {type: 'text/html'})); },
  'nav-other-origin': function () { location.href = 'http://127.0.0.1:OTHER_PORT/landing.html?nav-other-origin'; },
  'nav-redirect-away': function () { location.href = 'http://127.0.0.1:OWN_PORT/redirect-away'; },
  'nav-own-redirect': function () { location.href = 'http://127.0.0.1:OWN_PORT/redirect-home'; },
  'nav-outside-file': function () { location.href = 'OUTSIDE_URL#nav-outside-file'; },
  'nav-own-page': function () { location.href = 'second.html#nav-own-page'; }
};
go[target]();
)JS";

// A page that posts the label its #fragment or ?query carries.
const char *const label_page = R"JS(
lmProbePost(decodeURIComponent((location.hash || location.search).slice(1)));
)JS";

std::string replaced(std::string text, const std::string &from, const std::string &to) {
  for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}

std::string html_page(const char *body_script) {
  return std::string("<!doctype html><html><head><meta charset=\"utf-8\"><title>window gate probe</title><script>")
      + post_script + "</script></head><body><script>" + body_script + "</script></body></html>";
}

// ---------------------------------------------------------------------------
// The HTTP server: 127.0.0.1 only, each connection on its own thread, answered
// from a fixed table by path.

std::string http_response(const std::string &status, const std::string &headers, const std::string &body) {
  return "HTTP/1.1 " + status + "\r\n" + headers + "Content-Length: " + std::to_string(body.size())
      + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

using Routes = std::map<std::string, std::string>;

int listen_on_loopback(int &port) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  socklen_t length = sizeof address;
  if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || listen(listener, 64) != 0
      || getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
    close(listener);
    return -1;
  }
  port = ntohs(address.sin_port);
  return listener;
}

// Every request the server was asked, as "<port> <path?query>".
std::mutex requests_mutex;
std::vector<std::string> requests;

bool requested(int port, const std::string &target) {
  const std::lock_guard<std::mutex> lock(requests_mutex);
  for (const std::string &request : requests)
    if (request == std::to_string(port) + " " + target) return true;
  return false;
}

void answer(int client, int port, std::shared_ptr<const Routes> routes) {
  const int on = 1;
  setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
  timeval timeout{5, 0};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  std::string request;
  char buffer[4096];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
    const ssize_t received = recv(client, buffer, sizeof buffer, 0);
    if (received <= 0) break;
    request.append(buffer, static_cast<std::size_t>(received));
  }
  // "GET /path?query HTTP/1.1"
  const std::size_t start = request.find(' ');
  const std::size_t end = start == std::string::npos ? std::string::npos : request.find_first_of(" ?", start + 1);
  const std::string path = end == std::string::npos ? std::string() : request.substr(start + 1, end - start - 1);
  if (start != std::string::npos) {
    const std::lock_guard<std::mutex> lock(requests_mutex);
    requests.push_back(std::to_string(port) + " " + request.substr(start + 1, request.find(' ', start + 1) - start - 1));
  }
  const auto found = routes->find(path);
  const std::string response = found != routes->end() ? found->second : http_response("404 Not Found", "", "");
  std::size_t sent = 0;
  while (sent < response.size()) {
    const ssize_t wrote = send(client, response.data() + sent, response.size() - sent, 0);
    if (wrote <= 0) break;
    sent += static_cast<std::size_t>(wrote);
  }
  close(client);
}

void serve(int listener, int port, std::shared_ptr<const Routes> routes) {
  std::thread([listener, port, routes] {
    for (;;) {
      const int client = accept(listener, nullptr, nullptr);
      if (client >= 0) std::thread(answer, client, port, routes).detach();
    }
  }).detach();
}

// ---------------------------------------------------------------------------
// The callers and what each must come to.

enum class Expect {
  Reaches,     // the window's own page: must reach the handler, exactly once
  Dropped,     // its script runs, and nothing it posts reaches the handler
  Refused,     // the navigation policy stops it: no script runs there at all
  NotReached,  // nothing it posts reaches the handler (whether it ran or not)
  Info,        // shown, not judged
};

struct Caller {
  std::string label;
  std::string description;
  Expect expect;
  bool navigation = false;
};

std::vector<Caller> callers_of(const std::string &round, bool policy) {
  const Expect elsewhere = policy ? Expect::Refused : Expect::NotReached;
  std::vector<Caller> callers{
      {"main", "main frame, text body", Expect::Reaches},
      {"main-after-route", "main frame after a #/route change", Expect::Reaches},
      {"main-object", "main frame, postMessage({x:1})", Expect::Dropped},
      {"srcdoc", "about:srcdoc iframe", Expect::Dropped},
      {"data", "data: iframe", Expect::Dropped},
      {"blob", "blob: iframe", Expect::Dropped},
      {"sandboxed", "sandboxed iframe (allow-scripts)", Expect::Dropped},
      {"same-origin-frame", "same-folder iframe (own origin)", Expect::Dropped},
      {"cross-origin-frame", "cross-origin http iframe", policy ? Expect::Refused : Expect::Dropped},
      {"sandboxed-via-parent", "sandboxed iframe through parent", Expect::Dropped},
      {"srcdoc-via-parent", "srcdoc iframe through parent", Expect::Info},
      {"nav-data", "main frame navigated to data:", elsewhere, true},
      {"nav-blob", "main frame navigated to blob:", elsewhere, true},
      {"nav-other-origin", "main frame navigated to other origin", elsewhere, true},
  };
  if (round == "file") {
    callers.push_back({"nav-outside-file", "main frame navigated to file outside", elsewhere, true});
    callers.push_back({"nav-own-page", "main frame navigated to own page", Expect::Reaches, true});
  } else {
    callers.push_back({"nav-redirect-away", "main frame 302 to other origin", elsewhere, true});
    callers.push_back({"nav-own-page", "main frame navigated to own page", Expect::Reaches, true});
    callers.push_back({"nav-own-redirect", "main frame 302 within own origin", Expect::Reaches, true});
  }
  return callers;
}

const char *expectation(Expect expect) {
  switch (expect) {
  case Expect::Reaches: return "reaches";
  case Expect::Dropped: return "dropped";
  case Expect::Refused: return "refused";
  case Expect::NotReached: return "not reached";
  case Expect::Info: return "info";
  }
  return "";
}

const Witness *witnessed(const std::string &label) {
  for (const Witness &witness : witnesses)
    if (witness.label == label) return &witness;
  return nullptr;
}

std::string origin_text(const Witness &witness) {
  const std::string frame = witness.main_frame ? "main " : "sub ";
  if (witness.protocol.empty()) return frame + "opaque origin";
  return frame + witness.protocol + "://" + witness.host + ":" + std::to_string(witness.port);
}

// Runs the main run loop until every label in `needed` was witnessed, or
// `limit` seconds passed; then `settle` seconds more, for anything that comes
// late - a page a refused navigation would have shown.
void run_until_witnessed(const std::vector<std::string> &needed, double limit, double settle) {
  NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:limit];
  while ([deadline timeIntervalSinceNow] > 0) {
    bool all = true;
    for (const std::string &label : needed)
      if (witnessed(label) == nullptr) all = false;
    if (all) break;
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
  }
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:settle]];
}

WKWebView *open_view(WKUserContentController *controller, bool policy, NSURL *url, NSURL *read_access) {
  WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
  configuration.userContentController = controller;
  // In no window: nothing is shown on screen.
  WKWebView *view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300) configuration:configuration];
  if (policy) platform::install_navigation_policy((__bridge void *)view);
  if (url.isFileURL)
    [view loadFileURL:url allowingReadAccessToURL:read_access];
  else
    [view loadRequest:[NSURLRequest requestWithURL:url]];
  return view;
}

std::string short_url(const std::string &url, const std::string &root) {
  std::string shown = url.rfind(root, 0) == 0 ? "<tmp>/" + url.substr(root.size()) : url;
  return shown.size() > 46 ? shown.substr(0, 43) + "..." : shown;
}

struct Row {
  std::string caller;
  std::string ran;
  std::string reached;
  std::string expected;
  std::string verdict;
  std::string where;
};

void print_rows(const std::vector<Row> &rows) {
  std::cout << "  " << std::left << std::setw(41) << "CALLER" << std::setw(5) << "RAN" << std::setw(9) << "REACHED"
            << std::setw(12) << "EXPECTED" << std::setw(8) << "VERDICT" << "WHERE IT RAN / NOTE\n";
  for (const Row &row : rows)
    std::cout << "  " << std::left << std::setw(41) << row.caller << std::setw(5) << row.ran << std::setw(9)
              << row.reached << std::setw(12) << row.expected << std::setw(8) << row.verdict << row.where << "\n";
}

// ---------------------------------------------------------------------------
// One round, in this process.

int run_round(const std::string &round, bool gate, bool policy) {
  signal(SIGPIPE, SIG_IGN);
  [NSApplication sharedApplication];
  std::vector<Row> rows;
  int failures = 0;
  const auto judge = [&](bool passed) {
    if (!passed) ++failures;
    return std::string(passed ? "PASS" : "FAIL");
  };

  // The stand-in for the library's handler, and the gate in front of it.
  Class handler_class = objc_lookUpClass(library_handler_class);
  if (handler_class != nullptr) {
    std::cout << "A class named " << library_handler_class << " already exists; this probe needs its own stand-in.\n";
    return 2;
  }
  const bool installed_without_class = gate && platform::install_message_gate();
  handler_class = objc_allocateClassPair([NSResponder class], library_handler_class, 0);
  class_addProtocol(handler_class, @protocol(WKScriptMessageHandler));
  class_addMethod(handler_class, @selector(userContentController:didReceiveScriptMessage:),
      reinterpret_cast<IMP>(&stand_in_received), "v@:@@");
  objc_registerClassPair(handler_class);
  const Method method = class_getInstanceMethod(handler_class, @selector(userContentController:didReceiveScriptMessage:));
  const IMP stand_in = method_getImplementation(method);
  if (gate) {
    rows.push_back({"gate before the library's class exists", "-", "-", "refuses",
        judge(!installed_without_class), "install_message_gate() returned false"});
    const bool first = platform::install_message_gate();
    const IMP after_first = method_getImplementation(method);
    rows.push_back({"gate on WebviewWKScriptMessageHandler", "-", "-", "installs",
        judge(first && after_first != stand_in), "the library's IMP replaced, kept to call"});
    const bool second = platform::install_message_gate();
    rows.push_back({"gate installed a second time", "-", "-", "no-op",
        judge(second && method_getImplementation(method) == after_first), "IMP unchanged, nothing wraps itself"});
  }

  // The page, the frames and the pages the main frame is sent to.
  char folder_template[PATH_MAX];
  snprintf(folder_template, sizeof folder_template, "%slm-window-gate-probe.XXXXXX", NSTemporaryDirectory().UTF8String);
  char resolved[PATH_MAX];
  if (mkdtemp(folder_template) == nullptr || realpath(folder_template, resolved) == nullptr) {
    std::cout << "Could not make a temporary folder.\n";
    return 2;
  }
  const std::string root(resolved);
  const std::string frontend = root + "/frontend";
  const std::string outside = root + "/outside";
  [[NSFileManager defaultManager] createDirectoryAtPath:@(frontend.c_str()) withIntermediateDirectories:YES
                                             attributes:nil error:nil];
  [[NSFileManager defaultManager] createDirectoryAtPath:@(outside.c_str()) withIntermediateDirectories:YES
                                             attributes:nil error:nil];
  int own_port = 0;
  int other_port = 0;
  const int own_listener = listen_on_loopback(own_port);
  const int other_listener = listen_on_loopback(other_port);
  if (own_listener < 0 || other_listener < 0) {
    std::cout << "Could not listen on 127.0.0.1.\n";
    return 2;
  }
  NSURL *root_url = [NSURL fileURLWithPath:@(root.c_str()) isDirectory:YES];
  NSURL *frontend_url = [NSURL fileURLWithPath:@(frontend.c_str()) isDirectory:YES];
  const std::string outside_page = text([NSURL fileURLWithPath:@((outside + "/landing.html").c_str())].absoluteString);
  const auto fill = [&](const char *script) {
    std::string page = html_page(script);
    page = replaced(page, "OTHER_PORT", std::to_string(other_port));
    page = replaced(page, "OWN_PORT", std::to_string(own_port));
    return replaced(page, "OUTSIDE_URL", outside_page);
  };
  const std::map<std::string, std::string> files{
      {frontend + "/index.html", fill(frames_page)},
      {frontend + "/nav.html", fill(navigation_page)},
      {frontend + "/frame.html", fill(label_page)},
      {frontend + "/second.html", fill(label_page)},
      {outside + "/landing.html", fill(label_page)},
  };
  for (const auto &[path, content] : files) std::ofstream(path) << content;
  const std::string page_response = "Content-Type: text/html; charset=utf-8\r\n";
  auto routes = std::make_shared<Routes>(Routes{
      {"/", http_response("200 OK", page_response, fill(frames_page))},
      {"/index.html", http_response("200 OK", page_response, fill(frames_page))},
      {"/nav.html", http_response("200 OK", page_response, fill(navigation_page))},
      {"/frame.html", http_response("200 OK", page_response, fill(label_page))},
      {"/second.html", http_response("200 OK", page_response, fill(label_page))},
      {"/landing.html", http_response("200 OK", page_response, fill(label_page))},
      {"/redirect-away", http_response("302 Found",
          "Location: http://127.0.0.1:" + std::to_string(other_port) + "/landing.html?nav-redirect-away\r\n", "")},
      {"/redirect-home", http_response("302 Found", "Location: /second.html?nav-own-redirect\r\n", "")},
  });
  serve(own_listener, own_port, routes);
  serve(other_listener, other_port, routes);

  // The window's own place, remembered as the app remembers it.
  const std::string own_base =
      round == "file" ? text(frontend_url.absoluteString) : "http://127.0.0.1:" + std::to_string(own_port) + "/";
  platform::allow_window_prefix(platform::window_prefix_of(own_base, round == "file"));
  const std::string prefix = platform::window_prefix_of(own_base, round == "file");

  WKUserContentController *controller = [[WKUserContentController alloc] init];
  [controller addScriptMessageHandler:(id<WKScriptMessageHandler>)[[handler_class alloc] init] name:@"__webview__"];
  [controller addScriptMessageHandler:[[LMProbeWitness alloc] init] name:@"lmProbeWitness"];

  const std::vector<Caller> callers = callers_of(round, policy);
  std::map<std::string, WKWebView *> navigation_views;
  open_view(controller, policy, [NSURL URLWithString:@((own_base + (round == "file" ? "index.html" : "")).c_str())],
      root_url);
  std::vector<std::string> needed{"main", "main-after-route"};
  for (const Caller &caller : callers) {
    if (caller.navigation) {
      navigation_views[caller.label] = open_view(controller, policy,
          [NSURL URLWithString:@((own_base + "nav.html#" + caller.label).c_str())], root_url);
      needed.push_back("start:" + caller.label);
      if (caller.expect == Expect::Reaches) needed.push_back(caller.label);
    } else if (caller.expect == Expect::Dropped || caller.expect == Expect::Info) {
      needed.push_back(caller.label);
    }
  }
  run_until_witnessed(needed, 20.0, 2.5);

  for (const Caller &caller : callers) {
    const Witness *witness = witnessed(caller.label);
    int reached = 0;
    std::string raised;
    for (const Arrival &arrival : arrivals)
      if (arrival.label == caller.label) {
        ++reached;
        if (!arrival.raised.empty()) raised = arrival.raised;
      }
    const bool ran = witness != nullptr;
    std::string verdict;
    switch (caller.expect) {
    case Expect::Reaches: verdict = judge(reached == 1); break;
    case Expect::Dropped: verdict = judge(reached == 0 && ran); break;
    case Expect::Refused: verdict = judge(reached == 0 && !ran); break;
    case Expect::NotReached: verdict = judge(reached == 0); break;
    case Expect::Info: verdict = "INFO"; break;
    }
    std::string where = ran ? origin_text(*witness) : "";
    if (caller.navigation)
      where += (where.empty() ? "" : ", ") + std::string("ended on ")
          + short_url(text(navigation_views[caller.label].URL.absoluteString), text(root_url.absoluteString));
    // Whether the server behind a navigation was asked at all.
    if (caller.label == "nav-other-origin")
      where += requested(other_port, "/landing.html?nav-other-origin") ? "; other origin asked" : "; other origin never asked";
    if (caller.label == "nav-redirect-away")
      where += std::string(requested(own_port, "/redirect-away") ? "; own origin sent the 302" : "; no 302 sent")
          + (requested(other_port, "/landing.html?nav-redirect-away") ? ", other origin asked" : ", other origin never asked");
    if (!raised.empty()) where += ", -UTF8String raised " + raised + " (the app would crash)";
    if (caller.expect == Expect::Dropped && !ran && reached == 0) where += " (did not run: inconclusive)";
    rows.push_back({caller.description, ran ? "yes" : "no", reached == 0 ? "no" : reached == 1 ? "yes" : "yes x" +
        std::to_string(reached), expectation(caller.expect), verdict, where});
  }

  if (gate) {
    // A library that also answers through a reply method would get past a gate
    // on the first method alone.
    class_addMethod(handler_class, sel_registerName("userContentController:didReceiveScriptMessage:replyHandler:"),
        reinterpret_cast<IMP>(&stand_in_replied), "v@:@@@?");
    rows.push_back({"gate on a library with a reply method", "-", "-", "refuses",
        judge(!platform::install_message_gate()), "install_message_gate() returned false"});
  }

  std::cout << "\nROUND " << round << " - gate " << (gate ? "on" : "OFF") << ", navigation policy "
            << (policy ? "on" : "OFF") << "; window prefix " << short_url(prefix, text(root_url.absoluteString))
            << ", other origin http://127.0.0.1:" << other_port << "\n";
  if (const Witness *main = witnessed("main"))
    std::cout << "  WebKit's security origin for the main frame: protocol \"" << main->protocol << "\", host \""
              << main->host << "\", port " << main->port << "; frame URL " << short_url(main->url,
                  text(root_url.absoluteString)) << "\n";
  if (const Witness *routed = witnessed("main-after-route"))
    std::cout << "  frame URL after the #/route change: " << short_url(routed->url, text(root_url.absoluteString))
              << "\n";
  print_rows(rows);
  std::cout << "ROUND " << round << ": " << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failed)\n"
            << std::flush;
  [[NSFileManager defaultManager] removeItemAtPath:@(root.c_str()) error:nil];
  return failures == 0 ? 0 : 1;
}

// Both rounds, each in a process of its own.
int run_rounds(bool gate, bool policy) {
  char path[PATH_MAX];
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) return 2;
  int failed = 0;
  for (const char *round : {"file", "dev"}) {
    std::vector<std::string> arguments{path, "--round", round};
    if (!gate) arguments.push_back("--no-gate");
    if (!policy) arguments.push_back("--no-policy");
    std::vector<char *> argv;
    for (std::string &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::cout << std::flush;
    pid_t child = 0;
    int status = 0;
    if (posix_spawn(&child, path, nullptr, nullptr, argv.data(), environ) != 0 || waitpid(child, &status, 0) < 0) {
      std::cout << "ROUND " << round << " could not run\n";
      ++failed;
      continue;
    }
    if (WIFSIGNALED(status)) std::cout << "ROUND " << round << " ended by signal " << WTERMSIG(status) << "\n";
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ++failed;
  }
  std::cout << "\n" << (failed == 0 ? "ALL ROUNDS PASS" : "SOME ROUNDS FAIL") << "\n";
  return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  @autoreleasepool {
    bool gate = true;
    bool policy = true;
    std::string round;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--no-gate") {
        gate = false;
      } else if (argument == "--no-policy") {
        policy = false;
      } else if (argument == "--round" && index + 1 < argc && (std::string(argv[index + 1]) == "file"
                     || std::string(argv[index + 1]) == "dev")) {
        round = argv[++index];
      } else {
        std::cerr << "usage: window_navigation_webkit_probe [--no-gate] [--no-policy] [--round file|dev]\n";
        return 2;
      }
    }
    return round.empty() ? run_rounds(gate, policy) : run_round(round, gate, policy);
  }
}
