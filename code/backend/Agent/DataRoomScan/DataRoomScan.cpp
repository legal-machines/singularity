#include "Agent/DataRoomScan/DataRoomScan.hpp"

#include "Platform/FileIdentity/FileIdentity.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace singularity::agents {
namespace {

// The staging names Singularity's own staged writes use (Platform/AtomicFile):
// a document mid-replacement is never a document of the data room.
bool is_staged_name(const std::string &name) {
	return name.find(".lm-tmp-") != std::string::npos || name.find(".singularity-workspace-tmp-") != std::string::npos
		|| name.find(".singularity-workspace-patch-") != std::string::npos;
}

struct Listed {
	std::filesystem::path path;
	std::string relative_path;
	bool is_link = false;
	std::string error;
};

// What the file system says without reading the file: size, modification
// time to the nanosecond, and the file's own id, so a moved file can be told
// from a new one. False when even that cannot be asked.
bool describe(const std::filesystem::path &path, FileFingerprint &fingerprint) {
#if !defined(_WIN32)
	struct stat facts {};
	if (::lstat(path.c_str(), &facts) != 0) return false;
	fingerprint.size = static_cast<std::uintmax_t>(facts.st_size);
#if defined(__APPLE__)
	fingerprint.modified_ns = static_cast<long long>(facts.st_mtimespec.tv_sec) * 1000000000LL + facts.st_mtimespec.tv_nsec;
#else
	fingerprint.modified_ns = static_cast<long long>(facts.st_mtim.tv_sec) * 1000000000LL + facts.st_mtim.tv_nsec;
#endif
	fingerprint.file_id = static_cast<unsigned long long>(facts.st_ino);
	return true;
#else
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error) return false;
	const auto time = std::filesystem::last_write_time(path, error);
	if (error) return false;
	fingerprint.size = size;
	fingerprint.modified_ns = static_cast<long long>(time.time_since_epoch().count());
	// The file's own id through Platform/FileIdentity, as st_ino above: a
	// rename within the folder keeps it. A file system that keeps no stable id
	// (FAT, exFAT) gives none, and 0 leaves the file to be read again.
	fingerprint.file_id = 0;
	platform::FileIdentity identity;
	if (platform::read_file_identity(path, identity) == true && identity.file_id.has_value() == true)
		fingerprint.file_id = static_cast<unsigned long long>(*identity.file_id);
	return true;
#endif
}

std::string relative_of(const std::filesystem::path &root, const std::filesystem::path &path) {
	std::error_code error;
	std::filesystem::path relative = std::filesystem::relative(path, root, error);
	if (error || relative.empty() == true) relative = path.filename();
	return relative.generic_string();
}

// Walks the folder without following links. A folder that cannot be opened is
// listed as one entry with its error, so it is said, not skipped.
void list_folder(const std::filesystem::path &root, const std::filesystem::path &folder, std::vector<Listed> &listed,
	std::size_t mostFiles, bool &isTooMany) {
	std::error_code error;
	std::filesystem::directory_iterator iterator(folder, error);
	if (error) {
		listed.push_back({folder, relative_of(root, folder) + "/", false,
			"Singularity cannot open this folder, so the documents in it were not read."});
		return;
	}
	std::vector<std::filesystem::directory_entry> entries;
	for (const std::filesystem::directory_entry &entry : iterator) entries.push_back(entry);
	std::sort(entries.begin(), entries.end(), [](const std::filesystem::directory_entry &left,
		const std::filesystem::directory_entry &right) { return left.path().filename() < right.path().filename(); });
	for (const std::filesystem::directory_entry &entry : entries) {
		if (isTooMany == true) return;
		const std::string name = entry.path().filename().string();
		if (is_ignored_file_name(name) == true) continue;
		std::error_code statusError;
		const std::filesystem::file_status status = entry.symlink_status(statusError);
		if (statusError) {
			listed.push_back({entry.path(), relative_of(root, entry.path()), false,
				"Singularity cannot read this file's details, so it was not read."});
			continue;
		}
		if (std::filesystem::is_symlink(status) == true) {
			listed.push_back({entry.path(), relative_of(root, entry.path()), true,
				"A link to a file elsewhere: the watch reads only what is inside the chosen folder."});
			continue;
		}
		if (std::filesystem::is_directory(status) == true) {
			list_folder(root, entry.path(), listed, mostFiles, isTooMany);
			continue;
		}
		if (std::filesystem::is_regular_file(status) == false) continue;
		if (listed.size() >= mostFiles) {
			isTooMany = true;
			return;
		}
		listed.push_back({entry.path(), relative_of(root, entry.path()), false, ""});
	}
}

} // namespace

