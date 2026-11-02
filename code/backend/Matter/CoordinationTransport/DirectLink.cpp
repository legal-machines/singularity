#include "Matter/CoordinationTransport/DirectLink.hpp"

#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Trace/Trace.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace singularity::matter::coordination::transport::link {
namespace {

// The three datagrams: a call (the handshake's first message), its answer,
// and data under a session's keys.
constexpr std::uint8_t kCall = 1;
constexpr std::uint8_t kAnswer = 2;
constexpr std::uint8_t kData = 3;
constexpr std::size_t kKey = 32;
constexpr std::size_t kTag = 16;
// A call says when it was made and which run of its program made it; the
// answer says which run of the other's answers.
constexpr std::size_t kCallPayload = 8 + 8;
constexpr std::size_t kAnswerPayload = 8;
// [kind][caller's index 4][e 32][s sealed 48][payload sealed]
constexpr std::size_t kCallSize = 1 + 4 + kKey + (kKey + kTag) + (kCallPayload + kTag);
// [kind][answerer's index 4][caller's index 4][e 32][payload sealed]
constexpr std::size_t kAnswerSize = 1 + 4 + 4 + kKey + (kAnswerPayload + kTag);
// [kind][receiver's index 4][counter 8]: what a data datagram says outside its
// seal, and what the seal binds as its associated data.
constexpr std::size_t kDataHeader = 1 + 4 + 8;
// A call is tried every second at first, then every five.
constexpr std::chrono::milliseconds kCallQuickly{1000};
constexpr std::chrono::milliseconds kCallLater{5000};
constexpr int kQuickCalls = 10;
// The most datagrams handled before the timers are looked at.
constexpr int kMostPerTurn = 256;

void put32(Bytes& out, std::uint32_t value) {
	for (int index = 0; index < 4; ++index) out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFF));
}

void put64(Bytes& out, std::uint64_t value) {
	for (int index = 0; index < 8; ++index) out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFF));
}

std::uint32_t read32(const Bytes& bytes, std::size_t at) {
	std::uint32_t value = 0;
	for (std::size_t index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes[at + index]) << (8 * index);
	return value;
}

std::uint64_t read64(const Bytes& bytes, std::size_t at) {
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[at + index]) << (8 * index);
	return value;
}

Bytes prologue() {
	const std::string text = DirectLink::kPrologue;
	return Bytes(text.begin(), text.end());
}

noise::KeyPair freshKeys() {
	noise::Key secret{};
	platform::fill_secure_random(secret.data(), secret.size());
	return noise::key_pair_from_secret(secret);
}

std::uint64_t randomNumber() {
	Bytes bytes(8, 0);
	platform::fill_secure_random(bytes.data(), bytes.size());
	return read64(bytes, 0);
}

} // namespace

bool ReplayWindow::isSet(std::uint64_t counter) const {
	const std::uint64_t position = counter % kSize;
	return ((bits_[position / 64] >> (position % 64)) & 1u) != 0;
}

void ReplayWindow::set(std::uint64_t counter, bool value) {
	const std::uint64_t position = counter % kSize;
	if (value == true) bits_[position / 64] |= std::uint64_t{1} << (position % 64);
	else bits_[position / 64] &= ~(std::uint64_t{1} << (position % 64));
}

bool ReplayWindow::accept(std::uint64_t counter) {
	if (isAny_ == false) {
		isAny_ = true;
		highest_ = counter;
		set(counter, true);
		return true;
	}
	if (counter > highest_) {
		const std::uint64_t ahead = counter - highest_;
		if (ahead >= kSize) {
			bits_.fill(0);
		} else {
			for (std::uint64_t step = 1; step <= ahead; ++step) set(highest_ + step, false);
		}
		highest_ = counter;
		set(counter, true);
		return true;
	}
	if (highest_ - counter >= kSize) return false;
	if (isSet(counter) == true) return false;
	set(counter, true);
	return true;
}

