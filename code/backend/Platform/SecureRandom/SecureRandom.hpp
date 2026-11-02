#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#include <sys/types.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#error "Singularity takes its random bytes from the system: name this system's source here."
#endif

namespace singularity::platform {

// Bytes from the operating system's cryptographic random source, never from a
// seeded generator. A std::mt19937_64 seeded with one std::random_device()
// draw has only 2^32 starting states, however many digits it is asked for:
// identifiers drawn that way meet each other after some tens of thousands of
// draws - an operation file of a Project replacing another, one chat turn
// overwriting another. Every identifier that names something Singularity
// keeps, and every token that grants something, takes its bytes from here.
inline void fill_secure_random(std::uint8_t *bytes, std::size_t size) {
#if defined(__APPLE__)
	arc4random_buf(bytes, size);
#elif defined(__linux__)
	std::size_t filled = 0;
	while (filled < size) {
		const ssize_t got = getrandom(bytes + filled, size - filled, 0);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) throw std::runtime_error("The system's secure random source is unavailable.");
		filled += static_cast<std::size_t>(got);
	}
#else
	// Windows: the system's preferred generator, the one CNG gives every
	// process (bcrypt is linked into each executable: deploy/backend). Never
	// std::random_device, which the standard lets be a seeded generator, as
	// some MinGW libraries made it.
	std::size_t filled = 0;
	while (filled < size) {
		const ULONG piece = static_cast<ULONG>(size - filled < 0x40000000u ? size - filled : 0x40000000u);
		if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes + filled, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
			throw std::runtime_error("The system's secure random source is unavailable.");
		filled += piece;
	}
#endif
}

// `bytes` random bytes written as twice as many lowercase hexadecimal digits.
inline std::string secure_random_hex(std::size_t bytes) {
	std::vector<std::uint8_t> buffer(bytes, 0);
	fill_secure_random(buffer.data(), buffer.size());
	static const char *digits = "0123456789abcdef";
	std::string result;
	result.reserve(bytes * 2);
	for (const std::uint8_t byte : buffer) {
		result += digits[byte >> 4];
		result += digits[byte & 0x0F];
	}
	return result;
}

} // namespace singularity::platform
