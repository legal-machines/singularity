#include "Matter/CoordinationSeal/CoordinationSeal.hpp"

#include "Platform/SecureRandom/SecureRandom.hpp"

#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace singularity::matter::coordination::seal {
namespace {

using Json = nlohmann::json;
using Bytes = std::string;

constexpr std::size_t kNonceBytes = 24;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kKeyIdBytes = 8;
// What an envelope says of itself is short: a Matter, a sender, receivers and a counter.
constexpr std::size_t kMostHeaderBytes = 4096;
// Below it every sealed change looks the same.
constexpr std::size_t kSmallestPlaintext = 256;
constexpr std::size_t kNameBlock = 64;

constexpr const char *kChangesPurpose = "Singularity coordination 1: changes";
constexpr const char *kNamesPurpose = "Singularity coordination 1: names";
constexpr const char *kCardsPurpose = "Singularity coordination 1: address cards";
constexpr const char *kKeyIdPurpose = "Singularity coordination 1: key id";
constexpr const char *kWrapPurpose = "Singularity coordination 1: key wrap";
constexpr const char *kAccountPurpose = "Singularity coordination 1: account gift";
// The most an account's entry and key may be together, sealed.
constexpr std::size_t kMostAccountBytes = 256 * 1024;
constexpr const char *kInventoryPurpose = "Singularity coordination 1: inventories";

// ---- Small helpers ----------------------------------------------------------

const std::uint8_t *raw(const Bytes &bytes) { return reinterpret_cast<const std::uint8_t *>(bytes.data()); }
std::uint8_t *raw(Bytes &bytes) { return reinterpret_cast<std::uint8_t *>(bytes.data()); }

std::string hex(const std::uint8_t *bytes, std::size_t size) {
	static const char *digits = "0123456789abcdef";
	std::string text;
	text.reserve(size * 2);
	for (std::size_t index = 0; index < size; ++index) {
		text += digits[bytes[index] >> 4];
		text += digits[bytes[index] & 15];
	}
	return text;
}

void putLength(Bytes &out, std::size_t length) {
	out.push_back(static_cast<char>((length >> 8) & 255u));
	out.push_back(static_cast<char>(length & 255u));
}

std::optional<std::size_t> lengthAt(const Bytes &in, std::size_t at) {
	if (at + 2 > in.size()) return std::nullopt;
	return (static_cast<std::size_t>(static_cast<std::uint8_t>(in[at])) << 8) | static_cast<std::uint8_t>(in[at + 1]);
}

// A size a header says, or zero when it says none, or anything but a whole
// number not below zero.
std::uint64_t sizeSaid(const Json &header, const char *name) {
	const auto found = header.find(name);
	if (found == header.end() || found->is_number_unsigned() == false) return 0;
	return found->get<std::uint64_t>();
}

// Padmé (Nikitin et al., PURBs, 2019): a length rounded up so that it leaks at
// most O(log log n) bits, never more than about 12% larger.
std::uint64_t padme(std::uint64_t length) {
	if (length < 2) return length;
	int exponent = 63;
	while (((length >> exponent) & 1u) == 0) --exponent;
	int bits = 0;
	for (std::uint64_t value = static_cast<std::uint64_t>(exponent); value > 0; value >>= 1) ++bits;
	const int lastBits = exponent - bits;
	if (lastBits <= 0) return length;
	const std::uint64_t mask = (std::uint64_t{1} << lastBits) - 1;
	return (length + mask) & ~mask;
}

// The payload, its length before it, padded.
Bytes padded(const Bytes &payload) {
	Bytes plain;
	const auto length = static_cast<std::uint32_t>(payload.size());
	plain.push_back(static_cast<char>((length >> 24) & 255u));
	plain.push_back(static_cast<char>((length >> 16) & 255u));
	plain.push_back(static_cast<char>((length >> 8) & 255u));
	plain.push_back(static_cast<char>(length & 255u));
	plain += payload;
	plain.resize(std::max<std::size_t>(kSmallestPlaintext, static_cast<std::size_t>(padme(plain.size()))), '\0');
	return plain;
}

std::optional<Bytes> unpadded(const Bytes &plain) {
	if (plain.size() < 4) return std::nullopt;
	const std::size_t length = (static_cast<std::size_t>(static_cast<std::uint8_t>(plain[0])) << 24)
		| (static_cast<std::size_t>(static_cast<std::uint8_t>(plain[1])) << 16)
		| (static_cast<std::size_t>(static_cast<std::uint8_t>(plain[2])) << 8) | static_cast<std::uint8_t>(plain[3]);
	if (length > plain.size() - 4) return std::nullopt;
	return plain.substr(4, length);
}

// A key of its own for each use, from the Matter's key: what one use seals
// another can never open.
struct Derived {
	std::uint8_t bytes[kKeyBytes];
	Derived(const std::uint8_t *key, std::size_t key_size, const Bytes &message) {
		crypto_blake2b_keyed(bytes, sizeof(bytes), key, key_size, raw(message), message.size());
	}
	Derived(const MatterKey &key, const char *purpose)
		: Derived(key.bytes.data(), key.bytes.size(), Bytes(purpose)) {}
	~Derived() { crypto_wipe(bytes, sizeof(bytes)); }
	Derived(const Derived &) = delete;
	Derived &operator=(const Derived &) = delete;
};

// `bound` is everything before the nonce: what routes the envelope. The nonce,
// the tag and the sealed bytes follow it.
Bytes lockAfter(const Bytes &bound, const std::uint8_t key[kKeyBytes], const Bytes &plain) {
	Bytes out = bound;
	const std::size_t nonceAt = out.size();
	out.resize(nonceAt + kNonceBytes + kTagBytes + plain.size());
	std::uint8_t *nonce = raw(out) + nonceAt;
	std::uint8_t *tag = nonce + kNonceBytes;
	platform::fill_secure_random(nonce, kNonceBytes);
	crypto_aead_lock(tag + kTagBytes, tag, key, nonce, raw(bound), bound.size(), raw(plain), plain.size());
	return out;
}

std::optional<Bytes> unlockAfter(const Bytes &envelope, std::size_t nonceAt, std::size_t sealedEnd,
		const std::uint8_t key[kKeyBytes]) {
	if (sealedEnd > envelope.size() || nonceAt + kNonceBytes + kTagBytes > sealedEnd) return std::nullopt;
	const std::uint8_t *nonce = raw(envelope) + nonceAt;
	const std::uint8_t *tag = nonce + kNonceBytes;
	const std::size_t size = sealedEnd - nonceAt - kNonceBytes - kTagBytes;
	Bytes plain(size, '\0');
	if (crypto_aead_unlock(raw(plain), tag, key, nonce, raw(envelope), nonceAt, tag + kTagBytes, size) != 0) {
		return std::nullopt;
	}
	return plain;
}

Header headerFrom(const Json &value) {
	Header header;
	header.matter_id = value.value("m", std::string());
	header.from_device = value.value("f", std::string());
	if (value.contains("t") == true && value.at("t").is_array() == true) {
		for (const Json &to : value.at("t")) {
			if (to.is_string() == true) header.to_devices.push_back(to.get<std::string>());
		}
	} else if (value.contains("t") == true && value.at("t").is_string() == true) {
		header.to_devices.push_back(value.at("t").get<std::string>());
	}
	header.counter = value.value("c", static_cast<std::int64_t>(0));
	return header;
}

// [kind][key id][header length][header]: what a change or a card says of
// itself before it is opened.
Bytes keyedOutside(Kind kind, const MatterKey &key, const Json &header) {
	Bytes out;
	out.push_back(static_cast<char>(kind));
	const Derived id(key, kKeyIdPurpose);
	out.append(reinterpret_cast<const char *>(id.bytes), kKeyIdBytes);
	const Bytes text = header.dump();
	putLength(out, text.size());
	out += text;
	return out;
}

struct Parsed {
	Kind kind = Kind::Unknown;
	std::string key_id;
	Json header;
	std::size_t end = 0;
};

std::optional<Parsed> parseKeyed(const Bytes &envelope) {
	if (envelope.empty() == true) return std::nullopt;
	Parsed parsed;
	parsed.kind = static_cast<Kind>(static_cast<std::uint8_t>(envelope[0]));
	if (parsed.kind != Kind::Change && parsed.kind != Kind::AddressCard && parsed.kind != Kind::Inventory) return std::nullopt;
	if (envelope.size() < 1 + kKeyIdBytes + 2) return std::nullopt;
	parsed.key_id = hex(raw(envelope) + 1, kKeyIdBytes);
	const std::optional<std::size_t> length = lengthAt(envelope, 1 + kKeyIdBytes);
	const std::size_t headerAt = 1 + kKeyIdBytes + 2;
	if (length.has_value() == false || *length > kMostHeaderBytes || headerAt + *length > envelope.size()) return std::nullopt;
	parsed.header = Json::parse(envelope.substr(headerAt, *length), nullptr, false);
	if (parsed.header.is_object() == false) return std::nullopt;
	parsed.end = headerAt + *length;
	return parsed;
}

// [kind][length][text]...[signature length][signature]: the unkeyed kinds,
// signed by their sender.
struct Signed {
	Bytes body;
	std::string signature;
};

Bytes withSignature(const Bytes &body, const Sign &sign) {
	const std::string signature = sign(body);
	Bytes out = body;
	putLength(out, signature.size());
	out += signature;
	return out;
}

std::optional<Signed> splitSigned(const Bytes &envelope, std::size_t bodyEnd) {
	const std::optional<std::size_t> length = lengthAt(envelope, bodyEnd);
	if (length.has_value() == false || bodyEnd + 2 + *length != envelope.size() || *length == 0) return std::nullopt;
	return Signed{envelope.substr(0, bodyEnd), envelope.substr(bodyEnd + 2)};
}

// ---- Base64 (RFC 4648, padded) ------------------------------------------------

constexpr const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode64(const std::uint8_t *bytes, std::size_t size) {
	std::string text;
	std::size_t at = 0;
	while (at + 2 < size) {
		const unsigned value = (bytes[at] << 16) | (bytes[at + 1] << 8) | bytes[at + 2];
		text += kAlphabet[(value >> 18) & 63];
		text += kAlphabet[(value >> 12) & 63];
		text += kAlphabet[(value >> 6) & 63];
		text += kAlphabet[value & 63];
		at += 3;
	}
	if (at < size) {
		unsigned value = bytes[at] << 16;
		if (at + 1 < size) value |= bytes[at + 1] << 8;
		text += kAlphabet[(value >> 18) & 63];
		text += kAlphabet[(value >> 12) & 63];
		text += at + 1 < size ? kAlphabet[(value >> 6) & 63] : '=';
		text += '=';
	}
	return text;
}

std::optional<Bytes> decode64(const std::string &text) {
	if (text.size() % 4 != 0) return std::nullopt;
	Bytes bytes;
	for (std::size_t at = 0; at < text.size(); at += 4) {
		unsigned value = 0;
		int padding = 0;
		for (int index = 0; index < 4; ++index) {
			const char character = text[at + static_cast<std::size_t>(index)];
			const char *found = character == '=' ? nullptr : std::strchr(kAlphabet, character);
			if (character == '=' && at + 4 == text.size() && index >= 2) {
				++padding;
				value <<= 6;
				continue;
			}
			if (found == nullptr || character == '\0' || padding > 0) return std::nullopt;
			value = (value << 6) | static_cast<unsigned>(found - kAlphabet);
		}
		bytes += static_cast<char>((value >> 16) & 255);
		if (padding < 2) bytes += static_cast<char>((value >> 8) & 255);
		if (padding < 1) bytes += static_cast<char>(value & 255);
	}
	return bytes;
}

bool isAllZero(const std::uint8_t *bytes, std::size_t size) {
	std::uint8_t any = 0;
	for (std::size_t index = 0; index < size; ++index) any |= bytes[index];
	return any == 0;
}

} // namespace

