#pragma once

// Addresses as UdpSocket says them, for the two implementations alone: each
// includes its own system's socket headers before this one.

#include <cstring>
#include <string>

namespace singularity::platform::udp_addresses {

inline bool isLinkLocalOrLoopback(const std::string& text) {
	return text.rfind("127.", 0) == 0 || text.rfind("169.254.", 0) == 0 || text == "::1" || text.rfind("fe80", 0) == 0
		|| text.rfind("FE80", 0) == 0;
}

// An address's text, without its port: an IPv4 one mapped into IPv6 is said
// as IPv4.
inline std::string hostOf(const sockaddr* address) {
	char text[64] = {0};
	if (address->sa_family == AF_INET) {
		const sockaddr_in* four = reinterpret_cast<const sockaddr_in*>(address);
		inet_ntop(AF_INET, &four->sin_addr, text, sizeof text);
		return text;
	}
	if (address->sa_family == AF_INET6) {
		const sockaddr_in6* six = reinterpret_cast<const sockaddr_in6*>(address);
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&six->sin6_addr);
		static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
		if (std::memcmp(bytes, mapped, 12) == 0) {
			in_addr four{};
			std::memcpy(&four, bytes + 12, 4);
			inet_ntop(AF_INET, &four, text, sizeof text);
			return text;
		}
		inet_ntop(AF_INET6, &six->sin6_addr, text, sizeof text);
		return text;
	}
	return std::string();
}

inline std::string textOf(const sockaddr* address) {
	if (address->sa_family == AF_INET) {
		return hostOf(address) + ":" + std::to_string(ntohs(reinterpret_cast<const sockaddr_in*>(address)->sin_port));
	}
	if (address->sa_family == AF_INET6) {
		const std::string host = hostOf(address);
		const std::string port = std::to_string(ntohs(reinterpret_cast<const sockaddr_in6*>(address)->sin6_port));
		return host.find(':') == std::string::npos ? host + ":" + port : "[" + host + "]:" + port;
	}
	return std::string();
}

// "1.2.3.4:5" or "[2001:db8::1]:5" as an address for a socket of `family`: an
// IPv4 address is mapped into IPv6 for the dual-stack socket. False where the
// text is not an address.
inline bool parse(const std::string& text, int family, sockaddr_storage& out, int& length) {
	std::memset(&out, 0, sizeof out);
	std::string host;
	std::string port;
	if (text.empty() == false && text.front() == '[') {
		const std::size_t close = text.find(']');
		if (close == std::string::npos || close + 2 > text.size() || text[close + 1] != ':') return false;
		host = text.substr(1, close - 1);
		port = text.substr(close + 2);
	} else {
		const std::size_t colon = text.rfind(':');
		if (colon == std::string::npos || text.find(':') != colon) return false;
		host = text.substr(0, colon);
		port = text.substr(colon + 1);
	}
	if (port.empty() == true || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
	const unsigned long number = std::stoul(port);
	if (number == 0 || number > 65535) return false;
	in_addr four{};
	in6_addr six{};
	if (inet_pton(AF_INET, host.c_str(), &four) == 1) {
		if (family == AF_INET) {
			sockaddr_in* target = reinterpret_cast<sockaddr_in*>(&out);
			target->sin_family = AF_INET;
			target->sin_addr = four;
			target->sin_port = htons(static_cast<unsigned short>(number));
			length = static_cast<int>(sizeof(sockaddr_in));
			return true;
		}
		sockaddr_in6* target = reinterpret_cast<sockaddr_in6*>(&out);
		target->sin6_family = AF_INET6;
		unsigned char* bytes = reinterpret_cast<unsigned char*>(&target->sin6_addr);
		bytes[10] = 0xFF;
		bytes[11] = 0xFF;
		std::memcpy(bytes + 12, &four, 4);
		target->sin6_port = htons(static_cast<unsigned short>(number));
		length = static_cast<int>(sizeof(sockaddr_in6));
		return true;
	}
	if (family == AF_INET6 && inet_pton(AF_INET6, host.c_str(), &six) == 1) {
		sockaddr_in6* target = reinterpret_cast<sockaddr_in6*>(&out);
		target->sin6_family = AF_INET6;
		target->sin6_addr = six;
		target->sin6_port = htons(static_cast<unsigned short>(number));
		length = static_cast<int>(sizeof(sockaddr_in6));
		return true;
	}
	return false;
}

} // namespace singularity::platform::udp_addresses
