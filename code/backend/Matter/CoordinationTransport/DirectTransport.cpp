#include "Matter/CoordinationTransport/DirectTransport.hpp"

#include "Matter/CoordinationSeal/CoordinationSeal.hpp"
#include "Platform/UdpSocket/UdpSocket.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace singularity::matter::coordination::transport {
namespace {

constexpr const char* kScheme = "udp:";

// [the Matter's id's length, 2 bytes, little-endian][the Matter's id][the sealed envelope]
link::Bytes frameOf(const std::string& matterId, const SealedBytes& bytes) {
	link::Bytes frame;
	frame.reserve(2 + matterId.size() + bytes.size());
	frame.push_back(static_cast<std::uint8_t>(matterId.size() & 0xFF));
	frame.push_back(static_cast<std::uint8_t>((matterId.size() >> 8) & 0xFF));
	frame.insert(frame.end(), matterId.begin(), matterId.end());
	frame.insert(frame.end(), bytes.begin(), bytes.end());
	return frame;
}

bool readFrame(const link::Bytes& frame, std::string& matterId, SealedBytes& bytes) {
	if (frame.size() < 2) return false;
	const std::size_t length = static_cast<std::size_t>(frame[0]) | (static_cast<std::size_t>(frame[1]) << 8);
	if (length == 0 || frame.size() < 2 + length) return false;
	matterId.assign(frame.begin() + 2, frame.begin() + 2 + static_cast<std::ptrdiff_t>(length));
	bytes.assign(frame.begin() + 2 + static_cast<std::ptrdiff_t>(length), frame.end());
	return true;
}

// "udp:192.168.1.20:47380" is "192.168.1.20:47380"; anything else, nothing.
std::string addressOf(const std::string& candidate) {
	const std::size_t scheme = std::char_traits<char>::length(kScheme);
	if (candidate.size() <= scheme || candidate.compare(0, scheme, kScheme) != 0) return std::string();
	return candidate.substr(scheme);
}

} // namespace

link::DirectLink::Options DirectTransport::withAdmission(link::DirectLink::Options options, DirectTransport* self) {
	options.admit = [self](const link::PeerKey& key) { return self->admitted(key); };
	options.arrived = [self]() { self->tellArrived(); };
	return options;
}

void DirectTransport::on_arrival(std::function<void()> wake) {
	const std::lock_guard<std::mutex> lock(wakeMutex_);
	wake_ = std::move(wake);
}

void DirectTransport::tellArrived() {
	std::function<void()> wake;
	{
		const std::lock_guard<std::mutex> lock(wakeMutex_);
		wake = wake_;
	}
	if (wake) wake();
}

DirectTransport::DirectTransport(link::DirectLink::Options options) : link_(withAdmission(std::move(options), this)) {}

namespace {

// The program's own link. Never destroyed: the engine's thread may still poll
// it while the program's statics go at exit.
struct Made {
	std::mutex mutex;
	std::shared_ptr<DirectTransport> transport;
};

Made& made() {
	static Made* const state = new Made();
	return *state;
}

} // namespace

std::shared_ptr<DirectTransport> DirectTransport::shared(const noise::KeyPair& self, std::shared_ptr<link::Relay> relay) {
	Made& state = made();
	const std::lock_guard<std::mutex> lock(state.mutex);
	if (state.transport) return state.transport;
	link::DirectLink::Options options;
	options.self = self;
	options.port = kPreferredPort;
	options.stunServers = {"stun.l.google.com:19302", "stun.cloudflare.com:3478"};
	options.relay = std::move(relay);
	try {
		state.transport = std::make_shared<DirectTransport>(options);
	} catch (const std::exception&) {
		// Another program holds the port: a second Singularity, or someone else's.
		options.port = 0;
		state.transport = std::make_shared<DirectTransport>(options);
	}
	return state.transport;
}

std::shared_ptr<DirectTransport> DirectTransport::existing() {
	Made& state = made();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.transport;
}

bool DirectTransport::admitted(const link::PeerKey& key) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	if (refused_.count(key) > 0) return false;
	if (admitted_.count(key) > 0) return true;
	// A lobby open for any Matter answers a computer no card named: what it
	// sends is then taken only as a request to join (takesAsRequest).
	return std::any_of(lobbies_.begin(), lobbies_.end(), [this](const auto& lobby) { return isLobbyOpen(lobby.first); });
}

bool DirectTransport::isLobbyOpen(const std::string& matterId) const {
	const auto lobby = lobbies_.find(matterId);
	if (lobby == lobbies_.end()) return false;
	const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	return now < lobby->second;
}

bool DirectTransport::takesAsRequest(const std::string& matterId, const link::PeerKey& from, const SealedBytes& bytes) {
	if (refused_.count(from) > 0 || isLobbyOpen(matterId) == false) return false;
	const std::string envelope(bytes.begin(), bytes.end());
	if (seal::kind_of(envelope) != seal::Kind::JoinRequest) return false;
	// Only what the request says of itself is read here; its signature is the
	// engine's to check. It is taken only from the computer whose X25519 key it
	// names: the key the call was made with.
	const std::optional<seal::JoinRequest> request =
		seal::open_join_request(envelope, [](const std::string&, const std::string&, const std::string&) { return true; });
	if (request.has_value() == false || request->matter_id != matterId || request->coordination_key != from) return false;
	std::map<std::string, link::PeerKey>& asking = asking_[matterId];
	if (asking.count(request->device_id) == 0 && asking.size() >= kMostAsking) return false;
	asking[request->device_id] = from;
	return true;
}

