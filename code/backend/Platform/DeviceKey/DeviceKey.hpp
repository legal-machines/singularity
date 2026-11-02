#pragma once

#include <optional>
#include <string>

namespace singularity::platform {

/*
 * This computer's own signing key (Ed25519, RFC 8032): what lets a colleague's
 * Singularity tell that a record in a shared Matter's `.lm` was written by this
 * computer and not changed since.
 *
 * Made once, the first time it is needed, kept in this computer's own records
 * (`Device Key/device.key`, readable by its owner only, and never where another
 * computer reads it: Platform/AppSettings computer_records_directory). It is
 * never written to a Matter, a log or a request. Its public half is shown to
 * people as a short code they can compare between computers (device_key_code()).
 *
 * A signature proves which key wrote a record. Whose computer that key is - a
 * colleague's, or someone's who copied their name - is a person's decision
 * (Tool/DeviceTrust), never something the record itself can claim.
 */

// The public half of this computer's key, base64 (32 bytes).
std::string device_public_key();

// Signs `message` with this computer's key: base64 (64 bytes).
std::string sign_with_device_key(const std::string &message);

// Whether `signature` (base64) is `public_key`'s (base64) signature of
// `message`. False for anything malformed.
bool verify_device_signature(const std::string &public_key, const std::string &message, const std::string &signature);

// The code people compare to recognise a key: twelve characters from its
// SHA-256, in three groups ("7F3A-90C2-1BD4"). Empty for a malformed key.
std::string device_key_code(const std::string &public_key);

// Strict base64 (RFC 4648, padded). Exposed for the records that carry keys
// and signatures.
std::string base64_encode(const std::string &bytes);
std::optional<std::string> base64_decode(const std::string &text);

} // namespace singularity::platform
