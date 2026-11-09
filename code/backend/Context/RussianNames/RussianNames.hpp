#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Russian names in the case and gender a sentence needs, worked out on the
// lawyer's computer.
//
// Papinian Guard keeps a person's or a company's name from a provider's model,
// which then cannot decline it or agree a word with it: a live Russian draft
// read «…» and
// «…» (24 Sep 2026). The model asks for the
// form instead - {{case … [[E5]]}}, {{gender [[E5]]: …}}
// (Context/ComputedStatements) - and Singularity writes it here from the restored
// name. Nothing about the person leaves the device to make that possible.
//
// Only regular Russian patterns are declined: surnames in -… and
// -…, patronymics, the common first names below, and the organisation
// forms a contract names parties by. Anything else is written as it is:
// a foreign or unusual name is never guessed into a wrong form.
//
// Header-only and free of other modules, like Platform/TextFold, so every
// target that computes statements has it without a build change of its own.
namespace singularity::context::russian_names {

enum class Gender { Masculine, Feminine, Neuter, Unknown };
enum class Case { Nominative, Genitive, Dative, Accusative, Instrumental, Prepositional };

namespace detail {

// Lower-case Cyrillic (…) and ASCII; other bytes as they are.
inline std::string lowered(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte == 0xD0 && index + 1 < text.size()) {
			const unsigned char next = static_cast<unsigned char>(text[index + 1]);
			if (next >= 0x90 && next <= 0x9F) { result += '\xD0'; result += static_cast<char>(next + 0x20); ++index; continue; }
			if (next >= 0xA0 && next <= 0xAF) { result += '\xD1'; result += static_cast<char>(next - 0x20); ++index; continue; }
			if (next == 0x81) { result += "\xD1\x91"; ++index; continue; }   // … -> …
		}
		if (byte < 0x80) { result += static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + 32 : byte); continue; }
		result += static_cast<char>(byte);
	}
	return result;
}