// ---- Kinds and ids ------------------------------------------------------------

Kind kind_of(const std::string &envelope) {
	if (envelope.empty() == true) return Kind::Unknown;
	const auto first = static_cast<std::uint8_t>(envelope[0]);
	return first >= 1 && first <= 6 ? static_cast<Kind>(first) : Kind::Unknown;
}

// ---- Keys -----------------------------------------------------------------------

MatterKey::~MatterKey() {
	crypto_wipe(bytes.data(), bytes.size());
}

ComputerKeys::~ComputerKeys() {
	crypto_wipe(secret.data(), secret.size());
}

MatterKey make_matter_key() {
	MatterKey key;
	platform::fill_secure_random(key.bytes.data(), key.bytes.size());
	return key;
}

std::string matter_key_id(const MatterKey &key) {
	const Derived id(key, kKeyIdPurpose);
	return hex(id.bytes, kKeyIdBytes);
}

ComputerKeys make_computer_keys() {
	std::array<std::uint8_t, kKeyBytes> secret {};
	platform::fill_secure_random(secret.data(), secret.size());
	ComputerKeys keys = computer_keys_from_secret(secret);
	crypto_wipe(secret.data(), secret.size());
	return keys;
}

ComputerKeys computer_keys_from_secret(const std::array<std::uint8_t, kKeyBytes> &secret) {
	ComputerKeys keys;
	keys.secret = secret;
	crypto_x25519_public_key(keys.public_key.data(), keys.secret.data());
	return keys;
}

