#include "Platform/TrashFile/TrashFile.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace singularity::platform {
namespace {

std::mutex override_mutex;
std::optional<TrashResult> result_override;

// One trash directory of the freedesktop.org Trash specification, and how the
// Path key of its .trashinfo names the original: absolute in the home trash,
// relative to the volume's top directory in a volume's own trash.
struct TrashDirectory {
	std::filesystem::path directory;
	std::filesystem::path relative_to;
};

enum class MoveOutcome { kMoved, kOtherVolume, kFailed };

// Path is "escaped in the same way as URLs" (RFC 2396): everything but the
// unreserved characters and '/' is written as %XX of its UTF-8 bytes.
std::string escaped_path(const std::string &path) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string escaped;
	escaped.reserve(path.size());
	for (const unsigned char character : path) {
		const bool isUnreserved = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.'
			|| character == '!' || character == '~' || character == '*' || character == '\'' || character == '('
			|| character == ')' || character == '/';
		if (isUnreserved) {
			escaped.push_back(static_cast<char>(character));
		} else {
			escaped.push_back('%');
			escaped.push_back(kHex[character >> 4]);
			escaped.push_back(kHex[character & 0x0F]);
		}
	}
	return escaped;
}

std::string deletion_date() {
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	char text[32];
	return std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local) > 0 ? std::string(text) : std::string();
}

// $XDG_DATA_HOME, or its default ~/.local/share; a relative value is ignored,
// as the XDG Base Directory specification says.
std::optional<std::filesystem::path> data_home() {
	if (const char *value = std::getenv("XDG_DATA_HOME"); value != nullptr && value[0] == '/')
		return std::filesystem::path(value);
	if (const char *value = std::getenv("HOME"); value != nullptr && value[0] == '/')
		return std::filesystem::path(value) / ".local" / "share";
	if (const passwd *account = getpwuid(getuid()); account != nullptr && account->pw_dir != nullptr
			&& account->pw_dir[0] == '/')
		return std::filesystem::path(account->pw_dir) / ".local" / "share";
	return std::nullopt;
}

// A folder of the user's own, made 0700 where missing (as the specification
// asks of a trash it needs). `allow_symlink` is for the home trash only: a
// volume's trash must never be a link someone else could point elsewhere.
bool own_directory(const std::filesystem::path &path, bool allow_symlink) {
	if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
	struct stat metadata {};
	if ((allow_symlink ? ::stat(path.c_str(), &metadata) : ::lstat(path.c_str(), &metadata)) != 0) return false;
	return S_ISDIR(metadata.st_mode) && metadata.st_uid == getuid();
}

// Every missing folder of `path` made 0700, as XDG asks of $XDG_DATA_HOME.
bool make_directories(const std::filesystem::path &path) {
	std::filesystem::path current;
	for (const auto &part : path) {
		current /= part;
		if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) return false;
	}
	struct stat metadata {};
	return ::stat(path.c_str(), &metadata) == 0 && S_ISDIR(metadata.st_mode);
}

std::optional<TrashDirectory> home_trash() {
	const auto home = data_home();
	if (home.has_value() == false || make_directories(*home) == false) return std::nullopt;
	const auto trash = *home / "Trash";
	if (own_directory(trash, true) == false || own_directory(trash / "files", true) == false
			|| own_directory(trash / "info", true) == false)
		return std::nullopt;
	return TrashDirectory{trash, {}};
}

// The volume's top directory: the highest folder above `folder` still on the
// same device.
std::filesystem::path volume_top(std::filesystem::path folder, dev_t device) {
	for (;;) {
		const auto parent = folder.parent_path();
		if (parent == folder) return folder;
		struct stat metadata {};
		if (::stat(parent.c_str(), &metadata) != 0 || metadata.st_dev != device) return folder;
		folder = parent;
	}
}

// (1) $topdir/.Trash/$uid, when an administrator made $topdir/.Trash: a real
// folder, not a link, with the sticky bit. Otherwise it must not be used.
std::optional<TrashDirectory> administrator_trash(const std::filesystem::path &top) {
	const auto shared = top / ".Trash";
	struct stat metadata {};
	if (::lstat(shared.c_str(), &metadata) != 0 || S_ISDIR(metadata.st_mode) == false
			|| (metadata.st_mode & S_ISVTX) == 0)
		return std::nullopt;
	const auto trash = shared / std::to_string(getuid());
	if (own_directory(trash, false) == false || own_directory(trash / "files", false) == false
			|| own_directory(trash / "info", false) == false)
		return std::nullopt;
	return TrashDirectory{trash, top};
}

// (2) $topdir/.Trash-$uid, made by the user's own programs.
std::optional<TrashDirectory> user_trash(const std::filesystem::path &top) {
	const auto trash = top / (".Trash-" + std::to_string(getuid()));
	if (own_directory(trash, false) == false || own_directory(trash / "files", false) == false
			|| own_directory(trash / "info", false) == false)
		return std::nullopt;
	return TrashDirectory{trash, top};
}

