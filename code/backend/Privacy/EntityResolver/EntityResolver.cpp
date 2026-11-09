#include "Context/RussianNames/RussianNames.hpp"
#include "PapinianGuard/CommonWords/CommonWords.hpp"
#include "Privacy/EntityResolver/EntityResolver.hpp"

#include "Privacy/EntityRegistry/EntityRegistry.hpp"

#include "Platform/TextFold/TextFold.hpp"

#include <set>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace singularity::privacy {
namespace {

using protected_api::papinian::EntityType;

// Legal forms a name may carry, as written after case folding, each mapped to
// its class: "Ltd" and "Limited" are the same form, "plc" and "Limited" are not.
// Trailing forms (Latin and Cyrillic) are matched as whole tokens at the end of
// the name, leading forms (Cyrillic) at its start. Longest first.
struct LegalForm {
	const char *text;
	const char *form_class;
};

const std::array<LegalForm, 24> trailingLegalForms = {{
		{"l.l.c.", "llc"}, {"l.l.c", "llc"}, {"llc", "llc"},
		{"ltd.", "ltd"}, {"ltd", "ltd"}, {"limited", "ltd"},
		{"incorporated", "inc"}, {"inc.", "inc"}, {"inc", "inc"},
		{"corporation", "corp"}, {"corp.", "corp"}, {"corp", "corp"},
		{"gmbh", "gmbh"}, {"plc", "plc"}, {"llp", "llp"}, {"s.a.", "sa"}, {"sa", "sa"},
		{"\320\276\320\276\320\276", "ooo"}, {"\320\267\320\260\320\276", "zao"}, {"\320\276\320\260\320\276", "oao"}, {"\320\277\320\260\320\276", "pao"}, {"\320\260\320\276", "ao"}, {"\320\270\320\277", "ip"},
		{"\320\275\320\272\320\276", "nko"},
}};

const std::array<LegalForm, 12> leadingLegalForms = {{
		{"\320\276\320\261\321\211\320\265\321\201\321\202\320\262\320\276 \321\201 \320\276\320\263\321\200\320\260\320\275\320\270\321\207\320\265\320\275\320\275\320\276\320\271 \320\276\321\202\320\262\320\265\321\202\321\201\321\202\320\262\320\265\320\275\320\275\320\276\321\201\321\202\321\214\321\216", "ooo"},
		{"\320\277\321\203\320\261\320\273\320\270\321\207\320\275\320\276\320\265 \320\260\320\272\321\206\320\270\320\276\320\275\320\265\321\200\320\275\320\276\320\265 \320\276\320\261\321\211\320\265\321\201\321\202\320\262\320\276", "pao"},
		{"\320\267\320\260\320\272\321\200\321\213\321\202\320\276\320\265 \320\260\320\272\321\206\320\270\320\276\320\275\320\265\321\200\320\275\320\276\320\265 \320\276\320\261\321\211\320\265\321\201\321\202\320\262\320\276", "zao"},
		{"\320\276\321\202\320\272\321\200\321\213\321\202\320\276\320\265 \320\260\320\272\321\206\320\270\320\276\320\275\320\265\321\200\320\275\320\276\320\265 \320\276\320\261\321\211\320\265\321\201\321\202\320\262\320\276", "oao"},
		{"\320\275\320\265\320\277\321\203\320\261\320\273\320\270\321\207\320\275\320\276\320\265 \320\260\320\272\321\206\320\270\320\276\320\275\320\265\321\200\320\275\320\276\320\265 \320\276\320\261\321\211\320\265\321\201\321\202\320\262\320\276", "ao"},
		{"\320\260\320\272\321\206\320\270\320\276\320\275\320\265\321\200\320\275\320\276\320\265 \320\276\320\261\321\211\320\265\321\201\321\202\320\262\320\276", "ao"},
		{"\320\270\320\275\320\264\320\270\320\262\320\270\320\264\321\203\320\260\320\273\321\214\320\275\321\213\320\271 \320\277\321\200\320\265\320\264\320\277\321\200\320\270\320\275\320\270\320\274\320\260\321\202\320\265\320\273\321\214", "ip"},
		{"\320\276\320\276\320\276", "ooo"}, {"\320\267\320\260\320\276", "zao"}, {"\320\276\320\260\320\276", "oao"}, {"\320\277\320\260\320\276", "pao"}, {"\320\260\320\276", "ao"},
}};

std::string folded(const std::string &value, bool strip_marks) {
	return platform::folded_text(value, strip_marks);
}

std::string trimmed(const std::string &value) {
	const auto first = value.find_first_not_of(" \t\r\n.,;:");
	if (first == std::string::npos) return "";
	const auto last = value.find_last_not_of(" \t\r\n.,;:");
	return value.substr(first, last - first + 1);
}

// Quotation marks never distinguish a name: «…» and … are
// one surface form. Commas become spaces so a form after a comma is still a
// trailing token ("Example Holdings, LLC").
std::string without_quotes_and_commas(const std::string &value) {
	static const std::array<const char *, 10> quotes = {"\xC2\xAB", "\xC2\xBB", "\xE2\x80\x9C", "\xE2\x80\x9D",
			"\xE2\x80\x9E", "\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9A", "\"", "`"};
	std::string result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size();) {
		bool matched = false;
		for (const char *quote : quotes) {
			const std::size_t length = std::char_traits<char>::length(quote);
			if (value.compare(index, length, quote) == 0) {
				result.push_back(' ');
				index += length;
				matched = true;
				break;
			}
		}
		if (matched) continue;
		result.push_back(value[index] == ',' ? ' ' : value[index]);
		++index;
	}
	return result;
}

