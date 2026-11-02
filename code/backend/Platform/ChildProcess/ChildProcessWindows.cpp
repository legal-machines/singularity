#include "Platform/ChildProcess/ChildProcess.hpp"

#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/WindowsText/WindowsText.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace singularity::platform {

struct OwnedChildProcess::State {
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	DWORD process_id = 0;
};

namespace {

class Handle final {
public:
	Handle() = default;
	explicit Handle(HANDLE value) : value_(value == INVALID_HANDLE_VALUE ? nullptr : value) {}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	~Handle() { reset(); }
	HANDLE get() const { return value_; }
	HANDLE release() {
		const HANDLE value = value_;
		value_ = nullptr;
		return value;
	}
	void reset() {
		if (value_ != nullptr) CloseHandle(value_);
		value_ = nullptr;
	}
	explicit operator bool() const { return value_ != nullptr; }

private:
	HANDLE value_ = nullptr;
};

[[noreturn]] void fail(const char *what, DWORD code) {
	throw std::runtime_error(std::string(what) + ": " + windows::error_message(code) + ".");
}

// Quotes one argument so CommandLineToArgvW and the MSVC runtime's argv
// parser reconstruct it exactly (backslashes are literal except before a
// quote).
std::wstring quote_argument(const std::wstring &argument) {
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
	std::wstring quoted(1, L'"');
	std::size_t backslashes = 0;
	for (const wchar_t character : argument) {
		if (character == L'\\') {
			++backslashes;
			continue;
		}
		if (character == L'"') quoted.append(backslashes * 2 + 1, L'\\');
		else quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted.push_back(character);
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'"');
	return quoted;
}

bool same_variable(const std::wstring &entry, const std::wstring &name) {
	return entry.size() > name.size() && entry[name.size()] == L'=' &&
		CompareStringOrdinal(entry.c_str(), static_cast<int>(name.size()), name.c_str(),
			static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

// The inherited environment with the spec's overrides applied, as the sorted,
// double-NUL-terminated UTF-16 block CreateProcessW expects. Variable names
// are case-insensitive on Windows.
std::wstring environment_block(const ChildProcessSpec &spec) {
	std::vector<std::wstring> entries;
	if (wchar_t *inherited = GetEnvironmentStringsW()) {
		for (const wchar_t *entry = inherited; *entry != L'\0'; entry += wcslen(entry) + 1) entries.emplace_back(entry);
		FreeEnvironmentStringsW(inherited);
	}
	auto set_variable = [&entries](const std::wstring &name, const std::wstring &value) {
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&name](const std::wstring &entry) { return same_variable(entry, name); }), entries.end());
		entries.push_back(name + L"=" + value);
	};
	for (const auto &[name, value] : spec.environment) set_variable(windows::utf8_to_wide(name), windows::utf8_to_wide(value));
	if (!spec.library_directory.empty()) {
		std::wstring path = spec.library_directory.wstring();
		for (const auto &entry : entries) {
			if (same_variable(entry, L"PATH")) {
				path += L";" + entry.substr(5);
				break;
			}
		}
		set_variable(L"PATH", path);
	}
	std::sort(entries.begin(), entries.end(), [](const std::wstring &left, const std::wstring &right) {
		return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
			static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
	});
	std::wstring block;
	for (const auto &entry : entries) {
		block += entry;
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

const char *describe_exit_code(DWORD code) {
	switch (code) {
		case 0xC0000135UL: return "a library it needs is missing";
		case 0xC000007BUL: return "a library it needs is damaged or built for another architecture";
		case 0xC0000139UL: return "a library it needs is the wrong version";
		case 0xC000001DUL: return "this processor lacks an instruction the program was built for";
		case 0xC0000005UL: return "it crashed (access violation)";
		case 0xC00000FDUL: return "it crashed (stack overflow)";
		case 0xC0000409UL: return "it stopped on an internal error";
		case 0xC0000017UL: return "the system ran out of memory";
		default: return nullptr;
	}
}

} // namespace

OwnedChildProcess::OwnedChildProcess(const ChildProcessSpec &spec) : state_(std::make_unique<State>()) {
	if (spec.executable.empty()) throw std::invalid_argument("A child process executable is required.");

	std::wstring command_line = quote_argument(spec.executable.wstring());
	for (const auto &argument : spec.arguments) command_line += L" " + quote_argument(windows::utf8_to_wide(argument));
	std::wstring environment = environment_block(spec);

	// The only handles the child inherits: NUL as stdin (it must never read
	// the owner's console) and the owner-only log (or NUL) as stdout/stderr.
	SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!input) fail("The local process input could not be prepared", GetLastError());
	HANDLE output_handle = INVALID_HANDLE_VALUE;
	if (spec.output_log.empty()) {
		output_handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	} else {
		// Private to this computer's user (Platform/PrivateFile), like every
		// file local inference keeps for one launch.
		PrivateSecurity security(/*folder=*/false);
		if (!security.valid()) fail("The local process log could not be made private", ERROR_ACCESS_DENIED);
		output_handle = CreateFileW(spec.output_log.c_str(), GENERIC_WRITE | WRITE_DAC,
			FILE_SHARE_READ | FILE_SHARE_DELETE, security.attributes(/*inheritable_handle=*/true),
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (output_handle != INVALID_HANDLE_VALUE) (void)security.apply_to(output_handle);
	}
	const DWORD output_error = GetLastError();
	Handle output(output_handle);
	if (!output) fail("The local process log could not be opened", output_error);

	SIZE_T attribute_bytes = 0;
	(void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
	std::vector<unsigned char> attribute_storage(attribute_bytes);
	auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
	if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
		fail("The local process could not be prepared", GetLastError());
	HANDLE inherited_handles[] = {input.get(), output.get()};
	const bool attribute_set = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		inherited_handles, sizeof(inherited_handles), nullptr, nullptr) != FALSE;
	if (!attribute_set) {
		const DWORD error = GetLastError();
		DeleteProcThreadAttributeList(attributes);
		fail("The local process could not be prepared", error);
	}

	// The ownership boundary: closing the last handle to this job - which
	// only this process holds, so its exit by any means closes it - kills
	// every process in it. Unhandled exceptions end the child instead of
	// leaving it parked behind an error-reporting dialog.
	Handle job(CreateJobObjectW(nullptr, nullptr));
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
	if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
		const DWORD error = GetLastError();
		DeleteProcThreadAttributeList(attributes);
		fail("Windows could not establish ownership of the local process", error);
	}

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = input.get();
	startup.StartupInfo.hStdOutput = output.get();
	startup.StartupInfo.hStdError = output.get();
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION created{};
	// Suspended until it is inside the job, so nothing it starts can escape;
	// a new process group with no console, so the owner's Ctrl-C never
	// reaches it.
	const DWORD flags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW |
		CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
	const std::wstring executable = spec.executable.wstring();
	const BOOL started = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
		environment.data(), nullptr, &startup.StartupInfo, &created);
	const DWORD start_error = GetLastError();
	DeleteProcThreadAttributeList(attributes);
	if (!started) fail("The local process could not be started", start_error);
	Handle process(created.hProcess);
	Handle thread(created.hThread);
	if (!AssignProcessToJobObject(job.get(), process.get())) {
		const DWORD error = GetLastError();
		TerminateProcess(process.get(), 1);
		fail("Windows could not take ownership of the local process", error);
	}
	if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
		const DWORD error = GetLastError();
		TerminateJobObject(job.get(), 1);
		fail("The local process could not begin running", error);
	}
	state_->process = process.release();
	state_->job = job.release();
	state_->process_id = created.dwProcessId;
}

