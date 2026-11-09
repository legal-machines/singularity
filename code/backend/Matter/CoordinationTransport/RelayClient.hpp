#pragma once

#include "Matter/CoordinationTransport/Relay.hpp"
#include "Platform/WebSocket/WebSocket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace singularity::matter::coordination::transport::link {

// This computer's way to Legal Machines' relay: one WebSocket on port 443,
// kept open while the program runs, so the firm's other computers reach this
// one through it whenever no direct way does. It carries the direct link's
// datagrams as they are, sealed on this computer for the computer they go to;
// it adds who they go to, and the relay says who they came from.
// - It opens with the seat's session and this computer's coordination key; the
//   relay lets it in only as a computer its seat announced.
// - Opened, it is up once the relay says "ready". Turned away ("refused": not
//   signed in, or not announced yet), it tries again a minute later; broken,
//   at once and then less often, up to a minute apart.
// - It asks for a sign of life every 25 seconds; nothing heard for 75, the
//   connection is taken as dead and opened again.
// Two threads of its own: one keeps the connection and reads it, one writes.
class RelayClient final : public Relay {
public:
	struct Options {
		// The relay's address ("https://legalmachines.org/singularity/api/relay").
		std::string url;
		// The headers to open with: the seat's session and X-LM-Coordination-Key.
		// None while no one is signed in here: then it waits and asks again.
		std::function<std::vector<platform::HttpHeader>()> headers;
		// Opens the connection: the system's own client unless a test gives one.
		std::function<std::unique_ptr<platform::WebSocket>(const std::string&, const std::vector<platform::HttpHeader>&)> open;
		std::chrono::milliseconds signOfLife{25000};
		std::chrono::milliseconds deadAfter{75000};
		std::chrono::milliseconds refusedWait{60000};
		std::chrono::milliseconds longestWait{60000};
	};
	// The most datagrams waiting to go, and waiting to be taken: past them the
	// newest are dropped, as a full network drops them.
	static constexpr std::size_t kMostWaiting = 4096;

	explicit RelayClient(Options options);
	~RelayClient() override;
	RelayClient(const RelayClient&) = delete;
	RelayClient& operator=(const RelayClient&) = delete;

	bool is_up() const override;
	void send(const noise::Key& to, const Bytes& datagram) override;
	std::vector<Datagram> take() override;

private:
	void keep();
	void write();
	// Waits up to `wait` unless the client is stopping; true when it is.
	bool rest(std::chrono::milliseconds wait);

	Options options_;
	std::atomic<bool> isUp_{false};
	std::atomic<bool> isStopping_{false};
	std::mutex mutex_;
	std::condition_variable wake_;
	std::shared_ptr<platform::WebSocket> socket_;
	std::deque<std::string> outgoing_;
	// The keeper wants a sign of life asked for: the writer sends "ping".
	bool isPingWanted_ = false;
	std::vector<Datagram> incoming_;
	std::thread keeper_;
	std::thread writer_;
};

} // namespace singularity::matter::coordination::transport::link
