#include "Platform/WebSocket/WebSocket.hpp"
#include "Platform/WebSocket/WebSocketInbox.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winhttp.h>

#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Windows' own WebSocket client, WinHTTP's (Windows 8 and later): the
// system's TLS, proxies and trust, as the HTTP client
// (Platform/HttpClient/HttpClientWindows.cpp) keeps them. A thread of its own
// waits for what arrives; WinHTTP lets one send and one receive run at once.

namespace singularity::platform {
namespace {

std::wstring wide(const std::string &value) {
	if (value.empty()) return std::wstring();
	const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
	std::wstring result(static_cast<std::size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
	return result;
}

std::string failure(const char *what) {
	return std::string(what) + " (WinHTTP " + std::to_string(GetLastError()) + ")";
}

struct Handle {
	HINTERNET value = nullptr;
	explicit Handle(HINTERNET handle = nullptr) : value(handle) {}
	~Handle() {
		if (value != nullptr) WinHttpCloseHandle(value);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
};

class WebSocketWindows final : public WebSocket {
public:
	WebSocketWindows(HINTERNET session, HINTERNET connection, HINTERNET socket)
		: session_(session), connection_(connection), socket_(socket), inbox_(std::make_shared<WebSocketInbox>()) {
		reader_ = std::thread([this] { read(); });
	}
	~WebSocketWindows() override {
		close();
		if (reader_.joinable() == true) reader_.join();
		const std::lock_guard<std::mutex> lock(mutex_);
		if (socket_ != nullptr) WinHttpCloseHandle(socket_);
		WinHttpCloseHandle(connection_);
		WinHttpCloseHandle(session_);
	}

	void send_binary(const std::string &data) override { send(WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, data); }
	void send_text(const std::string &text) override { send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text); }

	void send(WINHTTP_WEB_SOCKET_BUFFER_TYPE type, const std::string &data) {
		const std::lock_guard<std::mutex> lock(mutex_);
		if (isClosed_ == true || inbox_->is_ended() == true) throw HttpTransportError("The connection closed.", false);
		const DWORD result = WinHttpWebSocketSend(socket_, type, const_cast<char *>(data.data()), static_cast<DWORD>(data.size()));
		if (result != NO_ERROR) {
			inbox_->end("The connection broke.");
			throw HttpTransportError("The connection broke.", result == ERROR_WINHTTP_TIMEOUT);
		}
	}

	std::optional<Message> receive(std::chrono::milliseconds wait) override { return inbox_->take(wait); }

	void close() override {
		inbox_->end("The connection closed.");
		HINTERNET closing = nullptr;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			if (isClosed_ == true) return;
			isClosed_ = true;
			closing = socket_;
			socket_ = nullptr;
		}
		// Closing the handle ends the receive that waits on it.
		WinHttpCloseHandle(closing);
	}

private:
	void read() {
		std::string message;
		char buffer[16384];
		for (;;) {
			HINTERNET socket = nullptr;
			{
				const std::lock_guard<std::mutex> lock(mutex_);
				if (isClosed_ == true) return;
				socket = socket_;
			}
			DWORD read = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
			const DWORD result = WinHttpWebSocketReceive(socket, buffer, static_cast<DWORD>(sizeof buffer), &read, &type);
			if (result != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
				inbox_->end("The connection closed.");
				return;
			}
			message.append(buffer, read);
			if (message.size() > (1u << 20)) {
				inbox_->end("A message was too large.");
				return;
			}
			if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
				inbox_->put(Message{type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, std::move(message)});
				message.clear();
			}
		}
	}

	HINTERNET session_;
	HINTERNET connection_;
	HINTERNET socket_;
	std::shared_ptr<WebSocketInbox> inbox_;
	std::mutex mutex_;
	bool isClosed_ = false;
	std::thread reader_;
};

} // namespace

std::unique_ptr<WebSocket> open_websocket(const std::string &url, const std::vector<HttpHeader> &headers, int timeout_seconds) {
	const std::wstring address = wide(url.rfind("wss://", 0) == 0 ? "https://" + url.substr(6) : url);
	wchar_t host[256]{};
	wchar_t path[2048]{};
	URL_COMPONENTS components{};
	components.dwStructSize = sizeof(components);
	components.lpszHostName = host;
	components.dwHostNameLength = static_cast<DWORD>(std::size(host));
	components.lpszUrlPath = path;
	components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
	components.dwSchemeLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(address.c_str(), 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS) {
		throw HttpTransportError("Only a secure address can be opened.", false);
	}
	Handle session(WinHttpOpen(L"Singularity", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (session.value == nullptr) throw HttpTransportError(failure("Unable to start an HTTP session"), false);
	const int timeout_ms = timeout_seconds * 1000;
	// No limit on waiting for the next message once open: the relay's own
	// sign of life (RelayClient) tells a dead connection.
	WinHttpSetTimeouts(session.value, timeout_ms, timeout_ms, timeout_ms, 0);
	Handle connection(WinHttpConnect(session.value, components.lpszHostName, components.nPort, 0));
	if (connection.value == nullptr) throw HttpTransportError(failure("Unable to connect"), false);
	Handle request(WinHttpOpenRequest(connection.value, L"GET", components.lpszUrlPath, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (request.value == nullptr) throw HttpTransportError(failure("Unable to open the request"), false);
	DWORD responseTimeout = static_cast<DWORD>(timeout_ms);
	WinHttpSetOption(request.value, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &responseTimeout, sizeof responseTimeout);
	if (!WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
		throw HttpTransportError(failure("This Windows cannot open a WebSocket"), false);
	}
	std::wstring lines;
	for (const HttpHeader &header : headers) lines += wide(header.name) + L": " + wide(header.value) + L"\r\n";
	if (!WinHttpSendRequest(request.value, lines.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : lines.c_str(),
			lines.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			|| !WinHttpReceiveResponse(request.value, nullptr)) {
		const bool timedOut = GetLastError() == ERROR_WINHTTP_TIMEOUT;
		throw HttpTransportError(failure("The relay could not be reached"), timedOut);
	}
	DWORD status = 0;
	DWORD size = sizeof status;
	WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
		&status, &size, WINHTTP_NO_HEADER_INDEX);
	if (status != 101) throw HttpTransportError("The relay answered " + std::to_string(status) + ".", false);
	HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request.value, 0);
	if (socket == nullptr) throw HttpTransportError(failure("The connection could not be upgraded"), false);
	HINTERNET kept = session.value;
	HINTERNET keptConnection = connection.value;
	session.value = nullptr;
	connection.value = nullptr;
	return std::make_unique<WebSocketWindows>(kept, keptConnection, socket);
}

} // namespace singularity::platform
