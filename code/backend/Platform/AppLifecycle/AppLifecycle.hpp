#pragma once

#include "Platform/HttpClient/HttpClient.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Singularity is closing. The Native Bridge says so first, before it interrupts
// anything. From then on no domain begins new work; a loop between two requests
// ends there and records what it did as interrupted, never as the lawyer's
// Stop; and every token bound to the closing is cancelled, so a provider
// request in flight stops on its connection.
//
// One fact for the whole process, so a domain needs no hook of its own in the
// bridge's shutdown, and nothing here is a copy of another state. Header-only,
// like Platform/Events, so every build that has a domain that asks has it too.
namespace singularity::platform::lifecycle {

namespace detail {

struct State {
	// A leaf: nothing else is locked while it is held.
	std::mutex mutex;
	bool closing = false;
	std::atomic<bool> closing_seen{false};
	std::vector<std::shared_ptr<HttpCancelToken>> tokens;
};

// Never destroyed: a thread still finishing as the process ends may ask.
inline State &state() {
	static State *value = new State();
	return *value;
}

} // namespace detail

inline bool is_closing() noexcept {
	return detail::state().closing_seen.load(std::memory_order_acquire);
}

// Said once; saying it again changes nothing. The bound tokens are cancelled
// outside the lock, since a token runs its request's abort under its own lock.
inline void begin_closing() noexcept {
	try {
		detail::State &state = detail::state();
		std::vector<std::shared_ptr<HttpCancelToken>> bound;
		{
			const std::lock_guard<std::mutex> lock(state.mutex);
			if (state.closing) return;
			state.closing = true;
			state.closing_seen.store(true, std::memory_order_release);
			bound = state.tokens;
		}
		for (const std::shared_ptr<HttpCancelToken> &token : bound) token->cancel();
	} catch (...) {
	}
}

// For as long as it lives, `token` is cancelled when closing begins, or at once
// if it already has.
class CancelOnClosing {
public:
	explicit CancelOnClosing(std::shared_ptr<HttpCancelToken> token) : token_(std::move(token)) {
		if (!token_) return;
		bool isClosing = false;
		{
			detail::State &state = detail::state();
			const std::lock_guard<std::mutex> lock(state.mutex);
			isClosing = state.closing;
			if (!isClosing) state.tokens.push_back(token_);
		}
		if (isClosing) token_->cancel();
	}

	~CancelOnClosing() {
		if (!token_) return;
		try {
			detail::State &state = detail::state();
			const std::lock_guard<std::mutex> lock(state.mutex);
			const auto found = std::find(state.tokens.begin(), state.tokens.end(), token_);
			if (found != state.tokens.end()) state.tokens.erase(found);
		} catch (...) {
		}
	}

	CancelOnClosing(const CancelOnClosing &) = delete;
	CancelOnClosing &operator=(const CancelOnClosing &) = delete;

private:
	std::shared_ptr<HttpCancelToken> token_;
};

// Binds a token of its own to the requests this thread makes, cancelled on
// closing, unless the thread already has one: a chat operation's own Stop
// token, which that operation's interruption cancels, is never replaced.
class ClosingCancelScope {
public:
	ClosingCancelScope() {
		if (current_http_cancel_token()) return;
		token_ = std::make_shared<HttpCancelToken>();
		binding_.emplace(token_);
		scope_.emplace(token_);
	}

	ClosingCancelScope(const ClosingCancelScope &) = delete;
	ClosingCancelScope &operator=(const ClosingCancelScope &) = delete;

private:
	std::shared_ptr<HttpCancelToken> token_;
	std::optional<CancelOnClosing> binding_;
	std::optional<HttpCancelScope> scope_;
};

// Tests only: the process as it was before closing began.
inline void reset_for_testing() noexcept {
	try {
		detail::State &state = detail::state();
		const std::lock_guard<std::mutex> lock(state.mutex);
		state.closing = false;
		state.closing_seen.store(false, std::memory_order_release);
		state.tokens.clear();
	} catch (...) {
	}
}

} // namespace singularity::platform::lifecycle
