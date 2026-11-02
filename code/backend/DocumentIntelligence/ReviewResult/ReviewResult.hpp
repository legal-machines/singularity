#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "DocumentIntelligence/ContextPreparation/ContextPreparation.hpp"
#include "DocumentIntelligence/Types/LegalDocument.hpp"

namespace singularity::document_intelligence {

// How far a place a model named is borne out by the source. A model's words
// are its assertion; only the source's words are evidence.
enum class SourceCheck {
	// The words stand, once, in the block the model named.
	Verified,
	// The words stand, once, in the source - in another block than the one it
	// named. The place is corrected and the correction recorded.
	Relocated,
	// The words stand in more than one place: a change cannot be anchored.
	Ambiguous,
	// The words are not the source's.
	NotFound,
	// The model cited a place the source does not have.
	UnknownPlace,
	// Nothing to check.
	NotApplicable,
};

const char* toString(SourceCheck check);

// One located edit of a rewritten clause: words the document holds once, and
// the words they become. Words around the edit are only its anchor - the
// writer strikes and inserts only the words that differ.
struct TextEdit {
	std::string currentText;
	std::string proposedText;
};

// One change a table's rewrite makes, named by the rows of the table as it was
// read (LegalBlock::rows), each written on its own as a tracked change:
// - "cell": the words of the cell at `row` and `column` become
//   `proposedText`;
// - "add_row": a new row with `cells` stands above the row read as `row`, or
//   at the table's end where `row` is the number of rows read;
// - "remove_row": the row read as `row` is struck.
struct TableEdit {
	std::string kind;
	std::size_t row = 0;
	std::size_t column = 0;
	std::string currentText;
	std::string proposedText;
	std::vector<std::string> cells;
};

// A rewritten clause as a redline reads: its words in order, each kept,
// struck or inserted.
struct RedlinePiece {
	std::string text;
	// "kept", "struck" or "inserted".
	std::string kind;
};

struct ProposedChange {
	std::string id;
	std::string positionId;
	// "rewrite" (the clause as it should read), "replace", "delete",
	// "insert_after" or "table" (a table as it should read).
	std::string action;
	// The place as the model cited it, and as Singularity found it.
	std::string citedClause;
	std::string blockId;
	std::string clauseNumber;
	// The words the change replaces and the words they become. For a rewrite,
	// the whole clause as it reads and as it should read.
	std::string currentText;
	std::string proposedText;
	// A rewrite's edits, each anchored once in the document, written together
	// or not at all; and the redline a lawyer reads.
	std::vector<TextEdit> edits;
	std::vector<RedlinePiece> redline;
	// A table's edits, written together or not at all, and the table's rows as
	// read, by which the writer finds the table as Word holds it.
	std::vector<TableEdit> tableEdits;
	std::vector<TableRowReading> tableRows;
	std::string explanation;
	std::string confidence;
	// The position's own severity, from the Playbook.
	std::string severity;
	SourceCheck check = SourceCheck::NotFound;
	std::string checkNote;
	// Whether it can be written into the document as a tracked change.
	bool isWritable = false;
	// "written", "refused" or "not_written", once a revision is written.
	std::string writeState = "not_written";
	std::string writeNote;
	// How its clause reads in the saved draft, every tracked change accepted,
	// as Word holds it: read back from the file once it is written, never
	// taken from what was proposed (D-102).
	std::string readBack;
};

struct PositionAssessment {
	std::string positionId;
	std::string topic;
	// "conforms", "deviates", "missing", "not_applicable", or "not_assessed"
	// where the model said nothing of the position.
	std::string assessment;
	std::vector<std::string> blockIds;
	std::string explanation;
	SourceCheck check = SourceCheck::NotApplicable;
	std::string checkNote;
};

struct UnresolvedPoint {
	// "missing_document", "unresolved_reference", "needs_lawyer",
	// "embedded_instruction" or "ambiguity".
	std::string kind;
	std::string description;
	std::vector<std::string> blockIds;
	// For "needs_lawyer", the decision as the lawyer takes it (D-120): why it
	// matters, the lawyer's options with the colleague's recommendation first
	// where it has one, and whether accepting or sending the draft waits on it.
	std::string why;
	std::vector<std::string> options;
	bool isRecommended = false;
	bool isBlocking = false;
	// "model", "structure" (Singularity found it in the document itself) or
	// "both".
	std::string foundBy;
	bool isConfirmedByStructure = false;
};

// A comment in the agreement's margin, by the colleague, on one clause (D-117):
// a Word comment in the draft, read by whoever the mark-up goes to - most
// often the other side - so it is written to them, never to the lawyer alone,
// whose points are unresolved ones.
struct MarginComment {
	std::string blockId;
	std::string clauseNumber;
	std::string text;
	// The clause's words as the source reads them, every pending change
	// accepted: how the comment finds its paragraph.
	std::string anchorText;
	// "written", "refused" or "not_written", once a revision is written, and
	// why it was not.
	std::string writeState = "not_written";
	std::string writeNote;
};

struct ValidatedReview {
	bool isWellFormed = false;
	// Why the answer could not be taken at all.
	std::vector<std::string> problems;
	std::string summary;
	std::vector<PositionAssessment> assessments;
	std::vector<ProposedChange> changes;
	std::vector<MarginComment> comments;
	std::vector<UnresolvedPoint> unresolved;
	// What the answer said that could not be taken, and why.
	std::vector<std::string> setAside;
};

// The answer's contract as JSON Schema, for a provider that enforces it.
nlohmann::json reviewAnswerSchema();

// How a clause becomes its rewrite: the clause as it should read in the
// document's own writing (its unchanged words exactly as the document writes
// them), the smallest edits, word by word, that make the one the other - each
// anchored on words `document` holds once - and the redline. No edits when the
// rewrite changes nothing.
struct ClauseRewrite {
	std::string text;
	std::vector<TextEdit> edits;
	std::vector<RedlinePiece> redline;
};
ClauseRewrite rewriteOfClause(const LegalDocument& document, const std::string& clauseText, const std::string& rewrite);

// How a table becomes its rewrite: the table as it should read, a line per row
// and its cells separated by "|", against its rows as read. A row it keeps
// word for word is left as it is; a row it keeps by its first cell is changed
// cell by cell; a row it leaves out is struck, and a row the table did not
// have is added where it stands. No column is added or taken away: a row with
// another number of cells than the table's is not written, and `problem` says
// why. `text` is the table as it will read.
struct TableRewrite {
	std::string text;
	std::vector<TableEdit> edits;
	std::string problem;
};
TableRewrite rewriteOfTable(const std::vector<TableRowReading>& rows, const std::string& rewrite);

// A model's answer, read and checked against the source and the Playbook:
// - an answer that is not the contract's JSON is not taken, and says why;
// - every change's quoted words are found in the source (Platform/PhraseMatch)
//   and its place corrected where the model named the wrong one; a change
//   whose words the source does not hold, or holds twice, is never written;
// - a change or an assessment for a position the Playbook does not have is
//   set aside; a position the model did not assess is said to be unassessed;
// - what the document's own structure shows - an exhibit or a Section it
//   refers to and lacks, words in it addressed to a model - is always among
//   the unresolved points, whether the model saw it or not.
//
// `request` is what the lawyer asked of this review, in their words: a clause
// the other side changed is struck whole only where it asks for that clause
// to go; otherwise a strike of it is their change undone (D-117).
ValidatedReview validateReviewAnswer(const std::string& answer, const LegalDocument& document,
	const std::vector<ReviewPosition>& positions, const std::string& request = std::string());

// The block a cited place names: an id ("s-6.1"), a number ("6.1", "6.1.",
// "Section 6.1"), or "" when the document has no such place.
std::string resolveCitedPlace(const LegalDocument& document, const std::string& cited);

// The words a numbered clause struck whole becomes: it keeps its number, so the
// agreement's numbering and its cross-references stay as they are.
inline const std::string kOmittedClause = "[Intentionally omitted.]";

// A place as a lawyer names it: "Section 5.1", "Exhibit B", "the heading of
// Section 6", "the comment by ..."; "" when the document has no such block.
std::string placeInLawyersWords(const LegalDocument& document, const std::string& blockId);

// Words for the lawyer with every block id in them ("s-6.1", "[h-6]") named
// as the agreement numbers it. Words that only look like an id are kept.
std::string inLawyersWords(const LegalDocument& document, const std::string& text);

nlohmann::json validatedReviewToJson(const ValidatedReview& review);
ValidatedReview validatedReviewFromJson(const nlohmann::json& value);

} // namespace singularity::document_intelligence
