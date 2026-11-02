// window_navigation_webkitgtk_probe [--no-gate] [--no-policy] [--round file|dev]
//
// Real WebKitGTK and the real webview library (webview 0.12, gtk_webkit_engine),
// in a window on the display the process is given - run it under xvfb-run, so
// that nothing appears on screen. The window loads a test page the way the app
// loads its frontend - from a temporary file:// folder whose name has a space
// and a non-ASCII letter (the packaged frontend, round "file") or from
// http://127.0.0.1:<port>/ (the development server, round "dev") - with the
// message gate and the navigation policy installed as in the app
// (AppWindow.cpp, WindowNavigationLinux.cpp). A small HTTP server in this
// process answers on two loopback ports: the window's own and another origin.
//
// The page calls the library's binding "probe" from its main frame, then
// builds every kind of frame - about:srcdoc, data:, blob:, sandboxed,
// same-folder, cross-origin - and asks for a second window, and each of them
// tries every way to the bound method: the binding where it has one, a message
// posted by hand to window.webkit.messageHandlers.__webview__, and the gate's
// own DOM event. Then the main frame is sent, one place after another, to
// data:, blob:, another origin, a file outside the folder, a 302 to another
// origin, and, as controls, to the window's own pages. WebKitGTK opens no
// second window unless the embedder answers "create"; the probe answers it
// (after the policy), as an embedder that opens windows would, to show what
// such a window would get. A second message handler, which no gate guards,
// witnesses which scripts ran, in which frame and with which origin.
//
// --no-gate and --no-policy show what reaches native code without each.
// Without --round both rounds run, each in its own process, since the window's
// prefixes last a process as in the app. Prints a PASS/FAIL row per caller;
// exits non-zero on any failure. Nothing leaves 127.0.0.1.

#include "Platform/WindowNavigation/WindowNavigation.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <webview/webview.h>

extern char** environ;

namespace {

namespace platform = singularity::platform;

// What a frame's script told the witness, which no gate guards: that it ran,
// and where.
struct Witness {
	std::string label;
	bool isMainFrame = false;
	std::string origin;
	std::string url;
};

// Read and written on the GTK main thread only.
std::vector<std::string> arrivals;
std::vector<Witness> witnesses;
int windowsAsked = 0;

// ---------------------------------------------------------------------------
// The pages.

// Every way a page could try to reach the bound native method "probe", then
// the witness.
const char* const postScript = R"JS(
function lmProbeWitness(label) {
	try {
		window.webkit.messageHandlers.lmProbeWitness.postMessage(JSON.stringify({
			label: label, url: String(location.href), origin: String(self.origin), top: window === window.top
		}));
	} catch (error) {}
}
function lmProbePost(label) {
	var message = JSON.stringify({id: label, method: 'probe', params: [label]});
	try { if (typeof window.probe === 'function') window.probe(label); } catch (error) {}
	try { window.webkit.messageHandlers.__webview__.postMessage(message); } catch (error) {}
	try { document.dispatchEvent(new CustomEvent('singularity-native-bridge', {detail: message})); } catch (error) {}
	lmProbeWitness(label);
}
function lmProbePage(script) {
	return '<!doctype html><meta charset="utf-8"><script>' + lmProbeWitness.toString() + '\n'
		+ lmProbePost.toString() + '\n' + script + '<\/script>';
}
)JS";

