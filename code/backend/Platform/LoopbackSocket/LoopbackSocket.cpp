#include "Platform/LoopbackSocket/LoopbackSocket.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>

// BSD sockets and Winsock share this whole implementation; they differ only
// in the spelling of the handful of calls isolated here.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
namespace {
using NativeSocket = SOCKET;
const NativeSocket kInvalidSocket = INVALID_SOCKET;
void close_native(NativeSocket socket) { ::closesocket(socket); }
using SocketLength = int;
bool set_receive_timeout(NativeSocket socket, std::chrono::seconds timeout) {
	const DWORD milliseconds = static_cast<DWORD>(std::max<long long>(1, timeout.count()) * 1000);
	return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&milliseconds),
		sizeof(milliseconds)) == 0;
}
int send_some(NativeSocket socket, const char *data, std::size_t size) {
	return ::send(socket, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
}
int receive_some(NativeSocket socket, char *data, std::size_t size) {
	return ::recv(socket, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
}
bool readable_within(NativeSocket socket, std::chrono::milliseconds limit) {
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(socket, &readable);
	timeval wait{static_cast<long>(limit.count() / 1000), static_cast<long>((limit.count() % 1000) * 1000)};
	return ::select(0, &readable, nullptr, nullptr, &wait) != 0;
}
} // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
namespace {
using NativeSocket = int;
const NativeSocket kInvalidSocket = -1;
void close_native(NativeSocket socket) { ::close(socket); }
using SocketLength = socklen_t;
bool set_receive_timeout(NativeSocket socket, std::chrono::seconds timeout) {
	struct timeval value {static_cast<time_t>(timeout.count()), 0};
	return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
}
ssize_t send_some(NativeSocket socket, const char *data, std::size_t size) {
	// MSG_NOSIGNAL where available: a peer that already closed must surface
	// as a failed send, never as a process-terminating SIGPIPE.
#if defined(MSG_NOSIGNAL)
	return ::send(socket, data, size, MSG_NOSIGNAL);
#else
	return ::send(socket, data, size, 0);
#endif
}
ssize_t receive_some(NativeSocket socket, char *data, std::size_t size) {
	for (;;) {
		const ssize_t count = ::recv(socket, data, size, 0);
		if (count < 0 && errno == EINTR) continue;
		return count;
	}
}
bool readable_within(NativeSocket socket, std::chrono::milliseconds limit) {
	pollfd watched{};
	watched.fd = socket;
	watched.events = POLLIN;
	for (;;) {
		const int ready = ::poll(&watched, 1, static_cast<int>(limit.count()));
		if (ready < 0 && errno == EINTR) continue;
		// An error or a hang-up is readable too: receive() then reports the end.
		return ready != 0;
	}
}
} // namespace
#endif

namespace singularity::platform {
namespace {

sockaddr_in loopback_address(unsigned short port) {
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return address;
}

NativeSocket native(std::uintptr_t socket) { return static_cast<NativeSocket>(socket); }

std::string lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	return value;
}

std::string trim(const std::string &value) {
	const auto first = value.find_first_not_of(" \t");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t");
	return value.substr(first, last - first + 1);
}

// One HTTP/1.1 response as it arrives: its head is read once complete, its
// body is complete by Content-Length, by the chunked terminator, or - with
// neither - only when the server closes the connection.
class ResponseReader final {
public:
	void append(const char *data, std::size_t count) {
		received_.append(data, count);
		if (head_end_ == std::string::npos) read_head();
	}

	bool head_complete() const { return head_end_ != std::string::npos; }

	// The decoded body, or std::nullopt while more of it is still to come.
	std::optional<std::string> body() const {
		if (!head_complete()) return std::nullopt;
		const std::size_t start = head_end_ + 4;
		if (chunked_) return decode_chunked(start);
		if (content_length_) {
			if (received_.size() - start < *content_length_) return std::nullopt;
			return received_.substr(start, static_cast<std::size_t>(*content_length_));
		}
		return std::nullopt;
	}

	// What arrived by the time the server closed the connection.
	std::optional<std::string> body_at_close() const {
		if (!head_complete()) return std::nullopt;
		if (chunked_ || content_length_) return body();
		return received_.substr(head_end_ + 4);
	}

	int status() const { return status_; }

private:
	void read_head() {
		head_end_ = received_.find("\r\n\r\n");
		if (head_end_ == std::string::npos) return;
		const std::string head = received_.substr(0, head_end_);
		std::size_t line_end = head.find("\r\n");
		const std::string status_line = head.substr(0, line_end);
		// "HTTP/1.1 200 OK"
		const auto space = status_line.find(' ');
		if (status_line.rfind("HTTP/", 0) == 0 && space != std::string::npos)
			status_ = std::atoi(status_line.c_str() + space + 1);
		while (line_end != std::string::npos) {
			const std::size_t next = head.find("\r\n", line_end + 2);
			const std::string line = head.substr(line_end + 2, next == std::string::npos ? std::string::npos
				: next - line_end - 2);
			line_end = next;
			const auto colon = line.find(':');
			if (colon == std::string::npos) continue;
			const std::string name = lowercase(trim(line.substr(0, colon)));
			const std::string value = trim(line.substr(colon + 1));
			if (name == "content-length") {
				char *end = nullptr;
				const unsigned long long length = std::strtoull(value.c_str(), &end, 10);
				if (end != value.c_str()) content_length_ = static_cast<std::uint64_t>(length);
			} else if (name == "transfer-encoding" && lowercase(value).find("chunked") != std::string::npos) {
				chunked_ = true;
			}
		}
	}

	std::optional<std::string> decode_chunked(std::size_t position) const {
		std::string body;
		for (;;) {
			const std::size_t size_end = received_.find("\r\n", position);
			if (size_end == std::string::npos) return std::nullopt;
			// A chunk's size may be followed by extensions after ';'.
			const std::string size_text = received_.substr(position, size_end - position);
			char *end = nullptr;
			const unsigned long long size = std::strtoull(size_text.c_str(), &end, 16);
			if (end == size_text.c_str()) return std::nullopt;
			position = size_end + 2;
			if (size == 0) {
				// The last chunk; the response ends at the blank line after any trailers.
				if (received_.size() < position + 2) return std::nullopt;
				if (received_.compare(position, 2, "\r\n") == 0) return body;
				return received_.find("\r\n\r\n", position) == std::string::npos ? std::nullopt
					: std::optional<std::string>(body);
			}
			if (received_.size() < position + size + 2) return std::nullopt;
			body.append(received_, position, static_cast<std::size_t>(size));
			position += static_cast<std::size_t>(size) + 2;
		}
	}

	std::string received_;
	std::size_t head_end_ = std::string::npos;
	int status_ = 0;
	std::optional<std::uint64_t> content_length_;
	bool chunked_ = false;
};

bool stopped(const LoopbackHttpRequest &request) {
	return request.stop != nullptr && request.stop->load();
}

} // namespace

bool ensure_socket_runtime() noexcept {
#if defined(_WIN32)
	static std::once_flag once;
	static bool available = false;
	std::call_once(once, [] {
		WSADATA data{};
		available = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	});
	return available;
#else
	return true;
#endif
}

LoopbackConnection::LoopbackConnection(std::uintptr_t socket) noexcept : socket_(socket) {}

LoopbackConnection::LoopbackConnection(LoopbackConnection &&other) noexcept : socket_(other.socket_) {
	other.socket_ = static_cast<std::uintptr_t>(kInvalidSocket);
}

LoopbackConnection &LoopbackConnection::operator=(LoopbackConnection &&other) noexcept {
	if (this != &other) {
		close();
		socket_ = other.socket_;
		other.socket_ = static_cast<std::uintptr_t>(kInvalidSocket);
	}
	return *this;
}

LoopbackConnection::~LoopbackConnection() { close(); }

std::optional<LoopbackConnection> LoopbackConnection::connect(unsigned short port,
		std::chrono::seconds receive_timeout) {
	if (!ensure_socket_runtime()) return std::nullopt;
	const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == kInvalidSocket) return std::nullopt;
	LoopbackConnection connection(static_cast<std::uintptr_t>(socket));
	(void)set_receive_timeout(socket, receive_timeout);
#if defined(SO_NOSIGPIPE)
	// Apple platforms have no MSG_NOSIGNAL; the socket-level option gives
	// send_some() the same no-SIGPIPE guarantee.
	const int no_sigpipe = 1;
	(void)::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
	const sockaddr_in address = loopback_address(port);
	if (::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) return std::nullopt;
	return connection;
}

