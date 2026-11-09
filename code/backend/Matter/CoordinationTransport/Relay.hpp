#pragma once

#include "Matter/CoordinationTransport/NoiseIK.hpp"
#include "Matter/CoordinationTransport/ReliableDelivery.hpp"

#include <utility>
#include <vector>

namespace singularity::matter::coordination::transport::link {

// The way to another of the firm's computers when no direct one reaches it:
// Legal Machines' relay, which passes the direct link's datagrams - already
// sealed for the two computers alone (Noise IK) - between the firm's
// signed-in computers, keeps none and opens none. The link uses it last: it
// calls directly first, and goes back to a direct way as soon as one answers.
class Relay {
public:
	struct Datagram {
		noise::Key from{};
		Bytes bytes;
	};

	virtual ~Relay() = default;
	// Whether a datagram handed over now can reach the relay.
	virtual bool is_up() const = 0;
	// Hands a sealed datagram over for the computer with this key; dropped
	// when the relay is not up, as a lost UDP datagram is.
	virtual void send(const noise::Key& to, const Bytes& datagram) = 0;
	// What came through the relay since the last call.
	virtual std::vector<Datagram> take() = 0;
};

} // namespace singularity::matter::coordination::transport::link
