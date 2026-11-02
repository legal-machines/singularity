#include "Matter/CoordinationTransport/ReliableDelivery.hpp"

#include "Platform/Trace/Trace.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace singularity::matter::coordination::transport::link {
namespace {

constexpr std::uint8_t kData = 1;
constexpr std::uint8_t kAcknowledgement = 2;
constexpr std::size_t kDataHeader = 1 + 8 + 4 + 4;
constexpr std::size_t kMostRanges = 64;
constexpr std::chrono::microseconds kLeastTimeout{200000};
constexpr std::chrono::microseconds kMostTimeout{5000000};
constexpr double kLeastWindow = 2.0;
constexpr double kMostWindow = 256.0;

void put64(Bytes& out, std::uint64_t value) {
	for (int index = 0; index < 8; ++index) out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFF));
}

void put32(Bytes& out, std::uint32_t value) {
	for (int index = 0; index < 4; ++index) out.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFF));
}

void put16(Bytes& out, std::uint16_t value) {
	out.push_back(static_cast<std::uint8_t>(value & 0xFF));
	out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

// Reads little-endian numbers from a frame, refusing to read past its end.
class Reader {
public:
	explicit Reader(const Bytes& bytes) : bytes_(bytes) {}
	bool read8(std::uint8_t& value) { return readUnsigned(1, value); }
	bool read16(std::uint16_t& value) { return readUnsigned(2, value); }
	bool read32(std::uint32_t& value) { return readUnsigned(4, value); }
	bool read64(std::uint64_t& value) { return readUnsigned(8, value); }
	std::size_t position() const { return position_; }

private:
	template <typename Number>
	bool readUnsigned(std::size_t size, Number& value) {
		if (bytes_.size() - position_ < size) return false;
		std::uint64_t read = 0;
		for (std::size_t index = 0; index < size; ++index) read |= static_cast<std::uint64_t>(bytes_[position_ + index]) << (8 * index);
		position_ += size;
		value = static_cast<Number>(read);
		return true;
	}
	const Bytes& bytes_;
	std::size_t position_ = 0;
};

} // namespace

ReliableDelivery::ReliableDelivery() = default;

std::uint64_t ReliableDelivery::send(Bytes message) {
	Outgoing outgoing;
	outgoing.id = nextId_++;
	// A message always has a fragment, even when it holds nothing.
	outgoing.count = static_cast<std::uint32_t>(std::max<std::size_t>(1, (message.size() + kFragmentSize - 1) / kFragmentSize));
	outgoing.fragments.resize(outgoing.count);
	outgoing.bytes = std::move(message);
	outgoing.queuedAt = Clock::now();
	outgoing_.push_back(std::move(outgoing));
	return outgoing_.back().id;
}

Bytes ReliableDelivery::dataFrame(const Outgoing& message, std::uint32_t index) const {
	const std::size_t from = static_cast<std::size_t>(index) * kFragmentSize;
	const std::size_t to = std::min(message.bytes.size(), from + kFragmentSize);
	Bytes frame;
	frame.reserve(kDataHeader + (to > from ? to - from : 0));
	frame.push_back(kData);
	put64(frame, message.id);
	put32(frame, index);
	put32(frame, message.count);
	if (to > from) frame.insert(frame.end(), message.bytes.begin() + static_cast<std::ptrdiff_t>(from), message.bytes.begin() + static_cast<std::ptrdiff_t>(to));
	return frame;
}

std::vector<Bytes> ReliableDelivery::frames_to_send(Clock::time_point now) {
	std::vector<Bytes> frames;
	// What the other side is owed first: it frees its own window.
	for (const std::uint64_t id : owed_) frames.push_back(acknowledgementFor(id));
	owed_.clear();

	// Fragments whose time ran out go again; a loss halves the window once and
	// lengthens the wait (RFC 6298's back-off).
	bool isLost = false;
	for (Outgoing& message : outgoing_) {
		for (std::uint32_t index = message.ackedBelow; index < message.sentUpTo; ++index) {
			Fragment& fragment = message.fragments[index];
			if (fragment.acknowledged == true) continue;
			if (now - fragment.sentAt < retransmissionTimeout_) continue;
			fragment.sentAt = now;
			++fragment.tries;
			++message.resends;
			frames.push_back(dataFrame(message, index));
			isLost = true;
		}
	}
	if (isLost == true) {
		window_ = std::max(kLeastWindow, window_ / 2.0);
		retransmissionTimeout_ = std::min(kMostTimeout, retransmissionTimeout_ * 2);
	}

	// New fragments, oldest message first, as the window allows.
	for (Outgoing& message : outgoing_) {
		while (message.sentUpTo < message.count) {
			if (static_cast<double>(inFlight_) >= window_) return frames;
			const std::uint32_t index = message.sentUpTo++;
			if (index == 0) message.firstSentAt = now;
			Fragment& fragment = message.fragments[index];
			fragment.sent = true;
			fragment.sentAt = now;
			fragment.tries = 1;
			++inFlight_;
			frames.push_back(dataFrame(message, index));
		}
	}
	return frames;
}

