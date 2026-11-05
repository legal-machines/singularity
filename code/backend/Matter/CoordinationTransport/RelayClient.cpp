#include "Matter/CoordinationTransport/RelayClient.hpp"

#include "Platform/Trace/Trace.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace singularity::matter::coordination::transport::link {

RelayClient::RelayClient(Options options) : options_(std::move(options)) {
	if (!options_.open) {
		options_.open = [](const std::string& url, const std::vector<platform::HttpHeader>& headers) {
			return platform::open_websocket(url, headers, 15);
		};
	}
	keeper_ = std::thread([this] { keep(); });
	writer_ = std::thread([this] { write(); });
}

RelayClient::~RelayClient() {
	std::shared_ptr<platform::WebSocket> socket;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		isStopping_ = true;
		socket = socket_;
	}
	wake_.notify_all();
	if (socket) socket->close();
	if (keeper_.joinable() == true) keeper_.join();
	if (writer_.joinable() == true) writer_.join();
}

bool RelayClient::is_up() const {
	return isUp_.load();
}

void RelayClient::send(const noise::Key& to, const Bytes& datagram) {
	if (isUp_.load() == false) return;
	std::string message(to.begin(), to.end());
	message.append(datagram.begin(), datagram.end());
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (outgoing_.size() >= kMostWaiting) return;
		outgoing_.push_back(std::move(message));
	}
	wake_.notify_all();
}

std::vector<Relay::Datagram> RelayClient::take() {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Datagram> taken;
	taken.swap(incoming_);
	return taken;
}

bool RelayClient::rest(std::chrono::milliseconds wait) {
	std::unique_lock<std::mutex> lock(mutex_);
	wake_.wait_for(lock, wait, [this] { return isStopping_.load(); });
	return isStopping_.load();
}

void RelayClient::keep() {
	std::chrono::milliseconds broken{1000};
	while (isStopping_.load() == false) {
		const std::vector<platform::HttpHeader> headers = options_.headers ? options_.headers() : std::vector<platform::HttpHeader>();
		if (headers.empty() == true) {
			if (rest(std::chrono::seconds(10)) == true) return;
			continue;
		}
		std::shared_ptr<platform::WebSocket> socket;
		try {
			socket = options_.open(options_.url, headers);
		} catch (const std::exception&) {
			platform::trace::record("relay", "unreachable", {});
			if (rest(broken) == true) return;
			broken = std::min(broken * 2, options_.longestWait);
			continue;
		}
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			if (isStopping_.load() == true) {
				socket->close();
				return;
			}
			socket_ = socket;
		}
		bool wasRefused = false;
		bool wasReady = false;
		auto heard = std::chrono::steady_clock::now();
		auto asked = heard;
		try {
			while (isStopping_.load() == false) {
				const auto now = std::chrono::steady_clock::now();
				if (now - heard >= (wasReady == true ? options_.deadAfter : std::chrono::milliseconds(20000))) {
					platform::trace::record("relay", "silent", {{"ready", wasReady}});
					break;
				}
				if (wasReady == true && now - asked >= options_.signOfLife) {
					asked = now;
					{
						const std::lock_guard<std::mutex> lock(mutex_);
						isPingWanted_ = true;
					}
					wake_.notify_all();
				}
				const std::optional<platform::WebSocket::Message> message = socket->receive(std::chrono::milliseconds(500));
				if (message.has_value() == false) continue;
				heard = std::chrono::steady_clock::now();
				if (message->isText == true) {
					if (message->data == "ready") {
						wasReady = true;
						isUp_ = true;
						broken = std::chrono::milliseconds(1000);
						asked = heard;
						platform::trace::record("relay", "up", {});
					} else if (message->data == "refused") {
						wasRefused = true;
						break;
					}
					continue;
				}
				if (message->data.size() <= 32) continue;
				Datagram datagram;
				std::copy(message->data.begin(), message->data.begin() + 32, datagram.from.begin());
				datagram.bytes.assign(message->data.begin() + 32, message->data.end());
				const std::lock_guard<std::mutex> lock(mutex_);
				if (incoming_.size() < kMostWaiting) incoming_.push_back(std::move(datagram));
			}
		} catch (const std::exception&) {
			platform::trace::record("relay", "broken", {{"ready", wasReady}});
		}
		isUp_ = false;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			socket_.reset();
			outgoing_.clear();
			isPingWanted_ = false;
		}
		socket->close();
		if (wasRefused == true) {
			platform::trace::record("relay", "refused", {});
			if (rest(options_.refusedWait) == true) return;
			continue;
		}
		if (rest(broken) == true) return;
		broken = std::min(broken * 2, options_.longestWait);
	}
}

void RelayClient::write() {
	for (;;) {
		std::string message;
		bool isPing = false;
		std::shared_ptr<platform::WebSocket> socket;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] {
				return isStopping_.load() == true || ((outgoing_.empty() == false || isPingWanted_ == true) && socket_);
			});
			if (isStopping_.load() == true) return;
			isPing = std::exchange(isPingWanted_, false);
			if (isPing == false) {
				message = std::move(outgoing_.front());
				outgoing_.pop_front();
			}
			socket = socket_;
		}
		try {
			if (isPing == true) socket->send_text("ping");
			else socket->send_binary(message);
		} catch (const std::exception&) {
			// The keeper sees the broken connection on its next receive.
		}
	}
}

} // namespace singularity::matter::coordination::transport::link
