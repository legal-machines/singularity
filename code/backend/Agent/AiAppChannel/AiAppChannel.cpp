#include "Agent/AiAppChannel/AiAppChannel.hpp"

#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>

#if defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <csignal>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace singularity::agents {
namespace {

using Json = nlohmann::json;

std::filesystem::path inbox_directory() {
	return ai_apps_directory() / "inbox";
}

std::filesystem::path outbox_directory() {
	return ai_apps_directory() / "outbox";
}

std::filesystem::path taken_directory() {
	return ai_apps_directory() / "taken";
}

// A request is a few short words and a key; an answer may list the agents of
// fifty Matters. A larger file was not written by Singularity or its helper,
// and is never read.
constexpr std::size_t kMostRequestBytes = 64 * 1024;
constexpr std::size_t kMostAnswerBytes = 8 * 1024 * 1024;
// A request is one object holding flat ones, and an answer is a few levels
// deep. A deeper value is refused as it is parsed and never built, so nothing
// copies or prints it level by level on the inbox thread's small stack.
constexpr int kDeepestValue = 16;

std::filesystem::path open_record_path() {
	return ai_apps_directory() / "open.json";
}

// The folders are the user's alone: a request in them carries a key.
void ensure_private_directory(const std::filesystem::path &directory) {
	platform::ensure_record_directory(directory);
	platform::restrict_to_owner(ai_apps_directory());
	platform::restrict_to_owner(directory);
}

void write_whole(const std::filesystem::path &path, const Json &value) {
	ensure_private_directory(path.parent_path());
	const std::filesystem::path staged = platform::staged_sibling_path(path);
	try {
#if defined(__APPLE__) || defined(__linux__)
		// The file is the user's alone from its first byte: made 0600 before a
		// key is written into it, never readable for a moment at the umask.
		const int descriptor = ::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (descriptor < 0) throw std::runtime_error("Singularity could not keep an AI app's request.");
		::close(descriptor);
#endif
		platform::write_file_durably(staged, value.dump());
		platform::commit_staged_file(staged, path, platform::StagedCommitMode::ReplaceExisting, false);
	} catch (...) {
		platform::discard_staged_file(staged);
		throw;
	}
}

// A file as read through one descriptor: its bytes, and when it was written,
// in seconds since 1970.
struct PlainFile {
	std::string bytes;
	long long written_at = 0;
};

#if defined(__APPLE__) || defined(__linux__)
// Files are opened without following a link, and without waiting for a
// writer when one is a pipe.
constexpr int kOpenUnfollowed = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;

bool is_plain_file_of(const struct stat &about, std::size_t most_bytes) {
	return S_ISREG(about.st_mode) && about.st_size >= 0 && static_cast<unsigned long long>(about.st_size) <= most_bytes;
}

// The rest of an open file, when it ends within `most_bytes`: one that grows
// past them while it is read is refused too.
std::optional<std::string> read_to_end(int descriptor, std::size_t most_bytes) {
	std::string bytes;
	char buffer[16 * 1024];
	while (true) {
		const ssize_t got = ::read(descriptor, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR) continue;
		if (got == 0) return bytes;
		if (got < 0 || bytes.size() + static_cast<std::size_t>(got) > most_bytes) return std::nullopt;
		bytes.append(buffer, static_cast<std::size_t>(got));
	}
}

// Whether `path` still names the file open as `descriptor` - not another one,
// and not none.
bool is_still_named(int descriptor, const std::filesystem::path &path) {
	struct stat opened {};
	struct stat named {};
	return ::fstat(descriptor, &opened) == 0 && ::lstat(path.c_str(), &named) == 0 && opened.st_dev == named.st_dev
		&& opened.st_ino == named.st_ino;
}
#else
// A request is claimed on Windows by opening it alone - no other handle may be
// open on it, and none may open while this one is - the lock POSIX takes with
// flock: whichever of Singularity and its helper opens it first removes it
// through that handle, and the other then finds it gone. A link is opened as
// itself, never followed. `access` adds GENERIC_READ for the taker; `flags`
// FILE_FLAG_DELETE_ON_CLOSE.
HANDLE open_alone(const std::filesystem::path &path, DWORD access, DWORD flags) {
	return CreateFileW(path.c_str(), access | DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | flags, nullptr);
}

// Another program holds the file for now: the other side of the claim, or a
// scanner that has just opened it.
bool is_held_elsewhere(DWORD error) {
	return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
}

// The rest of an open file, when it ends within `most_bytes`.
std::optional<std::string> read_to_end(HANDLE file, std::size_t most_bytes) {
	std::string bytes;
	char buffer[16 * 1024];
	while (true) {
		DWORD got = 0;
		if (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) == 0) return std::nullopt;
		if (got == 0) return bytes;
		if (bytes.size() + got > most_bytes) return std::nullopt;
		bytes.append(buffer, got);
	}
}

