#include "Platform/StaleRuntimeReaper/StaleRuntimeReaper.hpp"

#include <cerrno>
#include <cstring>
#include <chrono>
#include <csignal>
#include <iostream>
#include <system_error>
#include <thread>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <climits>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unistd.h>
#elif defined(_WIN32)
#include "Platform/StaleRuntimeReaper/StaleRuntimeReaperSeam.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <map>
#include <memory>
#endif

namespace singularity::platform::stale_runtime {

namespace {

// Where the owner's pid begins in a runtime's slot folder name, or npos.
// LocalInference/GenerativeRuntime names the folder "local-inference-slots-
// <owner pid>-<hex>"; releases before it named it "<word>-runtime-slots-
// <owner pid>-<hex>", so a runtime one of them left is still recognised.
std::size_t owner_pid_offset(const std::string &name) {
	static const std::string kCurrent = "local-inference-slots-";
	static const std::string kEarlier = "-runtime-slots-";
	if (name.rfind(kCurrent, 0) == 0) return kCurrent.size();
	const std::size_t at = name.find(kEarlier);
	return at == std::string::npos || at == 0 ? std::string::npos : at + kEarlier.size();
}

bool is_runtime_slot_folder(const std::filesystem::path &path) {
	return owner_pid_offset(path.filename().u8string()) != std::string::npos;
}

} // namespace

ProcessId runtime_owner_pid(const std::vector<std::string> &arguments, std::filesystem::path *slot_directory) {
	for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
		if (arguments[index] != "--slot-save-path") continue;
		const std::filesystem::path path(arguments[index + 1]);
		const std::string name = path.filename().string();
		const std::size_t offset = owner_pid_offset(name);
		if (offset == std::string::npos) return 0;
		std::size_t cursor = offset;
		std::size_t digits = 0;
		ProcessId owner = 0;
		while (cursor < name.size() && name[cursor] >= '0' && name[cursor] <= '9' && digits < 9) {
			owner = owner * 10 + (name[cursor] - '0');
			++cursor;
			++digits;
		}
		if (digits == 0 || cursor >= name.size() || name[cursor] != '-') return 0;
		if (slot_directory != nullptr) *slot_directory = path;
		return owner;
	}
	return 0;
}

std::vector<StaleRuntime> select_stale_runtimes(const std::vector<ProcessRecord> &processes,
		const std::string &bundled_runtime_path, UserId current_uid,
		const std::function<bool(ProcessId)> &is_live_singularity_process) {
	std::vector<StaleRuntime> stale;
	if (bundled_runtime_path.empty()) return stale;
	for (const ProcessRecord &process : processes) {
		if (process.pid <= 1 || process.uid != current_uid) continue;
		// Still the child of a live Singularity: that one stops it.
		if (process.parent_pid > 1 && is_live_singularity_process(process.parent_pid)) continue;
		if (process.executable_path != bundled_runtime_path) continue;
		std::filesystem::path slot;
		const ProcessId owner = runtime_owner_pid(process.arguments, &slot);
		if (owner <= 1) continue;
		if (is_live_singularity_process(owner)) continue;
		stale.push_back({process.pid, owner, slot});
	}
	return stale;
}

#if defined(__APPLE__) || defined(__linux__)
namespace {

bool is_alive(pid_t pid) {
	return kill(pid, 0) == 0 || errno == EPERM;
}

std::string canonical(const std::string &path) {
	std::error_code error;
	const auto resolved = std::filesystem::weakly_canonical(path, error);
	return error ? path : resolved.string();
}

// Ends each selected runtime (SIGTERM, then SIGKILL after a short grace) and
// removes its slot directory. Returns how many were ended.
std::size_t end_runtimes(const std::vector<StaleRuntime> &stale) {
	std::size_t ended = 0;
	for (const StaleRuntime &runtimeProcess : stale) {
		const auto pid = static_cast<pid_t>(runtimeProcess.pid);
		if (kill(pid, SIGTERM) != 0) continue;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (is_alive(pid) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (is_alive(pid)) kill(pid, SIGKILL);
		++ended;
		if (!runtimeProcess.slot_directory.empty() && is_runtime_slot_folder(runtimeProcess.slot_directory)) {
			std::error_code removal;
			std::filesystem::remove_all(runtimeProcess.slot_directory, removal);
		}
		std::cerr << "Singularity: ended a local model runtime left by an earlier session (pid "
			<< runtimeProcess.pid << ")\n";
	}
	return ended;
}

} // namespace
#endif

#if defined(__APPLE__)
namespace {

std::string process_path(pid_t pid) {
	char buffer[PROC_PIDPATHINFO_MAXSIZE];
	const int length = proc_pidpath(pid, buffer, sizeof(buffer));
	if (length <= 0) return {};
	return canonical(std::string(buffer, static_cast<std::size_t>(length)));
}

std::vector<std::string> process_arguments(pid_t pid) {
	int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
	std::size_t size = 0;
	if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int)) return {};
	std::vector<char> buffer(size);
	if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return {};
	int argc = 0;
	std::memcpy(&argc, buffer.data(), sizeof(int));
	std::size_t cursor = sizeof(int);
	while (cursor < size && buffer[cursor] != '\0') ++cursor; // executable path
	while (cursor < size && buffer[cursor] == '\0') ++cursor;
	std::vector<std::string> arguments;
	while (cursor < size && static_cast<int>(arguments.size()) < argc) {
		const std::size_t start = cursor;
		while (cursor < size && buffer[cursor] != '\0') ++cursor;
		arguments.emplace_back(buffer.data() + start, cursor - start);
		++cursor;
	}
	return arguments;
}