bool LoopbackConnection::send_all(std::string_view bytes) {
	if (native(socket_) == kInvalidSocket) return false;
	std::size_t sent = 0;
	while (sent < bytes.size()) {
		const auto written = send_some(native(socket_), bytes.data() + sent, bytes.size() - sent);
		if (written <= 0) return false;
		sent += static_cast<std::size_t>(written);
	}
	return true;
}

bool LoopbackConnection::wait_readable(std::chrono::milliseconds limit) {
	if (native(socket_) == kInvalidSocket) return true;
	return readable_within(native(socket_), std::max(limit, std::chrono::milliseconds(0)));
}

std::size_t LoopbackConnection::receive(char *buffer, std::size_t capacity) {
	if (native(socket_) == kInvalidSocket || capacity == 0) return 0;
	const auto count = receive_some(native(socket_), buffer, capacity);
	return count > 0 ? static_cast<std::size_t>(count) : 0;
}

void LoopbackConnection::close() noexcept {
	if (native(socket_) != kInvalidSocket) {
		close_native(native(socket_));
		socket_ = static_cast<std::uintptr_t>(kInvalidSocket);
	}
}

std::optional<unsigned short> available_loopback_port() {
	if (!ensure_socket_runtime()) return std::nullopt;
	const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket == kInvalidSocket) return std::nullopt;
	sockaddr_in address = loopback_address(0);
	SocketLength size = sizeof(address);
	const bool reserved = ::bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
		::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &size) == 0;
	close_native(socket);
	if (!reserved) return std::nullopt;
	return ntohs(address.sin_port);
}

