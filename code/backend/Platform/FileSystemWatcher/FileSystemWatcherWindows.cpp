#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace singularity::platform {
namespace {

std::optional<bool> watching_supported_override_for_testing;

// What one watch owns. The watching thread holds it too, so the handles and
// the callback live as long as the thread does. stop() signals the thread and
// then waits for it (FileSystemWatcher.hpp), so a report being delivered
// finishes before whatever it touches can be torn down - unless stop() is
// called from that report itself, when the thread is left to end on its own.
struct WatchState {
	HANDLE directory = INVALID_HANDLE_VALUE;
	HANDLE stop_event = nullptr;
	HANDLE io_event = nullptr;
	std::filesystem::path root;
	FileSystemChangeCallback on_change;

	~WatchState() {
		if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
		if (stop_event != nullptr) CloseHandle(stop_event);
		if (io_event != nullptr) CloseHandle(io_event);
	}
};

bool stopped(const WatchState &state) { return WaitForSingleObject(state.stop_event, 0) == WAIT_OBJECT_0; }

// ReadDirectoryChangesExW (Windows 10 1709 and later), asked of kernel32 by
// name: MinGW's headers name neither it nor ReadDirectoryNotifyExtendedInformation,
// and both compilers build this one file. A Windows without it watches
// nothing (file_system_watching_supported), and Projects are reconciled whole.
using ReadChangesEx = BOOL(WINAPI *)(HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD, LPOVERLAPPED,
	LPOVERLAPPED_COMPLETION_ROUTINE, int);
constexpr int kReadDirectoryNotifyExtendedInformation = 2;

ReadChangesEx read_changes_ex() {
	static const ReadChangesEx function = [] {
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		return kernel == nullptr ? nullptr
			: reinterpret_cast<ReadChangesEx>(reinterpret_cast<void (*)()>(GetProcAddress(kernel, "ReadDirectoryChangesExW")));
	}();
	return function;
}

// ReadDirectoryChangesExW over the whole tree under the root. Like the macOS
// stream's 0.3s latency, a burst of changes (an application saving a document
// through write-then-rename) is gathered for 0.3s into one report. Its scope
// follows the same rules as FileSystemWatcherMacOS.mm:
//   - a lost batch (the buffer overflowed and Windows dropped events) or a
//     watch that failed is coverage lost: full reconciliation;
//   - an event about a directory - above all a directory rename or move -
//     names only the directory, never the files that moved under it: full
//     reconciliation;
//   - a name with '~' may be an 8.3 short alias rather than the path the
//     Project knows: full reconciliation;
//   - otherwise every reported path, absolute and verbatim, is the report's
//     complete scope, which the registry then re-reads from disk.
void watch(const std::shared_ptr<WatchState> &state) {
	// 64 KiB is the most a network share accepts; DWORD storage keeps the
	// records DWORD-aligned as Windows requires.
	std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));
	const DWORD buffer_bytes = static_cast<DWORD>(buffer.size() * sizeof(DWORD));
	constexpr DWORD kChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
		| FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
	constexpr auto kLatency = std::chrono::milliseconds(300);

	std::vector<std::string> paths;
	bool full = false;
	bool pending = false;
	std::chrono::steady_clock::time_point deadline;
	const auto note = [&] {
		if (pending) return;
		pending = true;
		deadline = std::chrono::steady_clock::now() + kLatency;
	};
	const auto report = [&] {
		if (!pending || stopped(*state)) return;
		FileSystemChangeReport change;
		if (full) {
			change.scope = FileSystemChangeScope::kFullReconciliationRequired;
		} else {
			change.scope = FileSystemChangeScope::kAffectedPaths;
			change.affected_paths = std::move(paths);
		}
		paths.clear();
		full = false;
		pending = false;
		if (state->on_change) state->on_change(change);
	};

	const ReadChangesEx read_changes = read_changes_ex();
	if (read_changes == nullptr) {
		full = true;
		note();
		report();
		return;
	}
	OVERLAPPED overlapped{};
	overlapped.hEvent = state->io_event;
	for (;;) {
		ResetEvent(state->io_event);
		if (!read_changes(state->directory, buffer.data(), buffer_bytes, TRUE, kChanges, nullptr,
				&overlapped, nullptr, kReadDirectoryNotifyExtendedInformation)) {
			// The watch itself is gone (the root was removed, its volume
			// went away): coverage is lost for good.
			full = true;
			note();
			report();
			return;
		}
		for (;;) {
			DWORD timeout = INFINITE;
			if (pending) {
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now());
				timeout = left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
			}
			const HANDLE events[] = {state->stop_event, state->io_event};
			const DWORD signaled = WaitForMultipleObjects(2, events, FALSE, timeout);
			if (signaled == WAIT_OBJECT_0) {
				CancelIoEx(state->directory, &overlapped);
				DWORD ignored = 0;
				GetOverlappedResult(state->directory, &overlapped, &ignored, TRUE);
				return;
			}
			if (signaled == WAIT_TIMEOUT) {
				report();
				continue;
			}
			break;
		}
		DWORD transferred = 0;
		if (!GetOverlappedResult(state->directory, &overlapped, &transferred, FALSE)) {
			full = true;
			note();
			report();
			return;
		}
		note();
		if (transferred == 0) {
			full = true;
			continue;
		}
		const auto *bytes = reinterpret_cast<const unsigned char *>(buffer.data());
		for (DWORD offset = 0;;) {
			const auto *record = reinterpret_cast<const FILE_NOTIFY_EXTENDED_INFORMATION *>(bytes + offset);
			const std::wstring name(record->FileName, record->FileNameLength / sizeof(WCHAR));
			if ((record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || name.find(L'~') != std::wstring::npos)
				full = true;
			else if (!full)
				paths.push_back((state->root / name).u8string());
			if (record->NextEntryOffset == 0) break;
			offset += record->NextEntryOffset;
		}
	}
}