std::string collapsedWhitespace(const std::string &value) {
	std::string result;
	bool previousWasSpace = false;
	for (const char character : value) {
		const bool isSpace = character == ' ' || character == '\t' || character == '\n' || character == '\r';
		if (isSpace == true) {
			if (previousWasSpace == false && result.empty() == false) result += ' ';
			previousWasSpace = true;
			continue;
		}
		result += character;
		previousWasSpace = false;
	}
	while (result.empty() == false && result.back() == ' ') result.pop_back();
	return result;
}

// A name split into its base and legal-form class ("" when it carries none).
struct NameParts {
	std::string base;
	std::string form_class;
};

NameParts split_legal_form(const std::string &surface) {
	NameParts parts{surface, ""};
	for (const LegalForm &form : leadingLegalForms) {
		const std::string leading = std::string(form.text) + " ";
		if (parts.base.rfind(leading, 0) == 0 && parts.base.size() > leading.size()) {
			parts.base = trimmed(parts.base.substr(leading.size()));
			parts.form_class = form.form_class;
			break;
		}
	}
	if (!parts.form_class.empty()) return parts;
	for (const LegalForm &form : trailingLegalForms) {
		const std::string token(form.text);
		if (parts.base.size() <= token.size() + 1) continue;
		if (parts.base.compare(parts.base.size() - token.size(), token.size(), token) != 0) continue;
		if (parts.base[parts.base.size() - token.size() - 1] != ' ') continue;
		parts.base = trimmed(parts.base.substr(0, parts.base.size() - token.size()));
		parts.form_class = form.form_class;
		break;
	}
	return parts;
}

NameParts name_parts(const std::string &value) {
	return split_legal_form(surface_alias_key(value));
}

bool compatible_legal_forms(const std::string &a, const std::string &b) {
	return a.empty() || b.empty() || a == b;
}

// A minimal, explicit Cyrillic-to-Latin transliteration table covering the
// letters that actually appear in Russian legal-entity/person names - matching
// policy, applied after Unicode case folding, so only lower-case letters remain.
std::string transliteratedCyrillic(const std::string &value) {
	static const std::vector<std::pair<std::string, std::string>> table = {
			{"\321\221", "e"}, {"\320\266", "zh"}, {"\321\206", "ts"}, {"\321\207", "ch"}, {"\321\210", "sh"}, {"\321\211", "shch"},
			{"\321\216", "yu"}, {"\321\217", "ya"}, {"\321\205", "kh"}, {"\320\260", "a"}, {"\320\261", "b"}, {"\320\262", "v"},
			{"\320\263", "g"}, {"\320\264", "d"}, {"\320\265", "e"}, {"\320\267", "z"}, {"\320\270", "i"}, {"\320\271", "i"},
			{"\320\272", "k"}, {"\320\273", "l"}, {"\320\274", "m"}, {"\320\275", "n"}, {"\320\276", "o"}, {"\320\277", "p"},
			{"\321\200", "r"}, {"\321\201", "s"}, {"\321\202", "t"}, {"\321\203", "u"}, {"\321\204", "f"}, {"\321\213", "y"},
			{"\321\215", "e"}, {"\321\212", ""}, {"\321\214", ""},
	};
	std::string result;
	std::size_t index = 0;
	while (index < value.size()) {
		bool matched = false;
		for (const auto &entry : table) {
			const std::string &from = entry.first;
			if (value.compare(index, from.size(), from) == 0) {
				result += entry.second;
				index += from.size();
				matched = true;
				break;
			}
		}
		if (matched == false) {
			result += value[index];
			index += 1;
		}
	}
	return result;
}

// Nearby identifiers tell two people with one name apart: two "John Smith"s
// with different emails are two people. They never split an organisation, a
// place, a date or an amount - one company is named beside many phone numbers,
// emails and accounts across a data room, and the same full name of any of
// those is the same thing.
bool contextKeysConflict(const ProjectEntity &entity, const DetectedMention &mention) {
	if (mention.category != EntityType::Person && mention.category != EntityType::Judge) return false;
	if (mention.context_key.empty() || entity.context_keys.empty()) return false;
	return std::find(entity.context_keys.begin(), entity.context_keys.end(), mention.context_key) == entity.context_keys.end();
}

void addContextKey(ProjectEntity &entity, const std::string &context_key) {
	if (context_key.empty()) return;
	if (std::find(entity.context_keys.begin(), entity.context_keys.end(), context_key) != entity.context_keys.end()) {
		return;
	}
	entity.context_keys.push_back(context_key);
}

