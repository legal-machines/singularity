#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::platform {

struct ChildProcessSpec {
	std::filesystem::path executable;
	// Arguments after argv[0], as UTF-8.
	std::vector<std::string> arguments;
	// Added to the inherited environment, replacing a variable of the same
	// name.
	std::vector<std::pair<std::string, std::string>> environment;
	// Where the child's dynamic loader must find the libraries shipped next
	// to it: DYLD_LIBRARY_PATH (macOS) or LD_LIBRARY_PATH (Linux) is set to
	// it; on Windows it is prepended to PATH. Empty leaves the loader alone.
	std::filesystem::path library_directory;
	// Receives the child's stdout and stderr, created/truncated with
	// owner-only access. Empty discards both. The child never reads the
	// parent's stdin.
	std::filesystem::path output_log;
};

// What the OS reports about a running child. Every field is optional: a
// quantity a platform does not measure stays unknown, never zero.
struct ChildProcessResourceUsage {
	std::optional<std::uint64_t> resident_memory_bytes;
	std::optional<std::uint64_t> peak_resident_memory_bytes;
	std::optional<std::uint64_t> page_ins;
	// Bytes actually fetched from storage (not merely read through the
	// file cache) - reported only where the OS counts exactly that.
	std::optional<std::uint64_t> storage_read_bytes;
};

// A child process owned by this process for the child's whole life, with
// three guarantees on every platform:
//
//  1. It can never outlive its owner. It is force-killed the moment the
//     owning process disappears for any reason - a crash, SIGKILL or
//     TerminateProcess, a closed terminal - none of which run cleanup code.
//     POSIX launches it through a small guard supervisor that watches a
//     pipe only the owner holds; Windows places it in a kill-on-close Job
//     Object only the owner holds a handle to.
//  2. It is isolated from the terminal's interrupt. A user's Ctrl-C reaches
//     only the owner's cooperative cancellation, never the child (its own
//     process group on POSIX; a new process group without a console on
//     Windows).
//  3. It inherits nothing but its redirected standard streams.
//
// Destroying the object terminates the child.
class OwnedChildProcess final {
public:
	// Throws std::runtime_error with a plain-language reason when the child
	// cannot be started with every guarantee above.
	explicit OwnedChildProcess(const ChildProcessSpec &spec);
	~OwnedChildProcess();
	OwnedChildProcess(const OwnedChildProcess &) = delete;
	OwnedChildProcess &operator=(const OwnedChildProcess &) = delete;

	// A real liveness probe of the child, never a remembered state.
	bool running() const noexcept;
	std::int64_t process_id() const noexcept;
	// Asks the child to exit, waits up to `grace`, then kills it (on
	// Windows, together with anything it started). Idempotent.
	void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(1000)) noexcept;
	ChildProcessResourceUsage resource_usage() const noexcept;
	// Why an exited child stopped, in plain language, where the platform
	// reports it to the owner ("a library it needs is missing", "exit code
	// 3"); std::nullopt while running or when unknown.
	std::optional<std::string> exit_reason() const;

	struct State;

private:
	std::unique_ptr<State> state_;
};

} // namespace singularity::platform
