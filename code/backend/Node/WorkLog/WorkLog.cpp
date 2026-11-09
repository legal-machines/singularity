#include "Node/WorkLog/WorkLog.hpp"

#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// The Matters whose folders keep their chats' calls, and this computer's tag
// for its own calls' files there. Read and written with files_lock held.
std::optional<MatterRecords> &matter_records() {
	static std::optional<MatterRecords> records;
	return records;
}

std::string &own_tag() {
	static std::string tag;
	return tag;
}

// A file being written into a Matter's folder: the Matter's exchange never
// carries a name with this in it (MatterCoordination travels()).
constexpr const char *kWriting = ".lm-tmp-";

// Called only where named_directory() holds one.
std::filesystem::path directory() { return *named_directory(); }

std::filesystem::path marker() { return directory() / "kept"; }

// One file for each chat, named by a digest of its Matter and its id, so that
// no name of either stands on the disk.
std::string chat_key(const std::string &matter_id, const std::string &chat_id) {
	return platform::sha256_hex_digest(matter_id + "\n" + chat_id).substr(0, 32);
}

std::filesystem::path file_of(const std::string &matter_id, const std::string &chat_id) {
	return directory() / (chat_key(matter_id, chat_id) + ".jsonl");
}

// A Matter's chat's calls in its folder: one file a call, each computer's
// named by its own tag.
std::filesystem::path matter_dir_of(const std::filesystem::path &folder, const std::string &matter_id, const std::string &chat_id) {
	return folder / ".lm" / "WorkLog" / chat_key(matter_id, chat_id);
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

// "2026-11-06T10:13:38Z" as a file's name begins: "20261106T101338Z".
std::string stamp_of(const std::string &at) {
	std::string stamp;
	for (const char character : at)
		if (character != '-' && character != ':') stamp += character;
	return stamp;
}

// When a call's file says it was made, from the first 16 letters of its name.
std::optional<Clock::time_point> time_of_name(const std::string &name) {
	if (name.size() < 16 || name[8] != 'T' || name[15] != 'Z') return std::nullopt;
	return time_of(name.substr(0, 4) + "-" + name.substr(4, 2) + "-" + name.substr(6, 2) + "T" + name.substr(9, 2) + ":"
		+ name.substr(11, 2) + ":" + name.substr(13, 2) + "Z");
}

bool is_call_file(const std::string &name) {
	return name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0 && name.find(kWriting) == std::string::npos;
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
		{"device", call.device},
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
	call.device = value.value("device", "");
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

// A call into its Matter's folder, as one file whole: written under a name the
// exchange leaves alone, then given its own. Called with files_lock held.
void write_call_file(const std::filesystem::path &dir, const Call &call) {
	static std::atomic<unsigned long long> written {0};
	std::error_code error;
	std::filesystem::create_directories(dir, error);
	const std::string unique = platform::sha256_hex_digest(call.at + "\n" + call.device + "\n" + std::to_string(++written) + "\n"
		+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())).substr(0, 8);
	const std::string name = stamp_of(call.at) + "-" + (call.device.empty() == true ? std::string("unknown") : call.device) + "-" + unique + ".json";
	const std::filesystem::path writing = dir / (name + kWriting + unique);
	{
		std::ofstream out(writing, std::ios::binary | std::ios::trunc);
		out << line_of(call) << '\n';
		out.flush();
		if (out.good() == false) {
			out.close();
			std::filesystem::remove(writing, error);
			return;
		}
	}
	std::filesystem::rename(writing, dir / name, error);
	if (error) std::filesystem::remove(writing, error);
}

// A Matter's chat's calls without what is older than kKeptFor - whichever
// computer made it, as each of them does - and within kMostBytes, its newer
// half kept where it grew past it; a file left half written by a stop goes
// after an hour. At most once an hour for each chat. Called with files_lock
// held.
void prune_matter_dir(const std::filesystem::path &dir, Clock::time_point now) {
	auto &pruned = last_pruned()[dir.string()];
	if (now - pruned < std::chrono::hours(1)) return;
	pruned = now;
	struct Kept {
		std::filesystem::path file;
		Clock::time_point at;
		std::uintmax_t size = 0;
	};
	std::vector<Kept> kept;
	std::uintmax_t total = 0;
	std::error_code error;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir, error)) {
		const std::string name = entry.path().filename().u8string();
		const std::optional<Clock::time_point> at = time_of_name(name);
		if (at.has_value() == false) continue;
		std::error_code ignored;
		if (name.find(kWriting) != std::string::npos) {
			if (now - *at > std::chrono::hours(1)) std::filesystem::remove(entry.path(), ignored);
			continue;
		}
		if (is_call_file(name) == false) continue;
		if (now - *at > kKeptFor) {
			std::filesystem::remove(entry.path(), ignored);
			continue;
		}
		const std::uintmax_t size = entry.file_size(ignored);
		total += size;
		kept.push_back({entry.path(), *at, size});
	}
	if (total <= kMostBytes || kept.size() < 2) return;
	std::sort(kept.begin(), kept.end(), [](const Kept &left, const Kept &right) { return left.at < right.at; });
	for (std::size_t index = 0; index < kept.size() / 2; ++index) {
		std::error_code ignored;
		std::filesystem::remove(kept[index].file, ignored);
	}
}

