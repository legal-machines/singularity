#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace singularity::agents {

// What a data room watch knows of one file at one check: where it is in the
// folder, the facts the file system gives for free, and its content digest.
// The digest decides whether the file changed; size, time and file id only
// decide whether it has to be read again to know.
struct FileFingerprint {
	std::string relative_path;
	std::uintmax_t size = 0;
	long long modified_ns = 0;
	unsigned long long file_id = 0;
	// Empty when the file could not be read; read_error then says why.
	std::string sha256;
	std::string read_error;
};

// Every file of a watched folder at one check, ordered by relative path.
struct DataRoomInventory {
	std::vector<FileFingerprint> files;
};

enum class FileChangeKind {
	Added,
	Changed,
	Removed,
	Moved,
	Unreadable,
};

struct FileChange {
	FileChangeKind kind = FileChangeKind::Added;
	std::string relative_path;
	// Moved: where the same content stood before.
	std::string previous_path;
	// Empty when there was no readable earlier version (a file that could not
	// be read before and can be now is Changed with no previous digest).
	std::string previous_sha256;
	std::string current_sha256;
	// Unreadable: why, in words.
	std::string note;
};

struct ScanLimits {
	std::size_t most_files = 5000;
};

enum class ScanStatus {
	Complete,
	RootMissing,
	RootUnreadable,
	TooManyFiles,
	Cancelled,
};

struct ScanResult {
	ScanStatus status = ScanStatus::Complete;
	DataRoomInventory inventory;
	// Files read in full this time; the rest kept the digest they had.
	std::size_t read = 0;
	// Files changing while they were read: left as they were, read next time.
	std::vector<std::string> unsettled;
	std::string root_error;
};

// Called after each file with how many are done of all there are.
using ScanProgress = std::function<void(std::size_t done, std::size_t total)>;

// Lists `root` (never following a link out of it, leaving out hidden, system
// and Singularity's own staging files) and fingerprints each file, reading a
// file in full only when the file system says it may have changed since
// `previous`. Never writes anything.
ScanResult scan_data_room(const std::filesystem::path &root, const DataRoomInventory &previous, const ScanLimits &limits,
	const ScanProgress &progress, const std::atomic<bool> *cancelled);

// What happened between two inventories, by content: a file whose digest is
// unchanged did not change whatever its time says; a digest that left one
// path and appeared at another is a move.
std::vector<FileChange> compare_inventories(const DataRoomInventory &previous, const DataRoomInventory &current);

const char *to_string(FileChangeKind kind);

// Names a data room never means: hidden files, Office lock files, system
// clutter and Singularity's own staged writes.
bool is_ignored_file_name(const std::string &name);

} // namespace singularity::agents
