#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace singularity::platform {

// A blocking TCP connection to a service listening on 127.0.0.1: the server
// local inference starts for a model. A whole-response HTTP client cannot be
// interrupted while it waits; this can: the caller waits in short slices and
// simply closes the connection to cancel, which the server observes.
class LoopbackConnection final {
public:
	// std::nullopt when nothing accepts connections on that port.
	static std::optional<LoopbackConnection> connect(unsigned short port,
		std::chrono::seconds receive_timeout);

	LoopbackConnection(LoopbackConnection &&other) noexcept;
	LoopbackConnection &operator=(LoopbackConnection &&other) noexcept;
	LoopbackConnection(const LoopbackConnection &) = delete;
	LoopbackConnection &operator=(const LoopbackConnection &) = delete;
	~LoopbackConnection();

	// False when the connection failed before every byte was sent.
	bool send_all(std::string_view bytes);
	// True when receive() would not block: bytes have arrived, or the peer
	// has closed the connection. False when `limit` passed with neither.
	bool wait_readable(std::chrono::milliseconds limit);
	// The number of bytes placed in `buffer`; 0 once the peer has closed the
	// connection, the receive timeout elapsed, or the connection failed -
	// in every case there is nothing more to read.
	std::size_t receive(char *buffer, std::size_t capacity);
	void close() noexcept;

private:
	explicit LoopbackConnection(std::uintptr_t socket) noexcept;
	std::uintptr_t socket_;
};

// A TCP port on 127.0.0.1 that was free at the time of the call (bound with
// port 0, read back, released); std::nullopt when none could be reserved.
std::optional<unsigned short> available_loopback_port();

// Makes the OS socket API usable from this process (Winsock start-up on
// Windows, nothing elsewhere). Every function here calls it.
bool ensure_socket_runtime() noexcept;

// One HTTP/1.1 exchange with a service on 127.0.0.1 - how local inference
// talks to its runtime's server. The request is sent with "Connection: close"
// and the response is read whole (by Content-Length, chunked, or to the close).
struct LoopbackHttpRequest {
	std::string method = "GET";
	// The request target: a path and its query, e.g. "/slots/0?action=erase".
	std::string target = "/";
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
	// How long the whole response may take to arrive.
	std::chrono::seconds timeout{30};
	// When another thread sets it, the exchange ends within a fraction of a
	// second: the connection closes, which tells the server to stop the work
	// the request started, and loopback_http() throws a Stopped error.
	const std::atomic<bool> *stop = nullptr;
};

struct LoopbackHttpResponse {
	int status = 0;
	std::string body;
};

class LoopbackHttpError final : public std::runtime_error {
public:
	enum class Kind {
		// Nothing accepted the connection.
		Unreachable,
		// The response did not arrive within the request's timeout.
		TimedOut,
		// The request's stop flag ended it.
		Stopped,
		// The connection failed, or what came back is not an HTTP response.
		Failed,
	};
	LoopbackHttpError(Kind kind, const std::string &message) : std::runtime_error(message), kind_(kind) {}
	Kind kind() const noexcept { return kind_; }

private:
	Kind kind_;
};

// Throws LoopbackHttpError; any status the server answers with is returned.
LoopbackHttpResponse loopback_http(unsigned short port, const LoopbackHttpRequest &request);

} // namespace singularity::platform
