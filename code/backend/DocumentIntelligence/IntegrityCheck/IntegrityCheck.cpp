#include "DocumentIntelligence/IntegrityCheck/IntegrityCheck.hpp"
#include "DocumentIntelligence/LegalStructure/LegalStructure.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>

namespace singularity::document_intelligence {
namespace {

using Json = nlohmann::json;

// Words compared as a reader compares them: spacing and typographic quotes
// made plain.
std::string comparable(const std::string& text) {
	std::string words = normalizedSpacing(text);
	for (const auto& [typographic, plain] : std::vector<std::pair<std::string, std::string>>{
			{"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""}, {"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}}) {
		for (std::size_t at = words.find(typographic); at != std::string::npos; at = words.find(typographic, at)) {
			words.replace(at, typographic.size(), plain);
		}
	}
	while (words.empty() == false && words.back() == ' ') words.pop_back();
	return words;
}

bool same(const std::string& first, const std::string& second) {
	return comparable(first) == comparable(second);
}

bool holdsWords(const std::string& text, const std::string& words) {
	const std::string wanted = comparable(words);
	return wanted.empty() == true || comparable(text).find(wanted) != std::string::npos;
}

bool isOmitted(const LegalBlock& block) {
	return same(block.text, kOmittedClause);
}

// The agreement's own blocks, in order: what a reader reads, not the margin's
// comments or the page's header and footer, and not empty paragraphs.
std::vector<const LegalBlock*> agreementBlocks(const LegalDocument& document) {
	std::vector<const LegalBlock*> blocks;
	for (const LegalBlock& block : document.blocks) {
		if (block.kind == BlockKind::Comment || block.kind == BlockKind::HeaderFooter) continue;
		if (comparable(block.text).empty() == true && block.number.empty() == true) continue;
		blocks.push_back(&block);
	}
	return blocks;
}

std::string placeOf(const LegalDocument& document, const LegalBlock& block) {
	const std::string words = placeInLawyersWords(document, block.id);
	return words.empty() == true ? block.id : words;
}

std::string capitalized(std::string words) {
	if (words.empty() == false && words.front() >= 'a' && words.front() <= 'z') words.front() = static_cast<char>(words.front() - 'a' + 'A');
	return words;
}

std::string listed(const std::vector<std::string>& items) {
	std::string words;
	for (std::size_t index = 0; index < items.size(); ++index) {
		if (index > 0) words += index + 1 == items.size() ? " and " : ", ";
		words += items[index];
	}
	return words;
}

/* - - - - - - */
// Parties (P0, 30 Oct 2026): a party is named in the draft exactly as the
// source writes it - its spelling, its punctuation, its legal form - and in
// the capacity the source gives it. A reply's caption once read "Brightwater
// Foods, LLC]\"}" and a DPA "Harlan Retail Group, Inc. ’ Personal
// Information"; neither may reach the lawyer as a draft to accept.

// A party's capacity in a lawsuit or an agreement, lower case and singular.
std::string capacityOf(std::string word) {
	static const std::set<std::string> capacities{"plaintiff", "defendant", "counterclaim-plaintiff",
		"counterclaim-defendant", "counterclaimant", "counter-defendant", "cross-claimant", "cross-defendant",
		"petitioner", "respondent", "appellant", "appellee", "movant", "intervenor", "buyer", "seller", "purchaser",
		"vendor", "landlord", "tenant", "lessor", "lessee", "sublessor", "sublessee", "licensor", "licensee", "lender",
		"borrower", "guarantor", "employer", "employee", "customer", "supplier", "contractor", "consultant", "processor",
		"controller", "subprocessor", "discloser", "recipient", "investor", "issuer", "assignor", "assignee"};
	std::string lowered;
	for (const char character : word) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	while (lowered.empty() == false && std::ispunct(static_cast<unsigned char>(lowered.back())) != 0) lowered.pop_back();
	while (lowered.empty() == false && std::ispunct(static_cast<unsigned char>(lowered.front())) != 0) lowered.erase(0, 1);
	if (lowered.size() > 1 && lowered.back() == 's' && capacities.count(lowered.substr(0, lowered.size() - 1)) > 0)
		lowered.pop_back();
	return capacities.count(lowered) > 0 ? lowered : std::string();
}

// A party as a text writes it with its legal form: "Brightwater Foods, LLC",
// "COASTLINE LOGISTICS, INC.", "Harlan Retail Group, Inc." - never the
// capacity words before it ("Plaintiff Brightwater Foods, LLC").
struct PartyWriting {
	std::string writing;
	std::size_t start = 0;
	std::size_t end = 0;
};

std::string lowerCased(const std::string& value);

std::vector<PartyWriting> partyWritingsIn(const std::string& text) {
	static const std::regex named(
		"((?:[A-Z][A-Za-z0-9&'\\-]*\\.?)(?:[ \\t]+(?:[A-Z][A-Za-z0-9&'\\-]*\\.?|of|and|&|the|de|for))*),?[ \\t]+"
		"(Inc\\.|INC\\.|Inc|INC|LLC|L\\.L\\.C\\.|LLP|L\\.L\\.P\\.|L\\.P\\.|LP|Corp\\.|CORP\\.|Corp|Corporation|CORPORATION|"
		"Company|COMPANY|Co\\.|CO\\.|Ltd\\.|LTD\\.|Ltd|Limited|LIMITED|PLC|GmbH|PLLC|N\\.A\\.|S\\.A\\.)(?=[^A-Za-z]|$)");
	std::vector<PartyWriting> found;
	for (std::sregex_iterator match(text.begin(), text.end(), named), end; match != end; ++match) {
		std::size_t start = static_cast<std::size_t>(match->position(0));
		const std::size_t finish = start + static_cast<std::size_t>(match->length(0));
		// The words before the name are the text's, not the name's: a capacity
		// ("Defendant and Counterclaimant Coastline Logistics, Inc."), and the
		// paper a sentence names before it ("the Complaint of Plaintiff
		// Brightwater Foods, LLC"). The name starts after the last of them.
		static const std::set<std::string> papers{"complaint", "answer", "reply", "motion", "agreement", "addendum",
			"amendment", "order", "judgment", "counterclaims", "counterclaim", "petition", "notice", "exhibit", "schedule",
			"section", "the", "and", "of", "for"};
		std::vector<std::pair<std::size_t, std::string>> words;
		for (std::size_t at = start; at < finish;) {
			while (at < finish && (text[at] == ' ' || text[at] == '\t')) ++at;
			std::size_t end = at;
			while (end < finish && text[end] != ' ' && text[end] != '\t') ++end;
			if (end > at) words.emplace_back(at, text.substr(at, end - at));
			at = end;
		}
		std::size_t first = 0;
		for (std::size_t index = 0; index + 1 < words.size(); ++index)
			if (capacityOf(words[index].second).empty() == false) first = index + 1;
		while (first + 1 < words.size() && papers.count(lowerCased(words[first].second)) > 0) ++first;
		if (first < words.size()) start = words[first].first;
		if (finish > start) found.push_back(PartyWriting{text.substr(start, finish - start), start, finish});
	}
	return found;
}

std::string lowerCased(const std::string& value) {
	std::string lowered;
	for (const char character : value) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	return lowered;
}

// The same name whatever its spacing and punctuation: what tells a party's
// writing that went wrong from a different party.
std::string lettersOf(const std::string& value) {
	std::string letters;
	for (const char character : value)
		if (std::isalnum(static_cast<unsigned char>(character)) != 0)
			letters.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	return letters;
}

// Which party a writing names, with or without its legal form, in any case.
std::string partyKey(const std::string& writing) {
	static const std::regex form(",?[ \\t]+(Inc\\.|INC\\.|Inc|INC|Corp|Ltd|LLC|L\\.L\\.C\\.|LLP|L\\.L\\.P\\.|L\\.P\\.|LP|Corp\\.|CORP\\.|"
		"Corporation|CORPORATION|Company|COMPANY|Co\\.|CO\\.|Ltd\\.|LTD\\.|Limited|LIMITED|PLC|GmbH|PLLC|N\\.A\\.|S\\.A\\.)$");
	return lettersOf(std::regex_replace(writing, form, ""));
}

// What stands right after a party's name that belongs to no name: a closing
// bracket, brace or quotation mark with nothing opened before it on its line,
// or an apostrophe that makes no possessive ("Inc. ’").
std::string strayMarkAfter(const std::string& text, const PartyWriting& party) {
	const std::size_t lineStart = text.rfind('\n', party.start) == std::string::npos ? 0 : text.rfind('\n', party.start) + 1;
	const std::string before = text.substr(lineStart, party.start - lineStart);
	std::size_t at = party.end;
	const auto opened = [&](const std::string& open, const std::string& close) {
		std::size_t count = 0;
		for (std::size_t found = before.find(open); found != std::string::npos; found = before.find(open, found + open.size())) ++count;
		for (std::size_t found = before.find(close); found != std::string::npos; found = before.find(close, found + close.size())) {
			if (count > 0) --count;
		}
		return count > 0;
	};
	for (const auto& [open, close] : std::vector<std::pair<std::string, std::string>>{{"[", "]"}, {"{", "}"},
			{"(", ")"}, {"\xE2\x80\x9C", "\xE2\x80\x9D"}}) {
		if (text.compare(at, close.size(), close) == 0 && opened(open, close) == false) return close;
	}
	if (text.compare(at, 1, "\"") == 0) {
		std::size_t quotes = 0;
		for (const char character : before) if (character == '"') ++quotes;
		if (quotes % 2 == 0) return "\"";
	}
	while (at < text.size() && text[at] == ' ') ++at;
	for (const std::string apostrophe : {std::string("\xE2\x80\x99"), std::string("'")}) {
		if (text.compare(at, apostrophe.size(), apostrophe) != 0) continue;
		const std::size_t next = at + apostrophe.size();
		const bool possessive = at == party.end && next < text.size() && (text[next] == 's' || text[next] == 'S');
		if (possessive == false) return apostrophe;
	}
	return std::string();
}

// The capacities a text gives each party: "Plaintiff Brightwater Foods, LLC",
// "BRIGHTWATER FOODS, LLC, Plaintiff and Counterclaim-Defendant", "Northgate
// Analytics, Inc. (“Service Provider”)".
void addCapacities(const std::string& text, std::map<std::string, std::set<std::string>>& byParty,
	std::map<std::string, std::set<std::string>>& byCapacity, std::map<std::string, std::string>& writingOf,
	std::vector<std::pair<std::string, PartyWriting>>* bound) {
	for (const PartyWriting& party : partyWritingsIn(text)) {
		const std::string key = partyKey(party.writing);
		if (key.empty() == true) continue;
		if (writingOf.count(key) == 0) writingOf[key] = party.writing;
		std::vector<std::string> capacities;
		// Before it: the capacity words that run up to the name.
		std::size_t cursor = party.start;
		while (cursor > 0) {
			std::size_t end = cursor;
			while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
			std::size_t begin = end;
			while (begin > 0 && text[begin - 1] != ' ' && text[begin - 1] != '\t' && text[begin - 1] != '\n') --begin;
			if (begin == end) break;
			const std::string word = text.substr(begin, end - begin);
			if (word == "and") {
				cursor = begin;
				continue;
			}
			const std::string capacity = capacityOf(word);
			if (capacity.empty() == true || std::ispunct(static_cast<unsigned char>(word.back())) != 0) break;
			capacities.push_back(capacity);
			cursor = begin;
		}
		// After it: ", Plaintiff and Counterclaim-Defendant" or (“Service Provider”).
		std::string after = text.substr(party.end, 80);
		std::size_t from = 0;
		while (from < after.size() && (after[from] == ',' || after[from] == ';' || after[from] == ' ' || after[from] == '\n'
				|| after[from] == '\t')) ++from;
		if (after.compare(from, 1, "(") == 0) {
			const std::size_t close = after.find(')', from);
			if (close != std::string::npos) {
				std::string defined = after.substr(from + 1, close - from - 1);
				for (const std::string mark : {std::string("the "), std::string("\xE2\x80\x9C"), std::string("\xE2\x80\x9D"),
						std::string("\"")}) {
					for (std::size_t found = defined.find(mark); found != std::string::npos; found = defined.find(mark))
						defined.erase(found, mark.size());
				}
				const std::string capacity = capacityOf(defined);
				if (capacity.empty() == false) capacities.push_back(capacity);
			}
		} else if (party.end < text.size() && (text[party.end] == ',' || text[party.end] == ';')) {
			std::size_t word = from;
			while (word < after.size()) {
				std::size_t end = word;
				while (end < after.size() && after[end] != ' ' && after[end] != '\n' && after[end] != ',' && after[end] != '.'
					&& after[end] != ';') ++end;
				const std::string token = after.substr(word, end - word);
				if (token == "and") {
					word = end + 1;
					continue;
				}
				const std::string capacity = capacityOf(token);
				if (capacity.empty() == true) break;
				capacities.push_back(capacity);
				word = end;
				while (word < after.size() && (after[word] == ',' || after[word] == ';' || after[word] == ' '
						|| after[word] == '\n' || after[word] == '.'))
					++word;
			}
		}
		for (const std::string& capacity : capacities) {
			byParty[key].insert(capacity);
			byCapacity[capacity].insert(key);
			if (bound != nullptr) bound->push_back({capacity, party});
		}
	}
}

void settle(IntegrityCheckResult& check);

IntegrityCheckResult checkParties(const LegalDocument& source, const LegalDocument& draft, const PartyAuthority& authority) {
	IntegrityCheckResult parties{"parties", "Parties", "passed", "", {}};
	std::set<std::string> sourceWritings;
	std::map<std::string, std::string> sourceLetters;
	// Each party by its name without its legal form: "Coastline Logistics" is
	// "Coastline Logistics, Inc.", never "Coastline Logistics Corp.".
	std::map<std::string, std::string> sourceNames;
	std::map<std::string, std::set<std::string>> sourceByParty;
	std::map<std::string, std::set<std::string>> sourceByCapacity;
	std::map<std::string, std::string> sourceWritingOf;
	// A name said back as the documents write it in their text, never as a
	// caption shouts it ("COASTLINE LOGISTICS, INC.").
	const auto keepBetter = [](std::string& kept, const std::string& writing) {
		const auto shouted = [](const std::string& value) {
			return std::none_of(value.begin(), value.end(), [](char character) { return character >= 'a' && character <= 'z'; });
		};
		if (kept.empty() == true || (shouted(kept) == true && shouted(writing) == false)) kept = writing;
	};
	const auto learn = [&](const std::string& text) {
		for (const PartyWriting& party : partyWritingsIn(text)) {
			sourceWritings.insert(lowerCased(party.writing));
			keepBetter(sourceLetters[lettersOf(party.writing)], party.writing);
			keepBetter(sourceNames[partyKey(party.writing)], party.writing);
		}
		addCapacities(text, sourceByParty, sourceByCapacity, sourceWritingOf, nullptr);
	};
	for (const std::string& text : authority.texts) learn(text);
	// The source's own writings decide, unless it is a colleague's draft and the
	// Matter's documents say how its parties are written.
	const bool isFromDocuments = authority.isSourceAuthoritative == false && sourceWritings.empty() == false;
	if (isFromDocuments == false) {
		for (const LegalBlock& block : source.blocks) {
			if (block.section != "comment") learn(block.text);
		}
	}
	const std::string whose = isFromDocuments == true ? "the Matter's documents write" : "the source writes";
	std::set<std::string> said;
	for (const LegalBlock& block : draft.blocks) {
		if (block.section == "comment") continue;
		const std::string place = block.number.empty() == false ? "Section " + block.number : std::string("The draft");
		for (const PartyWriting& party : partyWritingsIn(block.text)) {
			const std::string stray = strayMarkAfter(block.text, party);
			if (stray.empty() == false && said.insert("stray|" + party.writing + "|" + stray).second == true) {
				parties.findings.push_back({"problem", place + " writes \"" + party.writing + stray + "\": a mark that belongs to "
					"no name stands right after the party's name.", {place}});
			}
			if (sourceWritings.count(lowerCased(party.writing)) > 0) continue;
			const auto same = sourceLetters.find(lettersOf(party.writing));
			const auto named = sourceNames.find(partyKey(party.writing));
			const std::string* written = same != sourceLetters.end() ? &same->second
				: named != sourceNames.end() && named->first.empty() == false ? &named->second : nullptr;
			if (written != nullptr && said.insert("writing|" + party.writing).second == true) {
				parties.findings.push_back({"problem", place + " names a party \"" + party.writing + "\", which " + whose + " \""
					+ *written + "\".", {place}});
			}
		}
		std::map<std::string, std::set<std::string>> draftByParty;
		std::map<std::string, std::set<std::string>> draftByCapacity;
		std::map<std::string, std::string> draftWritingOf;
		std::vector<std::pair<std::string, PartyWriting>> bound;
		addCapacities(block.text, draftByParty, draftByCapacity, draftWritingOf, &bound);
		for (const auto& [capacity, party] : bound) {
			const std::string key = partyKey(party.writing);
			const auto holders = sourceByCapacity.find(capacity);
			const auto known = sourceByParty.find(key);
			if (holders == sourceByCapacity.end() || known == sourceByParty.end() || holders->second.count(key) > 0) continue;
			const std::string other = sourceWritingOf.count(*holders->second.begin()) > 0
				? sourceWritingOf[*holders->second.begin()] : std::string("another party");
			if (said.insert("capacity|" + capacity + "|" + key).second == false) continue;
			parties.findings.push_back({"problem", place + " calls " + party.writing + " the " + capacity + ", which "
				+ std::string(isFromDocuments == true ? "the Matter's documents call " : "the source calls ") + other + ".", {place}});
		}
	}
	settle(parties);
	parties.summary = parties.state == "passed"
		? (isFromDocuments == true
			? "Every party the draft names is written exactly as the Matter's documents write it, in the capacity they give it."
			: "Every party the draft names is written exactly as the source writes it, in the capacity the source gives it.")
		: (isFromDocuments == true
			? "A party is named in the draft as the Matter's documents never name it, so the draft is not offered for acceptance."
			: "A party is named in the draft as the source never names it, so the draft is not offered for acceptance.");
	return parties;
}

void settle(IntegrityCheckResult& check) {
	check.state = "passed";
	for (const IntegrityFinding& finding : check.findings) {
		if (finding.severity == "problem") {
			check.state = "failed";
			return;
		}
		if (finding.severity == "attention") check.state = "attention";
	}
}

// The numbering as it reads: gaps, empty clauses and headings with nothing
// under them, each said once.
std::vector<std::pair<std::string, std::string>> numberingFindings(const LegalDocument& document, int& lastTop) {
	std::vector<std::pair<std::string, std::string>> found;
	lastTop = 0;
	int lastSub = 0;
	const LegalBlock* openHeading = nullptr;
	bool hasBody = true;
	const auto closeHeading = [&]() {
		if (openHeading != nullptr && hasBody == false) {
			found.emplace_back("Section " + openHeading->number + " (" + openHeading->text + ") has no clause under it.",
				"Section " + openHeading->number);
		}
	};
	for (const LegalBlock& block : document.blocks) {
		if (block.number.empty() == true || block.section != "body") continue;
		if (block.kind != BlockKind::Heading && block.kind != BlockKind::Clause) continue;
		const std::size_t dots = static_cast<std::size_t>(std::count(block.number.begin(), block.number.end(), '.'));
		if (dots == 0) {
			closeHeading();
			const int top = std::atoi(block.number.c_str());
			if (top != lastTop + 1) {
				found.emplace_back("The numbering goes from Section " + std::to_string(lastTop) + " to Section " + block.number + ".",
					"Section " + block.number);
			}
			lastTop = top;
			lastSub = 0;
			openHeading = &block;
			hasBody = false;
			continue;
		}
		if (dots == 1) {
			const std::size_t dot = block.number.find('.');
			const int top = std::atoi(block.number.substr(0, dot).c_str());
			const int sub = std::atoi(block.number.substr(dot + 1).c_str());
			if (top == lastTop) {
				if (sub != lastSub + 1) {
					found.emplace_back("The numbering goes from Section " + std::to_string(lastTop) + "." + std::to_string(lastSub)
						+ " to Section " + block.number + ".", "Section " + block.number);
				}
				lastSub = sub;
				hasBody = true;
			}
		}
		if (comparable(block.text).empty() == true) found.emplace_back("Section " + block.number + " is empty.", "Section " + block.number);
	}
	closeHeading();
	return found;
}

// The blocks of a document that use a term, other than the one defining it.
std::vector<std::string> usesOf(const LegalDocument& document, const std::string& term, const std::string& definingBlockId) {
	std::vector<std::string> places;
	for (const LegalBlock* block : agreementBlocks(document)) {
		if (block->id == definingBlockId || isOmitted(*block) == true) continue;
		const std::string text = comparable(block->text);
		for (std::size_t at = text.find(term); at != std::string::npos; at = text.find(term, at + 1)) {
			const bool startsWord = at == 0 || std::isalnum(static_cast<unsigned char>(text[at - 1])) == 0;
			const std::size_t end = at + term.size();
			const bool endsWord = end >= text.size() || std::isalnum(static_cast<unsigned char>(text[end])) == 0;
			if (startsWord == true && endsWord == true) {
				places.push_back(placeOf(document, *block));
				break;
			}
		}
	}
	return places;
}

} // namespace

std::map<std::string, std::string> partyCorrections(const LegalDocument& source, const PartyAuthority& authority) {
	std::map<std::string, std::string> corrections;
	if (authority.isSourceAuthoritative == true || authority.texts.empty() == true) return corrections;
	const auto shouted = [](const std::string& value) {
		return std::none_of(value.begin(), value.end(), [](char character) { return character >= 'a' && character <= 'z'; });
	};
	std::set<std::string> attested;
	std::map<std::string, std::vector<std::string>> byName;
	for (const std::string& text : authority.texts) {
		for (const PartyWriting& party : partyWritingsIn(text)) {
			attested.insert(lowerCased(party.writing));
			std::vector<std::string>& writings = byName[partyKey(party.writing)];
			if (std::find(writings.begin(), writings.end(), party.writing) == writings.end()) writings.push_back(party.writing);
		}
	}
	for (const LegalBlock& block : source.blocks) {
		if (block.section == "comment") continue;
		for (const PartyWriting& party : partyWritingsIn(block.text)) {
			if (attested.count(lowerCased(party.writing)) > 0 || corrections.count(party.writing) > 0) continue;
			const auto named = byName.find(partyKey(party.writing));
			if (named == byName.end() || named->first.empty() == true) continue;
			std::vector<const std::string*> sameCase;
			for (const std::string& writing : named->second)
				if (shouted(writing) == shouted(party.writing)) sameCase.push_back(&writing);
			if (sameCase.size() == 1) corrections[party.writing] = *sameCase.front();
		}
	}
	return corrections;
}

IntegrityReport checkDraftIntegrity(const LegalDocument& source, const LegalDocument& draft,
	const std::vector<ProposedChange>& changes, const PartyAuthority& authority) {

	IntegrityReport report;
	report.isChecked = true;
	const std::vector<const LegalBlock*> before = agreementBlocks(source);
	const std::vector<const LegalBlock*> after = agreementBlocks(draft);
	std::map<std::string, std::vector<const ProposedChange*>> touched;
	for (const ProposedChange& change : changes) {
		if (change.writeState == "written") touched[change.blockId].push_back(&change);
	}

	/* - - - - - - */
	// Completeness and untouched text: the source's blocks found in order in
	// the draft, each untouched one word for word.

	IntegrityCheckResult completeness{"completeness", "Nothing lost", "passed", "", {}};
	IntegrityCheckResult unchanged{"unchanged", "Untouched text unchanged", "passed", "", {}};
	std::vector<bool> accounted(after.size(), false);
	std::size_t numbered = 0;
	std::size_t untouched = 0;
	std::size_t next = 0;
	for (const LegalBlock* block : before) {
		if (block->number.empty() == false) ++numbered;
		const auto touch = touched.find(block->id);
		const bool isOnlyInsertedAfter = touch != touched.end() && std::all_of(touch->second.begin(), touch->second.end(),
			[](const ProposedChange* change) { return change->action == "insert_after"; });
		if (touch == touched.end() || isOnlyInsertedAfter == true) {
			std::size_t at = next;
			while (at < after.size() && same(after[at]->text, block->text) == false) ++at;
			if (at == after.size()) {
				const LegalBlock* counterpart = next < after.size() ? after[next] : nullptr;
				if (counterpart != nullptr && block->number.empty() == false && counterpart->number == block->number) {
					unchanged.findings.push_back({"problem", capitalized(placeOf(source, *block))
						+ " reads differently in the draft, though no change was made to it.", {placeOf(source, *block)}});
					accounted[next] = true;
					++next;
				} else {
					completeness.findings.push_back({"problem", capitalized(placeOf(source, *block)) + " is not in the draft.",
						{placeOf(source, *block)}});
				}
				continue;
			}
			accounted[at] = true;
			++untouched;
			next = at + 1;
		} else {
			// What a written change made of it: the draft's next block.
			if (next >= after.size()) {
				completeness.findings.push_back({"problem", capitalized(placeOf(source, *block)) + " is not in the draft.",
					{placeOf(source, *block)}});
				continue;
			}
			const LegalBlock* now = after[next];
			accounted[next] = true;
			++next;
			for (const ProposedChange* change : touch->second) {
				if (change->action == "replace" && holdsWords(now->text, change->proposedText) == false) {
					unchanged.findings.push_back({"attention", capitalized(placeOf(source, *block))
						+ " does not read in the draft as the change proposed it.", {placeOf(source, *block)}});
				}
			}
		}
		// What a change added after it: the next blocks hold its words.
		if (touch != touched.end()) {
			for (const ProposedChange* change : touch->second) {
				if (change->action != "insert_after") continue;
				if (next < after.size() && holdsWords(after[next]->text, change->proposedText) == true) {
					accounted[next] = true;
					++next;
				} else {
					completeness.findings.push_back({"attention", "The provision added after " + placeOf(source, *block)
						+ " is not where it was added.", {placeOf(source, *block)}});
				}
			}
		}
	}
	for (std::size_t index = 0; index < after.size(); ++index) {
		if (accounted[index] == true) continue;
		const std::string words = comparable(after[index]->text).substr(0, 60);
		unchanged.findings.push_back({"problem", "The draft has text no change accounts for: \"" + words + "...\"",
			{placeOf(draft, *after[index])}});
	}
	// The page's header and footer, which no review changes.
	{
		std::vector<std::string> sourceMargins;
		std::vector<std::string> draftMargins;
		for (const LegalBlock& block : source.blocks) if (block.kind == BlockKind::HeaderFooter) sourceMargins.push_back(comparable(block.text));
		for (const LegalBlock& block : draft.blocks) if (block.kind == BlockKind::HeaderFooter) draftMargins.push_back(comparable(block.text));
		if (sourceMargins != draftMargins) {
			unchanged.findings.push_back({"problem", "The page header or footer reads differently in the draft.", {}});
		}
	}
	settle(completeness);
	settle(unchanged);
	// Said of what was read: "All 0 numbered clauses" and "Sections 1 to 0" once
	// stood under a lease whose clauses were read as paragraphs (2026-10-30).
	completeness.summary = completeness.state == "passed"
		? (numbered == 0 ? std::string("Every part of the source is in the draft, in its order.")
			: "All " + std::to_string(numbered) + " numbered clauses and headings of the source are in the draft, in their order.")
		: std::to_string(completeness.findings.size()) + (completeness.findings.size() == 1 ? " thing" : " things")
			+ " in the source are not where the draft should have them.";
	unchanged.summary = unchanged.state == "passed"
		? std::to_string(untouched) + " parts of the agreement that no change touched read word for word as in the source, "
			"and the draft adds nothing that no change added."
		: std::to_string(unchanged.findings.size()) + (unchanged.findings.size() == 1 ? " place reads" : " places read")
			+ " differently from what the changes account for.";

	/* - - - - - - */
	// Numbering.

	IntegrityCheckResult numbering{"numbering", "Numbering", "passed", "", {}};
	int sourceSections = 0;
	int draftSections = 0;
	const std::vector<std::pair<std::string, std::string>> sourceNumbering = numberingFindings(source, sourceSections);
	std::set<std::string> alreadySo;
	for (const auto& [words, place] : sourceNumbering) alreadySo.insert(words);
	for (const auto& [words, place] : numberingFindings(draft, draftSections)) {
		if (alreadySo.count(words) > 0) numbering.findings.push_back({"note", words + " It is so in the source as well.", {place}});
		else numbering.findings.push_back({"problem", words, {place}});
	}
	settle(numbering);
	numbering.summary = numbering.state == "passed"
		? (draftSections == 0 ? std::string("No numbering gap was found, no numbered clause is empty, and every heading has a "
				"clause under it.")
			: "Sections 1 to " + std::to_string(draftSections) + " are numbered without a gap, no numbered clause is empty, and "
				"every heading has a clause under it.")
		: "The draft's numbering needs a look.";

	/* - - - - - - */
	// Definitions.

	IntegrityCheckResult definitions{"definitions", "Definitions", "passed", "", {}};
	for (const DefinedTerm& term : source.definitions) {
		const auto defined = std::find_if(draft.definitions.begin(), draft.definitions.end(),
			[&](const DefinedTerm& entry) { return entry.term == term.term; });
		const LegalBlock* definedIn = findBlock(source, term.blockId);
		const std::string definedPlace = definedIn != nullptr ? placeOf(source, *definedIn) : std::string("the agreement");
		if (defined == draft.definitions.end()) {
			const std::vector<std::string> uses = usesOf(draft, term.term, std::string());
			if (uses.empty() == false) {
				definitions.findings.push_back({"problem", "\"" + term.term + "\" is used in " + listed(uses)
					+ ", but the draft no longer defines it.", uses});
			}
			continue;
		}
		const std::vector<std::string> uses = usesOf(draft, term.term, defined->blockId);
		if (uses.empty() == true && term.usedIn.empty() == false) {
			definitions.findings.push_back({"attention", "\"" + term.term + "\" is defined in " + definedPlace
				+ ", but the draft no longer uses it.", {definedPlace}});
		}
	}
	for (const DefinedTerm& term : draft.definitions) {
		const bool isNew = std::none_of(source.definitions.begin(), source.definitions.end(),
			[&](const DefinedTerm& entry) { return entry.term == term.term; });
		if (isNew == false) continue;
		const LegalBlock* definedIn = findBlock(draft, term.blockId);
		definitions.findings.push_back({"note", "\"" + term.term + "\" is newly defined in "
			+ (definedIn != nullptr ? placeOf(draft, *definedIn) : std::string("the draft")) + ".", {}});
	}
	settle(definitions);
	definitions.summary = definitions.state == "passed"
		? "Every defined term the draft uses is defined in it, and every term it defines is still used."
		: "Some defined terms need a look.";

	/* - - - - - - */
	// Cross-references.

	IntegrityCheckResult references{"references", "Cross-references", "passed", "", {}};
	std::set<std::string> missingBefore;
	for (const CrossReference& reference : source.references) {
		if (reference.isResolved == false) missingBefore.insert(reference.kind + "|" + reference.label);
	}
	std::set<std::string> said;
	for (const CrossReference& reference : draft.references) {
		const LegalBlock* from = findBlock(draft, reference.fromBlockId);
		const std::string fromPlace = from != nullptr ? placeOf(draft, *from) : std::string("the draft");
		if (reference.isResolved == false) {
			const std::string key = reference.kind + "|" + reference.label;
			if (said.insert(key + "|" + fromPlace).second == false) continue;
			if (missingBefore.count(key) > 0) {
				references.findings.push_back({"note", reference.label + ", referred to in " + fromPlace
					+ ", is missing from the source as well.", {fromPlace}});
			} else {
				references.findings.push_back({"problem", reference.label + ", referred to in " + fromPlace
					+ ", does not exist in the draft.", {fromPlace}});
			}
			continue;
		}
		const LegalBlock* target = findBlock(draft, reference.targetBlockId);
		if (target != nullptr && isOmitted(*target) == true) {
			references.findings.push_back({"attention", capitalized(fromPlace) + " refers to " + reference.label
				+ ", which the draft omits.", {fromPlace}});
		}
	}
	settle(references);
	references.summary = references.state == "failed"
		? "The draft refers to places it does not have."
		: references.findings.empty() == true
			? "Every Section and Exhibit the draft refers to exists in it."
			: "Every Section and Exhibit the draft refers to exists in it, except what the source already lacked.";

	report.checks = {completeness, unchanged, numbering, definitions, references, checkParties(source, draft, authority)};
	return report;

}

Json integrityReportToJson(const IntegrityReport& report) {

	Json checks = Json::array();
	for (const IntegrityCheckResult& check : report.checks) {
		Json findings = Json::array();
		for (const IntegrityFinding& finding : check.findings) {
			findings.push_back(Json{{"severity", finding.severity}, {"text", finding.text}, {"places", finding.places}});
		}
		checks.push_back(Json{{"name", check.name}, {"title", check.title}, {"state", check.state},
			{"summary", check.summary}, {"findings", findings}});
	}
	return Json{{"isChecked", report.isChecked}, {"problem", report.problem}, {"checks", checks}};

}

IntegrityReport integrityReportFromJson(const Json& value) {

	IntegrityReport report;
	if (value.is_object() == false) return report;
	report.isChecked = value.value("isChecked", false);
	report.problem = value.value("problem", std::string());
	for (const Json& entry : value.value("checks", Json::array())) {
		IntegrityCheckResult check;
		check.name = entry.value("name", std::string());
		check.title = entry.value("title", std::string());
		check.state = entry.value("state", std::string());
		check.summary = entry.value("summary", std::string());
		for (const Json& item : entry.value("findings", Json::array())) {
			check.findings.push_back({item.value("severity", std::string()), item.value("text", std::string()),
				item.value("places", std::vector<std::string>())});
		}
		report.checks.push_back(check);
	}
	return report;

}

} // namespace singularity::document_intelligence