DirectLink::DirectLink(Options options) : options_(std::move(options)), socket_(options_.port), runId_(randomNumber()) {
	if (!options_.ephemeral) options_.ephemeral = freshKeys;
	thread_ = std::thread([this] { run(); });
	if (options_.stunServers.empty() == false) stunThread_ = std::thread([this] { askStun(); });
}

DirectLink::~DirectLink() {
	{
		const std::lock_guard<std::mutex> lock(stunMutex_);
		isStunStopping_ = true;
	}
	stunWake_.notify_all();
	if (stunThread_.joinable() == true) stunThread_.join();
	isRunning_ = false;
	socket_.close();
	if (thread_.joinable() == true) thread_.join();
}

void DirectLink::askStun() {
	std::unique_lock<std::mutex> wait(stunMutex_);
	while (isStunStopping_ == false) {
		wait.unlock();
		for (const std::string& server : options_.stunServers) {
			const std::size_t colon = server.rfind(':');
			if (colon == std::string::npos || colon + 1 >= server.size()) continue;
			const bool isSix = server.front() == '[' && colon > 1 && server[colon - 1] == ']';
			const std::string host = isSix == true ? server.substr(1, colon - 2) : server.substr(0, colon);
			const std::string portText = server.substr(colon + 1);
			if (portText.size() > 5 || portText.find_first_not_of("0123456789") != std::string::npos) continue;
			const unsigned long port = std::stoul(portText);
			if (port == 0 || port > 65535) continue;
			for (const std::string& address : platform::resolve(host, static_cast<std::uint16_t>(port))) {
				stun::Transaction transaction{};
				platform::fill_secure_random(transaction.data(), transaction.size());
				{
					const std::lock_guard<std::mutex> lock(mutex_);
					const Clock::time_point now = Clock::now();
					// Asked long ago and never answered: given up.
					for (auto asked = stunAsked_.begin(); asked != stunAsked_.end();) {
						asked = now - asked->second > std::chrono::seconds(30) ? stunAsked_.erase(asked) : std::next(asked);
					}
					stunAsked_[transaction] = now;
				}
				static_cast<void>(socket_.send_to(address, stun::binding_request(transaction)));
			}
		}
		wait.lock();
		stunWake_.wait_for(wait, options_.stunEvery, [this] { return isStunStopping_; });
	}
}

void DirectLink::onStun(const platform::UdpSocket::Datagram& datagram, Clock::time_point now) {
	for (auto asked = stunAsked_.begin(); asked != stunAsked_.end(); ++asked) {
		const std::optional<std::string> mapped = stun::mapped_address(datagram.bytes, asked->first);
		if (mapped.has_value() == false) continue;
		publicAddresses_[*mapped] = now;
		stunAsked_.erase(asked);
		return;
	}
}

std::vector<std::string> DirectLink::public_addresses() const {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::vector<std::string> found;
	const Clock::time_point now = Clock::now();
	// What the last two rounds of asking heard: one lost answer loses nothing.
	for (const auto& [address, heard] : publicAddresses_) {
		if (now - heard <= options_.stunEvery * 2 + std::chrono::seconds(30)) found.push_back(address);
	}
	return found;
}

DirectLink::Peer& DirectLink::peerOf(const PeerKey& key) {
	const auto [found, isNew] = peers_.try_emplace(key);
	if (isNew == true) found->second.key = key;
	return found->second;
}

void DirectLink::set_candidates(const PeerKey& peer, std::vector<std::string> addresses) {
	const std::lock_guard<std::mutex> lock(mutex_);
	peerOf(peer).candidates = std::move(addresses);
}

std::uint64_t DirectLink::send(const PeerKey& peer, Bytes message) {
	const std::lock_guard<std::mutex> lock(mutex_);
	Peer& found = peerOf(peer);
	const std::uint64_t id = nextLinkId_++;
	found.linkIds[found.delivery.send(std::move(message))] = id;
	// Where the link is up it goes at once, not on the thread's next turn, which
	// waits on the socket up to 10 ms - 16 on Windows (2026-10-26: each message
	// waited 14 to 21 ms before its first datagram left).
	if (found.session && found.endpoint.empty() == false) {
		const Clock::time_point now = Clock::now();
		for (const Bytes& frame : found.delivery.frames_to_send(now)) sendData(found, frame, now);
	}
	return id;
}