std::string own_executable_path() {
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string path(size, '\0');
	if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
	path.resize(std::char_traits<char>::length(path.c_str()));
	return canonical(path);
}

} // namespace

std::size_t reap_stale_runtimes() {
	try {
		const std::string ownExecutable = own_executable_path();
		if (ownExecutable.empty()) return 0;
		// <App>.app/Contents/MacOS/<exe> -> <App>.app/Contents/Resources/llama.cpp/llama-server
		const std::filesystem::path runtime = std::filesystem::path(ownExecutable).parent_path().parent_path()
			/ "Resources" / "llama.cpp" / "llama-server";
		std::error_code exists;
		if (!std::filesystem::exists(runtime, exists)) return 0;
		const std::string runtimePath = canonical(runtime.string());

		const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
		if (bytes <= 0) return 0;
		std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 16);
		const int filled = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
		if (filled <= 0) return 0;
		pids.resize(static_cast<std::size_t>(filled) / sizeof(pid_t));

		std::vector<ProcessRecord> records;
		for (const pid_t pid : pids) {
			if (pid <= 1) continue;
			struct proc_bsdinfo info {};
			if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, PROC_PIDTBSDINFO_SIZE) != PROC_PIDTBSDINFO_SIZE) continue;
			if (static_cast<pid_t>(info.pbi_ppid) != 1 || info.pbi_uid != getuid()) continue;
			const std::string path = process_path(pid);
			if (path != runtimePath) continue;
			records.push_back({pid, static_cast<pid_t>(info.pbi_ppid), info.pbi_uid, path, process_arguments(pid)});
		}
		const auto stale = select_stale_runtimes(records, runtimePath, getuid(), [&](pid_t owner) {
			return owner == getpid() || (is_alive(owner) && process_path(owner) == ownExecutable);
		});
		return end_runtimes(stale);
	} catch (...) {
		return 0;
	}
}
#elif defined(__linux__)
namespace {

std::string link_target(const std::string &link) {
	char buffer[PATH_MAX];
	const ssize_t length = readlink(link.c_str(), buffer, sizeof(buffer) - 1);
	if (length <= 0) return {};
	return std::string(buffer, static_cast<std::size_t>(length));
}

// The process's executable, as the kernel names it. A binary replaced on disk
// since it started reads "<path> (deleted)" and so never matches.
std::string process_path(pid_t pid) {
	const std::string target = link_target("/proc/" + std::to_string(pid) + "/exe");
	return target.empty() ? target : canonical(target);
}

std::vector<std::string> process_arguments(pid_t pid) {
	std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
	const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::vector<std::string> arguments;
	std::size_t start = 0;
	while (start < raw.size()) {
		const std::size_t end = raw.find('\0', start);
		arguments.push_back(raw.substr(start, (end == std::string::npos ? raw.size() : end) - start));
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return arguments;
}

// Its parent and its real user, from /proc/<pid>/status.
bool parent_and_user(pid_t pid, ProcessId &parent, UserId &uid) {
	std::ifstream file("/proc/" + std::to_string(pid) + "/status");
	std::string line;
	bool haveParent = false;
	bool haveUser = false;
	while (std::getline(file, line) && !(haveParent && haveUser)) {
		std::istringstream fields(line);
		std::string key;
		fields >> key;
		if (key == "PPid:") haveParent = static_cast<bool>(fields >> parent);
		else if (key == "Uid:") haveUser = static_cast<bool>(fields >> uid);
	}
	return haveParent && haveUser;
}

} // namespace

std::size_t reap_stale_runtimes() {
	try {
		const std::string ownExecutable = canonical(link_target("/proc/self/exe"));
		if (ownExecutable.empty()) return 0;
		// The flat layout Singularity looks for beside its host: <dir>/<exe> -> <dir>/llama.cpp/llama-server
		const std::filesystem::path runtime = std::filesystem::path(ownExecutable).parent_path() / "llama.cpp"
			/ "llama-server";
		std::error_code exists;
		if (!std::filesystem::exists(runtime, exists)) return 0;
		const std::string runtimePath = canonical(runtime.string());
		const UserId me = getuid();

		std::vector<ProcessRecord> records;
		std::error_code listing;
		for (const auto &entry : std::filesystem::directory_iterator("/proc", listing)) {
			const std::string name = entry.path().filename().string();
			if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
			const auto pid = static_cast<pid_t>(std::stol(name));
			if (pid <= 1) continue;
			ProcessId parent = 0;
			UserId uid = 0;
			if (!parent_and_user(pid, parent, uid) || uid != me) continue;
			const std::string path = process_path(pid);
			if (path != runtimePath) continue;
			records.push_back({pid, parent, uid, path, process_arguments(pid)});
		}
		const auto stale = select_stale_runtimes(records, runtimePath, me, [&](ProcessId owner) {
			const auto pid = static_cast<pid_t>(owner);
			return pid == getpid() || (is_alive(pid) && process_path(pid) == ownExecutable);
		});
		return end_runtimes(stale);
	} catch (...) {
		return 0;
	}
}
#elif defined(_WIN32)
namespace {

// Singularity starts its runtime suspended and resumes it only once the runtime is
// inside a Job Object that ends it with Singularity (air
// ChildProcessWindows.cpp, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). An Singularity
// that died in between leaves a runtime that never runs, yet holds
// llama-server.exe, so no update can replace it, and its slot folder.

class Handle {
public:
	explicit Handle(HANDLE value) : value_(value) {}
	~Handle() {
		if (is_open()) CloseHandle(value_);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	bool is_open() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
	HANDLE get() const { return value_; }

private:
	HANDLE value_;
};

std::string utf8_of(const std::wstring &text) {
	if (text.empty()) return {};
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
		nullptr, nullptr);
	if (length <= 0) return {};
	std::string result(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
		nullptr);
	return result;
}

// Windows' file names ignore case, so paths are compared folded.
std::wstring folded(std::wstring text) {
	if (text.empty()) return text;
	std::wstring lower(text.size(), L'\0');
	const int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
		static_cast<int>(text.size()), lower.data(), static_cast<int>(lower.size()), nullptr, nullptr, 0);
	return length == static_cast<int>(text.size()) ? lower : text;
}

std::wstring module_path() {
	std::wstring path(1024, L'\0');
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0) return {};
		if (length < path.size() - 1) {
			path.resize(length);
			return path;
		}
		path.resize(path.size() * 2);
	}
}

