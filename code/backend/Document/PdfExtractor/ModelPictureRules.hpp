#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

// When a picture a member attached goes to a model as it is, decided the same
// way on every system (D-256). Every provider here takes PNG, JPEG, GIF and
// WebP; Anthropic, the strictest, takes at most 5 MB of base64 a picture,
// which 3.75 MB of bytes make, and no side longer than 8000 pixels. A picture
// that is none of these, or larger, or turned by its file rather than in its
// pixels, is redrawn where the system can (PdfExtractor's picture_for_model).
// What a picture is is read from its bytes, never from its file's name.
namespace singularity::documents {

// A picture as a model reads it (D-256).
struct ModelPicture {
	// "image/png", "image/jpeg", "image/gif" or "image/webp".
	std::string media_type;
	std::string bytes;
};

} // namespace singularity::documents

namespace singularity::documents::picture {

inline constexpr std::size_t kMostBytesAsItIs = 3932160;
inline constexpr std::uint32_t kMostEdgeAsItIs = 8000;
// A redrawn picture's long edge: as much as any provider reads (Anthropic
// reads up to 1568 pixels, OpenAI up to 2048), and within Anthropic's limit
// for a request that carries many pictures (2000).
inline constexpr std::uint32_t kRedrawnLongEdge = 2000;
// JPEG's quality for a redrawn photograph: no loss a reader sees.
inline constexpr double kRedrawnJpegQuality = 0.85;

struct PictureFacts {
	// "image/png", "image/jpeg", "image/gif" or "image/webp"; empty for any
	// other picture, or bytes that are none.
	std::string media_type;
	// 0 where the bytes do not say.
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// The EXIF orientation of a JPEG (1 upright, 2 to 8 turned or mirrored);
	// 0 where the file names none.
	int orientation = 0;
};

namespace detail {

inline std::uint32_t big_endian(const std::string &bytes, std::size_t at, std::size_t count) {
	std::uint32_t value = 0;
	for (std::size_t index = 0; index < count; ++index)
		value = (value << 8) | static_cast<unsigned char>(bytes[at + index]);
	return value;
}

inline std::uint32_t little_endian(const std::string &bytes, std::size_t at, std::size_t count) {
	std::uint32_t value = 0;
	for (std::size_t index = count; index > 0; --index)
		value = (value << 8) | static_cast<unsigned char>(bytes[at + index - 1]);
	return value;
}

// A JPEG's EXIF orientation, from its APP1 segment's first image directory;
// 0 where it names none.
inline int exif_orientation(const std::string &bytes, std::size_t at, std::size_t length) {
	// "Exif\0\0", then a TIFF header: "II" or "MM", 42, the directory's offset.
	if (length < 14 || bytes.compare(at, 6, std::string("Exif\0\0", 6)) != 0) return 0;
	const std::size_t tiff = at + 6;
	const std::size_t end = at + length;
	const bool isLittle = bytes.compare(tiff, 2, "II") == 0;
	if (!isLittle && bytes.compare(tiff, 2, "MM") != 0) return 0;
	const auto read = [&](std::size_t from, std::size_t count) {
		return isLittle ? little_endian(bytes, from, count) : big_endian(bytes, from, count);
	};
	const std::size_t directory = tiff + read(tiff + 4, 4);
	if (directory + 2 > end) return 0;
	const std::size_t entries = read(directory, 2);
	for (std::size_t index = 0; index < entries; ++index) {
		const std::size_t entry = directory + 2 + index * 12;
		if (entry + 12 > end) return 0;
		if (read(entry, 2) == 0x0112) return static_cast<int>(read(entry + 8, 2));
	}
	return 0;
}

} // namespace detail

// What `bytes` are as a picture: its kind where every provider takes it, its
// size in pixels, and how a JPEG's file says it is turned.
inline PictureFacts facts_of(const std::string &bytes) {
	PictureFacts facts;
	const std::size_t size = bytes.size();
	if (size >= 24 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
		facts.media_type = "image/png";
		if (bytes.compare(12, 4, "IHDR") == 0) {
			facts.width = detail::big_endian(bytes, 16, 4);
			facts.height = detail::big_endian(bytes, 20, 4);
		}
	} else if (size >= 10 && (bytes.compare(0, 6, "GIF87a") == 0 || bytes.compare(0, 6, "GIF89a") == 0)) {
		facts.media_type = "image/gif";
		facts.width = detail::little_endian(bytes, 6, 2);
		facts.height = detail::little_endian(bytes, 8, 2);
	} else if (size >= 30 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) {
		facts.media_type = "image/webp";
		if (bytes.compare(12, 4, "VP8X") == 0) {
			facts.width = detail::little_endian(bytes, 24, 3) + 1;
			facts.height = detail::little_endian(bytes, 27, 3) + 1;
		} else if (bytes.compare(12, 4, "VP8 ") == 0 && static_cast<unsigned char>(bytes[23]) == 0x9d
				&& static_cast<unsigned char>(bytes[24]) == 0x01 && static_cast<unsigned char>(bytes[25]) == 0x2a) {
			facts.width = detail::little_endian(bytes, 26, 2) & 0x3fff;
			facts.height = detail::little_endian(bytes, 28, 2) & 0x3fff;
		} else if (bytes.compare(12, 4, "VP8L") == 0 && static_cast<unsigned char>(bytes[20]) == 0x2f) {
			const std::uint32_t bits = detail::little_endian(bytes, 21, 4);
			facts.width = (bits & 0x3fff) + 1;
			facts.height = ((bits >> 14) & 0x3fff) + 1;
		}
	} else if (size >= 4 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xd8
			&& static_cast<unsigned char>(bytes[2]) == 0xff) {
		facts.media_type = "image/jpeg";
		// Its segments, to the frame that gives its size.
		std::size_t at = 2;
		while (at + 4 <= size && static_cast<unsigned char>(bytes[at]) == 0xff) {
			const unsigned char marker = static_cast<unsigned char>(bytes[at + 1]);
			if (marker == 0xff) {
				++at;
				continue;
			}
			if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
				at += 2;
				continue;
			}
			if (marker == 0xd9 || marker == 0xda) break;
			const std::size_t length = detail::big_endian(bytes, at + 2, 2);
			if (length < 2 || at + 2 + length > size) break;
			if (marker == 0xe1 && facts.orientation == 0)
				facts.orientation = detail::exif_orientation(bytes, at + 4, length - 2);
			const bool isFrame = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
			if (isFrame && length >= 7) {
				facts.height = detail::big_endian(bytes, at + 5, 2);
				facts.width = detail::big_endian(bytes, at + 7, 2);
				break;
			}
			at += 2 + length;
		}
	}
	return facts;
}

// Whether `bytes` go to a model as they are: a kind every provider takes, no
// larger than every provider takes, and upright in its pixels.
inline bool goes_as_it_is(const std::string &bytes, const PictureFacts &facts) {
	return facts.media_type.empty() == false && bytes.size() <= kMostBytesAsItIs
		&& facts.width <= kMostEdgeAsItIs && facts.height <= kMostEdgeAsItIs
		&& (facts.orientation == 0 || facts.orientation == 1);
}

// The picture file as it is, where it goes so; nothing where it must be
// redrawn, or cannot be read.
inline std::optional<ModelPicture> as_it_is(const std::filesystem::path &path) {
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size == 0 || size > kMostBytesAsItIs) return std::nullopt;
	std::ifstream input(path, std::ios::binary);
	if (!input) return std::nullopt;
	std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	const PictureFacts facts = facts_of(bytes);
	if (goes_as_it_is(bytes, facts) == false) return std::nullopt;
	return ModelPicture{facts.media_type, std::move(bytes)};
}

} // namespace singularity::documents::picture
