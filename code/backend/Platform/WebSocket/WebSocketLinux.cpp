#include "Platform/WebSocket/WebSocket.hpp"
#include "Platform/WebSocket/WebSocketFrames.hpp"
#include "Platform/WebSocket/WebSocketInbox.hpp"

#ifdef SINGULARITY_HAVE_CURL
#include <curl/curl.h>
#include <poll.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

// Linux has no system WebSocket client: libcurl's TLS connection carries it
// (CURLOPT_CONNECT_ONLY, the same libcurl and trust as the HTTP client,
// Platform/HttpClient/HttpClientLinux.cpp), and RFC 6455's opening handshake
// and frames are spoken over it here (Platform/WebSocket/WebSocketFrames).
// curl_easy_send and curl_easy_recv never run at once: one lock covers both,
// held only while bytes move.

namespace singularity::platform {

#ifdef SINGULARITY_HAVE_CURL
namespace {

namespace frames = websocket_frames;

std::array<std::uint8_t, 4> freshMask() {
	thread_local std::mt19937 generator(std::random_device{}());
	std::array<std::uint8_t, 4> mask{};
	for (std::uint8_t &byte : mask) byte = static_cast<std::uint8_t>(generator() & 0xFF);
	return mask;
}

std::string freshKey() {
	thread_local std::mt19937 generator(std::random_device{}());
	static const char *const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string key;
	// Sixteen random bytes in base64: 22 characters and "==".
	for (int index = 0; index < 21; ++index) key += kAlphabet[generator() % 64];
	key += kAlphabet[(generator() % 4) * 16];
	return key + "==";
}

std::string lower(std::string text) {
	for (char &character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return text;
}

// The value of one header in an answer's head, or empty.
std::string headerOf(const std::string &head, const std::string &name) {
	const std::string wanted = "\r\n" + lower(name) + ":";
	const std::string text = lower(head);
	const std::size_t at = text.find(wanted);
	if (at == std::string::npos) return std::string();
	std::size_t begin = at + wanted.size();
	const std::size_t end = head.find("\r\n", begin);
	while (begin < end && (head[begin] == ' ' || head[begin] == '\t')) ++begin;
	std::size_t last = end;
	while (last > begin && (head[last - 1] == ' ' || head[last - 1] == '\t')) --last;
	return head.substr(begin, last - begin);
}

class WebSocketLinux final : public WebSocket {
public:
	WebSocketLinux(CURL *curl, curl_socket_t socket, std::string leftover)
		: curl_(curl), socket_(socket), inbox_(std::make_shared<WebSocketInbox>()) {
		reader_.feed(leftover.data(), leftover.size());
		thread_ = std::thread([this] { read(); });
	}
	~WebSocketLinux() override {
		close();
		if (thread_.joinable() == true) thread_.join();
		curl_easy_cleanup(curl_);
	}

	void send_binary(const std::string &data) override { send(frames::Opcode::Binary, data); }
	void send_text(const std::string &text) override { send(frames::Opcode::Text, text); }

	void send(frames::Opcode opcode, const std::string &data) {
		if (inbox_->is_ended() == true) throw HttpTransportError("The connection closed.", false);
		if (sendFrame(opcode, data) == false) {
			inbox_->end("The connection broke.");
			throw HttpTransportError("The connection broke.", false);
		}
	}

	std::optional<Message> receive(std::chrono::milliseconds wait) override { return inbox_->take(wait); }

	void close() override {
		if (isClosing_.exchange(true) == true) return;
		static_cast<void>(sendFrame(frames::Opcode::Close, std::string("\x03\xE8", 2)));
		inbox_->end("The connection closed.");
	}

private:
	// Writes a whole frame, waiting for the socket while it is full; false when
	// it broke or stayed full for fifteen seconds.
	bool sendFrame(frames::Opcode opcode, const std::string &payload) {
		const std::string frame = frames::client_frame(opcode, payload, freshMask());
		const std::lock_guard<std::mutex> lock(mutex_);
		std::size_t at = 0;
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (at < frame.size()) {
			std::size_t sent = 0;
			const CURLcode result = curl_easy_send(curl_, frame.data() + at, frame.size() - at, &sent);
			if (result == CURLE_OK) {
				at += sent;
				continue;
			}
			if (result != CURLE_AGAIN || std::chrono::steady_clock::now() >= until) return false;
			pollfd ready{socket_, POLLOUT, 0};
			static_cast<void>(poll(&ready, 1, 100));
		}
		return true;
	}

	void read() {
		char buffer[16384];
		while (isClosing_.load() == false) {
			pollfd ready{socket_, POLLIN, 0};
			const int polled = poll(&ready, 1, 250);
			if (polled < 0) break;
			{
				const std::lock_guard<std::mutex> lock(mutex_);
				for (;;) {
					std::size_t got = 0;
					const CURLcode result = curl_easy_recv(curl_, buffer, sizeof buffer, &got);
					if (result == CURLE_AGAIN) break;
					if (result != CURLE_OK || got == 0) {
						inbox_->end("The connection closed.");
						return;
					}
					reader_.feed(buffer, got);
				}
			}
			while (std::optional<frames::Reader::Message> message = reader_.next()) {
				switch (message->opcode) {
					case frames::Opcode::Ping: static_cast<void>(sendFrame(frames::Opcode::Pong, message->data)); break;
					case frames::Opcode::Close: inbox_->end("The connection closed."); return;
					case frames::Opcode::Text: inbox_->put(Message{true, std::move(message->data)}); break;
					case frames::Opcode::Binary: inbox_->put(Message{false, std::move(message->data)}); break;
					default: break;
				}
			}
			if (reader_.is_broken() == true) {
				inbox_->end("The connection broke.");
				return;
			}
		}
		inbox_->end("The connection closed.");
	}

	CURL *curl_;
	curl_socket_t socket_;
	std::shared_ptr<WebSocketInbox> inbox_;
	frames::Reader reader_;
	std::mutex mutex_;
	std::atomic<bool> isClosing_{false};
	std::thread thread_;
};

} // namespace

std::unique_ptr<WebSocket> open_websocket(const std::string &url, const std::vector<HttpHeader> &headers, int timeout_seconds) {
	const std::string address = url.rfind("wss://", 0) == 0 ? "https://" + url.substr(6) : url;
	if (address.rfind("https://", 0) != 0) throw HttpTransportError("Only a secure address can be opened.", false);
	const std::size_t pathAt = address.find('/', 8);
	const std::string host = address.substr(8, pathAt == std::string::npos ? std::string::npos : pathAt - 8);
	const std::string path = pathAt == std::string::npos ? "/" : address.substr(pathAt);
	CURL *curl = curl_easy_init();
	if (curl == nullptr) throw HttpTransportError("Unable to start an HTTP session.", false);
	auto fail = [curl](const std::string &message, bool timedOut) {
		curl_easy_cleanup(curl);
		throw HttpTransportError(message, timedOut);
	};
	curl_easy_setopt(curl, CURLOPT_URL, address.c_str());
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout_seconds));
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Singularity");
	const CURLcode connected = curl_easy_perform(curl);
	if (connected != CURLE_OK) fail(curl_easy_strerror(connected), connected == CURLE_OPERATION_TIMEDOUT);
	curl_socket_t socket = CURL_SOCKET_BAD;
	if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD) {
		fail("The connection could not be read.", false);
	}

	const std::string key = freshKey();
	std::string opening = "GET " + path + " HTTP/1.1\r\nHost: " + host
		+ "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key
		+ "\r\nUser-Agent: Singularity\r\n";
	for (const HttpHeader &header : headers) opening += header.name + ": " + header.value + "\r\n";
	opening += "\r\n";
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
	for (std::size_t at = 0; at < opening.size();) {
		std::size_t sent = 0;
		const CURLcode result = curl_easy_send(curl, opening.data() + at, opening.size() - at, &sent);
		if (result == CURLE_OK) {
			at += sent;
			continue;
		}
		if (result != CURLE_AGAIN) fail(curl_easy_strerror(result), false);
		if (std::chrono::steady_clock::now() >= until) fail("The relay did not answer in time.", true);
		pollfd ready{socket, POLLOUT, 0};
		static_cast<void>(poll(&ready, 1, 100));
	}
	std::string answer;
	std::size_t headEnd = std::string::npos;
	char buffer[4096];
	while (headEnd == std::string::npos) {
		if (std::chrono::steady_clock::now() >= until) fail("The relay did not answer in time.", true);
		if (answer.size() > 16384) fail("The relay's answer could not be read.", false);
		std::size_t got = 0;
		const CURLcode result = curl_easy_recv(curl, buffer, sizeof buffer, &got);
		if (result == CURLE_AGAIN) {
			pollfd ready{socket, POLLIN, 0};
			static_cast<void>(poll(&ready, 1, 100));
			continue;
		}
		if (result != CURLE_OK || got == 0) fail("The relay closed the connection.", false);
		answer.append(buffer, got);
		headEnd = answer.find("\r\n\r\n");
	}
	const std::string head = answer.substr(0, headEnd + 2);
	if (head.rfind("HTTP/1.1 101", 0) != 0) {
		fail("The relay answered " + head.substr(0, std::min<std::size_t>(head.find("\r\n"), 40)) + ".", false);
	}
	if (headerOf(head, "Sec-WebSocket-Accept") != websocket_frames::accept_for(key)) fail("The relay's answer did not match.", false);
	return std::make_unique<WebSocketLinux>(curl, socket, answer.substr(headEnd + 4));
}

#else

std::unique_ptr<WebSocket> open_websocket(const std::string &, const std::vector<HttpHeader> &, int) {
	throw HttpTransportError("This computer has no libcurl, so the relay cannot be reached.", false);
}

#endif

} // namespace singularity::platform
