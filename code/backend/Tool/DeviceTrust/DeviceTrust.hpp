#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace singularity::tools {

/*
 * Which colleagues' computers the person at this computer has confirmed: their
 * decision, kept in this computer's own records (`Trusted Devices.json`,
 * readable by its owner only; Platform/AppSettings computer_records_directory),
 * never in a Matter.
 *
 * A record in a shared Matter's `.lm` signed by a computer's key
 * (Platform/DeviceKey) proves which key wrote it. That the key is a colleague's
 * computer - and not someone's who copied their name into the folder - is what
 * a person confirms here, comparing the key's short code with the colleague.
 * A key confirmed for one person is theirs in every Matter; revoked, nothing it
 * signed counts as theirs again.
 */
enum class DeviceTrustState {
	// This computer's own key.
	ThisComputer,
	// Confirmed by the person here as this colleague's computer.
	Confirmed,
	// Seen, never confirmed: what it signed is shown as not confirmed.
	Unconfirmed,
	// Revoked by the person here: nothing it signed counts as theirs.
	Revoked,
};

const char *to_string(DeviceTrustState state);

// The state of `public_key` (base64) as `actor_id`'s computer.
DeviceTrustState device_trust(const std::string &actor_id, const std::string &public_key);

// The person here confirms, or revokes, `public_key` as `actor_id`'s computer.
// Revoking is final for that key: it is never confirmed again.
void confirm_device(const std::string &actor_id, const std::string &public_key, const std::string &decided_by);
void revoke_device(const std::string &actor_id, const std::string &public_key, const std::string &decided_by);

// Whether a record signed by `public_key` (base64) counts as `actor_id`'s:
// this computer's own key for its own person, or a key the person here
// confirmed and has not revoked.
bool counts_as_theirs(const std::string &actor_id, const std::string &public_key);

// Whether the person here revoked `public_key` (base64) as anyone's computer.
bool is_refused_key(const std::string &public_key);

// How many decisions the person here has made in this session: what a
// colleague's computer signed earlier may count as theirs once it changes.
std::uint64_t device_trust_decisions();

} // namespace singularity::tools
