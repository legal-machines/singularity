#include "Platform/FileSystemWatcher/FileSystemWatcher.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace singularity::platform {
namespace {

std::optional<bool> watching_supported_override_for_testing;

// FSEvents flags that mean this event batch's per-path detail cannot be
// trusted as the complete affected set (Phase 08 correction round 3, item
// 1): MustScanSubDirs/UserDropped/KernelDropped are FSEvents' own documented
// "event coverage was lost, re-scan instead" signals; RootChanged means the
// watched root itself (or an ancestor) was moved/recreated/deleted;
// Mount/Unmount means a volume appeared/disappeared under the root. Any one
// of these anywhere in a batch is treated as invalidating the whole batch -
// batches are already coalesced within the stream's own 0.3s latency window,
// so a mix of trustworthy and untrustworthy events in one callback is rare,
// and correctness matters more here than avoiding a rare full scan.
//
// kFSEventStreamEventFlagItemIsDir is included too: even with
// kFSEventStreamCreateFlagFileEvents (real per-file paths for FILE events),
// a directory-level event - most importantly a directory rename/move - only
// ever reports the directory's own path, never every file that logically
// moved underneath it. There is no way to enumerate the real, current set of
// affected descendant files from the event alone, so this is exactly the
// "directory rename/move where affected children cannot be known" case the
// correction calls out - handled by falling back to a full reconciliation,
// which naturally recomputes every real path under the Project from disk.
constexpr FSEventStreamEventFlags kFlagsRequiringFullReconciliation = kFSEventStreamEventFlagMustScanSubDirs |
		kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped |
		kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount |
		kFSEventStreamEventFlagItemIsDir;

class FSEventsWatcher final : public FileSystemWatcher {
public:
	FSEventsWatcher(const std::filesystem::path& root, FileSystemChangeCallback onChange)
			: onChange_(std::move(onChange)) {
		CFStringRef pathString =
				CFStringCreateWithCString(kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
		CFArrayRef paths = CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void**>(&pathString), 1,
				&kCFTypeArrayCallBacks);
		FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};
		// 0.3s latency lets the OS coalesce a burst of individual writes (e.g.
		// an application saving a document via write-then-rename) into one
		// callback, matching the "watcher coalescing" requirement in
		// phase-08-documents-filesystem-states.md Sec.7 - callers still always
		// reconcile against real current state, never trust event content.
		//
		// kFSEventStreamCreateFlagFileEvents asks FSEvents for per-FILE paths
		// in the callback (eventPaths below) instead of only the containing
		// directory - without it, an edit deep in a large Project would only
		// ever be reported as "something under this directory changed," which
		// is exactly the information correction item 2 needs to avoid a full-
		// tree rehash on every event: with it, handleEvents() below can hand
		// Document/DocumentRegistry.hpp::reconcile_affected_paths() the real
		// changed file(s) directly.
		stream_ = FSEventStreamCreate(kCFAllocatorDefault, &FSEventsWatcher::handleEvents, &context, paths,
				kFSEventStreamEventIdSinceNow, 0.3,
				kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagFileEvents);
		CFRelease(paths);
		CFRelease(pathString);

		queue_ = dispatch_queue_create("com.legalmachines.singularity.filesystemwatcher", DISPATCH_QUEUE_SERIAL);
		// Marks the queue as this watcher's own, so stop() can tell whether it
		// runs inside one of this watcher's callbacks.
		dispatch_queue_set_specific(queue_, &kQueueKey, this, nullptr);
		FSEventStreamSetDispatchQueue(stream_, queue_);
		FSEventStreamStart(stream_);
	}

	~FSEventsWatcher() override { stop(); }

	// Stopping a stream does not stop a callback already running on the queue:
	// FSEvents only promises that no NEW callback starts. The watcher - and the
	// callback it owns, with everything that callback captured (the Project
	// id a reading pass holds, say) - must outlive that running callback, so
	// stop() waits for the queue to drain before the watcher can be destroyed.
	// Called from one of this watcher's own callbacks it cannot wait for
	// itself: that callback is then the one still running, and it ends first.
	void stop() override {
		if (stream_ == nullptr) return;
		FSEventStreamStop(stream_);
		FSEventStreamInvalidate(stream_);
		FSEventStreamRelease(stream_);
		stream_ = nullptr;
		if (queue_ != nullptr && dispatch_get_specific(&kQueueKey) != this) dispatch_sync(queue_, ^{});
	}

private:
	// eventPaths is a plain `const char* const*` (numEvents entries) because
	// the stream was created without kFSEventStreamCreateFlagUseCFTypes above
	// - each real absolute path the OS reported, verbatim, handed straight to
	// onChange_ with no filtering here: Document/DocumentRegistry.hpp's
	// reconcile_affected_paths() is the one place that decides what to trust
	// (boundary/symlink checks, existence, etc.), not this platform layer.
	// This function's own job (Phase 08 correction round 3, item 1) is only
	// deciding scope - kAffectedPaths (paths trustworthy) vs
	// kFullReconciliationRequired (fall back to a full scan) - from the real
	// eventFlags FSEvents reports, never leaking those raw flags themselves
	// past this file.
	static void handleEvents(ConstFSEventStreamRef, void* info, size_t numEvents, void* eventPaths,
			const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId[]) {
		FSEventsWatcher* watcher = static_cast<FSEventsWatcher*>(info);
		if (!watcher->onChange_) return;

		// An empty batch has no per-path detail to act on at all - "empty or
		// otherwise incomplete event scope" from the correction - so it is not
		// safe to read as "nothing happened."
		if (numEvents == 0) {
			watcher->onChange_(FileSystemChangeReport{FileSystemChangeScope::kFullReconciliationRequired, {}});
			return;
		}

		const char* const* rawPaths = static_cast<const char* const*>(eventPaths);
		std::vector<std::string> paths;
		paths.reserve(numEvents);
		bool fullReconciliationRequired = false;
		for (size_t index = 0; index < numEvents; ++index) {
			if ((eventFlags[index] & kFlagsRequiringFullReconciliation) != 0) fullReconciliationRequired = true;
			paths.emplace_back(rawPaths[index]);
		}

		if (fullReconciliationRequired == true) {
			watcher->onChange_(FileSystemChangeReport{FileSystemChangeScope::kFullReconciliationRequired, {}});
			return;
		}
		watcher->onChange_(FileSystemChangeReport{FileSystemChangeScope::kAffectedPaths, std::move(paths)});
	}

	static inline char kQueueKey = 0;
	FileSystemChangeCallback onChange_;
	FSEventStreamRef stream_ = nullptr;
	dispatch_queue_t queue_ = nullptr;
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
	return std::make_unique<FSEventsWatcher>(root, std::move(onChange));
}

void set_watching_supported_override_for_testing(std::optional<bool> forced_value) {
	watching_supported_override_for_testing = forced_value;
}

} // namespace singularity::platform
