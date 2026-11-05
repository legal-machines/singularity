#include "Platform/UpdateInstall/UpdateInstall.hpp"

#include "Platform/Sha256/Sha256.hpp"

#import <Foundation/Foundation.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <system_error>
#include <vector>

extern char **environ;

namespace singularity::platform::update_install {

namespace {

// The new app, unpacked and checked, waits here until Singularity ends; after
// the swap the old app waits here until the next start removes it.
std::filesystem::path stagedApp(const std::filesystem::path &folder) {
	return folder / "Singularity-update.app";
}

// Where the disk image is attached while the new app is copied out of it.
std::filesystem::path mountPoint(const std::filesystem::path &folder) {
	return folder / "mount";
}

const char *const kByHand = "so this update is installed by hand.";

std::string byHand(const std::string &why) {
	return why + ", " + kByHand;
}

// Runs `tool` with `arguments` - reading nothing, its output discarded - and
// waits for it: its exit status, or -1 when it did not run or end normally.
int run(const char *tool, const std::vector<std::string> &arguments) {
	std::vector<char *> argv;
	argv.push_back(const_cast<char *>(tool));
	for (const std::string &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
	argv.push_back(nullptr);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
	pid_t pid = 0;
	const int started = posix_spawn(&pid, tool, &actions, &attributes, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attributes);
	if (started != 0) return -1;
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The app this process runs from; empty when it is not an app bundle.
std::filesystem::path runningApp() {
	@autoreleasepool {
		NSString *path = [[NSBundle mainBundle] bundlePath];
		if (path == nil || ![path hasSuffix:@".app"]) return {};
		return std::filesystem::path([path fileSystemRepresentation]);
	}
}

// A value of a bundle's Info.plist, read afresh from disk (NSBundle keeps what
// it once read of a path).
std::string infoValue(const std::filesystem::path &bundle, NSString *key) {
	@autoreleasepool {
		NSString *plist = [NSString stringWithUTF8String:(bundle / "Contents" / "Info.plist").c_str()];
		NSDictionary *info = plist == nil ? nil : [NSDictionary dictionaryWithContentsOfFile:plist];
		id value = [info objectForKey:key];
		return [value isKindOfClass:[NSString class]] ? std::string([(NSString *)value UTF8String]) : std::string();
	}
}

bool sameVolume(const std::filesystem::path &first, const std::filesystem::path &second) {
	struct stat a {};
	struct stat b {};
	return stat(first.c_str(), &a) == 0 && stat(second.c_str(), &b) == 0 && a.st_dev == b.st_dev;
}

void detach(const std::filesystem::path &mount) {
	std::error_code error;
	if (!std::filesystem::exists(mount, error)) return;
	if (run("/usr/bin/hdiutil", {"detach", "-quiet", mount.string()}) != 0) {
		run("/usr/bin/hdiutil", {"detach", "-force", "-quiet", mount.string()});
	}
	// Only the empty folder: an image still attached is never emptied by hand.
	std::filesystem::remove(mount, error);
}

// Opens `app` once this process has ended - a new instance, never this one
// ending - through Launch Services, as the member would; in a session of its
// own, so nothing of this process's end reaches it. It carries HOME, so a
// profile of its own stays its own, and a development build's own settings,
// as a helper on Windows or Linux inherits them.
void reopenAfterExit(const std::filesystem::path &app) {
	static const char *const script =
		"pid=$1; shift\n"
		"i=0\n"
		"while kill -0 \"$pid\" 2>/dev/null; do\n"
		"  i=$((i+1)); [ \"$i\" -gt 600 ] && exit 1\n"
		"  sleep 0.1\n"
		"done\n"
		"exec /usr/bin/open \"$@\"\n";
	std::vector<std::string> arguments = {"/bin/sh", "-c", script, "singularity-reopen", std::to_string(getpid()), "-n"};
	std::vector<const char *> carried = {"HOME"};
#if !defined(NDEBUG)
	for (const char *name : {"NATIVE_APP_DEV_SERVER_URL", "NATIVE_APP_ALLOW_REMOTE_DEV_SERVER", "SINGULARITY_UPDATE_FEED", "SINGULARITY_UPDATE_KEY"}) {
		carried.push_back(name);
	}
#endif
	for (const char *name : carried) {
		const char *value = std::getenv(name);
		if (value == nullptr || *value == '\0') continue;
		arguments.push_back("--env");
		arguments.push_back(std::string(name) + "=" + value);
	}
	arguments.push_back(app.string());
	std::vector<char *> argv;
	for (const std::string &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
	argv.push_back(nullptr);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSID);
	pid_t child = 0;
	(void)posix_spawn(&child, "/bin/sh", &actions, &attributes, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attributes);
}

// Why a swap did not happen, in the member's words.
std::string swapProblem(int code) {
	if (code == EPERM || code == EACCES) return "macOS did not let Singularity replace itself";
	if (code == EXDEV) return "Singularity is on another disk than its own records";
	if (code == ENOENT) return "Singularity was moved while it ran";
	return "Singularity could not replace itself";
}

} // namespace

std::string prepare(const Update &update) {
	try {
		const std::filesystem::path folder = update.installer.parent_path();
		const std::filesystem::path target = runningApp();
		if (target.empty()) return byHand("Singularity is not running as an app here");
		// An app macOS runs from a read-only copy of its own, as it does for an
		// app opened straight from a download, cannot be replaced in place.
		if (target.string().find("/AppTranslocation/") != std::string::npos) {
			return byHand("Singularity runs from a copy macOS made of it");
		}
		if (access(target.parent_path().c_str(), W_OK) != 0 || access(target.c_str(), W_OK) != 0) {
			return byHand("Singularity is in a folder this account cannot change");
		}
		if (!sameVolume(folder, target.parent_path())) return byHand("Singularity is on another disk than its own records");
		if (platform::sha256_hex_digest_of_file(update.installer) != update.sha256) {
			return byHand("The downloaded update changed on disk");
		}
		const std::string identifier = infoValue(target, @"CFBundleIdentifier");
		if (identifier.empty()) return byHand("Singularity's own details could not be read");

		std::error_code error;
		const std::filesystem::path staged = stagedApp(folder);
		const std::filesystem::path mount = mountPoint(folder);
		std::filesystem::remove_all(staged, error);
		detach(mount);
		std::filesystem::create_directories(mount, error);
		if (run("/usr/bin/hdiutil", {"attach", "-nobrowse", "-readonly", "-noautoopen", "-quiet", "-mountpoint", mount.string(),
		        update.installer.string()}) != 0) {
			std::filesystem::remove(mount, error);
			return byHand("The update could not be opened");
		}
		std::filesystem::path source;
		for (const auto &entry : std::filesystem::directory_iterator(mount, error)) {
			if (entry.path().extension() == ".app" && infoValue(entry.path(), @"CFBundleIdentifier") == identifier) {
				source = entry.path();
				break;
			}
		}
		const int copied = source.empty() ? -1 : run("/usr/bin/ditto", {source.string(), staged.string()});
		detach(mount);
		if (copied != 0) {
			std::filesystem::remove_all(staged, error);
			return byHand(source.empty() ? "The update holds no Singularity" : "The update could not be unpacked");
		}
		if (infoValue(staged, @"CFBundleIdentifier") != identifier || infoValue(staged, @"CFBundleShortVersionString") != update.version) {
			std::filesystem::remove_all(staged, error);
			return byHand("The update is not the version Legal Machines named");
		}
		if (run("/usr/bin/codesign", {"--verify", "--deep", "--strict", staged.string()}) != 0) {
			std::filesystem::remove_all(staged, error);
			return byHand("The update's code signature does not hold");
		}
		return "";
	} catch (...) {
		return byHand("The update could not be readied");
	}
}

bool hand_over(const Update &update, bool reopen) {
	try {
		const std::filesystem::path folder = update.installer.parent_path();
		const std::filesystem::path staged = stagedApp(folder);
		const std::filesystem::path target = runningApp();
		std::error_code error;
		if (target.empty() || !std::filesystem::is_directory(staged, error)) return false;
		// One step: the new app takes the old one's place, and the old one, in
		// the new one's, waits for the next start to remove it. A process ending
		// halfway leaves either app whole where it was.
		if (renamex_np(staged.c_str(), target.c_str(), RENAME_SWAP) != 0) {
			const int code = errno;
			record_outcome(folder, Outcome{"failed", update.version,
				"Singularity " + update.version + " could not install itself: " + swapProblem(code) + "."});
			return false;
		}
		record_outcome(folder, Outcome{"installed", update.version, ""});
		if (reopen) reopenAfterExit(target);
		return true;
	} catch (...) {
		return false;
	}
}

void tidy(const std::filesystem::path &folder) {
	try {
		detach(mountPoint(folder));
		std::error_code error;
		std::filesystem::remove_all(stagedApp(folder), error);
	} catch (...) {
	}
}

} // namespace singularity::platform::update_install
