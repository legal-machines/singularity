#include "Node/OpenPgpMessage/OpenPgpMessage.hpp"

#include "Platform/SecureRandom/SecureRandom.hpp"

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <monocypher.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace singularity::node::openpgp {

namespace {

constexpr std::uint8_t kPkeskTag = 1;
constexpr std::uint8_t kPublicKeyTag = 6;
constexpr std::uint8_t kLiteralTag = 11;
constexpr std::uint8_t kUserIdTag = 13;
constexpr std::uint8_t kPublicSubkeyTag = 14;
constexpr std::uint8_t kSeipdTag = 18;
constexpr std::uint8_t kEcdh = 18;
constexpr std::uint8_t kSha256 = 8;
constexpr std::uint8_t kAes256 = 9;
// Curve25519's OID as OpenPGP writes it (1.3.6.1.4.1.3029.1.5.1).
constexpr std::uint8_t kCurve25519Oid[] = {0x2B, 0x06, 0x01, 0x04, 0x01, 0x97, 0x55, 0x01, 0x05, 0x01};
// What RFC 9580's KDF takes in place of a sender (section 11.5).
constexpr char kAnonymousSender[] = "Anonymous Sender    ";

std::uint8_t octet(const std::string &bytes, std::size_t at) { return static_cast<std::uint8_t>(bytes[at]); }

const unsigned char *raw(const std::string &bytes) { return reinterpret_cast<const unsigned char *>(bytes.data()); }

std::string text_of(const std::uint8_t *bytes, std::size_t count) { return std::string(reinterpret_cast<const char *>(bytes), count); }

void wipe(std::string &bytes) {
	if (bytes.empty() == false) crypto_wipe(bytes.data(), bytes.size());
}

// A packet as every reader takes it: its tag, then its length in one, two or
// five octets (RFC 9580, section 4.2.1).
std::string packet(std::uint8_t tag, const std::string &body) {
	std::string out(1, static_cast<char>(0xC0 | tag));
	const std::size_t length = body.size();
	if (length < 192) {
		out += static_cast<char>(length);
	} else if (length < 8384) {
		const std::size_t rest = length - 192;
		out += static_cast<char>((rest >> 8) + 192);
		out += static_cast<char>(rest & 0xFF);
	} else {
		out += static_cast<char>(0xFF);
		for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((length >> shift) & 0xFF);
	}
	return out + body;
}

// One packet of key data at `at`: its tag and body; false where the bytes end
// early or a length cannot be known.
bool next_packet(const std::string &data, std::size_t &at, int &tag, std::string &body) {
	if (at >= data.size()) return false;
	const unsigned head = octet(data, at++);
	if ((head & 0x80) == 0) return false;
	body.clear();
	if ((head & 0x40) == 0) {
		tag = static_cast<int>((head >> 2) & 0x0F);
		const unsigned type = head & 0x03;
		if (type == 3) return false;
		const std::size_t count = type == 0 ? 1 : type == 1 ? 2 : 4;
		if (data.size() - at < count) return false;
		std::size_t length = 0;
		for (std::size_t i = 0; i < count; ++i) length = (length << 8) | octet(data, at++);
		if (data.size() - at < length) return false;
		body.assign(data, at, length);
		at += length;
		return true;
	}
	tag = static_cast<int>(head & 0x3F);
	for (;;) {
		if (at >= data.size()) return false;
		const unsigned first = octet(data, at++);
		std::size_t length = 0;
		bool partial = false;
		if (first < 192) {
			length = first;
		} else if (first < 224) {
			if (at >= data.size()) return false;
			length = ((first - 192) << 8) + octet(data, at++) + 192;
		} else if (first == 255) {
			if (data.size() - at < 4) return false;
			length = (static_cast<std::size_t>(octet(data, at)) << 24) | (static_cast<std::size_t>(octet(data, at + 1)) << 16)
				| (static_cast<std::size_t>(octet(data, at + 2)) << 8) | octet(data, at + 3);
			at += 4;
		} else {
			length = std::size_t(1) << (first & 0x1F);
			partial = true;
		}
		if (data.size() - at < length) return false;
		body.append(data, at, length);
		at += length;
		if (partial == false) return true;
	}
}

std::array<std::uint8_t, 20> sha1_of(const std::string &data) {
	std::array<std::uint8_t, 20> digest {};
	if (mbedtls_sha1(raw(data), data.size(), digest.data()) != 0) throw std::runtime_error("SHA-1 could not be computed.");
	return digest;
}

std::array<std::uint8_t, 32> sha256_of(const std::string &data) {
	std::array<std::uint8_t, 32> digest {};
	if (mbedtls_sha256(raw(data), data.size(), digest.data(), 0) != 0) throw std::runtime_error("SHA-256 could not be computed.");
	return digest;
}

// A v4 key's fingerprint: SHA-1 of 0x99, its body's length in two octets, and
// its body (RFC 9580, section 5.5.4.2).
std::array<std::uint8_t, 20> v4_fingerprint(const std::string &body) {
	std::string framed(1, static_cast<char>(0x99));
	framed += static_cast<char>((body.size() >> 8) & 0xFF);
	framed += static_cast<char>(body.size() & 0xFF);
	return sha1_of(framed + body);
}

std::optional<std::string> base64_decoded(const std::string &text) {
	std::size_t written = 0;
	std::string out(text.size(), '\0');
	if (mbedtls_base64_decode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &written, raw(text), text.size()) != 0)
		return std::nullopt;
	out.resize(written);
	return out;
}

std::string base64_encoded(const std::string &bytes) {
	std::size_t needed = 0;
	mbedtls_base64_encode(nullptr, 0, &needed, raw(bytes), bytes.size());
	std::string out(needed, '\0');
	std::size_t written = 0;
	if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &written, raw(bytes), bytes.size()) != 0)
		throw std::runtime_error("Base64 could not be written.");
	out.resize(written);
	return out;
}