bool is_ignored_file_name(const std::string &name) {
	if (name.empty() == true) return true;
	if (name[0] == '.') return true;
	if (name.rfind("~$", 0) == 0) return true;
	if (name == "Thumbs.db" || name == "desktop.ini" || name == "Icon\r") return true;
	return is_staged_name(name);
}

const char *to_string(FileChangeKind kind) {
	switch (kind) {
		case FileChangeKind::Added: return "added";
		case FileChangeKind::Changed: return "changed";
		case FileChangeKind::Removed: return "removed";
		case FileChangeKind::Moved: return "moved";
		case FileChangeKind::Unreadable: return "unreadable";
	}
	return "changed";
}

ScanResult scan_data_room(const std::filesystem::path &root, const DataRoomInventory &previous, const ScanLimits &limits,
	const ScanProgress &progress, const std::atomic<bool> *cancelled) {
	ScanResult result;
	std::error_code error;
	if (std::filesystem::exists(root, error) == false) {
		result.status = ScanStatus::RootMissing;
		result.root_error = "The watched folder is not where it was: it may have been moved, renamed or disconnected.";
		return result;
	}
	if (std::filesystem::is_directory(root, error) == false) {
		result.status = ScanStatus::RootMissing;
		result.root_error = "The watched place is no longer a folder.";
		return result;
	}
	std::filesystem::directory_iterator probe(root, error);
	if (error) {
		result.status = ScanStatus::RootUnreadable;
		result.root_error = "Singularity is not allowed to open the watched folder.";
		return result;
	}

	std::vector<Listed> listed;
	bool isTooMany = false;
	list_folder(root, root, listed, limits.most_files, isTooMany);
	if (isTooMany == true) {
		result.status = ScanStatus::TooManyFiles;
		result.root_error = "The folder holds more than " + std::to_string(limits.most_files)
			+ " files: choose the data room's own folder, not a folder above it.";
		return result;
	}

	std::map<std::string, const FileFingerprint *> earlier;
	std::map<unsigned long long, const FileFingerprint *> earlierById;
	for (const FileFingerprint &fingerprint : previous.files) {
		earlier[fingerprint.relative_path] = &fingerprint;
		if (fingerprint.file_id != 0) earlierById[fingerprint.file_id] = &fingerprint;
	}

	const std::size_t total = listed.size();
	std::size_t done = 0;
	for (const Listed &item : listed) {
		if (cancelled != nullptr && cancelled->load() == true) {
			result.status = ScanStatus::Cancelled;
			return result;
		}
		FileFingerprint fingerprint;
		fingerprint.relative_path = item.relative_path;
		if (item.error.empty() == false) {
			fingerprint.read_error = item.error;
			result.inventory.files.push_back(fingerprint);
			++done;
			if (progress) progress(done, total);
			continue;
		}
		if (describe(item.path, fingerprint) == false) {
			fingerprint.read_error = "Singularity cannot read this file's details, so it was not read.";
			result.inventory.files.push_back(fingerprint);
			++done;
			if (progress) progress(done, total);
			continue;
		}
		// Unchanged by everything the file system knows - at its old place, or
		// renamed within the folder, which keeps a file's id, size and time -
		// its earlier digest stands, and the file is not read.
		const auto found = earlier.find(item.relative_path);
		const FileFingerprint *same = found != earlier.end() ? found->second : nullptr;
		if (same == nullptr && fingerprint.file_id != 0) {
			const auto byId = earlierById.find(fingerprint.file_id);
			if (byId != earlierById.end()) same = byId->second;
		}
		if (same != nullptr && same->sha256.empty() == false && same->size == fingerprint.size
				&& same->modified_ns == fingerprint.modified_ns && same->file_id == fingerprint.file_id) {
			fingerprint.sha256 = same->sha256;
			result.inventory.files.push_back(fingerprint);
			++done;
			if (progress) progress(done, total);
			continue;
		}
		try {
			fingerprint.sha256 = platform::sha256_hex_digest_of_file(item.path);
			++result.read;
		} catch (const std::exception &) {
			fingerprint.read_error = "Singularity cannot read this file: it may be locked by another program or not shared with you.";
		}
		// A file still being written reads differently from one moment to the
		// next: it is left as it was this time and read at the next check.
		FileFingerprint after;
		if (fingerprint.read_error.empty() == true && (describe(item.path, after) == false
				|| after.size != fingerprint.size || after.modified_ns != fingerprint.modified_ns)) {
			result.unsettled.push_back(item.relative_path);
			if (found != earlier.end()) result.inventory.files.push_back(*found->second);
			++done;
			if (progress) progress(done, total);
			continue;
		}
		result.inventory.files.push_back(fingerprint);
		++done;
		if (progress) progress(done, total);
	}
	std::sort(result.inventory.files.begin(), result.inventory.files.end(),
		[](const FileFingerprint &left, const FileFingerprint &right) { return left.relative_path < right.relative_path; });
	return result;
}