std::string key_text(const std::array<std::uint8_t, kKeyBytes> &bytes) {
	return encode64(bytes.data(), bytes.size());
}

std::optional<std::array<std::uint8_t, kKeyBytes>> key_from_text(const std::string &text) {
	std::optional<Bytes> bytes = decode64(text);
	if (bytes.has_value() == false || bytes->size() != kKeyBytes) {
		if (bytes.has_value() == true) crypto_wipe(raw(*bytes), bytes->size());
		return std::nullopt;
	}
	std::array<std::uint8_t, kKeyBytes> key {};
	std::memcpy(key.data(), bytes->data(), kKeyBytes);
	crypto_wipe(raw(*bytes), bytes->size());
	return key;
}

// ---- 0x01: a change -------------------------------------------------------------

std::string seal_change(const MatterKey &key, const Header &header, const std::string &plaintext) {
	const Bytes bound = keyedOutside(Kind::Change, key,
		Json{{"m", header.matter_id}, {"f", header.from_device}, {"t", header.to_devices}, {"c", header.counter}});
	Bytes plain = padded(plaintext);
	const Derived derived(key, kChangesPurpose);
	Bytes envelope = lockAfter(bound, derived.bytes, plain);
	crypto_wipe(raw(plain), plain.size());
	return envelope;
}

