#include "Platform/StorageProbe/StorageProbe.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <system_error>
#include <vector>

namespace singularity::platform {

std::optional<std::uint64_t> measure_uncached_read_bytes_per_second(
		const std::filesystem::path &probe_file, std::uint64_t bytes) {
	std::error_code error;
	const int writer = open(probe_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (writer < 0) return std::nullopt;
#if defined(__APPLE__)
	if (fcntl(writer, F_NOCACHE, 1) != 0) {
		close(writer);
		std::filesystem::remove(probe_file, error);
		return std::nullopt;
	}
#endif
	std::vector<char> payload(1024 * 1024);
	std::uint32_t random_state = 0x9e3779b9U;
	for (char &value : payload) {
		random_state ^= random_state << 13;
		random_state ^= random_state >> 17;
		random_state ^= random_state << 5;
		value = static_cast<char>(random_state & 0xffU);
	}
	std::uint64_t total_written = 0;
	while (total_written < bytes) {
		const ssize_t count = write(writer, payload.data(), payload.size());
		if (count <= 0) break;
		total_written += static_cast<std::uint64_t>(count);
	}
	const bool write_complete = total_written >= bytes && fsync(writer) == 0;
	close(writer);
	if (!write_complete) {
		std::filesystem::remove(probe_file, error);
		return std::nullopt;
	}

	const int descriptor = open(probe_file.c_str(), O_RDONLY);
	if (descriptor < 0) {
		std::filesystem::remove(probe_file, error);
		return std::nullopt;
	}
#if defined(__APPLE__)
	// F_NOCACHE makes this an actual storage-path measurement instead of a
	// read of the pages the write above just populated in the filesystem cache.
	if (fcntl(descriptor, F_NOCACHE, 1) != 0) {
		close(descriptor);
		std::filesystem::remove(probe_file, error);
		return std::nullopt;
	}
#elif defined(__linux__)
	if (posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED) != 0) {
		close(descriptor);
		std::filesystem::remove(probe_file, error);
		return std::nullopt;
	}
#endif

	std::vector<char> read_back(1024 * 1024);
	std::uint64_t total_read = 0;
	const auto started = std::chrono::steady_clock::now();
	while (total_read < total_written) {
		const ssize_t count = read(descriptor, read_back.data(), read_back.size());
		if (count <= 0) break;
		total_read += static_cast<std::uint64_t>(count);
	}
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	close(descriptor);
	std::filesystem::remove(probe_file, error);

	if (elapsed <= 0.0 || total_read != total_written) return std::nullopt;
	return static_cast<std::uint64_t>(static_cast<double>(total_read) / elapsed);
}

} // namespace singularity::platform
