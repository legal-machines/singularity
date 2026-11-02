#pragma once

#include "DocumentIntelligence/MaterialFacts/MaterialFacts.hpp"
#include "DocumentIntelligence/Types/LegalDocument.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace singularity::matters {
struct Matter;
}

namespace singularity::document_review {

// The facts a colleague's work rests on, checked across the Matter's documents
// on the lawyer's computer (D-119; DocumentIntelligence/MaterialFacts): what a
// part is told disagrees before it works, and what its saved draft still gets
// wrong after. Every value of a draft is accounted for - it agrees with a
// source, differs from one, or no document gives it - so no part is called
// reconciled while a value it holds disagrees with the document it comes from.

// A document's statements as Singularity reads it: a Word document by its
// clauses and its tables' rows, under their headings; a workbook by its cells,
// each under its column's header and its row; a deck by its slides' lines; any
// other document line by line. An agreement, a letter, a PDF and notes are
// sources; a workbook, a deck and an email restate.
struct DocumentFacts {
	std::vector<document_intelligence::Statement> statements;
	document_intelligence::FactDocument places;
	std::string problem;
};
DocumentFacts factsOfDocument(const std::filesystem::path& path, const std::string& documentId, const std::string& name);
// The same of an agreement already read.
DocumentFacts factsOfAgreement(const document_intelligence::LegalDocument& document, const std::string& documentId,
	const std::string& name);

// One assignment's readings, so each document is read once however many parts
// check against it: by the file's SHA-256, so a draft written meanwhile is read
// as it is.
class FactsReading {
public:
	const DocumentFacts& of(const std::filesystem::path& path, const std::string& documentId, const std::string& name);

private:
	std::mutex mutex_;
	std::map<std::string, DocumentFacts> read_;
};

// What a part is checked against: every document of the Matter there now -
// never the one worked on or another version of it, nor a colleague's draft
// nobody accepted; a draft this assignment made, or an accepted one, stands for
// its original.
struct MatterFacts {
	std::vector<document_intelligence::Statement> sources;
	std::vector<document_intelligence::FactDocument> documents;
};
MatterFacts matterFacts(const matters::Matter& matter, const std::string& workedOnDocumentId,
	const std::vector<std::string>& assignmentDraftIds, FactsReading& reading);

// One document checked against them: each value, and each clause it cites.
struct FactsCheck {
	std::vector<document_intelligence::FactCheck> checks;
	std::size_t agreeing = 0;
	std::size_t differing = 0;
	std::size_t unclear = 0;
	std::size_t unsourced = 0;
};
FactsCheck checkAgainstMatter(const std::vector<document_intelligence::Statement>& statements, const MatterFacts& facts);

// What disagrees, the clearest first, at most `most`, one line each: what a
// part is told before it works, and what remains after.
std::vector<const document_intelligence::FactCheck*> differences(const FactsCheck& check, std::size_t most);
std::string differencesText(const FactsCheck& check, std::size_t most);

// The check as a step's record keeps it: its counts, and each value that
// differs or is unclear with its evidence.
nlohmann::json factsCheckToJson(const FactsCheck& check);

// Where each value a change writes comes from, as a record keeps it: sourced
// with the document and its words, or unsourced.
nlohmann::json originsToJson(const std::vector<document_intelligence::ValueOrigin>& origins);

} // namespace singularity::document_review
