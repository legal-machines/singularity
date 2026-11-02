#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// A file the interface built itself - a kept result as a Word document, a
// table as an Excel workbook - saved where the person chooses in the system's
// own Save dialog.
//
// The page never names a path: it supplies the bytes and a suggested name, and
// the person chooses the place. Only the two office formats the viewers
// produce are accepted, and the bytes must be the package they claim to be.
// Header-only on purpose: pure validation and one file write, nothing to link.
namespace singularity::bridge::generated_file_bridge {

struct PreparedFile {
	std::string bytes;
	std::string default_name;
	std::string filter_name;
	std::string extension;
};

inline constexpr std::size_t kMaximumEncodedBytes = 48U * 1024U * 1024U;

// Strict base64 (RFC 4648, with padding, no line breaks). std::nullopt for
// anything else, so a malformed payload is never half-written to disk.
inline std::optional<std::string> decode_base64(std::string_view text) {
	if (text.size() % 4 != 0) return std::nullopt;
	std::array<std::int8_t, 256> values{};
	values.fill(-1);
	constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (std::size_t index = 0; index < alphabet.size(); ++index) {
		values[static_cast<unsigned char>(alphabet[index])] = static_cast<std::int8_t>(index);
	}

	std::string result;
	result.reserve(text.size() / 4 * 3);
	for (std::size_t offset = 0; offset < text.size(); offset += 4) {
		const bool isLast = offset + 4 == text.size();
		std::uint32_t group = 0;
		int padding = 0;
		for (std::size_t index = 0; index < 4; ++index) {
			const char symbol = text[offset + index];
			if (symbol == '=') {
				// Padding only closes the last group, in its last one or two places.
				if (isLast == false || index < 2) return std::nullopt;
				++padding;
				group <<= 6;
				continue;
			}
			if (padding > 0) return std::nullopt;
			const std::int8_t value = values[static_cast<unsigned char>(symbol)];
			if (value < 0) return std::nullopt;
			group = (group << 6) | static_cast<std::uint32_t>(value);
		}
		result.push_back(static_cast<char>((group >> 16) & 0xFF));
		if (padding < 2) result.push_back(static_cast<char>((group >> 8) & 0xFF));
		if (padding < 1) result.push_back(static_cast<char>(group & 0xFF));
	}
	return result;
}

// The suggested file name: what the page proposed, without anything a file
// system reads as a path or refuses in a name.
inline std::string suggested_name(std::string_view proposed, std::string_view fallback) {
	std::string name;
	for (const char symbol : proposed) {
		const unsigned char byte = static_cast<unsigned char>(symbol);
		const bool isRefused = byte < 0x20 || byte == 0x7F || symbol == '/' || symbol == '\\' || symbol == ':'
			|| symbol == '*' || symbol == '?' || symbol == '"' || symbol == '<' || symbol == '>' || symbol == '|';
		name.push_back(isRefused == true ? ' ' : symbol);
	}
	while (name.empty() == false && (name.back() == ' ' || name.back() == '.')) name.pop_back();
	std::size_t start = 0;
	while (start < name.size() && (name[start] == ' ' || name[start] == '.')) ++start;
	name.erase(0, start);
	// Cut on a character boundary: a UTF-8 continuation byte is never the last one kept.
	constexpr std::size_t maximum = 120;
	if (name.size() > maximum) {
		std::size_t end = maximum;
		while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80) --end;
		name.resize(end);
	}
	return name.empty() == true ? std::string(fallback) : name;
}

inline PreparedFile prepare(const nlohmann::json& params) {
	const std::string extension = params.at("extension").get<std::string>();
	PreparedFile prepared;
	std::string fallback;
	if (extension == "docx") {
		prepared.filter_name = "Word document";
		fallback = "Document";
	} else if (extension == "xlsx") {
		prepared.filter_name = "Excel workbook";
		fallback = "Table";
	} else {
		throw std::runtime_error("Singularity saves Word documents and Excel workbooks only.");
	}
	prepared.extension = extension;

	const std::string& encoded = params.at("contentBase64").get_ref<const std::string&>();
	if (encoded.empty() == true) throw std::runtime_error("There is nothing to save.");
	if (encoded.size() > kMaximumEncodedBytes) throw std::runtime_error("This file is too large to save from here.");
	std::optional<std::string> bytes = decode_base64(encoded);
	// Both formats are ZIP packages: anything else is not a file the viewers made.
	if (bytes == std::nullopt || bytes->size() < 4 || bytes->compare(0, 4, "PK\x03\x04") != 0) {
		throw std::runtime_error("Singularity could not prepare this file.");
	}
	prepared.bytes = std::move(*bytes);
	prepared.default_name = suggested_name(params.value("name", std::string()), fallback) + "." + extension;
	return prepared;
}

// Cancelling the dialog resolves false, not an error.
inline nlohmann::json finish(const PreparedFile& prepared, const std::optional<std::string>& selected_path) {
	if (selected_path == std::nullopt || selected_path->empty() == true) return false;
	std::ofstream file(std::filesystem::path(*selected_path), std::ios::binary | std::ios::trunc);
	if (!file) throw std::runtime_error("Could not write the file.");
	file.write(prepared.bytes.data(), static_cast<std::streamsize>(prepared.bytes.size()));
	file.flush();
	if (!file) throw std::runtime_error("Could not write the file.");
	return true;
}

} // namespace singularity::bridge::generated_file_bridge