// Seconds since 1970 of a time Windows gives in 100-nanosecond units since 1601.
long long seconds_since_1970(const FILETIME &time) {
	const unsigned long long ticks = (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	return static_cast<long long>((ticks - 116444736000000000ULL) / 10000000ULL);
}
#endif

// A plain file of at most `most_bytes`, and nothing else: a link is never
// followed, a pipe is opened without waiting for a writer and refused, and a
// folder, a device or a larger file is refused before a byte is read - so
// nothing left in these folders can hold the inbox or exhaust its memory.
std::optional<PlainFile> read_plain_file(const std::filesystem::path &path, std::size_t most_bytes) {
	PlainFile file;
#if defined(__APPLE__) || defined(__linux__)
	const int descriptor = ::open(path.c_str(), kOpenUnfollowed);
	if (descriptor < 0) return std::nullopt;
	struct stat about {};
	std::optional<std::string> bytes;
	if (::fstat(descriptor, &about) == 0 && is_plain_file_of(about, most_bytes) == true) bytes = read_to_end(descriptor, most_bytes);
	::close(descriptor);
	if (bytes.has_value() == false) return std::nullopt;
	file.bytes = std::move(*bytes);
	file.written_at = static_cast<long long>(about.st_mtime);
#else
	std::error_code error;
	if (std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) == false) return std::nullopt;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size > most_bytes) return std::nullopt;
	const auto written = std::filesystem::last_write_time(path, error);
	if (error) return std::nullopt;
	file.written_at = std::chrono::duration_cast<std::chrono::seconds>(
		(std::chrono::system_clock::now() - (std::filesystem::file_time_type::clock::now() - written)).time_since_epoch()).count();
	std::ifstream input(path, std::ios::binary);
	if (input.is_open() == false) return std::nullopt;
	file.bytes.resize(static_cast<std::size_t>(size) + 1);
	input.read(&file.bytes[0], static_cast<std::streamsize>(file.bytes.size()));
	file.bytes.resize(static_cast<std::size_t>(input.gcount()));
	if (input.bad() == true || file.bytes.size() > most_bytes) return std::nullopt;
#endif
	return file;
}

// The JSON object in `bytes`, never deeper than kDeepestValue.
std::optional<Json> parsed_object(const std::string &bytes) {
	bool isTooDeep = false;
	Json value = Json::parse(bytes, [&isTooDeep](int depth, Json::parse_event_t event, Json &) {
		if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= kDeepestValue)
			isTooDeep = true;
		// Once it is too deep, nothing more is built.
		return isTooDeep == false;
	}, false);
	if (isTooDeep == true || value.is_discarded() == true || value.is_object() == false) return std::nullopt;
	return value;
}

std::optional<Json> read_whole(const std::filesystem::path &path, std::size_t most_bytes) {
	const std::optional<PlainFile> file = read_plain_file(path, most_bytes);
	if (file.has_value() == false) return std::nullopt;
	return parsed_object(file->bytes);
}

// The arguments as the helper checks them (McpServer, mcp_argument_problem):
// one object of text, whole numbers, true or false, and null - nothing in it
// nested.
bool is_flat_object(const Json &value) {
	if (value.is_object() == false) return false;
	for (const Json &field : value)
		if (field.is_string() == false && field.is_number_integer() == false && field.is_boolean() == false && field.is_null() == false)
			return false;
	return true;
}

