#include "Matter/CoordinationTransport/NoiseIK.hpp"

#include <monocypher.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

namespace singularity::matter::coordination::transport::noise {
namespace {

constexpr std::size_t kHashLength = 64;
constexpr std::size_t kBlockLength = 128;
constexpr std::size_t kTagLength = 16;
constexpr std::size_t kKeyLength = 32;

using Digest = std::array<std::uint8_t, kHashLength>;
using Part = std::pair<const std::uint8_t*, std::size_t>;

Digest hashOf(std::initializer_list<Part> parts) {
	crypto_blake2b_ctx context;
	crypto_blake2b_init(&context, kHashLength);
	for (const Part& part : parts) crypto_blake2b_update(&context, part.first, part.second);
	Digest digest{};
	crypto_blake2b_final(&context, digest.data());
	return digest;
}

// HMAC (RFC 2104) with BLAKE2b, as Noise defines its HKDF.
Digest hmac(const std::uint8_t* key, std::size_t keySize, std::initializer_list<Part> data) {
	std::array<std::uint8_t, kBlockLength> block{};
	if (keySize > kBlockLength) {
		const Digest hashed = hashOf({{key, keySize}});
		std::copy(hashed.begin(), hashed.end(), block.begin());
	} else if (keySize > 0) {
		std::copy(key, key + keySize, block.begin());
	}
	std::array<std::uint8_t, kBlockLength> inner{};
	std::array<std::uint8_t, kBlockLength> outer{};
	for (std::size_t index = 0; index < kBlockLength; ++index) {
		inner[index] = static_cast<std::uint8_t>(block[index] ^ 0x36);
		outer[index] = static_cast<std::uint8_t>(block[index] ^ 0x5c);
	}
	crypto_blake2b_ctx context;
	crypto_blake2b_init(&context, kHashLength);
	crypto_blake2b_update(&context, inner.data(), inner.size());
	for (const Part& part : data) crypto_blake2b_update(&context, part.first, part.second);
	Digest innerHash{};
	crypto_blake2b_final(&context, innerHash.data());
	const Digest result = hashOf({{outer.data(), outer.size()}, {innerHash.data(), innerHash.size()}});
	crypto_wipe(block.data(), block.size());
	crypto_wipe(inner.data(), inner.size());
	crypto_wipe(outer.data(), outer.size());
	crypto_wipe(innerHash.data(), innerHash.size());
	return result;
}

// Noise's HKDF with two outputs.
void hkdf2(const Digest& chainingKey, const std::uint8_t* inputKeyMaterial, std::size_t size, Digest& first, Digest& second) {
	Digest tempKey = hmac(chainingKey.data(), chainingKey.size(), {{inputKeyMaterial, size}});
	const std::uint8_t one = 0x01;
	const std::uint8_t two = 0x02;
	first = hmac(tempKey.data(), tempKey.size(), {{&one, 1}});
	second = hmac(tempKey.data(), tempKey.size(), {{first.data(), first.size()}, {&two, 1}});
	crypto_wipe(tempKey.data(), tempKey.size());
}

Key truncated(const Digest& digest) {
	Key key{};
	std::copy(digest.begin(), digest.begin() + kKeyLength, key.begin());
	return key;
}

Key dh(const Key& secret, const Key& publicKey) {
	Key shared{};
	crypto_x25519(shared.data(), secret.data(), publicKey.data());
	return shared;
}

void nonceOf(std::uint64_t counter, std::uint8_t nonce[12]) {
	std::memset(nonce, 0, 12);
	for (int index = 0; index < 8; ++index) nonce[4 + index] = static_cast<std::uint8_t>((counter >> (8 * index)) & 0xFF);
}

} // namespace

KeyPair key_pair_from_secret(const Key& secret) {
	KeyPair pair;
	pair.secret = secret;
	crypto_x25519_public_key(pair.public_key.data(), pair.secret.data());
	return pair;
}

CipherState::CipherState(const Key& key) : key_(key), hasKey_(true) {}

Bytes CipherState::encrypt(std::uint64_t nonce, const Bytes& ad, const Bytes& plaintext) const {
	if (hasKey_ == false) return plaintext;
	std::uint8_t iv[12];
	nonceOf(nonce, iv);
	crypto_aead_ctx context;
	crypto_aead_init_ietf(&context, key_.data(), iv);
	Bytes sealed(plaintext.size() + kTagLength);
	crypto_aead_write(&context, sealed.data(), sealed.data() + plaintext.size(), ad.data(), ad.size(), plaintext.data(),
		plaintext.size());
	crypto_wipe(&context, sizeof context);
	return sealed;
}

std::optional<Bytes> CipherState::decrypt(std::uint64_t nonce, const Bytes& ad, const Bytes& ciphertext) const {
	if (hasKey_ == false) return ciphertext;
	if (ciphertext.size() < kTagLength) return std::nullopt;
	std::uint8_t iv[12];
	nonceOf(nonce, iv);
	crypto_aead_ctx context;
	crypto_aead_init_ietf(&context, key_.data(), iv);
	const std::size_t size = ciphertext.size() - kTagLength;
	Bytes plaintext(size);
	const int mismatch = crypto_aead_read(&context, plaintext.data(), ciphertext.data() + size, ad.data(), ad.size(),
		ciphertext.data(), size);
	crypto_wipe(&context, sizeof context);
	if (mismatch != 0) {
		crypto_wipe(plaintext.data(), plaintext.size());
		return std::nullopt;
	}
	return plaintext;
}

Bytes CipherState::encrypt_next(const Bytes& ad, const Bytes& plaintext) {
	Bytes sealed = encrypt(nonce_, ad, plaintext);
	if (hasKey_ == true) ++nonce_;
	return sealed;
}

std::optional<Bytes> CipherState::decrypt_next(const Bytes& ad, const Bytes& ciphertext) {
	std::optional<Bytes> opened = decrypt(nonce_, ad, ciphertext);
	if (opened.has_value() == true && hasKey_ == true) ++nonce_;
	return opened;
}

namespace detail {

void SymmetricState::initialize(const Bytes& protocolName) {
	hash.fill(0);
	if (protocolName.size() <= kHashLength) {
		std::copy(protocolName.begin(), protocolName.end(), hash.begin());
	} else {
		hash = hashOf({{protocolName.data(), protocolName.size()}});
	}
	chaining_key = hash;
	cipher = CipherState();
}

void SymmetricState::mix_hash(const std::uint8_t* data, std::size_t size) {
	hash = hashOf({{hash.data(), hash.size()}, {data, size}});
}

void SymmetricState::mix_key(const Key& inputKeyMaterial) {
	Digest next{};
	Digest tempKey{};
	hkdf2(chaining_key, inputKeyMaterial.data(), inputKeyMaterial.size(), next, tempKey);
	chaining_key = next;
	cipher = CipherState(truncated(tempKey));
	crypto_wipe(tempKey.data(), tempKey.size());
}

Bytes SymmetricState::encrypt_and_hash(const Bytes& plaintext) {
	const Bytes ad(hash.begin(), hash.end());
	Bytes sealed = cipher.encrypt_next(ad, plaintext);
	mix_hash(sealed.data(), sealed.size());
	return sealed;
}

std::optional<Bytes> SymmetricState::decrypt_and_hash(const Bytes& ciphertext) {
	const Bytes ad(hash.begin(), hash.end());
	std::optional<Bytes> opened = cipher.decrypt_next(ad, ciphertext);
	if (opened.has_value() == false) return std::nullopt;
	mix_hash(ciphertext.data(), ciphertext.size());
	return opened;
}

void SymmetricState::split(CipherState& first, CipherState& second) const {
	Digest one{};
	Digest two{};
	hkdf2(chaining_key, nullptr, 0, one, two);
	first = CipherState(truncated(one));
	second = CipherState(truncated(two));
	crypto_wipe(one.data(), one.size());
	crypto_wipe(two.data(), two.size());
}

} // namespace detail

namespace {

Bytes protocolNameBytes() {
	const std::string name = kProtocolName;
	return Bytes(name.begin(), name.end());
}

constexpr std::size_t kSealedKeyLength = kKeyLength + kTagLength;

} // namespace

Initiator::Initiator(const KeyPair& self, const Key& remoteStatic, const Bytes& prologue, const KeyPair& ephemeral)
	: self_(self), ephemeral_(ephemeral), remoteStatic_(remoteStatic) {
	state_.initialize(protocolNameBytes());
	state_.mix_hash(prologue.data(), prologue.size());
	// The pre-message "<- s": the responder's static key, known before.
	state_.mix_hash(remoteStatic_.data(), remoteStatic_.size());
}

Bytes Initiator::write_first(const Bytes& payload) {
	Bytes message(ephemeral_.public_key.begin(), ephemeral_.public_key.end());
	state_.mix_hash(ephemeral_.public_key.data(), ephemeral_.public_key.size());
	Key shared = dh(ephemeral_.secret, remoteStatic_);
	state_.mix_key(shared);
	const Bytes sealedStatic = state_.encrypt_and_hash(Bytes(self_.public_key.begin(), self_.public_key.end()));
	message.insert(message.end(), sealedStatic.begin(), sealedStatic.end());
	shared = dh(self_.secret, remoteStatic_);
	state_.mix_key(shared);
	crypto_wipe(shared.data(), shared.size());
	const Bytes sealedPayload = state_.encrypt_and_hash(payload);
	message.insert(message.end(), sealedPayload.begin(), sealedPayload.end());
	return message;
}

std::optional<Bytes> Initiator::read_second(const Bytes& message) {
	if (message.size() < kKeyLength + kTagLength) return std::nullopt;
	std::copy(message.begin(), message.begin() + kKeyLength, remoteEphemeral_.begin());
	detail::SymmetricState state = state_;
	state.mix_hash(remoteEphemeral_.data(), remoteEphemeral_.size());
	Key shared = dh(ephemeral_.secret, remoteEphemeral_);
	state.mix_key(shared);
	shared = dh(self_.secret, remoteEphemeral_);
	state.mix_key(shared);
	crypto_wipe(shared.data(), shared.size());
	std::optional<Bytes> payload = state.decrypt_and_hash(Bytes(message.begin() + kKeyLength, message.end()));
	if (payload.has_value() == false) return std::nullopt;
	state_ = state;
	state_.split(session_.send, session_.receive);
	session_.handshake_hash = state_.hash;
	session_.remote_static = remoteStatic_;
	return payload;
}

Responder::Responder(const KeyPair& self, const Bytes& prologue, const KeyPair& ephemeral)
	: self_(self), ephemeral_(ephemeral) {
	state_.initialize(protocolNameBytes());
	state_.mix_hash(prologue.data(), prologue.size());
	state_.mix_hash(self_.public_key.data(), self_.public_key.size());
}

std::optional<Bytes> Responder::read_first(const Bytes& message) {
	if (message.size() < kKeyLength + kSealedKeyLength + kTagLength) return std::nullopt;
	detail::SymmetricState state = state_;
	Key remoteEphemeral{};
	std::copy(message.begin(), message.begin() + kKeyLength, remoteEphemeral.begin());
	state.mix_hash(remoteEphemeral.data(), remoteEphemeral.size());
	Key shared = dh(self_.secret, remoteEphemeral);
	state.mix_key(shared);
	const std::optional<Bytes> remoteStatic = state.decrypt_and_hash(
		Bytes(message.begin() + kKeyLength, message.begin() + kKeyLength + kSealedKeyLength));
	if (remoteStatic.has_value() == false || remoteStatic->size() != kKeyLength) {
		crypto_wipe(shared.data(), shared.size());
		return std::nullopt;
	}
	Key caller{};
	std::copy(remoteStatic->begin(), remoteStatic->end(), caller.begin());
	shared = dh(self_.secret, caller);
	state.mix_key(shared);
	crypto_wipe(shared.data(), shared.size());
	std::optional<Bytes> payload = state.decrypt_and_hash(Bytes(message.begin() + kKeyLength + kSealedKeyLength, message.end()));
	if (payload.has_value() == false) return std::nullopt;
	state_ = state;
	remoteEphemeral_ = remoteEphemeral;
	remoteStatic_ = caller;
	return payload;
}

Bytes Responder::write_second(const Bytes& payload) {
	Bytes message(ephemeral_.public_key.begin(), ephemeral_.public_key.end());
	state_.mix_hash(ephemeral_.public_key.data(), ephemeral_.public_key.size());
	Key shared = dh(ephemeral_.secret, remoteEphemeral_);
	state_.mix_key(shared);
	shared = dh(ephemeral_.secret, remoteStatic_);
	state_.mix_key(shared);
	crypto_wipe(shared.data(), shared.size());
	const Bytes sealedPayload = state_.encrypt_and_hash(payload);
	message.insert(message.end(), sealedPayload.begin(), sealedPayload.end());
	// The responder sends with the second key and receives with the first.
	CipherState first;
	CipherState second;
	state_.split(first, second);
	session_.send = second;
	session_.receive = first;
	session_.handshake_hash = state_.hash;
	session_.remote_static = remoteStatic_;
	return message;
}

} // namespace singularity::matter::coordination::transport::noise
