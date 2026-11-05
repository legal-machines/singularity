#include "Node/WorkLog/WorkLog.hpp"

#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

namespace singularity::node::work_log {

namespace {

using Clock = std::chrono::system_clock;

// How long a call is kept, and how much one chat's record holds before its
// older half goes.
constexpr std::chrono::hours kKeptFor {24 * 14};
constexpr std::uintmax_t kMostBytes = 64ull * 1024 * 1024;

std::mutex &files_lock() {
	static std::mutex lock;
	return lock;
}

std::optional<std::filesystem::path> &named_directory() {
	static std::optional<std::filesystem::path> directory;
	return directory;
}

// -1: not read yet; 0: not kept; 1: kept.
std::atomic<int> &kept_state() {
	static std::atomic<int> state {-1};
	return state;
}

std::optional<std::pair<std::string, std::string>> &chat_in_scope() {
	static thread_local std::optional<std::pair<std::string, std::string>> chat;
	return chat;
}

// When each chat's record was last cleared of what is too old, in this run.
std::map<std::string, Clock::time_point> &last_pruned() {
	static std::map<std::string, Clock::time_point> pruned;
	return pruned;
}

// Called only where named_directory() holds one.
std::filesystem::path directory() { return *named_directory(); }

std::filesystem::path marker() { return directory() / "kept"; }

// One file for each chat, named by a digest of its Matter and its id, so that
// no name of either stands on the disk.
std::filesystem::path file_of(const std::string &matter_id, const std::string &chat_id) {
	return directory() / (platform::sha256_hex_digest(matter_id + "\n" + chat_id).substr(0, 32) + ".jsonl");
}

std::string time_text(Clock::time_point when) {
	const std::time_t seconds = Clock::to_time_t(when);
	std::tm parts {};
#if defined(_WIN32)
	gmtime_s(&parts, &seconds);
#else
	gmtime_r(&seconds, &parts);
#endif
	std::ostringstream out;
	out << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
	return out.str();
}

std::optional<Clock::time_point> time_of(const std::string &text) {
	std::tm parts {};
	std::istringstream in(text);
	in >> std::get_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
	if (in.fail() == true) return std::nullopt;
#if defined(_WIN32)
	const std::time_t seconds = _mkgmtime(&parts);
#else
	const std::time_t seconds = timegm(&parts);
#endif
	if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
	return Clock::from_time_t(seconds);
}

std::string line_of(const Call &call) {
	const nlohmann::json line {
		{"at", call.at},
		{"milliseconds", call.milliseconds},
		{"provider", call.provider_id},
		{"model", call.model_id},
		{"purpose", call.purpose},
		{"protected", call.is_protected},
		{"request", call.request},
		{"answer", call.answer},
		{"failure", call.failure},
		{"inputTokens", call.input_tokens},
		{"outputTokens", call.output_tokens},
	};
	// A body that is not whole UTF-8 is kept with its broken bytes replaced,
	// never refused.
	return line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<Call> call_of(const std::string &line) {
	const nlohmann::json value = nlohmann::json::parse(line, nullptr, false);
	if (value.is_object() == false) return std::nullopt;
	Call call;
	call.at = value.value("at", "");
	call.milliseconds = value.value("milliseconds", 0LL);
	call.provider_id = value.value("provider", "");
	call.model_id = value.value("model", "");
	call.purpose = value.value("purpose", "");
	call.is_protected = value.value("protected", false);
	call.request = value.value("request", "");
	call.answer = value.value("answer", "");
	call.failure = value.value("failure", "");
	call.input_tokens = value.value("inputTokens", -1LL);
	call.output_tokens = value.value("outputTokens", -1LL);
	return call;
}

std::vector<std::string> lines_in(const std::filesystem::path &file) {
	std::vector<std::string> lines;
	std::ifstream in(file, std::ios::binary);
	for (std::string line; std::getline(in, line);)
		if (line.empty() == false) lines.push_back(line);
	return lines;
}

void write_lines(const std::filesystem::path &file, const std::vector<std::string> &lines) {
	const std::filesystem::path next = file.string() + ".next";
	{
		std::ofstream out(next, std::ios::binary | std::ios::trunc);
		for (const std::string &line : lines) out << line << '\n';
	}
	std::error_code ignored;
	std::filesystem::rename(next, file, ignored);
}

// A chat's record without what is older than kKeptFor, and within kMostBytes
// (its newer half kept where it grew past it). Called with files_lock held.
void prune(const std::filesystem::path &file, Clock::time_point now) {
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(file, error);
	auto &pruned = last_pruned()[file.string()];
	const bool too_big = error == std::error_code() && size > kMostBytes;
	if (too_big == false && now - pruned < std::chrono::hours(24)) return;
	pruned = now;
	std::vector<std::string> kept;
	for (const std::string &line : lines_in(file)) {
		const std::optional<Call> call = call_of(line);
		const std::optional<Clock::time_point> at = call.has_value() == true ? time_of(call->at) : std::nullopt;
		if (at.has_value() == true && now - *at <= kKeptFor) kept.push_back(line);
	}
	if (too_big == true && kept.size() > 1) kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(kept.size() / 2));
	write_lines(file, kept);
}

} // namespace

void set_directory(const std::filesystem::path &directory) {
	const std::lock_guard<std::mutex> guard(files_lock());
	named_directory() = directory;
	kept_state().store(-1);
	last_pruned().clear();
}

void set_kept(bool keep) {
	const std::lock_guard<std::mutex> guard(files_lock());
	if (named_directory().has_value() == false) {
		kept_state().store(0);
		return;
	}
	std::error_code ignored;
	if (keep == true) {
		std::filesystem::create_directories(directory(), ignored);
		std::ofstream(marker(), std::ios::trunc) << "kept for a design partner (D-257)\n";
	} else {
		std::filesystem::remove_all(directory(), ignored);
		last_pruned().clear();
	}
	kept_state().store(keep == true ? 1 : 0);
}

bool is_kept() {
	if (named_directory().has_value() == false) return false;
	int state = kept_state().load();
	if (state == -1) {
		std::error_code ignored;
		state = std::filesystem::exists(marker(), ignored) == true ? 1 : 0;
		kept_state().store(state);
	}
	return state == 1;
}

ChatScope::ChatScope(std::string matter_id, std::string chat_id) : previous_(chat_in_scope()) {
	chat_in_scope() = std::make_pair(std::move(matter_id), std::move(chat_id));
}

ChatScope::~ChatScope() { chat_in_scope() = previous_; }

bool wanted() {
	return chat_in_scope().has_value() == true && chat_in_scope()->second.empty() == false && is_kept() == true;
}

void record(const Call &call) {
	if (wanted() == false) return;
	const auto &[matter_id, chat_id] = *chat_in_scope();
	Call kept = call;
	if (kept.at.empty() == true) kept.at = time_text(Clock::now());
	const std::lock_guard<std::mutex> guard(files_lock());
	std::error_code ignored;
	std::filesystem::create_directories(directory(), ignored);
	const std::filesystem::path file = file_of(matter_id, chat_id);
	{
		std::ofstream out(file, std::ios::binary | std::ios::app);
		out << line_of(kept) << '\n';
	}
	prune(file, Clock::now());
}

std::string now_text() { return time_text(Clock::now()); }

std::vector<Call> calls_of(const std::string &matter_id, const std::string &chat_id) {
	const std::lock_guard<std::mutex> guard(files_lock());
	std::vector<Call> calls;
	if (named_directory().has_value() == false) return calls;
	const Clock::time_point now = Clock::now();
	for (const std::string &line : lines_in(file_of(matter_id, chat_id))) {
		std::optional<Call> call = call_of(line);
		if (call.has_value() == false) continue;
		const std::optional<Clock::time_point> at = time_of(call->at);
		if (at.has_value() == true && now - *at <= kKeptFor) calls.push_back(std::move(*call));
	}
	return calls;
}

} // namespace singularity::node::work_log
