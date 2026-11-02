#pragma once

#include <utf8proc.h>

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::platform {

// Whether a phrase a model wrote is the same phrase a document holds.
//
// This is the ONE place that decides it. Citation checking asks it whether a
// quotation stands in a passage; document editing asks it where a change is
// anchored. Neither may have a matcher of its own: an anchor that resolves for
// one and not for the other is how a lawyer is told a quotation is unverified,
// or loses a change they were told was made.
//
// What never decides: typography. A model reflows what it quotes and re-marks
// the quotation marks inside it; a word processor writes a no-break space
// between a party and the word after it, an en dash where a person typed a
// hyphen, and a curly apostrophe in a possessive. So runs of whitespace count
// as one space, every kind of quotation mark and apostrophe counts as the same
// character, every kind of dash as the same dash, and case is ignored.
//
// What always decides: the words. They must be the text's own, in order, and
// whole: a phrase that begins or ends with a letter or a figure stands only
// where the text's word or number begins or ends there too. "exclusive" is not
// in "nonexclusive", "$1,000" is not in "$1,000,000" and "30 days" is not in
// "130 days" - a quotation verified, or a change anchored, inside another word
// or figure says something the document does not. Case is ignored in every
// script (utf8proc), not only in ASCII.
// Nothing here ever changes what is written - a caller is given the exact bytes
// of the text it searched, and edits or quotes those.

namespace phrase_match_detail {

inline bool is_space(char32_t character) {
	return character == U' ' || character == U'\t' || character == U'\n' || character == U'\r';
}

struct Folded {
	// The character as compared: ASCII folded, a quotation mark or a dash as
	// its one kind, any other letter in its lower case (a Unicode code point).
	char32_t character = 0;
	std::size_t bytes = 1;
};

inline Folded fold_at(const std::string &text, std::size_t at) {
	const auto byte = static_cast<unsigned char>(text[at]);
	if (byte < 0x80) {
		if (byte == '"' || byte == '\'' || byte == '`') return {U'\'', 1};
		if (byte == '-') return {U'-', 1};
		return {static_cast<char32_t>(std::tolower(byte)), 1};
	}
	// The punctuation a word processor and a model actually produce.
	static const std::vector<std::pair<std::string, char>> punctuation{
		{"\xe2\x80\x98", '\''}, {"\xe2\x80\x99", '\''}, {"\xe2\x80\x9a", '\''}, {"\xe2\x80\x9b", '\''},
		{"\xe2\x80\x9c", '\''}, {"\xe2\x80\x9d", '\''}, {"\xe2\x80\x9e", '\''}, {"\xe2\x80\x9f", '\''},
		{"\xc2\xab", '\''}, {"\xc2\xbb", '\''}, {"\xe2\x80\xb2", '\''}, {"\xe2\x80\xb3", '\''},
		{"\xe2\x80\x90", '-'}, {"\xe2\x80\x91", '-'}, {"\xe2\x80\x92", '-'}, {"\xe2\x80\x93", '-'},
		{"\xe2\x80\x94", '-'}, {"\xe2\x80\x95", '-'}, {"\xc2\xa0", ' '},
	};
	for (const auto &[bytes, folded] : punctuation)
		if (text.compare(at, bytes.size(), bytes) == 0) return {static_cast<char32_t>(folded), bytes.size()};
	utf8proc_int32_t codepoint = 0;
	const utf8proc_ssize_t length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + at),
		static_cast<utf8proc_ssize_t>(text.size() - at), &codepoint);
	if (length <= 0) return {static_cast<char32_t>(0x110000 + byte), 1};
	return {static_cast<char32_t>(utf8proc_tolower(codepoint)), static_cast<std::size_t>(length)};
}



// A no-break space is a space: judging by what the character IS, not by how
// many bytes hold it, is what lets the same phrase be recognised either way.
inline bool space_at(const std::string &text, std::size_t at) {
	return is_space(fold_at(text, at).character);
}

// Whether the code point that begins at `at` is a letter, a figure or a mark
// (a word's own character, in any script).
inline bool word_character_at(const std::string &text, std::size_t at) {
	if (at >= text.size()) return false;
	const auto byte = static_cast<unsigned char>(text[at]);
	if (byte < 0x80) return std::isalnum(byte) != 0;
	utf8proc_int32_t codepoint = 0;
	if (utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + at),
			static_cast<utf8proc_ssize_t>(text.size() - at), &codepoint) <= 0)
		return false;
	switch (utf8proc_category(codepoint)) {
		case UTF8PROC_CATEGORY_LU: case UTF8PROC_CATEGORY_LL: case UTF8PROC_CATEGORY_LT: case UTF8PROC_CATEGORY_LM:
		case UTF8PROC_CATEGORY_LO: case UTF8PROC_CATEGORY_MN: case UTF8PROC_CATEGORY_MC: case UTF8PROC_CATEGORY_ND:
		case UTF8PROC_CATEGORY_NL: case UTF8PROC_CATEGORY_NO:
			return true;
		default:
			return false;
	}
}