// This computer's own calls in a Matter's folder, erased; another computer's
// stay, that computer's to erase. Called with files_lock held.
void erase_own_calls(const std::filesystem::path &folder, const std::string &tag) {
	const std::filesystem::path records = folder / ".lm" / "WorkLog";
	std::error_code error;
	if (std::filesystem::is_directory(records, error) == false) return;
	const std::string mark = "-" + tag + "-";
	for (const std::filesystem::directory_entry &chat : std::filesystem::directory_iterator(records, error)) {
		std::error_code ignored;
		if (chat.is_directory(ignored) == false) continue;
		for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(chat.path(), ignored)) {
			if (entry.path().filename().u8string().find(mark) != std::string::npos) {
				std::error_code removing;
				std::filesystem::remove(entry.path(), removing);
			}
		}
		if (std::filesystem::is_empty(chat.path(), ignored) == true) std::filesystem::remove(chat.path(), ignored);
	}
	std::error_code ignored;
	if (std::filesystem::is_empty(records, ignored) == true) std::filesystem::remove(records, ignored);
}

// The folder whose `.lm` keeps this Matter's calls, asked of the app outside
// files_lock: the answer may wait for the Matters' own lock.
std::optional<std::filesystem::path> matter_folder_now(const std::string &matter_id) {
	std::function<std::optional<std::filesystem::path>(const std::string &)> folder_of;
	{
		const std::lock_guard<std::mutex> guard(files_lock());
		if (matter_records().has_value() == true) folder_of = matter_records()->folder_of;
	}
	if (!folder_of || matter_id.empty() == true) return std::nullopt;
	try {
		return folder_of(matter_id);
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

} // namespace

void set_directory(const std::filesystem::path &directory) {
	const std::lock_guard<std::mutex> guard(files_lock());
	named_directory() = directory;
	kept_state().store(-1);
	last_pruned().clear();
}

void set_matter_records(MatterRecords records) {
	const std::lock_guard<std::mutex> guard(files_lock());
	own_tag() = records.device_id.empty() == true ? std::string() : platform::sha256_hex_digest(records.device_id).substr(0, 12);
	matter_records() = std::move(records);
}

void set_kept(bool keep) {
	// The Matters' folders, asked before files_lock: the answer may wait for the
	// Matters' own lock.
	std::vector<std::filesystem::path> folders;
	if (keep == false) {
		std::function<std::vector<std::filesystem::path>()> list;
		{
			const std::lock_guard<std::mutex> guard(files_lock());
			const int state = kept_state().load();
			std::error_code ignored;
			const bool was_kept = state == 1
				|| (state == -1 && named_directory().has_value() == true && std::filesystem::exists(marker(), ignored) == true);
			if (was_kept == true && matter_records().has_value() == true) list = matter_records()->folders;
		}
		if (list) {
			try {
				folders = list();
			} catch (const std::exception &) {
				folders.clear();
			}
		}
	}
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
		if (own_tag().empty() == false)
			for (const std::filesystem::path &folder : folders) erase_own_calls(folder, own_tag());
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
	const auto [matter_id, chat_id] = *chat_in_scope();
	Call kept = call;
	if (kept.at.empty() == true) kept.at = time_text(Clock::now());
	const std::optional<std::filesystem::path> folder = matter_folder_now(matter_id);
	const std::lock_guard<std::mutex> guard(files_lock());
	kept.device = own_tag();
	// A Matter's chat: in the Matter's own folder, which carries it to each of
	// its computers.
	if (folder.has_value() == true) {
		const std::filesystem::path dir = matter_dir_of(*folder, matter_id, chat_id);
		write_call_file(dir, kept);
		prune_matter_dir(dir, Clock::now());
		return;
	}
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
	const std::optional<std::filesystem::path> folder = matter_folder_now(matter_id);
	const std::lock_guard<std::mutex> guard(files_lock());
	std::vector<Call> calls;
	if (named_directory().has_value() == false) return calls;
	const Clock::time_point now = Clock::now();
	const auto take = [&](const std::string &line) {
		std::optional<Call> call = call_of(line);
		if (call.has_value() == false) return;
		const std::optional<Clock::time_point> at = time_of(call->at);
		if (at.has_value() == true && now - *at <= kKeptFor) calls.push_back(std::move(*call));
	};
	// What this computer kept in its own records, and, for a Matter's chat,
	// what every computer of the Matter kept in its folder.
	for (const std::string &line : lines_in(file_of(matter_id, chat_id))) take(line);
	if (folder.has_value() == true) {
		std::vector<std::filesystem::path> files;
		std::error_code error;
		for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(matter_dir_of(*folder, matter_id, chat_id), error)) {
			std::error_code ignored;
			if (entry.is_regular_file(ignored) == true && is_call_file(entry.path().filename().u8string()) == true) files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());
		for (const std::filesystem::path &file : files)
			for (const std::string &line : lines_in(file)) take(line);
	}
	std::stable_sort(calls.begin(), calls.end(), [](const Call &left, const Call &right) { return left.at < right.at; });
	return calls;
}

} // namespace singularity::node::work_log