// OpenPGP's checksum of armored data (RFC 9580, section 6.1).
std::uint32_t crc24_of(const std::string &data) {
	std::uint32_t crc = 0xB704CE;
	for (const char character : data) {
		crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(character)) << 16;
		for (int bit = 0; bit < 8; ++bit) {
			crc <<= 1;
			if ((crc & 0x1000000) != 0) crc ^= 0x1864CFB;
		}
	}
	return crc & 0xFFFFFF;
}

// The bytes of an armored public key block, its checksum checked where it
// carries one.
std::optional<std::string> dearmored_key(const std::string &armored) {
	std::istringstream lines(armored);
	std::string line, text, sum;
	bool inside = false, headers = false, ended = false;
	while (std::getline(lines, line)) {
		if (line.empty() == false && line.back() == '\r') line.pop_back();
		if (inside == false) {
			if (line == "-----BEGIN PGP PUBLIC KEY BLOCK-----") inside = headers = true;
			continue;
		}
		if (line == "-----END PGP PUBLIC KEY BLOCK-----") {
			ended = true;
			break;
		}
		if (headers == true) {
			if (line.find_first_not_of(" \t") == std::string::npos) headers = false;
			continue;
		}
		if (line.size() == 5 && line[0] == '=') {
			sum = line.substr(1);
			continue;
		}
		text += line;
	}
	if (ended == false || text.empty() == true) return std::nullopt;
	std::optional<std::string> binary = base64_decoded(text);
	if (binary.has_value() == false || binary->empty() == true) return std::nullopt;
	if (sum.empty() == false) {
		const std::optional<std::string> told = base64_decoded(sum);
		const std::uint32_t crc = crc24_of(*binary);
		if (told.has_value() == false || told->size() != 3 || octet(*told, 0) != ((crc >> 16) & 0xFF) || octet(*told, 1) != ((crc >> 8) & 0xFF)
				|| octet(*told, 2) != (crc & 0xFF))
			return std::nullopt;
	}
	return binary;
}

std::string address_of(const std::string &user_id) {
	const std::size_t open = user_id.rfind('<');
	const std::size_t close = user_id.rfind('>');
	if (open != std::string::npos && close != std::string::npos && close > open) return user_id.substr(open + 1, close - open - 1);
	return user_id.find('@') != std::string::npos ? user_id : std::string();
}