std::vector<DirectLink::Arrival> DirectLink::take_arrivals() {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Arrival> taken;
	taken.swap(arrivals_);
	return taken;
}

std::vector<DirectLink::Confirmation> DirectLink::take_confirmed() {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Confirmation> taken;
	taken.swap(confirmations_);
	return taken;
}

void DirectLink::forget(const PeerKey& key) {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = peers_.find(key);
	if (found == peers_.end()) return;
	Peer& peer = found->second;
	drop(peer.session);
	drop(peer.previous);
	drop(peer.next);
	if (peer.pending.has_value() == true) byIndex_.erase(peer.pending->localIndex);
	peers_.erase(found);
}

bool DirectLink::is_connected(const PeerKey& peer) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = peers_.find(peer);
	return found != peers_.end() && found->second.session != nullptr;
}

std::string DirectLink::endpoint_of(const PeerKey& peer) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = peers_.find(peer);
	return found == peers_.end() ? std::string() : found->second.endpoint;
}

std::size_t DirectLink::sessions_with(const PeerKey& peer) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = peers_.find(peer);
	return found == peers_.end() ? 0 : found->second.sessions;
}

std::uint64_t DirectLink::nextStamp() {
	const std::uint64_t now = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
	lastStamp_ = std::max(lastStamp_ + 1, now);
	return lastStamp_;
}

std::uint32_t DirectLink::newIndex() {
	for (;;) {
		const std::uint32_t index = static_cast<std::uint32_t>(randomNumber());
		if (index != 0 && byIndex_.count(index) == 0) return index;
	}
}

void DirectLink::run() {
	while (isRunning_.load() == true) {
		// Waits without the lock, so send() is never held up by the network.
		std::optional<platform::UdpSocket::Datagram> datagram = socket_.receive(std::chrono::milliseconds(10));
		bool hasArrived = false;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			Clock::time_point now = Clock::now();
			for (int taken = 0; datagram.has_value() == true && taken < kMostPerTurn; ++taken) {
				handle(*datagram, now);
				datagram = socket_.receive(std::chrono::milliseconds(0));
				now = Clock::now();
			}
			if (datagram.has_value() == true) handle(*datagram, now);
			tick(now);
			hasArrived = std::exchange(hasArrived_, false);
		}
		if (hasArrived == true && options_.arrived) options_.arrived();
	}
}

void DirectLink::handle(const platform::UdpSocket::Datagram& datagram, Clock::time_point now) {
	if (datagram.bytes.empty() == true) return;
	// A STUN server's answer shares its first byte with a call: told apart by
	// its cookie and its length.
	if (stun::is_stun(datagram.bytes) == true) {
		onStun(datagram, now);
		return;
	}
	switch (datagram.bytes.front()) {
		case kCall: onCall(datagram, now); break;
		case kAnswer: onAnswer(datagram, now); break;
		case kData: onData(datagram, now); break;
		default: break;
	}
}

