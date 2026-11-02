#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace singularity::platform {
namespace {

std::optional<bool> watching_supported_override_for_testing;

// What inotify is asked about every folder under the root. inotify watches one
// folder, not a tree, so every folder gets its own watch, and a folder made or
// moved in later is added as it appears.
constexpr std::uint32_t kWatchedEvents = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM
	| IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;

// What one watch owns. The watching thread is the only one that reads the
// inotify descriptor or the folder table once it runs.
struct WatchState {
	int inotify = -1;
	int wake = -1;
	std::atomic<bool> stopping{false};
	std::filesystem::path root;
	FileSystemChangeCallback on_change;
	// Each watch descriptor and the folder it names now. A folder renamed
	// inside the root is forgotten under its old name and watched again under
	// its new one, so a path built from this table is always current.
	std::map<int, std::filesystem::path> folders;
	int root_watch = -1;

	~WatchState() {
		if (inotify >= 0) close(inotify);
		if (wake >= 0) close(wake);
	}
};

enum class AddOutcome { kWatched, kGone, kLimitReached };

// Watches one folder. A folder that vanished or cannot be read is no loss of
// coverage: nothing in it can be listed by the reconciliation either. The
// system's limit on watches (fs.inotify.max_user_watches) is.
AddOutcome watch_folder(WatchState &state, const std::filesystem::path &folder, bool is_root) {
	const int descriptor =
		inotify_add_watch(state.inotify, folder.c_str(), kWatchedEvents | (is_root ? 0u : IN_DONT_FOLLOW));
	if (descriptor < 0) {
		if (errno == ENOSPC || errno == ENOMEM) return AddOutcome::kLimitReached;
		return AddOutcome::kGone;
	}
	state.folders[descriptor] = folder;
	if (is_root) state.root_watch = descriptor;
	return AddOutcome::kWatched;
}

// Watches `top` and every real folder under it. A symlinked folder is never
// followed: its target may lie outside the Matter (DocumentRegistry never
// reads through one either).
bool watch_tree(WatchState &state, const std::filesystem::path &top, bool is_root) {
	std::vector<std::filesystem::path> pending{top};
	bool isFirst = true;
	while (pending.empty() == false) {
		const std::filesystem::path folder = std::move(pending.back());
		pending.pop_back();
		const AddOutcome outcome = watch_folder(state, folder, is_root && isFirst);
		isFirst = false;
		if (outcome == AddOutcome::kLimitReached) return false;
		if (outcome == AddOutcome::kGone) continue;
		std::error_code error;
		for (std::filesystem::directory_iterator entry(folder, std::filesystem::directory_options::skip_permission_denied,
					 error), end;
				 !error && entry != end; entry.increment(error)) {
			std::error_code typeError;
			if (entry->is_symlink(typeError) == false && entry->is_directory(typeError) == true)
				pending.push_back(entry->path());
		}
	}
	return true;
}

// Stops watching `top` and everything under it: the folder left the root, or
// is about to be watched again under a new name.
void forget_tree(WatchState &state, const std::filesystem::path &top) {
	const std::string prefix = top.string() + "/";
	for (auto watch = state.folders.begin(); watch != state.folders.end();) {
		const std::string folder = watch->second.string();
		if (watch->first != state.root_watch && (folder == top.string() || folder.rfind(prefix, 0) == 0)) {
			inotify_rm_watch(state.inotify, watch->first);
			watch = state.folders.erase(watch);
			continue;
		}
		++watch;
	}
}

// The watching thread. Like the macOS stream's 0.3s latency, a burst of changes
// (an application saving a document through write-then-rename, a folder copied
// in) is gathered for 0.3s into one report. Its scope follows the same rules as
// FileSystemWatcherMacOS.mm and FileSystemWatcherWindows.cpp:
//   - a lost batch (the queue overflowed and the kernel dropped events) or a
//     watch that failed is coverage lost: full reconciliation;
//   - an event about a folder - above all a folder rename or move - names only
//     the folder, never the files that moved under it: full reconciliation;
//   - the root itself removed or moved away: full reconciliation, and the
//     watch ends, since nothing is left to watch;
//   - otherwise every reported path, absolute, is the report's complete scope,
//     which the registry then re-reads from disk.
void watch(const std::shared_ptr<WatchState> &state) {
	constexpr auto kLatency = std::chrono::milliseconds(300);
	alignas(struct inotify_event) char buffer[64 * 1024];

	std::vector<std::string> paths;
	std::unordered_set<std::string> named;
	bool full = false;
	bool pending = false;
	std::chrono::steady_clock::time_point deadline;
	const auto note = [&] {
		if (pending) return;
		pending = true;
		deadline = std::chrono::steady_clock::now() + kLatency;
	};
	const auto report = [&] {
		if (!pending || state->stopping) return;
		FileSystemChangeReport change;
		if (full) {
			change.scope = FileSystemChangeScope::kFullReconciliationRequired;
		} else {
			change.scope = FileSystemChangeScope::kAffectedPaths;
			change.affected_paths = std::move(paths);
		}
		paths.clear();
		named.clear();
		full = false;
		pending = false;
		if (state->on_change) state->on_change(change);
	};
	const auto lose_coverage = [&] {
		full = true;
		note();
		report();
	};

	for (;;) {
		int timeout = -1;
		if (pending) {
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			timeout = left.count() > 0 ? static_cast<int>(left.count()) : 0;
		}
		pollfd descriptors[2] = {{state->wake, POLLIN, 0}, {state->inotify, POLLIN, 0}};
		const int ready = poll(descriptors, 2, timeout);
		if (ready < 0) {
			if (errno == EINTR) continue;
			lose_coverage();
			return;
		}
		if (descriptors[0].revents != 0 || state->stopping) return;
		if (ready == 0) {
			report();
			continue;
		}
		if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			lose_coverage();
			return;
		}
		const ssize_t length = read(state->inotify, buffer, sizeof(buffer));
		if (length < 0) {
			if (errno == EAGAIN || errno == EINTR) continue;
			lose_coverage();
			return;
		}
		note();
		bool isRootGone = false;
		for (ssize_t offset = 0; offset < length;) {
			const auto *event = reinterpret_cast<const struct inotify_event *>(buffer + offset);
			offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);
			if ((event->mask & IN_Q_OVERFLOW) != 0) {
				full = true;
				continue;
			}
			if ((event->mask & IN_IGNORED) != 0) {
				state->folders.erase(event->wd);
				if (event->wd == state->root_watch) isRootGone = true;
				continue;
			}
			const auto folder = state->folders.find(event->wd);
			// A folder forgotten a moment ago may still have events queued.
			if (folder == state->folders.end()) continue;
			if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
				full = true;
				if (event->wd == state->root_watch) isRootGone = true;
				continue;
			}
			if (event->len == 0) {
				full = true;
				continue;
			}
			const std::filesystem::path changed = folder->second / event->name;
			if ((event->mask & IN_ISDIR) != 0) {
				full = true;
				if ((event->mask & IN_MOVED_FROM) != 0) forget_tree(*state, changed);
				// A folder that cannot be watched whole leaves part of the Matter
				// unheard: this report already asks for a full look, and the rest
				// stays watched.
				if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) (void)watch_tree(*state, changed, false);
				continue;
			}
			if (full) continue;
			std::string path = changed.string();
			if (named.insert(path).second) paths.push_back(std::move(path));
		}
		if (isRootGone) {
			lose_coverage();
			return;
		}
		// Activity that never pauses still reports once per latency window.
		if (pending && std::chrono::steady_clock::now() >= deadline) report();
	}
}

