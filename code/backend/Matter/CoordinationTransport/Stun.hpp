#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matter::coordination::transport::stun {

// How a computer behind a router learns the address the router shows outside
// for its direct link (D-124): a STUN Binding request (RFC 8489) to a public
// server, here Google's or Cloudflare's, which the owner allowed, and the
// address the server says it came from. The server learns this computer's
// public address and nothing else; nothing of the firm passes through it.
// Only the Binding request and its success response, with
// XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS from an older server): no
// authentication, no relay.

using Bytes = std::vector<std::uint8_t>;
using Transaction = std::array<std::uint8_t, 12>;

constexpr std::uint32_t kMagicCookie = 0x2112A442;

// A Binding request for `transaction`: 20 bytes, no attributes.
Bytes binding_request(const Transaction& transaction);

// Whether a datagram is a STUN message: its first two bits zero, the magic
// cookie in place and its length what the datagram holds. Told apart this
// way from the link's own datagrams, which share its first byte.
bool is_stun(const Bytes& datagram);

// The address a Binding success response for `transaction` names
// ("203.0.113.7:41641", "[2001:db8:0:0:0:0:0:1]:41641"); nothing for a
// datagram that is not one.
std::optional<std::string> mapped_address(const Bytes& response, const Transaction& transaction);

// A Binding success response naming `address` ("ip:port", "[ipv6]:port") by
// XOR-MAPPED-ADDRESS, as a server answers: for tests and a firm's own server.
// Empty where the address is not one.
Bytes binding_response(const Transaction& transaction, const std::string& address);

} // namespace singularity::matter::coordination::transport::stun