// Seconds since 1970 of a time as the helper writes it, "2026-10-10T21:04:00Z".
std::optional<long long> seconds_of(const std::string &timestamp) {
	if (timestamp.size() != 20 || timestamp[4] != '-' || timestamp[7] != '-' || timestamp[10] != 'T' || timestamp.back() != 'Z')
		return std::nullopt;
	std::tm utc {};
	std::istringstream input(timestamp.substr(0, 19));
	input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
	if (input.fail() == true) return std::nullopt;
#if defined(_WIN32)
	const std::time_t seconds = _mkgmtime(&utc);
#else
	const std::time_t seconds = timegm(&utc);
#endif
	if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
	return static_cast<long long>(seconds);
}

std::string timestamp_of(long long seconds) {
	const std::time_t time = static_cast<std::time_t>(seconds);
	std::tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &time);
#else
	gmtime_r(&time, &utc);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

std::string text_of(const Json &value, const char *key) {
	const auto found = value.find(key);
	if (found == value.end() || found->is_string() == false) return "";
	return found->get<std::string>();
}

std::string random_hex(std::size_t bytes) {
	return platform::secure_random_hex(bytes);
}

long long seconds_now() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool is_process_alive(long long pid) {
	if (pid <= 0) return false;
#if defined(__APPLE__) || defined(__linux__)
	if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
	return errno == EPERM;
#else
	return true;
#endif
}

} // namespace

std::filesystem::path ai_apps_directory() {
	return platform::application_data_directory() / "AI Apps";
}

bool is_request_id(const std::string &value) {
	if (value.size() != 32) return false;
	for (const char character : value)
		if ((character < '0' || character > '9') && (character < 'a' || character > 'f')) return false;
	return true;
}

std::string new_request_id() {
	return random_hex(16);
}

Json request_to_json(const AiAppRequest &request) {
	return Json{
		{"id", request.id},
		{"key", request.key},
		{"tool", request.tool},
		{"arguments", request.arguments.is_object() == true ? request.arguments : Json::object()},
		{"clientName", request.client_name},
		{"clientVersion", request.client_version},
		{"caller", Json{
			{"path", request.caller.path},
			{"signer", request.caller.signer},
			{"teamId", request.caller.team_id},
			{"signingId", request.caller.signing_id},
			{"isSignerVerified", request.caller.is_signer_verified},
		}},
		{"sentAt", request.sent_at},
	};
}

std::optional<AiAppRequest> request_from_json(const Json &value) {
	if (value.is_object() == false) return std::nullopt;
	AiAppRequest request;
	request.id = text_of(value, "id");
	request.key = text_of(value, "key");
	request.tool = text_of(value, "tool");
	if (is_request_id(request.id) == false || request.tool.empty() == true) return std::nullopt;
	const auto arguments = value.find("arguments");
	if (arguments != value.end()) {
		// Checked before it is copied: anything nested is refused, never copied.
		if (is_flat_object(*arguments) == false) return std::nullopt;
		request.arguments = *arguments;
	}
	request.client_name = text_of(value, "clientName");
	request.client_version = text_of(value, "clientVersion");
	const auto caller = value.find("caller");
	if (caller != value.end() && caller->is_object() == true) {
		request.caller.path = text_of(*caller, "path");
		request.caller.signer = text_of(*caller, "signer");
		request.caller.team_id = text_of(*caller, "teamId");
		request.caller.signing_id = text_of(*caller, "signingId");
		const auto verified = caller->find("isSignerVerified");
		request.caller.is_signer_verified = verified != caller->end() && verified->is_boolean() == true && verified->get<bool>() == true;
	}
	request.sent_at = text_of(value, "sentAt");
	return request;
}

Json answer_to_json(const AiAppAnswer &answer) {
	return Json{{"requestId", answer.request_id}, {"isRefused", answer.is_refused}, {"text", answer.text}, {"data", answer.data}};
}

std::optional<AiAppAnswer> answer_from_json(const Json &value) {
	if (value.is_object() == false) return std::nullopt;
	AiAppAnswer answer;
	answer.request_id = text_of(value, "requestId");
	if (is_request_id(answer.request_id) == false) return std::nullopt;
	const auto refused = value.find("isRefused");
	answer.is_refused = refused != value.end() && refused->is_boolean() == true && refused->get<bool>() == true;
	answer.text = text_of(value, "text");
	const auto data = value.find("data");
	if (data != value.end()) answer.data = *data;
	return answer;
}

void leave_request(const AiAppRequest &request) {
	if (is_request_id(request.id) == false) throw std::invalid_argument("A request needs its own id.");
	write_whole(inbox_directory() / (request.id + ".json"), request_to_json(request));
}