void DirectTransport::open_lobby(const std::string& matterId, std::int64_t untilSeconds) {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::int64_t& until = lobbies_[matterId];
	until = std::max(until, untilSeconds);
}

void DirectTransport::refuse(const link::PeerKey& key) {
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		refused_.insert(key);
		for (auto& [matter, devices] : asking_) {
			static_cast<void>(matter);
			for (auto device = devices.begin(); device != devices.end();) device = device->second == key ? devices.erase(device) : std::next(device);
		}
	}
	link_.forget(key);
}

void DirectTransport::learn(const std::string& matterId, const Peer& peer) {
	if (matterId.empty() == true || peer.device_id.empty() == true) return;
	std::vector<std::string> addresses;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		// A computer the lawyer here refused is not learned of again.
		if (refused_.count(peer.key) > 0) return;
		peers_[matterId][peer.device_id] = peer;
		asking_[matterId].erase(peer.device_id);
		// A computer whose key changed is spoken with only by its new one.
		admitted_.clear();
		for (const auto& [matter, devices] : peers_) {
			static_cast<void>(matter);
			for (const auto& [device, known] : devices) {
				static_cast<void>(device);
				admitted_.insert(known.key);
				// Where it may be reached, by every Matter that named it: one that
				// names it without addresses - a computer let in, known by the key
				// its request named - never takes away what another Matter's card
				// said (live, 2026-10-26: Amsterdam's left the PC no way to call the
				// Mac that Halden's invitation had given).
				if (known.key != peer.key) continue;
				for (const std::string& candidate : known.candidates) {
					const std::string address = addressOf(candidate);
					if (address.empty() == false && std::find(addresses.begin(), addresses.end(), address) == addresses.end())
						addresses.push_back(address);
				}
			}
		}
	}
	link_.set_candidates(peer.key, std::move(addresses));
}

std::vector<std::string> DirectTransport::connected_devices(const std::string& matterId) const {
	std::vector<std::pair<std::string, link::PeerKey>> known;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = peers_.find(matterId);
		if (found == peers_.end()) return {};
		for (const auto& [device, peer] : found->second) known.emplace_back(device, peer.key);
	}
	// Asked of the link with mutex_ released: the link takes its own lock.
	std::vector<std::string> connected;
	for (const auto& [device, key] : known) {
		if (link_.is_connected(key) == true) connected.push_back(device);
	}
	return connected;
}

std::vector<std::string> DirectTransport::candidates() const {
	std::vector<std::string> found;
	const std::string port = std::to_string(link_.local_port());
	for (const std::string& host : platform::local_addresses()) {
		const bool isSix = host.find(':') != std::string::npos;
		found.push_back(std::string(kScheme) + (isSix ? "[" + host + "]" : host) + ":" + port);
	}
	// And the address its router shows outside, as a STUN server saw it: a
	// computer behind another router calls it there.
	for (const std::string& address : link_.public_addresses()) {
		const std::string candidate = std::string(kScheme) + address;
		if (std::find(found.begin(), found.end(), candidate) == found.end()) found.push_back(candidate);
	}
	return found;
}

void DirectTransport::send(const std::string& matterId, const SealedBytes& bytes, const std::vector<std::string>& toDevices) {
	std::vector<std::pair<std::string, link::PeerKey>> targets;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto matter = peers_.find(matterId);
		if (matter == peers_.end()) return;
		for (const auto& [device, peer] : matter->second) {
			const bool isFor = toDevices.empty() == true || std::find(toDevices.begin(), toDevices.end(), device) != toDevices.end();
			const std::size_t queued = queued_.count(peer.key) > 0 ? queued_.at(peer.key) : 0;
			// A computer that takes nothing in now is sent no more than the bound
			// holds: the firm's folder carries the rest.
			if (isFor == true && queued + bytes.size() <= kMostQueuedBytes) {
				targets.emplace_back(device, peer.key);
				queued_[peer.key] = queued + bytes.size();
			}
		}
	}
	const link::Bytes frame = frameOf(matterId, bytes);
	for (const auto& [device, key] : targets) {
		const std::uint64_t id = link_.send(key, frame);
		const std::lock_guard<std::mutex> lock(mutex_);
		Sending& flow = sending_[{matterId, device}];
		// The last transfer is over once all of it was confirmed: this message
		// starts the next from nothing.
		if (flow.messages > 0 && flow.confirmed_messages >= flow.messages) flow = Sending{};
		flow.key = key;
		flow.messages += 1;
		flow.bytes += bytes.size();
		const Sent sent{key, bytes.size(), matterId, device};
		if (early_.erase(id) > 0) confirm(sent);
		else sent_[id] = sent;
	}
}

