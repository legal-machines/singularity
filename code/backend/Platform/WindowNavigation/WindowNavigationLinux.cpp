#include "Platform/WindowNavigation/WindowNavigation.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <webkit2/webkit2.h>

#if GTK_MAJOR_VERSION >= 4
#error "WindowNavigationLinux.cpp is written for WebKitGTK's GTK 3 API (webkit2gtk-4.1); review it for webkitgtk-6.0"
#endif

// The window's navigation policy and native bridge gate on Linux (WebKitGTK),
// the counterpart of WindowNavigationMacOS.cpp: the same decisions
// (WindowNavigation.cpp), taken where WebKitGTK offers them.
//
// - Navigation: "decide-policy" ignores what window_may_show() refuses.
//   WebKitGTK does not say which frame a navigation action is for, so there a
//   navigation is held to the sub-frame rule (the window's prefixes,
//   about:blank, and what a page builds itself: about:srcdoc, blob:, data:).
//   The response decision does say whether a page is the main frame's, and
//   holds it to the main-frame rule before anything of it runs. No second
//   window opens: new-window decisions are ignored and "create" makes none.
// - Messages: WebKitGTK tells the receiver of a script message nothing about
//   the frame that posted it, and the webview library's handler (webview 0.12,
//   gtk_webkit_engine, "script-message-received::__webview__") is registered
//   in the pages' own script world, where every frame of every page the window
//   shows can post to it. The gate takes that name out of the pages' world.
//   The only way in is a handler of the gate's own, registered in an isolated
//   script world in which no script of any page runs - only the gate's
//   forwarder, injected into the top frame alone. The window's page hands it
//   each call as a DOM event (message_gate_page_script()); the forwarder
//   passes the call on with what the top frame's document really is, its URL
//   and its security origin, read in the isolated world where no page can
//   change them. In this process the gate gives a message to the library's own
//   handler only when message_may_reach_native() allows that sender, as on
//   macOS.

namespace singularity::platform {
namespace {

// The webview library's script message handler (gtk_webkit_engine).
constexpr const char* libraryHandlerName = "__webview__";
// The gate's handler, the isolated world it lives in, and the DOM event that
// carries the page's calls to it (message_gate_page_script()).
constexpr const char* gateHandlerName = "singularityNativeBridge";
constexpr const char* gateWorldName = "singularity-native-bridge";
// What a content manager and a web view keep of the gate and the policy.
constexpr const char* gateScriptKey = "singularity-native-bridge-forwarder";
constexpr const char* policyKey = "singularity-navigation-policy";

// In the gate's isolated world of the top frame only. A page's scripts never run
// here and cannot reach this world's message handlers, its JSON or its
// location; they can only dispatch the event on their own document.
const char* const forwarderScript = R"JS((function () {
	'use strict';
	var handlers = window.webkit && window.webkit.messageHandlers;
	var bridge = handlers ? handlers.singularityNativeBridge : undefined;
	if (bridge === undefined || window !== window.top) return;
	var parse = JSON.parse;
	var stringify = JSON.stringify;
	document.addEventListener('singularity-native-bridge', function (event) {
		if (typeof event.detail !== 'string') return;
		var message;
		try {
			message = parse(event.detail);
		} catch (error) {
			return;
		}
		if (message === null || typeof message !== 'object' || Array.isArray(message)) return;
		message.singularitySender = {
			url: String(location.href),
			origin: String(self.origin),
			mainFrame: window === window.top
		};
		bridge.postMessage(stringify(message));
	});
})();
)JS";

// In the page's own world, top frame only, right after the library's script:
// its calls go to the gate instead of the library's handler, which the gate
// has taken out of this world.
const char* const pageScript = R"JS((function () {
	'use strict';
	var bridge = window.__webview__;
	if (bridge === undefined) return;
	var target = document;
	var BridgeEvent = window.CustomEvent;
	bridge.post = function (message) {
		target.dispatchEvent(new BridgeEvent('singularity-native-bridge', { detail: String(message) }));
	};
})();
)JS";

// The prefixes the window was loaded from; one window per process.
std::mutex& prefixMutex() {
	static std::mutex mutex;
	return mutex;
}

std::vector<std::string>& windowPrefixes() {
	static std::vector<std::string> prefixes;
	return prefixes;
}

int hexValue(char digit) {
	if (digit >= '0' && digit <= '9') return digit - '0';
	if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
	return -1;
}

