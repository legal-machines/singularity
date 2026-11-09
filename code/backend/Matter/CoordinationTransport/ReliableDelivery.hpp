#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace singularity::matter::coordination::transport::link {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

// Whole messages - sealed envelopes of any size - carried between two of a
// firm's computers over UDP, which loses, repeats and reorders datagrams.
// Each message goes in fragments small enough for any path; the other side
// acknowledges what it holds, in order and in ranges beyond; what is not
// acknowledged in time is sent again; and the window of fragments in flight
// grows while everything arrives and halves when something is lost, so a
// lawyer's home connection is never flooded. The receiving side hands each
// message over once, whole.
//
// Pure logic: the time comes from the caller, and the frames it makes are
// sealed and sent by the link (DirectLink), which also proves who sent them.
class ReliableDelivery {
public:
	// The most payload one fragment this computer sends carries: with the
	// frame's and the link's headers and the seal's tag, a datagram of 946
	// bytes.
	static constexpr std::size_t kFragmentSize = 900;
	// The most payload a fragment this computer takes may carry: what earlier
	// builds send, so they are still heard.
	static constexpr std::size_t kMostFragmentSize = 1100;
	// The most a message may be, and the most messages received in part at once.
	static constexpr std::size_t kMostMessageBytes = 512u * 1024u * 1024u;
	static constexpr std::size_t kMostPartialMessages = 64;

	ReliableDelivery();

	// Queues a message; its id, which take_confirmed() reports once the other
	// side holds all of it.
	std::uint64_t send(Bytes message);
	// The frames to put on the wire now: acknowledgements owed, fragments sent
	// again whose time ran out, and new fragments as the window allows.
	std::vector<Bytes> frames_to_send(Clock::time_point now);
	// A frame from the other side. One that is not well formed is ignored.
	void receive(const Bytes& frame, Clock::time_point now);
	// The messages that arrived whole since the last call, each once.
	std::vector<Bytes> take_delivered();
	// The ids of messages the other side now holds whole.
	std::vector<std::uint64_t> take_confirmed();
	// When frames_to_send() next has a fragment to send again; nothing when
	// nothing waits.
	std::optional<Clock::time_point> next_deadline() const;
	// Nothing in flight, queued or owed.
	bool is_idle() const;
	// The messages the other side does not yet hold whole, oldest first, taken
	// out with the ids send gave them: what a new session between the same
	// computers sends again. Across sessions a message may come twice; the
	// engine takes an envelope once by its id.
	struct Unconfirmed {
		std::uint64_t id = 0;
		Bytes bytes;
	};
	std::vector<Unconfirmed> take_unconfirmed();

	// What is in flight, for tests and the record.
	std::size_t window() const { return static_cast<std::size_t>(window_); }
	std::size_t in_flight() const { return inFlight_; }

private:
	struct Fragment {
		bool sent = false;
		bool acknowledged = false;
		Clock::time_point sentAt{};
		int tries = 0;
	};
	struct Outgoing {
		std::uint64_t id = 0;
		Bytes bytes;
		std::uint32_t count = 0;
		std::vector<Fragment> fragments;
		std::uint32_t acknowledged = 0;
		// Fragments are sent in order: those from sentUpTo on are not sent yet,
		// and every one below ackedBelow is acknowledged. Only the span between
		// them is ever looked at again.
		std::uint32_t sentUpTo = 0;
		std::uint32_t ackedBelow = 0;
		// For the timing trace (Platform/Trace): when it was queued, when its
		// first fragment went, and how many fragments went again.
		Clock::time_point queuedAt{};
		Clock::time_point firstSentAt{};
		std::uint32_t resends = 0;
	};
	struct Incoming {
		std::uint32_t count = 0;
		std::vector<Bytes> fragments;
		std::vector<bool> held;
		std::uint32_t heldCount = 0;
		std::size_t bytes = 0;
		// When its first fragment came, for the timing trace.
		Clock::time_point firstAt{};
	};

	// Sending side.
	std::uint64_t nextId_ = 1;
	std::deque<Outgoing> outgoing_;
	std::vector<std::uint64_t> confirmed_;
	std::size_t inFlight_ = 0;
	double window_ = 4.0;
	// The round trip, smoothed, and its variation (RFC 6298).
	std::optional<std::chrono::microseconds> smoothedRoundTrip_;
	std::chrono::microseconds roundTripVariation_{0};
	std::chrono::microseconds retransmissionTimeout_{300000};

	// Receiving side.
	std::map<std::uint64_t, Incoming> incoming_;
	// Every message id up to this one was taken whole; above it, those in the set.
	std::uint64_t takenThrough_ = 0;
	std::set<std::uint64_t> takenAbove_;
	std::vector<Bytes> delivered_;
	// Acknowledgements owed, by message.
	std::set<std::uint64_t> owed_;

	bool isTaken(std::uint64_t id) const;
	void markTaken(std::uint64_t id);
	Bytes acknowledgementFor(std::uint64_t id) const;
	void onData(const Bytes& frame, Clock::time_point now);
	void onAcknowledgement(const Bytes& frame, Clock::time_point now);
	void measure(std::chrono::microseconds sample);
	Bytes dataFrame(const Outgoing& message, std::uint32_t index) const;
};

} // namespace singularity::matter::coordination::transport::link