std::wstring image_path_of(HANDLE process) {
	std::wstring path(32768, L'\0');
	DWORD size = static_cast<DWORD>(path.size());
	if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
	path.resize(size);
	return path;
}

// A process's command line as Windows keeps it for the process, readable even
// while it is suspended (ProcessCommandLineInformation, Windows 8.1 and later).
std::wstring command_line_of(HANDLE process) {
	using Query = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr) return {};
	const auto query = reinterpret_cast<Query>(
		reinterpret_cast<void (*)()>(GetProcAddress(ntdll, "NtQueryInformationProcess")));
	if (query == nullptr) return {};
	constexpr ULONG kProcessCommandLineInformation = 60;
	ULONG size = 0;
	query(process, kProcessCommandLineInformation, nullptr, 0, &size);
	if (size < sizeof(UNICODE_STRING)) return {};
	std::vector<unsigned char> buffer(size);
	if (query(process, kProcessCommandLineInformation, buffer.data(), size, &size) < 0) return {};
	const auto *text = reinterpret_cast<const UNICODE_STRING *>(buffer.data());
	if (text->Buffer == nullptr) return {};
	return std::wstring(text->Buffer, text->Length / sizeof(wchar_t));
}

// The arguments a program received, split by the rules it splits them by.
std::vector<std::wstring> arguments_of(const std::wstring &command_line) {
	std::vector<std::wstring> arguments;
	if (command_line.empty()) return arguments;
	int count = 0;
	LPWSTR *split = CommandLineToArgvW(command_line.c_str(), &count);
	if (split == nullptr) return arguments;
	for (int index = 0; index < count; ++index) arguments.emplace_back(split[index]);
	LocalFree(split);
	return arguments;
}