void DirectLink::onCall(const platform::UdpSocket::Datagram& datagram, Clock::time_point now) {
	const Bytes& bytes = datagram.bytes;
	if (bytes.size() != kCallSize) return;
	if (now - countedSince_ >= std::chrono::seconds(1)) {
		countedSince_ = now;
		callsCounted_ = 0;
	}
	if (++callsCounted_ > kMostCallsPerSecond) {
		platform::trace::record("link", "call_refused", {{"reason", "rate"}});
		return;
	}
	noise::Responder responder(options_.self, prologue(), options_.ephemeral());
	const std::optional<Bytes> payload = responder.read_first(Bytes(bytes.begin() + 5, bytes.end()));
	if (payload.has_value() == false || payload->size() != kCallPayload) {
		platform::trace::record("link", "call_refused", {{"reason", "unreadable"}, {"from", datagram.from}});
		return;
	}
	// Only a computer the lawyers confirmed is spoken with; any other is not
	// answered at all.
	const PeerKey caller = responder.remote_static();
	if (!options_.admit || options_.admit(caller) == false) {
		platform::trace::record("link", "call_refused", {{"reason", "not_admitted"}, {"from", datagram.from}});
		return;
	}
	Peer& peer = peerOf(caller);
	const std::uint64_t stamp = read64(*payload, 0);
	if (stamp <= peer.lastCall) {
		platform::trace::record("link", "call_refused", {{"reason", "stale_stamp"}, {"from", datagram.from},
			{"stamp", stamp}, {"lastCall", peer.lastCall}});
		return;
	}
	peer.lastCall = stamp;

	auto session = std::make_unique<Session>();
	session->localIndex = newIndex();
	session->remoteIndex = read32(bytes, 1);
	session->peerRun = read64(*payload, 8);
	session->establishedAt = now;
	Bytes answerPayload;
	put64(answerPayload, runId_);
	const Bytes second = responder.write_second(answerPayload);
	session->send = responder.session().send;
	session->receive = responder.session().receive;
	Bytes answer{kAnswer};
	put32(answer, session->localIndex);
	put32(answer, session->remoteIndex);
	answer.insert(answer.end(), second.begin(), second.end());
	sendDatagram(datagram.from, answer);
	platform::trace::record("link", "call_answered", {{"from", datagram.from},
		{"newRun", peer.peerRun.has_value() == false || *peer.peerRun != session->peerRun}});
	// Nothing is sent under it until the caller sends under it: until then
	// nothing shows the caller holds these keys now, and not someone playing
	// back an old call after this computer restarted.
	drop(peer.next);
	byIndex_[session->localIndex] = peer.key;
	peer.next = std::move(session);
}

void DirectLink::onAnswer(const platform::UdpSocket::Datagram& datagram, Clock::time_point now) {
	const Bytes& bytes = datagram.bytes;
	if (bytes.size() != kAnswerSize) return;
	const std::uint32_t callerIndex = read32(bytes, 5);
	const auto owner = byIndex_.find(callerIndex);
	if (owner == byIndex_.end()) return;
	Peer& peer = peers_.at(owner->second);
	if (peer.pending.has_value() == false || peer.pending->localIndex != callerIndex) {
		platform::trace::record("link", "answer_ignored", {{"reason", "no_pending_call"}, {"from", datagram.from}});
		return;
	}
	// Read on a copy: a forged answer leaves the call able to take the real one.
	noise::Initiator attempt = *peer.pending->initiator;
	const std::optional<Bytes> payload = attempt.read_second(Bytes(bytes.begin() + 9, bytes.end()));
	if (payload.has_value() == false || payload->size() != kAnswerPayload) {
		platform::trace::record("link", "answer_ignored", {{"reason", "unreadable"}, {"from", datagram.from}});
		return;
	}
	auto session = std::make_unique<Session>();
	session->localIndex = callerIndex;
	session->remoteIndex = read32(bytes, 1);
	session->send = attempt.session().send;
	session->receive = attempt.session().receive;
	session->isInitiator = true;
	session->peerRun = read64(*payload, 0);
	session->establishedAt = now;
	peer.pending.reset();
	peer.endpoint = datagram.from;
	establish(peer, std::move(session));
	// The first datagram under the new keys tells the other side to take them up.
	sendData(peer, Bytes(), now);
}

void DirectLink::establish(Peer& peer, std::unique_ptr<Session> session) {
	// Another run of the other computer's program: what it held in part is
	// gone with its sessions, so what it did not confirm goes again from the
	// start, and what it sends now is new.
	platform::trace::record("link", "established", {{"initiator", session->isInitiator},
		{"newRun", peer.peerRun.has_value() == true && *peer.peerRun != session->peerRun}, {"endpoint", peer.endpoint}});
	if (peer.peerRun.has_value() == true && *peer.peerRun != session->peerRun) {
		restartDelivery(peer);
		drop(peer.session);
		drop(peer.previous);
	}
	if (peer.next && peer.next->peerRun != session->peerRun) drop(peer.next);
	peer.peerRun = session->peerRun;
	if (peer.pending.has_value() == true) {
		byIndex_.erase(peer.pending->localIndex);
		peer.pending.reset();
	}
	byIndex_[session->localIndex] = peer.key;
	if (peer.session) {
		drop(peer.previous);
		peer.previous = std::move(peer.session);
	}
	peer.session = std::move(session);
	peer.unansweredSince.reset();
	++peer.sessions;
}