// The window's page: the main frame calls, then builds every kind of frame and
// asks for a second window.
const char* const framesPage = R"JS(
window.probe('main');
lmProbeWitness('main');
try {
	window.webkit.messageHandlers.__webview__.postMessage(JSON.stringify({id: 'main-raw', method: 'probe', params: ['main-raw']}));
} catch (error) {}
lmProbeWitness('main-raw');
location.hash = '/project/mt_1/overview';
window.probe('main-after-route');
lmProbeWitness('main-after-route');
function frame(attributes) {
	var element = document.createElement('iframe');
	Object.keys(attributes).forEach(function (name) { element.setAttribute(name, attributes[name]); });
	document.body.appendChild(element);
}
var viaParent = "try { parent.probe(LABEL); } catch (error) {}\n"
	+ "try { parent.document.dispatchEvent(new parent.CustomEvent('singularity-native-bridge', {detail: JSON.stringify({id: LABEL, method: 'probe', params: [LABEL]})})); } catch (error) {}\n"
	+ "try { parent.webkit.messageHandlers.__webview__.postMessage(JSON.stringify({id: LABEL, method: 'probe', params: [LABEL]})); } catch (error) {}\n"
	+ "lmProbeWitness(LABEL);";
frame({srcdoc: lmProbePage("lmProbePost('srcdoc')")});
frame({src: 'data:text/html;charset=utf-8,' + encodeURIComponent(lmProbePage("lmProbePost('data')"))});
frame({src: URL.createObjectURL(new Blob([lmProbePage("lmProbePost('blob')")], {type: 'text/html'}))});
frame({sandbox: 'allow-scripts', src: 'frame.html#sandboxed'});
frame({src: 'frame.html#same-origin-frame'});
frame({src: 'http://127.0.0.1:OTHER_PORT/frame.html#cross-origin-frame'});
frame({sandbox: 'allow-scripts', srcdoc: lmProbePage(viaParent.split('LABEL').join("'sandboxed-via-parent'"))});
frame({srcdoc: lmProbePage(viaParent.split('LABEL').join("'srcdoc-via-parent'"))});
setTimeout(function () {
	try { window.open('http://127.0.0.1:OTHER_PORT/landing.html?second-window'); } catch (error) {}
}, 300);
)JS";

// A page that sends the main frame to the place its #fragment names.
const char* const navigationPage = R"JS(
var target = decodeURIComponent(location.hash.slice(1));
lmProbeWitness('start:' + target);
var go = {
	'nav-data': function () { location.href = 'data:text/html;charset=utf-8,' + encodeURIComponent(lmProbePage("lmProbePost('nav-data')")); },
	'nav-blob': function () { location.href = URL.createObjectURL(new Blob([lmProbePage("lmProbePost('nav-blob')")], {type: 'text/html'})); },
	'nav-other-origin': function () { location.href = 'http://127.0.0.1:OTHER_PORT/landing.html?nav-other-origin'; },
	'nav-redirect-away': function () { location.href = 'http://127.0.0.1:OWN_PORT/redirect-away'; },
	'nav-own-redirect': function () { location.href = 'http://127.0.0.1:OWN_PORT/redirect-home'; },
	'nav-outside-file': function () { location.href = 'OUTSIDE_URL#nav-outside-file'; },
	'nav-own-page': function () { location.href = 'second.html#nav-own-page'; }
};
setTimeout(go[target], 50);
)JS";

// A page that tries every way with the label its #fragment or ?query carries.
const char* const labelPage = R"JS(
lmProbePost(decodeURIComponent((location.hash || location.search).slice(1)));
)JS";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
	for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
		text.replace(at, from.size(), to);
	return text;
}

std::string htmlPage(const char* bodyScript) {
	return std::string("<!doctype html><html><head><meta charset=\"utf-8\"><title>window gate probe</title><script>")
		+ postScript + "</script></head><body><script>" + bodyScript + "</script></body></html>";
}

// A file URL written the way AppPaths file_url writes the app's own.
std::string fileUrl(const std::string& path) {
	std::string url = "file://";
	for (const char character : path) {
		const unsigned char byte = static_cast<unsigned char>(character);
		const bool isPlain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
			|| byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '/' || byte == ':';
		if (isPlain == true) {
			url.push_back(character);
		} else {
			char escape[4];
			std::snprintf(escape, sizeof escape, "%%%02X", byte);
			url += escape;
		}
	}
	return url;
}

