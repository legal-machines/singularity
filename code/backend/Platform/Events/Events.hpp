#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// What changed, said once to whoever listens: the window today, through the
// Native Bridge, and another transport later, such as the firm's always-on home
// (#275). A domain publishes the fact that something it owns changed: ids and
// states only, never a document's words. A listener reads again what it shows,
// so nobody has to poll to find out.
//
// Published today.
// - "matter.changed" {projectId}: a Matter's saved record changed (its chats,
//   operations, documents, Work Product, tables);
// - "agent.changed" {agentId, matterId}: an AI colleague, one of its runs, or
//   work given to it changed;
// - "operation.changed" {projectId, operationId, state}: a running operation
//   moved on, its live steps included.
//
// Header-only, so every build that has a publishing domain has it too.
namespace singularity::platform::events {

using Sink = std::function<void(const std::string &name, const nlohmann::json &payload)>;

namespace detail {
inline std::mutex &sink_mutex() {
	static std::mutex mutex;
	return mutex;
}
inline Sink &sink() {
	static Sink installed;
	return installed;
}
inline std::vector<Sink> &observers() {
	static std::vector<Sink> kept;
	return kept;
}
} // namespace detail

// The application installs its listener at launch, and an empty one at
// shutdown.
inline void set_sink(Sink sink) {
	const std::lock_guard<std::mutex> lock(detail::sink_mutex());
	detail::sink() = std::move(sink);
}

// A domain that acts on what the others publish observes it for as long as
// the program runs: the firm's coordination sends a Matter's changes to its
// other computers at once. An observer returns at once, and never publishes
// what it observes.
inline void observe(Sink observer) {
	const std::lock_guard<std::mutex> lock(detail::sink_mutex());
	detail::observers().push_back(std::move(observer));
}

// Never throws, and never waits on the listener's own work: a domain may
// publish while it holds its locks.
inline void publish(const std::string &name, const nlohmann::json &payload) noexcept {
	Sink listener;
	std::vector<Sink> observers;
	try {
		const std::lock_guard<std::mutex> lock(detail::sink_mutex());
		listener = detail::sink();
		observers = detail::observers();
	} catch (...) {
		return;
	}
	if (listener) {
		try {
			listener(name, payload);
		} catch (...) {
		}
	}
	for (const Sink &observer : observers) {
		try {
			observer(name, payload);
		} catch (...) {
		}
	}
}

} // namespace singularity::platform::events
