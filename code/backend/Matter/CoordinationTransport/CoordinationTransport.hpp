#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace singularity::matter::coordination::transport {

// The bytes of one sealed envelope: a transport carries them and never
// reads inside.
using SealedBytes = std::vector<std::uint8_t>;

// What an envelope is known by wherever it travels: BLAKE2b-256 of its sealed
// bytes, in lowercase hex. One that came two ways is taken once. This is the
// one definition: the seal (Matter/CoordinationSeal) and the engine use it.
std::string envelope_id(const SealedBytes& bytes);

// A way a firm's computers pass sealed envelopes to each other directly, never
// through Legal Machines: the firm's own folder, the office network, a direct
// link across the internet. Each keeps its own cursor and cleans up after
// itself; none knows what it carries.
class Transport {
public:
	virtual ~Transport() = default;
	// Sends a Matter's sealed bytes to the given computers; empty: to every
	// computer of the Matter this transport reaches.
	virtual void send(const std::string& matterId, const SealedBytes& bytes, const std::vector<std::string>& toDevices) = 0;
	// The Matter's sealed envelopes that came and are not yet acknowledged here.
	// One the engine could not open yet (no key) is offered again later.
	virtual std::vector<SealedBytes> poll(const std::string& matterId) = 0;
	// The engine opened and applied the envelope, or set it aside for good: it
	// is not offered again here.
	virtual void ack(const std::string& matterId, const std::string& envelopeId) = 0;
	// Its name in the record and the window: "Firm folder", "Office network",
	// "Direct".
	virtual std::string name() const = 0;
	// Whether every computer of the firm reads what is put in it, as the firm's
	// folder is read: what one computer took from it needs no passing on. A
	// link between two computers reaches only the computer at its other end.
	virtual bool reaches_every_computer() const { return false; }
	// Calls `wake`, on the transport's own thread, as soon as an envelope has
	// come: the engine takes it at once, as a socket that pushes wakes its
	// reader, not on its next round. `wake` must not wait long. A transport that
	// cannot tell when something came never calls it, and is polled each round.
	virtual void on_arrival(std::function<void()> wake) { static_cast<void>(wake); }
};

} // namespace singularity::matter::coordination::transport
