#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::node::openpgp {

/*
 * An OpenPGP message sealed on this computer to its recipients' public keys
 * (RFC 9580), so that only a holder of one of their secret keys opens it. A
 * design partner's work log (D-257) is sealed so to Legal Machines' keys: the
 * mail server that carries it reads none of it, and Seal opens it.
 *
 * It writes what every OpenPGP program reads, for the keys Seal, openpgp.js
 * and GnuPG make (v4 keys whose encryption subkey is Curve25519 ECDH):
 * - for each recipient, the session key wrapped to its encryption subkey
 *   (PKESK version 3, ECDH: X25519 with a fresh key of its own, the key that
 *   wraps derived by the key's KDF, RFC 9580 section 11.5, and AES key wrap,
 *   RFC 3394);
 * - then the data, one literal data packet, sealed under a fresh AES-256
 *   session key with its modification detection code (SEIPD version 1).
 * Nothing secret is kept: the session key and each fresh X25519 key are wiped
 * once used.
 */

// One recipient: a v4 key's Curve25519 encryption subkey.
struct Recipient {
	// The primary key's fingerprint, 40 upper-case hex digits: whose key it is.
	std::string primary_fingerprint;
	// The encryption subkey's fingerprint, which the session names it by and
	// its KDF binds, and its X25519 public key.
	std::array<std::uint8_t, 20> subkey_fingerprint {};
	std::array<std::uint8_t, 32> public_key {};
	// The KDF's hash (8: SHA-256) and the cipher that wraps (7, 8, 9: AES-128,
	// -192, -256), as the subkey says.
	std::uint8_t kdf_hash = 8;
	std::uint8_t kdf_cipher = 7;
	// The address of the key's first user ID, for reading.
	std::string address;
};

// 40 upper-case hex digits of a fingerprint.
std::string fingerprint_hex(const std::array<std::uint8_t, 20> &fingerprint);

// The recipient a v4 public key names (armored or binary): its Curve25519 ECDH
// subkey whose fingerprint is `subkey_fingerprint` (40 hex digits, any case),
// or, when that is empty, its newest one. Nothing when the key is not one
// primary key, or holds no such subkey, or its hash or cipher is not one this
// writes.
std::optional<Recipient> recipient_from_key(const std::string &key, const std::string &subkey_fingerprint = std::string());

// Random bytes: production gives the platform's secure random; a test may give
// its own to seal the same message twice.
using RandomBytes = std::function<void(std::uint8_t *into, std::size_t count)>;
RandomBytes secure_random();

// The message, binary, sealed to every recipient: `data` as a literal data
// packet named `name` (at most 255 bytes) and dated `when` (seconds since
// 1970). Throws std::invalid_argument with no recipient, std::runtime_error
// where a recipient's key is not one X25519 can use.
std::string seal(const std::vector<Recipient> &recipients, const std::string &data, const std::string &name, std::uint32_t when,
	const RandomBytes &random);

// ASCII armor (RFC 9580, section 6.2): "-----BEGIN PGP MESSAGE-----", the
// base64 in lines of 64, and its CRC-24.
std::string armored(const std::string &binary);

// AES key wrap (RFC 3394) of `plain` (a multiple of 8 bytes, at least 16)
// under `kek` (16, 24 or 32 bytes): what wraps each session key, given for its
// test against the RFC's own values.
std::string aes_key_wrap(const std::string &kek, const std::string &plain);

} // namespace singularity::node::openpgp
