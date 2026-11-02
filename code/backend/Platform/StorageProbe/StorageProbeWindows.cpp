#include "Platform/StorageProbe/StorageProbe.hpp"

#include <windows.h>

#include <chrono>
#include <system_error>

namespace singularity::platform {

std::optional<std::uint64_t> measure_uncached_read_bytes_per_second(
		const std::filesystem::path &probe_file, std::uint64_t bytes) {
	// FILE_FLAG_NO_BUFFERING needs sector-aligned buffers and transfer
	// sizes; VirtualAlloc returns page-aligned memory and 1 MiB is a
	// multiple of every sector size Windows supports.
	constexpr DWORD kChunk = 1024 * 1024;
	const std::uint64_t total = (bytes + kChunk - 1) / kChunk * kChunk;
	void *buffer = VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (buffer == nullptr) return std::nullopt;
	auto *bytes_view = static_cast<unsigned char *>(buffer);
	std::uint32_t random_state = 0x9e3779b9U;
	for (DWORD index = 0; index < kChunk; ++index) {
		random_state ^= random_state << 13;
		random_state ^= random_state >> 17;
		random_state ^= random_state << 5;
		bytes_view[index] = static_cast<unsigned char>(random_state & 0xffU);
	}
	std::error_code error;
	auto finish = [&](std::optional<std::uint64_t> result) {
		VirtualFree(buffer, 0, MEM_RELEASE);
		std::filesystem::remove(probe_file, error);
		return result;
	};

	const HANDLE writer = CreateFileW(probe_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (writer == INVALID_HANDLE_VALUE) return finish(std::nullopt);
	std::uint64_t written = 0;
	while (written < total) {
		DWORD count = 0;
		if (!WriteFile(writer, buffer, kChunk, &count, nullptr) || count != kChunk) break;
		written += count;
	}
	const bool write_complete = written == total && FlushFileBuffers(writer) != FALSE;
	CloseHandle(writer);
	if (!write_complete) return finish(std::nullopt);

	// Unbuffered reads go straight to the device: the pages written above
	// are not served from the file cache.
	const HANDLE reader = CreateFileW(probe_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (reader == INVALID_HANDLE_VALUE) return finish(std::nullopt);
	std::uint64_t read_total = 0;
	const auto started = std::chrono::steady_clock::now();
	while (read_total < total) {
		DWORD count = 0;
		if (!ReadFile(reader, buffer, kChunk, &count, nullptr) || count != kChunk) break;
		read_total += count;
	}
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	CloseHandle(reader);
	if (elapsed <= 0.0 || read_total != total) return finish(std::nullopt);
	return finish(static_cast<std::uint64_t>(static_cast<double>(read_total) / elapsed));
}

} // namespace singularity::platform