std::optional<Outside> outside_of(const std::string &envelope) {
	const Kind kind = kind_of(envelope);
	if (kind == Kind::Change || kind == Kind::AddressCard || kind == Kind::Inventory) {
		const std::optional<Parsed> parsed = parseKeyed(envelope);
		if (parsed.has_value() == false) return std::nullopt;
		return Outside{parsed->kind, parsed->key_id, headerFrom(parsed->header)};
	}
	if (kind == Kind::KeyWrap) {
		const std::optional<KeyOutside> outside = key_outside(envelope);
		if (outside.has_value() == false) return std::nullopt;
		Outside result;
		result.kind = kind;
		result.header.matter_id = outside->matter_id;
		result.header.from_device = outside->from_device;
		result.header.to_devices = {outside->to_device};
		return result;
	}
	if (kind == Kind::JoinRequest) {
		const std::optional<std::size_t> length = lengthAt(envelope, 1);
		if (length.has_value() == false || 3 + *length > envelope.size()) return std::nullopt;
		const Json body = Json::parse(envelope.substr(3, *length), nullptr, false);
		if (body.is_object() == false) return std::nullopt;
		Outside result;
		result.kind = kind;
		result.header.matter_id = body.value("m", std::string());
		result.header.from_device = body.value("d", std::string());
		return result;
	}
	return std::nullopt;
}

std::optional<OpenedChange> open_change(const MatterKey &key, const std::string &envelope) {
	const std::optional<Parsed> parsed = parseKeyed(envelope);
	if (parsed.has_value() == false || parsed->kind != Kind::Change || parsed->key_id != matter_key_id(key)) return std::nullopt;
	const Derived derived(key, kChangesPurpose);
	std::optional<Bytes> plain = unlockAfter(envelope, parsed->end, envelope.size(), derived.bytes);
	if (plain.has_value() == false) return std::nullopt;
	std::optional<Bytes> payload = unpadded(*plain);
	crypto_wipe(raw(*plain), plain->size());
	if (payload.has_value() == false) return std::nullopt;
	return OpenedChange{headerFrom(parsed->header), parsed->key_id, std::move(*payload)};
}

// ---- 0x02: a Matter's key, to one computer -----------------------------------------

std::string seal_key(const KeyGift &gift, const std::array<std::uint8_t, kKeyBytes> &recipient_public, const Sign &sign) {
	Bytes body;
	body.push_back(static_cast<char>(Kind::KeyWrap));
	Json said = Json{{"m", gift.matter_id}, {"f", gift.from_device}, {"k", gift.from_device_key},
		{"t", gift.to_device}, {"c", gift.counter}};
	// The Matter's size, where its giver says it (D-207).
	if (gift.files > 0 || gift.bytes > 0) {
		said["n"] = gift.files;
		said["b"] = gift.bytes;
	}
	const Bytes header = said.dump();
	putLength(body, header.size());
	body += header;
	// A key pair made for this seal alone: only the recipient's secret key
	// and this one, now forgotten, reach the shared secret.
	ComputerKeys ephemeral = make_computer_keys();
	body.append(reinterpret_cast<const char *>(ephemeral.public_key.data()), kKeyBytes);
	std::uint8_t shared[kKeyBytes];
	crypto_x25519(shared, ephemeral.secret.data(), recipient_public.data());
	Bytes context = kWrapPurpose;
	context.append(reinterpret_cast<const char *>(ephemeral.public_key.data()), kKeyBytes);
	context.append(reinterpret_cast<const char *>(recipient_public.data()), kKeyBytes);
	const Derived wrap(shared, sizeof(shared), context);
	crypto_wipe(shared, sizeof(shared));
	Bytes plain(reinterpret_cast<const char *>(gift.key.bytes.data()), kKeyBytes);
	Bytes sealed = lockAfter(body, wrap.bytes, plain);
	crypto_wipe(raw(plain), plain.size());
	return withSignature(sealed, sign);
}