class DirectoryChangesWatcher final : public FileSystemWatcher {
public:
	explicit DirectoryChangesWatcher(std::shared_ptr<WatchState> state)
		: state_(std::move(state)), thread_([state = state_] { watch(state); }) {}

	~DirectoryChangesWatcher() override { stop(); }

	void stop() override {
		if (state_ == nullptr) return;
		SetEvent(state_->stop_event);
		if (thread_.joinable()) {
			// From the watching thread itself (a report that stops its own
			// watcher) there is nothing to wait for: it ends once the report
			// returns. From anywhere else, wait for it.
			if (thread_.get_id() == std::this_thread::get_id())
				thread_.detach();
			else
				thread_.join();
		}
		state_.reset();
	}

private:
	std::shared_ptr<WatchState> state_;
	std::thread thread_;
};

} // namespace

bool file_system_watching_supported() noexcept {
	if (watching_supported_override_for_testing.has_value() == true) return *watching_supported_override_for_testing;
	return read_changes_ex() != nullptr;
}

std::unique_ptr<FileSystemWatcher> start_watching(
		const std::filesystem::path& root, FileSystemChangeCallback onChange) {
	if (file_system_watching_supported() == false) return nullptr;
	std::error_code error;
	if (std::filesystem::is_directory(root, error) == false) return nullptr;
	auto state = std::make_shared<WatchState>();
	state->root = root;
	state->on_change = std::move(onChange);
	state->directory = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	state->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	state->io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	// No watcher is the honest answer when one cannot be set up: the caller
	// then keeps reconciling the whole Project on every listing.
	if (state->directory == INVALID_HANDLE_VALUE || state->stop_event == nullptr || state->io_event == nullptr)
		return nullptr;
	return std::make_unique<DirectoryChangesWatcher>(std::move(state));
}

void set_watching_supported_override_for_testing(std::optional<bool> forced_value) {
	watching_supported_override_for_testing = forced_value;
}

} // namespace singularity::platform