bool withdraw_request(const std::string &request_id) {
	if (is_request_id(request_id) == false) return false;
	const std::filesystem::path waiting = inbox_directory() / (request_id + ".json");
#if defined(__APPLE__) || defined(__linux__)
	// Under the request's own lock, which Singularity takes to claim it
	// (take_request), and only while the inbox still holds it.
	const int descriptor = ::open(waiting.c_str(), kOpenUnfollowed);
	if (descriptor < 0) return false;
	int locked = -1;
	do locked = ::flock(descriptor, LOCK_EX);
	while (locked != 0 && errno == EINTR);
	const bool isWithdrawn = locked == 0 && is_still_named(descriptor, waiting) == true && ::unlink(waiting.c_str()) == 0;
	::close(descriptor);
	return isWithdrawn;
#else
	// Under the claim take_request makes (open_alone): removed through a handle
	// no one else holds, so Singularity never takes it after this says it is
	// withdrawn. While Singularity holds it, or a scanner for a moment, this
	// waits up to two seconds; a request taken meanwhile is then gone.
	for (int attempt = 0; attempt <= 40; ++attempt) {
		const HANDLE file = open_alone(waiting, 0, 0);
		if (file != INVALID_HANDLE_VALUE) {
			FILE_DISPOSITION_INFO disposition{};
			disposition.DeleteFile = TRUE;
			const bool isWithdrawn = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
			CloseHandle(file);
			return isWithdrawn;
		}
		if (is_held_elsewhere(GetLastError()) == false) return false;
		Sleep(50);
	}
	return false;
#endif
}

std::optional<AiAppAnswer> take_answer(const std::string &request_id) {
	if (is_request_id(request_id) == false) return std::nullopt;
	const std::filesystem::path path = outbox_directory() / (request_id + ".json");
	const std::optional<Json> value = read_whole(path, kMostAnswerBytes);
	if (value.has_value() == false) return std::nullopt;
	std::error_code error;
	std::filesystem::remove(path, error);
	return answer_from_json(*value);
}

bool is_workspace_open(std::chrono::seconds fresh_within) {
	const std::optional<Json> record = read_whole(open_record_path(), kMostRequestBytes);
	if (record.has_value() == false) return false;
	const auto pid = record->find("pid");
	const auto at = record->find("at");
	if (pid == record->end() || pid->is_number_integer() == false || at == record->end() || at->is_number_integer() == false) return false;
	if (seconds_now() - at->get<long long>() > fresh_within.count()) return false;
	return is_process_alive(pid->get<long long>());
}

void say_workspace_open() {
#if defined(__APPLE__) || defined(__linux__)
	const long long pid = static_cast<long long>(::getpid());
#else
	const long long pid = 1;
#endif
	write_whole(open_record_path(), Json{{"pid", pid}, {"at", seconds_now()}});
}

void say_workspace_closed() {
	std::error_code error;
	std::filesystem::remove(open_record_path(), error);
}

std::vector<std::string> waiting_request_ids() {
	std::vector<std::string> ids;
	std::error_code error;
	std::filesystem::directory_iterator entries(inbox_directory(), error);
	if (error) return ids;
	for (const auto &entry : entries) {
		const std::filesystem::path path = entry.path();
		if (path.extension() != ".json") continue;
		const std::string id = path.stem().string();
		if (is_request_id(id) == true) ids.push_back(id);
	}
	return ids;
}