// A file URL as Linux names the file: percent escapes decoded. Singularity
// writes its own file URLs (AppPaths file_url) with escapes WebKit may spell
// differently; compared decoded, the window's own pages match their folder and
// nothing else does. A decoded ".." still climbs, and window_may_show() still
// refuses it. Other URLs are compared as WebKit reports them.
std::string comparable(const std::string& url) {
	if (url.compare(0, 7, "file://") != 0) return url;
	std::string decoded;
	decoded.reserve(url.size());
	for (std::size_t at = 0; at < url.size(); ++at) {
		const bool isEscape = url[at] == '%' && at + 2 < url.size() && hexValue(url[at + 1]) >= 0
			&& hexValue(url[at + 2]) >= 0;
		if (isEscape == true) {
			decoded.push_back(static_cast<char>(hexValue(url[at + 1]) * 16 + hexValue(url[at + 2])));
			at += 2;
		} else {
			decoded.push_back(url[at]);
		}
	}
	return decoded;
}

std::vector<std::string> rememberedPrefixes() {
	const std::lock_guard<std::mutex> lock(prefixMutex());
	std::vector<std::string> prefixes;
	for (const std::string& prefix : windowPrefixes()) prefixes.push_back(comparable(prefix));
	return prefixes;
}

// Said once a process per kind, and never with what was refused: a page
// retrying in a loop must not fill the log.
void reportOnce(std::atomic<bool>& reported, const std::string& what) {
	if (reported.exchange(true) == true) return;
	std::cerr << "Singularity " << what << ". Later ones are not reported.\n";
}

std::atomic<bool> refusedNavigation{false};
std::atomic<bool> refusedPage{false};
std::atomic<bool> refusedWindow{false};
std::atomic<bool> refusedMessage{false};

// The scheme, host and port of a serialised origin ("http://127.0.0.1:5173",
// "file://"), as the gate compares origins: port 0 for a file origin and for a
// scheme's default port. An opaque origin ("null") gives no scheme at all.
void originParts(const std::string& origin, std::string& protocol, std::string& host, int& port) {
	protocol.clear();
	host.clear();
	port = 0;
	const std::size_t colon = origin.find(':');
	if (colon == std::string::npos || origin.compare(colon, 3, "://") != 0) return;
	protocol = origin.substr(0, colon);
	for (char& character : protocol)
		if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
	if (protocol == "file") return;
	std::string authority = origin.substr(colon + 3, origin.find_first_of("/?#", colon + 3) - (colon + 3));
	const std::size_t portColon = authority.rfind(':');
	const std::size_t bracket = authority.rfind(']');
	const bool hasPort = portColon != std::string::npos && (bracket == std::string::npos || portColon > bracket);
	host = hasPort == true ? authority.substr(0, portColon) : authority;
	if (hasPort == false) return;
	const std::string digits = authority.substr(portColon + 1);
	const bool isNumber = digits.empty() == false && digits.size() <= 5
		&& std::all_of(digits.begin(), digits.end(), [](char digit) { return digit >= '0' && digit <= '9'; });
	port = isNumber == true ? std::stoi(digits) : -1;
	if ((protocol == "http" && port == 80) || (protocol == "https" && port == 443)) port = 0;
}

// "script-message-received::singularityNativeBridge": what the forwarder passed
// on. Allowed messages go to the library's own handler, exactly as it would
// have received them, with the sender the forwarder added; everything else is
// dropped before the library reads it.
void passMessage(WebKitUserContentManager* manager, WebKitJavascriptResult* result, gpointer) {

	JSCValue* value = result == nullptr ? nullptr : webkit_javascript_result_get_js_value(result);
	if (value == nullptr || jsc_value_is_string(value) == FALSE) {
		reportOnce(refusedMessage, "refused a message to its native bridge that was not text");
		return;
	}
	char* text = jsc_value_to_string(value);
	const std::string message = text == nullptr ? std::string() : std::string(text);
	g_free(text);

	const nlohmann::json parsed = nlohmann::json::parse(message, nullptr, false);
	const nlohmann::json sender = parsed.is_object() == true && parsed.contains("singularitySender") == true
		? parsed["singularitySender"]
		: nlohmann::json();
	const bool isDescribed = sender.is_object() == true && sender.contains("url") == true
		&& sender["url"].is_string() == true && sender.contains("origin") == true
		&& sender["origin"].is_string() == true && sender.contains("mainFrame") == true
		&& sender["mainFrame"].is_boolean() == true;
	if (isDescribed == false) {
		reportOnce(refusedMessage, "refused a message to its native bridge whose sender was not described");
		return;
	}

	const std::string url = sender["url"].get<std::string>();
	const bool isMainFrame = sender["mainFrame"].get<bool>();
	std::string origin = sender["origin"].get<std::string>();
	// WebKitGTK gives every file document the opaque origin "null"
	// (window_navigation_webkitgtk_probe shows it): a file page is told apart by
	// its URL alone, as on Windows. A file document has no response headers
	// that could sandbox it.
	if (origin == "null" && comparable(url).compare(0, 7, "file://") == 0) origin = "file://";
	std::string protocol;
	std::string host;
	int port = 0;
	originParts(origin, protocol, host, port);
	if (message_may_reach_native(isMainFrame, comparable(url), protocol, host, port, rememberedPrefixes()) == false) {
		reportOnce(refusedMessage, "refused a message to its native bridge from a page that is not the window's own");
		return;
	}

	const std::string libraryDetail = std::string("script-message-received::") + libraryHandlerName;
	g_signal_emit_by_name(manager, libraryDetail.c_str(), result);

}

