#pragma once

#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace singularity::agents {

// An AI colleague's id, bound to the key of the computer that makes it and runs
// it (D-128): "agent_" and 24 hex of the SHA-256 of "lm-agent-id-v1", that
// computer's device key (Ed25519, base64, Platform/DeviceKey) and a nonce, one
// to a line. A card for that id can then come only from that computer: another
// would need a nonce that gives the same 96 bits from its own key. The id is as
// long as a random one was, so nothing that keeps ids changes.
inline std::string bound_agent_id(const std::string &device_key, const std::string &nonce) {
	return "agent_" + platform::sha256_hex_digest("lm-agent-id-v1\n" + device_key + "\n" + nonce).substr(0, 24);
}

// A new id bound to `device_key`, and its nonce (16 random bytes, hex). With no
// device key, a random id and no nonce, as an older agent has.
inline std::pair<std::string, std::string> new_bound_agent_id(const std::string &device_key) {
	if (device_key.empty() == true) return {"agent_" + platform::secure_random_hex(12), std::string()};
	const std::string nonce = platform::secure_random_hex(16);
	return {bound_agent_id(device_key, nonce), nonce};
}

// What the computer that runs a colleague signs on the colleague's card, so the
// Matter's other computers know the card is that computer's (D-128): the
// colleague, the computer, its key, the id's nonce, the lawyer answerable for
// it and the card's counter, one to a line.
inline std::string agent_card_signed_text(const std::string &agent_id, const std::string &host_device_id,
		const std::string &host_device_key, const std::string &id_nonce, const std::string &responsible_actor_id,
		std::int64_t counter) {
	return "lm-agent-card-v1\n" + agent_id + "\n" + host_device_id + "\n" + host_device_key + "\n" + id_nonce + "\n"
		+ responsible_actor_id + "\n" + std::to_string(counter);
}

} // namespace singularity::agents