LoopbackHttpResponse loopback_http(unsigned short port, const LoopbackHttpRequest &request) {
	using Kind = LoopbackHttpError::Kind;
	// How often a wait looks at the stop flag: well inside the second the
	// server itself takes to notice a closed connection.
	constexpr std::chrono::milliseconds kSlice(100);
	if (stopped(request)) throw LoopbackHttpError(Kind::Stopped, "The request was stopped before it was sent.");
	auto connection = LoopbackConnection::connect(port, std::chrono::seconds(1));
	if (!connection) throw LoopbackHttpError(Kind::Unreachable, "The local inference runtime is not reachable.");

	std::string message = request.method + " " + request.target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
		std::to_string(port) + "\r\nConnection: close\r\n";
	for (const auto &[name, value] : request.headers) message += name + ": " + value + "\r\n";
	if (!request.body.empty() || request.method == "POST")
		message += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
	message += "\r\n";
	message += request.body;
	if (!connection->send_all(message))
		throw LoopbackHttpError(Kind::Failed, "The request could not be sent to the local inference runtime.");

	const auto deadline = std::chrono::steady_clock::now() + request.timeout;
	ResponseReader response;
	char buffer[16384];
	for (;;) {
		if (stopped(request)) {
			// Closing is the cancellation: the server notices the connection
			// is gone and stops what the request started.
			connection->close();
			throw LoopbackHttpError(Kind::Stopped, "The request was stopped.");
		}
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			connection->close();
			throw LoopbackHttpError(Kind::TimedOut, "The local inference runtime did not answer within " +
				std::to_string(request.timeout.count()) + " seconds.");
		}
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
		if (!connection->wait_readable(std::min(kSlice, remaining))) continue;
		const std::size_t count = connection->receive(buffer, sizeof(buffer));
		if (count == 0) {
			// The server closed the connection: what arrived is the response.
			if (auto body = response.body_at_close()) return LoopbackHttpResponse{response.status(), std::move(*body)};
			throw LoopbackHttpError(Kind::Failed, "The local inference runtime closed the connection without a "
				"complete response.");
		}
		response.append(buffer, count);
		if (response.head_complete() && response.status() == 0)
			throw LoopbackHttpError(Kind::Failed, "The local inference runtime did not answer in HTTP.");
		if (auto body = response.body()) return LoopbackHttpResponse{response.status(), std::move(*body)};
	}
}

} // namespace singularity::platform