// The slot folder a runtime's arguments name, as Windows spells it: a user
// folder outside ASCII survives, which a narrow path would not.
std::filesystem::path slot_directory_of(const std::vector<std::wstring> &arguments) {
	for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
		if (arguments[index] == L"--slot-save-path") return std::filesystem::path(arguments[index + 1]);
	}
	return {};
}

// Whether a process runs as the same user as this one.
bool is_this_users(HANDLE process) {
	const auto user_of = [](HANDLE owner, std::vector<unsigned char> &buffer) -> PSID {
		HANDLE raw = nullptr;
		if (!OpenProcessToken(owner, TOKEN_QUERY, &raw)) return nullptr;
		const Handle token(raw);
		DWORD size = 0;
		GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
		if (size == 0) return nullptr;
		buffer.resize(size);
		if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) return nullptr;
		return reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
	};
	std::vector<unsigned char> theirs;
	std::vector<unsigned char> ours;
	const PSID their_user = user_of(process, theirs);
	const PSID our_user = user_of(GetCurrentProcess(), ours);
	return their_user != nullptr && our_user != nullptr && EqualSid(their_user, our_user) != FALSE;
}

// A runtime as listed, held open from the listing to its end, so a pid that
// Windows hands to a new process meanwhile is never the one ended.
struct ListedRuntime {
	std::unique_ptr<Handle> process;
	std::vector<std::wstring> arguments;
};

} // namespace

std::size_t reap_stale_runtimes_for(const std::filesystem::path &runtime, const std::filesystem::path &singularity) {
	try {
		std::error_code exists;
		if (runtime.empty() || singularity.empty() || !std::filesystem::exists(runtime, exists)) return 0;
		const std::string runtime_path = utf8_of(folded(runtime.wstring()));
		const std::wstring workspace_path = folded(singularity.wstring());
		const std::wstring runtime_name = folded(runtime.filename().wstring());

		const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (!snapshot.is_open()) return 0;
		std::vector<ProcessRecord> records;
		std::map<ProcessId, ListedRuntime> listed;
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
			if (folded(entry.szExeFile) != runtime_name) continue;
			auto process = std::make_unique<Handle>(OpenProcess(
				PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID));
			if (!process->is_open() || WaitForSingleObject(process->get(), 0) != WAIT_TIMEOUT) continue;
			// The command line is read from the process's own memory, so only from
			// a process that is the bundled runtime: never from one ending, or
			// stuck before it ever started, which can hold such a read.
			const std::wstring path = image_path_of(process->get());
			if (path.empty() || utf8_of(folded(path)) != runtime_path || !is_this_users(process->get())) continue;
			std::vector<std::wstring> arguments = arguments_of(command_line_of(process->get()));
			std::vector<std::string> utf8_arguments;
			for (const std::wstring &argument : arguments) utf8_arguments.push_back(utf8_of(argument));
			const auto pid = static_cast<ProcessId>(entry.th32ProcessID);
			records.push_back({pid, static_cast<ProcessId>(entry.th32ParentProcessID), 0, utf8_of(folded(path)),
				std::move(utf8_arguments)});
			listed[pid] = {std::move(process), std::move(arguments)};
		}
		const auto stale = select_stale_runtimes(records, runtime_path, 0, [&](ProcessId pid) {
			const Handle process(
				OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
			return process.is_open() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT
				&& folded(image_path_of(process.get())) == workspace_path;
		});
		std::size_t ended = 0;
		for (const StaleRuntime &runtime_process : stale) {
			const auto found = listed.find(runtime_process.pid);
			if (found == listed.end() || !TerminateProcess(found->second.process->get(), 1)) continue;
			WaitForSingleObject(found->second.process->get(), 3000);
			++ended;
			const std::filesystem::path slot = slot_directory_of(found->second.arguments);
			if (!slot.empty() && is_runtime_slot_folder(slot)) {
				std::error_code removal;
				std::filesystem::remove_all(slot, removal);
			}
			std::cerr << "Singularity: ended a local model runtime left by an earlier session (pid "
				<< runtime_process.pid << ")\n";
		}
		return ended;
	} catch (...) {
		return 0;
	}
}

std::size_t reap_stale_runtimes() {
	try {
		const std::filesystem::path own = module_path();
		if (own.empty()) return 0;
		// The flat layout Singularity looks for beside its host:
		// <dir>\<exe> -> <dir>\llama.cpp\llama-server.exe
		return reap_stale_runtimes_for(own.parent_path() / L"llama.cpp" / L"llama-server.exe", own);
	} catch (...) {
		return 0;
	}
}
#else
std::size_t reap_stale_runtimes() { return 0; }
#endif

} // namespace singularity::platform::stale_runtime
