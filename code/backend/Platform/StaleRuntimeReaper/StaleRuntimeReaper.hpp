#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace singularity::platform::stale_runtime {

// Process and user ids as the OS reports them, as plain integers: the
// selection below is the same on every platform. macOS and Linux reap the
// runtimes an Singularity that ended without stopping them left behind. On
// Windows local inference's Job Object ends a runtime with its owner. What can be left
// there is a runtime Singularity started suspended and then died before the
// runtime joined its job: it never runs, yet it holds llama-server.exe and its
// slot folder, and that is what Windows reaps. Windows names users by SID, not
// by number, so its records hold only this user's processes, compared by SID
// beforehand, and all carry uid 0.
using ProcessId = std::int64_t;
using UserId = std::uint32_t;

// What Singularity knows about one running process when deciding whether it is
// a local model runtime a previous Singularity left behind.
struct ProcessRecord {
	ProcessId pid = 0;
	ProcessId parent_pid = 0;
	UserId uid = 0;
	std::string executable_path;
	std::vector<std::string> arguments;
};

struct StaleRuntime {
	ProcessId pid = 0;
	ProcessId owner_pid = 0;
	std::filesystem::path slot_directory;
};

// The pid a runtime's --slot-save-path names ("…/local-inference-slots-<pid>-<hex>",
// or an earlier release's "…/<word>-runtime-slots-<pid>-<hex>"),
// or 0 when the arguments carry no such path.
ProcessId runtime_owner_pid(const std::vector<std::string> &arguments, std::filesystem::path *slot_directory = nullptr);

// Selects only runtimes that are certainly orphans of this app: the exact
// bundled llama-server binary, run by this user, no longer the child of a live
// Singularity (re-parented to launchd, to init or to a session's subreaper),
// whose slot path names an owner that is no longer an Singularity process.
// Anything else - another app's llama-server, a runtime with a live owner, a
// process run by another user - is never selected.
std::vector<StaleRuntime> select_stale_runtimes(const std::vector<ProcessRecord> &processes,
	const std::string &bundled_runtime_path, UserId current_uid,
	const std::function<bool(ProcessId)> &is_live_singularity_process);

// Finds and ends this app's orphaned local model runtimes (SIGTERM, then
// SIGKILL after a short grace period; on Windows TerminateProcess, since a
// suspended runtime has nothing to save) and removes their slot directories.
// Never throws. Returns how many runtimes were ended.
std::size_t reap_stale_runtimes();

} // namespace singularity::platform::stale_runtime
