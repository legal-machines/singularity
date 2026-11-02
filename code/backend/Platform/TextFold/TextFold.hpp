#pragma once

#include <utf8proc.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace singularity::platform {

// Text compared the way a person reads it: Unicode case folding and composition
// in every script (utf8proc), optionally without diacritics. Matching policy
// (which forms count as equal) stays with each caller. Text that is not valid
// UTF-8 falls back to ASCII lower case.
inline std::string folded_text(const std::string &text, bool strip_marks = false) {
	utf8proc_uint8_t *mapped = nullptr;
	int options = UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD;
	if (strip_marks) options |= UTF8PROC_STRIPMARK;
	const utf8proc_ssize_t length = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
		static_cast<utf8proc_ssize_t>(text.size()), &mapped, static_cast<utf8proc_option_t>(options));
	std::string result;
	if (length >= 0 && mapped != nullptr) {
		result.assign(reinterpret_cast<const char *>(mapped), static_cast<std::size_t>(length));
	} else {
		result = text;
		for (char &character : result)
			if (static_cast<unsigned char>(character) < 0x80)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	if (mapped != nullptr) std::free(mapped);
	return result;
}

// The text in lower case with every letter in as many bytes as before, so a
// place found in it stands at the same offset in the text: utf8proc's own
// mapping (never a table of our own), kept where the lower-case letter is
// written in as many bytes as the original - Latin, Cyrillic, Greek and most
// scripts - and the letter left as written where it is not. Bytes that are not
// valid UTF-8 are kept as they are. What a matcher needs to find a name
// however it is cased without moving any offset.
inline std::string same_length_lowered(const std::string &text) {
	std::string result = text;
	std::size_t at = 0;
	while (at < text.size()) {
		const auto byte = static_cast<unsigned char>(text[at]);
		if (byte < 0x80) {
			if (byte >= 'A' && byte <= 'Z') result[at] = static_cast<char>(byte - 'A' + 'a');
			++at;
			continue;
		}
		utf8proc_int32_t codepoint = 0;
		const utf8proc_ssize_t length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + at),
			static_cast<utf8proc_ssize_t>(text.size() - at), &codepoint);
		if (length <= 0) {
			++at;
			continue;
		}
		const utf8proc_int32_t lower = utf8proc_tolower(codepoint);
		if (lower != codepoint) {
			utf8proc_uint8_t encoded[4];
			if (utf8proc_encode_char(lower, encoded) == length)
				for (utf8proc_ssize_t index = 0; index < length; ++index) result[at + static_cast<std::size_t>(index)] = static_cast<char>(encoded[index]);
		}
		at += static_cast<std::size_t>(length);
	}
	return result;
}

// The same text with its first letter written as a capital, in whatever script
// it is written in (utf8proc, never a table of our own). A chat names itself
// from a model's reply, and a name in a list reads as a name only when it opens
// with a capital. Text that is not valid UTF-8, or that does not open with a
// letter, is returned exactly as it came.
inline std::string with_initial_capital(const std::string &text) {
	if (text.empty()) return text;
	utf8proc_int32_t codepoint = 0;
	const utf8proc_ssize_t read = utf8proc_iterate(
		reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
		static_cast<utf8proc_ssize_t>(text.size()), &codepoint);
	if (read <= 0 || codepoint < 0) return text;
	const utf8proc_int32_t capital = utf8proc_toupper(codepoint);
	if (capital == codepoint) return text;
	utf8proc_uint8_t encoded[4] = {0, 0, 0, 0};
	const utf8proc_ssize_t written = utf8proc_encode_char(capital, encoded);
	if (written <= 0) return text;
	std::string result(reinterpret_cast<const char *>(encoded), static_cast<std::size_t>(written));
	result.append(text, static_cast<std::size_t>(read), std::string::npos);
	return result;
}

} // namespace singularity::platform
