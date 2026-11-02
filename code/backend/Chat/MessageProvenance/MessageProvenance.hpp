#pragma once

#include "Chat/Types/Chat.hpp"

#include <string>

namespace singularity::chat {

/*
 * Whose a person's message in a shared Matter is, as this computer can tell.
 *
 * The computer that writes a message signs it (Platform/DeviceKey): its words,
 * author, thread, the people and agents it names, the Matter. Anyone with the
 * Matter's shared folder can still write a message under any name; a message
 * counts as its author's only when it is signed by this computer, or by a
 * computer the person here confirmed as the author's (Tool/DeviceTrust).
 *
 * A message that does not count as its author's is shown as such, and is
 * never taken as an instruction - no agent takes it up as work.
 */
enum class MessageProvenance {
	// Written on this computer.
	ThisComputer,
	// Signed by a computer the person here confirmed as the author's.
	Confirmed,
	// Signed, by a computer nobody here has confirmed as the author's yet.
	Unconfirmed,
	// Signed by a computer the person here revoked.
	Revoked,
	// Signed, but not as it reads now: changed after it was written.
	Unverified,
	// Not signed: written before Singularity signed what it records, or by a
	// version that does not.
	Unsigned,
};

const char *to_string(MessageProvenance provenance);

// Whose a signed record is, as this computer can tell: `signature` by
// `public_key` over `signed_bytes`, the record `actor_id`'s.
MessageProvenance signed_record_provenance(const std::string &actor_id, const std::string &public_key,
	const std::string &signature, const std::string &signed_bytes);

// Signs `message` (a "member_message" of `project_id`) with this computer's key.
void sign_member_message(const std::string &project_id, matters::MatterTask &message);

// Whose `message` is, as this computer can tell.
MessageProvenance member_message_provenance(const std::string &project_id, const matters::MatterTask &message);

// Whether `message` counts as its author's: written here, or signed by a
// computer the person here confirmed as theirs.
bool counts_as_authors(const std::string &project_id, const matters::MatterTask &message);

} // namespace singularity::chat
