#include "DocumentIntelligence/LegalStructure/LegalStructure.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "Platform/PhraseMatch/PhraseMatch.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

#include <pugixml.hpp>

namespace singularity::document_intelligence {
namespace {

using Json = nlohmann::json;

// ---- Words -----------------------------------------------------------------

bool isAsciiSpace(char character) {
	return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f'
		|| character == '\v';
}

std::string trimmed(const std::string& text) {
	std::size_t first = 0;
	while (first < text.size() && isAsciiSpace(text[first]) == true) ++first;
	std::size_t last = text.size();
	while (last > first && isAsciiSpace(text[last - 1]) == true) --last;
	return text.substr(first, last - first);
}

// Curly quotation marks as straight ones, so a pattern can find a defined term
// whichever marks the drafter's word processor wrote. Used only to FIND; the
// words kept are always the document's own.
std::string withStraightQuotes(const std::string& text) {

	std::string result;
	result.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (index + 2 < text.size() && static_cast<unsigned char>(text[index]) == 0xE2
			&& static_cast<unsigned char>(text[index + 1]) == 0x80) {
			const unsigned char third = static_cast<unsigned char>(text[index + 2]);
			if (third == 0x9C || third == 0x9D) {
				result += '"';
				index += 2;
				continue;
			}
			if (third == 0x98 || third == 0x99) {
				result += '\'';
				index += 2;
				continue;
			}
		}
		result += text[index];
	}
	return result;

}

bool isWordCharacter(char character) {
	return std::isalnum(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80;
}

// Whether `term` stands in `text` as whole words, case kept: "Party" stands in
// "a Party may", not in "Partyline".
bool standsAsWords(const std::string& text, const std::string& term) {

	std::size_t from = 0;
	while (true) {
		const std::size_t at = text.find(term, from);
		if (at == std::string::npos) return false;
		const bool isStartClear = at == 0 || isWordCharacter(text[at - 1]) == false;
		const std::size_t end = at + term.size();
		const bool isEndClear = end >= text.size() || isWordCharacter(text[end]) == false;
		if (isStartClear == true && isEndClear == true) return true;
		from = at + 1;
	}

}

std::string withoutFinalStop(std::string text) {
	text = trimmed(text);
	while (text.empty() == false && (text.back() == '.' || text.back() == ',' || text.back() == ';' || text.back() == ':')) {
		text.pop_back();
	}
	return trimmed(text);
}

// "6." is clause 6; "6.1" is clause 6.1; a bracketed "(a)" stays as written.
std::string clauseNumber(const std::string& drawn) {
	return withoutFinalStop(drawn);
}

bool isTopLevelNumber(const std::string& number) {
	return number.empty() == false && number.find('.') == std::string::npos
		&& std::all_of(number.begin(), number.end(), [](unsigned char character) { return std::isdigit(character) != 0; });
}

std::string fingerprintOf(const std::string& text) {
	return platform::sha256_hex_digest(normalizedSpacing(text));
}

// ---- A Word package's XML ----------------------------------------------------

// An element's name without its namespace prefix: "p" for "w:p". A package
// may bind WordprocessingML to any prefix, though Word itself writes "w".
std::string localName(const pugi::xml_node& node) {
	const std::string name = node.name();
	const std::size_t colon = name.find(':');
	return colon == std::string::npos ? name : name.substr(colon + 1);
}

std::string attributeValue(const pugi::xml_node& node, const char* localAttribute) {

	for (const pugi::xml_attribute& attribute : node.attributes()) {
		const std::string name = attribute.name();
		const std::size_t colon = name.find(':');
		const std::string local = colon == std::string::npos ? name : name.substr(colon + 1);
		if (local == localAttribute) return attribute.value();
	}
	return std::string();

}

pugi::xml_node childElement(const pugi::xml_node& node, const char* local) {
	for (const pugi::xml_node& child : node.children()) {
		if (child.type() == pugi::node_element && localName(child) == local) return child;
	}
	return pugi::xml_node();
}

bool parseXml(const std::string& bytes, pugi::xml_document& document) {
	const pugi::xml_parse_result result = document.load_buffer(bytes.data(), bytes.size(),
		pugi::parse_default | pugi::parse_ws_pcdata_single);
	return static_cast<bool>(result);
}

// The words a tracked change carries: what it inserted, or what it struck.
std::string revisionWords(const pugi::xml_node& node) {
	std::string words;
	for (const pugi::xml_node& child : node.children()) {
		if (child.type() != pugi::node_element) continue;
		const std::string name = localName(child);
		if (name == "t" || name == "delText") words += child.text().get();
		else if (name == "tab" || name == "br" || name == "cr") words += ' ';
		else if (name != "rPr" && name != "pPr" && name != "instrText") words += revisionWords(child);
	}
	return words;
}

// What a paragraph's reading keeps besides its words, where asked: its pending
// tracked changes, the footnote and endnote marks it carries, and how many
// pictures or embedded objects it holds.
struct ParagraphExtras {
	std::vector<PendingChange>* changes = nullptr;
	std::vector<std::string>* noteIds = nullptr;
	std::size_t* objects = nullptr;
};

// The words of a paragraph as a reader sees them, every pending tracked change
// as it stands: inserted words in, struck and moved-away words out. Counts the
// revisions it passes, gathers the comments anchored inside it, and keeps what
// `extras` asks for.
void collectParagraphText(const pugi::xml_node& node, std::string& text, std::size_t& revisions,
	std::vector<std::string>& commentIds, const ParagraphExtras* extras = nullptr) {

	for (const pugi::xml_node& child : node.children()) {
		if (child.type() != pugi::node_element) continue;
		const std::string name = localName(child);
		if (name == "del" || name == "moveFrom") {
			++revisions;
			if (extras != nullptr && extras->changes != nullptr) {
				const std::string words = revisionWords(child);
				if (trimmed(words).empty() == false) extras->changes->push_back({"struck", attributeValue(child, "author"), words, false});
			}
			continue;
		}
		if (name == "ins" || name == "moveTo") {
			++revisions;
			ParagraphExtras inner;
			if (extras != nullptr) {
				inner = *extras;
				if (extras->changes != nullptr) {
					const std::string words = revisionWords(child);
					if (trimmed(words).empty() == false) extras->changes->push_back({"inserted", attributeValue(child, "author"), words, false});
				}
			}
			// Its own words are the insertion's: not counted as a change again.
			inner.changes = nullptr;
			collectParagraphText(child, text, revisions, commentIds, &inner);
			continue;
		}
		if (name == "footnoteReference" || name == "endnoteReference") {
			if (extras != nullptr && extras->noteIds != nullptr) {
				extras->noteIds->push_back(std::string(name == "footnoteReference" ? "footnote:" : "endnote:")
					+ attributeValue(child, "id"));
			}
			continue;
		}
		if (name == "drawing" || name == "pict" || name == "object") {
			if (extras != nullptr && extras->objects != nullptr) ++*extras->objects;
			// A text box inside one still holds words.
			collectParagraphText(child, text, revisions, commentIds, extras);
			continue;
		}
		if (name == "t") {
			text += child.text().get();
			continue;
		}
		if (name == "tab" || name == "br" || name == "cr") {
			text += ' ';
			continue;
		}
		if (name == "noBreakHyphen") {
			text += '-';
			continue;
		}
		if (name == "delText" || name == "instrText" || name == "pPr" || name == "rPr") continue;
		if (name == "commentRangeStart" || name == "commentReference") {
			const std::string id = attributeValue(child, "id");
			if (id.empty() == false && std::find(commentIds.begin(), commentIds.end(), id) == commentIds.end()) {
				commentIds.push_back(id);
			}
			continue;
		}
		// Runs, hyperlinks, smart tags, fields and content controls hold words -
		// and the marks, pictures and changes inside them.
		collectParagraphText(child, text, revisions, commentIds, extras);
	}

}

// A paragraph's words before its pending tracked changes: what was struck is
// in, what was inserted is out - the document every pending change would leave
// as it was if rejected.
void collectOriginalText(const pugi::xml_node& node, std::string& text) {

	for (const pugi::xml_node& child : node.children()) {
		if (child.type() != pugi::node_element) continue;
		const std::string name = localName(child);
		if (name == "ins" || name == "moveTo") continue;
		if (name == "del" || name == "moveFrom") {
			text += revisionWords(child);
			continue;
		}
		if (name == "t") {
			text += child.text().get();
			continue;
		}
		if (name == "tab" || name == "br" || name == "cr") {
			text += ' ';
			continue;
		}
		if (name == "noBreakHyphen") {
			text += '-';
			continue;
		}
		if (name == "delText" || name == "instrText" || name == "pPr" || name == "rPr") continue;
		collectOriginalText(child, text);
	}

}

// Whether a paragraph's mark is a pending insertion: the paragraph itself was
// inserted and nobody has accepted it yet.
bool isMarkInserted(const pugi::xml_node& paragraph) {
	const pugi::xml_node properties = childElement(paragraph, "pPr");
	if (!properties) return false;
	return static_cast<bool>(childElement(childElement(properties, "rPr"), "ins"));
}

std::string styleOf(const pugi::xml_node& paragraph) {
	const pugi::xml_node properties = childElement(paragraph, "pPr");
	if (!properties) return std::string();
	return attributeValue(childElement(properties, "pStyle"), "val");
}

// A cell a vertical merge continues from the row above: its words are the
// cell's above, and Word draws none of its own (as Document/DocxExtractor
// reads it).
bool isContinuedCell(const pugi::xml_node& cell) {
	const pugi::xml_node merge = childElement(childElement(cell, "tcPr"), "vMerge");
	if (!merge) return false;
	const std::string value = attributeValue(merge, "val");
	return value.empty() == true || value == "continue";
}

// A row struck whole as a revision nobody has accepted yet.
bool isStruckRow(const pugi::xml_node& row) {
	return static_cast<bool>(childElement(childElement(row, "trPr"), "del"));
}

// A table's words, one line per row, its cells separated by " | " - every
// cell in its place, an empty one too, so each stands in its column - and its
// rows as read, cell by cell. A row with no words in any cell is not read; nor
// is a row struck as a pending revision, though what it struck is kept with
// the table's pending changes.
std::string tableText(const pugi::xml_node& table, std::size_t& revisions,
	std::vector<PendingChange>* changes = nullptr, std::size_t* objects = nullptr,
	std::vector<TableRowReading>* rows = nullptr) {

	std::string text;
	for (const pugi::xml_node& row : table.children()) {
		if (row.type() != pugi::node_element || localName(row) != "tr") continue;
		TableRowReading reading;
		bool hasWords = false;
		for (const pugi::xml_node& cell : row.children()) {
			if (cell.type() != pugi::node_element || localName(cell) != "tc") continue;
			std::string cellText;
			for (const pugi::xml_node& paragraph : cell.children()) {
				if (paragraph.type() != pugi::node_element || localName(paragraph) != "p") continue;
				std::string paragraphText;
				std::vector<std::string> ignored;
				ParagraphExtras extras;
				extras.changes = changes;
				extras.objects = objects;
				collectParagraphText(paragraph, paragraphText, revisions, ignored, &extras);
				paragraphText = trimmed(paragraphText);
				if (paragraphText.empty() == true) continue;
				if (cellText.empty() == false) cellText += "; ";
				cellText += paragraphText;
			}
			if (cellText.empty() == false) hasWords = true;
			reading.cells.push_back(cellText);
			reading.isContinued.push_back(isContinuedCell(cell));
		}
		if (hasWords == false || isStruckRow(row) == true) continue;
		std::string line;
		for (std::size_t index = 0; index < reading.cells.size(); ++index) {
			if (index > 0) line += " | ";
			line += reading.cells[index];
		}
		if (text.empty() == false) text += '\n';
		text += line;
		if (rows != nullptr) rows->push_back(std::move(reading));
	}
	return text;

}

// Every paragraph of a header, a footer or a comment, joined.
std::string partText(const pugi::xml_node& root) {

	std::string text;
	std::size_t revisions = 0;
	std::vector<std::string> ignored;
	for (pugi::xml_node node = root.first_child(); node; node = node.next_sibling()) {
		if (node.type() != pugi::node_element) continue;
		if (localName(node) == "p") {
			std::string paragraphText;
			collectParagraphText(node, paragraphText, revisions, ignored);
			paragraphText = trimmed(paragraphText);
			if (paragraphText.empty() == true) continue;
			if (text.empty() == false) text += ' ';
			text += paragraphText;
			continue;
		}
		const std::string inner = partText(node);
		if (inner.empty() == true) continue;
		if (text.empty() == false) text += ' ';
		text += inner;
	}
	return text;

}

std::string partOr(const SourceIntake& intake, const std::string& name) {
	const auto found = intake.parts.find(name);
	return found == intake.parts.end() ? std::string() : found->second;
}

// What docProps says about who wrote the file: its creator, its last author,
// the company it names and its title, where it says them.
std::vector<DocumentProperty> readProperties(const SourceIntake& intake) {

	std::vector<DocumentProperty> properties;
	const auto readFrom = [&](const std::string& part, const std::vector<std::pair<const char*, const char*>>& fields) {
		const std::string bytes = partOr(intake, part);
		if (bytes.empty() == true) return;
		pugi::xml_document document;
		if (parseXml(bytes, document) == false) return;
		for (const std::pair<const char*, const char*>& field : fields) {
			for (pugi::xml_node node = document.document_element().first_child(); node; node = node.next_sibling()) {
				if (node.type() != pugi::node_element || localName(node) != field.first) continue;
				const std::string value = trimmed(node.text().get());
				if (value.empty() == false) properties.push_back({field.second, value});
			}
		}
	};
	readFrom("docProps/core.xml", {{"title", "title"}, {"creator", "author"}, {"lastModifiedBy", "lastModifiedBy"}});
	readFrom("docProps/app.xml", {{"Company", "company"}, {"Manager", "manager"}});
	return properties;

}

// ---- From paragraphs to the legal document -----------------------------------

struct ExhibitHeading {
	bool isExhibit = false;
	std::string label;
	std::string title;
};

ExhibitHeading exhibitHeadingOf(const std::string& text) {

	ExhibitHeading heading;
	const std::string words = trimmed(text);
	if (words.size() > 160) return heading;
	static const std::regex pattern(
		R"(^(EXHIBIT|Exhibit|SCHEDULE|Schedule|ANNEX|Annex|APPENDIX|Appendix)\s+([A-Z]|\d{1,2})(?![A-Za-z0-9])\s*[.:\-]?\s*(.*)$)");
	std::smatch match;
	if (std::regex_match(words, match, pattern) == false) return heading;
	// "Exhibit A sets out the transaction. ..." is a sentence about an exhibit,
	// not the line that begins one.
	const std::string rest = match[3].str();
	if (rest.size() > 100 || rest.find(". ") != std::string::npos) return heading;
	std::string kind = match[1].str();
	std::transform(kind.begin() + 1, kind.end(), kind.begin() + 1,
		[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
	heading.isExhibit = true;
	heading.label = kind + " " + match[2].str();
	heading.title = withoutFinalStop(match[3].str());
	return heading;

}

std::string sentenceHolding(const std::string& text, std::size_t at) {

	std::size_t start = 0;
	for (std::size_t index = at; index > 0; --index) {
		if ((text[index - 1] == '.' || text[index - 1] == ';') && index < text.size() && text[index] == ' ') {
			start = index + 1;
			break;
		}
	}
	std::size_t end = text.size();
	for (std::size_t index = at; index + 1 < text.size(); ++index) {
		if ((text[index] == '.' || text[index] == ';') && text[index + 1] == ' ') {
			end = index + 1;
			break;
		}
	}
	return trimmed(text.substr(start, end - start));

}

void findDefinitions(LegalDocument& document) {

	static const std::regex atStart(
		R"rx(^\s*(?:[A-Z][^".]{0,60}\.\s+)?"([^"]{2,80})"\s+(means|shall mean|has the meaning|shall have the meaning|refers to))rx");
	static const std::regex inBrackets(
		R"rx(\((?:the |this |each(?:,)? (?:a |an |the )?|together(?:,)? (?:the )?|collectively(?:,)? (?:the )?|hereinafter (?:the )?)?"([^"]{2,80})"\))rx");
	static const std::regex asNamed(R"rx(\bas (?:a |an |the )"([^"]{2,80})")rx");

	std::set<std::string> known;
	for (const LegalBlock& block : document.blocks) {
		if (block.kind == BlockKind::Comment || block.kind == BlockKind::HeaderFooter) continue;
		const std::string text = withStraightQuotes(block.text);
		const auto keep = [&](std::string term, std::size_t at, bool isWholeBlock) {
			term = withoutFinalStop(term);
			if (term.empty() == true || known.count(term) > 0) return;
			known.insert(term);
			DefinedTerm definition;
			definition.term = term;
			definition.blockId = block.id;
			definition.definition = isWholeBlock == true ? block.text : sentenceHolding(block.text, std::min(at, block.text.size()));
			document.definitions.push_back(definition);
		};
		std::smatch match;
		if (std::regex_search(text, match, atStart) == true) keep(match[1].str(), 0, true);
		for (std::sregex_iterator it(text.begin(), text.end(), inBrackets), end; it != end; ++it) {
			keep((*it)[1].str(), static_cast<std::size_t>(it->position(0)), false);
		}
		for (std::sregex_iterator it(text.begin(), text.end(), asNamed), end; it != end; ++it) {
			keep((*it)[1].str(), static_cast<std::size_t>(it->position(0)), false);
		}
	}

	for (DefinedTerm& definition : document.definitions) {
		for (const LegalBlock& block : document.blocks) {
			if (block.id == definition.blockId || block.kind == BlockKind::Comment || block.kind == BlockKind::HeaderFooter) {
				continue;
			}
			if (standsAsWords(withStraightQuotes(block.text), definition.term) == true) definition.usedIn.push_back(block.id);
		}
	}

}

void findReferences(LegalDocument& document) {

	std::map<std::string, std::string> blockByNumber;
	for (const LegalBlock& block : document.blocks) {
		if (block.number.empty() == true) continue;
		if (block.kind == BlockKind::Clause || block.kind == BlockKind::Heading) {
			blockByNumber.emplace(block.number, block.id);
		}
	}
	std::map<std::string, std::size_t> exhibitIndex;
	for (std::size_t index = 0; index < document.exhibits.size(); ++index) exhibitIndex[document.exhibits[index].label] = index;

	static const std::regex sectionWord(R"(\b(Sections?|Clauses?|Articles?|Paragraphs?)\s+(\d+(?:\.\d+)*))");
	static const std::regex continuation(R"(^(?:\s*,\s*|\s+and\s+|\s+or\s+|\s+through\s+|\s+to\s+|\s*&\s*)(\d+(?:\.\d+)*))");
	static const std::regex exhibitWord(R"(\b(Exhibit|Schedule|Annex|Appendix)\s+([A-Z]|\d{1,2})(?![A-Za-z0-9]))");

	for (const LegalBlock& block : document.blocks) {
		if (block.kind == BlockKind::Comment || block.kind == BlockKind::HeaderFooter
			|| block.kind == BlockKind::ExhibitHeading) {
			continue;
		}
		const std::string& text = block.text;
		std::set<std::string> said;
		const auto keepSection = [&](const std::string& number) {
			const std::string label = "Section " + number;
			if (said.count(label) > 0) return;
			said.insert(label);
			CrossReference reference;
			reference.label = label;
			reference.kind = "section";
			reference.fromBlockId = block.id;
			// "Section 6" names clause 6 as a whole; "Section 6.1" one of its parts.
			const auto found = blockByNumber.find(number);
			if (found != blockByNumber.end()) {
				reference.targetBlockId = found->second;
				reference.isResolved = true;
			}
			document.references.push_back(reference);
		};
		for (std::sregex_iterator it(text.begin(), text.end(), sectionWord), end; it != end; ++it) {
			keepSection((*it)[2].str());
			std::string rest = text.substr(static_cast<std::size_t>(it->position(0) + it->length(0)));
			std::smatch more;
			while (std::regex_search(rest, more, continuation) == true) {
				keepSection(more[1].str());
				rest = rest.substr(static_cast<std::size_t>(more.position(0) + more.length(0)));
			}
		}
		for (std::sregex_iterator it(text.begin(), text.end(), exhibitWord), end; it != end; ++it) {
			const std::string label = (*it)[1].str() + " " + (*it)[2].str();
			if (said.count(label) > 0) continue;
			said.insert(label);
			CrossReference reference;
			reference.label = label;
			reference.kind = "exhibit";
			reference.fromBlockId = block.id;
			const auto found = exhibitIndex.find(label);
			if (found == exhibitIndex.end()) {
				ExhibitStatus status;
				status.label = label;
				exhibitIndex[label] = document.exhibits.size();
				document.exhibits.push_back(status);
			}
			ExhibitStatus& status = document.exhibits[exhibitIndex[label]];
			status.referencedIn.push_back(block.id);
			if (status.isPresent == true) {
				reference.isResolved = true;
				reference.targetBlockId = "x-" + label.substr(label.find(' ') + 1);
			}
			document.references.push_back(reference);
		}
	}

}

// Who the signature blocks bind - each block's first line, the entity that
// signs - and whether the opening words of the agreement name it as a party.
// Compared in plain letters (case, punctuation and spacing aside), so
// "HALVANE HOLDINGS, INC." is "Halvane Holdings, Inc."; a signature page that
// names an entity the agreement does not is the document's own finding.
std::string plainLetters(const std::string& text) {
	std::string plain;
	bool isSpace = false;
	for (const unsigned char character : text) {
		if (std::isalnum(character) != 0) {
			plain += static_cast<char>(std::tolower(character));
			isSpace = false;
		} else if (isSpace == false && plain.empty() == false) {
			plain += ' ';
			isSpace = true;
		}
	}
	while (plain.empty() == false && plain.back() == ' ') plain.pop_back();
	return plain;
}

void findSignatories(LegalDocument& document) {
	std::string preamble;
	for (const LegalBlock& block : document.blocks) {
		if (block.section != "body") continue;
		if (block.kind == BlockKind::Heading || block.kind == BlockKind::Clause || block.kind == BlockKind::Signature) break;
		if (block.kind == BlockKind::Paragraph) preamble += " " + block.text;
	}
	const std::string plainPreamble = " " + plainLetters(preamble) + " ";
	for (const LegalBlock& block : document.blocks) {
		if (block.kind != BlockKind::Signature) continue;
		std::size_t start = 0;
		while (start <= block.text.size()) {
			const std::size_t end = block.text.find(" | ", start);
			const std::string cell = block.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
			const std::string first = trimmed(cell.substr(0, cell.find("; ")));
			const std::string plainName = plainLetters(first);
			const bool isLabel = first.rfind("By:", 0) == 0 || first.rfind("Name:", 0) == 0 || first.rfind("Title:", 0) == 0;
			if (plainName.empty() == false && isLabel == false) {
				SigningEntity entity;
				entity.name = first;
				entity.blockId = block.id;
				entity.isNamedInPreamble = plainPreamble.find(" " + plainName + " ") != std::string::npos;
				document.signatories.push_back(entity);
			}
			if (end == std::string::npos) break;
			start = end + 3;
		}
	}
}

// Words in the document that speak to a model, not to the parties. Kept as
// the document's text; never followed.
void findEmbeddedInstructions(LegalDocument& document) {

	static const std::vector<std::string> signals = {
		"ai review", "ai tool", "ai assistant", "ai model", "language model", "llm", "chatgpt", "ignore previous",
		"ignore all previous", "ignore the above", "disregard previous", "disregard the above", "system prompt",
		"as an ai", "you are an ai", "note to ai", "for ai reviewers",
	};
	for (const LegalBlock& block : document.blocks) {
		std::string lower = block.text;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		for (const std::string& signal : signals) {
			if (lower.find(signal) == std::string::npos) continue;
			document.embeddedInstructions.push_back({block.id, block.text});
			break;
		}
	}

}

std::string uniqueId(std::set<std::string>& used, const std::string& wanted) {
	std::string id = wanted;
	int suffix = 2;
	while (used.count(id) > 0) id = wanted + "~" + std::to_string(suffix++);
	used.insert(id);
	return id;
}

Json blockToJson(const LegalBlock& block) {
	Json value = Json{{"id", block.id}, {"kind", toString(block.kind)}, {"number", block.number}, {"heading", block.heading},
		{"text", block.text}, {"path", block.path}, {"section", block.section}, {"annotates", block.annotates},
		{"author", block.author}, {"order", block.order}, {"sourceIndex", block.sourceIndex},
		{"fingerprint", block.fingerprint}};
	if (block.pendingChanges.empty() == false) {
		Json changes = Json::array();
		for (const PendingChange& change : block.pendingChanges) {
			changes.push_back(Json{{"kind", change.kind}, {"author", change.author}, {"text", change.text},
				{"isWholeParagraph", change.isWholeParagraph}});
		}
		value["pendingChanges"] = changes;
		value["originalText"] = block.originalText;
		value["isPendingInsertion"] = block.isPendingInsertion;
	}
	if (block.rows.empty() == false) {
		Json rows = Json::array();
		for (const TableRowReading& row : block.rows) {
			Json continued = Json::array();
			for (std::size_t index = 0; index < row.isContinued.size(); ++index) {
				if (row.isContinued[index] == true) continued.push_back(index);
			}
			rows.push_back(Json{{"cells", row.cells}, {"continued", continued}});
		}
		value["rows"] = rows;
	}
	return value;
}

BlockKind blockKindFromString(const std::string& value) {
	if (value == "title") return BlockKind::Title;
	if (value == "heading") return BlockKind::Heading;
	if (value == "clause") return BlockKind::Clause;
	if (value == "table") return BlockKind::Table;
	if (value == "signature") return BlockKind::Signature;
	if (value == "exhibit_heading") return BlockKind::ExhibitHeading;
	if (value == "header_footer") return BlockKind::HeaderFooter;
	if (value == "comment") return BlockKind::Comment;
	if (value == "note") return BlockKind::Note;
	return BlockKind::Paragraph;
}

bool isAgreementText(const LegalBlock& block) {
	return block.kind != BlockKind::Comment && block.kind != BlockKind::HeaderFooter;
}

} // namespace

const char* toString(BlockKind kind) {
	switch (kind) {
		case BlockKind::Title: return "title";
		case BlockKind::Paragraph: return "paragraph";
		case BlockKind::Heading: return "heading";
		case BlockKind::Clause: return "clause";
		case BlockKind::Table: return "table";
		case BlockKind::Signature: return "signature";
		case BlockKind::ExhibitHeading: return "exhibit_heading";
		case BlockKind::HeaderFooter: return "header_footer";
		case BlockKind::Comment: return "comment";
		case BlockKind::Note: return "note";
	}
	return "paragraph";
}

const char* legalStructureVersion() {
	return "legal-structure/3";
}

std::string normalizedSpacing(const std::string& text) {

	std::string result;
	result.reserve(text.size());
	bool isSpace = false;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const char character = text[index];
		// A no-break space (U+00A0) is spacing too.
		const bool isNoBreak = index + 1 < text.size() && static_cast<unsigned char>(character) == 0xC2
			&& static_cast<unsigned char>(text[index + 1]) == 0xA0;
		if (isAsciiSpace(character) == true || isNoBreak == true) {
			if (isNoBreak == true) ++index;
			isSpace = true;
			continue;
		}
		if (isSpace == true && result.empty() == false) result += ' ';
		isSpace = false;
		result += character;
	}
	return result;

}

LegalDocument buildLegalDocument(const SourceIdentity& identity, const std::vector<SourceParagraph>& paragraphs,
	const std::vector<SourceComment>& comments, const std::vector<std::pair<std::string, std::string>>& headerFooter,
	const std::vector<DocumentProperty>& properties, const std::map<std::string, std::string>& notes) {

	LegalDocument document;
	document.source = identity;
	document.properties = properties;
	document.structureVersion = legalStructureVersion();

	std::map<std::string, const SourceComment*> commentById;
	for (const SourceComment& comment : comments) commentById[comment.id] = &comment;

	std::set<std::string> usedIds;
	std::string heading;
	std::string headingNumber;
	std::string section = "body";
	std::string exhibitLetter;
	bool isExhibitTitleNext = false;
	std::size_t tableCount = 0;
	std::size_t signatureCount = 0;
	std::size_t footnoteCount = 0;
	std::size_t endnoteCount = 0;
	std::size_t exhibitParagraphs = 0;
	std::size_t numberedClauses = 0;

	for (const SourceParagraph& paragraph : paragraphs) {
		const std::string text = trimmed(paragraph.text);
		if (text.empty() == true) {
			// A paragraph struck as a whole, the strike not yet accepted: no words
			// stand, and the proposal is kept with the block before it.
			for (const PendingChange& change : paragraph.pendingChanges) {
				if (change.kind != "struck" || document.blocks.empty() == true) continue;
				PendingChange whole = change;
				whole.isWholeParagraph = true;
				document.blocks.back().pendingChanges.push_back(whole);
			}
			continue;
		}
		LegalBlock block;
		block.text = text;
		if (paragraph.isTable == true) block.rows = paragraph.rows;
		block.pendingChanges = paragraph.pendingChanges;
		if (block.pendingChanges.empty() == false) {
			block.originalText = trimmed(paragraph.originalText);
			block.isPendingInsertion = paragraph.isMarkInserted == true && block.originalText.empty() == true;
		}
		block.order = document.blocks.size();
		block.sourceIndex = paragraph.sourceIndex;
		block.fingerprint = fingerprintOf(text);
		const std::string number = clauseNumber(paragraph.drawnNumber);
		const ExhibitHeading exhibit = paragraph.isTable == false && number.empty() == true
			? exhibitHeadingOf(text) : ExhibitHeading();

		if (exhibit.isExhibit == true) {
			// An exhibit begins: everything after it is the exhibit's, until the next.
			exhibitLetter = exhibit.label.substr(exhibit.label.find(' ') + 1);
			section = "exhibit:" + exhibitLetter;
			heading = exhibit.label;
			headingNumber.clear();
			exhibitParagraphs = 0;
			block.kind = BlockKind::ExhibitHeading;
			block.id = uniqueId(usedIds, "x-" + exhibitLetter);
			block.path = exhibit.label;
			ExhibitStatus status;
			status.label = exhibit.label;
			status.isPresent = true;
			status.title = exhibit.title;
			const auto known = std::find_if(document.exhibits.begin(), document.exhibits.end(),
				[&](const ExhibitStatus& entry) { return entry.label == exhibit.label; });
			if (known == document.exhibits.end()) document.exhibits.push_back(status);
			else {
				known->isPresent = true;
				known->title = exhibit.title;
			}
			isExhibitTitleNext = exhibit.title.empty() == true;
		} else if (isExhibitTitleNext == true && paragraph.isTable == false && text.size() < 160
			&& (paragraph.isHeading == true || paragraph.styleId.find("Exhibit") != std::string::npos
				|| paragraph.styleId.find("Title") != std::string::npos)) {
			// The exhibit's own title, on the line after its label.
			isExhibitTitleNext = false;
			block.kind = BlockKind::Heading;
			block.id = uniqueId(usedIds, "x-" + exhibitLetter + "-title");
			block.path = heading;
			block.section = section;
			for (ExhibitStatus& status : document.exhibits) {
				if (status.label == heading) status.title = withoutFinalStop(text);
			}
		} else if (paragraph.isTable == true) {
			isExhibitTitleNext = false;
			const bool isSignature = text.find("By:") != std::string::npos
				&& (text.find("Name:") != std::string::npos || text.find("Title:") != std::string::npos);
			if (isSignature == true) {
				block.kind = BlockKind::Signature;
				block.id = uniqueId(usedIds, "sig-" + std::to_string(++signatureCount));
				block.path = "Signature page";
			} else {
				block.kind = BlockKind::Table;
				block.id = uniqueId(usedIds, "t-" + std::to_string(++tableCount));
				block.path = section == "body" ? (heading.empty() == true ? std::string("Table") : heading) : heading;
			}
		} else if (paragraph.isTitle == true) {
			block.kind = BlockKind::Title;
			block.id = uniqueId(usedIds, "title");
			block.path = "Title";
			if (document.title.empty() == true) document.title = text;
		} else if (number.empty() == false && section == "body"
			&& (paragraph.isHeading == true || (isTopLevelNumber(number) == true && text.size() < 80
				&& text.find(". ") == std::string::npos))) {
			isExhibitTitleNext = false;
			block.kind = BlockKind::Heading;
			block.number = number;
			heading = withoutFinalStop(text);
			headingNumber = number;
			block.id = uniqueId(usedIds, "h-" + number);
			block.path = number + " " + heading;
		} else if (number.empty() == false) {
			isExhibitTitleNext = false;
			++numberedClauses;
			block.kind = BlockKind::Clause;
			block.number = number;
			block.heading = heading;
			block.id = uniqueId(usedIds, section == "body" ? "s-" + number : "x-" + exhibitLetter + "-s-" + number);
			if (headingNumber.empty() == false) block.path = headingNumber + " " + heading + " > " + number;
			else block.path = heading.empty() == true ? number : heading + " > " + number;
		} else {
			isExhibitTitleNext = false;
			block.kind = BlockKind::Paragraph;
			if (section == "body") {
				block.id = uniqueId(usedIds, "p-" + std::to_string(block.order));
				block.path = heading.empty() == true ? std::string("Opening") : heading;
			} else {
				block.id = uniqueId(usedIds, "x-" + exhibitLetter + "-p-" + std::to_string(++exhibitParagraphs));
				block.path = heading;
			}
		}
		if (block.section.empty() == true) block.section = section;
		document.blocks.push_back(block);

		// The notes in its margin follow it, each as a block of its own.
		const std::string annotated = block.id;
		const std::string annotatedName = block.number.empty() == false ? "Section " + block.number : block.path;
		for (const std::string& commentId : paragraph.commentIds) {
			const auto found = commentById.find(commentId);
			if (found == commentById.end()) continue;
			const std::string commentText = trimmed(found->second->text);
			if (commentText.empty() == true) continue;
			LegalBlock note;
			note.kind = BlockKind::Comment;
			note.id = uniqueId(usedIds, "c-" + commentId);
			note.text = commentText;
			note.author = found->second->author;
			note.annotates = annotated;
			note.section = "comment";
			note.path = "Comment on " + annotatedName;
			note.order = document.blocks.size();
			note.fingerprint = fingerprintOf(commentText);
			document.blocks.push_back(note);
		}
		for (const std::string& noteId : paragraph.noteIds) {
			const auto found = notes.find(noteId);
			if (found == notes.end()) continue;
			const bool isFootnote = noteId.rfind("footnote:", 0) == 0;
			// Word numbers footnotes and endnotes each from one.
			const std::size_t noteCount = isFootnote == true ? ++footnoteCount : ++endnoteCount;
			LegalBlock note;
			note.kind = BlockKind::Note;
			note.id = uniqueId(usedIds, std::string(isFootnote == true ? "fn-" : "en-") + std::to_string(noteCount));
			note.text = found->second;
			note.annotates = annotated;
			note.section = isFootnote == true ? "footnote" : "endnote";
			note.path = std::string(isFootnote == true ? "Footnote " : "Endnote ") + std::to_string(noteCount) + " to " + annotatedName;
			note.order = document.blocks.size();
			note.fingerprint = fingerprintOf(note.text);
			document.blocks.push_back(note);
		}
	}

	std::size_t headerFooterCount = 0;
	for (const std::pair<std::string, std::string>& part : headerFooter) {
		const std::string text = trimmed(part.second);
		if (text.empty() == true) continue;
		LegalBlock block;
		block.kind = BlockKind::HeaderFooter;
		block.id = uniqueId(usedIds, "hf-" + std::to_string(++headerFooterCount));
		block.text = text;
		block.section = part.first;
		block.path = part.first == "header" ? "Page header" : "Page footer";
		block.order = document.blocks.size();
		block.fingerprint = fingerprintOf(text);
		document.blocks.push_back(block);
	}

	findDefinitions(document);
	findReferences(document);
	findEmbeddedInstructions(document);
	findSignatories(document);

	if (numberedClauses == 0) {
		document.limitations.push_back("No clause in this document carries a number Singularity could read; its "
			"paragraphs are named by their order instead.");
	}
	return document;

}

StructureReading readDocxStructure(const SourceIntake& intake) {

	StructureReading reading;
	if (intake.isAccepted == false || intake.identity.format != "docx") {
		reading.problem = intake.refusal.empty() == false ? intake.refusal : "The document is not a Word document.";
		return reading;
	}
	pugi::xml_document document;
	if (parseXml(partOr(intake, "word/document.xml"), document) == false) {
		reading.problem = "The Word document's text could not be read: its XML is damaged.";
		return reading;
	}
	pugi::xml_node body;
	for (const pugi::xml_node& child : document.children()) {
		if (child.type() == pugi::node_element && localName(child) == "document") body = childElement(child, "body");
	}
	if (!body) {
		reading.problem = "The Word document has no body text.";
		return reading;
	}

	// What Word draws in front of each numbered paragraph, and which paragraphs
	// its styles call headings - read the way the document writer reads them
	// (Document/DocxExtractor), so a block's number is the number a lawyer sees.
	const std::string stylesXml = partOr(intake, "word/styles.xml");
	const std::string numberingXml = partOr(intake, "word/numbering.xml");
	std::map<const void*, std::string> drawnNumbers;
	for (const std::pair<pugi::xml_node, std::string>& entry : documents::docx_drawn_numbers(body, stylesXml, numberingXml)) {
		drawnNumbers[entry.first.internal_object()] = entry.second;
	}
	documents::TextEditRequest packageReading;
	documents::read_docx_numbering_into(packageReading, stylesXml, numberingXml);

	std::vector<SourceParagraph> paragraphs;
	std::size_t revisions = 0;
	std::size_t sourceIndex = 0;
	std::vector<pugi::xml_node> pending;
	for (pugi::xml_node child = body.last_child(); child; child = child.previous_sibling()) pending.push_back(child);
	while (pending.empty() == false) {
		const pugi::xml_node node = pending.back();
		pending.pop_back();
		if (node.type() != pugi::node_element) continue;
		const std::string name = localName(node);
		if (name == "sdt") {
			// A content control: its content stands in the body in its place.
			const pugi::xml_node content = childElement(node, "sdtContent");
			for (pugi::xml_node child = content.last_child(); child; child = child.previous_sibling()) pending.push_back(child);
			continue;
		}
		if (name == "tbl") {
			SourceParagraph paragraph;
			paragraph.isTable = true;
			paragraph.text = tableText(node, revisions, &paragraph.pendingChanges, &paragraph.unreadObjects, &paragraph.rows);
			paragraph.sourceIndex = sourceIndex++;
			paragraphs.push_back(paragraph);
			continue;
		}
		if (name != "p") continue;
		SourceParagraph paragraph;
		ParagraphExtras extras;
		extras.changes = &paragraph.pendingChanges;
		extras.noteIds = &paragraph.noteIds;
		extras.objects = &paragraph.unreadObjects;
		collectParagraphText(node, paragraph.text, revisions, paragraph.commentIds, &extras);
		if (paragraph.pendingChanges.empty() == false) collectOriginalText(node, paragraph.originalText);
		paragraph.isMarkInserted = isMarkInserted(node);
		paragraph.styleId = styleOf(node);
		std::string lowerStyle = paragraph.styleId;
		std::transform(lowerStyle.begin(), lowerStyle.end(), lowerStyle.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		paragraph.isTitle = lowerStyle == "title";
		paragraph.isHeading = packageReading.is_heading ? packageReading.is_heading(node) : false;
		const auto drawn = drawnNumbers.find(node.internal_object());
		if (drawn != drawnNumbers.end()) paragraph.drawnNumber = drawn->second;
		paragraph.sourceIndex = sourceIndex++;
		paragraphs.push_back(paragraph);
	}

	std::vector<SourceComment> comments;
	pugi::xml_document commentsDocument;
	if (parseXml(partOr(intake, "word/comments.xml"), commentsDocument) == true) {
		for (pugi::xml_node node = commentsDocument.document_element().first_child(); node; node = node.next_sibling()) {
			if (node.type() != pugi::node_element || localName(node) != "comment") continue;
			comments.push_back({attributeValue(node, "id"), attributeValue(node, "author"), partText(node)});
		}
	}

	std::vector<std::pair<std::string, std::string>> headerFooter;
	for (const std::pair<const std::string, std::string>& part : intake.parts) {
		const bool isHeader = part.first.rfind("word/header", 0) == 0;
		const bool isFooter = part.first.rfind("word/footer", 0) == 0;
		if (isHeader == false && isFooter == false) continue;
		pugi::xml_document partDocument;
		if (parseXml(part.second, partDocument) == false) continue;
		headerFooter.push_back({isHeader == true ? "header" : "footer", partText(partDocument.document_element())});
	}

	// Footnotes and endnotes: their words, by the id their marks name. The
	// separators Word keeps among them are not notes.
	std::map<std::string, std::string> notes;
	for (const std::pair<const char*, const char*>& kind : {std::pair<const char*, const char*>{"word/footnotes.xml", "footnote"},
			std::pair<const char*, const char*>{"word/endnotes.xml", "endnote"}}) {
		pugi::xml_document notesDocument;
		if (parseXml(partOr(intake, kind.first), notesDocument) == false) continue;
		for (pugi::xml_node node = notesDocument.document_element().first_child(); node; node = node.next_sibling()) {
			if (node.type() != pugi::node_element || (localName(node) != "footnote" && localName(node) != "endnote")) continue;
			const std::string type = attributeValue(node, "type");
			if (type == "separator" || type == "continuationSeparator" || type == "continuationNotice") continue;
			const std::string words = trimmed(partText(node));
			if (words.empty() == false) notes[std::string(kind.second) + ":" + attributeValue(node, "id")] = words;
		}
	}

	reading.document = buildLegalDocument(intake.identity, paragraphs, comments, headerFooter, readProperties(intake), notes);
	reading.document.pendingRevisions = revisions;
	if (revisions > 0) {
		std::set<std::string> authors;
		for (const LegalBlock& block : reading.document.blocks) {
			for (const PendingChange& change : block.pendingChanges) authors.insert(change.author.empty() == true ? "someone" : change.author);
		}
		std::string names;
		for (const std::string& author : authors) names += (names.empty() == true ? "" : ", ") + author;
		reading.document.limitations.push_back("The document holds " + std::to_string(revisions)
			+ " tracked changes nobody has accepted or rejected, by " + (names.empty() == true ? std::string("someone") : names)
			+ ". Each is kept as pending with its clause: the words as they would stand, and the words before it.");
	}

	// The source manifest: every part of the package, and what the reading did
	// with it.
	for (const std::string& name : intake.packageParts) {
		SourcePart entry;
		entry.name = name;
		const auto startsWith = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
		if (name == "word/document.xml" || startsWith("word/header") || startsWith("word/footer") || name == "word/comments.xml"
			|| name == "word/footnotes.xml" || name == "word/endnotes.xml") {
			entry.state = "read";
		} else if (name == "word/numbering.xml" || name == "word/styles.xml") {
			entry.state = "used";
			entry.note = "how paragraphs are numbered and which are headings";
		} else if (name == "docProps/core.xml" || name == "docProps/app.xml") {
			entry.state = "read";
			entry.note = "the file's properties: who wrote it, kept apart from its terms";
		} else if (startsWith("docProps/")) {
			entry.state = "not_read";
			entry.note = "properties the file's author added";
		} else if (startsWith("word/media/")) {
			entry.state = "not_read";
			entry.note = "a picture";
		} else if (startsWith("word/embeddings/")) {
			entry.state = "not_read";
			entry.note = "an embedded file or object";
		} else if (startsWith("customXml/") || startsWith("word/glossary/")) {
			entry.state = "not_read";
			entry.note = "data the document carries outside its text";
		} else if (name == "word/settings.xml" || name == "word/fontTable.xml" || startsWith("word/theme/") || name == "word/webSettings.xml"
			|| name == "[Content_Types].xml" || name.find("_rels/") != std::string::npos || name == "word/stylesWithEffects.xml") {
			entry.state = "not_needed";
			entry.note = "how the file is laid out, with no words of its own";
		} else {
			entry.state = "not_read";
			entry.note = "a part the reading does not know";
		}
		reading.document.parts.push_back(entry);
	}
	// Addresses outside the file that the document links to: what they hold is
	// not in it.
	pugi::xml_document relations;
	if (parseXml(partOr(intake, "word/_rels/document.xml.rels"), relations) == true) {
		for (pugi::xml_node node = relations.document_element().first_child(); node; node = node.next_sibling()) {
			if (node.type() != pugi::node_element || attributeValue(node, "TargetMode") != "External") continue;
			const std::string target = attributeValue(node, "Target");
			if (target.empty() == false) reading.document.externalTargets.push_back(target);
		}
	}
	std::size_t unread = 0;
	for (const SourceParagraph& paragraph : paragraphs) unread += paragraph.unreadObjects;
	for (const SourcePart& part : reading.document.parts) {
		if (part.state != "not_read" || part.note == "a picture") continue;
		reading.document.unreadContent.push_back(part.name + ": " + part.note);
	}
	if (unread > 0) {
		reading.document.unreadContent.push_back(std::to_string(unread) + (unread == 1 ? " picture or object" : " pictures or objects")
			+ " in the body, whose content is not read");
		reading.document.limitations.push_back("The document holds " + std::to_string(unread)
			+ (unread == 1 ? " picture or embedded object" : " pictures or embedded objects")
			+ " Singularity does not read; whatever it shows is not in the review.");
	}
	if (reading.document.externalTargets.empty() == false) {
		reading.document.limitations.push_back("The document links to " + std::to_string(reading.document.externalTargets.size())
			+ (reading.document.externalTargets.size() == 1 ? " address" : " addresses")
			+ " outside it; what they hold is not in the document.");
	}
	for (const std::string& note : intake.notes) reading.document.limitations.push_back(note);
	reading.isRead = true;
	return reading;

}

StructureReading readTextStructure(const SourceIdentity& identity, const std::string& text) {

	StructureReading reading;
	static const std::regex typedNumber(R"(^\s*(\d+(?:\.\d+)*)\.?\s+(\S.*)$)");
	std::vector<SourceParagraph> paragraphs;
	std::string current;
	std::size_t sourceIndex = 0;
	const auto close = [&]() {
		const std::string words = trimmed(current);
		current.clear();
		if (words.empty() == true) return;
		SourceParagraph paragraph;
		paragraph.sourceIndex = sourceIndex++;
		std::smatch match;
		if (std::regex_match(words, match, typedNumber) == true) {
			paragraph.drawnNumber = match[1].str();
			paragraph.text = match[2].str();
			paragraph.isHeading = isTopLevelNumber(match[1].str()) == true && paragraph.text.size() < 60
				&& paragraph.text.find(". ") == std::string::npos;
		} else {
			paragraph.text = words;
		}
		paragraphs.push_back(paragraph);
	};
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t end = text.find('\n', start);
		const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (trimmed(line).empty() == true) close();
		else if (std::regex_match(trimmed(line), typedNumber) == true) {
			close();
			current = line;
		} else {
			if (current.empty() == false) current += ' ';
			current += trimmed(line);
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	close();
	if (paragraphs.empty() == true) {
		reading.problem = "The document has no text Singularity could read.";
		return reading;
	}
	reading.document = buildLegalDocument(identity, paragraphs, {}, {}, {});
	reading.document.limitations.push_back("Read from the document's text: a PDF or text file declares no numbering or "
		"headings of its own, so clause numbers are the ones typed at each paragraph's head.");
	reading.isRead = true;
	return reading;

}

namespace {

Json signatoriesJson(const LegalDocument& document) {
	Json signatories = Json::array();
	for (const SigningEntity& entity : document.signatories) {
		signatories.push_back(Json{{"name", entity.name}, {"blockId", entity.blockId}, {"isNamedInPreamble", entity.isNamedInPreamble}});
	}
	return signatories;
}

Json partsJson(const LegalDocument& document) {
	Json parts = Json::array();
	for (const SourcePart& part : document.parts) parts.push_back(Json{{"name", part.name}, {"state", part.state}, {"note", part.note}});
	return parts;
}

} // namespace

Json legalDocumentToJson(const LegalDocument& document) {

	Json blocks = Json::array();
	for (const LegalBlock& block : document.blocks) blocks.push_back(blockToJson(block));
	Json definitions = Json::array();
	for (const DefinedTerm& definition : document.definitions) {
		definitions.push_back(Json{{"term", definition.term}, {"blockId", definition.blockId},
			{"definition", definition.definition}, {"usedIn", definition.usedIn}});
	}
	Json references = Json::array();
	for (const CrossReference& reference : document.references) {
		references.push_back(Json{{"label", reference.label}, {"kind", reference.kind},
			{"fromBlockId", reference.fromBlockId}, {"targetBlockId", reference.targetBlockId},
			{"isResolved", reference.isResolved}});
	}
	Json exhibits = Json::array();
	for (const ExhibitStatus& exhibit : document.exhibits) {
		exhibits.push_back(Json{{"label", exhibit.label}, {"isPresent", exhibit.isPresent}, {"title", exhibit.title},
			{"referencedIn", exhibit.referencedIn}});
	}
	Json properties = Json::array();
	for (const DocumentProperty& property : document.properties) {
		properties.push_back(Json{{"name", property.name}, {"value", property.value}});
	}
	Json instructions = Json::array();
	for (const EmbeddedInstruction& instruction : document.embeddedInstructions) {
		instructions.push_back(Json{{"blockId", instruction.blockId}, {"text", instruction.text}});
	}
	const SourceIdentity& source = document.source;
	return Json{
		{"source", Json{{"fileName", source.fileName}, {"format", source.format}, {"sha256", source.sha256},
			{"sizeBytes", source.sizeBytes}, {"matterId", source.matterId}, {"documentId", source.documentId},
			{"documentVersion", source.documentVersion}}},
		{"title", document.title},
		{"blocks", std::move(blocks)},
		{"definitions", std::move(definitions)},
		{"references", std::move(references)},
		{"exhibits", std::move(exhibits)},
		{"properties", std::move(properties)},
		{"embeddedInstructions", std::move(instructions)},
		{"limitations", document.limitations},
		{"pendingRevisions", document.pendingRevisions},
		{"signatories", signatoriesJson(document)},
		{"parts", partsJson(document)},
		{"externalTargets", document.externalTargets},
		{"unreadContent", document.unreadContent},
		{"structureVersion", document.structureVersion},
	};

}

LegalDocument legalDocumentFromJson(const Json& value) {

	LegalDocument document;
	const Json source = value.value("source", Json::object());
	document.source.fileName = source.value("fileName", std::string());
	document.source.format = source.value("format", std::string());
	document.source.sha256 = source.value("sha256", std::string());
	document.source.sizeBytes = source.value("sizeBytes", 0LL);
	document.source.matterId = source.value("matterId", std::string());
	document.source.documentId = source.value("documentId", std::string());
	document.source.documentVersion = source.value("documentVersion", 0);
	document.title = value.value("title", std::string());
	for (const Json& entry : value.value("blocks", Json::array())) {
		LegalBlock block;
		block.id = entry.value("id", std::string());
		block.kind = blockKindFromString(entry.value("kind", std::string()));
		block.number = entry.value("number", std::string());
		block.heading = entry.value("heading", std::string());
		block.text = entry.value("text", std::string());
		block.path = entry.value("path", std::string());
		block.section = entry.value("section", std::string());
		block.annotates = entry.value("annotates", std::string());
		block.author = entry.value("author", std::string());
		block.order = entry.value("order", static_cast<std::size_t>(0));
		block.sourceIndex = entry.value("sourceIndex", static_cast<std::size_t>(0));
		block.fingerprint = entry.value("fingerprint", std::string());
		for (const Json& change : entry.value("pendingChanges", Json::array())) {
			block.pendingChanges.push_back({change.value("kind", std::string()), change.value("author", std::string()),
				change.value("text", std::string()), change.value("isWholeParagraph", false)});
		}
		block.originalText = entry.value("originalText", std::string());
		block.isPendingInsertion = entry.value("isPendingInsertion", false);
		for (const Json& row : entry.value("rows", Json::array())) {
			TableRowReading reading;
			reading.cells = row.value("cells", std::vector<std::string>());
			reading.isContinued.assign(reading.cells.size(), false);
			for (const Json& index : row.value("continued", Json::array())) {
				if (index.is_number_unsigned() == true && index.get<std::size_t>() < reading.isContinued.size()) {
					reading.isContinued[index.get<std::size_t>()] = true;
				}
			}
			block.rows.push_back(std::move(reading));
		}
		document.blocks.push_back(block);
	}
	for (const Json& entry : value.value("definitions", Json::array())) {
		DefinedTerm definition;
		definition.term = entry.value("term", std::string());
		definition.blockId = entry.value("blockId", std::string());
		definition.definition = entry.value("definition", std::string());
		definition.usedIn = entry.value("usedIn", std::vector<std::string>());
		document.definitions.push_back(definition);
	}
	for (const Json& entry : value.value("references", Json::array())) {
		CrossReference reference;
		reference.label = entry.value("label", std::string());
		reference.kind = entry.value("kind", std::string());
		reference.fromBlockId = entry.value("fromBlockId", std::string());
		reference.targetBlockId = entry.value("targetBlockId", std::string());
		reference.isResolved = entry.value("isResolved", false);
		document.references.push_back(reference);
	}
	for (const Json& entry : value.value("exhibits", Json::array())) {
		ExhibitStatus exhibit;
		exhibit.label = entry.value("label", std::string());
		exhibit.isPresent = entry.value("isPresent", false);
		exhibit.title = entry.value("title", std::string());
		exhibit.referencedIn = entry.value("referencedIn", std::vector<std::string>());
		document.exhibits.push_back(exhibit);
	}
	for (const Json& entry : value.value("properties", Json::array())) {
		document.properties.push_back({entry.value("name", std::string()), entry.value("value", std::string())});
	}
	for (const Json& entry : value.value("embeddedInstructions", Json::array())) {
		document.embeddedInstructions.push_back({entry.value("blockId", std::string()), entry.value("text", std::string())});
	}
	document.limitations = value.value("limitations", std::vector<std::string>());
	document.pendingRevisions = value.value("pendingRevisions", static_cast<std::size_t>(0));
	for (const Json& entry : value.value("signatories", Json::array())) {
		document.signatories.push_back({entry.value("name", std::string()), entry.value("blockId", std::string()),
			entry.value("isNamedInPreamble", false)});
	}
	for (const Json& entry : value.value("parts", Json::array())) {
		document.parts.push_back({entry.value("name", std::string()), entry.value("state", std::string()),
			entry.value("note", std::string())});
	}
	document.externalTargets = value.value("externalTargets", std::vector<std::string>());
	document.unreadContent = value.value("unreadContent", std::vector<std::string>());
	document.structureVersion = value.value("structureVersion", std::string());
	return document;

}

const LegalBlock* findBlock(const LegalDocument& document, const std::string& blockId) {
	for (const LegalBlock& block : document.blocks) {
		if (block.id == blockId) return &block;
	}
	return nullptr;
}

std::vector<std::string> blocksHoldingPhrase(const LegalDocument& document, const std::string& phrase) {

	std::vector<std::string> found;
	const std::string wanted = platform::trimmed_phrase(phrase);
	if (wanted.empty() == true) return found;
	for (const LegalBlock& block : document.blocks) {
		if (isAgreementText(block) == false) continue;
		if (platform::locate_phrase(block.text, wanted).matches > 0) found.push_back(block.id);
	}
	return found;

}

std::size_t placesHoldingPhrase(const LegalDocument& document, const std::string& phrase) {

	std::size_t places = 0;
	const std::string wanted = platform::trimmed_phrase(phrase);
	if (wanted.empty() == true) return 0;
	for (const LegalBlock& block : document.blocks) {
		if (isAgreementText(block) == false) continue;
		places += platform::locate_phrase(block.text, wanted).matches;
		if (places > 1) return places;
	}
	return places;

}

std::vector<StructureChange> compareStructures(const LegalDocument& previous, const LegalDocument& current) {

	std::map<std::string, std::string> before;
	for (const LegalBlock& block : previous.blocks) before[block.id] = block.fingerprint;
	std::vector<StructureChange> changes;
	std::set<std::string> seen;
	for (const LegalBlock& block : current.blocks) {
		seen.insert(block.id);
		const auto found = before.find(block.id);
		if (found == before.end()) changes.push_back({block.id, "added"});
		else if (found->second != block.fingerprint) changes.push_back({block.id, "changed"});
	}
	for (const LegalBlock& block : previous.blocks) {
		if (seen.count(block.id) == 0) changes.push_back({block.id, "removed"});
	}
	return changes;

}

std::vector<std::string> dependentBlocksOf(const LegalDocument& document, const std::string& blockId) {

	std::set<std::string> found;
	for (const CrossReference& reference : document.references) {
		if (reference.targetBlockId == blockId) found.insert(reference.fromBlockId);
	}
	for (const DefinedTerm& definition : document.definitions) {
		if (definition.blockId != blockId) continue;
		for (const std::string& use : definition.usedIn) found.insert(use);
	}
	for (const LegalBlock& block : document.blocks) {
		if (block.annotates == blockId) found.insert(block.id);
	}
	found.erase(blockId);
	std::vector<std::string> ordered;
	for (const LegalBlock& block : document.blocks) {
		if (found.count(block.id) > 0) ordered.push_back(block.id);
	}
	return ordered;

}

} // namespace singularity::document_intelligence
