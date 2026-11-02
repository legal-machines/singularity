#include "Matter/CoordinationTransport/CoordinationTransport.hpp"

#include <monocypher.h>

namespace singularity::matter::coordination::transport {

std::string envelope_id(const SealedBytes& bytes) {
	std::uint8_t hash[32];
	crypto_blake2b(hash, sizeof hash, bytes.data(), bytes.size());
	static const char* const digits = "0123456789abcdef";
	std::string id;
	id.reserve(sizeof hash * 2);
	for (const std::uint8_t byte : hash) {
		id += digits[byte >> 4];
		id += digits[byte & 0x0F];
	}
	return id;
}

} // namespace singularity::matter::coordination::transport
