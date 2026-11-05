#include "Platform/WebSocket/WebSocketFrames.hpp"

#include <cstring>

namespace singularity::platform::websocket_frames {
namespace {

// SHA-1 (FIPS 180-4), only for the opening handshake's Sec-WebSocket-Accept,
// which RFC 6455 defines by it; it guards nothing here.
std::array<std::uint8_t, 20> sha1(const std::string& text) {
	std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	std::string message = text;
	const std::uint64_t bits = static_cast<std::uint64_t>(text.size()) * 8u;
	message.push_back(static_cast<char>(0x80));
	while (message.size() % 64 != 56) message.push_back('\0');
	for (int shift = 56; shift >= 0; shift -= 8) message.push_back(static_cast<char>((bits >> shift) & 0xFF));
	const auto rotate = [](std::uint32_t value, int by) { return (value << by) | (value >> (32 - by)); };
	for (std::size_t block = 0; block < message.size(); block += 64) {
		std::uint32_t w[80];
		for (int index = 0; index < 16; ++index) {
			w[index] = 0;
			for (int byte = 0; byte < 4; ++byte)
				w[index] = (w[index] << 8) | static_cast<std::uint8_t>(message[block + index * 4 + byte]);
		}
		for (int index = 16; index < 80; ++index) w[index] = rotate(w[index - 3] ^ w[index - 8] ^ w[index - 14] ^ w[index - 16], 1);
		std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int index = 0; index < 80; ++index) {
			std::uint32_t f = 0, k = 0;
			if (index < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999u;
			} else if (index < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1u;
			} else if (index < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCu;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6u;
			}
			const std::uint32_t next = rotate(a, 5) + f + e + k + w[index];
			e = d;
			d = c;
			c = rotate(b, 30);
			b = a;
			a = next;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	std::array<std::uint8_t, 20> digest{};
	for (int index = 0; index < 20; ++index) digest[index] = static_cast<std::uint8_t>((h[index / 4] >> (24 - 8 * (index % 4))) & 0xFF);
	return digest;
}

std::string base64(const std::uint8_t* bytes, std::size_t size) {
	static const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string text;
	for (std::size_t at = 0; at < size; at += 3) {
		const std::uint32_t value = (static_cast<std::uint32_t>(bytes[at]) << 16)
			| (at + 1 < size ? static_cast<std::uint32_t>(bytes[at + 1]) << 8 : 0u)
			| (at + 2 < size ? static_cast<std::uint32_t>(bytes[at + 2]) : 0u);
		text += kAlphabet[(value >> 18) & 63];
		text += kAlphabet[(value >> 12) & 63];
		text += at + 1 < size ? kAlphabet[(value >> 6) & 63] : '=';
		text += at + 2 < size ? kAlphabet[value & 63] : '=';
	}
	return text;
}

bool isControl(Opcode opcode) {
	return (static_cast<std::uint8_t>(opcode) & 0x8) != 0;
}

} // namespace

std::string client_frame(Opcode opcode, const std::string& payload, const std::array<std::uint8_t, 4>& mask) {
	std::string frame;
	frame.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(opcode)));
	const std::uint64_t size = payload.size();
	if (size < 126) {
		frame.push_back(static_cast<char>(0x80 | size));
	} else if (size <= 0xFFFF) {
		frame.push_back(static_cast<char>(0x80 | 126));
		frame.push_back(static_cast<char>((size >> 8) & 0xFF));
		frame.push_back(static_cast<char>(size & 0xFF));
	} else {
		frame.push_back(static_cast<char>(0x80 | 127));
		for (int shift = 56; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((size >> shift) & 0xFF));
	}
	for (const std::uint8_t byte : mask) frame.push_back(static_cast<char>(byte));
	for (std::size_t index = 0; index < payload.size(); ++index)
		frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[index]) ^ mask[index % 4]));
	return frame;
}

std::string accept_for(const std::string& key) {
	const std::array<std::uint8_t, 20> digest = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	return base64(digest.data(), digest.size());
}

void Reader::feed(const char* bytes, std::size_t size) {
	if (at_ > 0 && at_ >= buffer_.size() / 2) {
		buffer_.erase(0, at_);
		at_ = 0;
	}
	buffer_.append(bytes, size);
}

std::optional<Reader::Message> Reader::next() {
	while (isBroken_ == false) {
		const std::size_t left = buffer_.size() - at_;
		if (left < 2) return std::nullopt;
		const auto* head = reinterpret_cast<const std::uint8_t*>(buffer_.data() + at_);
		const bool isFinal = (head[0] & 0x80) != 0;
		const auto opcode = static_cast<Opcode>(head[0] & 0x0F);
		const bool isMasked = (head[1] & 0x80) != 0;
		std::uint64_t size = head[1] & 0x7F;
		std::size_t header = 2;
		if (size == 126) {
			if (left < 4) return std::nullopt;
			size = (static_cast<std::uint64_t>(head[2]) << 8) | head[3];
			header = 4;
		} else if (size == 127) {
			if (left < 10) return std::nullopt;
			size = 0;
			for (int index = 2; index < 10; ++index) size = (size << 8) | head[index];
			header = 10;
		}
		const bool isKnown = opcode == Opcode::Continuation || opcode == Opcode::Text || opcode == Opcode::Binary
			|| opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
		// A server never masks; a control frame is whole and short.
		if (isMasked == true || isKnown == false || size > kMostMessage
				|| (isControl(opcode) == true && (isFinal == false || size > 125))) {
			isBroken_ = true;
			return std::nullopt;
		}
		if (left < header + size) return std::nullopt;
		std::string payload = buffer_.substr(at_ + header, static_cast<std::size_t>(size));
		at_ += header + static_cast<std::size_t>(size);
		if (isControl(opcode) == true) return Message{opcode, std::move(payload)};
		if (opcode == Opcode::Continuation) {
			if (partialOpcode_.has_value() == false) {
				isBroken_ = true;
				return std::nullopt;
			}
		} else {
			if (partialOpcode_.has_value() == true) {
				isBroken_ = true;
				return std::nullopt;
			}
			partialOpcode_ = opcode;
			partial_.clear();
		}
		if (partial_.size() + payload.size() > kMostMessage) {
			isBroken_ = true;
			return std::nullopt;
		}
		partial_ += payload;
		if (isFinal == true) {
			Message message{*partialOpcode_, std::move(partial_)};
			partial_.clear();
			partialOpcode_.reset();
			return message;
		}
	}
	return std::nullopt;
}

} // namespace singularity::platform::websocket_frames
