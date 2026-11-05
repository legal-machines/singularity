#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include "Platform/Sha256/Sha256.hpp"

#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <system_error>
#include <vector>

extern char **environ;

namespace singularity::platform::update_install {

namespace {

std::string byHand(const std::string &why) {
	return why + ", so this update is installed by hand.";
}

bool isExecutable(const char *path) {
	return access(path, X_OK) == 0;
}

// Spawns `argv` with nothing of this process's but the standard streams, which
// read and write nothing; `detached` in a session of its own, never waited for.
// Its exit status when waited for, else 0 once started; -1 when it did not run.
int spawn(const std::vector<std::string> &arguments, bool detached) {
	std::vector<char *> argv;
	for (const std::string &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
	argv.push_back(nullptr);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
	// Nothing else this process holds - a socket it listens on above all - may
	// outlive it in the helper, or the new version could not take it up.
	posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	if (detached) posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
	pid_t pid = 0;
	const int started = posix_spawn(&pid, argv[0], &actions, &attributes, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attributes);
	if (started != 0) return -1;
	if (detached) return 0;
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::filesystem::path runningExecutable() {
	std::error_code error;
	const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
	return error ? std::filesystem::path() : path;
}

} // namespace

std::string prepare(const Update &update) {
	try {
		const std::filesystem::path executable = runningExecutable();
		if (executable.empty()) return byHand("Singularity could not find itself");
		// Installed from its package, as dpkg knows it: a copy run from
		// anywhere else is not one the package replaces.
		if (spawn({"/usr/bin/dpkg-query", "-S", executable.string()}, false) != 0) {
			return byHand("Singularity was not installed from its package here");
		}
		if (!isExecutable("/usr/bin/pkexec")) return byHand("This computer cannot ask for the password an install needs");
		if (!isExecutable("/usr/bin/dpkg")) return byHand("This computer has no package installer");
		if (platform::sha256_hex_digest_of_file(update.installer) != update.sha256) {
			return byHand("The downloaded update changed on disk");
		}
		return "";
	} catch (...) {
		return byHand("The update could not be readied");
	}
}

bool hand_over(const Update &update, bool reopen) {
	try {
		const std::filesystem::path executable = runningExecutable();
		if (executable.empty()) return false;
		// Waits for Singularity to end, checks the package once more, installs
		// it once the system has the member's password, says what happened for
		// the next start, and opens Singularity again when asked to.
		static const char *const script =
			"pid=$1; installer=$2; sum=$3; app=$4; reopen=$5; folder=$6; version=$7\n"
			"i=0\n"
			"while kill -0 \"$pid\" 2>/dev/null; do i=$((i+1)); [ \"$i\" -gt 600 ] && exit 1; sleep 0.1; done\n"
			"say() { printf '%s\\t%s\\t%s\\n' \"$1\" \"$version\" \"$2\" > \"$folder/outcome.txt.part\""
			" && mv -f \"$folder/outcome.txt.part\" \"$folder/outcome.txt\"; }\n"
			"if [ \"$(sha256sum \"$installer\" | cut -d' ' -f1)\" != \"$sum\" ]; then\n"
			"  say failed \"Singularity $version could not install itself: the downloaded update changed on disk.\"\n"
			"else\n"
			"  pkexec /usr/bin/dpkg -i \"$installer\"\n"
			"  code=$?\n"
			"  if [ \"$code\" -eq 0 ]; then say installed \"\"\n"
			"  elif [ \"$code\" -eq 126 ] || [ \"$code\" -eq 127 ]; then"
			" say refused \"Singularity $version was not installed: the password was not given. It asks again when Singularity next closes.\"\n"
			"  else say failed \"Singularity $version could not install itself: its package did not install.\"\n"
			"  fi\n"
			"fi\n"
			"if [ \"$reopen\" = \"1\" ]; then exec \"$app\"; fi\n";
		return spawn({"/bin/sh", "-c", script, "singularity-update", std::to_string(getpid()), update.installer.string(),
			              update.sha256, executable.string(), reopen ? "1" : "0", update.installer.parent_path().string(), update.version},
			       true) == 0;
	} catch (...) {
		return false;
	}
}

void tidy(const std::filesystem::path &folder) {
	std::error_code error;
	std::filesystem::remove(folder / "outcome.txt.part", error);
}

} // namespace singularity::platform::update_install