std::optional<KeyOutside> key_outside(const std::string &envelope) {
	if (kind_of(envelope) != Kind::KeyWrap) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(envelope, 1);
	if (length.has_value() == false || *length > kMostHeaderBytes || 3 + *length > envelope.size()) return std::nullopt;
	const Json header = Json::parse(envelope.substr(3, *length), nullptr, false);
	if (header.is_object() == false) return std::nullopt;
	return KeyOutside{header.value("m", std::string()), header.value("f", std::string()), header.value("k", std::string()),
		header.value("t", std::string()), sizeSaid(header, "n"), sizeSaid(header, "b")};
}

std::optional<KeyGift> open_key(const std::string &envelope, const ComputerKeys &mine, const std::string &sender_device_key,
		const Verify &verify) {
	if (kind_of(envelope) != Kind::KeyWrap) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(envelope, 1);
	if (length.has_value() == false || *length > kMostHeaderBytes) return std::nullopt;
	const std::size_t ephemeralAt = 3 + *length;
	const std::size_t nonceAt = ephemeralAt + kKeyBytes;
	const std::size_t sealedEnd = nonceAt + kNonceBytes + kTagBytes + kKeyBytes;
	if (sealedEnd > envelope.size()) return std::nullopt;
	const std::optional<Signed> signedBy = splitSigned(envelope, sealedEnd);
	if (signedBy.has_value() == false || verify(sender_device_key, signedBy->body, signedBy->signature) == false) {
		return std::nullopt;
	}
	const Json header = Json::parse(envelope.substr(3, *length), nullptr, false);
	if (header.is_object() == false || header.value("k", std::string()) != sender_device_key) return std::nullopt;
	std::array<std::uint8_t, kKeyBytes> ephemeral {};
	std::memcpy(ephemeral.data(), envelope.data() + ephemeralAt, kKeyBytes);
	std::uint8_t shared[kKeyBytes];
	crypto_x25519(shared, mine.secret.data(), ephemeral.data());
	if (isAllZero(shared, sizeof(shared)) == true) return std::nullopt;
	Bytes context = kWrapPurpose;
	context.append(reinterpret_cast<const char *>(ephemeral.data()), kKeyBytes);
	context.append(reinterpret_cast<const char *>(mine.public_key.data()), kKeyBytes);
	const Derived wrap(shared, sizeof(shared), context);
	crypto_wipe(shared, sizeof(shared));
	std::optional<Bytes> plain = unlockAfter(envelope, nonceAt, sealedEnd, wrap.bytes);
	if (plain.has_value() == false || plain->size() != kKeyBytes) return std::nullopt;
	KeyGift gift;
	gift.matter_id = header.value("m", std::string());
	gift.from_device = header.value("f", std::string());
	gift.from_device_key = header.value("k", std::string());
	gift.to_device = header.value("t", std::string());
	gift.counter = header.value("c", static_cast<std::int64_t>(0));
	gift.files = sizeSaid(header, "n");
	gift.bytes = sizeSaid(header, "b");
	std::memcpy(gift.key.bytes.data(), plain->data(), kKeyBytes);
	crypto_wipe(raw(*plain), plain->size());
	return gift;
}

// ---- 0x06: one of the firm's model accounts, given (D-210) ---------------------------

AccountGift::~AccountGift() {
	if (credential.empty() == false) crypto_wipe(raw(credential), credential.size());
}

std::string seal_account(const AccountGift &gift, const std::array<std::uint8_t, kKeyBytes> &recipient_public, const Sign &sign) {
	Bytes plain = Json{{"e", gift.entry}, {"x", gift.credential}, {"r", gift.removed}}.dump();
	if (plain.size() > kMostAccountBytes) {
		crypto_wipe(raw(plain), plain.size());
		throw std::invalid_argument("This account is too large to give.");
	}
	Bytes body;
	body.push_back(static_cast<char>(Kind::AccountGift));
	// The sealed part's length is in the header, which the seal and the
	// signature cover: an envelope cut short or lengthened never opens.
	const Bytes header = Json{{"m", gift.matter_id}, {"f", gift.from_device}, {"k", gift.from_device_key},
		{"t", gift.to_device}, {"c", gift.counter}, {"p", gift.provider_id}, {"s", plain.size()}}.dump();
	putLength(body, header.size());
	body += header;
	ComputerKeys ephemeral = make_computer_keys();
	body.append(reinterpret_cast<const char *>(ephemeral.public_key.data()), kKeyBytes);
	std::uint8_t shared[kKeyBytes];
	crypto_x25519(shared, ephemeral.secret.data(), recipient_public.data());
	Bytes context = kAccountPurpose;
	context.append(reinterpret_cast<const char *>(ephemeral.public_key.data()), kKeyBytes);
	context.append(reinterpret_cast<const char *>(recipient_public.data()), kKeyBytes);
	const Derived wrap(shared, sizeof(shared), context);
	crypto_wipe(shared, sizeof(shared));
	Bytes sealed = lockAfter(body, wrap.bytes, plain);
	crypto_wipe(raw(plain), plain.size());
	return withSignature(sealed, sign);
}

