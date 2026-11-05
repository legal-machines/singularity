#pragma once

#include "Matter/CoordinationTransport/NoiseIK.hpp"
#include "Matter/CoordinationTransport/Relay.hpp"
#include "Matter/CoordinationTransport/ReliableDelivery.hpp"
#include "Matter/CoordinationTransport/Stun.hpp"
#include "Platform/UdpSocket/UdpSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace singularity::matter::coordination::transport::link {

using PeerKey = noise::Key;

// Which counters of a session's datagrams were taken: each is taken once, and
// one too far behind the newest is refused, as WireGuard keeps its own (a
// window of 2,048). Checked only after the datagram's seal is.
class ReplayWindow {
public:
	static constexpr std::uint64_t kSize = 2048;
	bool accept(std::uint64_t counter);

private:
	std::uint64_t highest_ = 0;
	bool isAny_ = false;
	std::array<std::uint64_t, kSize / 64> bits_{};
	bool isSet(std::uint64_t counter) const;
	void set(std::uint64_t counter, bool value);
};

// Whether a computer that calls may be spoken with: one of the firm's computers
// a lawyer confirmed by its code (D-095, D-123), known by its X25519 key.
using Admit = std::function<bool(const PeerKey&)>;

// A computer's direct link to the firm's other computers (D-124), built as
// WireGuard builds its own: one UDP socket; a Noise IK handshake with each
// computer it speaks with, whose key it knows from the Matter's confirmed
// record; datagrams sealed under the session's keys, each with its counter
// and checked against replay; and whole messages carried over them, each
// delivered once, whole (ReliableDelivery). No server stands between two
// computers or knows what passes: it is sealed for the two of them alone.
// - A computer is tried at every address it may be reached at (its office
//   address, the one its router shows outside) until one answers; trying
//   from both sides at once is what gets through most routers.
// - Where it was last heard from, it is answered: a laptop that moves from the
//   office to home keeps its link.
// - Keys are renewed every two minutes and given up after three, so keys
//   taken from a computer later open nothing it sent before.
// - A computer that calls is answered only when it is admitted, at most a
//   hundred calls a second, and a call played back by someone is refused by
//   its time. Its new keys are used only once it sends under them.
// - A message survives renewed keys and a moved computer: it is delivered
//   once, whole. After the other computer restarts, what it did not confirm
//   goes again from the start.
// - Where no direct way answers - a router that gives each destination its own
//   outside port, a network that lets no UDP through - the same sealed
//   datagrams go through Legal Machines' relay (D-242), which opens none: a
//   computer is called directly first, then through the relay and directly by
//   turns once two calls go unanswered, and directly again every half minute
//   while the relay carries it, so the link goes back to a direct way as soon
//   as one answers.
// A thread of its own runs it, from construction to destruction.
class DirectLink {
public:
	struct Options {
		// This computer's X25519 key pair (D-123).
		noise::KeyPair self;
		// 0: any free port.
		std::uint16_t port = 0;
		// Called on the link's own thread while it holds its lock: it must answer
		// at once and must not call the link.
		Admit admit;
		// Fresh key pairs for each handshake: the system's secure random source
		// unless a test gives its own.
		std::function<noise::KeyPair()> ephemeral;
		// While a link is up, a datagram this often keeps the routers' way open.
		std::chrono::milliseconds keepalive{20000};
		// The computer that began a session renews its keys after this long; the
		// other, halfway to the end. After the end they are given up.
		std::chrono::milliseconds rekeyAfter{120000};
		std::chrono::milliseconds rejectAfter{180000};
		// Sent and nothing heard back for this long: the other computer may have
		// moved or restarted, and a new handshake is begun.
		std::chrono::milliseconds silence{15000};
		// For tests alone: this share of outgoing datagrams is dropped, to show
		// the link holds over a path that loses them.
		double testLoss = 0.0;
		// STUN servers ("stun.l.google.com:19302"), asked when the link starts
		// and every few minutes after what address this computer's router shows
		// outside (RFC 8489): what it tells the firm's other computers, so one
		// behind another router can reach it (D-124). None: its own addresses
		// alone. A server learns that address and nothing else.
		std::vector<std::string> stunServers;
		std::chrono::milliseconds stunEvery{300000};
		// Legal Machines' relay (D-242), used only where no direct way answers;
		// none: direct ways alone.
		std::shared_ptr<Relay> relay;
		// While the relay carries a computer, a direct way to it is tried this
		// often.
		std::chrono::milliseconds directRetry{30000};
		// Called on the link's own thread, without its lock, as soon as a whole
		// message has arrived: whoever takes them wakes at once, as a socket that
		// pushes does, instead of on its next round. It must not wait long; it
		// may call the link.
		std::function<void()> arrived;
	};
	struct Arrival {
		PeerKey from{};
		Bytes message;
	};
	struct Confirmation {
		PeerKey to{};
		std::uint64_t id = 0;
	};

	explicit DirectLink(Options options);
	~DirectLink();
	DirectLink(const DirectLink&) = delete;
	DirectLink& operator=(const DirectLink&) = delete;