// The library receives page messages through its one handler for
// "script-message-received::__webview__", and nothing else listens to the
// manager's messages: a handler for every name, one already on the gate's
// name, or one for messages with replies would take messages the gate never
// sees.
bool libraryIsAsExpected(WebKitUserContentManager* manager) {
	const guint received = g_signal_lookup("script-message-received", WEBKIT_TYPE_USER_CONTENT_MANAGER);
	const guint withReply = g_signal_lookup("script-message-with-reply-received", WEBKIT_TYPE_USER_CONTENT_MANAGER);
	if (received == 0) return false;
	const GSignalMatchType byDetail = static_cast<GSignalMatchType>(G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DETAIL);
	const bool hasLibraryHandler =
		g_signal_has_handler_pending(manager, received, g_quark_from_static_string(libraryHandlerName), TRUE) == TRUE;
	const bool hasHandlerForAll = g_signal_handler_find(manager, byDetail, received, 0, nullptr, nullptr, nullptr) != 0;
	const bool hasHandlerOnGate = g_signal_handler_find(manager, byDetail, received,
		g_quark_from_static_string(gateHandlerName), nullptr, nullptr, nullptr) != 0;
	const bool hasReplyHandler = withReply != 0 && g_signal_has_handler_pending(manager, withReply, 0, TRUE) == TRUE;
	return hasLibraryHandler == true && hasHandlerForAll == false && hasHandlerOnGate == false
		&& hasReplyHandler == false;
}

void unrefScript(gpointer script) {
	webkit_user_script_unref(static_cast<WebKitUserScript*>(script));
}

std::string uriOf(WebKitURIRequest* request) {
	const gchar* uri = request == nullptr ? nullptr : webkit_uri_request_get_uri(request);
	return uri == nullptr ? std::string() : std::string(uri);
}

// "decide-policy": every navigation of the window, server redirects included,
// and every page's response before the page runs.
gboolean decidePolicy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {

	if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
		webkit_policy_decision_ignore(decision);
		reportOnce(refusedWindow, "kept its page from opening a second window");
		return TRUE;
	}

	if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
		WebKitNavigationAction* action =
			webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
		const std::string url = uriOf(action == nullptr ? nullptr : webkit_navigation_action_get_request(action));
		// Which frame this is for is not known yet: what any frame may show
		// goes on, and a main-frame page is held to its own rule at its response.
		if (window_may_show(comparable(url), rememberedPrefixes(), false) == true) return FALSE;
		webkit_policy_decision_ignore(decision);
		reportOnce(refusedNavigation, "kept its window from showing a page that is not its own");
		return TRUE;
	}

	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
		WebKitResponsePolicyDecision* response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
		const bool isMainFrame = webkit_response_policy_decision_is_main_frame_main_resource(response) == TRUE;
		WebKitURIResponse* answer = webkit_response_policy_decision_get_response(response);
		const gchar* answered = answer == nullptr ? nullptr : webkit_uri_response_get_uri(answer);
		const std::string requested = uriOf(webkit_response_policy_decision_get_request(response));
		const std::vector<std::string> prefixes = rememberedPrefixes();
		const bool isShown = window_may_show(comparable(requested), prefixes, isMainFrame) == true
			&& (answered == nullptr || window_may_show(comparable(answered), prefixes, isMainFrame) == true);
		if (isShown == true) return FALSE;
		webkit_policy_decision_ignore(decision);
		reportOnce(refusedPage, "kept its window from showing a page that is not its own");
		return TRUE;
	}

	return FALSE;

}

