#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform {

// A real OS-level notification stream for external filesystem activity under
// one root - a hint that Document/DocumentRegistry.hpp's targeted
// reconciliation (reconcile_affected_paths()) should run again over the
// reported paths, never itself a source of truth. Destroying the instance
// (or calling stop()) is the only way to stop watching.
//
// stop() and destruction wait for a report the watcher is delivering to
// finish, unless they are called from that report: nothing a report touches
// is torn down under it (a Matter's watch still reconciling while the process
// exits destroyed the state it was using). So never stop a watcher while
// holding a lock its report takes.
class FileSystemWatcher {
public:
	virtual ~FileSystemWatcher() = default;

	virtual void stop() = 0;
};

// True when start_watching is backed by a real OS watcher on this platform
// (currently: macOS, via FSEvents - see FileSystemWatcherMacOS.mm). Phase 08
// correction round 3, item 1: Matter/MatterActions/MatterActions.cpp's
// list_documents relies on this being false meaning "no watcher will ever
// register for this Matter" to keep re-running the full startup/restart audit
// scan on every call - see that function's own comment.
[[nodiscard]] bool file_system_watching_supported() noexcept;

// Whether `affected_paths` below can be relied on as the complete, narrow
// set of paths a caller needs to reconcile, or whether real OS event
// coverage was lost or is otherwise not trustworthy for this notification -
// e.g. macOS FSEvents' own MustScanSubDirs/UserDropped/KernelDropped
// conditions, a directory-level rename/move whose descendants cannot be
// enumerated from the event itself, or a batch with no usable per-path
// detail at all. This is a small platform-generic abstraction over those
// OS-specific semantics (see FileSystemWatcherMacOS.mm for how macOS maps
// its own FSEventStreamEventFlags onto it) - callers must never see raw OS
// event flags, only this.
enum class FileSystemChangeScope {
	kAffectedPaths,
	kFullReconciliationRequired,
};

// `scope == kAffectedPaths`: affected_paths lists real absolute paths of
// files/directories the OS reported changed (verbatim OS event data, never
// fabricated) and can be trusted as this notification's complete scope - a
// caller may reconcile just those paths (Document/DocumentRegistry.hpp's
// reconcile_affected_paths()) rather than the whole Project. Still only a
// narrowing HINT for what to look at first, never a guarantee of what
// changed: the reconciling code re-stats/re-hashes the real file, it never
// trusts the event's own claim about what happened.
//
// `scope == kFullReconciliationRequired`: event coverage for this
// notification was lost or is otherwise not safely narrowable to specific
// paths - affected_paths is empty and must not be used. A caller must fall
// back to the same full-Project reconciliation the startup/restart audit
// path already runs (Document/DocumentRegistry.hpp::detect_filesystem_changes())
// rather than silently doing nothing. Correctness is more important than
// avoiding a rare full scan here.
struct FileSystemChangeReport {
	FileSystemChangeScope scope = FileSystemChangeScope::kFullReconciliationRequired;
	std::vector<std::string> affected_paths;
};

// `on_report` is invoked from a background thread, coalesced by the
// platform implementation to at most a few calls per second even under
// heavy activity - callers must not assume one callback maps to one
// filesystem event.
using FileSystemChangeCallback = std::function<void(const FileSystemChangeReport& report)>;

// Starts watching `root` recursively for external filesystem activity.
// Returns nullptr when file_system_watching_supported() is false.
[[nodiscard]] std::unique_ptr<FileSystemWatcher> start_watching(
		const std::filesystem::path& root, FileSystemChangeCallback on_change);

// Test-only seam (Phase 08 correction round 3, item 1): forces
// file_system_watching_supported()/start_watching() to behave as if no live
// OS watcher exists on this platform (std::nullopt restores real platform
// behavior), so the "watcher unavailable -> list_documents() keeps
// re-running the full audit scan" contract can be proven deterministically
// on a real macOS host too, without needing an actual non-macOS CI machine.
// Production code must never call this.
void set_watching_supported_override_for_testing(std::optional<bool> forced_value);

} // namespace singularity::platform