std::optional<AccountOutside> account_outside(const std::string &envelope) {
	if (kind_of(envelope) != Kind::AccountGift) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(envelope, 1);
	if (length.has_value() == false || *length > kMostHeaderBytes || 3 + *length > envelope.size()) return std::nullopt;
	const Json header = Json::parse(envelope.substr(3, *length), nullptr, false);
	if (header.is_object() == false) return std::nullopt;
	return AccountOutside{header.value("m", std::string()), header.value("f", std::string()), header.value("k", std::string()),
		header.value("t", std::string()), header.value("p", std::string())};
}

std::optional<AccountGift> open_account(const std::string &envelope, const ComputerKeys &mine, const std::string &sender_device_key,
		const Verify &verify) {
	if (kind_of(envelope) != Kind::AccountGift) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(envelope, 1);
	if (length.has_value() == false || *length > kMostHeaderBytes || 3 + *length > envelope.size()) return std::nullopt;
	const Json header = Json::parse(envelope.substr(3, *length), nullptr, false);
	if (header.is_object() == false || header.value("k", std::string()) != sender_device_key) return std::nullopt;
	const std::uint64_t sealedSize = header.contains("s") == true && header.at("s").is_number_unsigned() == true
		? header.at("s").get<std::uint64_t>() : 0;
	if (sealedSize == 0 || sealedSize > kMostAccountBytes) return std::nullopt;
	const std::size_t ephemeralAt = 3 + *length;
	const std::size_t nonceAt = ephemeralAt + kKeyBytes;
	const std::size_t sealedEnd = nonceAt + kNonceBytes + kTagBytes + static_cast<std::size_t>(sealedSize);
	if (sealedEnd > envelope.size()) return std::nullopt;
	const std::optional<Signed> signedBy = splitSigned(envelope, sealedEnd);
	if (signedBy.has_value() == false || verify(sender_device_key, signedBy->body, signedBy->signature) == false) return std::nullopt;
	std::array<std::uint8_t, kKeyBytes> ephemeral {};
	std::memcpy(ephemeral.data(), envelope.data() + ephemeralAt, kKeyBytes);
	std::uint8_t shared[kKeyBytes];
	crypto_x25519(shared, mine.secret.data(), ephemeral.data());
	if (isAllZero(shared, sizeof(shared)) == true) return std::nullopt;
	Bytes context = kAccountPurpose;
	context.append(reinterpret_cast<const char *>(ephemeral.data()), kKeyBytes);
	context.append(reinterpret_cast<const char *>(mine.public_key.data()), kKeyBytes);
	const Derived wrap(shared, sizeof(shared), context);
	crypto_wipe(shared, sizeof(shared));
	std::optional<Bytes> plain = unlockAfter(envelope, nonceAt, sealedEnd, wrap.bytes);
	if (plain.has_value() == false) return std::nullopt;
	const Json inside = Json::parse(*plain, nullptr, false);
	crypto_wipe(raw(*plain), plain->size());
	if (inside.is_object() == false || inside.contains("x") == false || inside.at("x").is_string() == false) return std::nullopt;
	AccountGift gift;
	gift.matter_id = header.value("m", std::string());
	gift.from_device = header.value("f", std::string());
	gift.from_device_key = header.value("k", std::string());
	gift.to_device = header.value("t", std::string());
	gift.counter = header.value("c", static_cast<std::int64_t>(0));
	gift.provider_id = header.value("p", std::string());
	gift.entry = inside.value("e", std::string());
	gift.credential = inside.at("x").get<std::string>();
	gift.removed = inside.value("r", false);
	return gift;
}

// ---- 0x03: a computer asks to join ------------------------------------------------

std::string seal_join_request(const JoinRequest &request, const Sign &sign) {
	Bytes body;
	body.push_back(static_cast<char>(Kind::JoinRequest));
	const Bytes text = Json{{"m", request.matter_id}, {"d", request.device_id}, {"a", request.actor_id},
		{"k", request.device_key}, {"x", key_text(request.coordination_key)}, {"at", request.at}}.dump();
	putLength(body, text.size());
	body += text;
	return withSignature(body, sign);
}

