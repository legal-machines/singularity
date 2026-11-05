#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform::websocket_frames {

// RFC 6455's framing, for a client that speaks it over a raw TLS stream (the
// Linux client, Platform/WebSocket/WebSocketLinux.cpp): frames a client sends,
// each masked, and the messages read back out of what a server sends.

enum class Opcode : std::uint8_t {
	Continuation = 0x0,
	Text = 0x1,
	Binary = 0x2,
	Close = 0x8,
	Ping = 0x9,
	Pong = 0xA,
};

// One whole frame, FIN set, masked with `mask`.
std::string client_frame(Opcode opcode, const std::string& payload, const std::array<std::uint8_t, 4>& mask);

// The opening request's Sec-WebSocket-Accept for a Sec-WebSocket-Key.
std::string accept_for(const std::string& key);

// Reads a server's frames as their bytes arrive, in any pieces.
class Reader {
public:
	struct Message {
		Opcode opcode = Opcode::Binary;
		std::string data;
	};
	// The most bytes of one message: a server that sends more is broken.
	static constexpr std::size_t kMostMessage = 1u << 20;

	void feed(const char* bytes, std::size_t size);
	// The next whole message - a text or binary one put together from its
	// frames, or a control frame - or nothing yet.
	std::optional<Message> next();
	// The server broke the framing: a masked frame, an unknown opcode, a
	// message too large, a control frame in pieces.
	bool is_broken() const { return isBroken_; }

private:
	std::string buffer_;
	std::size_t at_ = 0;
	std::string partial_;
	std::optional<Opcode> partialOpcode_;
	bool isBroken_ = false;
};

} // namespace singularity::platform::websocket_frames
