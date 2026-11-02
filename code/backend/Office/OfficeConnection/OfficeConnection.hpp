#pragma once

#include "Office/OfficeClients/OfficeClients.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// The Office connection (D-105): Singularity's second carrier of the Native
// API (docs/NATIVE_TRANSPORT.md), for its panes in Word, Excel and Outlook.
//
// It listens on this computer's loopback only (127.0.0.1), over TLS with the
// certificate of Office/OfficeCertificate, and answers:
//   - the panes' own files, from the packaged frontend's office folder - code
//     and pictures only, never data;
//   - a pane's pairing (POST /api/pair/start, /api/pair/finish), which Singularity's
//     window asks the person to allow (Office/OfficeClients);
//   - a paired pane's Native API call, POST /api/call {method, params}, for
//     the methods its kind of client needs and no other (may_call), through
//     the bridge's one handler table and error envelope (TRN-4);
//   - a paired pane's events, POST /api/events, streamed as server-sent events.
//
// Reaching the port proves nothing, so nothing that holds data is answered
// without a paired client's token, a moment within two minutes and a nonce
// not used before (TRN-5). A call must name this server as its Host (no page
// can rebind a name of its own to this computer), and a browser's call must
// come from the pane's own origin. A Matter a call names must be one open on
// this computer. The security review is in docs/OFFICE.md.
namespace singularity::office {

// How the connection reaches the Native API: the bridge, through
// NativeBridge::answer_carried_call and its event listeners.
struct Carrier {
	// A call by its wire name and JSON params: {0, result} or {1, NativeError}.
	std::function<std::pair<int, std::string>(const std::string &method, const std::string &json_params)> call;
	// Every event the window receives, as its name and JSON payload; returns a
	// handle to stop.
	std::function<int(std::function<void(const std::string &name, const std::string &json_payload)>)> subscribe;
	std::function<void(int)> unsubscribe;
	// The window's own page script (window.nativeApp), which a pane loads
	// after the connection's transport.
	std::string page_script;
};

// What the connection asks before it answers.
struct Gate {
	std::shared_ptr<OfficeClients> clients;
	// Whether an id names a Matter open on this computer.
	std::function<bool(const std::string &matter_id)> is_matter_here;
	// A pane asked to pair: Singularity's window asks the person.
	std::function<void()> pairing_requested;
	// Where each answer is recorded - its moment, method, path, status and
	// the client's name, never a body, a query or a token; empty for none.
	std::filesystem::path access_log;
};

// ---- The protocol, as plain functions -------------------------------------------

struct HttpRequest {
	std::string method;
	std::string target;
	// The target's path, without its query.
	std::string path;
	// Header names in lower case.
	std::map<std::string, std::string> headers;
	std::string body;
};

enum class ParseState { incomplete, complete, invalid, too_large };

// Reads one request from the front of `bytes`. On `complete`, `consumed` is
// how many bytes it took.
ParseState parse_request(const std::string &bytes, HttpRequest &request, std::size_t &consumed, std::size_t max_body);

// Why the request must be refused, or nothing when it may be answered.
std::optional<std::string> refusal_of(const HttpRequest &request, int port);

// Whether any kind of client may call the method (OfficeClients' may_call).
bool is_carried_method(const std::string &method);

// Whether an event reaches a pane of this kind: what changed in a Matter and
// its colleagues' work reaches every pane; a place to show ("office.reveal")
// only the panes of documents, Word's and Excel's; the connection's own
// events (a pairing asked for) none.
bool pane_event_reaches(const std::string &name, ClientKind kind);

// The Matter a call's params name (projectId or matterId), if any.
std::optional<std::string> matter_named_by(const std::string &json_params);

// The file under `root` a pane path names ("/office/..."), or nothing when it
// names none or reaches outside.
std::optional<std::filesystem::path> static_file_for(const std::filesystem::path &root, const std::string &path);

std::string content_type_of(const std::filesystem::path &file);

// The transport a pane loads before the page script: window.__nativeInvoke
// over POST /api/call with the pane's token, its pairing (window.__lmOffice),
// and the events from /api/events delivered as the window delivers them.
std::string pane_transport_script();

// ---- The server --------------------------------------------------------------------

class OfficeServer final {
public:
	OfficeServer(Carrier carrier, std::filesystem::path content_root, Gate gate);
	~OfficeServer();

	OfficeServer(const OfficeServer &) = delete;
	OfficeServer &operator=(const OfficeServer &) = delete;

	// Listens on 127.0.0.1:`port` with the certificate. Throws
	// std::runtime_error when it cannot, as when the port is taken.
	void start(int port, const std::string &certificate_pem, const std::string &key_pem);

	// Stops listening and ends every connection; returns once they ended.
	void stop();

	bool is_listening() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace singularity::office