void DirectLink::restartDelivery(Peer& peer) {
	std::vector<ReliableDelivery::Unconfirmed> left = peer.delivery.take_unconfirmed();
	std::map<std::uint64_t, std::uint64_t> linkIds;
	peer.delivery = ReliableDelivery();
	for (ReliableDelivery::Unconfirmed& message : left) {
		const auto found = peer.linkIds.find(message.id);
		const std::uint64_t id = peer.delivery.send(std::move(message.bytes));
		if (found != peer.linkIds.end()) linkIds[id] = found->second;
	}
	peer.linkIds = std::move(linkIds);
}

void DirectLink::drop(std::unique_ptr<Session>& session) {
	if (!session) return;
	byIndex_.erase(session->localIndex);
	session.reset();
}

void DirectLink::onData(const platform::UdpSocket::Datagram& datagram, Clock::time_point now) {
	const Bytes& bytes = datagram.bytes;
	if (bytes.size() < kDataHeader + kTag) return;
	const std::uint32_t receiverIndex = read32(bytes, 1);
	const std::uint64_t counter = read64(bytes, 5);
	const auto owner = byIndex_.find(receiverIndex);
	if (owner == byIndex_.end()) {
		// Data under a session this run never made: the other computer still
		// speaks to this one's previous run. Said at most every five seconds.
		if (now - unknownSaidAt_ >= std::chrono::seconds(5)) {
			unknownSaidAt_ = now;
			platform::trace::record("link", "data_unknown_session", {{"from", datagram.from}});
		}
		return;
	}
	Peer& peer = peers_.at(owner->second);
	Session* session = nullptr;
	for (Session* known : {peer.session.get(), peer.previous.get(), peer.next.get()}) {
		if (known != nullptr && known->localIndex == receiverIndex) session = known;
	}
	if (session == nullptr) return;
	const Bytes header(bytes.begin(), bytes.begin() + kDataHeader);
	const std::optional<Bytes> frame = session->receive.decrypt(counter, header, Bytes(bytes.begin() + kDataHeader, bytes.end()));
	if (frame.has_value() == false) return;
	// Played back: the seal holds, but this counter was taken already.
	if (session->replay.accept(counter) == false) return;
	if (session == peer.next.get()) {
		// The caller sends under the keys it was answered with: they are now
		// the ones this computer sends under too.
		std::unique_ptr<Session> taken = std::move(peer.next);
		establish(peer, std::move(taken));
	}
	// Answered where it was last heard from: a laptop that moved keeps its link.
	peer.endpoint = datagram.from;
	peer.unansweredSince.reset();
	if (frame->empty() == true) return;
	peer.delivery.receive(*frame, now);
	collect(peer);
}

void DirectLink::collect(Peer& peer) {
	for (Bytes& message : peer.delivery.take_delivered()) {
		arrivals_.push_back(Arrival{peer.key, std::move(message)});
		hasArrived_ = true;
	}
	for (const std::uint64_t id : peer.delivery.take_confirmed()) {
		const auto found = peer.linkIds.find(id);
		if (found == peer.linkIds.end()) continue;
		confirmations_.push_back(Confirmation{peer.key, found->second});
		peer.linkIds.erase(found);
	}
}

