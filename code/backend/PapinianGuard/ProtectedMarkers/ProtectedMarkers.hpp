#pragma once

#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/ProtectedMarkers/MarkerStyle.hpp"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a protected symbol looks like.
//
// A symbol stands in the request for a value that stays on the lawyer's
// computer. The compact form, [[E17]], says nothing of what it stands for; the
// typed form, [[Org1]], [[Person2]], [[Amount3]], [[Date4]], says what kind of
// value it is in the model's own words - no more than the legend already says
// - and costs the same: four o200k tokens each, like [[E1]] (measured). Gender
// is never in a symbol, nor anything the legend does not give.
//
// Every "what does a symbol look like" decision is here: the prefix a row is
// issued with, its writing, and reading symbols back - typed, compact, and the
// small variations a model makes ("[[ org1 ]]", "[Org1]").
namespace singularity::protected_api::papinian::markers {

// The word a typed symbol begins with, for a row of `type` holding `value`.
inline std::string prefix_for(std::optional<EntityType> type, const std::optional<semantic::Value> &value,
		DetectionSource source) {
	// A name or a code is what it names, whatever figures it writes ("3M").
	if (type)
		switch (*type) {
			case EntityType::Person:
			case EntityType::Judge: return "Person";
			case EntityType::Organisation:
			case EntityType::LegalEntity:
			case EntityType::Court: return "Org";
			case EntityType::Location:
			case EntityType::Address: return "Place";
			case EntityType::Email: return "Email";
			case EntityType::Phone: return "Phone";
			case EntityType::BankAccount:
			case EntityType::TaxId:
			case EntityType::CaseNumber:
			case EntityType::ContractIdentifier: return "Id";
			default: break;
		}
	if (value) {
		switch (value->kind) {
			case semantic::Kind::Money: return "Amount";
			case semantic::Kind::Percentage: return "Rate";
			case semantic::Kind::Date:
			case semantic::Kind::DayOfYear: return "Date";
			case semantic::Kind::Duration: return "Duration";
			case semantic::Kind::Number: return "Number";
			case semantic::Kind::Count: return "Count";
			case semantic::Kind::Boolean: break;
		}
	}
	if (!type) return source == DetectionSource::CustomTerm ? "Term" : "Value";
	switch (*type) {
		case EntityType::Person:
		case EntityType::Judge: return "Person";
		case EntityType::Organisation:
		case EntityType::LegalEntity:
		case EntityType::Court: return "Org";
		case EntityType::Location:
		case EntityType::Address: return "Place";
		case EntityType::Email: return "Email";
		case EntityType::Phone: return "Phone";
		case EntityType::BankAccount:
		case EntityType::TaxId:
		case EntityType::CaseNumber:
		case EntityType::ContractIdentifier: return "Id";
		case EntityType::Date: return "Date";
		case EntityType::MoneyAmount: return "Amount";
		case EntityType::Percentage: return "Rate";
		case EntityType::Number: return "Number";
	}
	return "Value";
}

// The protected computation protocol's own prefixes (DESIGN4 §2.4): a count
// Singularity worked out ([[Count1]]), a truth value kept as a symbol
// ([[Flag1]]), a set of rows ([[Rows1]]), rows grouped by a key
// ([[Groups1]]), and the handles of a table, a band schedule and a
// business-day calendar ([[Table1]], [[Schedule1]], [[Calendar1]]). Declared
// by the seam. `collides` below knows the four that are issued symbols; the
// three handles are Singularity's own words in its blocks, never a fact's symbol.
inline constexpr const char *kComputePrefixes[] = {"Count", "Flag", "Rows", "Groups", "Table", "Schedule", "Calendar"};

// "[[Org1]]", "[[E17]]".
inline std::string format(const std::string &prefix, std::size_t number) {
	return "[[" + prefix + std::to_string(number) + "]]";
}

// One symbol written in a text: its identity ("Org1", "E17") and part
// ("short", "first"), where it stands, and whether it is written exactly as
// issued ("[[Org1]]") rather than as a model varied it ("[[ org1 ]]", "[Org1]").
struct Parsed {
	std::string base;
	std::string part;
	std::size_t start = 0;
	std::size_t length = 0;
	bool canonical = false;
};

inline bool is_ascii_letter(char character) {
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

inline bool is_ascii_digit(char character) {
	return character >= '0' && character <= '9';
}

// Every symbol shape a text writes: letters, then digits, then an optional
// ".part", inside [[ ]] (spaces allowed) or single [ ]. The identity keeps the
// letters' case as written; callers compare it case-blind.
inline std::vector<Parsed> find_all(std::string_view text) {
	std::vector<Parsed> found;
	for (std::size_t open = text.find('['); open != std::string_view::npos; open = text.find('[', open + 1)) {
		const bool doubled = open + 1 < text.size() && text[open + 1] == '[';
		std::size_t at = open + (doubled ? 2 : 1);
		const auto spaces = [&]() { while (at < text.size() && text[at] == ' ') ++at; };
		spaces();
		const std::size_t lettersStart = at;
		while (at < text.size() && is_ascii_letter(text[at]) && at - lettersStart < 12) ++at;
		if (at == lettersStart) continue;
		const std::size_t digitsStart = at;
		while (at < text.size() && is_ascii_digit(text[at]) && at - digitsStart < 6) ++at;
		if (at == digitsStart) continue;
		const std::size_t identityEnd = at;
		std::string part;
		if (at < text.size() && text[at] == '.') {
			std::size_t partEnd = at + 1;
			while (partEnd < text.size() && is_ascii_letter(text[partEnd])) ++partEnd;
			if (partEnd > at + 1) {
				part = std::string(text.substr(at + 1, partEnd - at - 1));
				at = partEnd;
			}
		}
		spaces();
		const std::string_view close = doubled ? "]]" : "]";
		if (text.compare(at, close.size(), close) != 0) continue;
		// A single bracket inside a double one is the double one's.
		if (!doubled && open > 0 && text[open - 1] == '[') continue;
		Parsed parsed;
		parsed.base = std::string(text.substr(lettersStart, identityEnd - lettersStart));
		for (char &character : part) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		parsed.part = part;
		parsed.start = open;
		parsed.length = at + close.size() - open;
		parsed.canonical = doubled && lettersStart == open + 2 && text[at - 1] != ' '
			&& text.substr(open, parsed.length) == "[[" + parsed.base + (part.empty() ? std::string() : "." + part) + "]]";
		found.push_back(std::move(parsed));
	}
	return found;
}

// Whether a text already writes the shape a typed symbol takes, so that a
// table issuing typed symbols could be confused with the text's own words:
// the table then issues compact ones.
inline bool collides(std::string_view text) {
	for (const Parsed &parsed : find_all(text)) {
		std::string letters;
		for (const char character : parsed.base)
			if (is_ascii_letter(character)) letters += character;
		for (const char *prefix : {"Person", "Org", "Place", "Email", "Phone", "Url", "Id", "Amount", "Rate", "Date",
				"Duration", "Number", "Term", "Value", "Count", "Flag", "Rows", "Groups"}) {
			std::string lowered = letters;
			std::string wanted = prefix;
			for (char &character : lowered) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			for (char &character : wanted) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			if (lowered == wanted) return true;
		}
	}
	return false;
}

// How a disclosure names a symbol to the lawyer: "[[Org3]]" is "Org 3", a
// compact "[[E17]]" nothing of its own ("").
inline std::string label_of(const std::string &marker) {
	const std::vector<Parsed> parsed = find_all(marker);
	if (parsed.size() != 1) return {};
	std::string letters;
	std::string digits;
	for (const char character : parsed.front().base) (is_ascii_digit(character) ? digits : letters) += character;
	if (letters.size() <= 1) return {};
	return letters + " " + digits;
}

} // namespace singularity::protected_api::papinian::markers