void ReliableDelivery::receive(const Bytes& frame, Clock::time_point now) {
	if (frame.empty() == true) return;
	if (frame.front() == kData) onData(frame, now);
	else if (frame.front() == kAcknowledgement) onAcknowledgement(frame, now);
}

void ReliableDelivery::onData(const Bytes& frame, Clock::time_point now) {
	Reader reader(frame);
	std::uint8_t kind = 0;
	std::uint64_t id = 0;
	std::uint32_t index = 0;
	std::uint32_t count = 0;
	if (reader.read8(kind) == false || reader.read64(id) == false || reader.read32(index) == false || reader.read32(count) == false) return;
	const std::size_t size = frame.size() - reader.position();
	if (id == 0 || count == 0 || index >= count || size > kMostFragmentSize) return;
	if (static_cast<std::uint64_t>(count) * kFragmentSize > kMostMessageBytes + kFragmentSize) return;
	// Already taken whole: said again, so the other side stops sending it.
	if (isTaken(id) == true) {
		owed_.insert(id);
		return;
	}
	auto found = incoming_.find(id);
	if (found == incoming_.end()) {
		if (incoming_.size() >= kMostPartialMessages) return;
		Incoming made;
		made.count = count;
		made.fragments.resize(count);
		made.held.assign(count, false);
		made.firstAt = now;
		found = incoming_.emplace(id, std::move(made)).first;
	}
	Incoming& message = found->second;
	if (message.count != count) return;
	if (message.held[index] == false) {
		message.fragments[index].assign(frame.begin() + static_cast<std::ptrdiff_t>(reader.position()), frame.end());
		message.held[index] = true;
		++message.heldCount;
		message.bytes += size;
	}
	owed_.insert(id);
	if (message.heldCount < message.count) return;
	Bytes whole;
	whole.reserve(message.bytes);
	for (const Bytes& part : message.fragments) whole.insert(whole.end(), part.begin(), part.end());
	if (platform::trace::enabled() == true) {
		platform::trace::record("link", "delivered", {{"bytes", whole.size()}, {"fragments", message.count},
			{"spreadMs", std::chrono::duration<double, std::milli>(now - message.firstAt).count()}});
	}
	delivered_.push_back(std::move(whole));
	markTaken(id);
	incoming_.erase(found);
}

Bytes ReliableDelivery::acknowledgementFor(std::uint64_t id) const {
	Bytes frame;
	frame.push_back(kAcknowledgement);
	put64(frame, id);
	const auto found = incoming_.find(id);
	if (found == incoming_.end()) {
		// Taken whole: everything is held. The count is not needed then.
		put32(frame, 0);
		put32(frame, 0xFFFFFFFFu);
		put16(frame, 0);
		return frame;
	}
	const Incoming& message = found->second;
	std::uint32_t through = 0;
	while (through < message.count && message.held[through] == true) ++through;
	put32(frame, message.count);
	put32(frame, through);
	std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
	for (std::uint32_t index = through; index < message.count && ranges.size() < kMostRanges;) {
		if (message.held[index] == false) {
			++index;
			continue;
		}
		std::uint32_t end = index;
		while (end < message.count && message.held[end] == true) ++end;
		ranges.emplace_back(index, end);
		index = end;
	}
	put16(frame, static_cast<std::uint16_t>(ranges.size()));
	for (const auto& [begin, end] : ranges) {
		put32(frame, begin);
		put32(frame, end);
	}
	return frame;
}