std::vector<FileChange> compare_inventories(const DataRoomInventory &previous, const DataRoomInventory &current) {
	std::map<std::string, const FileFingerprint *> before;
	std::map<std::string, const FileFingerprint *> now;
	for (const FileFingerprint &fingerprint : previous.files) before[fingerprint.relative_path] = &fingerprint;
	for (const FileFingerprint &fingerprint : current.files) now[fingerprint.relative_path] = &fingerprint;

	std::vector<FileChange> changes;
	std::vector<const FileFingerprint *> appeared;
	std::vector<const FileFingerprint *> left;
	for (const auto &[path, fingerprint] : now) {
		const auto found = before.find(path);
		if (found == before.end()) {
			appeared.push_back(fingerprint);
			continue;
		}
		const FileFingerprint &earlier = *found->second;
		if (fingerprint->read_error.empty() == false) {
			// Still unreadable is not news; newly unreadable is.
			if (earlier.read_error.empty() == true)
				changes.push_back({FileChangeKind::Unreadable, path, "", earlier.sha256, "", fingerprint->read_error});
			continue;
		}
		if (earlier.read_error.empty() == false) {
			changes.push_back({FileChangeKind::Changed, path, "", "", fingerprint->sha256, ""});
			continue;
		}
		if (earlier.sha256 != fingerprint->sha256)
			changes.push_back({FileChangeKind::Changed, path, "", earlier.sha256, fingerprint->sha256, ""});
	}
	for (const auto &[path, fingerprint] : before)
		if (now.count(path) == 0) left.push_back(fingerprint);

	// A digest that left one place and appeared at another is the same
	// document moved; the same file id is preferred when several match.
	std::vector<bool> isLeftUsed(left.size(), false);
	for (const FileFingerprint *arrived : appeared) {
		if (arrived->read_error.empty() == false) {
			changes.push_back({FileChangeKind::Unreadable, arrived->relative_path, "", "", "", arrived->read_error});
			continue;
		}
		std::size_t match = left.size();
		for (std::size_t index = 0; index < left.size(); ++index) {
			if (isLeftUsed[index] == true || left[index]->sha256.empty() == true || left[index]->sha256 != arrived->sha256) continue;
			if (match == left.size()) match = index;
			if (arrived->file_id != 0 && left[index]->file_id == arrived->file_id) {
				match = index;
				break;
			}
		}
		if (match < left.size()) {
			isLeftUsed[match] = true;
			changes.push_back({FileChangeKind::Moved, arrived->relative_path, left[match]->relative_path, left[match]->sha256,
				arrived->sha256, ""});
			continue;
		}
		changes.push_back({FileChangeKind::Added, arrived->relative_path, "", "", arrived->sha256, ""});
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (isLeftUsed[index] == true) continue;
		changes.push_back({FileChangeKind::Removed, left[index]->relative_path, "", left[index]->sha256, "", ""});
	}
	std::sort(changes.begin(), changes.end(),
		[](const FileChange &a, const FileChange &b) { return a.relative_path < b.relative_path; });
	return changes;
}

} // namespace singularity::agents