bool rejects(const ProjectEntity &entity, const std::string &surface) {
	return std::find(entity.rejected_aliases.begin(), entity.rejected_aliases.end(), surface) != entity.rejected_aliases.end();
}

bool has_surface_alias(const ProjectEntity &entity, const std::string &surface) {
	return std::any_of(entity.aliases.begin(), entity.aliases.end(),
			[&](const std::string &alias) { return surface_alias_key(alias) == surface; });
}

bool has_locked_surface_alias(const ProjectEntity &entity, const std::string &surface) {
	return std::any_of(entity.locked_aliases.begin(), entity.locked_aliases.end(),
			[&](const std::string &alias) { return surface_alias_key(alias) == surface; });
}

bool has_compatible_alias(const ProjectEntity &entity, const NameParts &parts) {
	return std::any_of(entity.aliases.begin(), entity.aliases.end(), [&](const std::string &alias) {
		const NameParts aliasParts = name_parts(alias);
		return aliasParts.base == parts.base && compatible_legal_forms(aliasParts.form_class, parts.form_class);
	});
}

// Defined below, once the name-part helpers it reads are in scope.
std::size_t writingFullness(EntityType category, const std::string &value);

void addAlias(ProjectEntity &entity, const std::string &raw_value) {
	if (std::find(entity.aliases.begin(), entity.aliases.end(), raw_value) != entity.aliases.end()) return;
	entity.aliases.push_back(raw_value);
	// The Project reads this entity by its fullest writing, not by whichever one
	// a document happened to use first: "Daniela" read before "Daniela Iancu" is
	// still that person, and the person's own list should say so.
	if (writingFullness(entity.category, raw_value) > writingFullness(entity.category, entity.canonical_value))
		entity.canonical_value = raw_value;
}