std::size_t kek_bytes_of(std::uint8_t cipher) {
	switch (cipher) {
		case 7: return 16;
		case 8: return 24;
		case 9: return 32;
		default: return 0;
	}
}

// What the KDF binds a wrap to (RFC 9580, section 11.5): the curve, the
// algorithm, the KDF's own parameters, "Anonymous Sender" and the recipient's
// subkey fingerprint.
std::string kdf_parameters(const Recipient &recipient) {
	std::string parameters(1, static_cast<char>(sizeof(kCurve25519Oid)));
	parameters += text_of(kCurve25519Oid, sizeof(kCurve25519Oid));
	parameters += static_cast<char>(kEcdh);
	parameters += '\x03';
	parameters += '\x01';
	parameters += static_cast<char>(recipient.kdf_hash);
	parameters += static_cast<char>(recipient.kdf_cipher);
	parameters += std::string(kAnonymousSender, sizeof(kAnonymousSender) - 1);
	parameters += text_of(recipient.subkey_fingerprint.data(), recipient.subkey_fingerprint.size());
	return parameters;
}

// One recipient's session (PKESK version 3, ECDH): a fresh X25519 key's
// public half, and the session key wrapped under what the shared secret and
// the KDF give.
std::string session_for(const Recipient &recipient, const std::array<std::uint8_t, 32> &session_key, const RandomBytes &random) {
	const std::size_t kek_bytes = kek_bytes_of(recipient.kdf_cipher);
	if (recipient.kdf_hash != kSha256 || kek_bytes == 0) throw std::runtime_error("A recipient's key asks for a key derivation this does not write.");
	std::array<std::uint8_t, 32> secret {};
	random(secret.data(), secret.size());
	std::array<std::uint8_t, 32> ephemeral {};
	crypto_x25519_public_key(ephemeral.data(), secret.data());
	std::array<std::uint8_t, 32> shared {};
	crypto_x25519(shared.data(), secret.data(), recipient.public_key.data());
	crypto_wipe(secret.data(), secret.size());
	if (std::all_of(shared.begin(), shared.end(), [](std::uint8_t byte) { return byte == 0; }) == true)
		throw std::runtime_error("A recipient's key is not one X25519 can use.");

	std::string derivation("\x00\x00\x00\x01", 4);
	derivation += text_of(shared.data(), shared.size());
	derivation += kdf_parameters(recipient);
	crypto_wipe(shared.data(), shared.size());
	std::array<std::uint8_t, 32> digest = sha256_of(derivation);
	wipe(derivation);
	std::string kek = text_of(digest.data(), kek_bytes);
	crypto_wipe(digest.data(), digest.size());

	// The algorithm, the key and its two-octet sum, padded to a multiple of
	// eight as PKCS #5 pads (RFC 9580, section 11.5).
	std::string wrapped_plain(1, static_cast<char>(kAes256));
	wrapped_plain += text_of(session_key.data(), session_key.size());
	unsigned sum = 0;
	for (const std::uint8_t byte : session_key) sum += byte;
	wrapped_plain += static_cast<char>((sum >> 8) & 0xFF);
	wrapped_plain += static_cast<char>(sum & 0xFF);
	const std::size_t padding = 8 - (wrapped_plain.size() % 8);
	wrapped_plain.append(padding, static_cast<char>(padding));
	const std::string wrapped = aes_key_wrap(kek, wrapped_plain);
	wipe(wrapped_plain);
	wipe(kek);

	std::string body(1, '\x03');
	body += text_of(recipient.subkey_fingerprint.data() + 12, 8);
	body += static_cast<char>(kEcdh);
	// The point as an MPI of 263 bits: 0x40, then the X25519 key.
	body += '\x01';
	body += '\x07';
	body += '\x40';
	body += text_of(ephemeral.data(), ephemeral.size());
	body += static_cast<char>(wrapped.size());
	body += wrapped;
	return body;
}

} // namespace

std::string fingerprint_hex(const std::array<std::uint8_t, 20> &fingerprint) {
	static const char *digits = "0123456789ABCDEF";
	std::string out;
	for (const std::uint8_t byte : fingerprint) {
		out += digits[byte >> 4];
		out += digits[byte & 0x0F];
	}
	return out;
}

