#include "Platform/DeviceKey/DeviceKey.hpp"

#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>

namespace singularity::platform {
namespace {

using Json = nlohmann::json;

constexpr const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void fill_random(std::uint8_t *bytes, std::size_t size) {
	fill_secure_random(bytes, size);
}

std::mutex &key_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::filesystem::path key_path() {
	return computer_records_directory() / "Device Key" / "device.key";
}

struct KeyPair {
	std::array<std::uint8_t, 64> secret{};
	std::array<std::uint8_t, 32> public_key{};
};

std::string bytes_of(const std::uint8_t *data, std::size_t size) {
	return std::string(reinterpret_cast<const char *>(data), size);
}

// This computer's key pair: read once, made once, per profile (the settings
// directory it lives in). The secret is kept as its 32-byte seed; the pair is
// derived from it on load.
const KeyPair &key_pair() {
	static std::map<std::filesystem::path, KeyPair> pairs;
	const std::lock_guard<std::mutex> lock(key_mutex());
	const std::filesystem::path path = key_path();
	const auto known = pairs.find(path);
	if (known != pairs.end()) return known->second;
	std::array<std::uint8_t, 32> seed{};
	bool loaded = false;
	{
		std::ifstream input(path, std::ios::binary);
		if (input) {
			const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			const Json record = Json::parse(text, nullptr, false);
			if (record.is_object() && record.value("format", std::string()) == "singularity-device-key"
					&& record.contains("seed") && record.at("seed").is_string()) {
				const std::optional<std::string> raw = base64_decode(record.at("seed").get<std::string>());
				if (raw && raw->size() == seed.size()) {
					std::copy(raw->begin(), raw->end(), seed.begin());
					loaded = true;
				}
			}
		}
	}
	if (!loaded) {
		if (std::filesystem::exists(path))
			throw std::runtime_error("This computer's signing key could not be read, so Singularity did not make another "
				"one in its place. Restore " + path.string() + " from a backup, or remove it to start a new one.");
		fill_random(seed.data(), seed.size());
		std::filesystem::create_directories(path.parent_path());
		restrict_to_owner(path.parent_path());
		const Json record{{"format", "singularity-device-key"}, {"formatVersion", 1}, {"algorithm", "ed25519"},
			{"seed", base64_encode(bytes_of(seed.data(), seed.size()))}};
		const std::filesystem::path staged = staged_sibling_path(path);
		try {
			write_file_durably(staged, record.dump());
			restrict_to_owner(staged);
			// Never replaces a key another process made a moment ago.
			commit_staged_file(staged, path, StagedCommitMode::CreateExclusive, false);
		} catch (...) {
			discard_staged_file(staged);
			crypto_wipe(seed.data(), seed.size());
			// Another Singularity process may have made it first: read theirs.
			std::ifstream input(path, std::ios::binary);
			const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			const Json record2 = Json::parse(text, nullptr, false);
			const std::optional<std::string> raw = record2.is_object() && record2.contains("seed") && record2.at("seed").is_string()
				? base64_decode(record2.at("seed").get<std::string>()) : std::nullopt;
			if (!raw || raw->size() != seed.size()) throw;
			std::copy(raw->begin(), raw->end(), seed.begin());
		}
	}
	KeyPair made;
	crypto_ed25519_key_pair(made.secret.data(), made.public_key.data(), seed.data()); // wipes the seed
	return pairs.emplace(path, made).first->second;
}

} // namespace

std::string base64_encode(const std::string &bytes) {
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	std::size_t index = 0;
	while (index + 2 < bytes.size()) {
		const std::uint32_t chunk = (static_cast<std::uint8_t>(bytes[index]) << 16)
			| (static_cast<std::uint8_t>(bytes[index + 1]) << 8) | static_cast<std::uint8_t>(bytes[index + 2]);
		out += kAlphabet[(chunk >> 18) & 63];
		out += kAlphabet[(chunk >> 12) & 63];
		out += kAlphabet[(chunk >> 6) & 63];
		out += kAlphabet[chunk & 63];
		index += 3;
	}
	const std::size_t left = bytes.size() - index;
	if (left == 1) {
		const std::uint32_t chunk = static_cast<std::uint8_t>(bytes[index]) << 16;
		out += kAlphabet[(chunk >> 18) & 63];
		out += kAlphabet[(chunk >> 12) & 63];
		out += "==";
	} else if (left == 2) {
		const std::uint32_t chunk = (static_cast<std::uint8_t>(bytes[index]) << 16)
			| (static_cast<std::uint8_t>(bytes[index + 1]) << 8);
		out += kAlphabet[(chunk >> 18) & 63];
		out += kAlphabet[(chunk >> 12) & 63];
		out += kAlphabet[(chunk >> 6) & 63];
		out += '=';
	}
	return out;
}

std::optional<std::string> base64_decode(const std::string &text) {
	if (text.size() % 4 != 0) return std::nullopt;
	const auto value_of = [](char character) -> int {
		if (character >= 'A' && character <= 'Z') return character - 'A';
		if (character >= 'a' && character <= 'z') return character - 'a' + 26;
		if (character >= '0' && character <= '9') return character - '0' + 52;
		if (character == '+') return 62;
		if (character == '/') return 63;
		return -1;
	};
	std::string out;
	out.reserve(text.size() / 4 * 3);
	for (std::size_t index = 0; index < text.size(); index += 4) {
		const bool last = index + 4 == text.size();
		int values[4];
		int padding = 0;
		for (int offset = 0; offset < 4; ++offset) {
			const char character = text[index + offset];
			if (character == '=' && last && offset >= 2) {
				values[offset] = 0;
				++padding;
				continue;
			}
			if (padding > 0) return std::nullopt;
			values[offset] = value_of(character);
			if (values[offset] < 0) return std::nullopt;
		}
		const std::uint32_t chunk = (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
		out += static_cast<char>((chunk >> 16) & 0xFF);
		if (padding < 2) out += static_cast<char>((chunk >> 8) & 0xFF);
		if (padding < 1) out += static_cast<char>(chunk & 0xFF);
		// Canonical only: the bits padding hides must be zero.
		if (padding == 2 && (chunk & 0xFFFF) != 0) return std::nullopt;
		if (padding == 1 && (chunk & 0xFF) != 0) return std::nullopt;
	}
	return out;
}

std::string device_public_key() {
	const KeyPair &pair = key_pair();
	return base64_encode(bytes_of(pair.public_key.data(), pair.public_key.size()));
}

std::string sign_with_device_key(const std::string &message) {
	const KeyPair &pair = key_pair();
	std::array<std::uint8_t, 64> signature{};
	crypto_ed25519_sign(signature.data(), pair.secret.data(), reinterpret_cast<const std::uint8_t *>(message.data()),
		message.size());
	return base64_encode(bytes_of(signature.data(), signature.size()));
}

bool verify_device_signature(const std::string &public_key, const std::string &message, const std::string &signature) {
	const std::optional<std::string> key = base64_decode(public_key);
	const std::optional<std::string> signed_bytes = base64_decode(signature);
	if (!key || key->size() != 32 || !signed_bytes || signed_bytes->size() != 64) return false;
	return crypto_ed25519_check(reinterpret_cast<const std::uint8_t *>(signed_bytes->data()),
		reinterpret_cast<const std::uint8_t *>(key->data()), reinterpret_cast<const std::uint8_t *>(message.data()),
		message.size()) == 0;
}

std::string device_key_code(const std::string &public_key) {
	const std::optional<std::string> key = base64_decode(public_key);
	if (!key || key->size() != 32) return std::string();
	std::string digest = sha256_hex_digest(*key).substr(0, 12);
	for (char &character : digest) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	return digest.substr(0, 4) + "-" + digest.substr(4, 4) + "-" + digest.substr(8, 4);
}

} // namespace singularity::platform