// True when `a`/`b` (in either order) already carry a KeptSeparate
// resolution - a prior explicit "Keep separate" user decision persists and
// must never be re-litigated by a later automatic pass raising the same
// ambiguity again.
bool alreadyKeptSeparate(const matters::Matter &matter, const std::string &a, const std::string &b) {
	for (const EntityAmbiguity &ambiguity : matter.entity_ambiguities) {
		if (ambiguity.status != AmbiguityStatus::KeptSeparate) continue;
		const bool sameOrder = ambiguity.entity_a_id == a && ambiguity.entity_b_id == b;
		const bool reverseOrder = ambiguity.entity_a_id == b && ambiguity.entity_b_id == a;
		if (sameOrder || reverseOrder) return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Short forms of a known name.
//
// A data room names the same party many ways: "Brennan Holt", "Mr. Holt",
// "Holt"; "Ashgrove Medical Properties, LLC", "Ashgrove". Each is masked, but a
// model that sees a different symbol for each cannot know the consent in one
// document is owed by the landlord in another - the relation between the
// documents is lost, which is the one thing protected work depends on. A short
// form is therefore attached to the entity it abbreviates, but only when the
// Project leaves no doubt which one that is: exactly one person whose name
// ends in that surname, exactly one organisation whose name begins with those
// words. Two candidates mean no link at all - a separate identity is always
// safer than a wrong merge, and a person can still merge or split either in
// Papinian Guard.

std::vector<std::string> tokensOf(const std::string &surfaceKey) {
	std::vector<std::string> tokens;
	std::string current;
	for (const char character : surfaceKey) {
		if (character == ' ') {
			if (!current.empty()) tokens.push_back(std::move(current));
			current.clear();
		} else {
			current.push_back(character);
		}
	}
	if (!current.empty()) tokens.push_back(std::move(current));
	return tokens;
}

std::string withoutPossessive(std::string token) {
	if (token.size() > 2 && token.compare(token.size() - 2, 2, "'s") == 0) token.erase(token.size() - 2);
	while (!token.empty() && (token.back() == '.' || token.back() == '\'')) token.pop_back();
	return token;
}

bool isHonorific(const std::string &token) {
	static const std::set<std::string> honorifics{
		"mr", "mrs", "ms", "miss", "mx", "dr", "prof", "professor", "sir", "madam", "hon", "judge", "justice",
		"atty", "attorney", "\320\263-\320\275", "\320\263-\320\266\320\260", "\320\263\320\276\321\201\320\277\320\276\320\264\320\270\320\275", "\320\263\320\276\321\201\320\277\320\276\320\266\320\260", "\320\264-\321\200",
	};
	return honorifics.count(token) > 0;
}

// Letters after a name that are a qualification, not part of it.
bool isNameSuffix(const std::string &token) {
	static const std::set<std::string> suffixes{
		"md", "m.d", "do", "d.o", "dds", "d.d.s", "dmd", "d.m.d", "phd", "ph.d", "esq", "jr", "sr", "ii", "iii",
		"iv", "cpa", "rn", "np", "mba", "jd", "j.d", "llm", "ll.m", "mph", "facs", "faan",
	};
	return suffixes.count(token) > 0;
}

// A person's name without titles or qualifications, each word bare:
// "Mr. Holt's" -> {"holt"}; "Victor Pham, M.D." -> {"victor", "pham"}.
// A Russian person's name in one shape - given name, patronymic, surname, each
// in the nominative, an initial kept as its letter - whatever order and case
// the text writes it in: «…» and
// «…» are […] and […]. Nothing
// when the words do not read as one Russian name (live R1, 27 Sep 2026: the
// preamble's name and the signature's initials became two people).
std::optional<std::vector<std::string>> russianNameTokens(const std::string &value) {
	namespace names = singularity::context::russian_names;
	const std::optional<names::NameParts> written = names::name_parts(value);
	if (!written || written->surname.empty()) return std::nullopt;
	// A surname alone is left to the short-form rules.
	if (written->first.empty() && written->patronymic.empty() && written->initials.empty()) return std::nullopt;
	// Declined to the nominative where Singularity can decline it with certainty;
	// as written otherwise (a surname with initials only reads the same).
	const std::optional<names::NameParts> nominative =
		names::name_parts(names::declined(value, names::Case::Nominative, names::gender_of(value)));
	const names::NameParts &parts = nominative && !nominative->surname.empty() ? *nominative : *written;
	// Each initial's letter: «…» or «…».
	std::vector<std::string> initials;
	for (const std::string &written_initials : parts.initials)
		for (std::size_t from = 0; from < written_initials.size();) {
			const std::size_t dot = written_initials.find('.', from);
			const std::size_t end = dot == std::string::npos ? written_initials.size() : dot;
			if (end > from) initials.push_back(written_initials.substr(from, end - from));
			from = end + 1;
		}
	std::vector<std::string> tokens;
	if (!parts.first.empty()) tokens.push_back(parts.first);
	else if (!initials.empty()) tokens.push_back(initials.front());
	if (!parts.patronymic.empty()) tokens.push_back(parts.patronymic);
	else if (initials.size() >= 2) tokens.push_back(initials[1]);
	tokens.push_back(parts.surname);
	if (tokens.size() < 2) return std::nullopt;
	std::vector<std::string> keyed;
	for (const std::string &token : tokens) {
		const std::vector<std::string> folded = tokensOf(surface_alias_key(token));
		if (folded.size() != 1) return std::nullopt;
		keyed.push_back(folded.front());
	}
	return keyed;
}

std::vector<std::string> personNameTokens(const std::string &value) {
	if (const std::optional<std::vector<std::string>> russian = russianNameTokens(value)) return *russian;
	std::vector<std::string> result;
	for (const std::string &token : tokensOf(surface_alias_key(value))) {
		const std::string bare = withoutPossessive(token);
		if (bare.empty() || isHonorific(bare) || isNameSuffix(bare)) continue;
		result.push_back(bare);
	}
	return result;
}

// One letter: "b" from "B.", or one two-byte letter such as a Cyrillic initial.
bool isInitial(const std::string &token) {
	if (token.size() == 1) return true;
	return token.size() == 2 && static_cast<unsigned char>(token[0]) >= 0xC0;
}

// The surname a short mention consists of ("Holt", "Mr. Holt", "B. Holt"), or
// nothing when the mention is not that short.
std::optional<std::string> shortPersonSurname(const std::vector<std::string> &tokens) {
	if (tokens.size() == 1 && tokens[0].size() >= 3) return tokens[0];
	if (tokens.size() == 2 && isInitial(tokens[0]) && tokens[1].size() >= 3) return tokens[1];
	return std::nullopt;
}

bool isFullPersonName(const std::vector<std::string> &tokens) {
	return tokens.size() >= 2 && !shortPersonSurname(tokens).has_value();
}

bool isPersonCategory(EntityType category) {
	return category == EntityType::Person || category == EntityType::Judge;
}

bool isOrganisationCategory(EntityType category) {
	return category == EntityType::Organisation || category == EntityType::LegalEntity;
}

// Words too common in organisation names to identify one on their own.
bool isGenericOrganisationWord(const std::string &token) {
	static const std::set<std::string> generic{
		"the", "and", "of", "for", "new", "first", "national", "united", "american", "general", "global",
		"international", "group", "bank", "health", "medical", "capital", "partners", "holdings", "company",
		"services", "management", "associates", "fund", "trust", "insurance", "financial", "properties",
	};
	return generic.count(token) > 0;
}

std::vector<std::string> organisationBaseTokens(const std::string &value) {
	std::vector<std::string> result;
	for (const std::string &token : tokensOf(name_parts(value).base)) {
		const std::string bare = withoutPossessive(token);
		if (!bare.empty()) result.push_back(bare);
	}
	return result;
}

// How fully a writing names somebody or something: more name parts first, and
// between two writings of the same length the one that is not shouted, because
// a document that sets a name in capitals is styling a heading rather than
// saying the name differently.
std::size_t writingFullness(EntityType category, const std::string &value) {
	const std::vector<std::string> tokens = isPersonCategory(category)
		? personNameTokens(value) : organisationBaseTokens(value);
	const bool shouted = std::none_of(value.begin(), value.end(),
		[](unsigned char character) { return character < 0x80 && std::islower(character) != 0; });
	// A writing that carries the sentence's next word inside it -  "Meridia
	// Clinical Systems support" - is longer without naming the party any more
	// fully, and the Project must not read the party by it. The word has to be
	// written in lower case and be an ordinary word of the language
	// (CommonWords.hpp) for this to fire, so "Larkspur Clinics of Texas" and a
	// made-up brand word are untouched.
	const bool trailingOrdinaryWord = tokens.size() > 1
		&& std::all_of(tokens.back().begin(), tokens.back().end(), [](unsigned char character) {
			return character < 0x80 && std::islower(character) != 0;
		})
		&& singularity::protected_api::papinian::is_common_word(tokens.back());
	if (trailingOrdinaryWord) return (tokens.size() - 1) * 2 + (shouted ? 0 : 1);
	return tokens.size() * 2 + (shouted ? 0 : 1);
}


bool isStrictPrefix(const std::vector<std::string> &shorter, const std::vector<std::string> &longer) {
	if (shorter.empty() || shorter.size() >= longer.size()) return false;
	return std::equal(shorter.begin(), shorter.end(), longer.begin());
}

bool hasDistinctiveWord(const std::vector<std::string> &tokens) {
	return std::any_of(tokens.begin(), tokens.end(), [](const std::string &token) {
		return token.size() >= 4 && !isGenericOrganisationWord(token)
			&& !std::all_of(token.begin(), token.end(), [](unsigned char character) { return std::isdigit(character) != 0; });
	});
}

// "Mr. Pham" and "Ms. Pham" are two people; "Dr. Pham" and "Pham" are not
// known to be. Only titles that say which of the two a person is can conflict.
int titleGender(const std::string &value) {
	for (const std::string &token : tokensOf(surface_alias_key(value))) {
		const std::string bare = withoutPossessive(token);
		if (bare == "mr" || bare == "sir" || bare == "\320\263\320\276\321\201\320\277\320\276\320\264\320\270\320\275" || bare == "\320\263-\320\275") return 1;
		if (bare == "mrs" || bare == "ms" || bare == "miss" || bare == "madam" || bare == "\320\263\320\276\321\201\320\277\320\276\320\266\320\260" || bare == "\320\263-\320\266\320\260") return 2;
	}
	return 0;
}

bool titlesConflict(const std::string &left, const std::string &right) {
	const int leftGender = titleGender(left);
	const int rightGender = titleGender(right);
	return leftGender != 0 && rightGender != 0 && leftGender != rightGender;
}

// Whether `shortTokens` abbreviates `longValue` for this kind of name. For a
// person the same full name with or without a title or a qualification
// ("Nora Whitfield", "Dr. Nora Whitfield", "Nora Whitfield, M.D.") counts too.
// One name part stands for another when they are the same, or when one of them
// is the single initial the other begins with.
bool namePartsAgree(const std::string &left, const std::string &right) {
	if (left == right) return true;
	// An initial is one letter - one byte, or two for a Cyrillic letter - that
	// the other name part begins with.
	if (isInitial(left) && right.size() > left.size()) return right.compare(0, left.size(), left) == 0;
	if (isInitial(right) && left.size() > right.size()) return left.compare(0, right.size(), right) == 0;
	return false;
}

// Two writings of one person's name that differ only in how much of the middle
// is written out: fewer middle names, or an initial where the other writes the
// name in full. The given name and the surname must agree, and every middle
// name of the shorter must stand, in order, in the longer. This is candidacy,
// not a merge: uniqueShortFormPartner() attaches only when exactly one person
// in the Project qualifies, so two people who genuinely share a given name and
// a surname stay apart and go to review.
bool samePersonNameVariant(const std::vector<std::string> &shortTokens, const std::vector<std::string> &longTokens) {
	if (isFullPersonName(shortTokens) == false || isFullPersonName(longTokens) == false) return false;
	if (shortTokens.size() > longTokens.size()) return false;
	if (namePartsAgree(shortTokens.front(), longTokens.front()) == false) return false;
	// A Russian surname in another case is the same surname («…»).
	if (!singularity::context::russian_names::same_surname(shortTokens.back(), longTokens.back())) return false;
	std::size_t at = 1;
	for (std::size_t index = 1; index + 1 < longTokens.size() && at + 1 < shortTokens.size(); ++index)
		if (namePartsAgree(shortTokens[at], longTokens[index])) ++at;
	return at + 1 == shortTokens.size();
}

bool abbreviates(EntityType category, const std::vector<std::string> &shortTokens, const std::string &longValue) {
	if (isPersonCategory(category)) {
		const std::vector<std::string> longTokens = personNameTokens(longValue);
		if (isFullPersonName(shortTokens) && shortTokens == longTokens) return true;
		if (samePersonNameVariant(shortTokens, longTokens)) return true;
		// A given name on its own is the same kind of evidence as a surname on
		// its own, and just as much a candidate for exactly one known name -
		// but only against a name shaped like a person's. A name that carries a
		// legal form, or that runs longer than a given name, a middle name and
		// a surname, is an organisation whatever the detector called it, and
		// its first word is its trade name rather than anybody's given name.
		if (shortTokens.size() == 1 && shortTokens.front().size() >= 3 && isFullPersonName(longTokens)
				&& longTokens.size() <= 3 && legal_form_class(longValue).empty()
				&& longTokens.front() == shortTokens.front())
			return true;
		const auto surname = shortPersonSurname(shortTokens);
		return surname && isFullPersonName(longTokens)
			&& singularity::context::russian_names::same_surname(longTokens.back(), *surname);
	}
	return hasDistinctiveWord(shortTokens) && isStrictPrefix(shortTokens, organisationBaseTokens(longValue));
}

bool sameNameFamily(EntityType left, EntityType right) {
	if (isPersonCategory(left)) return left == right;
	return isOrganisationCategory(left) && isOrganisationCategory(right);
}

// The one entity this mention is a short form of - or the one entity made only
// of short forms of this mention - and nullptr when there is none or more than
// one.
ProjectEntity *uniqueShortFormPartner(matters::Matter &matter, const DetectedMention &mention,
		const std::string &surface) {
	if (!isPersonCategory(mention.category) && !isOrganisationCategory(mention.category)) return nullptr;
	const std::vector<std::string> mentionTokens = isPersonCategory(mention.category)
		? personNameTokens(mention.value) : organisationBaseTokens(mention.value);
	if (mentionTokens.empty()) return nullptr;

	// A surname on its own belongs to everyone in a family, so an entity known
	// by nothing but this mention's surname is evidence of a name, not of a
	// person: it neither takes the mention nor stops the mention from joining
	// the person it belongs to. An entity known by nothing but this mention's
	// given name is the opposite - the same person, read before anybody wrote
	// their surname - so it stays a candidate. Which of the two an entity is
	// does not depend on the shape of the word, only on where that word stands
	// in the name being resolved.
	// How many people the Project knows by that surname. One is a person the
	// Project has so far seen only by their surname; two or more make the
	// surname the family's rather than anybody's, and then it is evidence of a
	// name and not of a person.
	std::size_t peopleWithThisSurname = 0;
	if (isPersonCategory(mention.category) && mentionTokens.size() >= 2) {
		for (const ProjectEntity &entity : matter.entities) {
			if (entity.merged_into_entity_id || !isPersonCategory(entity.category)) continue;
			for (const std::string &alias : entity.aliases) {
				const std::vector<std::string> aliasTokens = personNameTokens(alias);
				if (isFullPersonName(aliasTokens) && aliasTokens.back() == mentionTokens.back()) {
					++peopleWithThisSurname;
					break;
				}
			}
		}
	}
	const auto isTheFamilyName = [&](const ProjectEntity &entity) {
		if (!isPersonCategory(entity.category) || mentionTokens.size() < 2) return false;
		if (peopleWithThisSurname < 2) return false;
		return std::all_of(entity.aliases.begin(), entity.aliases.end(), [&](const std::string &alias) {
			const std::vector<std::string> aliasTokens = personNameTokens(alias);
			return aliasTokens.size() == 1 && aliasTokens.front() == mentionTokens.back();
		});
	};

	ProjectEntity *partner = nullptr;
	std::size_t partners = 0;
	std::vector<ProjectEntity *> candidates;
	for (ProjectEntity &entity : matter.entities) {
		if (entity.merged_into_entity_id || !sameNameFamily(mention.category, entity.category)) continue;
		if (entity.aliases.empty()) continue;
		// The mention abbreviates one of this entity's names...
		bool mentionIsShort = std::any_of(entity.aliases.begin(), entity.aliases.end(),
			[&](const std::string &alias) { return abbreviates(mention.category, mentionTokens, alias); });
		// ...or is the same bare surname this entity is so far only known by
		// ("Dr. Pham", then "Pham"), unless their titles name two people.
		if (!mentionIsShort && isPersonCategory(mention.category) && shortPersonSurname(mentionTokens)) {
			mentionIsShort = std::all_of(entity.aliases.begin(), entity.aliases.end(), [&](const std::string &alias) {
				return personNameTokens(alias) == mentionTokens && !titlesConflict(alias, mention.value);
			});
		}
		// ...or every name this entity has so far abbreviates the mention.
		const bool entityIsShort = std::all_of(entity.aliases.begin(), entity.aliases.end(),
			[&](const std::string &alias) {
				const std::vector<std::string> aliasTokens = isPersonCategory(entity.category)
					? personNameTokens(alias) : organisationBaseTokens(alias);
				return abbreviates(mention.category, aliasTokens, mention.value);
			});
		if (!mentionIsShort && !entityIsShort) continue;
		if (isTheFamilyName(entity)) continue;
		++partners;
		candidates.push_back(&entity);
		// A person removed this form from the entity, or nearby evidence says
		// these are two people: never re-attached automatically.
		if (rejects(entity, surface) || contextKeysConflict(entity, mention)) continue;
		partner = &entity;
	}
	if (partners == 1) return partner;
	// Several candidates are not necessarily several people. When every one of
	// them is itself a writing of the same name - a given name read before the
	// full name, the full name read before the one with the middle spelled out
	// - they are one person the Project has not joined up yet, and the mention
	// belongs with the fullest of them. Anything less uniform is a real
	// ambiguity and is left for a person to decide.
	if (partners > 1) {
		// Two candidates are one name only when the full names they are written
		// by agree. A surname standing alone is not evidence of that: every
		// member of a family abbreviates to it, so comparing by it would make
		// any two relatives look like one person. A candidate the Project knows
		// by nothing but a short form has nothing to compare, and is therefore
		// never taken as the same person as another.
		const auto fullNamesOf = [&](const ProjectEntity &entity) {
			std::vector<std::string> names;
			for (const std::string &alias : entity.aliases) {
				const std::vector<std::string> tokens = isPersonCategory(entity.category)
					? personNameTokens(alias) : organisationBaseTokens(alias);
				const bool full = isPersonCategory(entity.category) ? isFullPersonName(tokens) : tokens.size() > 1;
				if (full) names.push_back(alias);
			}
			return names;
		};
		const auto variantsOfEachOther = [&](const ProjectEntity &left, const ProjectEntity &right) {
			const std::vector<std::string> leftNames = fullNamesOf(left);
			const std::vector<std::string> rightNames = fullNamesOf(right);
			if (leftNames.empty() || rightNames.empty()) return false;
			return std::all_of(leftNames.begin(), leftNames.end(), [&](const std::string &leftAlias) {
				const std::vector<std::string> leftTokens = isPersonCategory(left.category)
					? personNameTokens(leftAlias) : organisationBaseTokens(leftAlias);
				return std::all_of(rightNames.begin(), rightNames.end(), [&](const std::string &rightAlias) {
					const std::vector<std::string> rightTokens = isPersonCategory(right.category)
						? personNameTokens(rightAlias) : organisationBaseTokens(rightAlias);
					return abbreviates(mention.category, leftTokens, rightAlias)
						|| abbreviates(mention.category, rightTokens, leftAlias);
				});
			});
		};
		bool oneName = true;
		for (std::size_t left = 0; left < candidates.size() && oneName; ++left)
			for (std::size_t right = left + 1; right < candidates.size() && oneName; ++right)
				oneName = variantsOfEachOther(*candidates[left], *candidates[right]);
		if (oneName) {
			ProjectEntity *fullest = nullptr;
			for (ProjectEntity *entity : candidates) {
				if (rejects(*entity, surface) || contextKeysConflict(*entity, mention)) continue;
				const std::size_t width = std::accumulate(entity->aliases.begin(), entity->aliases.end(),
					std::size_t{0}, [&](std::size_t widest, const std::string &alias) {
						return std::max(widest, personNameTokens(alias).size());
					});
				const std::size_t best = fullest == nullptr ? 0
					: std::accumulate(fullest->aliases.begin(), fullest->aliases.end(), std::size_t{0},
						[&](std::size_t widest, const std::string &alias) {
							return std::max(widest, personNameTokens(alias).size());
						});
				if (fullest == nullptr || width > best) fullest = entity;
			}
			return fullest;
		}
	}
	return nullptr;
}

std::optional<std::string> pendingAmbiguityBetween(
		const matters::Matter &matter, const std::string &a, const std::string &b) {
	for (const EntityAmbiguity &ambiguity : matter.entity_ambiguities) {
		if (ambiguity.status != AmbiguityStatus::Pending) continue;
		const bool sameOrder = ambiguity.entity_a_id == a && ambiguity.entity_b_id == b;
		const bool reverseOrder = ambiguity.entity_a_id == b && ambiguity.entity_b_id == a;
		if (sameOrder || reverseOrder) return ambiguity.id;
	}
	return std::nullopt;
}

} // namespace

std::string surface_alias_key(const std::string &value) {
	return trimmed(collapsedWhitespace(without_quotes_and_commas(folded(value, false))));
}

std::string normalize_alias(const std::string &value) {
	return name_parts(value).base;
}

std::string legal_form_class(const std::string &value) {
	return name_parts(value).form_class;
}

std::string comparable_key(const std::string &normalized_value) {
	return split_legal_form(collapsedWhitespace(transliteratedCyrillic(folded(normalized_value, true)))).base;
}

ResolutionOutcome resolve_mention(matters::Matter &matter, const DetectedMention &mention, const std::string &timestamp) {
	const std::string normalized = normalize_alias(mention.value);
	if (normalized.empty()) {
		const ProjectEntity created = make_new_entity(matter, mention.category, mention.value, timestamp);
		matter.entities.push_back(created);
		return ResolutionOutcome{ResolutionKind::CreatedNew, created.id, std::nullopt};
	}

	// 1a. The same surface form (case, spacing and quotes aside) an entity
	// already holds - the one a person locked there first - unless that entity
	// rejects it (a person split or removed it) or nearby evidence conflicts.
	const std::string surface = surface_alias_key(mention.value);
	ProjectEntity *surfaceMatch = nullptr;
	for (ProjectEntity &entity : matter.entities) {
		if (entity.merged_into_entity_id || entity.category != mention.category) continue;
		if (rejects(entity, surface) || has_surface_alias(entity, surface) == false) continue;
		if (contextKeysConflict(entity, mention) == true) continue;
		if (surfaceMatch == nullptr || (has_locked_surface_alias(entity, surface)
				&& has_locked_surface_alias(*surfaceMatch, surface) == false))
			surfaceMatch = &entity;
	}
	// 1b. Otherwise the same name with a compatible legal form ("Ltd" and
	// "Limited", or a form on one side only) - attached only when exactly one
	// entity qualifies. Different forms ("plc" and "Limited", "…" and "…") are
	// never merged: they fall through to review below.
	const NameParts parts = name_parts(mention.value);
	std::vector<ProjectEntity *> compatibleMatches;
	if (surfaceMatch == nullptr) {
		for (ProjectEntity &entity : matter.entities) {
			if (entity.merged_into_entity_id || entity.category != mention.category) continue;
			if (rejects(entity, surface) || has_compatible_alias(entity, parts) == false) continue;
			if (contextKeysConflict(entity, mention) == true) continue;
			compatibleMatches.push_back(&entity);
		}
	}
	ProjectEntity *attachTo = surfaceMatch != nullptr ? surfaceMatch
		: compatibleMatches.size() == 1 ? compatibleMatches.front() : nullptr;
	// 1c. Otherwise a short form of exactly one known name, or the full name
	// exactly one known short form abbreviates (see uniqueShortFormPartner).
	if (attachTo == nullptr && compatibleMatches.empty()) attachTo = uniqueShortFormPartner(matter, mention, surface);
	if (attachTo != nullptr) {
		addAlias(*attachTo, mention.value);
		addContextKey(*attachTo, mention.context_key);
		attachTo->updated_at = timestamp;
		return ResolutionOutcome{ResolutionKind::AttachedToExisting, attachTo->id, std::nullopt};
	}

	// 2. Comparable-key candidate match against a different existing entity -
	// never an automatic merge, only grounds for review. The matching
	// entity's id/canonical_value are copied out (never kept as a reference)
	// before this function appends anything to matter.entities/
	// entity_ambiguities below: either append can reallocate the vector and
	// invalidate a reference held into it.
	const std::string key = comparable_key(normalized);
	std::optional<std::string> candidateEntityId;
	std::string candidateCanonicalValue;
	if (key.empty() == false) {
		for (const ProjectEntity &entity : matter.entities) {
			if (entity.merged_into_entity_id || entity.category != mention.category) continue;
			// A person's split already decided this pair: the rejecting entity is
			// not a candidate for the value it gave away.
			if (rejects(entity, surface)) continue;
			const bool candidateMatch = std::any_of(entity.aliases.begin(), entity.aliases.end(),
					[&](const std::string &alias) { return comparable_key(normalize_alias(alias)) == key; });
			if (candidateMatch == false) continue;
			candidateEntityId = entity.id;
			candidateCanonicalValue = entity.canonical_value;
			break;
		}
	}

	if (candidateEntityId) {
		const ProjectEntity created = make_new_entity(matter, mention.category, mention.value, timestamp);
		matter.entities.push_back(created);
		addContextKey(matter.entities.back(), mention.context_key);
		record_occurrence(matter, created.id, mention.document_id, mention.value);

		if (alreadyKeptSeparate(matter, *candidateEntityId, created.id) == true) {
			return ResolutionOutcome{ResolutionKind::CreatedNew, created.id, std::nullopt};
		}
		if (const auto existingAmbiguity = pendingAmbiguityBetween(matter, *candidateEntityId, created.id)) {
			return ResolutionOutcome{ResolutionKind::AmbiguityRaised, created.id, *existingAmbiguity};
		}

		EntityAmbiguity ambiguity;
		ambiguity.id = generate_ambiguity_id();
		ambiguity.category = mention.category;
		ambiguity.entity_a_id = *candidateEntityId;
		ambiguity.entity_b_id = created.id;
		ambiguity.evidence_alias_a = candidateCanonicalValue;
		ambiguity.evidence_alias_b = mention.value;
		ambiguity.status = AmbiguityStatus::Pending;
		ambiguity.created_at = timestamp;
		matter.entity_ambiguities.push_back(ambiguity);
		return ResolutionOutcome{ResolutionKind::AmbiguityRaised, created.id, ambiguity.id};
	}

	// 3. No candidate at all - brand-new entity.
	const ProjectEntity created = make_new_entity(matter, mention.category, mention.value, timestamp);
	matter.entities.push_back(created);
	addContextKey(matter.entities.back(), mention.context_key);
	return ResolutionOutcome{ResolutionKind::CreatedNew, created.id, std::nullopt};
}

} // namespace singularity::privacy