std::optional<Recipient> recipient_from_key(const std::string &key, const std::string &subkey_fingerprint) {
	std::string wanted = subkey_fingerprint;
	std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	wanted.erase(std::remove(wanted.begin(), wanted.end(), ' '), wanted.end());
	std::string binary = key;
	if (key.find("-----BEGIN PGP PUBLIC KEY BLOCK-----") != std::string::npos) {
		std::optional<std::string> dearmored = dearmored_key(key);
		if (dearmored.has_value() == false) return std::nullopt;
		binary = *dearmored;
	}
	std::optional<std::string> primary;
	std::string address;
	std::optional<Recipient> chosen;
	std::uint32_t newest = 0;
	std::size_t at = 0;
	while (at < binary.size()) {
		int tag = 0;
		std::string body;
		if (next_packet(binary, at, tag, body) == false) return std::nullopt;
		if (tag == kPublicKeyTag) {
			if (primary.has_value() == true || body.empty() == true || octet(body, 0) != 4) return std::nullopt;
			primary = body;
		} else if (tag == kUserIdTag && address.empty() == true) {
			address = address_of(body);
		} else if (tag == kPublicSubkeyTag) {
			// version 4, its time, ECDH, Curve25519's OID, the point, the KDF
			if (body.size() < 6 || octet(body, 0) != 4 || octet(body, 5) != kEcdh) continue;
			std::size_t place = 6;
			if (octet(body, place) != sizeof(kCurve25519Oid) || body.size() < place + 1 + sizeof(kCurve25519Oid)
					|| std::memcmp(body.data() + place + 1, kCurve25519Oid, sizeof(kCurve25519Oid)) != 0)
				continue;
			place += 1 + sizeof(kCurve25519Oid);
			if (body.size() < place + 2 + 33 + 4) continue;
			const unsigned bits = (static_cast<unsigned>(octet(body, place)) << 8) | octet(body, place + 1);
			place += 2;
			if (bits != 263 || octet(body, place) != 0x40) continue;
			Recipient candidate;
			std::memcpy(candidate.public_key.data(), body.data() + place + 1, 32);
			place += 33;
			if (octet(body, place) != 3 || octet(body, place + 1) != 1) continue;
			candidate.kdf_hash = octet(body, place + 2);
			candidate.kdf_cipher = octet(body, place + 3);
			if (candidate.kdf_hash != kSha256 || kek_bytes_of(candidate.kdf_cipher) == 0) continue;
			candidate.subkey_fingerprint = v4_fingerprint(body);
			const std::uint32_t created = (static_cast<std::uint32_t>(octet(body, 1)) << 24) | (static_cast<std::uint32_t>(octet(body, 2)) << 16)
				| (static_cast<std::uint32_t>(octet(body, 3)) << 8) | octet(body, 4);
			const bool named = wanted.empty() == false && fingerprint_hex(candidate.subkey_fingerprint) == wanted;
			if (named == true || (wanted.empty() == true && (chosen.has_value() == false || created >= newest))) {
				chosen = candidate;
				newest = created;
			}
		}
	}
	if (primary.has_value() == false || chosen.has_value() == false) return std::nullopt;
	chosen->primary_fingerprint = fingerprint_hex(v4_fingerprint(*primary));
	chosen->address = address;
	return chosen;
}

RandomBytes secure_random() {
	return [](std::uint8_t *into, std::size_t count) { platform::fill_secure_random(into, count); };
}