inline bool ends_with(const std::string &text, const std::string &suffix) {
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool is_cyrillic_word(const std::string &word) {
	if (word.size() < 4) return false;
	for (std::size_t index = 0; index + 1 < word.size(); index += 2) {
		const unsigned char byte = static_cast<unsigned char>(word[index]);
		if (byte != 0xD0 && byte != 0xD1) return false;
	}
	return word.size() % 2 == 0;
}

// The word's own capitalisation carried onto a new form: all capitals stay
// capitals, a capitalised word stays capitalised.
inline std::string shaped_like(const std::string &original, const std::string &lowerForm) {
	// A hyphenated word («…») is shaped part by part.
	if (original.find('-') != std::string::npos
			&& std::count(original.begin(), original.end(), '-') == std::count(lowerForm.begin(), lowerForm.end(), '-')) {
		std::string result;
		std::size_t from = 0;
		std::size_t formFrom = 0;
		for (;;) {
			const std::size_t dash = original.find('-', from);
			const std::size_t formDash = lowerForm.find('-', formFrom);
			result += shaped_like(original.substr(from, dash == std::string::npos ? std::string::npos : dash - from),
				lowerForm.substr(formFrom, formDash == std::string::npos ? std::string::npos : formDash - formFrom));
			if (dash == std::string::npos) return result;
			result += '-';
			from = dash + 1;
			formFrom = formDash + 1;
		}
	}
	const std::string lower = lowered(original);
	if (lower == original) return lowerForm;
	bool allCapitals = true;
	for (std::size_t index = 2; index + 1 < original.size(); index += 2)
		if (original.compare(index, 2, lower, index, 2) == 0) { allCapitals = false; break; }
	std::string result;
	for (std::size_t index = 0; index + 1 < lowerForm.size(); index += 2) {
		const std::string letter = lowerForm.substr(index, 2);
		if (index == 0 || allCapitals) {
			const unsigned char first = static_cast<unsigned char>(letter[0]);
			const unsigned char second = static_cast<unsigned char>(letter[1]);
			if (first == 0xD0 && second >= 0xB0 && second <= 0xBF) { result += '\xD0'; result += static_cast<char>(second - 0x20); continue; }
			if (first == 0xD1 && second >= 0x80 && second <= 0x8F) { result += '\xD0'; result += static_cast<char>(second + 0x20); continue; }
			if (first == 0xD1 && second == 0x91) { result += "\xD0\x81"; continue; }
		}
		result += letter;
	}
	if (lowerForm.size() % 2 == 1) result += lowerForm.back();
	return result;
}

// Six forms: nominative, genitive, dative, accusative, instrumental, prepositional.
using Forms = std::array<std::string, 6>;

inline Forms forms_by_ending(const std::string &stem, const std::array<const char *, 6> &endings) {
	Forms forms;
	for (std::size_t index = 0; index < 6; ++index) forms[index] = stem + endings[index];
	return forms;
}

// A first name's six forms from its nominative, lower case.
inline std::optional<Forms> first_name_forms(const std::string &name, Gender gender) {
	// Names that do not follow the pattern of their ending.
	if (name == "\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xB5\xD0\xBB")  // …
		return Forms{name, "\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB0", "\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x83",
			"\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB0", "\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xBE\xD0\xBC",
			"\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB5"};
	if (name == "\xD0\xBB\xD0\xB5\xD0\xB2")  // …
		return Forms{name, "\xD0\xBB\xD1\x8C\xD0\xB2\xD0\xB0", "\xD0\xBB\xD1\x8C\xD0\xB2\xD1\x83", "\xD0\xBB\xD1\x8C\xD0\xB2\xD0\xB0",
			"\xD0\xBB\xD1\x8C\xD0\xB2\xD0\xBE\xD0\xBC", "\xD0\xBB\xD1\x8C\xD0\xB2\xD0\xB5"};
	if (name == "\xD0\xBF\xD1\x91\xD1\x82\xD1\x80" || name == "\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80")  // …
		return Forms{name, "\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80\xD0\xB0", "\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80\xD1\x83",
			"\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80\xD0\xB0", "\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xBC",
			"\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80\xD0\xB5"};
	if (name == "\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD1\x8C")  // …
		return Forms{name, "\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD0\xB8", "\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD0\xB8",
			name, "\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD1\x8C\xD1\x8E", "\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD0\xB8"};
	const auto stem = [&](std::size_t letters) { return name.substr(0, name.size() - 2 * letters); };
	// A name ending in a vowel other than -… (…) keeps its form.
	for (const char *vowel : {"\xD0\xBE", "\xD0\xB5", "\xD0\xB8", "\xD1\x83", "\xD1\x8E", "\xD1\x8D", "\xD1\x8B"})  // …
		if (ends_with(name, vowel)) return Forms{name, name, name, name, name, name};
	if (ends_with(name, "\xD0\xB8\xD1\x8F"))  // -…: …
		return forms_by_ending(stem(1), {"\xD1\x8F", "\xD0\xB8", "\xD0\xB8", "\xD1\x8E", "\xD0\xB5\xD0\xB9", "\xD0\xB8"});
	if (ends_with(name, "\xD1\x8C\xD1\x8F"))  // -…: …
		return forms_by_ending(stem(1), {"\xD1\x8F", "\xD0\xB8", "\xD0\xB5", "\xD1\x8E", "\xD0\xB5\xD0\xB9", "\xD0\xB5"});
	if (ends_with(name, "\xD1\x8F"))  // -… after anything else: …
		return forms_by_ending(stem(1), {"\xD1\x8F", "\xD0\xB8", "\xD0\xB5", "\xD1\x8E", "\xD0\xB5\xD0\xB9", "\xD0\xB5"});
	if (ends_with(name, "\xD0\xB0")) {  // -…: …
		const std::string base = stem(1);
		const bool softGenitive = ends_with(base, "\xD0\xB3") || ends_with(base, "\xD0\xBA") || ends_with(base, "\xD1\x85")
			|| ends_with(base, "\xD0\xB6") || ends_with(base, "\xD1\x88") || ends_with(base, "\xD1\x87") || ends_with(base, "\xD1\x89");
		return forms_by_ending(base, {"\xD0\xB0", softGenitive ? "\xD0\xB8" : "\xD1\x8B", "\xD0\xB5", "\xD1\x83",
			"\xD0\xBE\xD0\xB9", "\xD0\xB5"});
	}
	if (gender == Gender::Feminine) {
		// A woman's name in -… is declined (…); one ending in another
		// consonant is not (…).
		if (ends_with(name, "\xD1\x8C")) return forms_by_ending(stem(1), {"\xD1\x8C", "\xD0\xB8", "\xD0\xB8", "\xD1\x8C", "\xD1\x8C\xD1\x8E", "\xD0\xB8"});  // -…: …
		return Forms{name, name, name, name, name, name};
	}
	if (gender != Gender::Masculine) return std::nullopt;
	if (ends_with(name, "\xD0\xB8\xD0\xB9"))  // -…: …
		return forms_by_ending(stem(1), {"\xD0\xB9", "\xD1\x8F", "\xD1\x8E", "\xD1\x8F", "\xD0\xB5\xD0\xBC", "\xD0\xB8"});
	if (ends_with(name, "\xD0\xB9"))  // -…, -…: …
		return forms_by_ending(stem(1), {"\xD0\xB9", "\xD1\x8F", "\xD1\x8E", "\xD1\x8F", "\xD0\xB5\xD0\xBC", "\xD0\xB5"});
	if (ends_with(name, "\xD1\x8C"))  // -…: …
		return forms_by_ending(stem(1), {"\xD1\x8C", "\xD1\x8F", "\xD1\x8E", "\xD1\x8F", "\xD0\xB5\xD0\xBC", "\xD0\xB5"});
	return forms_by_ending(name, {"", "\xD0\xB0", "\xD1\x83", "\xD0\xB0", "\xD0\xBE\xD0\xBC", "\xD0\xB5"});  // …
}

struct KnownName {
	const char *nominative;
	Gender gender;
};

// Common Russian first names, lower case: a name is recognised in any case by
// its forms, and declined only when it is recognised.
inline const std::vector<KnownName> &known_first_names() {
	static const std::vector<KnownName> names = [] {
		std::vector<KnownName> list;
		for (const char *name : {
				"\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB4\xD1\x80", "\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB5\xD0\xB9",
				"\xD0\xB0\xD0\xBD\xD0\xB4\xD1\x80\xD0\xB5\xD0\xB9", "\xD0\xB0\xD0\xBD\xD1\x82\xD0\xBE\xD0\xBD", "\xD0\xB0\xD1\x80\xD1\x82\xD1\x91\xD0\xBC",
				"\xD0\xB0\xD1\x80\xD1\x82\xD0\xB5\xD0\xBC", "\xD0\xB1\xD0\xBE\xD1\x80\xD0\xB8\xD1\x81", "\xD0\xB2\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xBC",
				"\xD0\xB2\xD0\xB0\xD1\x81\xD0\xB8\xD0\xBB\xD0\xB8\xD0\xB9", "\xD0\xB2\xD0\xB8\xD0\xBA\xD1\x82\xD0\xBE\xD1\x80",
				"\xD0\xB2\xD0\xBB\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xBC\xD0\xB8\xD1\x80", "\xD0\xB2\xD0\xBB\xD0\xB0\xD0\xB4\xD0\xB8\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2",
				"\xD0\xB3\xD0\xB5\xD0\xBD\xD0\xBD\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xB9", "\xD0\xB3\xD0\xB5\xD0\xBE\xD1\x80\xD0\xB3\xD0\xB8\xD0\xB9",
				"\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x81", "\xD0\xB4\xD0\xBC\xD0\xB8\xD1\x82\xD1\x80\xD0\xB8\xD0\xB9",
				"\xD0\xB5\xD0\xB2\xD0\xB3\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB9", "\xD0\xB5\xD0\xB3\xD0\xBE\xD1\x80", "\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xBD",
				"\xD0\xB8\xD0\xB3\xD0\xBE\xD1\x80\xD1\x8C", "\xD0\xB8\xD0\xBB\xD1\x8C\xD1\x8F", "\xD0\xBA\xD0\xB8\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBB",
				"\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD1\x82\xD0\xB8\xD0\xBD", "\xD0\xBB\xD0\xB5\xD0\xB2",
				"\xD0\xBB\xD0\xB5\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xB4", "\xD0\xBC\xD0\xB0\xD0\xBA\xD1\x81\xD0\xB8\xD0\xBC",
				"\xD0\xBC\xD0\xB8\xD1\x85\xD0\xB0\xD0\xB8\xD0\xBB", "\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xB8\xD1\x82\xD0\xB0",
				"\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xBB\xD0\xB0\xD0\xB9", "\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB3", "\xD0\xBF\xD0\xB0\xD0\xB2\xD0\xB5\xD0\xBB",
				"\xD0\xBF\xD1\x91\xD1\x82\xD1\x80", "\xD0\xBF\xD0\xB5\xD1\x82\xD1\x80", "\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB0\xD0\xBD",
				"\xD1\x80\xD1\x83\xD1\x81\xD0\xBB\xD0\xB0\xD0\xBD", "\xD1\x81\xD0\xB5\xD1\x80\xD0\xB3\xD0\xB5\xD0\xB9",
				"\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2", "\xD1\x82\xD0\xB8\xD0\xBC\xD1\x83\xD1\x80",
				"\xD1\x84\xD1\x91\xD0\xB4\xD0\xBE\xD1\x80", "\xD1\x8E\xD1\x80\xD0\xB8\xD0\xB9", "\xD1\x8F\xD0\xBA\xD0\xBE\xD0\xB2",
				"\xD0\xB0\xD0\xBD\xD0\xB0\xD1\x82\xD0\xBE\xD0\xBB\xD0\xB8\xD0\xB9", "\xD0\xB2\xD0\xB0\xD0\xBB\xD0\xB5\xD1\x80\xD0\xB8\xD0\xB9",
				"\xD0\xB2\xD0\xB8\xD1\x82\xD0\xB0\xD0\xBB\xD0\xB8\xD0\xB9", "\xD0\xB0\xD1\x80\xD0\xBA\xD0\xB0\xD0\xB4\xD0\xB8\xD0\xB9",
				"\xD0\xB3\xD1\x80\xD0\xB8\xD0\xB3\xD0\xBE\xD1\x80\xD0\xB8\xD0\xB9", "\xD1\x8D\xD0\xB4\xD1\x83\xD0\xB0\xD1\x80\xD0\xB4"})
			list.push_back({name, Gender::Masculine});
		for (const char *name : {
				"\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB4\xD1\x80\xD0\xB0", "\xD0\xB0\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xB0",
				"\xD0\xB0\xD0\xBB\xD0\xBB\xD0\xB0", "\xD0\xB0\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD0\xB0\xD1\x81\xD0\xB8\xD1\x8F",
				"\xD0\xB0\xD0\xBD\xD0\xBD\xD0\xB0", "\xD0\xB0\xD0\xBD\xD1\x82\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xBD\xD0\xB0",
				"\xD0\xB2\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xB2\xD0\xB0\xD1\x80\xD0\xB2\xD0\xB0\xD1\x80\xD0\xB0",
				"\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB0", "\xD0\xB2\xD0\xB8\xD0\xBA\xD1\x82\xD0\xBE\xD1\x80\xD0\xB8\xD1\x8F",
				"\xD0\xB3\xD0\xB0\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xB4\xD0\xB0\xD1\x80\xD1\x8C\xD1\x8F",
				"\xD0\xB5\xD0\xB2\xD0\xB3\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F", "\xD0\xB5\xD0\xBA\xD0\xB0\xD1\x82\xD0\xB5\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB0",
				"\xD0\xB5\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB0", "\xD0\xB7\xD0\xBE\xD1\x8F", "\xD0\xB8\xD0\xBD\xD0\xBD\xD0\xB0",
				"\xD0\xB8\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xBA\xD1\x81\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F",
				"\xD0\xBB\xD0\xB0\xD1\x80\xD0\xB8\xD1\x81\xD0\xB0", "\xD0\xBB\xD1\x8E\xD0\xB4\xD0\xBC\xD0\xB8\xD0\xBB\xD0\xB0",
				"\xD0\xBB\xD1\x8E\xD0\xB1\xD0\xBE\xD0\xB2\xD1\x8C", "\xD0\xBC\xD0\xB0\xD1\x80\xD0\xB3\xD0\xB0\xD1\x80\xD0\xB8\xD1\x82\xD0\xB0",
				"\xD0\xBC\xD0\xB0\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xBC\xD0\xB0\xD1\x80\xD0\xB8\xD1\x8F",
				"\xD0\xBD\xD0\xB0\xD0\xB4\xD0\xB5\xD0\xB6\xD0\xB4\xD0\xB0", "\xD0\xBD\xD0\xB0\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C\xD1\x8F",
				"\xD0\xBD\xD0\xB0\xD1\x82\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x8F", "\xD0\xBE\xD0\xBA\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB0",
				"\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xB3\xD0\xB0", "\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xB0",
				"\xD1\x81\xD0\xB2\xD0\xB5\xD1\x82\xD0\xBB\xD0\xB0\xD0\xBD\xD0\xB0", "\xD1\x81\xD0\xBE\xD1\x84\xD1\x8C\xD1\x8F",
				"\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C\xD1\x8F\xD0\xBD\xD0\xB0", "\xD1\x8E\xD0\xBB\xD0\xB8\xD1\x8F",
				"\xD1\x8F\xD0\xBD\xD0\xB0", "\xD1\x8D\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xB4\xD0\xB8\xD0\xB0\xD0\xBD\xD0\xB0",
				"\xD0\xBB\xD0\xB8\xD0\xB4\xD0\xB8\xD1\x8F", "\xD1\x80\xD0\xB0\xD0\xB8\xD1\x81\xD0\xB0"})
			list.push_back({name, Gender::Feminine});
		return list;
	}();
	return names;
}

// The known first name `lowerWord` is a form of, with its gender.
inline std::optional<std::pair<Forms, Gender>> recognised_first_name(const std::string &lowerWord) {
	for (const KnownName &known : known_first_names()) {
		const auto forms = first_name_forms(known.nominative, known.gender);
		if (!forms) continue;
		for (const std::string &form : *forms)
			if (form == lowerWord) return std::make_pair(*forms, known.gender);
	}
	return std::nullopt;
}

// A patronymic in any case: its nominative and gender.
inline std::optional<std::pair<std::string, Gender>> patronymic_nominative(const std::string &lowerWord) {
	const std::size_t masculine = lowerWord.rfind("\xD0\xB8\xD1\x87");   // …
	if (masculine != std::string::npos && masculine + 4 >= lowerWord.size() - 6 && masculine >= 4) {
		const std::string rest = lowerWord.substr(masculine + 4);
		if (rest.empty() || rest == "\xD0\xB0" || rest == "\xD1\x83" || rest == "\xD0\xB5\xD0\xBC" || rest == "\xD0\xBE\xD0\xBC"
				|| rest == "\xD0\xB5")
			return std::make_pair(lowerWord.substr(0, masculine + 4), Gender::Masculine);
	}
	for (const char *stem : {"\xD0\xB2\xD0\xBD", "\xD1\x87\xD0\xBD"}) {  // …
		const std::size_t at = lowerWord.rfind(stem);
		if (at == std::string::npos || at < 4) continue;
		const std::string rest = lowerWord.substr(at + 4);
		if (rest == "\xD0\xB0" || rest == "\xD1\x8B" || rest == "\xD0\xB5" || rest == "\xD1\x83" || rest == "\xD0\xBE\xD0\xB9"
				|| rest == "\xD0\xBE\xD1\x8E")
			return std::make_pair(lowerWord.substr(0, at + 4) + "\xD0\xB0", Gender::Feminine);
	}
	return std::nullopt;
}

inline bool is_initial(const std::string &word);

// Which word of a name is its patronymic: one that stands right after another
// name word. A surname in -… («…») reads like a
// patronymic too, but it begins the name, follows the patronymic or stands
// with initials - never right after the first name.
inline std::optional<std::size_t> patronymic_position(const std::vector<std::string> &words) {
	for (std::size_t index = 1; index < words.size(); ++index) {
		if (!is_cyrillic_word(words[index]) || !patronymic_nominative(lowered(words[index]))) continue;
		const std::string &before = words[index - 1];
		if (is_initial(before) || !is_cyrillic_word(before) || patronymic_nominative(lowered(before))) continue;
		return index;
	}
	return std::nullopt;
}

inline Forms patronymic_forms(const std::string &nominative, Gender gender) {
	if (gender == Gender::Masculine) {
		// A patronymic stressed on -… takes -…: «…»; the rest -…: «…».
		bool stressed = false;
		for (const char *name : {"\xD0\xB8\xD0\xBB\xD1\x8C\xD0\xB8\xD1\x87", "\xD0\xBA\xD1\x83\xD0\xB7\xD1\x8C\xD0\xBC\xD0\xB8\xD1\x87",
				"\xD0\xBB\xD1\x83\xD0\xBA\xD0\xB8\xD1\x87", "\xD1\x84\xD0\xBE\xD0\xBC\xD0\xB8\xD1\x87"})  // …
			if (nominative == name) stressed = true;
		return forms_by_ending(nominative, {"", "\xD0\xB0", "\xD1\x83", "\xD0\xB0", stressed ? "\xD0\xBE\xD0\xBC" : "\xD0\xB5\xD0\xBC", "\xD0\xB5"});
	}
	return forms_by_ending(nominative.substr(0, nominative.size() - 2),
		{"\xD0\xB0", "\xD1\x8B", "\xD0\xB5", "\xD1\x83", "\xD0\xBE\xD0\xB9", "\xD0\xB5"});
}

// A surname's six forms, from any of its forms, when its pattern is a regular one.
inline std::optional<Forms> surname_forms(const std::string &lowerWord, Gender gender) {
	struct Pattern { const char *nominative; std::array<const char *, 6> endings; };
	static const Pattern masculine[] = {
		{"\xD0\xBE\xD0\xB2", {"\xD0\xBE\xD0\xB2", "\xD0\xBE\xD0\xB2\xD0\xB0", "\xD0\xBE\xD0\xB2\xD1\x83", "\xD0\xBE\xD0\xB2\xD0\xB0", "\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xBC", "\xD0\xBE\xD0\xB2\xD0\xB5"}},
		{"\xD0\xB5\xD0\xB2", {"\xD0\xB5\xD0\xB2", "\xD0\xB5\xD0\xB2\xD0\xB0", "\xD0\xB5\xD0\xB2\xD1\x83", "\xD0\xB5\xD0\xB2\xD0\xB0", "\xD0\xB5\xD0\xB2\xD1\x8B\xD0\xBC", "\xD0\xB5\xD0\xB2\xD0\xB5"}},
		{"\xD1\x91\xD0\xB2", {"\xD1\x91\xD0\xB2", "\xD1\x91\xD0\xB2\xD0\xB0", "\xD1\x91\xD0\xB2\xD1\x83", "\xD1\x91\xD0\xB2\xD0\xB0", "\xD1\x91\xD0\xB2\xD1\x8B\xD0\xBC", "\xD1\x91\xD0\xB2\xD0\xB5"}},
		{"\xD0\xB8\xD0\xBD", {"\xD0\xB8\xD0\xBD", "\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xB8\xD0\xBD\xD1\x83", "\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xB8\xD0\xBD\xD1\x8B\xD0\xBC", "\xD0\xB8\xD0\xBD\xD0\xB5"}},
		{"\xD1\x8B\xD0\xBD", {"\xD1\x8B\xD0\xBD", "\xD1\x8B\xD0\xBD\xD0\xB0", "\xD1\x8B\xD0\xBD\xD1\x83", "\xD1\x8B\xD0\xBD\xD0\xB0", "\xD1\x8B\xD0\xBD\xD1\x8B\xD0\xBC", "\xD1\x8B\xD0\xBD\xD0\xB5"}},
		{"\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", {"\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xBC\xD1\x83", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x81\xD0\xBA\xD0\xB8\xD0\xBC", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xBC"}},
		{"\xD1\x86\xD0\xBA\xD0\xB8\xD0\xB9", {"\xD1\x86\xD0\xBA\xD0\xB8\xD0\xB9", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xBC\xD1\x83", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x86\xD0\xBA\xD0\xB8\xD0\xBC", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xBC"}},
		{"\xD0\xB8\xD1\x87", {"\xD0\xB8\xD1\x87", "\xD0\xB8\xD1\x87\xD0\xB0", "\xD0\xB8\xD1\x87\xD1\x83", "\xD0\xB8\xD1\x87\xD0\xB0", "\xD0\xB8\xD1\x87\xD0\xB5\xD0\xBC", "\xD0\xB8\xD1\x87\xD0\xB5"}},
	};
	static const Pattern feminine[] = {
		{"\xD0\xBE\xD0\xB2\xD0\xB0", {"\xD0\xBE\xD0\xB2\xD0\xB0", "\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xBE\xD0\xB2\xD1\x83", "\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB9"}},
		{"\xD0\xB5\xD0\xB2\xD0\xB0", {"\xD0\xB5\xD0\xB2\xD0\xB0", "\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xB5\xD0\xB2\xD1\x83", "\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB9"}},
		{"\xD1\x91\xD0\xB2\xD0\xB0", {"\xD1\x91\xD0\xB2\xD0\xB0", "\xD1\x91\xD0\xB2\xD0\xBE\xD0\xB9", "\xD1\x91\xD0\xB2\xD0\xBE\xD0\xB9", "\xD1\x91\xD0\xB2\xD1\x83", "\xD1\x91\xD0\xB2\xD0\xBE\xD0\xB9", "\xD1\x91\xD0\xB2\xD0\xBE\xD0\xB9"}},
		{"\xD0\xB8\xD0\xBD\xD0\xB0", {"\xD0\xB8\xD0\xBD\xD0\xB0", "\xD0\xB8\xD0\xBD\xD0\xBE\xD0\xB9", "\xD0\xB8\xD0\xBD\xD0\xBE\xD0\xB9", "\xD0\xB8\xD0\xBD\xD1\x83", "\xD0\xB8\xD0\xBD\xD0\xBE\xD0\xB9", "\xD0\xB8\xD0\xBD\xD0\xBE\xD0\xB9"}},
		{"\xD1\x8B\xD0\xBD\xD0\xB0", {"\xD1\x8B\xD0\xBD\xD0\xB0", "\xD1\x8B\xD0\xBD\xD0\xBE\xD0\xB9", "\xD1\x8B\xD0\xBD\xD0\xBE\xD0\xB9", "\xD1\x8B\xD0\xBD\xD1\x83", "\xD1\x8B\xD0\xBD\xD0\xBE\xD0\xB9", "\xD1\x8B\xD0\xBD\xD0\xBE\xD0\xB9"}},
		{"\xD1\x81\xD0\xBA\xD0\xB0\xD1\x8F", {"\xD1\x81\xD0\xBA\xD0\xB0\xD1\x8F", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9", "\xD1\x81\xD0\xBA\xD1\x83\xD1\x8E", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB9"}},
		{"\xD1\x86\xD0\xBA\xD0\xB0\xD1\x8F", {"\xD1\x86\xD0\xBA\xD0\xB0\xD1\x8F", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB9", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB9", "\xD1\x86\xD0\xBA\xD1\x83\xD1\x8E", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB9", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB9"}},
		// A woman's -… keeps its form: «…».
		{"\xD0\xB8\xD1\x87", {"\xD0\xB8\xD1\x87", "\xD0\xB8\xD1\x87", "\xD0\xB8\xD1\x87", "\xD0\xB8\xD1\x87", "\xD0\xB8\xD1\x87", "\xD0\xB8\xD1\x87"}},
	};
	if (gender != Gender::Masculine && gender != Gender::Feminine) return std::nullopt;
	const auto begin = gender == Gender::Masculine ? std::begin(masculine) : std::begin(feminine);
	const auto end = gender == Gender::Masculine ? std::end(masculine) : std::end(feminine);
	// The longest ending a form of the pattern ends in names its stem.
	for (auto pattern = begin; pattern != end; ++pattern) {
		std::size_t best = 0;
		for (const char *ending : pattern->endings) {
			const std::string written = ending;
			if (written.size() > best && ends_with(lowerWord, written) && lowerWord.size() >= written.size() + 4) best = written.size();
		}
		if (best == 0) continue;
		return forms_by_ending(lowerWord.substr(0, lowerWord.size() - best), pattern->endings);
	}
	return std::nullopt;
}

// A man's surname ending in a consonant (…)
// declines like a noun, from the case the rest of the name shows it is
// written in; a woman's keeps its form. -… (…) and adjectival
// endings never decline this way.
inline std::optional<Forms> consonant_surname_forms(const std::string &lowerWord, std::size_t writtenCase) {
	static const std::array<const char *, 6> hard{"", "\xD0\xB0", "\xD1\x83", "\xD0\xB0", "\xD0\xBE\xD0\xBC", "\xD0\xB5"};                    // ∅ …
	static const std::array<const char *, 6> hushing{"", "\xD0\xB0", "\xD1\x83", "\xD0\xB0", "\xD0\xB5\xD0\xBC", "\xD0\xB5"};                 // ∅ …
	static const std::array<const char *, 6> softSign{"\xD1\x8C", "\xD1\x8F", "\xD1\x8E", "\xD1\x8F", "\xD0\xB5\xD0\xBC", "\xD0\xB5"};       // …
	static const std::array<const char *, 6> shortI{"\xD0\xB9", "\xD1\x8F", "\xD1\x8E", "\xD1\x8F", "\xD0\xB5\xD0\xBC", "\xD0\xB5"};         // …
	const auto last_letter = [](const std::string &word) { return word.size() >= 2 ? word.substr(word.size() - 2) : std::string(); };
	const auto consonant = [](const std::string &letter) {
		static const std::array<const char *, 20> letters{"\xD0\xB1", "\xD0\xB2", "\xD0\xB3", "\xD0\xB4", "\xD0\xB6", "\xD0\xB7",
			"\xD0\xBA", "\xD0\xBB", "\xD0\xBC", "\xD0\xBD", "\xD0\xBF", "\xD1\x80", "\xD1\x81", "\xD1\x82", "\xD1\x84", "\xD1\x85",
			"\xD1\x86", "\xD1\x87", "\xD1\x88", "\xD1\x89"};  // …
		return std::find(letters.begin(), letters.end(), letter) != letters.end();
	};
	const auto hushed = [](const std::string &letter) {
		return letter == "\xD0\xB6" || letter == "\xD1\x88" || letter == "\xD1\x87" || letter == "\xD1\x89" || letter == "\xD1\x86";
	};
	const auto vowel = [](const std::string &letter) {
		for (const char *written : {"\xD0\xB0", "\xD0\xBE", "\xD1\x83", "\xD1\x8D", "\xD1\x8B", "\xD0\xB8", "\xD0\xB5", "\xD1\x91", "\xD1\x8E", "\xD1\x8F"})
			if (letter == written) return true;
		return false;
	};
	// The nominative from the written form.
	std::string nominative;
	for (const auto *table : {&hard, &hushing, &softSign, &shortI}) {
		const std::string ending = (*table)[writtenCase];
		if (lowerWord.size() < ending.size() + 4 || !ends_with(lowerWord, ending)) continue;
		const std::string stem = lowerWord.substr(0, lowerWord.size() - ending.size());
		const std::string candidate = stem + (*table)[0];
		const std::string last = last_letter(stem);
		const bool fits = table == &hard ? consonant(last) && !hushed(last)
			: table == &hushing ? hushed(last)
			: table == &softSign ? consonant(last)
			: vowel(last) && last != "\xD0\xB8" && last != "\xD0\xBE" && last != "\xD1\x8B";  // not -…, -…, -…
		if (!fits) continue;
		nominative = candidate;
		break;
	}
	if (nominative.empty() || nominative.size() < 6) return std::nullopt;
	if (ends_with(nominative, "\xD1\x8B\xD1\x85") || ends_with(nominative, "\xD0\xB8\xD1\x85")) return std::nullopt;  // -…, -…
	const std::string last = last_letter(nominative);
	if (last == "\xD1\x8C") return forms_by_ending(nominative.substr(0, nominative.size() - 2), softSign);
	if (last == "\xD0\xB9") return forms_by_ending(nominative.substr(0, nominative.size() - 2), shortI);
	return forms_by_ending(nominative, hushed(last) ? hushing : hard);
}

// How surely a word is a surname: 3 for -…, -…, -… (and their feminine
// forms), 2 for another regular pattern (-…, -…, -…: «…», but also the
// first names «…»), 1 for a man's surname in a consonant, 0 for none.
inline int surname_rank(const std::string &lowerWord, Gender gender, std::size_t writtenCase = 0) {
	if (const auto forms = surname_forms(lowerWord, gender)) {
		for (const char *strong : {"\xD0\xBE\xD0\xB2", "\xD0\xB5\xD0\xB2", "\xD1\x91\xD0\xB2", "\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",
				"\xD1\x86\xD0\xBA\xD0\xB8\xD0\xB9", "\xD0\xBE\xD0\xB2\xD0\xB0", "\xD0\xB5\xD0\xB2\xD0\xB0", "\xD1\x91\xD0\xB2\xD0\xB0",
				"\xD1\x81\xD0\xBA\xD0\xB0\xD1\x8F", "\xD1\x86\xD0\xBA\xD0\xB0\xD1\x8F"})  // …
			if (ends_with((*forms)[0], strong)) return 3;
		return 2;
	}
	if (gender == Gender::Masculine && consonant_surname_forms(lowerWord, writtenCase)) return 1;
	return 0;
}

// Which of the six forms `lowerWord` is (the first that matches).
inline std::size_t form_index(const Forms &forms, const std::string &lowerWord) {
	for (std::size_t index = 0; index < forms.size(); ++index)
		if (forms[index] == lowerWord) return index;
	return 0;
}

// A first name Singularity does not know, standing before a patronymic: its forms,
// from the case the patronymic shows it is written in («…» is
// the genitive of «…» of «…»).
inline std::optional<Forms> unknown_first_name_forms(const std::string &lowerWord, Gender gender, std::size_t writtenCase) {
	if (writtenCase == 0) return first_name_forms(lowerWord, gender);
	// A declined reading first; a name that keeps its form last.
	for (std::size_t letters = 1; letters <= 3; ++letters) {
		if (lowerWord.size() < 2 * letters + 4) break;
		const std::string stem = lowerWord.substr(0, lowerWord.size() - 2 * letters);
		for (const char *ending : {"", "\xD0\xB0", "\xD1\x8F", "\xD1\x8C", "\xD0\xB9", "\xD0\xB8\xD1\x8F", "\xD1\x8C\xD1\x8F"}) {  // ∅ …
			const auto forms = first_name_forms(stem + ending, gender);
			if (forms && (*forms)[writtenCase] == lowerWord && (*forms)[0] != lowerWord) return forms;
		}
	}
	const auto forms = first_name_forms(lowerWord, gender);
	if (forms && (*forms)[writtenCase] == lowerWord) return forms;
	return std::nullopt;
}

// (neuter), «…» (feminine),
// «…» (masculine) in any case: how much of the word is its stem, the gender,
// and the six endings.
struct OrganisationNoun {
	std::size_t stem = 0;
	Gender gender = Gender::Neuter;
	std::array<const char *, 6> endings{};
};

inline std::optional<OrganisationNoun> organisation_noun(const std::string &lowerWord) {
	static const std::array<const char *, 6> neuter{"\xD0\xBE", "\xD0\xB0", "\xD1\x83", "\xD0\xBE", "\xD0\xBE\xD0\xBC", "\xD0\xB5"};  // …
	static const std::array<const char *, 6> neuterSoft{"\xD0\xB5", "\xD1\x8F", "\xD1\x8E", "\xD0\xB5", "\xD0\xB5\xD0\xBC", "\xD0\xB8"};  // …
	static const std::array<const char *, 6> feminine{"\xD1\x8F", "\xD0\xB8", "\xD0\xB8", "\xD1\x8E", "\xD0\xB5\xD0\xB9", "\xD0\xB8"};  // …
	static const std::array<const char *, 6> masculine{"", "\xD0\xB0", "\xD1\x83", "", "\xD0\xBE\xD0\xBC", "\xD0\xB5"};  // ∅ … ∅ …
	struct Family { std::vector<const char *> stems; Gender gender; const std::array<const char *, 6> *endings; };
	static const std::array<Family, 4> families{{
		{{"\xD0\xBE\xD0\xB1\xD1\x89\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2", "\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x80\xD0\xB8\xD1\x89\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2", "\xD0\xBF\xD0\xB0\xD1\x80\xD1\x82\xD0\xBD\xD1\x91\xD1\x80\xD1\x81\xD1\x82\xD0\xB2", "\xD0\xBF\xD0\xB0\xD1\x80\xD1\x82\xD0\xBD\xD0\xB5\xD1\x80\xD1\x81\xD1\x82\xD0\xB2"},  // …
			Gender::Neuter, &neuter},
		{{"\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBF\xD1\x80\xD0\xB8\xD1\x8F\xD1\x82\xD0\xB8", "\xD1\x83\xD1\x87\xD1\x80\xD0\xB5\xD0\xB6\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xB8"},  // …
			Gender::Neuter, &neuterSoft},
		{{"\xD0\xBA\xD0\xBE\xD0\xBC\xD0\xBF\xD0\xB0\xD0\xBD\xD0\xB8", "\xD0\xBA\xD0\xBE\xD1\x80\xD0\xBF\xD0\xBE\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8", "\xD0\xBE\xD1\x80\xD0\xB3\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB7\xD0\xB0\xD1\x86\xD0\xB8", "\xD0\xB0\xD1\x81\xD1\x81\xD0\xBE\xD1\x86\xD0\xB8\xD0\xB0\xD1\x86\xD0\xB8"},  // …
			Gender::Feminine, &feminine},
		{{"\xD1\x84\xD0\xBE\xD0\xBD\xD0\xB4"}, Gender::Masculine, &masculine},  // …
	}};
	for (const Family &family : families)
		for (const char *stem : family.stems)
			for (const char *ending : *family.endings)
				if (lowerWord == std::string(stem) + ending) return OrganisationNoun{std::string(stem).size(), family.gender, *family.endings};
	return std::nullopt;
}

// An adjective before such a noun («…»,
// «…»): the length of its stem, when its ending is one of the gender's.
inline std::optional<std::size_t> organisation_adjective_stem(const std::string &lowerWord, Gender gender) {
	if (!is_cyrillic_word(lowerWord)) return std::nullopt;
	const std::vector<const char *> endings = gender == Gender::Feminine
		? std::vector<const char *>{"\xD0\xB0\xD1\x8F", "\xD0\xBE\xD0\xB9", "\xD1\x83\xD1\x8E"}  // …
		: gender == Gender::Masculine
		? std::vector<const char *>{"\xD0\xBE\xD0\xB3\xD0\xBE", "\xD0\xBE\xD0\xBC\xD1\x83", "\xD1\x8B\xD0\xB9", "\xD0\xB8\xD0\xB9", "\xD1\x8B\xD0\xBC", "\xD0\xB8\xD0\xBC", "\xD0\xBE\xD0\xBC"}  // …
		: std::vector<const char *>{"\xD0\xBE\xD0\xB3\xD0\xBE", "\xD0\xBE\xD0\xBC\xD1\x83", "\xD0\xBE\xD0\xB5", "\xD1\x8B\xD0\xBC", "\xD0\xB8\xD0\xBC", "\xD0\xBE\xD0\xBC"};  // …
	for (const char *ending : endings) {
		const std::string written = ending;
		if (ends_with(lowerWord, written) && lowerWord.size() >= written.size() + 4) return lowerWord.size() - written.size();
	}
	return std::nullopt;
}

// The adjective's form in a case: hard endings, -… / -… after … and a hushing consonant.
inline std::string organisation_adjective_form(const std::string &lowerStem, Gender gender, std::size_t caseIndex) {
	static const std::array<const char *, 6> neuter{"\xD0\xBE\xD0\xB5", "\xD0\xBE\xD0\xB3\xD0\xBE", "\xD0\xBE\xD0\xBC\xD1\x83", "\xD0\xBE\xD0\xB5", "\xD1\x8B\xD0\xBC", "\xD0\xBE\xD0\xBC"};  // …
	static const std::array<const char *, 6> masculine{"\xD1\x8B\xD0\xB9", "\xD0\xBE\xD0\xB3\xD0\xBE", "\xD0\xBE\xD0\xBC\xD1\x83", "\xD1\x8B\xD0\xB9", "\xD1\x8B\xD0\xBC", "\xD0\xBE\xD0\xBC"};  // …
	static const std::array<const char *, 6> feminine{"\xD0\xB0\xD1\x8F", "\xD0\xBE\xD0\xB9", "\xD0\xBE\xD0\xB9", "\xD1\x83\xD1\x8E", "\xD0\xBE\xD0\xB9", "\xD0\xBE\xD0\xB9"};  // …
	if (gender == Gender::Feminine) return lowerStem + feminine[caseIndex];
	bool soft = false;
	for (const char *letter : {"\xD0\xBA", "\xD0\xB3", "\xD1\x85", "\xD0\xB6", "\xD1\x88", "\xD1\x87", "\xD1\x89"})  // …
		if (ends_with(lowerStem, letter)) soft = true;
	if (caseIndex == 4 && soft) return lowerStem + "\xD0\xB8\xD0\xBC";  // -…
	if (gender == Gender::Masculine) {
		if ((caseIndex == 0 || caseIndex == 3) && soft) return lowerStem + "\xD0\xB8\xD0\xB9";  // -…: «…»
		return lowerStem + masculine[caseIndex];
	}
	return lowerStem + neuter[caseIndex];
}

// The cases a form of such a noun can be, a bit per case.
inline unsigned organisation_noun_cases(const std::string &lowerWord, const OrganisationNoun &noun) {
	unsigned cases = 0;
	for (std::size_t index = 0; index < 6; ++index)
		if (lowerWord.compare(noun.stem, std::string::npos, noun.endings[index]) == 0) cases |= 1u << index;
	return cases;
}

// The cases a form of such an adjective can be, a bit per case (none when it is not one).
inline unsigned organisation_adjective_cases(const std::string &lowerWord, Gender gender) {
	const auto stem = organisation_adjective_stem(lowerWord, gender);
	if (!stem) return 0;
	const std::string stemText = lowerWord.substr(0, *stem);
	unsigned cases = 0;
	for (std::size_t index = 0; index < 6; ++index)
		if (organisation_adjective_form(stemText, gender, index) == lowerWord) cases |= 1u << index;
	if (gender != Gender::Feminine && ends_with(lowerWord, "\xD0\xB8\xD0\xBC")) cases |= 1u << 4;  // -…
	return cases;
}

// Where an organisation's noun stands in `words` (after at most four
// adjectives agreeing with it: «…», never
// «…»), with the noun.
inline std::optional<std::pair<std::size_t, OrganisationNoun>> organisation_head(const std::vector<std::string> &words) {
	for (std::size_t at = 0; at < words.size() && at <= 4; ++at) {
		if (const auto noun = organisation_noun(lowered(words[at]))) {
			const unsigned cases = organisation_noun_cases(lowered(words[at]), *noun);
			for (std::size_t before = 0; before < at; ++before)
				if ((organisation_adjective_cases(lowered(words[before]), noun->gender) & cases) == 0) return std::nullopt;
			return std::make_pair(at, *noun);
		}
	}
	return std::nullopt;
}

inline bool is_initial(const std::string &word) {
	if (word.empty() || word.back() != '.') return false;
	std::size_t letters = 0;
	for (std::size_t index = 0; index < word.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(word[index]);
		if (byte == '.') continue;
		if ((byte == 0xD0 || byte == 0xD1) && index + 1 < word.size()) { ++letters; ++index; continue; }
		if (std::isalpha(byte) != 0) { ++letters; continue; }
		return false;
	}
	return letters >= 1 && letters <= 3;
}

inline std::vector<std::string> words_of(const std::string &text) {
	std::vector<std::string> words;
	std::string current;
	for (const char character : text) {
		if (character == ' ') { if (!current.empty()) words.push_back(current); current.clear(); continue; }
		current += character;
	}
	if (!current.empty()) words.push_back(current);
	return words;
}

inline std::string joined(const std::vector<std::string> &words) {
	std::string text;
	for (const std::string &word : words) {
		if (!text.empty()) text += ' ';
		text += word;
	}
	return text;
}

inline std::size_t index_of(Case form) { return static_cast<std::size_t>(form); }

struct PersonReading {
	Gender gender = Gender::Unknown;
	// For each word: its six forms when Singularity can decline it.
	std::vector<std::optional<Forms>> forms;
	bool person = false;
};

// `given` is the gender the caller already knows (from the fullest writing of
// the name); without it the gender is read from the words themselves.
inline PersonReading read_person(const std::vector<std::string> &words, Gender given = Gender::Unknown) {
	PersonReading reading;
	reading.forms.assign(words.size(), std::nullopt);
	const bool known = given == Gender::Masculine || given == Gender::Feminine;
	bool anyName = false;
	// Gender from a patronymic first, then a known first name.
	const std::optional<std::size_t> patronymicAt = patronymic_position(words);
	if (patronymicAt) {
		reading.gender = patronymic_nominative(lowered(words[*patronymicAt]))->second;
		anyName = true;
	}
	if (reading.gender == Gender::Unknown)
		for (const std::string &word : words) {
			if (!is_cyrillic_word(word)) continue;
			if (const auto name = recognised_first_name(lowered(word))) { reading.gender = name->second; anyName = true; break; }
		}
	// A surname in -…, -…, -…, -…, -… or -… is a man's, written in the
	// nominative («…»); its feminine forms say nothing on their own.
	if (reading.gender == Gender::Unknown)
		for (const std::string &word : words) {
			if (!is_cyrillic_word(word)) continue;
			const std::string lower = lowered(word);
			for (const char *ending : {"\xD0\xBE\xD0\xB2", "\xD0\xB5\xD0\xB2", "\xD1\x91\xD0\xB2", "\xD0\xB8\xD0\xBD",
					"\xD1\x8B\xD0\xBD", "\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", "\xD1\x86\xD0\xBA\xD0\xB8\xD0\xB9"})
				if (ends_with(lower, ending) && lower.size() >= std::string(ending).size() + 4) reading.gender = Gender::Masculine;
			if (reading.gender != Gender::Unknown) break;
		}
	// Forms only a woman's surname takes («…»), or only a
	// man's («…»).
	if (reading.gender == Gender::Unknown)
		for (const std::string &word : words) {
			if (!is_cyrillic_word(word)) continue;
			const std::string lower = lowered(word);
			for (const char *ending : {"\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB9", "\xD1\x91\xD0\xB2\xD0\xBE\xD0\xB9", "\xD0\xB8\xD0\xBD\xD0\xBE\xD0\xB9", "\xD1\x8B\xD0\xBD\xD0\xBE\xD0\xB9"})  // …
				if (ends_with(lower, ending) && lower.size() >= std::string(ending).size() + 4) reading.gender = Gender::Feminine;
			for (const char *ending : {"\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xBC", "\xD0\xB5\xD0\xB2\xD1\x8B\xD0\xBC", "\xD1\x91\xD0\xB2\xD1\x8B\xD0\xBC", "\xD0\xB8\xD0\xBD\xD1\x8B\xD0\xBC", "\xD1\x8B\xD0\xBD\xD1\x8B\xD0\xBC", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x81\xD0\xBA\xD0\xBE\xD0\xBC\xD1\x83", "\xD1\x86\xD0\xBA\xD0\xBE\xD0\xBC\xD1\x83", "\xD1\x81\xD0\xBA\xD0\xB8\xD0\xBC", "\xD1\x86\xD0\xBA\xD0\xB8\xD0\xBC"})  // …
				if (ends_with(lower, ending) && lower.size() >= std::string(ending).size() + 4) reading.gender = Gender::Masculine;
			if (reading.gender != Gender::Unknown) break;
		}
	if (known) reading.gender = given;
	// The word before a patronymic is the first name, known to Singularity or not.
	std::optional<std::size_t> writtenCase;
	Gender patronymicGender = Gender::Unknown;
	if (patronymicAt) {
		const auto patronymic = patronymic_nominative(lowered(words[*patronymicAt]));
		patronymicGender = patronymic->second;
		writtenCase = form_index(patronymic_forms(patronymic->first, patronymic->second), lowered(words[*patronymicAt]));
	}
	// Without a patronymic, a known first name shows the case the name is written in.
	if (!writtenCase)
		for (const std::string &word : words)
			if (is_cyrillic_word(word))
				if (const auto name = recognised_first_name(lowered(word))) { writtenCase = form_index(name->first, lowered(word)); break; }
	// A surname in the reading's gender: its regular pattern, or a man's ending in a consonant.
	const auto surname = [&](const std::string &lower) -> std::optional<Forms> {
		if (auto forms = surname_forms(lower, reading.gender)) return forms;
		if (reading.gender == Gender::Masculine) return consonant_surname_forms(lower, writtenCase.value_or(0));
		return std::nullopt;
	};
	// Two name words and nothing else to tell them apart («…»,
	// «…»): the surer surname is the surname, and the other word the
	// first name, known to Singularity or not, in the surname's case. Two words
	// either of which could be the surname («…») are left as written:
	// a first name declined as a surname («…») is a wrong form.
	std::optional<std::size_t> firstAt;
	bool undecided = false;
	if (!patronymicAt && (reading.gender == Gender::Masculine || reading.gender == Gender::Feminine)) {
		std::vector<std::size_t> candidates;
		bool other = false;
		for (std::size_t index = 0; index < words.size(); ++index) {
			const std::string lower = lowered(words[index]);
			if (!is_cyrillic_word(words[index]) || recognised_first_name(lower)) other = true;
			else candidates.push_back(index);
		}
		if (!other && candidates.size() == 2) {
			const std::string one = lowered(words[candidates[0]]);
			const std::string two = lowered(words[candidates[1]]);
			const int oneRank = surname_rank(one, reading.gender);
			const int twoRank = surname_rank(two, reading.gender);
			if (oneRank != twoRank) {
				const bool firstIsSurname = oneRank > twoRank;
				firstAt = candidates[firstIsSurname ? 1 : 0];
				const std::string &surnameWord = firstIsSurname ? one : two;
				if (const auto forms = surname_forms(surnameWord, reading.gender)) writtenCase = form_index(*forms, surnameWord);
			} else if (oneRank > 0) {
				undecided = true;
			}
		}
	}
	std::size_t declinable = 0;
	for (std::size_t index = 0; index < words.size(); ++index) {
		const std::string &word = words[index];
		if (is_initial(word)) { anyName = true; continue; }
		if (firstAt && index == *firstAt) {
			const auto name = unknown_first_name_forms(lowered(word), reading.gender, writtenCase.value_or(0));
			if (!name) { undecided = true; continue; }
			reading.forms[index] = name;
			anyName = true;
			++declinable;
			continue;
		}
		// A double surname declines part by part: «…».
		if (word.find('-') != std::string::npos) {
			std::vector<Forms> partForms;
			const std::string lower = lowered(word);
			for (std::size_t from = 0;;) {
				const std::size_t dash = lower.find('-', from);
				const std::string part = lower.substr(from, dash == std::string::npos ? std::string::npos : dash - from);
				const auto forms = is_cyrillic_word(part) ? surname(part) : std::nullopt;
				if (!forms) { partForms.clear(); break; }
				partForms.push_back(*forms);
				if (dash == std::string::npos) break;
				from = dash + 1;
			}
			if (partForms.size() >= 2) {
				Forms joinedForms;
				for (std::size_t form = 0; form < 6; ++form)
					for (std::size_t part = 0; part < partForms.size(); ++part) joinedForms[form] += (part == 0 ? "" : "-") + partForms[part][form];
				reading.forms[index] = joinedForms;
				++declinable;
			}
			continue;
		}
		if (!is_cyrillic_word(word)) continue;
		const std::string lower = lowered(word);
		if (patronymicAt && index == *patronymicAt) {
			const auto patronymic = patronymic_nominative(lower);
			reading.forms[index] = patronymic_forms(patronymic->first, patronymic->second);
			++declinable;
			continue;
		}
		if (const auto name = recognised_first_name(lower)) {
			reading.forms[index] = name->first;
			++declinable;
			continue;
		}
		if (patronymicAt && index + 1 == *patronymicAt) {
			if (const auto name = unknown_first_name_forms(lower, patronymicGender, *writtenCase)) {
				reading.forms[index] = name;
				++declinable;
				continue;
			}
		}
		if (const auto forms = surname(lower)) {
			reading.forms[index] = forms;
			++declinable;
		}
	}
	reading.person = (anyName || known) && declinable > 0 && words.size() <= 4 && !undecided;
	return reading;
}

} // namespace detail

// "…", "gen" ... - the case a statement asks for.
inline std::optional<Case> case_from_word(const std::string &word) {
	const std::string lower = detail::lowered(word);
	const auto starts = [&](const char *prefix) { return lower.rfind(prefix, 0) == 0; };
	if (lower == "nom" || starts("\xD0\xB8\xD0\xBC") || lower == "\xD0\xB8") return Case::Nominative;                  // …
	if (lower == "gen" || starts("\xD1\x80\xD0\xBE\xD0\xB4") || lower == "\xD1\x80") return Case::Genitive;           // …
	if (lower == "dat" || starts("\xD0\xB4\xD0\xB0\xD1\x82") || lower == "\xD0\xB4") return Case::Dative;            // …
	if (lower == "acc" || starts("\xD0\xB2\xD0\xB8\xD0\xBD") || lower == "\xD0\xB2") return Case::Accusative;        // …
	if (lower == "ins" || starts("\xD1\x82\xD0\xB2") || lower == "\xD1\x82") return Case::Instrumental;            // …
	if (lower == "prep" || starts("\xD0\xBF\xD1\x80") || lower == "\xD0\xBF") return Case::Prepositional;         // …
	return std::nullopt;
}

// The grammatical gender a name or an organisation takes: a person's from the
// patronymic or a known first name, an organisation's from its form, in any
// case («…» - neuter; «…» - feminine).
// Unknown otherwise.
inline Gender gender_of(const std::string &value) {
	const std::vector<std::string> words = detail::words_of(value);
	if (words.empty()) return Gender::Unknown;
	const std::string first = detail::lowered(words.front());
	for (const char *neuter : {"\xD0\xBE\xD0\xB1\xD1\x89\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE",             // …
			"\xD0\xB0\xD0\xBA\xD1\x86\xD0\xB8\xD0\xBE\xD0\xBD\xD0\xB5\xD1\x80\xD0\xBD\xD0\xBE\xD0\xB5",            // …
			"\xD0\xBF\xD1\x83\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x87\xD0\xBD\xD0\xBE\xD0\xB5",                            // …
			"\xD0\xBD\xD0\xB5\xD0\xBF\xD1\x83\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x87\xD0\xBD\xD0\xBE\xD0\xB5",            // …
			"\xD0\xBE\xD0\xBE\xD0\xBE", "\xD0\xB0\xD0\xBE", "\xD0\xBf\xD0\xB0\xD0\xBE", "\xD0\xB7\xD0\xB0\xD0\xBE",  // …
			"\xD0\xBD\xD0\xB0\xD0\xBE", "\xD0\xBE\xD0\xB0\xD0\xBE"})  // …
		if (first == neuter) return Gender::Neuter;
	if (first == "\xD0\xBA\xD0\xBE\xD0\xBC\xD0\xBF\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8F") return Gender::Feminine;  // …
	if (const auto head = detail::organisation_head(words)) return head->second.gender;
	std::vector<std::string> person = words;
	if (first == "\xD0\xB8\xD0\xBF") person.erase(person.begin());                                                // …
	return detail::read_person(person).gender;
}

// `value` in the case asked for: a Russian person's name, alone or after
// «…», or an organisation named by its form,
// written in any case («…»»,
// «…»», «…»»; … never changes).
// `gender` is the person's, when the caller knows it from the fullest writing
// of the name: it decides what the words cannot («…» is a woman,
// «…» a man in the genitive). Anything Singularity cannot decline with
// certainty is returned as it is.
inline std::string declined(const std::string &value, Case target, Gender gender) {
	std::vector<std::string> words = detail::words_of(value);
	if (words.empty()) return value;
	const std::size_t index = detail::index_of(target);
	const std::string first = detail::lowered(words.front());
	// Organisations named by their form: the adjectives and the noun.
	if (const auto head = detail::organisation_head(words)) {
		const auto &[at, noun] = *head;
		for (std::size_t adjective = 0; adjective < at; ++adjective) {
			const std::string lower = detail::lowered(words[adjective]);
			const std::size_t stem = *detail::organisation_adjective_stem(lower, noun.gender);
			words[adjective] = detail::shaped_like(words[adjective],
				detail::organisation_adjective_form(lower.substr(0, stem), noun.gender, index));
		}
		const std::string lower = detail::lowered(words[at]);
		words[at] = detail::shaped_like(words[at], lower.substr(0, noun.stem) + noun.endings[index]);
		return detail::joined(words);
	}
	static const std::array<const char *, 6> masculineAdjective{"\xD1\x8B\xD0\xB9", "\xD0\xBE\xD0\xB3\xD0\xBE", "\xD0\xBE\xD0\xBC\xD1\x83",
		"\xD0\xBE\xD0\xB3\xD0\xBE", "\xD1\x8B\xD0\xBC", "\xD0\xBE\xD0\xBC"};
	// «…» and «…» before a person's name.
	std::size_t personStart = 0;
	if (words.size() >= 3 && first == "\xD0\xB8\xD0\xBD\xD0\xB4\xD0\xB8\xD0\xB2\xD0\xB8\xD0\xB4\xD1\x83\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD\xD1\x8B\xD0\xB9"
			&& detail::lowered(words[1]) == "\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB8\xD0\xBC\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C") {
		words[0] = words[0].substr(0, words[0].size() - 4) + masculineAdjective[index];
		static const std::array<const char *, 6> soft{"\xD1\x8C", "\xD1\x8F", "\xD1\x8E", "\xD1\x8F", "\xD0\xB5\xD0\xBC", "\xD0\xB5"};
		words[1] = words[1].substr(0, words[1].size() - 2) + soft[index];
		personStart = 2;
	} else if (first == "\xD0\xB8\xD0\xBF") {  // …
		personStart = 1;
	}
	if (personStart >= words.size()) return value;
	std::vector<std::string> person(words.begin() + static_cast<std::ptrdiff_t>(personStart), words.end());
	const detail::PersonReading reading = detail::read_person(person, gender);
	if (!reading.person || reading.gender == Gender::Unknown) return value;
	for (std::size_t word = 0; word < person.size(); ++word)
		if (reading.forms[word]) person[word] = detail::shaped_like(person[word], (*reading.forms[word])[index]);
	for (std::size_t word = 0; word < person.size(); ++word) words[personStart + word] = person[word];
	return detail::joined(words);
}

inline std::string declined(const std::string &value, Case target) { return declined(value, target, Gender::Unknown); }

// A person's name taken apart, each part as written (in whatever case):
// surname, first name, patronymic, and initials («…» or «…»).
struct NameParts {
	std::string surname;
	std::string first;
	std::string patronymic;
	std::vector<std::string> initials;
};

// Nothing when `value` does not read as one person's name: more than four
// words, a word that is not Cyrillic, or two words either of which could be
// the surname.
inline std::optional<NameParts> name_parts(const std::string &value) {
	const std::vector<std::string> words = detail::words_of(value);
	if (words.empty() || words.size() > 4) return std::nullopt;
	NameParts parts;
	// «…»: the patronymic is the -… word after the first name.
	const std::optional<std::size_t> patronymicAt = detail::patronymic_position(words);
	std::vector<std::size_t> others;
	for (std::size_t index = 0; index < words.size(); ++index) {
		const std::string &word = words[index];
		if (detail::is_initial(word)) { parts.initials.push_back(word); continue; }
		std::string letters = word;
		letters.erase(std::remove(letters.begin(), letters.end(), '-'), letters.end());
		if (!detail::is_cyrillic_word(letters)) return std::nullopt;
		const std::string lower = detail::lowered(word);
		if (patronymicAt && index == *patronymicAt) { parts.patronymic = word; continue; }
		if (parts.first.empty() && detail::recognised_first_name(lower)) { parts.first = word; continue; }
		others.push_back(index);
	}
	// The word before a patronymic is the first name, known to Singularity or not.
	if (patronymicAt && parts.first.empty() && *patronymicAt > 0) {
		const auto before = std::find(others.begin(), others.end(), *patronymicAt - 1);
		if (before != others.end()) { parts.first = words[*before]; others.erase(before); }
	}
	if (others.size() == 2 && parts.first.empty() && parts.initials.empty()) {
		// «…»: the surname is the word with the surer surname's pattern.
		int ranks[2] = {0, 0};
		for (std::size_t at = 0; at < 2; ++at) {
			const std::string lower = detail::lowered(words[others[at]]);
			ranks[at] = std::max(detail::surname_rank(lower, Gender::Masculine), detail::surname_rank(lower, Gender::Feminine));
		}
		if (ranks[0] == ranks[1]) return std::nullopt;
		const std::size_t surname = ranks[0] > ranks[1] ? 0 : 1;
		parts.surname = words[others[surname]];
		parts.first = words[others[1 - surname]];
		return parts;
	}
	if (others.size() > 1) return std::nullopt;
	if (others.size() == 1) parts.surname = words[others.front()];
	if (parts.surname.empty() && parts.first.empty()) return std::nullopt;
	return parts;
}

// Whether two writings can be one Russian surname in two of its cases: equal,
// or declined in one gender's regular pattern from the same nominative
// («…» and «…» and «…»). Which person a surname
// alone belongs to is not decided here: «…» is his dative and her
// accusative alike; the rest of the name decides.
inline bool same_surname(const std::string &left, const std::string &right) {
	const std::string a = detail::lowered(left);
	const std::string b = detail::lowered(right);
	if (a == b) return true;
	if (!detail::is_cyrillic_word(a) || !detail::is_cyrillic_word(b)) return false;
	for (const Gender gender : {Gender::Masculine, Gender::Feminine}) {
		const std::optional<detail::Forms> one = detail::surname_forms(a, gender);
		const std::optional<detail::Forms> two = detail::surname_forms(b, gender);
		if (!one || !two || (*one)[0] != (*two)[0]) continue;
		const bool aIsForm = std::find(one->begin(), one->end(), a) != one->end();
		const bool bIsForm = std::find(two->begin(), two->end(), b) != two->end();
		if (aIsForm && bIsForm) return true;
	}
	return false;
}

} // namespace singularity::context::russian_names
