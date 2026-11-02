#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "DocumentIntelligence/SourceIntake/SourceIntake.hpp"
#include "DocumentIntelligence/Types/LegalDocument.hpp"

namespace singularity::document_intelligence {

// One paragraph, table or note of a source as it is taken in, before its legal
// structure is worked out: its words, the number Word draws in front of it,
// whether the document's own styles call it a heading or its title, and where
// it stands. Structure is read from what the document declares - its
// numbering, its styles - never guessed by a model.
struct SourceParagraph {
	std::string text;
	// As Word draws it ("6.", "6.1") or as it is typed at the paragraph's head
	// in a document that has no numbering of its own.
	std::string drawnNumber;
	bool isHeading = false;
	bool isTitle = false;
	bool isTable = false;
	std::string styleId;
	std::size_t sourceIndex = 0;
	// The comments anchored in it, by their ids in the package.
	std::vector<std::string> commentIds;
	// Its tracked changes nobody has accepted or rejected, in order, and its
	// words before them (struck words in, inserted words out).
	std::vector<PendingChange> pendingChanges;
	std::string originalText;
	// Its paragraph mark is a pending insertion: the whole paragraph is one.
	bool isMarkInserted = false;
	// The footnotes and endnotes whose marks it carries ("footnote:2").
	std::vector<std::string> noteIds;
	// Pictures and embedded objects in it, which the reading does not read.
	std::size_t unreadObjects = 0;
	// A table's rows, cell by cell, as its text shows them.
	std::vector<TableRowReading> rows;
};

struct SourceComment {
	std::string id;
	std::string author;
	std::string text;
};

// What reading a source gave: its legal structure, or why there is none.
struct StructureReading {
	bool isRead = false;
	std::string problem;
	LegalDocument document;
};

// The legal structure of paragraphs already taken in: blocks with stable ids,
// the terms the document defines and where it uses them, what it refers to
// and whether it holds it, the exhibits it has and lacks, and any words in it
// addressed to a model. `headerFooter` holds ("header" or "footer", words).
LegalDocument buildLegalDocument(const SourceIdentity& identity, const std::vector<SourceParagraph>& paragraphs,
	const std::vector<SourceComment>& comments,
	const std::vector<std::pair<std::string, std::string>>& headerFooter,
	const std::vector<DocumentProperty>& properties,
	const std::map<std::string, std::string>& notes = {});

// A Word document taken in (SourceIntake), read from its own parts: its
// paragraphs and tables in order, the numbers Word draws for them, the
// headings its styles declare, the comments in its margin, its header and
// footer and its properties. Tracked changes still pending are read as they
// stand - inserted words in, struck words out - and counted.
StructureReading readDocxStructure(const SourceIntake& intake);

// A PDF or a text document, from the text its extractor gave: one paragraph
// per block of lines, a number typed at a paragraph's head taken as its
// clause number. Less than a Word document declares, and said so.
StructureReading readTextStructure(const SourceIdentity& identity, const std::string& text);

nlohmann::json legalDocumentToJson(const LegalDocument& document);
LegalDocument legalDocumentFromJson(const nlohmann::json& value);

const char* legalStructureVersion();

const LegalBlock* findBlock(const LegalDocument& document, const std::string& blockId);

// The blocks of the agreement's own text - not a comment, a header or a
// footer - whose words hold `phrase`, judged as every quotation is judged
// (Platform/PhraseMatch): typography never decides, the words do.
std::vector<std::string> blocksHoldingPhrase(const LegalDocument& document, const std::string& phrase);

// How many places in the agreement's own text `phrase` stands at, across
// every block: a change is written only where this is exactly one.
std::size_t placesHoldingPhrase(const LegalDocument& document, const std::string& phrase);

// What changed between the readings of two versions of one document, block
// by block: "added", "removed" or "changed".
struct StructureChange {
	std::string blockId;
	std::string change;
};
std::vector<StructureChange> compareStructures(const LegalDocument& previous, const LegalDocument& current);

// Words with every run of spacing made one space and the ends trimmed: what a
// block's fingerprint is taken of.
std::string normalizedSpacing(const std::string& text);

} // namespace singularity::document_intelligence