class InotifyWatcher final : public FileSystemWatcher {
public:
	explicit InotifyWatcher(std::shared_ptr<WatchState> state) : state_(std::move(state)) {
		thread_ = std::thread([state = state_] { watch(state); });
	}

	~InotifyWatcher() override { stop(); }

	// Like FileSystemWatcherMacOS.mm: once stop() returns no callback runs, so
	// whatever the callback captured may go. It waits for a callback already
	// running. Called from one of this watcher's own callbacks it cannot wait
	// for itself: the thread is let go, and ends when that callback returns.
	void stop() override {
		if (thread_.joinable() == false) return;
		state_->stopping = true;
		// An eventfd counter cannot overflow from one write; the flag above is
		// what the thread reads either way.
		const std::uint64_t one = 1;
		const ssize_t woken = write(state_->wake, &one, sizeof(one));
		(void)woken;
		if (thread_.get_id() == std::this_thread::get_id()) {
			thread_.detach();
		} else {
			thread_.join();
		}
	}

private:
	std::shared_ptr<WatchState> state_;
	std::thread thread_;
};

} // namespace

bool file_system_watching_supported() noexcept {
	if (watching_supported_override_for_testing.has_value() == true) return *watching_supported_override_for_testing;
	return true;
}

std::unique_ptr<FileSystemWatcher> start_watching(
		const std::filesystem::path& root, FileSystemChangeCallback onChange) {
	if (file_system_watching_supported() == false) return nullptr;
	std::error_code error;
	if (std::filesystem::is_directory(root, error) == false) return nullptr;
	auto state = std::make_shared<WatchState>();
	state->root = root;
	state->on_change = std::move(onChange);
	state->inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	state->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	// No watcher is the honest answer when one cannot be set up whole - inotify
	// unavailable, or the Matter has more folders than the system lets one
	// person watch: the caller then keeps reconciling the whole Project on
	// every listing.
	if (state->inotify < 0 || state->wake < 0) return nullptr;
	if (watch_tree(*state, root, true) == false || state->root_watch < 0) return nullptr;
	return std::make_unique<InotifyWatcher>(std::move(state));
}

void set_watching_supported_override_for_testing(std::optional<bool> forced_value) {
	watching_supported_override_for_testing = forced_value;
}

} // namespace singularity::platform
