#pragma once

#include "Platform/WebSocket/WebSocket.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace singularity::platform {

// What a system's own WebSocket client hands over as it arrives, on its own
// thread, kept until receive() takes it: the macOS and Windows clients.
class WebSocketInbox {
public:
	// The most messages kept unread: past it the connection is broken, as a
	// reader that stopped reading is.
	static constexpr std::size_t kMostUnread = 8192;

	void put(WebSocket::Message message) {
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			if (isEnded_ == true) return;
			if (messages_.size() >= kMostUnread) {
				isEnded_ = true;
				reason_ = "The connection fell behind.";
			} else {
				messages_.push_back(std::move(message));
			}
		}
		wake_.notify_all();
	}

	void end(const std::string& reason) {
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			if (isEnded_ == true) return;
			isEnded_ = true;
			reason_ = reason;
		}
		wake_.notify_all();
	}

	bool is_ended() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		return isEnded_;
	}

	// What came first, waiting up to `wait`; throws once the connection ended
	// and nothing is left unread.
	std::optional<WebSocket::Message> take(std::chrono::milliseconds wait) {
		std::unique_lock<std::mutex> lock(mutex_);
		wake_.wait_for(lock, wait, [this] { return messages_.empty() == false || isEnded_ == true; });
		if (messages_.empty() == false) {
			WebSocket::Message message = std::move(messages_.front());
			messages_.pop_front();
			return message;
		}
		if (isEnded_ == true) throw HttpTransportError(reason_, false);
		return std::nullopt;
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<WebSocket::Message> messages_;
	bool isEnded_ = false;
	std::string reason_;
};

} // namespace singularity::platform
