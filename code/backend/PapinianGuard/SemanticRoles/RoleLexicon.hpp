#pragma once

#include "PapinianGuard/CommonWords/CommonWords.hpp"

#include <utf8proc.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// The role lexicons (DESIGN4 §5.2): the legal-general words a label, a table
// header or a clause caption names a figure's role with - "Liability Cap",
// «Ставка неустойки», "Paid" - each under its closed universal class (LIMIT,
// PENALTY, PAID ...), and the plain vocabulary of a table's labels ("Month",
// «Премия», "Net Sales") under VOCAB. Data files beside this header
// (role_lexicon_en.txt, role_lexicon_ru.txt), staged into the app's Resources;
// never a name of a party, a product or a place.
//
// Header-only, so Papinian Guard's detection (the owner-approved label
// exemption, DESIGN4 §6.8) and SemanticRoles read one lexicon without either
// linking the other. When no file can be read the lexicon is empty: no role
// class is named and no label is let through - the masking stays as it is.
namespace singularity::protected_api::papinian::roles {

// One phrase of a class: its words' stems, folded ("liability", "cap";
// «ставк», «неустойк»).
struct LexiconPhrase {
	std::string role_class;
	std::vector<std::string> stems;
	// Its order in the files: the earlier class wins a tie.
	std::size_t order = 0;
};

struct RoleLexicon {
	std::vector<LexiconPhrase> phrases;
	std::vector<std::filesystem::path> sources;
};

namespace lexicon_detail {

// Case-folded (utf8proc), "ё" read as "е": how every label and stem is compared.
inline std::string fold(std::string_view text) {
	utf8proc_uint8_t *folded = nullptr;
	const utf8proc_ssize_t size = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
		static_cast<utf8proc_ssize_t>(text.size()), &folded,
		static_cast<utf8proc_option_t>(UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE | UTF8PROC_STABLE));
	if (size < 0 || folded == nullptr) return std::string(text);
	std::string result(reinterpret_cast<const char *>(folded), static_cast<std::size_t>(size));
	std::free(folded);
	for (std::size_t at = result.find("\xD1\x91"); at != std::string::npos; at = result.find("\xD1\x91", at))
		result.replace(at, 2, "\xD0\xB5");
	return result;
}

// Words of a text, folded: letters and digits of any script; everything else
// separates. "Base Rent", «Ставка неустойки (по договору)» -> base, rent;
// ставка, неустойки, по, договору.
inline std::vector<std::string> folded_words(std::string_view text) {
	const std::string folded = fold(text);
	std::vector<std::string> words;
	std::string word;
	std::size_t at = 0;
	while (at < folded.size()) {
		utf8proc_int32_t code = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(folded.data()) + at,
			static_cast<utf8proc_ssize_t>(folded.size() - at), &code);
		if (consumed <= 0) {
			++at;
			continue;
		}
		const auto category = utf8proc_category(code);
		const bool letter = category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LU
			|| category == UTF8PROC_CATEGORY_LO || category == UTF8PROC_CATEGORY_ND || code == '\'' || code == '-';
		if (letter) {
			word.append(folded, at, static_cast<std::size_t>(consumed));
		} else if (!word.empty()) {
			while (!word.empty() && (word.back() == '-' || word.back() == '\'')) word.pop_back();
			if (!word.empty()) words.push_back(word);
			word.clear();
		}
		at += static_cast<std::size_t>(consumed);
	}
	while (!word.empty() && (word.back() == '-' || word.back() == '\'')) word.pop_back();
	if (!word.empty()) words.push_back(word);
	return words;
}