std::string seal(const std::vector<Recipient> &recipients, const std::string &data, const std::string &name, std::uint32_t when,
	const RandomBytes &random) {
	if (recipients.empty() == true) throw std::invalid_argument("A sealed message needs a recipient.");
	if (!random) throw std::invalid_argument("A sealed message needs random bytes.");
	std::array<std::uint8_t, 32> session_key {};
	random(session_key.data(), session_key.size());
	std::string message;
	try {
		for (const Recipient &recipient : recipients) message += packet(kPkeskTag, session_for(recipient, session_key, random));
	} catch (...) {
		crypto_wipe(session_key.data(), session_key.size());
		throw;
	}

	// The data as a literal data packet: binary, its name and its time.
	const std::string file = name.substr(0, 255);
	std::string literal(1, 'b');
	literal += static_cast<char>(file.size());
	literal += file;
	for (int shift = 24; shift >= 0; shift -= 8) literal += static_cast<char>((when >> shift) & 0xFF);
	literal += data;

	// Sixteen random octets, their last two again, the packet, and the SHA-1
	// of all of it with the code's own two octets (RFC 9580, section 5.13.1).
	std::array<std::uint8_t, 16> prefix {};
	random(prefix.data(), prefix.size());
	std::string plain = text_of(prefix.data(), prefix.size());
	plain += static_cast<char>(prefix[14]);
	plain += static_cast<char>(prefix[15]);
	plain += packet(kLiteralTag, literal);
	wipe(literal);
	plain += "\xD3\x14";
	const std::array<std::uint8_t, 20> code = sha1_of(plain);
	plain += text_of(code.data(), code.size());

	std::string sealed(1, '\x01');
	sealed.resize(1 + plain.size());
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	unsigned char iv[16] = {};
	std::size_t offset = 0;
	const bool keyed = mbedtls_aes_setkey_enc(&aes, session_key.data(), 256) == 0;
	const bool done = keyed == true
		&& mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_ENCRYPT, plain.size(), &offset, iv, raw(plain),
			   reinterpret_cast<unsigned char *>(sealed.data()) + 1) == 0;
	mbedtls_aes_free(&aes);
	crypto_wipe(session_key.data(), session_key.size());
	wipe(plain);
	if (done == false) throw std::runtime_error("The message could not be sealed.");
	return message + packet(kSeipdTag, sealed);
}

std::string armored(const std::string &binary) {
	const std::string text = base64_encoded(binary);
	std::string out = "-----BEGIN PGP MESSAGE-----\n\n";
	for (std::size_t at = 0; at < text.size(); at += 64) out += text.substr(at, 64) + "\n";
	const std::uint32_t crc = crc24_of(binary);
	const std::string sum {static_cast<char>((crc >> 16) & 0xFF), static_cast<char>((crc >> 8) & 0xFF), static_cast<char>(crc & 0xFF)};
	return out + "=" + base64_encoded(sum) + "\n-----END PGP MESSAGE-----\n";
}

std::string aes_key_wrap(const std::string &kek, const std::string &plain) {
	if (plain.size() < 16 || plain.size() % 8 != 0 || (kek.size() != 16 && kek.size() != 24 && kek.size() != 32))
		throw std::invalid_argument("AES key wrap takes 16, 24 or 32 bytes of key and a multiple of 8 bytes, at least 16.");
	const std::size_t blocks = plain.size() / 8;
	std::uint8_t register_a[8];
	std::memset(register_a, 0xA6, sizeof(register_a));
	std::string registers = plain;
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	if (mbedtls_aes_setkey_enc(&aes, raw(kek), static_cast<unsigned>(kek.size() * 8)) != 0) {
		mbedtls_aes_free(&aes);
		throw std::runtime_error("AES key wrap could not take its key.");
	}
	unsigned char input[16];
	unsigned char output[16];
	for (std::uint64_t round = 0; round <= 5; ++round) {
		for (std::size_t index = 1; index <= blocks; ++index) {
			std::memcpy(input, register_a, 8);
			std::memcpy(input + 8, registers.data() + (index - 1) * 8, 8);
			mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);
			const std::uint64_t step = blocks * round + index;
			std::memcpy(register_a, output, 8);
			for (int byte = 0; byte < 8; ++byte) register_a[7 - byte] ^= static_cast<std::uint8_t>((step >> (8 * byte)) & 0xFF);
			std::memcpy(registers.data() + (index - 1) * 8, output + 8, 8);
		}
	}
	mbedtls_aes_free(&aes);
	crypto_wipe(input, sizeof(input));
	crypto_wipe(output, sizeof(output));
	std::string wrapped = text_of(register_a, sizeof(register_a)) + registers;
	wipe(registers);
	return wrapped;
}

} // namespace singularity::node::openpgp