// "create": window.open and target="_blank" get no second window, which would
// get the bridge too. Stopping the emission keeps any handler connected after
// this one from making one.
GtkWidget* refuseNewWindow(WebKitWebView* webView, WebKitNavigationAction*, gpointer) {
	reportOnce(refusedWindow, "kept its page from opening a second window");
	g_signal_stop_emission_by_name(webView, "create");
	return nullptr;
}

} // namespace

void allow_window_prefix(const std::string &prefix) {
	if (prefix.empty() == true) return;
	const std::lock_guard<std::mutex> lock(prefixMutex());
	std::vector<std::string>& prefixes = windowPrefixes();
	if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) prefixes.push_back(prefix);
}

void install_navigation_policy(void *web_view) {
	if (web_view == nullptr || WEBKIT_IS_WEB_VIEW(web_view) == FALSE)
		throw std::runtime_error("The window's navigation policy could not be installed, so no page is loaded");
	GObject* view = G_OBJECT(web_view);
	if (g_object_get_data(view, policyKey) != nullptr) return;
	// The policy decides first: a handler connected before it could let a page
	// through, or open a window, before the policy is asked.
	const guint decide = g_signal_lookup("decide-policy", WEBKIT_TYPE_WEB_VIEW);
	const guint create = g_signal_lookup("create", WEBKIT_TYPE_WEB_VIEW);
	if (decide == 0 || create == 0 || g_signal_has_handler_pending(view, decide, 0, TRUE) == TRUE
		|| g_signal_has_handler_pending(view, create, 0, TRUE) == TRUE) {
		std::cerr << "Singularity cannot install its navigation policy: something else already decides the "
					 "window's navigations or opens its windows. Review Platform/WindowNavigation.\n";
		throw std::runtime_error("The window's navigation policy could not be installed, so no page is loaded");
	}
	g_signal_connect(view, "decide-policy", G_CALLBACK(&decidePolicy), nullptr);
	g_signal_connect(view, "create", G_CALLBACK(&refuseNewWindow), nullptr);
	g_object_set_data(view, policyKey, GINT_TO_POINTER(1));
}

bool install_message_gate(void *web_view) {
	static std::mutex mutex;
	const std::lock_guard<std::mutex> lock(mutex);
	if (web_view == nullptr || WEBKIT_IS_WEB_VIEW(web_view) == FALSE) {
		std::cerr << "Singularity cannot guard its native bridge: the window has no WebKitWebView.\n";
		return false;
	}
	WebKitUserContentManager* manager = webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(web_view));
	WebKitUserScript* forwarder = static_cast<WebKitUserScript*>(g_object_get_data(G_OBJECT(manager), gateScriptKey));

	if (forwarder == nullptr) {
		// The gate moves one handler. A library that receives messages some other
		// way as well would leave every frame free to call every native method:
		// say so, and let the window refuse to load a page, rather than run
		// unguarded.
		if (libraryIsAsExpected(manager) == false) {
			std::cerr << "Singularity cannot guard its native bridge: the webview library no longer receives page "
						 "messages through WebKitUserContentManager::script-message-received::"
					  << libraryHandlerName
					  << " alone. Review Platform/WindowNavigation before using this webview library.\n";
			return false;
		}
		webkit_user_content_manager_unregister_script_message_handler(manager, libraryHandlerName);
		if (webkit_user_content_manager_register_script_message_handler_in_world(manager, gateHandlerName, gateWorldName)
			== FALSE) {
			std::cerr << "Singularity cannot guard its native bridge: its message handler could not be registered.\n";
			return false;
		}
		const std::string gateDetail = std::string("script-message-received::") + gateHandlerName;
		g_signal_connect(manager, gateDetail.c_str(), G_CALLBACK(&passMessage), nullptr);
		forwarder = webkit_user_script_new_for_world(forwarderScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, gateWorldName, nullptr, nullptr);
		g_object_set_data_full(G_OBJECT(manager), gateScriptKey, forwarder, &unrefScript);
	}

	// The library's bind() removes every user script and adds back only its own:
	// the forwarder goes back in, once.
	webkit_user_content_manager_remove_script(manager, forwarder);
	webkit_user_content_manager_add_script(manager, forwarder);
	return true;
}

const char *message_gate_page_script() {
	return pageScript;
}

} // namespace singularity::platform
