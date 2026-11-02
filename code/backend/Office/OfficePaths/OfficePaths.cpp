#include "Office/OfficePaths/OfficePaths.hpp"

#include <mbedtls/base64.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace singularity::office {
namespace {

std::string folded(std::string text) {
#if defined(_WIN32) || defined(__APPLE__)
	// The file systems Office runs on compare names without case.
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
#endif
	return text;
}

std::filesystem::path settled(const std::filesystem::path &path) {
	std::error_code error;
	// The real place, through any link or junction a folder is reached by.
	const auto real = std::filesystem::weakly_canonical(path, error);
	return error ? path.lexically_normal() : real;
}

int hex_value(char character) {
	if (character >= '0' && character <= '9') return character - '0';
	if (character >= 'a' && character <= 'f') return character - 'a' + 10;
	if (character >= 'A' && character <= 'F') return character - 'A' + 10;
	return -1;
}

} // namespace

std::string path_of_location(const std::string &location) {
	if (location.rfind("file://", 0) != 0) return location;
	std::string rest = location.substr(std::string("file://").size());
	if (rest.rfind("localhost/", 0) == 0) rest = rest.substr(std::string("localhost").size());
	std::string decoded;
	for (std::size_t index = 0; index < rest.size(); ++index) {
		if (rest[index] == '%' && index + 2 < rest.size() && hex_value(rest[index + 1]) >= 0 && hex_value(rest[index + 2]) >= 0) {
			decoded += static_cast<char>(hex_value(rest[index + 1]) * 16 + hex_value(rest[index + 2]));
			index += 2;
		} else {
			decoded += rest[index];
		}
	}
#if defined(_WIN32)
	// "/C:/Users/..." is "C:/Users/..."; "//server/share" stays a share.
	if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(0, 1);
#endif
	return decoded;
}

std::optional<std::string> relative_inside(const std::filesystem::path &folder, const std::filesystem::path &file) {
	if (folder.empty() || file.empty()) return std::nullopt;
	std::string base = settled(folder).generic_u8string();
	if (base.empty()) return std::nullopt;
	if (base.back() != '/') base += '/';
	const std::string whole = settled(file).generic_u8string();
	if (whole.size() <= base.size()) return std::nullopt;
	if (folded(whole).rfind(folded(base), 0) != 0) return std::nullopt;
	return whole.substr(base.size());
}

bool same_place(const std::string &one, const std::string &other) {
	return folded(std::filesystem::u8path(one).generic_u8string()) == folded(std::filesystem::u8path(other).generic_u8string());
}

std::string safe_file_name(const std::string &name) {
	std::string safe;
	for (const unsigned char character : name) {
		if (character < 0x20 || std::string("<>:\"/\\|?*").find(static_cast<char>(character)) != std::string::npos) {
			safe += ' ';
		} else {
			safe += static_cast<char>(character);
		}
	}
	const auto first = safe.find_first_not_of(" .");
	const auto last = safe.find_last_not_of(" .");
	safe = first == std::string::npos ? std::string() : safe.substr(first, last - first + 1);
	if (safe.empty()) safe = "Attachment";
	if (safe.size() > 180) {
		const auto dot = safe.find_last_of('.');
		const std::string extension = dot != std::string::npos && safe.size() - dot <= 10 ? safe.substr(dot) : std::string();
		safe = safe.substr(0, 180 - extension.size()) + extension;
	}
	return safe;
}

std::filesystem::path free_path_in(const std::filesystem::path &folder, const std::string &name) {
	const std::filesystem::path wanted = std::filesystem::u8path(name);
	std::error_code error;
	if (!std::filesystem::exists(folder / wanted, error)) return folder / wanted;
	const std::string stem = wanted.stem().u8string();
	const std::string extension = wanted.extension().u8string();
	for (int number = 2; number < 1000; ++number) {
		const auto candidate = folder / std::filesystem::u8path(stem + " (" + std::to_string(number) + ")" + extension);
		if (!std::filesystem::exists(candidate, error)) return candidate;
	}
	throw std::runtime_error("The Matter's folder already holds too many files of that name.");
}

std::string encode_base64(const std::string &bytes) {
	std::size_t length = 0;
	(void)mbedtls_base64_encode(nullptr, 0, &length, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
	std::string text(length, '\0');
	if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(text.data()), text.size(), &length,
			reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) != 0) {
		return {};
	}
	text.resize(length);
	return text;
}

std::optional<std::string> decode_base64(const std::string &text) {
	std::size_t length = 0;
	const int sized = mbedtls_base64_decode(nullptr, 0, &length, reinterpret_cast<const unsigned char *>(text.data()), text.size());
	if (sized == MBEDTLS_ERR_BASE64_INVALID_CHARACTER) return std::nullopt;
	std::string bytes(length, '\0');
	if (mbedtls_base64_decode(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size(), &length,
			reinterpret_cast<const unsigned char *>(text.data()), text.size()) != 0) {
		return std::nullopt;
	}
	bytes.resize(length);
	return bytes;
}

} // namespace singularity::office
