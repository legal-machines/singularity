#pragma once

// A timing trace of what Singularity itself does: which native call ran, how long
// it waited for a worker, how long it held the persistence lock, what each
// stage of a chat operation cost. Off unless SINGULARITY_TRACE_FILE names a file;
// then every record is one JSON line appended to it.
//
// The trace is diagnostic metadata only - names, ids, counts, byte sizes,
// durations, short content fingerprints. Never document text, a prompt, a
// detected value, a credential or an Authorization header: a call site that
// holds such content records its size or fingerprint() of it, nothing else.
//
// Header-only and dependency-free on purpose, so every target - including the
// small privacy test targets - can include it without build changes. When the
// trace is off a call site costs one atomic read.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::platform::trace {

// One recorded fact. Numbers and booleans are written unquoted so the file
// can be aggregated without parsing strings.
struct Field {
	std::string key;
	std::string value;
	bool quoted = true;

	Field(std::string name, std::string text) : key(std::move(name)), value(std::move(text)) {}
	Field(std::string name, const char *text) : key(std::move(name)), value(text == nullptr ? "" : text) {}
	Field(std::string name, std::string_view text) : key(std::move(name)), value(text) {}
	Field(std::string name, bool flag) : key(std::move(name)), value(flag ? "true" : "false"), quoted(false) {}
	Field(std::string name, double number) : key(std::move(name)), quoted(false) {
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.3f", number);
		value = buffer;
	}
	template <typename Integer, typename = std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>>>
	Field(std::string name, Integer number) : key(std::move(name)), value(std::to_string(number)), quoted(false) {}
};

using Fields = std::vector<Field>;

namespace detail {

struct State {
	std::mutex mutex;
	std::ofstream file;
	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	std::unordered_map<std::thread::id, int> threads;
	std::atomic<std::uint64_t> sequence{0};
};

inline State &state() {
	static State *value = new State();
	return *value;
}

inline std::atomic<int> &enabled_flag() {
	// 0 = not decided yet, 1 = on, -1 = off.
	static std::atomic<int> flag{0};
	return flag;
}

inline bool decide_enabled() {
	const char *path = std::getenv("SINGULARITY_TRACE_FILE");
	if (path == nullptr || *path == '\0') {
		enabled_flag().store(-1);
		return false;
	}
	State &shared = state();
	const std::lock_guard<std::mutex> lock(shared.mutex);
	if (enabled_flag().load() == 1) return true;
	shared.file.open(path, std::ios::binary | std::ios::app);
	if (!shared.file) {
		enabled_flag().store(-1);
		return false;
	}
	enabled_flag().store(1);
	return true;
}

inline void append_escaped(std::string &out, std::string_view text) {
	for (const char character : text) {
		const auto byte = static_cast<unsigned char>(character);
		switch (character) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (byte < 0x20) {
					char buffer[8];
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
					out += buffer;
				} else {
					out += character;
				}
		}
	}
}

// The calling thread's label, kept inside a function rather than as an inline
// thread_local variable: MinGW's GCC gives such a variable an initialiser in
// every file that uses it, and the Windows link refuses the copies.
inline std::string &thread_label_value() {
	thread_local std::string label;
	return label;
}

} // namespace detail

inline bool enabled() noexcept {
	const int flag = detail::enabled_flag().load(std::memory_order_relaxed);
	if (flag != 0) return flag == 1;
	try {
		return detail::decide_enabled();
	} catch (...) {
		return false;
	}
}

// Names the calling thread in every record it writes ("bridge-worker").
inline void set_thread_label(std::string label) {
	detail::thread_label_value() = std::move(label);
}

// Milliseconds since this process started tracing.
inline double now_ms() {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - detail::state().started).count();
}

// A short, non-reversible fingerprint of content the trace must not contain,
// so two sightings of the same text can be recognised as the same text.
inline std::string fingerprint(std::string_view content) {
	std::uint64_t hash = 1469598103934665603ULL;
	for (const char character : content) {
		hash ^= static_cast<unsigned char>(character);
		hash *= 1099511628211ULL;
	}
	char buffer[24];
	std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
	return buffer;
}

inline void record(std::string_view category, std::string_view name, const Fields &fields = {},
		double duration_ms = -1.0) {
	if (!enabled()) return;
	try {
		detail::State &shared = detail::state();
		const double at = now_ms();
		const auto wall = std::chrono::system_clock::now();
		const std::time_t seconds = std::chrono::system_clock::to_time_t(wall);
		const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000;
		std::tm local{};
#if defined(_WIN32)
		localtime_s(&local, &seconds);
#else
		localtime_r(&seconds, &local);
#endif
		char clock[32];
		std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
			static_cast<int>(millis));

		std::string line;
		line.reserve(192 + fields.size() * 32);
		char number[64];
		line += "{\"seq\":";
		line += std::to_string(shared.sequence.fetch_add(1) + 1);
		std::snprintf(number, sizeof(number), "%.3f", at);
		line += ",\"t\":";
		line += number;
		line += ",\"clock\":\"";
		line += clock;
		line += "\",\"cat\":\"";
		detail::append_escaped(line, category);
		line += "\",\"name\":\"";
		detail::append_escaped(line, name);
		line += "\"";
		if (duration_ms >= 0.0) {
			std::snprintf(number, sizeof(number), "%.3f", duration_ms);
			line += ",\"ms\":";
			line += number;
		}
		for (const Field &field : fields) {
			line += ",\"";
			detail::append_escaped(line, field.key);
			line += "\":";
			if (field.quoted) {
				line += "\"";
				detail::append_escaped(line, field.value);
				line += "\"";
			} else {
				line += field.value.empty() ? "null" : field.value;
			}
		}

		const std::lock_guard<std::mutex> lock(shared.mutex);
		const auto inserted = shared.threads.emplace(std::this_thread::get_id(), static_cast<int>(shared.threads.size()) + 1);
		line += ",\"thread\":";
		line += std::to_string(inserted.first->second);
		if (!detail::thread_label_value().empty()) {
			line += ",\"threadLabel\":\"";
			detail::append_escaped(line, detail::thread_label_value());
			line += "\"";
		}
		line += "}\n";
		shared.file << line;
		shared.file.flush();
	} catch (...) {
		// A trace that cannot be written never affects the work it describes.
	}
}

// Times a scope. The record is written when the scope ends, carrying every
// field added along the way; `announce` also writes a record when it starts,
// so work that never finishes is visible in the file.
class Span final {
public:
	Span(std::string_view category, std::string_view name, Fields fields = {}, bool announce = false)
		: active_(enabled()) {
		if (!active_) return;
		category_ = category;
		name_ = name;
		fields_ = std::move(fields);
		started_ = std::chrono::steady_clock::now();
		if (announce) {
			Fields begin = fields_;
			begin.emplace_back("phase", "begin");
			record(category_, name_, begin);
		}
	}
	~Span() { finish(); }
	Span(const Span &) = delete;
	Span &operator=(const Span &) = delete;

	[[nodiscard]] bool active() const noexcept { return active_; }

	template <typename Value>
	void add(std::string key, Value &&value) {
		if (active_) fields_.emplace_back(std::move(key), std::forward<Value>(value));
	}

	[[nodiscard]] double elapsed_ms() const {
		if (!active_) return 0.0;
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_).count();
	}

	void finish() {
		if (!active_) return;
		active_ = false;
		record(category_, name_, fields_,
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_).count());
	}

private:
	bool active_ = false;
	std::string category_;
	std::string name_;
	Fields fields_;
	std::chrono::steady_clock::time_point started_;
};

} // namespace singularity::platform::trace
