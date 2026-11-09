#include "Platform/ChildProcess/ChildProcess.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(__APPLE__)
#include <crt_externs.h>
#include <libproc.h>
#include <sys/resource.h>
#else
extern char **environ;
#endif

namespace singularity::platform {
namespace {

// The process environment. On Apple platforms code that may end up in a
// dynamic library has no `environ` symbol to link against; the loader
// provides it through _NSGetEnviron() instead.
char **process_environment() {
#if defined(__APPLE__)
	return *_NSGetEnviron();
#else
	return environ;
#endif
}

} // namespace

struct OwnedChildProcess::State {
	pid_t worker = -1;
	pid_t supervisor = -1;
	int guard_write = -1;
};

namespace {

// Everything the forked processes use is built here, before fork(): between
// fork() and execve() a child of a multi-threaded process may only make
// async-signal-safe calls, and allocating (std::string, setenv) is not one.
struct PreparedLaunch {
	std::string executable;
	std::string log;
	std::vector<std::string> argument_storage;
	std::vector<char *> argv;
	std::vector<std::string> environment_storage;
	std::vector<char *> envp;
	int max_descriptor = 0;
};

PreparedLaunch prepare_launch(const ChildProcessSpec &spec) {
	PreparedLaunch launch;
	launch.executable = spec.executable.string();
	launch.log = spec.output_log.empty() ? std::string("/dev/null") : spec.output_log.string();

	launch.argument_storage.push_back(launch.executable);
	for (const auto &argument : spec.arguments) launch.argument_storage.push_back(argument);
	for (auto &argument : launch.argument_storage) launch.argv.push_back(argument.data());
	launch.argv.push_back(nullptr);

	// Later entries win, exactly like successive setenv(..., 1) calls.
	std::vector<std::pair<std::string, std::string>> overrides;
	auto set_override = [&overrides](const std::string &name, const std::string &value) {
		for (auto &entry : overrides) {
			if (entry.first == name) {
				entry.second = value;
				return;
			}
		}
		overrides.emplace_back(name, value);
	};
	for (const auto &[name, value] : spec.environment) set_override(name, value);
	if (!spec.library_directory.empty()) {
#if defined(__APPLE__)
		set_override("DYLD_LIBRARY_PATH", spec.library_directory.string());
#else
		set_override("LD_LIBRARY_PATH", spec.library_directory.string());
#endif
	}
	for (char **entry = process_environment(); entry != nullptr && *entry != nullptr; ++entry) {
		bool replaced = false;
		for (const auto &override_entry : overrides) {
			const std::string &name = override_entry.first;
			if (std::strncmp(*entry, name.c_str(), name.size()) == 0 && (*entry)[name.size()] == '=') {
				replaced = true;
				break;
			}
		}
		if (!replaced) launch.environment_storage.emplace_back(*entry);
	}
	for (const auto &[name, value] : overrides) launch.environment_storage.push_back(name + "=" + value);
	for (auto &entry : launch.environment_storage) launch.envp.push_back(entry.data());
	launch.envp.push_back(nullptr);

	const long max_descriptor = ::sysconf(_SC_OPEN_MAX);
	launch.max_descriptor = max_descriptor > 0 ? static_cast<int>(max_descriptor) : 1024;
	return launch;
}

bool close_on_exec(int descriptor) {
	const int flags = ::fcntl(descriptor, F_GETFD);
	return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

void close_pair(int (&pipe_fds)[2]) {
	for (int &descriptor : pipe_fds) {
		if (descriptor >= 0) ::close(descriptor);
		descriptor = -1;
	}
}

bool alive(pid_t process) { return process > 0 && ::kill(process, 0) == 0; }

#if defined(__linux__)
std::optional<std::uint64_t> proc_status_kilobytes(pid_t process, const char *key) {
	std::ifstream status("/proc/" + std::to_string(process) + "/status");
	std::string line;
	const std::string prefix = std::string(key) + ":";
	while (std::getline(status, line)) {
		if (line.rfind(prefix, 0) != 0) continue;
		std::istringstream value(line.substr(prefix.size()));
		std::uint64_t kilobytes = 0;
		if (value >> kilobytes) return kilobytes * 1024;
		return std::nullopt;
	}
	return std::nullopt;
}
#endif

} // namespace

OwnedChildProcess::OwnedChildProcess(const ChildProcessSpec &spec) : state_(std::make_unique<State>()) {
	if (spec.executable.empty()) throw std::invalid_argument("A child process executable is required.");
	const PreparedLaunch launch = prepare_launch(spec);

	int guard_pipe[2] = {-1, -1};
	int info_pipe[2] = {-1, -1};
	if (::pipe(guard_pipe) != 0 || ::pipe(info_pipe) != 0 || !close_on_exec(guard_pipe[0]) ||
		!close_on_exec(guard_pipe[1]) || !close_on_exec(info_pipe[0]) || !close_on_exec(info_pipe[1])) {
		close_pair(guard_pipe);
		close_pair(info_pipe);
		throw std::runtime_error("The local process could not be started.");
	}

	// The child is launched two forks deep, through a small guard supervisor,
	// so it is force-killed the instant THIS process disappears for ANY
	// reason - SIGKILL, a crash, the terminal closing - none of which run a
	// single line of cleanup code here. The supervisor blocks on the read end
	// of `guard_pipe`; this process holds the write end open for exactly as
	// long as it is alive (State::guard_write), so the kernel closes it - and
	// wakes the supervisor with EOF - the moment this process exits, by any
	// means whatsoever.
	const pid_t supervisor = ::fork();
	if (supervisor < 0) {
		close_pair(guard_pipe);
		close_pair(info_pipe);
		throw std::runtime_error("The local process could not be started.");
	}
	if (supervisor == 0) {
		::close(guard_pipe[1]);
		::close(info_pipe[0]);
		for (int descriptor = 0; descriptor < launch.max_descriptor; ++descriptor) {
			if (descriptor == guard_pipe[0] || descriptor == info_pipe[1]) continue;
			::close(descriptor);
		}
		::setpgid(0, 0);

		const pid_t child = ::fork();
		if (child == 0) {
			// Its OWN process group before anything else.
			::setpgid(0, 0);
			// stdin stays closed; stdout/stderr go to the log.
			const int log = ::open(launch.log.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
			if (log >= 0) {
				::dup2(log, STDOUT_FILENO);
				::dup2(log, STDERR_FILENO);
				if (log != STDOUT_FILENO && log != STDERR_FILENO) ::close(log);
			}
			::execve(launch.executable.c_str(), launch.argv.data(), launch.envp.data());
			// execve only returns on failure.
			::_exit(127);
		}
		if (child < 0) ::_exit(127);

		const pid_t worker = child;
		[[maybe_unused]] const ssize_t reported = ::write(info_pipe[1], &worker, sizeof(worker));
		::close(info_pipe[1]);

		// Supervise: force-kill the worker the instant either (a) it has
		// already exited on its own - nothing left to guard, or (b) the
		// guard pipe's write end closes, meaning the process that owns it is
		// gone, by any means whatsoever.
		for (;;) {
			int status = 0;
			if (::waitpid(worker, &status, WNOHANG) == worker) break;
			struct pollfd watched {};
			watched.fd = guard_pipe[0];
			watched.events = POLLIN;
			(void)::poll(&watched, 1, 500);
			if (watched.revents & (POLLHUP | POLLERR)) {
				::kill(worker, SIGKILL);
				(void)::waitpid(worker, &status, 0);
				break;
			}
		}
		::_exit(0);
	}

	::close(guard_pipe[0]);
	::close(info_pipe[1]);
	pid_t worker = -1;
	ssize_t read_bytes = -1;
	do {
		read_bytes = ::read(info_pipe[0], &worker, sizeof(worker));
	} while (read_bytes < 0 && errno == EINTR);
	::close(info_pipe[0]);
	if (read_bytes != static_cast<ssize_t>(sizeof(worker)) || worker <= 0) {
		::close(guard_pipe[1]);
		int status = 0;
		(void)::waitpid(supervisor, &status, 0);
		throw std::runtime_error("The local process could not be started.");
	}
	state_->worker = worker;
	state_->supervisor = supervisor;
	state_->guard_write = guard_pipe[1];
}

OwnedChildProcess::~OwnedChildProcess() { terminate(); }

bool OwnedChildProcess::running() const noexcept {
	// The worker is a grandchild (see the constructor), so kill(pid, 0) is
	// the liveness probe rather than waitpid().
	return state_ && alive(state_->worker);
}

std::int64_t OwnedChildProcess::process_id() const noexcept {
	return state_ ? static_cast<std::int64_t>(state_->worker) : -1;
}

void OwnedChildProcess::terminate(std::chrono::milliseconds grace) noexcept {
	if (!state_) return;
	const auto wait_until_gone = [](pid_t process, std::chrono::milliseconds limit) {
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (alive(process) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	};
	if (state_->worker > 0) {
		::kill(state_->worker, SIGTERM);
		wait_until_gone(state_->worker, grace);
		if (alive(state_->worker)) {
			::kill(state_->worker, SIGKILL);
			wait_until_gone(state_->worker, std::chrono::milliseconds(1000));
		}
		state_->worker = -1;
	}
	if (state_->guard_write >= 0) {
		// Closing our end now lets the supervisor notice and exit at once
		// instead of on its next poll timeout.
		::close(state_->guard_write);
		state_->guard_write = -1;
	}
	if (state_->supervisor > 0) {
		int status = 0;
		bool reaped = false;
		for (int attempt = 0; attempt < 50; ++attempt) {
			if (::waitpid(state_->supervisor, &status, WNOHANG) == state_->supervisor) {
				reaped = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (!reaped) {
			::kill(state_->supervisor, SIGKILL);
			(void)::waitpid(state_->supervisor, &status, 0);
		}
		state_->supervisor = -1;
	}
}

ChildProcessResourceUsage OwnedChildProcess::resource_usage() const noexcept {
	ChildProcessResourceUsage usage;
	if (!state_ || state_->worker <= 0) return usage;
#if defined(__APPLE__)
	rusage_info_v4 info{};
	if (proc_pid_rusage(static_cast<int>(state_->worker), RUSAGE_INFO_V4,
		reinterpret_cast<rusage_info_t *>(&info)) == 0) {
		usage.resident_memory_bytes = info.ri_resident_size;
		usage.peak_resident_memory_bytes = info.ri_lifetime_max_phys_footprint;
		usage.page_ins = info.ri_pageins;
		usage.storage_read_bytes = info.ri_diskio_bytesread;
	}
#elif defined(__linux__)
	try {
		usage.resident_memory_bytes = proc_status_kilobytes(state_->worker, "VmRSS");
		usage.peak_resident_memory_bytes = proc_status_kilobytes(state_->worker, "VmHWM");
		// read_bytes counts what the process really caused to be fetched
		// from the storage layer - the same meaning as Darwin's
		// ri_diskio_bytesread.
		std::ifstream io("/proc/" + std::to_string(state_->worker) + "/io");
		std::string key;
		std::uint64_t value = 0;
		while (io >> key >> value) {
			if (key == "read_bytes:") {
				usage.storage_read_bytes = value;
				break;
			}
		}
		// Field 12 of /proc/<pid>/stat: major faults, i.e. faults that had to
		// read a page in - the Linux counterpart of Darwin's page-ins.
		std::ifstream stat("/proc/" + std::to_string(state_->worker) + "/stat");
		std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
		const auto command_end = content.rfind(')');
		if (command_end != std::string::npos) {
			std::istringstream fields(content.substr(command_end + 2));
			std::string field;
			for (int index = 3; index <= 12 && (fields >> field); ++index) {
				if (index == 12) usage.page_ins = std::stoull(field);
			}
		}
	} catch (...) {
	}
#endif
	return usage;
}

std::optional<std::string> OwnedChildProcess::exit_reason() const {
	// The guard supervisor, not this process, reaps the worker, so its exit
	// status is not observable here.
	return std::nullopt;
}

} // namespace singularity::platform
