#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace singularity::matter::coordination::transport::noise {

// How two of a firm's computers meet directly: the Noise protocol's IK
// handshake, Noise_IK_25519_ChaChaPoly_BLAKE2b (noiseprotocol.org, revision
// 34), checked against its published test vectors. The computer that calls
// knows the other's static X25519 key from the Matter's member record, which a
// lawyer confirmed by its code; the one called learns the caller's key from
// the first message and admits it only if it is a confirmed computer of the
// Matter. No server can stand between them: none knows their keys.

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;
using HandshakeHash = std::array<std::uint8_t, 64>;

struct KeyPair {
	Key secret{};
	Key public_key{};
};

// The X25519 key pair of a secret key.
KeyPair key_pair_from_secret(const Key& secret);

// One direction of an established session: ChaCha20-Poly1305 (RFC 8439) under
// its key, the 96-bit nonce being four zero bytes and the 64-bit counter,
// little-endian, as Noise sets it.
class CipherState {
public:
	CipherState() = default;
	explicit CipherState(const Key& key);
	bool has_key() const { return hasKey_; }
	// With the nonce given: what a datagram carries, as WireGuard does, since
	// UDP neither keeps order nor delivers everything.
	Bytes encrypt(std::uint64_t nonce, const Bytes& ad, const Bytes& plaintext) const;
	std::optional<Bytes> decrypt(std::uint64_t nonce, const Bytes& ad, const Bytes& ciphertext) const;
	// With Noise's own counter: each message the next nonce.
	Bytes encrypt_next(const Bytes& ad, const Bytes& plaintext);
	std::optional<Bytes> decrypt_next(const Bytes& ad, const Bytes& ciphertext);
	std::uint64_t next_nonce() const { return nonce_; }

private:
	Key key_{};
	bool hasKey_ = false;
	std::uint64_t nonce_ = 0;
};

// What a finished handshake gives each side.
struct Session {
	CipherState send;
	CipherState receive;
	HandshakeHash handshake_hash{};
	Key remote_static{};
};

namespace detail {
struct SymmetricState {
	std::array<std::uint8_t, 64> chaining_key{};
	std::array<std::uint8_t, 64> hash{};
	CipherState cipher;
	void initialize(const Bytes& protocol_name);
	void mix_hash(const std::uint8_t* data, std::size_t size);
	void mix_key(const Key& input_key_material);
	Bytes encrypt_and_hash(const Bytes& plaintext);
	std::optional<Bytes> decrypt_and_hash(const Bytes& ciphertext);
	void split(CipherState& first, CipherState& second) const;
};
} // namespace detail

// The computer that calls. `ephemeral` is a fresh key pair from the system's
// secure random source; tests give the vectors' own.
class Initiator {
public:
	Initiator(const KeyPair& self, const Key& remote_static, const Bytes& prologue, const KeyPair& ephemeral);
	// -> e, es, s, ss: the first message, carrying `payload`.
	Bytes write_first(const Bytes& payload);
	// <- e, ee, se: the answer; its payload, or nothing where it is not the
	// expected computer's or was changed on the way.
	std::optional<Bytes> read_second(const Bytes& message);
	// Once read_second succeeded.
	const Session& session() const { return session_; }

private:
	KeyPair self_;
	KeyPair ephemeral_;
	Key remoteStatic_{};
	Key remoteEphemeral_{};
	detail::SymmetricState state_;
	Session session_;
};

// The computer that is called.
class Responder {
public:
	Responder(const KeyPair& self, const Bytes& prologue, const KeyPair& ephemeral);
	// The first message: its payload, or nothing where it was changed on the
	// way. remote_static() then names the caller, to be admitted or not.
	std::optional<Bytes> read_first(const Bytes& message);
	const Key& remote_static() const { return remoteStatic_; }
	// The answer, once the caller is admitted.
	Bytes write_second(const Bytes& payload);
	const Session& session() const { return session_; }

private:
	KeyPair self_;
	KeyPair ephemeral_;
	Key remoteStatic_{};
	Key remoteEphemeral_{};
	detail::SymmetricState state_;
	Session session_;
};

// The protocol's name, as the handshake hashes it.
inline constexpr const char* kProtocolName = "Noise_IK_25519_ChaChaPoly_BLAKE2b";

} // namespace singularity::matter::coordination::transport::noise