// A message the other computer holds whole: no longer queued for it, and
// counted as come in its transfer.
void DirectTransport::confirm(const Sent& sent) {
	settle(sent.key, sent.size);
	const auto flow = sending_.find({sent.matter_id, sent.device_id});
	if (flow == sending_.end()) return;
	flow->second.confirmed_messages += 1;
	flow->second.confirmed_bytes += sent.size;
	if (flow->second.confirmed_messages >= flow->second.messages) flow->second.settled_at = std::chrono::steady_clock::now();
}

std::vector<DirectTransport::Flow> DirectTransport::flows(const std::string& matterId) const {
	std::vector<std::pair<Flow, link::PeerKey>> found;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto now = std::chrono::steady_clock::now();
		for (const auto& [place, flow] : sending_) {
			if (place.first != matterId || flow.messages == 0) continue;
			// A transfer all confirmed is shown a little while, then forgotten.
			if (flow.confirmed_messages >= flow.messages && now - flow.settled_at > kFlowKept) continue;
			Flow shown;
			shown.device_id = place.second;
			shown.messages = flow.messages;
			shown.bytes = flow.bytes;
			shown.confirmed_messages = flow.confirmed_messages;
			shown.confirmed_bytes = flow.confirmed_bytes;
			found.emplace_back(std::move(shown), flow.key);
		}
	}
	// Asked of the link with mutex_ released: the link takes its own lock.
	std::vector<Flow> result;
	for (auto& [flow, key] : found) {
		flow.reachable = link_.is_connected(key);
		result.push_back(std::move(flow));
	}
	return result;
}

std::uint64_t DirectTransport::waiting_bytes(const std::string& matterId) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto waiting = waiting_.find(matterId);
	if (waiting == waiting_.end()) return 0;
	std::uint64_t total = 0;
	for (const auto& [id, bytes] : waiting->second.byId) {
		static_cast<void>(id);
		total += bytes.size();
	}
	return total;
}

void DirectTransport::settle(const link::PeerKey& key, std::size_t size) {
	auto queued = queued_.find(key);
	if (queued == queued_.end()) return;
	queued->second = queued->second > size ? queued->second - size : 0;
	if (queued->second == 0) queued_.erase(queued);
}

void DirectTransport::takeArrivals() {
	std::vector<link::DirectLink::Arrival> arrivals = link_.take_arrivals();
	const std::vector<link::DirectLink::Confirmation> confirmations = link_.take_confirmed();
	const std::lock_guard<std::mutex> lock(mutex_);
	for (const link::DirectLink::Confirmation& confirmation : confirmations) {
		const auto sent = sent_.find(confirmation.id);
		// Confirmed before send() wrote it down: settled when it does.
		if (sent == sent_.end()) {
			early_.insert(confirmation.id);
			continue;
		}
		confirm(sent->second);
		sent_.erase(sent);
	}
	for (link::DirectLink::Arrival& arrival : arrivals) {
		std::string matterId;
		SealedBytes bytes;
		if (readFrame(arrival.message, matterId, bytes) == false) continue;
		// Taken for a Matter whose checked card named the computer that sent it;
		// from any other, only as a request to join through an open lobby.
		const auto matter = peers_.find(matterId);
		const bool isNamed = matter != peers_.end() && std::any_of(matter->second.begin(), matter->second.end(),
			[&](const auto& entry) { return entry.second.key == arrival.from; });
		if (isNamed == false && takesAsRequest(matterId, arrival.from, bytes) == false) continue;
		Waiting& waiting = waiting_[matterId];
		const std::string id = envelope_id(bytes);
		if (waiting.byId.count(id) > 0) continue;
		waiting.byId.emplace(id, std::move(bytes));
		// What just came goes first: a key, a request to join.
		waiting.order.push_front(id);
		while (waiting.order.size() > kMostWaiting) {
			waiting.byId.erase(waiting.order.back());
			waiting.order.pop_back();
		}
	}
}

std::vector<SealedBytes> DirectTransport::poll(const std::string& matterId) {
	takeArrivals();
	const std::lock_guard<std::mutex> lock(mutex_);
	std::vector<SealedBytes> found;
	const auto waiting = waiting_.find(matterId);
	if (waiting == waiting_.end()) return found;
	// What is offered goes to the back: what still waits after it is offered in
	// turn, and what comes next goes before both.
	const std::size_t count = std::min(kMostPerPoll, waiting->second.order.size());
	for (std::size_t index = 0; index < count; ++index) {
		const std::string id = waiting->second.order.front();
		waiting->second.order.pop_front();
		waiting->second.order.push_back(id);
		found.push_back(waiting->second.byId.at(id));
	}
	return found;
}

void DirectTransport::ack(const std::string& matterId, const std::string& envelopeId) {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto waiting = waiting_.find(matterId);
	if (waiting == waiting_.end() || waiting->second.byId.erase(envelopeId) == 0) return;
	auto& order = waiting->second.order;
	order.erase(std::remove(order.begin(), order.end(), envelopeId), order.end());
}

std::string DirectTransport::name() const {
	return "Direct link";
}

} // namespace singularity::matter::coordination::transport