std::optional<AiAppRequest> take_request(const std::string &request_id) {
	if (is_request_id(request_id) == false) return std::nullopt;
	const std::filesystem::path waiting = inbox_directory() / (request_id + ".json");
	std::error_code error;
	PlainFile file;
#if defined(__APPLE__) || defined(__linux__)
	const std::filesystem::path claimed = taken_directory() / (request_id + ".json");
	ensure_private_directory(claimed.parent_path());
	const int descriptor = ::open(waiting.c_str(), kOpenUnfollowed);
	if (descriptor < 0) {
		// Withdrawn by its helper - or a link, removed and never followed.
		if (errno == ELOOP) std::filesystem::remove(waiting, error);
		return std::nullopt;
	}
	struct stat about {};
	if (::fstat(descriptor, &about) != 0 || is_plain_file_of(about, kMostRequestBytes) == false) {
		// A pipe, a folder, a device or a larger file: removed unread.
		::close(descriptor);
		std::filesystem::remove_all(waiting, error);
		return std::nullopt;
	}
	// Claimed by moving it out of the inbox before a byte is read, under the
	// lock its helper takes to withdraw it. A rename and a removal of one name
	// that race may both report success (APFS does), so the two never run at
	// once; and one locked now is being withdrawn, and is left to its helper.
	bool isClaimed = ::flock(descriptor, LOCK_EX | LOCK_NB) == 0 && is_still_named(descriptor, waiting) == true;
	if (isClaimed == true) {
		std::filesystem::rename(waiting, claimed, error);
		if (error) isClaimed = false;
	}
	// Gone at once: it carries a key. It is read through the descriptor.
	if (isClaimed == true) std::filesystem::remove(claimed, error);
	const std::optional<std::string> bytes = isClaimed == true ? read_to_end(descriptor, kMostRequestBytes) : std::nullopt;
	::close(descriptor);
	if (bytes.has_value() == false) return std::nullopt;
	file.bytes = *bytes;
	file.written_at = static_cast<long long>(about.st_mtime);
#else
	// Claimed by opening it alone before a byte is read (open_alone): its
	// helper cannot withdraw it meanwhile, and one its helper is withdrawing,
	// or a scanner holds, is left where it is and asked for again. It is read
	// through that handle and removed as the handle closes, read or refused:
	// it carries a key.
	const HANDLE claim = open_alone(waiting, GENERIC_READ, FILE_FLAG_DELETE_ON_CLOSE);
	if (claim == INVALID_HANDLE_VALUE) {
		// A folder is never opened as a file: removed unread.
		const DWORD attributes = GetFileAttributesW(waiting.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			std::filesystem::remove_all(waiting, error);
		return std::nullopt;
	}
	// A link, a device or a larger file: removed unread too.
	BY_HANDLE_FILE_INFORMATION about{};
	LARGE_INTEGER size{};
	const bool isPlain = GetFileInformationByHandle(claim, &about) != 0
		&& (about.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0
		&& GetFileType(claim) == FILE_TYPE_DISK && GetFileSizeEx(claim, &size) != 0 && size.QuadPart >= 0
		&& static_cast<unsigned long long>(size.QuadPart) <= kMostRequestBytes;
	const std::optional<std::string> bytes = isPlain == true ? read_to_end(claim, kMostRequestBytes) : std::nullopt;
	CloseHandle(claim);
	if (bytes.has_value() == false) return std::nullopt;
	file.bytes = *bytes;
	file.written_at = seconds_since_1970(about.ftLastWriteTime);
#endif
	const std::optional<Json> value = parsed_object(file.bytes);
	if (value.has_value() == false) return std::nullopt;
	std::optional<AiAppRequest> request = request_from_json(*value);
	if (request.has_value() == false || request->id != request_id) return std::nullopt;
	if (seconds_of(request->sent_at).has_value() == false) request->sent_at = timestamp_of(file.written_at);
	return request;
}

bool is_request_stale(const AiAppRequest &request, std::chrono::seconds older_than) {
	const std::optional<long long> sent = seconds_of(request.sent_at);
	if (sent.has_value() == false) return true;
	const long long age = seconds_now() - *sent;
	// A time far ahead of this computer's clock tells no more than none.
	return age > older_than.count() || age < -older_than.count();
}

void leave_answer(const AiAppAnswer &answer) {
	if (is_request_id(answer.request_id) == false) return;
	write_whole(outbox_directory() / (answer.request_id + ".json"), answer_to_json(answer));
}

void discard_stale_answers(std::chrono::seconds older_than) {
	std::error_code error;
	const auto now = std::filesystem::file_time_type::clock::now();
	for (const std::filesystem::path &directory : {outbox_directory(), inbox_directory(), taken_directory()}) {
		std::filesystem::directory_iterator entries(directory, error);
		if (error) {
			error.clear();
			continue;
		}
		for (const auto &entry : entries) {
			const auto written = std::filesystem::last_write_time(entry.path(), error);
			if (error) {
				error.clear();
				continue;
			}
			if (now - written > older_than) std::filesystem::remove(entry.path(), error);
		}
	}
}

} // namespace singularity::agents
