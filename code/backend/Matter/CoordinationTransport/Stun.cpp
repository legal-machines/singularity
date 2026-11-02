#include "Matter/CoordinationTransport/Stun.hpp"

#include <cstdio>

namespace singularity::matter::coordination::transport::stun {
namespace {

constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccess = 0x0101;
constexpr std::uint16_t kMappedAddress = 0x0001;
constexpr std::uint16_t kXorMappedAddress = 0x0020;
// What servers built before RFC 5389 sent it as.
constexpr std::uint16_t kOldXorMappedAddress = 0x8020;
constexpr std::uint8_t kFour = 0x01;
constexpr std::uint8_t kSix = 0x02;

std::uint16_t read16(const Bytes& bytes, std::size_t at) {
	return static_cast<std::uint16_t>((bytes[at] << 8) | bytes[at + 1]);
}

void put16(Bytes& out, std::uint16_t value) {
	out.push_back(static_cast<std::uint8_t>(value >> 8));
	out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void putCookie(Bytes& out) {
	for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>((kMagicCookie >> shift) & 0xFF));
}

// What an XOR'd address is XOR'd with: the cookie, then the transaction.
std::array<std::uint8_t, 16> xorMask(const Transaction& transaction) {
	std::array<std::uint8_t, 16> mask{};
	for (int index = 0; index < 4; ++index) mask[index] = static_cast<std::uint8_t>((kMagicCookie >> (24 - 8 * index)) & 0xFF);
	for (std::size_t index = 0; index < transaction.size(); ++index) mask[4 + index] = transaction[index];
	return mask;
}

std::string textOf(std::uint8_t family, const std::uint8_t* address, std::uint16_t port) {
	char text[64];
	if (family == kFour) {
		std::snprintf(text, sizeof text, "%u.%u.%u.%u:%u", address[0], address[1], address[2], address[3], static_cast<unsigned>(port));
		return text;
	}
	static const std::uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
	bool isMapped = true;
	for (int index = 0; index < 12; ++index) isMapped = isMapped && address[index] == mapped[index];
	// An IPv4 address mapped into IPv6 is said as IPv4, as the socket says it.
	if (isMapped == true) return textOf(kFour, address + 12, port);
	std::string host;
	for (int group = 0; group < 8; ++group) {
		std::snprintf(text, sizeof text, "%x", static_cast<unsigned>((address[2 * group] << 8) | address[2 * group + 1]));
		host += (group == 0 ? "" : ":") + std::string(text);
	}
	return "[" + host + "]:" + std::to_string(port);
}

std::optional<std::string> addressIn(const Bytes& value, bool isXored, const Transaction& transaction) {
	if (value.size() < 4) return std::nullopt;
	const std::uint8_t family = value[1];
	const std::size_t size = family == kFour ? 4 : family == kSix ? 16 : 0;
	if (size == 0 || value.size() < 4 + size) return std::nullopt;
	std::uint16_t port = read16(value, 2);
	std::array<std::uint8_t, 16> address{};
	for (std::size_t index = 0; index < size; ++index) address[index] = value[4 + index];
	if (isXored == true) {
		const std::array<std::uint8_t, 16> mask = xorMask(transaction);
		port = static_cast<std::uint16_t>(port ^ (kMagicCookie >> 16));
		for (std::size_t index = 0; index < size; ++index) address[index] = static_cast<std::uint8_t>(address[index] ^ mask[index]);
	}
	return textOf(family, address.data(), port);
}

std::optional<std::uint16_t> portOf(const std::string& text) {
	if (text.empty() == true || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
	const unsigned long port = std::stoul(text);
	if (port == 0 || port > 65535) return std::nullopt;
	return static_cast<std::uint16_t>(port);
}

std::optional<std::array<std::uint8_t, 4>> fourOf(const std::string& text) {
	std::array<std::uint8_t, 4> address{};
	std::size_t at = 0;
	for (int part = 0; part < 4; ++part) {
		const std::size_t end = part == 3 ? text.size() : text.find('.', at);
		if (end == std::string::npos || end == at || end - at > 3) return std::nullopt;
		const std::string digits = text.substr(at, end - at);
		if (digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
		const unsigned long value = std::stoul(digits);
		if (value > 255) return std::nullopt;
		address[part] = static_cast<std::uint8_t>(value);
		at = end + 1;
	}
	return address;
}

std::optional<std::vector<std::uint16_t>> groupsOf(const std::string& text) {
	std::vector<std::uint16_t> groups;
	if (text.empty() == true) return groups;
	std::size_t at = 0;
	for (;;) {
		const std::size_t end = text.find(':', at);
		const std::string group = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (group.empty() == true || group.size() > 4 || group.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
			return std::nullopt;
		groups.push_back(static_cast<std::uint16_t>(std::stoul(group, nullptr, 16)));
		if (end == std::string::npos) return groups;
		at = end + 1;
	}
}

std::optional<std::array<std::uint8_t, 16>> sixOf(const std::string& text) {
	const std::size_t gap = text.find("::");
	std::optional<std::vector<std::uint16_t>> head = groupsOf(gap == std::string::npos ? text : text.substr(0, gap));
	std::optional<std::vector<std::uint16_t>> tail = gap == std::string::npos ? std::vector<std::uint16_t>() : groupsOf(text.substr(gap + 2));
	if (head.has_value() == false || tail.has_value() == false) return std::nullopt;
	const std::size_t given = head->size() + tail->size();
	if ((gap == std::string::npos && given != 8) || (gap != std::string::npos && given > 7)) return std::nullopt;
	std::vector<std::uint16_t> groups = *head;
	groups.resize(8 - tail->size(), 0);
	groups.insert(groups.end(), tail->begin(), tail->end());
	std::array<std::uint8_t, 16> address{};
	for (std::size_t group = 0; group < 8; ++group) {
		address[2 * group] = static_cast<std::uint8_t>(groups[group] >> 8);
		address[2 * group + 1] = static_cast<std::uint8_t>(groups[group] & 0xFF);
	}
	return address;
}

} // namespace

Bytes binding_request(const Transaction& transaction) {
	Bytes request;
	put16(request, kBindingRequest);
	put16(request, 0);
	putCookie(request);
	request.insert(request.end(), transaction.begin(), transaction.end());
	return request;
}

bool is_stun(const Bytes& datagram) {
	if (datagram.size() < 20 || (datagram[0] & 0xC0) != 0) return false;
	const std::uint32_t cookie = (static_cast<std::uint32_t>(datagram[4]) << 24) | (static_cast<std::uint32_t>(datagram[5]) << 16)
		| (static_cast<std::uint32_t>(datagram[6]) << 8) | datagram[7];
	const std::size_t length = read16(datagram, 2);
	return cookie == kMagicCookie && length % 4 == 0 && datagram.size() == 20 + length;
}

std::optional<std::string> mapped_address(const Bytes& response, const Transaction& transaction) {
	if (is_stun(response) == false || read16(response, 0) != kBindingSuccess) return std::nullopt;
	for (std::size_t index = 0; index < transaction.size(); ++index) {
		if (response[8 + index] != transaction[index]) return std::nullopt;
	}
	std::optional<std::string> plain;
	for (std::size_t at = 20; at + 4 <= response.size();) {
		const std::uint16_t type = read16(response, at);
		const std::size_t length = read16(response, at + 2);
		if (at + 4 + length > response.size()) return std::nullopt;
		const Bytes value(response.begin() + static_cast<std::ptrdiff_t>(at + 4),
			response.begin() + static_cast<std::ptrdiff_t>(at + 4 + length));
		if (type == kXorMappedAddress || type == kOldXorMappedAddress) {
			const std::optional<std::string> found = addressIn(value, true, transaction);
			if (found.has_value() == true) return found;
		} else if (type == kMappedAddress && plain.has_value() == false) {
			plain = addressIn(value, false, transaction);
		}
		// Each value is padded to four bytes.
		at += 4 + ((length + 3) / 4) * 4;
	}
	return plain;
}

Bytes binding_response(const Transaction& transaction, const std::string& address) {
	std::string host;
	std::string portText;
	if (address.empty() == false && address.front() == '[') {
		const std::size_t close = address.find(']');
		if (close == std::string::npos || close + 2 > address.size() || address[close + 1] != ':') return Bytes();
		host = address.substr(1, close - 1);
		portText = address.substr(close + 2);
	} else {
		const std::size_t colon = address.rfind(':');
		if (colon == std::string::npos) return Bytes();
		host = address.substr(0, colon);
		portText = address.substr(colon + 1);
	}
	const std::optional<std::uint16_t> port = portOf(portText);
	if (port.has_value() == false) return Bytes();
	Bytes value{0};
	std::array<std::uint8_t, 16> bytes{};
	std::size_t size = 0;
	if (const std::optional<std::array<std::uint8_t, 4>> four = fourOf(host); four.has_value() == true) {
		value.push_back(kFour);
		for (std::size_t index = 0; index < 4; ++index) bytes[index] = (*four)[index];
		size = 4;
	} else if (const std::optional<std::array<std::uint8_t, 16>> six = sixOf(host); six.has_value() == true) {
		value.push_back(kSix);
		bytes = *six;
		size = 16;
	} else {
		return Bytes();
	}
	put16(value, static_cast<std::uint16_t>(*port ^ (kMagicCookie >> 16)));
	const std::array<std::uint8_t, 16> mask = xorMask(transaction);
	for (std::size_t index = 0; index < size; ++index) value.push_back(static_cast<std::uint8_t>(bytes[index] ^ mask[index]));
	Bytes response;
	put16(response, kBindingSuccess);
	put16(response, static_cast<std::uint16_t>(4 + value.size()));
	putCookie(response);
	response.insert(response.end(), transaction.begin(), transaction.end());
	put16(response, kXorMappedAddress);
	put16(response, static_cast<std::uint16_t>(value.size()));
	response.insert(response.end(), value.begin(), value.end());
	return response;
}

} // namespace singularity::matter::coordination::transport::stun