void ReliableDelivery::onAcknowledgement(const Bytes& frame, Clock::time_point now) {
	Reader reader(frame);
	std::uint8_t kind = 0;
	std::uint64_t id = 0;
	std::uint32_t count = 0;
	std::uint32_t through = 0;
	std::uint16_t rangeCount = 0;
	if (reader.read8(kind) == false || reader.read64(id) == false || reader.read32(count) == false || reader.read32(through) == false
		|| reader.read16(rangeCount) == false || rangeCount > kMostRanges) return;
	std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
	for (std::uint16_t index = 0; index < rangeCount; ++index) {
		std::uint32_t begin = 0;
		std::uint32_t end = 0;
		if (reader.read32(begin) == false || reader.read32(end) == false) return;
		ranges.emplace_back(begin, end);
	}
	const auto message = std::find_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& entry) { return entry.id == id; });
	if (message == outgoing_.end()) return;
	const auto hold = [&](std::uint32_t index) {
		Fragment& fragment = message->fragments[index];
		if (fragment.sent == false || fragment.acknowledged == true) return;
		fragment.acknowledged = true;
		--inFlight_;
		++message->acknowledged;
		// Karn's rule: a fragment sent more than once tells nothing of the round
		// trip.
		if (fragment.tries == 1) measure(std::chrono::duration_cast<std::chrono::microseconds>(now - fragment.sentAt));
		window_ = window_ < 32.0 ? window_ + 1.0 : window_ + 1.0 / window_;
		window_ = std::min(window_, kMostWindow);
	};
	const std::uint32_t heldThrough = std::min(through, message->sentUpTo);
	for (std::uint32_t index = message->ackedBelow; index < heldThrough; ++index) hold(index);
	for (const auto& [begin, end] : ranges) {
		for (std::uint32_t index = std::max(begin, message->ackedBelow); index < end && index < message->sentUpTo; ++index) hold(index);
	}
	while (message->ackedBelow < message->sentUpTo && message->fragments[message->ackedBelow].acknowledged == true) ++message->ackedBelow;
	if (message->acknowledged < message->count) return;
	if (platform::trace::enabled() == true) {
		platform::trace::record("link", "confirmed", {{"bytes", message->bytes.size()}, {"fragments", message->count},
			{"waitedMs", std::chrono::duration<double, std::milli>(message->firstSentAt - message->queuedAt).count()},
			{"roundMs", std::chrono::duration<double, std::milli>(now - message->firstSentAt).count()},
			{"resends", message->resends}, {"window", window_}});
	}
	confirmed_.push_back(message->id);
	outgoing_.erase(message);
}

void ReliableDelivery::measure(std::chrono::microseconds sample) {
	if (smoothedRoundTrip_.has_value() == false) {
		smoothedRoundTrip_ = sample;
		roundTripVariation_ = sample / 2;
	} else {
		const std::chrono::microseconds difference = *smoothedRoundTrip_ > sample ? *smoothedRoundTrip_ - sample : sample - *smoothedRoundTrip_;
		roundTripVariation_ = (roundTripVariation_ * 3 + difference) / 4;
		smoothedRoundTrip_ = (*smoothedRoundTrip_ * 7 + sample) / 8;
	}
	retransmissionTimeout_ = std::clamp(*smoothedRoundTrip_ + std::max(std::chrono::microseconds(1000), roundTripVariation_ * 4),
		kLeastTimeout, kMostTimeout);
}

bool ReliableDelivery::isTaken(std::uint64_t id) const {
	return id <= takenThrough_ || takenAbove_.count(id) > 0;
}

void ReliableDelivery::markTaken(std::uint64_t id) {
	if (id == takenThrough_ + 1) {
		takenThrough_ = id;
		while (takenAbove_.erase(takenThrough_ + 1) > 0) ++takenThrough_;
	} else if (id > takenThrough_) {
		takenAbove_.insert(id);
	}
}

std::vector<Bytes> ReliableDelivery::take_delivered() {
	std::vector<Bytes> taken;
	taken.swap(delivered_);
	return taken;
}

std::vector<std::uint64_t> ReliableDelivery::take_confirmed() {
	std::vector<std::uint64_t> taken;
	taken.swap(confirmed_);
	return taken;
}

std::optional<Clock::time_point> ReliableDelivery::next_deadline() const {
	if (owed_.empty() == false) return Clock::time_point{};
	std::optional<Clock::time_point> earliest;
	for (const Outgoing& message : outgoing_) {
		if (message.sentUpTo < message.count && static_cast<double>(inFlight_) < window_) return Clock::time_point{};
		for (std::uint32_t index = message.ackedBelow; index < message.sentUpTo; ++index) {
			const Fragment& fragment = message.fragments[index];
			if (fragment.acknowledged == true) continue;
			const Clock::time_point due = fragment.sentAt + retransmissionTimeout_;
			if (earliest.has_value() == false || due < *earliest) earliest = due;
		}
	}
	return earliest;
}

bool ReliableDelivery::is_idle() const {
	return outgoing_.empty() == true && owed_.empty() == true;
}

std::vector<ReliableDelivery::Unconfirmed> ReliableDelivery::take_unconfirmed() {
	std::vector<Unconfirmed> taken;
	for (Outgoing& message : outgoing_) taken.push_back(Unconfirmed{message.id, std::move(message.bytes)});
	outgoing_.clear();
	inFlight_ = 0;
	return taken;
}

} // namespace singularity::matter::coordination::transport::link