OwnedChildProcess::~OwnedChildProcess() { terminate(); }

bool OwnedChildProcess::running() const noexcept {
	return state_ && state_->process != nullptr && WaitForSingleObject(state_->process, 0) == WAIT_TIMEOUT;
}

std::int64_t OwnedChildProcess::process_id() const noexcept {
	return state_ && state_->process != nullptr ? static_cast<std::int64_t>(state_->process_id) : -1;
}

void OwnedChildProcess::terminate(std::chrono::milliseconds) noexcept {
	if (!state_) return;
	// A console-less child has no request-to-exit channel on Windows (there
	// is no SIGTERM), so it is ended at once - together with anything it
	// started, since the whole job is terminated.
	if (state_->job != nullptr) (void)TerminateJobObject(state_->job, 1);
	if (state_->process != nullptr) {
		(void)WaitForSingleObject(state_->process, 5000);
		CloseHandle(state_->process);
		state_->process = nullptr;
	}
	if (state_->job != nullptr) {
		CloseHandle(state_->job);
		state_->job = nullptr;
	}
	state_->process_id = 0;
}

ChildProcessResourceUsage OwnedChildProcess::resource_usage() const noexcept {
	ChildProcessResourceUsage usage;
	if (!running()) return usage;
	PROCESS_MEMORY_COUNTERS counters{};
	if (GetProcessMemoryInfo(state_->process, &counters, sizeof(counters))) {
		usage.resident_memory_bytes = counters.WorkingSetSize;
		usage.peak_resident_memory_bytes = counters.PeakWorkingSetSize;
	}
	// Windows counts page faults (soft and hard together) and read I/O
	// through the file cache, neither of which means "fetched from storage";
	// page_ins and storage_read_bytes therefore stay unknown.
	return usage;
}

std::optional<std::string> OwnedChildProcess::exit_reason() const {
	if (!state_ || state_->process == nullptr || WaitForSingleObject(state_->process, 0) != WAIT_OBJECT_0)
		return std::nullopt;
	DWORD code = 0;
	if (!GetExitCodeProcess(state_->process, &code)) return std::nullopt;
	if (const char *described = describe_exit_code(code)) return std::string(described);
	return "exit code " + std::to_string(code);
}

} // namespace singularity::platform