// ---------------------------------------------------------------------------
// The HTTP server: 127.0.0.1 only, each connection on its own thread, answered
// from a fixed table by path.

std::string httpResponse(const std::string& status, const std::string& headers, const std::string& body) {
	return "HTTP/1.1 " + status + "\r\n" + headers + "Content-Length: " + std::to_string(body.size())
		+ "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

using Routes = std::map<std::string, std::string>;

int listenOnLoopback(int& port) {
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) return -1;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	socklen_t length = sizeof address;
	if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(listener, 64) != 0
		|| getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
		close(listener);
		return -1;
	}
	port = ntohs(address.sin_port);
	return listener;
}

// Every request the server was asked, as "<port> <path?query>".
std::mutex requestsMutex;
std::vector<std::string> requests;

bool wasRequested(int port, const std::string& target) {
	const std::lock_guard<std::mutex> lock(requestsMutex);
	for (const std::string& request : requests)
		if (request == std::to_string(port) + " " + target) return true;
	return false;
}

void answer(int client, int port, std::shared_ptr<const Routes> routes) {
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
		const std::lock_guard<std::mutex> lock(requestsMutex);
		requests.push_back(std::to_string(port) + " " + request.substr(start + 1, request.find(' ', start + 1) - start - 1));
	}
	const Routes::const_iterator found = routes->find(path);
	const std::string response = found != routes->end() ? found->second : httpResponse("404 Not Found", "", "");
	std::size_t sent = 0;
	while (sent < response.size()) {
		const ssize_t wrote = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
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
// What the window's web view reports.

std::string textOf(WebKitJavascriptResult* result) {
	JSCValue* value = result == nullptr ? nullptr : webkit_javascript_result_get_js_value(result);
	if (value == nullptr || jsc_value_is_string(value) == FALSE) return {};
	char* text = jsc_value_to_string(value);
	const std::string copy = text == nullptr ? std::string() : std::string(text);
	g_free(text);
	return copy;
}

void witnessed(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
	const nlohmann::json parsed = nlohmann::json::parse(textOf(result), nullptr, false);
	if (parsed.is_object() == false) return;
	Witness witness;
	witness.label = parsed.value("label", std::string());
	witness.isMainFrame = parsed.value("top", false);
	witness.origin = parsed.value("origin", std::string());
	witness.url = parsed.value("url", std::string());
	witnesses.push_back(witness);
}

const Witness* witnessOf(const std::string& label) {
	for (const Witness& witness : witnesses)
		if (witness.label == label) return &witness;
	return nullptr;
}

// The embedder that opens windows: a related web view - the same web process
// and the same user content manager, so the library's scripts and handlers -
// in a window of its own.
GtkWidget* openSecondWindow(WebKitWebView* webView, WebKitNavigationAction*, gpointer) {
	++windowsAsked;
	GtkWidget* created = webkit_web_view_new_with_related_view(webView);
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
	gtk_container_add(GTK_CONTAINER(window), created);
	gtk_widget_show_all(window);
	return created;
}

// Runs the GTK main loop until `isDone` or `limit` seconds passed; then `settle`
// seconds more, for anything that comes late - a page a refused navigation
// would have shown.
void runUntil(const std::function<bool()>& isDone, double limit, double settle) {
	using Clock = std::chrono::steady_clock;
	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(static_cast<long>(limit * 1000));
	while (Clock::now() < deadline && isDone() == false) {
		while (g_main_context_iteration(nullptr, FALSE) == TRUE) {}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const Clock::time_point settled = Clock::now() + std::chrono::milliseconds(static_cast<long>(settle * 1000));
	while (Clock::now() < settled) {
		while (g_main_context_iteration(nullptr, FALSE) == TRUE) {}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

// ---------------------------------------------------------------------------
// The callers and what each must come to.

enum class Expect {
	Reaches,     // the window's own page: must reach the bound method
	Dropped,     // its script runs, and nothing it tries reaches the bound method
	Refused,     // the navigation policy stops it: no script runs there at all
	NotReached,  // nothing it tries reaches the bound method (whether it ran or not)
	Info,        // shown, not judged
};

struct Caller {
	std::string label;
	std::string description;
	Expect expect;
	bool isNavigation = false;
};

std::vector<Caller> callersOf(const std::string& round, bool hasPolicy) {
	const Expect elsewhere = hasPolicy == true ? Expect::Refused : Expect::NotReached;
	std::vector<Caller> callers{
		{"main", "main frame, its binding", Expect::Reaches},
		{"main-after-route", "main frame after a #/route change", Expect::Reaches},
		{"main-raw", "main frame, raw post to __webview__", Expect::Info},
		{"srcdoc", "about:srcdoc iframe", Expect::Dropped},
		{"data", "data: iframe", Expect::Dropped},
		{"blob", "blob: iframe", Expect::Dropped},
		{"sandboxed", "sandboxed iframe (allow-scripts)", Expect::Dropped},
		{"same-origin-frame", "same-folder iframe (own origin)", Expect::Dropped},
		{"cross-origin-frame", "cross-origin http iframe", hasPolicy == true ? Expect::Refused : Expect::Dropped},
		{"sandboxed-via-parent", "sandboxed iframe through parent", Expect::Dropped},
		{"srcdoc-via-parent", "srcdoc iframe through parent", Expect::Info},
		{"second-window", "second window (window.open)", elsewhere},
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

const char* expectation(Expect expect) {
	switch (expect) {
	case Expect::Reaches: return "reaches";
	case Expect::Dropped: return "dropped";
	case Expect::Refused: return "refused";
	case Expect::NotReached: return "not reached";
	case Expect::Info: return "info";
	}
	return "";
}

std::string shortUrl(const std::string& url, const std::string& root) {
	const std::string shown = url.rfind(root, 0) == 0 ? "<tmp>/" + url.substr(root.size()) : url;
	return shown.size() > 46 ? shown.substr(0, 43) + "..." : shown;
}

std::string originText(const Witness& witness, const std::string& root) {
	return std::string(witness.isMainFrame == true ? "main " : "sub ") + witness.origin + " " + shortUrl(witness.url, root);
}

struct Row {
	std::string caller;
	std::string ran;
	std::string reached;
	std::string expected;
	std::string verdict;
	std::string where;
};

void printRows(const std::vector<Row>& rows) {
	std::cout << "  " << std::left << std::setw(41) << "CALLER" << std::setw(5) << "RAN" << std::setw(9) << "REACHED"
			  << std::setw(12) << "EXPECTED" << std::setw(8) << "VERDICT" << "WHERE IT RAN / NOTE\n";
	for (const Row& row : rows)
		std::cout << "  " << std::left << std::setw(41) << row.caller << std::setw(5) << row.ran << std::setw(9)
				  << row.reached << std::setw(12) << row.expected << std::setw(8) << row.verdict << row.where << "\n";
}

// ---------------------------------------------------------------------------
// One round, in this process.

int runRound(const std::string& round, bool hasGate, bool hasPolicy) {

	signal(SIGPIPE, SIG_IGN);
	std::vector<Row> rows;
	int failures = 0;
	const auto judge = [&failures](bool isPassed) {
		if (isPassed == false) ++failures;
		return std::string(isPassed == true ? "PASS" : "FAIL");
	};

	// The pages, in a folder named the way a user's own or an install folder
	// may be: with a space and a non-ASCII letter.
	std::string folderTemplate = (std::filesystem::temp_directory_path() / "lm-window-gate probe \xD0\x96.XXXXXX").string();
	if (mkdtemp(folderTemplate.data()) == nullptr) {
		std::cout << "Could not make a temporary folder.\n";
		return 2;
	}
	const std::string root = std::filesystem::canonical(folderTemplate).string();
	const std::string frontend = root + "/frontend";
	const std::string outside = root + "/outside";
	std::filesystem::create_directories(frontend);
	std::filesystem::create_directories(outside);
	int ownPort = 0;
	int otherPort = 0;
	const int ownListener = listenOnLoopback(ownPort);
	const int otherListener = listenOnLoopback(otherPort);
	if (ownListener < 0 || otherListener < 0) {
		std::cout << "Could not listen on 127.0.0.1.\n";
		return 2;
	}
	const std::string rootUrl = fileUrl(root) + "/";
	const std::string outsidePage = fileUrl(outside + "/landing.html");
	const auto fill = [&](const char* script) {
		std::string page = htmlPage(script);
		page = replaced(page, "OTHER_PORT", std::to_string(otherPort));
		page = replaced(page, "OWN_PORT", std::to_string(ownPort));
		return replaced(page, "OUTSIDE_URL", outsidePage);
	};
	const std::map<std::string, std::string> files{
		{frontend + "/index.html", fill(framesPage)},
		{frontend + "/nav.html", fill(navigationPage)},
		{frontend + "/frame.html", fill(labelPage)},
		{frontend + "/second.html", fill(labelPage)},
		{outside + "/landing.html", fill(labelPage)},
	};
	for (const std::pair<const std::string, std::string>& file : files) std::ofstream(file.first) << file.second;
	const std::string pageHeaders = "Content-Type: text/html; charset=utf-8\r\n";
	auto routes = std::make_shared<Routes>(Routes{
		{"/", httpResponse("200 OK", pageHeaders, fill(framesPage))},
		{"/index.html", httpResponse("200 OK", pageHeaders, fill(framesPage))},
		{"/nav.html", httpResponse("200 OK", pageHeaders, fill(navigationPage))},
		{"/frame.html", httpResponse("200 OK", pageHeaders, fill(labelPage))},
		{"/second.html", httpResponse("200 OK", pageHeaders, fill(labelPage))},
		{"/landing.html", httpResponse("200 OK", pageHeaders, fill(labelPage))},
		{"/redirect-away", httpResponse("302 Found",
			"Location: http://127.0.0.1:" + std::to_string(otherPort) + "/landing.html?nav-redirect-away\r\n", "")},
		{"/redirect-home", httpResponse("302 Found", "Location: /second.html?nav-own-redirect\r\n", "")},
	});
	serve(ownListener, ownPort, routes);
	serve(otherListener, otherPort, routes);

	// The window, as AppWindow makes it.
	webview::webview view(false, nullptr);
	GtkWidget* widget = static_cast<GtkWidget*>(view.widget().value());
	WebKitWebView* webView = WEBKIT_WEB_VIEW(widget);
	WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(webView);
	if (hasGate == true) {
		const bool isInstalled = platform::install_message_gate(widget);
		rows.push_back({"gate on the window's web view", "-", "-", "installs", judge(isInstalled),
			"install_message_gate() returned " + std::string(isInstalled == true ? "true" : "false")});
		if (isInstalled == false) {
			printRows(rows);
			return 1;
		}
		view.init(platform::message_gate_page_script());
	}
	if (hasPolicy == true) platform::install_navigation_policy(widget);

	// What the probe adds: a page may open windows without a click (a click
	// would allow it too), an embedder that opens them, and the witness.
	webkit_settings_set_javascript_can_open_windows_automatically(webkit_web_view_get_settings(webView), TRUE);
	g_signal_connect(webView, "create", G_CALLBACK(&openSecondWindow), nullptr);
	g_signal_connect(manager, "script-message-received::lmProbeWitness", G_CALLBACK(&witnessed), nullptr);
	webkit_user_content_manager_register_script_message_handler(manager, "lmProbeWitness");

	view.bind("probe", [](const std::string& request) -> std::string {
		const nlohmann::json parsed = nlohmann::json::parse(request, nullptr, false);
		const bool isLabelled = parsed.is_array() == true && parsed.empty() == false && parsed[0].is_string() == true;
		arrivals.push_back(isLabelled == true ? parsed[0].get<std::string>() : std::string("?"));
		return "";
	});
	if (hasGate == true) {
		// As AppWindow::bind does after every binding.
		const bool isAgain = platform::install_message_gate(widget);
		rows.push_back({"gate installed again after bind()", "-", "-", "installs", judge(isAgain),
			"install_message_gate() returned " + std::string(isAgain == true ? "true" : "false")});
		GtkWidget* plain = webkit_web_view_new();
		g_object_ref_sink(plain);
		const bool isPlainGuarded = platform::install_message_gate(plain);
		rows.push_back({"gate on a view without the library", "-", "-", "refuses", judge(isPlainGuarded == false),
			"install_message_gate() returned " + std::string(isPlainGuarded == true ? "true" : "false")});
		g_object_unref(plain);
	}

	// The window's own place, remembered as the app remembers it.
	const std::string ownBase =
		round == "file" ? fileUrl(frontend) + "/" : "http://127.0.0.1:" + std::to_string(ownPort) + "/";
	const std::string prefix = platform::window_prefix_of(ownBase, round == "file");
	platform::allow_window_prefix(prefix);
	view.navigate(ownBase + (round == "file" ? "index.html" : ""));

	const std::vector<Caller> callers = callersOf(round, hasPolicy);
	std::vector<std::string> needed{"main", "main-after-route"};
	for (const Caller& caller : callers)
		if (caller.isNavigation == false && (caller.expect == Expect::Dropped || caller.expect == Expect::Info))
			needed.push_back(caller.label);
	const auto allWitnessed = [&needed] {
		for (const std::string& label : needed)
			if (witnessOf(label) == nullptr) return false;
		return true;
	};
	runUntil(allWitnessed, 20.0, 3.0);
	const std::string firstUri = webkit_web_view_get_uri(webView) == nullptr ? "" : webkit_web_view_get_uri(webView);

	// The main frame, sent to one place after another from the window's own page.
	std::map<std::string, std::string> endedOn;
	for (const Caller& caller : callers) {
		if (caller.isNavigation == false) continue;
		// The query makes each a new document, not a change of #fragment.
		view.navigate(ownBase + "nav.html?" + caller.label + "#" + caller.label);
		const std::string start = "start:" + caller.label;
		const bool isReaching = caller.expect == Expect::Reaches;
		runUntil([&start, &caller, isReaching] {
			return witnessOf(start) != nullptr && (isReaching == false || witnessOf(caller.label) != nullptr);
		}, 10.0, 2.0);
		const gchar* uri = webkit_web_view_get_uri(webView);
		endedOn[caller.label] = uri == nullptr ? "" : uri;
	}

	for (const Caller& caller : callers) {
		const Witness* witness = witnessOf(caller.label);
		int reached = 0;
		for (const std::string& arrival : arrivals)
			if (arrival == caller.label) ++reached;
		const bool hasRun = witness != nullptr;
		std::string verdict;
		switch (caller.expect) {
		case Expect::Reaches: verdict = judge(reached >= 1); break;
		case Expect::Dropped: verdict = judge(reached == 0 && hasRun == true); break;
		case Expect::Refused: verdict = judge(reached == 0 && hasRun == false); break;
		case Expect::NotReached: verdict = judge(reached == 0); break;
		case Expect::Info: verdict = "INFO"; break;
		}
		std::string where = hasRun == true ? originText(*witness, rootUrl) : "";
		if (caller.isNavigation == true)
			where += (where.empty() == true ? "" : ", ") + std::string("ended on ") + shortUrl(endedOn[caller.label], rootUrl);
		if (caller.label == "nav-other-origin")
			where += wasRequested(otherPort, "/landing.html?nav-other-origin") == true ? "; other origin asked"
																						: "; other origin never asked";
		if (caller.label == "nav-redirect-away")
			where += std::string(wasRequested(ownPort, "/redirect-away") == true ? "; own origin sent the 302" : "; no 302 sent")
				+ (wasRequested(otherPort, "/landing.html?nav-redirect-away") == true ? ", other origin asked"
																						: ", other origin never asked");
		if (caller.label == "second-window")
			where += (where.empty() == true ? "" : ", ") + std::string("embedder asked for ")
				+ std::to_string(windowsAsked) + " window(s)";
		if (caller.expect == Expect::Dropped && hasRun == false && reached == 0) where += " (did not run: inconclusive)";
		const std::string reachedText = reached == 0 ? "no" : reached == 1 ? "yes" : "yes x" + std::to_string(reached);
		rows.push_back({caller.description, hasRun == true ? "yes" : "no", reachedText, expectation(caller.expect), verdict,
			where});
	}

	std::cout << "\nROUND " << round << " - gate " << (hasGate == true ? "on" : "OFF") << ", navigation policy "
			  << (hasPolicy == true ? "on" : "OFF") << "; WebKitGTK " << webkit_get_major_version() << "."
			  << webkit_get_minor_version() << "." << webkit_get_micro_version() << "; window prefix "
			  << shortUrl(prefix, rootUrl) << ", other origin http://127.0.0.1:" << otherPort << "\n";
	std::cout << "  temporary folder: " << root << "\n";
	if (const Witness* main = witnessOf("main"))
		std::cout << "  the main frame's origin (self.origin): \"" << main->origin << "\"; its URL "
				  << shortUrl(main->url, rootUrl) << "; the view's URI " << shortUrl(firstUri, rootUrl) << "\n";
	if (const Witness* routed = witnessOf("main-after-route"))
		std::cout << "  URL after the #/route change: " << shortUrl(routed->url, rootUrl) << "\n";
	printRows(rows);
	std::cout << "ROUND " << round << ": " << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failed)\n"
			  << std::flush;
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	return failures == 0 ? 0 : 1;

}

// Both rounds, each in a process of its own.
int runRounds(bool hasGate, bool hasPolicy) {
	int failed = 0;
	for (const char* round : {"file", "dev"}) {
		std::vector<std::string> arguments{"/proc/self/exe", "--round", round};
		if (hasGate == false) arguments.push_back("--no-gate");
		if (hasPolicy == false) arguments.push_back("--no-policy");
		std::vector<char*> argv;
		for (std::string& argument : arguments) argv.push_back(argument.data());
		argv.push_back(nullptr);
		std::cout << std::flush;
		pid_t child = 0;
		int status = 0;
		if (posix_spawn(&child, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) != 0
			|| waitpid(child, &status, 0) < 0) {
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

int main(int argc, char** argv) {
	bool hasGate = true;
	bool hasPolicy = true;
	std::string round;
	for (int index = 1; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "--no-gate") {
			hasGate = false;
		} else if (argument == "--no-policy") {
			hasPolicy = false;
		} else if (argument == "--round" && index + 1 < argc
			&& (std::string(argv[index + 1]) == "file" || std::string(argv[index + 1]) == "dev")) {
			round = argv[++index];
		} else {
			std::cerr << "usage: window_navigation_webkitgtk_probe [--no-gate] [--no-policy] [--round file|dev]\n";
			return 2;
		}
	}
	if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
		std::cerr << "window_navigation_webkitgtk_probe needs a display: run it under xvfb-run.\n";
		return 2;
	}
	try {
		return round.empty() == true ? runRounds(hasGate, hasPolicy) : runRound(round, hasGate, hasPolicy);
	} catch (const std::exception& error) {
		std::cout << "FAIL the probe stopped: " << error.what() << "\n";
		return 1;
	}
}
