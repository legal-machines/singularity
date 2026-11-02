#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#if defined(__linux__)
#include <fcntl.h>
#endif

namespace singularity::platform {

// The OS's own identity of a file object, which survives a rename, a move
// within one volume and an edit of its content in place - what lets a
// Document keep its identity through them. A field this platform or file
// system cannot supply stays unset rather than fabricated (see
// DocumentRegistry's no-identity fallback for what that means).
struct FileIdentity {
	std::optional<std::uint64_t> device_id;
	std::optional<std::uint64_t> file_id;
	// The file object's creation time where the OS keeps one, written as a
	// token: it tells a genuinely new file apart from an old one even when a
	// file id is reused.
	std::optional<std::string> creation_time_token;
	long long size_bytes = 0;
};

// False only when the file itself cannot be inspected (it vanished, or this
// account may not read its attributes).
inline bool read_file_identity(const std::filesystem::path &path, FileIdentity &result) {
#if defined(_WIN32)
	// Attribute-only access is never refused by, and never blocks, whoever
	// else has the file open (Word, a sync client).
	const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	FILE_STANDARD_INFO standard{};
	FILE_ID_INFO id{};
	FILE_BASIC_INFO basic{};
	const bool have_size = GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) != 0;
	const bool have_id = GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) != 0;
	const bool have_times = GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) != 0;
	CloseHandle(file);
	if (!have_size) return false;
	result.size_bytes = static_cast<long long>(standard.EndOfFile.QuadPart);
	// FAT and exFAT keep no stable file id: nothing is claimed for them.
	if (have_id) {
		std::uint64_t low = 0;
		std::uint64_t high = 0;
		std::memcpy(&low, id.FileId.Identifier, sizeof(low));
		std::memcpy(&high, id.FileId.Identifier + sizeof(low), sizeof(high));
		result.device_id = static_cast<std::uint64_t>(id.VolumeSerialNumber);
		// An NTFS file id is the low 64 bits exactly; a 128-bit ReFS id is
		// folded, and is still compared together with its volume and
		// creation time.
		result.file_id = high == 0 ? low : low ^ (high * 0x9E3779B97F4A7C15ull);
	}
	if (have_times) result.creation_time_token = std::to_string(basic.CreationTime.QuadPart);
#else
	struct stat file_stat {};
	if (::stat(path.c_str(), &file_stat) != 0) return false;
	result.device_id = static_cast<std::uint64_t>(file_stat.st_dev);
	result.file_id = static_cast<std::uint64_t>(file_stat.st_ino);
	result.size_bytes = static_cast<long long>(file_stat.st_size);
#if defined(__APPLE__)
	// st_birthtimespec is the file object's real creation time - immutable
	// across rename/move/content edit, and different for a genuinely new file
	// even when the OS reuses a freed inode number for it (real, documented
	// APFS behavior, not hypothetical).
	result.creation_time_token =
		std::to_string(file_stat.st_birthtimespec.tv_sec) + "." + std::to_string(file_stat.st_birthtimespec.tv_nsec);
#elif defined(__linux__)
	// The same on Linux, through statx (kernel 4.11): ext4, XFS and Btrfs keep
	// it, and ext4 hands a freed inode number to the very next file. A file
	// system that keeps no creation time leaves it unset.
	struct statx about {};
	if (::statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &about) == 0 && (about.stx_mask & STATX_BTIME) != 0)
		result.creation_time_token = std::to_string(about.stx_btime.tv_sec) + "." + std::to_string(about.stx_btime.tv_nsec);
#endif
#endif
	return true;
}

} // namespace singularity::platform