// Where the code point that ends just before `at` begins.
inline std::size_t previous_character(const std::string &text, std::size_t at) {
	std::size_t start = at - 1;
	while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
	return start;
}

inline bool digit_at(const std::string &text, std::size_t at) {
	return at < text.size() && text[at] >= '0' && text[at] <= '9';
}

// Whether a match of `phrase` at text[start, end) stands whole: where the
// phrase begins (ends) with a word's character, the text's word begins (ends)
// there too, and a figure never stops short of the rest of its number - a
// thousands separator or a decimal point with figures after it, but for a
// zero fraction (".00").
inline bool stands_whole(const std::string &text, std::size_t start, std::size_t end, const std::string &phrase) {
	std::size_t first = 0;
	while (first < phrase.size() && space_at(phrase, first)) first += fold_at(phrase, first).bytes;
	if (first < phrase.size() && word_character_at(phrase, first) && start > 0) {
		const std::size_t before = previous_character(text, start);
		if (word_character_at(text, before)) return false;
		// "000 days" is not in "1,000 days".
		if (digit_at(phrase, first) && (text[before] == ',' || text[before] == '.') && before > 0 && digit_at(text, before - 1))
			return false;
	}
	if (phrase.empty() || end >= text.size()) return true;
	std::size_t last = phrase.size();
	while (last > 0 && space_at(phrase, previous_character(phrase, last))) last = previous_character(phrase, last);
	if (last == 0) return true;
	const std::size_t lastStart = previous_character(phrase, last);
	if (!word_character_at(phrase, lastStart)) return true;
	if (word_character_at(text, end)) return false;
	if (digit_at(phrase, lastStart) && (text[end] == ',' || text[end] == '.') && digit_at(text, end + 1)) {
		const bool zeroFraction = text[end] == '.' && text.compare(end + 1, 2, "00") == 0 && !digit_at(text, end + 3);
		if (!zeroFraction) return false;
	}
	return true;
}

} // namespace phrase_match_detail

// Where `phrase` stands in `text` at or after `from`, as the exact bytes of
// `text`. Nothing is returned when it does not stand there.
[[nodiscard]] inline std::optional<std::pair<std::size_t, std::size_t>> find_phrase(
		const std::string &text, const std::string &phrase, std::size_t from = 0) {
	using namespace phrase_match_detail;
	if (phrase.empty()) return std::nullopt;
	for (std::size_t start = from; start < text.size(); ++start) {
		if (space_at(text, start)) continue;
		std::size_t here = start;
		std::size_t there = 0;
		bool matched = true;
		while (there < phrase.size()) {
			if (space_at(phrase, there)) {
				// One run of whitespace in the phrase matches one run in the text -
				// and a run the text does not have at all fails, because that would
				// join two different words.
				while (there < phrase.size() && space_at(phrase, there)) there += fold_at(phrase, there).bytes;
				if (here >= text.size() || !space_at(text, here)) { matched = false; break; }
				while (here < text.size() && space_at(text, here)) here += fold_at(text, here).bytes;
				continue;
			}
			if (here >= text.size()) { matched = false; break; }
			const Folded left = fold_at(text, here);
			const Folded right = fold_at(phrase, there);
			if (left.character != right.character) { matched = false; break; }
			here += left.bytes;
			there += right.bytes;
		}
		if (matched && stands_whole(text, start, here, phrase)) return std::make_pair(start, here - start);
	}
	return std::nullopt;
}

struct PhrasePlacement {
	std::size_t start = 0;
	std::size_t length = 0;
	// How many places the phrase stands at. A change may only be anchored, and
	// a quotation only repaired, where this is exactly one.
	std::size_t matches = 0;
};