std::optional<JoinRequest> open_join_request(const std::string &envelope, const Verify &verify) {
	if (kind_of(envelope) != Kind::JoinRequest) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(envelope, 1);
	if (length.has_value() == false || *length > kMostHeaderBytes || 3 + *length > envelope.size()) return std::nullopt;
	const Json body = Json::parse(envelope.substr(3, *length), nullptr, false);
	if (body.is_object() == false) return std::nullopt;
	const std::optional<Signed> signedBy = splitSigned(envelope, 3 + *length);
	JoinRequest request;
	request.matter_id = body.value("m", std::string());
	request.device_id = body.value("d", std::string());
	request.actor_id = body.value("a", std::string());
	request.device_key = body.value("k", std::string());
	request.at = body.value("at", std::string());
	const auto coordination = key_from_text(body.value("x", std::string()));
	if (signedBy.has_value() == false || coordination.has_value() == false || request.device_key.empty() == true
			|| verify(request.device_key, signedBy->body, signedBy->signature) == false) {
		return std::nullopt;
	}
	request.coordination_key = *coordination;
	return request;
}

// ---- 0x04: where a computer can be reached ------------------------------------------

std::string seal_address_card(const MatterKey &key, const AddressCard &card, const Sign &sign) {
	const Bytes bound = keyedOutside(Kind::AddressCard, key,
		Json{{"m", card.matter_id}, {"f", card.device_id}, {"c", card.counter}});
	// Signed inside the seal: a member of the Matter holds its key, so only the
	// device key says which computer the card is for.
	const Bytes card_text = Json{{"d", card.device_id}, {"cand", card.candidates}, {"exp", card.expires_at},
		{"c", card.counter}, {"m", card.matter_id}, {"x", card.coordination_key}}.dump();
	const std::string signature = sign(card_text);
	Bytes payload;
	putLength(payload, card_text.size());
	payload += card_text;
	payload += signature;
	Bytes plain = padded(payload);
	const Derived derived(key, kCardsPurpose);
	Bytes envelope = lockAfter(bound, derived.bytes, plain);
	crypto_wipe(raw(plain), plain.size());
	return envelope;
}

std::optional<AddressCard> open_address_card(const MatterKey &key, const std::string &envelope, const std::string &device_key,
		const Verify &verify) {
	const std::optional<Parsed> parsed = parseKeyed(envelope);
	if (parsed.has_value() == false || parsed->kind != Kind::AddressCard || parsed->key_id != matter_key_id(key)) {
		return std::nullopt;
	}
	const Derived derived(key, kCardsPurpose);
	const std::optional<Bytes> plain = unlockAfter(envelope, parsed->end, envelope.size(), derived.bytes);
	if (plain.has_value() == false) return std::nullopt;
	const std::optional<Bytes> payload = unpadded(*plain);
	if (payload.has_value() == false) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(*payload, 0);
	if (length.has_value() == false || 2 + *length > payload->size()) return std::nullopt;
	const Bytes card_text = payload->substr(2, *length);
	const std::string signature = payload->substr(2 + *length);
	if (signature.empty() == true || verify(device_key, card_text, signature) == false) return std::nullopt;
	const Json value = Json::parse(card_text, nullptr, false);
	if (value.is_object() == false) return std::nullopt;
	AddressCard card;
	card.matter_id = value.value("m", std::string());
	card.device_id = value.value("d", std::string());
	card.expires_at = value.value("exp", std::string());
	card.counter = value.value("c", static_cast<std::int64_t>(0));
	card.coordination_key = value.value("x", std::string());
	if (value.contains("cand") == true && value.at("cand").is_array() == true) {
		for (const Json &candidate : value.at("cand")) {
			if (candidate.is_string() == true) card.candidates.push_back(candidate.get<std::string>());
		}
	}
	// What the outside says must be what the signed card says.
	const Header header = headerFrom(parsed->header);
	if (header.matter_id != card.matter_id || header.from_device != card.device_id || header.counter != card.counter) {
		return std::nullopt;
	}
	return card;
}

// ---- 0x05: a Matter whole, as one computer sends it ----------------------------------

namespace {

// A file's place in a Matter's folder: forward slashes, no part that is empty,
// "." or "..", no drive, no root, no backslash and no NUL.
bool isFolderPath(const std::string &path) {
	if (path.empty() == true || path.size() > 1024 || path.front() == '/') return false;
	if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos || path.find('\0') != std::string::npos) return false;
	std::size_t start = 0;
	while (start <= path.size()) {
		const std::size_t end = std::min(path.find('/', start), path.size());
		const std::string part = path.substr(start, end - start);
		if (part.empty() == true || part == "." || part == "..") return false;
		start = end + 1;
	}
	return true;
}