void DirectLink::tick(Clock::time_point now) {
	for (auto& [key, peer] : peers_) {
		static_cast<void>(key);
		// Keys past their time are given up: nothing more is sent or taken
		// under them.
		for (std::unique_ptr<Session>* session : {&peer.session, &peer.previous, &peer.next}) {
			if (*session && now - (*session)->establishedAt >= options_.rejectAfter) drop(*session);
		}
		const bool isWanted = peer.delivery.is_idle() == false || peer.candidates.empty() == false;
		bool needsCall = false;
		const char *reason = "";
		if (isWanted == true) {
			if (!peer.session) {
				needsCall = true;
				reason = "no_session";
			} else if (peer.unansweredSince.has_value() == true && now - *peer.unansweredSince >= options_.silence) {
				needsCall = true;
				reason = "silence";
			} else {
				const auto renewAt = peer.session->isInitiator == true ? options_.rekeyAfter
					: options_.rekeyAfter + (options_.rejectAfter - options_.rekeyAfter) / 2;
				needsCall = now - peer.session->establishedAt >= renewAt;
				reason = "rekey";
			}
		}
		if (needsCall == true) {
			const bool isDue = peer.pending.has_value() == false
				|| now - peer.pending->sentAt >= (peer.pending->tries < kQuickCalls ? kCallQuickly : kCallLater);
			if (isDue == true) {
				platform::trace::record("link", "call", {{"reason", reason}, {"tries", peer.pending.has_value() ? peer.pending->tries + 1 : 1},
					{"targets", peer.candidates.size() + (peer.endpoint.empty() ? 0u : 1u)}, {"inFlight", peer.delivery.in_flight()}});
				call(peer, now);
			}
		}
		if (peer.session && peer.endpoint.empty() == false) {
			for (const Bytes& frame : peer.delivery.frames_to_send(now)) sendData(peer, frame, now);
			collect(peer);
			if (now - peer.lastSent >= options_.keepalive) sendData(peer, Bytes(), now);
		}
	}
}

void DirectLink::call(Peer& peer, Clock::time_point now) {
	// Every address it may be at, the last it was heard from among them: the
	// first to answer is the way.
	std::vector<std::string> targets = peer.candidates;
	if (peer.endpoint.empty() == false && std::find(targets.begin(), targets.end(), peer.endpoint) == targets.end())
		targets.push_back(peer.endpoint);
	if (targets.empty() == true) return;
	const int tries = peer.pending.has_value() == true ? peer.pending->tries + 1 : 1;
	if (peer.pending.has_value() == true) byIndex_.erase(peer.pending->localIndex);
	Pending pending;
	pending.localIndex = newIndex();
	pending.initiator = std::make_unique<noise::Initiator>(options_.self, peer.key, prologue(), options_.ephemeral());
	Bytes payload;
	put64(payload, nextStamp());
	put64(payload, runId_);
	const Bytes first = pending.initiator->write_first(payload);
	Bytes datagram{kCall};
	put32(datagram, pending.localIndex);
	datagram.insert(datagram.end(), first.begin(), first.end());
	for (const std::string& target : targets) sendDatagram(target, datagram);
	pending.sentAt = now;
	pending.tries = tries;
	byIndex_[pending.localIndex] = peer.key;
	peer.pending = std::move(pending);
}

void DirectLink::sendData(Peer& peer, const Bytes& frame, Clock::time_point now) {
	Session& session = *peer.session;
	Bytes datagram{kData};
	put32(datagram, session.remoteIndex);
	put64(datagram, session.sendCounter);
	const Bytes sealed = session.send.encrypt(session.sendCounter, datagram, frame);
	++session.sendCounter;
	datagram.insert(datagram.end(), sealed.begin(), sealed.end());
	sendDatagram(peer.endpoint, datagram);
	peer.lastSent = now;
	// Sent and waiting for an answer: if none comes, the way may be gone.
	if (frame.empty() == false && peer.delivery.in_flight() > 0 && peer.unansweredSince.has_value() == false) peer.unansweredSince = now;
}

void DirectLink::sendDatagram(const std::string& address, const Bytes& bytes) {
	if (options_.testLoss > 0.0) {
		// A fixed sequence, so a test that fails fails the same way again.
		dropCounter_ = dropCounter_ * 6364136223846793005ULL + 1442695040888963407ULL;
		const double chance = static_cast<double>(dropCounter_ >> 11) / static_cast<double>(1ULL << 53);
		if (chance < options_.testLoss) return;
	}
	static_cast<void>(socket_.send_to(address, bytes));
}

} // namespace singularity::matter::coordination::transport::link