// Where `phrase` stands in `text`, and how many places it stands at. Counting
// stops at two: nothing downstream may act on an ambiguous phrase, so knowing
// there is more than one place is the whole answer.
[[nodiscard]] inline PhrasePlacement locate_phrase(const std::string &text, const std::string &phrase) {
	PhrasePlacement placement;
	if (phrase.empty()) return placement;
	std::size_t from = 0;
	while (true) {
		const auto at = find_phrase(text, phrase, from);
		if (!at) break;
		if (placement.matches == 0) { placement.start = at->first; placement.length = at->second; }
		++placement.matches;
		if (placement.matches > 1) break;
		from = at->first + (at->second > 0 ? at->second : 1);
	}
	return placement;
}

// Whether two pieces of text SAY the same thing, judged the way everything
// else here is judged: typography never decides, the words do. Used where a
// caller must know that a value it was given is the value a document holds -
// a table cell a model quotes back, for instance, which it will have
// reproduced with its own quotation marks and spacing.
// `equivalent_writings` groups the names that ARE the same thing: a party the
// Project knows as "Meridia", "Meridia Clinical Systems" and "Meridia Clinical
// Systems, Inc." A model never sees a document's own bytes - it sees protected
// text, and what it quotes back is restored - so the writing it uses for a
// party is often another writing of the same party. Where that is the only
// difference, the two texts say the same thing, and a change anchored to one of
// them belongs to the other. The WORDS still decide: only writings the Project
// itself holds for one identity are ever treated as interchangeable.
[[nodiscard]] inline bool phrases_equal(const std::string &left, const std::string &right,
		const std::vector<std::vector<std::string>> &equivalent_writings = {}) {
	using namespace phrase_match_detail;
	std::size_t here = 0;
	std::size_t there = 0;
	const auto skipSpace = [](const std::string &text, std::size_t &at) {
		while (at < text.size() && space_at(text, at)) at += fold_at(text, at).bytes;
	};
	// Whether `text` holds `phrase` exactly at `at`, typography aside; returns
	// how many bytes of `text` it took.
	const auto writingAt = [](const std::string &text, std::size_t at, const std::string &phrase) -> std::size_t {
		std::size_t here = at;
		std::size_t there = 0;
		while (there < phrase.size()) {
			if (space_at(phrase, there)) {
				while (there < phrase.size() && space_at(phrase, there)) there += fold_at(phrase, there).bytes;
				if (here >= text.size() || !space_at(text, here)) return 0;
				while (here < text.size() && space_at(text, here)) here += fold_at(text, here).bytes;
				continue;
			}
			if (here >= text.size()) return 0;
			const Folded a = fold_at(text, here);
			const Folded b = fold_at(phrase, there);
			if (a.character != b.character) return 0;
			here += a.bytes;
			there += b.bytes;
		}
		return here - at;
	};
	skipSpace(left, here);
	skipSpace(right, there);
	while (here < left.size() && there < right.size()) {
		// A party first: where both texts name the same identity here - each in
		// its own writing - both names are consumed and the comparison goes on
		// after them. Tried BEFORE the characters are compared, because two
		// writings of one party agree at their start ("Meridia", "Meridia
		// Clinical Systems") and diverge only afterwards.
		bool substituted = false;
		for (const std::vector<std::string> &group : equivalent_writings) {
			std::size_t leftTook = 0;
			std::size_t rightTook = 0;
			for (const std::string &writing : group) {
				const std::size_t took = writingAt(left, here, writing);
				if (took > leftTook) leftTook = took;
			}
			if (leftTook == 0) continue;
			for (const std::string &writing : group) {
				const std::size_t took = writingAt(right, there, writing);
				if (took > rightTook) rightTook = took;
			}
			if (rightTook == 0) continue;
			here += leftTook;
			there += rightTook;
			substituted = true;
			break;
		}
		if (substituted) continue;
		if (space_at(left, here) || space_at(right, there)) {
			if (!space_at(left, here) || !space_at(right, there)) return false;
			skipSpace(left, here);
			skipSpace(right, there);
			continue;
		}
		const Folded a = fold_at(left, here);
		const Folded b = fold_at(right, there);
		if (a.character != b.character) return false;
		here += a.bytes;
		there += b.bytes;
	}
	skipSpace(left, here);
	skipSpace(right, there);
	return here >= left.size() && there >= right.size();
}

// The same phrase without the whitespace a sentence put around it: a model
// quoting a sentence often takes the space before it.
[[nodiscard]] inline std::string trimmed_phrase(std::string phrase) {
	while (!phrase.empty() && std::isspace(static_cast<unsigned char>(phrase.front()))) phrase.erase(phrase.begin());
	while (!phrase.empty() && std::isspace(static_cast<unsigned char>(phrase.back()))) phrase.pop_back();
	return phrase;
}

} // namespace singularity::platform