// A rename that never replaces what is at the destination.
int rename_without_replacing(const std::filesystem::path &from, const std::filesystem::path &to) {
#if defined(RENAME_NOREPLACE)
	if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return 0;
	if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
	// A file system without RENAME_NOREPLACE: the name was found free just
	// before, and its .trashinfo, made exclusively, reserves it.
	struct stat existing {};
	if (::lstat(to.c_str(), &existing) == 0) {
		errno = EEXIST;
		return -1;
	}
	return ::rename(from.c_str(), to.c_str());
}

// "Name.txt", then "Name 2.txt", "Name 3.txt"... as Files and Finder name a
// second copy.
std::string candidate_name(const std::filesystem::path &original, int attempt) {
	if (attempt == 1) return original.filename().string();
	return original.stem().string() + " " + std::to_string(attempt) + original.extension().string();
}

// The specification's order: the .trashinfo is created first, exclusively,
// which reserves the name; then the file is moved in. A move that fails takes
// its .trashinfo back out, and the file stays where it was.
MoveOutcome move_into(const TrashDirectory &trash, const std::filesystem::path &original) {
	const std::string original_path =
		trash.relative_to.empty() ? original.string() : original.lexically_relative(trash.relative_to).string();
	if (original_path.empty() == true) return MoveOutcome::kFailed;
	const std::string contents =
		"[Trash Info]\nPath=" + escaped_path(original_path) + "\nDeletionDate=" + deletion_date() + "\n";
	for (int attempt = 1; attempt <= 10000; ++attempt) {
		const std::string name = candidate_name(original, attempt);
		const auto info = trash.directory / "info" / (name + ".trashinfo");
		const auto destination = trash.directory / "files" / name;
		const int descriptor = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (descriptor < 0) {
			if (errno == EEXIST) continue;
			return MoveOutcome::kFailed;
		}
		const ssize_t written = ::write(descriptor, contents.data(), contents.size());
		const bool isRecorded = written == static_cast<ssize_t>(contents.size()) && ::close(descriptor) == 0;
		if (isRecorded == false) {
			::unlink(info.c_str());
			return MoveOutcome::kFailed;
		}
		if (rename_without_replacing(original, destination) == 0) return MoveOutcome::kMoved;
		const int error = errno;
		::unlink(info.c_str());
		if (error == EEXIST) continue;
		return error == EXDEV ? MoveOutcome::kOtherVolume : MoveOutcome::kFailed;
	}
	return MoveOutcome::kFailed;
}

TrashResult trash(const std::string &path) {
	if (path.empty() == true) return TrashResult::Failed;
	std::error_code error;
	const std::filesystem::path given = std::filesystem::absolute(std::filesystem::path(path), error).lexically_normal();
	if (error || given.has_filename() == false) return TrashResult::Failed;
	struct stat item {};
	if (::lstat(given.c_str(), &item) != 0) return TrashResult::Failed;
	// The folder the file is in, with its links resolved; the file itself is
	// trashed as it is, a link as a link.
	const std::filesystem::path folder = std::filesystem::canonical(given.parent_path(), error);
	if (error) return TrashResult::Failed;
	const std::filesystem::path original = folder / given.filename();

	// The home trash takes what is on its own volume.
	if (const auto home = home_trash(); home.has_value() == true) {
		struct stat trashMetadata {};
		if (::stat(home->directory.c_str(), &trashMetadata) == 0 && trashMetadata.st_dev == item.st_dev) {
			const MoveOutcome outcome = move_into(*home, original);
			if (outcome == MoveOutcome::kMoved) return TrashResult::Succeeded;
			if (outcome == MoveOutcome::kFailed) return TrashResult::Failed;
		}
	}

	// Another volume has its own trash. A file is never copied across volumes,
	// so a volume with no usable trash refuses: nothing is deleted.
	// $topdir/.Trash-$uid is made only when $topdir/.Trash cannot be used.
	const std::filesystem::path top = volume_top(folder, item.st_dev);
	auto candidate = administrator_trash(top);
	if (candidate.has_value() == false) candidate = user_trash(top);
	if (candidate.has_value() == false) return TrashResult::Failed;
	return move_into(*candidate, original) == MoveOutcome::kMoved ? TrashResult::Succeeded : TrashResult::Failed;
}

} // namespace

const char *trash_name() { return "Trash"; }

// The freedesktop.org Trash specification, which GNOME Files, Dolphin and the
// other Linux file managers read: the file can be put back from the Trash
// there, exactly as on macOS and Windows.
TrashResult move_to_trash(const std::string &path) {
	{
		const std::lock_guard<std::mutex> lock(override_mutex);
		if (result_override) return *result_override;
	}
	return trash(path);
}

void set_trash_result_override_for_testing(std::optional<TrashResult> value) {
	const std::lock_guard<std::mutex> lock(override_mutex);
	result_override = value;
}

} // namespace singularity::platform
