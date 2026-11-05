#pragma once

#include "Matter/CoordinationTransport/CoordinationTransport.hpp"
#include "Matter/CoordinationTransport/DirectLink.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace singularity::matter::coordination::transport {

// The firm's computers linked directly (D-124): the engine's sealed envelopes
// carried over one DirectLink to the computers a Matter's address cards named
// (D-123, 0x04), each by its X25519 key and the addresses it may be reached at.
// A computer it knows no key or address for, or one that is away, the firm's
// folder carries to: the engine sends every envelope by every carrier, and
// takes each once, by its id.
// - Only a computer some Matter's checked card named is spoken with, and what
//   it sends is taken only for the Matters whose cards named it.
// - What came waits for the engine until it is taken, as in the firm's folder:
//   one it cannot open yet is offered again.
// - One for the whole program: the engine makes its carriers anew each round,
//   and a link lives as long as the program.
class DirectTransport final : public Transport {
public:
	struct Peer {
		std::string device_id;
		link::PeerKey key{};
		// "udp:192.168.1.20:47380", "udp:[2001:db8::7]:47380".
		std::vector<std::string> candidates;
	};

	explicit DirectTransport(link::DirectLink::Options options);
	DirectTransport(const DirectTransport&) = delete;
	DirectTransport& operator=(const DirectTransport&) = delete;

	// The program's own, made with this computer's X25519 keys the first time
	// it is asked for: on the port Singularity prefers, or any free one when
	// that is taken; with Legal Machines' relay for the computers no direct
	// way reaches (D-242), when one is given.
	static std::shared_ptr<DirectTransport> shared(const noise::KeyPair& self,
		std::shared_ptr<link::Relay> relay = nullptr);
	// The program's own, if one was made; never makes one.
	static std::shared_ptr<DirectTransport> existing();
	static constexpr std::uint16_t kPreferredPort = 47380;

	// What one of a Matter's address cards said of a computer, once its seal and
	// its signature were checked (D-123): it may be spoken with about that Matter.
	void learn(const std::string& matterId, const Peer& peer);
	// A Matter whose invitation was given lately (D-127): until `untilSeconds`
	// (since 1970) a computer no card named may call this one about it - and
	// what it sends is taken only as a request to join (0x03) whose X25519 key
	// is the one it called with, from at most kMostAsking computers.
	void open_lobby(const std::string& matterId, std::int64_t untilSeconds);
	// A computer whose request the lawyer here refused: nothing it sends is
	// taken, and it is not answered again.
	void refuse(const link::PeerKey& key);
	static constexpr std::size_t kMostAsking = 8;
	// Where this computer may be reached now: each of its addresses, with the
	// link's port.
	std::vector<std::string> candidates() const;
	// The computers of a Matter this one holds a live encrypted session with
	// now, by device (D-137): the link keeps each session alive while both ends
	// answer, so this is what the lawyer is told is connected.
	std::vector<std::string> connected_devices(const std::string& matterId) const;
	std::uint16_t port() const { return link_.local_port(); }

	// What went to one computer about one Matter in the transfer under way, and
	// how much of it that computer holds whole, as its link confirmed (D-207):
	// the lawyer is shown a Matter going by its bytes. A new transfer starts
	// from nothing once every message of the last was confirmed, which is shown
	// for kFlowKept and then forgotten.
	struct Flow {
		std::string device_id;
		std::uint64_t messages = 0;
		std::uint64_t bytes = 0;
		std::uint64_t confirmed_messages = 0;
		std::uint64_t confirmed_bytes = 0;
		// Whether the link holds a live session with that computer now: one away
		// takes nothing until it is back.
		bool reachable = false;
	};
	std::vector<Flow> flows(const std::string& matterId) const;
	static constexpr std::chrono::seconds kFlowKept{8};
	// The sealed bytes come for a Matter that still wait for the engine to take
	// them: before its key is taken here, what of the Matter is already on this
	// computer, closed (D-207).
	std::uint64_t waiting_bytes(const std::string& matterId) const;

	void send(const std::string& matterId, const SealedBytes& bytes, const std::vector<std::string>& toDevices) override;
	std::vector<SealedBytes> poll(const std::string& matterId) override;
	void ack(const std::string& matterId, const std::string& envelopeId) override;
	std::string name() const override;
	// Called on the link's thread the moment a whole envelope has come.
	void on_arrival(std::function<void()> wake) override;

	// The most envelopes of one Matter held for the engine at once; beyond it the
	// oldest gives way, and the firm's folder still carries it.
	static constexpr std::size_t kMostWaiting = 4096;
	static constexpr std::size_t kMostPerPoll = 256;
	// The most bytes sent to one computer and not yet held by it: one that is
	// away is not sent a whole Matter into this computer's memory.
	static constexpr std::size_t kMostQueuedBytes = 256u * 1024u * 1024u;

private:
	struct Waiting {
		std::map<std::string, SealedBytes> byId;
		std::deque<std::string> order;
	};

	mutable std::mutex mutex_;
	std::map<std::string, std::map<std::string, Peer>> peers_;
	std::set<link::PeerKey> admitted_;
	// Each invited Matter's lobby, open until when (seconds since 1970); the
	// computers that asked through it, by device; and those refused.
	std::map<std::string, std::int64_t> lobbies_;
	std::map<std::string, std::map<std::string, link::PeerKey>> asking_;
	std::set<link::PeerKey> refused_;
	std::map<std::string, Waiting> waiting_;
	// What each computer was sent and does not hold yet, in bytes; each message
	// sent by the link's id, until it is confirmed; and confirmations that came
	// before send() wrote their message down.
	struct Sent {
		link::PeerKey key{};
		std::size_t size = 0;
		std::string matter_id;
		std::string device_id;
	};
	std::map<link::PeerKey, std::size_t> queued_;
	std::map<std::uint64_t, Sent> sent_;
	std::set<std::uint64_t> early_;
	// Each transfer under way, by Matter and computer (D-207).
	struct Sending {
		link::PeerKey key{};
		std::uint64_t messages = 0;
		std::uint64_t bytes = 0;
		std::uint64_t confirmed_messages = 0;
		std::uint64_t confirmed_bytes = 0;
		std::chrono::steady_clock::time_point settled_at{};
	};
	std::map<std::pair<std::string, std::string>, Sending> sending_;
	void confirm(const Sent& sent);
	// Whom to wake when an envelope has come: held apart from mutex_, since the
	// link calls it from its own thread.
	std::mutex wakeMutex_;
	std::function<void()> wake_;
	void tellArrived();
	// Last: made after what its thread asks about (admitted()), and gone, its
	// thread with it, before them. Never called into while mutex_ is held,
	// since the link asks admitted() while it holds its own lock.
	link::DirectLink link_;

	bool admitted(const link::PeerKey& key) const;
	bool isLobbyOpen(const std::string& matterId) const;
	bool takesAsRequest(const std::string& matterId, const link::PeerKey& from, const SealedBytes& bytes);
	void takeArrivals();
	void settle(const link::PeerKey& key, std::size_t size);
	static link::DirectLink::Options withAdmission(link::DirectLink::Options options, DirectTransport* self);
};

} // namespace singularity::matter::coordination::transport