inline std::vector<std::filesystem::path> candidate_directories() {
	std::vector<std::filesystem::path> directories;
	if (const char *override_path = std::getenv("SINGULARITY_ROLE_LEXICON_DIR"))
		if (*override_path != '\0') directories.emplace_back(override_path);
	const std::filesystem::path beside = common_words_detail::executable_directory();
	if (!beside.empty()) {
		directories.push_back(beside.parent_path() / "Resources");
		directories.push_back(beside);
		directories.push_back(beside.parent_path() / "share" / "singularity");
	}
#ifdef NATIVE_APP_ROOT_DIR
	directories.push_back(std::filesystem::path(NATIVE_APP_ROOT_DIR) / "code" / "backend" / "PapinianGuard" / "SemanticRoles");
#endif
	// The source tree this was built from: a development build and every test.
	directories.push_back(std::filesystem::path(__FILE__).parent_path());
	return directories;
}

// "CLASS: phrase | phrase | ..." per line; "#" begins a comment.
inline void read_into(const std::filesystem::path &path, RoleLexicon &lexicon) {
	std::ifstream file(path);
	if (!file.is_open()) return;
	std::string line;
	bool any = false;
	while (std::getline(file, line)) {
		const std::size_t hash = line.find('#');
		if (hash != std::string::npos) line.erase(hash);
		const std::size_t colon = line.find(':');
		if (colon == std::string::npos) continue;
		std::string role_class = line.substr(0, colon);
		while (!role_class.empty() && role_class.back() == ' ') role_class.pop_back();
		while (!role_class.empty() && role_class.front() == ' ') role_class.erase(0, 1);
		if (role_class.empty()) continue;
		const std::string rest = line.substr(colon + 1);
		std::size_t from = 0;
		while (from <= rest.size()) {
			const std::size_t bar = rest.find('|', from);
			const std::string phrase = rest.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
			std::vector<std::string> stems = folded_words(phrase);
			if (!stems.empty()) {
				lexicon.phrases.push_back({role_class, std::move(stems), lexicon.phrases.size()});
				any = true;
			}
			if (bar == std::string::npos) break;
			from = bar + 1;
		}
	}
	if (any) lexicon.sources.push_back(path);
}

} // namespace lexicon_detail

// Both languages' lexicons, read once.
[[nodiscard]] inline const RoleLexicon &role_lexicon() {
	static const RoleLexicon lexicon = [] {
		RoleLexicon loading;
		for (const char *name : {"role_lexicon_en.txt", "role_lexicon_ru.txt"})
			for (const std::filesystem::path &directory : lexicon_detail::candidate_directories()) {
				const std::size_t before = loading.phrases.size();
				lexicon_detail::read_into(directory / name, loading);
				if (loading.phrases.size() > before) break;
			}
		return loading;
	}();
	return lexicon;
}

// Whether a folded word is a form of a folded stem: the stem itself, or the
// stem with an inflection after it (a Russian ending, an English plural or
// "-ed"/"-ing"): «неустойки» of «неустойк», "invoiced" of "invoice".
[[nodiscard]] inline bool word_of_stem(const std::string &word, const std::string &stem) {
	if (word == stem) return true;
	if (stem.size() < 3 || word.size() <= stem.size() || word.compare(0, stem.size(), stem) != 0) return false;
	// An English word takes only an inflection after its stem ("fees",
	// "invoiced", "billing"): "counterparts" is no form of "count", nor
	// "keystone" of "key".
	const bool latin = static_cast<unsigned char>(stem.back()) < 0x80;
	if (latin) {
		const std::string ending = word.substr(stem.size());
		for (const char *allowed : {"s", "es", "ed", "d", "ing", "ly", "'s"})
			if (ending == allowed) return true;
		return false;
	}
	return word.size() - stem.size() <= 8;
}

// Whether a folded word is a word of the lexicon (any class, VOCAB included).
[[nodiscard]] inline bool lexicon_word(const std::string &folded) {
	for (const LexiconPhrase &phrase : role_lexicon().phrases)
		for (const std::string &stem : phrase.stems)
			if (word_of_stem(folded, stem)) return true;
	return false;
}

} // namespace singularity::protected_api::papinian::roles