bool isSha256(const std::string &text) {
	return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}

} // namespace

std::string seal_inventory(const MatterKey &key, const Inventory &inventory) {
	Json files = Json::array();
	for (const InventoryEntry &entry : inventory.files) {
		if (isFolderPath(entry.path) == false) throw std::invalid_argument("An inventory names a file outside its Matter's folder.");
		if (isSha256(entry.sha256) == false) throw std::invalid_argument("An inventory's file has no SHA-256.");
		files.push_back(Json::array({entry.path, entry.sha256, entry.size, entry.counter, entry.sender}));
	}
	const Header &header = inventory.header;
	const Bytes bound = keyedOutside(Kind::Inventory, key,
		Json{{"m", header.matter_id}, {"f", header.from_device}, {"t", header.to_devices}, {"c", header.counter}});
	Bytes plain = padded(Json{{"files", files}}.dump());
	const Derived derived(key, kInventoryPurpose);
	Bytes envelope = lockAfter(bound, derived.bytes, plain);
	crypto_wipe(raw(plain), plain.size());
	return envelope;
}

std::optional<Inventory> open_inventory(const MatterKey &key, const std::string &envelope) {
	const std::optional<Parsed> parsed = parseKeyed(envelope);
	if (parsed.has_value() == false || parsed->kind != Kind::Inventory || parsed->key_id != matter_key_id(key)) return std::nullopt;
	const Derived derived(key, kInventoryPurpose);
	std::optional<Bytes> plain = unlockAfter(envelope, parsed->end, envelope.size(), derived.bytes);
	if (plain.has_value() == false) return std::nullopt;
	std::optional<Bytes> payload = unpadded(*plain);
	crypto_wipe(raw(*plain), plain->size());
	if (payload.has_value() == false) return std::nullopt;
	const Json value = Json::parse(*payload, nullptr, false);
	crypto_wipe(raw(*payload), payload->size());
	if (value.is_object() == false || value.contains("files") == false || value.at("files").is_array() == false) return std::nullopt;
	Inventory inventory;
	inventory.header = headerFrom(parsed->header);
	for (const Json &file : value.at("files")) {
		if (file.is_array() == false || file.size() != 5 || file[0].is_string() == false || file[1].is_string() == false
				|| file[2].is_number_unsigned() == false || file[3].is_number_integer() == false || file[4].is_string() == false) {
			return std::nullopt;
		}
		InventoryEntry entry{file[0].get<std::string>(), file[1].get<std::string>(), file[2].get<std::uint64_t>(),
			file[3].get<std::int64_t>(), file[4].get<std::string>()};
		if (isFolderPath(entry.path) == false || isSha256(entry.sha256) == false) return std::nullopt;
		inventory.files.push_back(std::move(entry));
	}
	return inventory;
}

// ---- Names ------------------------------------------------------------------------------

std::string seal_name(const MatterKey &key, const std::string &kind, const std::string &id, const std::string &name) {
	Bytes bound = "LMC1 name";
	bound.push_back('\0');
	bound += kind;
	bound.push_back('\0');
	bound += id;
	Bytes plain;
	const std::string clipped = name.substr(0, 4000);
	putLength(plain, clipped.size());
	plain += clipped;
	plain.resize((plain.size() + kNameBlock - 1) / kNameBlock * kNameBlock, '\0');
	const Derived derived(key, kNamesPurpose);
	// The binding is not sent: both sides know what the name is of.
	const Bytes sealed = lockAfter(bound, derived.bytes, plain);
	return sealed.substr(bound.size());
}

std::optional<std::string> open_name(const MatterKey &key, const std::string &kind, const std::string &id,
		const std::string &sealed) {
	Bytes bound = "LMC1 name";
	bound.push_back('\0');
	bound += kind;
	bound.push_back('\0');
	bound += id;
	const Bytes whole = bound + sealed;
	const Derived derived(key, kNamesPurpose);
	const std::optional<Bytes> plain = unlockAfter(whole, bound.size(), whole.size(), derived.bytes);
	if (plain.has_value() == false) return std::nullopt;
	const std::optional<std::size_t> length = lengthAt(*plain, 0);
	if (length.has_value() == false || 2 + *length > plain->size()) return std::nullopt;
	return plain->substr(2, *length);
}

std::size_t sealed_size_at_most(std::size_t plaintext_bytes) {
	return 1 + kKeyIdBytes + 2 + kMostHeaderBytes + kNonceBytes + kTagBytes
		+ std::max<std::size_t>(kSmallestPlaintext, static_cast<std::size_t>(padme(4 + plaintext_bytes)));
}

} // namespace singularity::matter::coordination::seal
