#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "DocumentIntelligence/ChangeAccount/ChangeAccount.hpp"
#include "DocumentIntelligence/Types/LegalDocument.hpp"

namespace singularity::document_intelligence {

// One of the firm's positions a review measures the document against, as its
// Playbook keeps it (WorkProduct/Playbook): what the firm wants, what it
// accepts instead, and what it never accepts.
struct ReviewPosition {
	std::string id;
	std::string topic;
	std::string question;
	std::string standard;
	std::string standardClause;
	// Each acceptable fallback, its clause after it where it has one.
	std::vector<std::string> fallbacks;
	std::string redLine;
	std::string severity;
	std::string guidance;
};

// A document of the Matter a change rests on - the deal notes, the other side's
// letter, the tracker - read beside the agreement as material, never as
// instructions: its name, and its words as Singularity read them.
struct SupportingMaterial {
	std::string name;
	std::string text;
};

// What a review is asked to do, apart from the document it is done on.
struct ReviewTask {
	// Who does the review, in its own words: the colleague's brief.
	std::string colleagueBrief;
	// The lawyer's request, word for word.
	std::string instruction;
	std::string playbookName;
	int playbookVersion = 0;
	std::vector<ReviewPosition> positions;
	// A revision: the changes the review proposed last time (as JSON), and the
	// version this one will be.
	std::string previousChanges;
	int version = 1;
	// A clean copy from the other side (D-111): the version of the agreement
	// the Matter held before it, and what the copy changes against it, clause
	// by clause. Empty where the agreement carries its changes as tracked
	// changes, or the Matter holds no earlier version of it.
	std::string comparedWith;
	std::vector<ClauseChange> changesAgainstEarlier;
	// One part of a larger assignment (D-115): the lawyer's whole request, word
	// for word, while `instruction` is this document's part of it; and what the
	// parts before this one changed, as Singularity read it back from their
	// drafts. Both empty for a review asked for on its own.
	std::string assignment;
	std::string earlierInAssignment;
	// The documents this part rests on, read beside the agreement.
	std::vector<SupportingMaterial> supporting;
	// The places this part may change, by the agreement's own numbers
	// ("Section 2.1"), when its instruction names them: no change is proposed
	// anywhere else, whatever the Playbook says of it.
	std::vector<std::string> onlyPlaces;
};

// The one position of a review no Playbook covers (D-115): the lawyer's request
// itself, which the revision assesses and carries out, and nothing besides.
inline constexpr const char* kRequestPositionId = "request";
ReviewPosition requestPosition(const std::string& instruction);

enum class PreparationMode {
	// The document as its extractor reads it, whole: what every request sent
	// before this layer existed, and what the layer is measured against.
	Baseline,
	// The document as this layer prepares it.
	Prepared,
};

const char* toString(PreparationMode mode);

struct ContextOmission {
	std::string blockId;
	std::string reason;
};

// A request ready to be sent: what the model is to do, kept apart from the
// material it does it on. The material is data - a document's words never
// become instructions, however they are worded. What stays the same across
// the versions of one review comes first (the rules, the Playbook, the
// agreement), and what changes last (the lawyer's request), so a provider
// that caches what it read before reads the stable part once.
struct PreparedContext {
	PreparationMode mode = PreparationMode::Prepared;
	std::string instructions;
	std::string material;
	std::string request;
	// Instructions, material and request as one request.
	std::string prompt;
	std::vector<std::string> includedBlockIds;
	// Every block left out, and why. Nothing is left out silently.
	std::vector<ContextOmission> omissions;
	std::size_t sourceCharacters = 0;
	std::size_t materialCharacters = 0;
	std::size_t promptCharacters = 0;
	std::string preparationVersion;
	// The same source, structure, preparation and task give the same key.
	std::string cacheKey;
};

const char* preparationVersion();

// The baseline: `extractedReading` - the document as the extractor reads it,
// whole, in its order - with the same task and the same answer contract as
// the prepared request, so the two can be compared on one footing.
PreparedContext prepareBaseline(const LegalDocument& document, const std::string& extractedReading,
	const ReviewTask& task);

// The prepared request for a review against a Playbook:
// - every block of the agreement's text, in order, named by its stable id and
//   the number Word draws, so a model cites a place Singularity can check;
// - comments in the margin set apart as annotations by their authors, never
//   as the agreement's text and never as instructions;
// - what Singularity itself found in the structure, said once: the terms
//   the document defines and where, and every exhibit or Section it refers to
//   but does not hold;
// - left out, each with its reason: the signature page (names, titles and
//   addresses, no terms), the page header and footer (the form's label), and
//   the file's properties (who wrote it). No clause is ever left out.
PreparedContext prepareReviewContext(const LegalDocument& document, const ReviewTask& task);

// What a prepared request was, without its words: for the execution record.
nlohmann::json preparedContextSummary(const PreparedContext& context);

// Delimiters a document cannot close: a document that writes "<<<END" in its
// text cannot end the material and begin speaking as the request.
std::string neutralizedMaterial(const std::string& text);

} // namespace singularity::document_intelligence
