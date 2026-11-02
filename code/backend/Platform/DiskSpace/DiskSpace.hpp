#pragma once

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace singularity::platform {

// The disk that holds a file Singularity is saving - a Project document, or Singularity's
// own record of it - has no room for it. Nothing was changed: the save
// is refused before its file is written, or the partial file it left is
// removed, and the file stays as it was. Its message is the lawyer's to read -
// no path, no error number.
class DiskFull final : public std::runtime_error {
public:
	DiskFull()
		: std::runtime_error("The disk is full, so Singularity could not save this change. Nothing was lost: free some "
			"space on this computer, then try again.")
	{
	}
};

inline bool is_disk_full_error(int value) noexcept {
#if defined(EDQUOT)
	if (value == EDQUOT) return true;
#endif
	return value == ENOSPC;
}

// Room kept free beyond the file itself: a save never takes the last of the
// disk, which the system and the lawyer's other programs need as well.
inline constexpr std::uintmax_t kFreeSpaceMargin = 1024 * 1024;

// Test seam: the free space every check below sees, instead of the disk's own.
inline std::optional<std::uintmax_t> &available_space_override_for_testing() {
	static std::optional<std::uintmax_t> bytes;
	return bytes;
}

// Asks the disk before writing: a save it cannot hold is refused whole, as a
// full disk, before any file is begun. A disk that cannot say how much room
// it has is not refused here; its write fails on its own if it must.
inline void require_free_space(const std::filesystem::path &directory, std::uintmax_t bytes) {
	std::uintmax_t available = 0;
	if (available_space_override_for_testing()) {
		available = *available_space_override_for_testing();
	} else {
		std::error_code error;
		const std::filesystem::space_info space = std::filesystem::space(directory, error);
		if (error) return;
		available = space.available;
	}
	if (available < bytes + kFreeSpaceMargin) throw DiskFull();
}

} // namespace singularity::platform