	std::uint16_t local_port() const { return socket_.local_port(); }
	// Where a computer may be reached ("ip:port"), tried together until one
	// answers. A computer with addresses is kept linked while there are any; one
	// without is linked when it calls.
	void set_candidates(const PeerKey& peer, std::vector<std::string> addresses);
	// Queues a message for a computer: it goes once the link is up, whole and
	// once, and waits while it is not. Its id, which take_confirmed() names
	// once the computer holds it whole.
	std::uint64_t send(const PeerKey& peer, Bytes message);
	// The messages that arrived since the last call, with who sent them.
	std::vector<Arrival> take_arrivals();
	// The messages the other computers now hold whole, since the last call.
	std::vector<Confirmation> take_confirmed();
	// Drops all the link knows of a computer: its sessions and what waits for
	// it. Whoever stops admitting it forgets it here.
	void forget(const PeerKey& peer);
	bool is_connected(const PeerKey& peer) const;
	// The address the computer was last heard from; empty before.
	std::string endpoint_of(const PeerKey& peer) const;
	// Whether the relay carries the link to the computer now (D-242).
	bool is_relayed(const PeerKey& peer) const;
	// How many sessions with the computer this link began or took up.
	std::size_t sessions_with(const PeerKey& peer) const;
	// The addresses STUN servers said this computer's router shows outside for
	// the link ("203.0.113.7:47380"), heard lately; none before one answered.
	std::vector<std::string> public_addresses() const;

	// The address of a computer reached through the relay: "relay/" and its
	// key in hex. It stands where an "ip:port" stands, so the link answers
	// a computer where it was last heard from, the relay or a direct way alike.
	static constexpr const char* kRelayPrefix = "relay/";
	// Calls made directly before the relay is tried too.
	static constexpr int kDirectCalls = 2;

	// What a handshake is bound to: two programs that are not this link never
	// complete one with it.
	static constexpr const char* kPrologue = "Singularity direct link 1";
	// The most calls answered in a second: a flood of them costs this computer
	// no more.
	static constexpr std::uint32_t kMostCallsPerSecond = 100;

private:
	// The keys of one handshake, one way each.
	struct Session {
		std::uint32_t localIndex = 0;
		std::uint32_t remoteIndex = 0;
		noise::CipherState send;
		noise::CipherState receive;
		bool isInitiator = false;
		// Which run of the other computer's program made it.
		std::uint64_t peerRun = 0;
		Clock::time_point establishedAt{};
		std::uint64_t sendCounter = 0;
		ReplayWindow replay;
	};
	struct Pending {
		std::uint32_t localIndex = 0;
		std::unique_ptr<noise::Initiator> initiator;
		Clock::time_point sentAt{};
		int tries = 0;
	};
	struct Peer {
		PeerKey key{};
		std::vector<std::string> candidates;
		std::string endpoint;
		// What this computer sends under; the one before it, still heard; and
		// one it answered, taken up once the caller sends under it.
		std::unique_ptr<Session> session;
		std::unique_ptr<Session> previous;
		std::unique_ptr<Session> next;
		std::optional<Pending> pending;
		// Messages both ways, across the sessions of one run of each program.
		ReliableDelivery delivery;
		// The link's id of each message the delivery carries, by its own.
		std::map<std::uint64_t, std::uint64_t> linkIds;
		std::optional<std::uint64_t> peerRun;
		std::uint64_t lastCall = 0;
		Clock::time_point lastSent{};
		std::optional<Clock::time_point> unansweredSince;
		// When a direct way was last tried while the relay carried it.
		Clock::time_point directTriedAt{};
		std::size_t sessions = 0;
	};

	void run();
	void askStun();
	void onStun(const platform::UdpSocket::Datagram& datagram, Clock::time_point now);
	void handle(const platform::UdpSocket::Datagram& datagram, Clock::time_point now);
	void onCall(const platform::UdpSocket::Datagram& datagram, Clock::time_point now);
	void onAnswer(const platform::UdpSocket::Datagram& datagram, Clock::time_point now);
	void onData(const platform::UdpSocket::Datagram& datagram, Clock::time_point now);
	void tick(Clock::time_point now);
	// Calls the computer: directly, through the relay, or both.
	void call(Peer& peer, Clock::time_point now, bool isDirectOnly = false);
	void establish(Peer& peer, std::unique_ptr<Session> session);
	void restartDelivery(Peer& peer);
	void drop(std::unique_ptr<Session>& session);
	void collect(Peer& peer);
	void sendData(Peer& peer, const Bytes& frame, Clock::time_point now);
	void sendDatagram(const std::string& address, const Bytes& bytes);
	std::uint32_t newIndex();
	std::uint64_t nextStamp();
	Peer& peerOf(const PeerKey& key);

	Options options_;
	platform::UdpSocket socket_;
	// This run of the program, said in each handshake: another after a restart.
	std::uint64_t runId_ = 0;
	mutable std::mutex mutex_;
	std::map<PeerKey, Peer> peers_;
	// Which computer each of this computer's session indexes belongs to.
	std::map<std::uint32_t, PeerKey> byIndex_;
	std::vector<Arrival> arrivals_;
	// A whole message arrived in this turn of the link's thread: said once the
	// lock is let go.
	bool hasArrived_ = false;
	std::vector<Confirmation> confirmations_;
	std::uint64_t nextLinkId_ = 1;
	// Each handshake this computer starts says a later time than the one
	// before, so one played back is known.
	std::uint64_t lastStamp_ = 0;
	std::uint32_t callsCounted_ = 0;
	Clock::time_point countedSince_{};
	// When data under an unknown session was last said to the trace.
	Clock::time_point unknownSaidAt_{};
	std::uint64_t dropCounter_ = 0;
	// STUN requests in flight, and the addresses the answers named, by when.
	std::map<stun::Transaction, Clock::time_point> stunAsked_;
	std::map<std::string, Clock::time_point> publicAddresses_;
	std::mutex stunMutex_;
	std::condition_variable stunWake_;
	bool isStunStopping_ = false;
	std::atomic<bool> isRunning_{true};
	std::thread thread_;
	// Asks the STUN servers, apart from the link's thread: finding a server's
	// address may wait on the network.
	std::thread stunThread_;
};

} // namespace singularity::matter::coordination::transport::link
