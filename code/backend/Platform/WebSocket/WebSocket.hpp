#pragma once

#include "Platform/HttpClient/HttpClient.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform {

// A WebSocket over TLS (RFC 6455) to a Legal Machines address: what the direct
// link's relay is reached by, on port 443 as a web page is, so a network that
// lets nothing else through still lets it. Each system's own client carries it:
// NSURLSession on macOS, WinHTTP on Windows, libcurl on Linux.
//
// One thread may send while another receives; close() may come from any
// thread and ends a receive that waits.
class WebSocket {
public:
	struct Message {
		bool isText = false;
		std::string data;
	};

	virtual ~WebSocket() = default;
	// Sends one binary message. Throws HttpTransportError once the connection
	// is closed or broken.
	virtual void send_binary(const std::string& data) = 0;
	// Sends one text message, UTF-8; the same as send_binary otherwise.
	virtual void send_text(const std::string& text) = 0;
	// The next whole message, waiting up to `wait`; nothing when none came in
	// time. Throws HttpTransportError once the connection is closed or broken.
	virtual std::optional<Message> receive(std::chrono::milliseconds wait) = 0;
	virtual void close() = 0;
};

// Opens a WebSocket to `url` ("https://host/path", reached as wss://), with
// these headers on its opening request. Throws HttpTransportError when it
// cannot be opened; on a system whose client opens it in the background, a
// failure shows at the first receive instead.
std::unique_ptr<WebSocket> open_websocket(const std::string& url, const std::vector<HttpHeader>& headers,
	int timeout_seconds = 15);

} // namespace singularity::platform
