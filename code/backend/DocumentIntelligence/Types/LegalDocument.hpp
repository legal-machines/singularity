#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace singularity::document_intelligence {

// The Document Intelligence & Preparation Layer (working name; the owner,
// 2026-10-24): a professional document turned into material a model can
// work on - its structure understood, what a task needs selected, what must
// stay here kept here, and every word a model cites traceable back to the
// place the source holds it. Model-independent: nothing here knows which
// model reads it, or whether one reads it at all.

// Who a source is: the file Singularity read, byte for byte. A source is
// never changed by anything in this layer.
struct SourceIdentity {
	std::string fileName;
	// "docx", "pdf" or "text".
	std::string format;
	// SHA-256 of the file's bytes, as read.
	std::string sha256;
	long long sizeBytes = 0;
	// The Matter's own record of the file, where it has one.
	std::string matterId;
	std::string documentId;
	int documentVersion = 0;
};

enum class BlockKind {
	Title,
	Paragraph,
	// A clause's heading, as Word draws it: "6. Residuals."
	Heading,
	// A numbered clause: "6.1 Notwithstanding anything ..."
	Clause,
	Table,
	Signature,
	// "Exhibit A", where an exhibit or schedule begins.
	ExhibitHeading,
	// What a page header or footer says.
	HeaderFooter,
	// A note someone left in the margin: words about the document, not of it.
	Comment,
	// A footnote or endnote: part of the document's text, set at the foot of a
	// page or at its end, on the block whose words carry its mark.
	Note,
};

const char* toString(BlockKind kind);

// A tracked change in the document nobody has accepted or rejected yet: words
// someone inserted or struck, still only proposed. A block's text reads as it
// would stand with it (inserted words in, struck words out); this says the
// words are another's proposal, not agreed text.
struct PendingChange {
	// "inserted" or "struck".
	std::string kind;
	std::string author;
	std::string text;
	// Struck as a whole paragraph, which stood after the block that keeps this.
	bool isWholeParagraph = false;
};

// One unit of the document a lawyer would point at. Its id stays the same for
// the same clause in every version that numbers it the same, so a model, a
// result and a lawyer all name one place: "s-6.1" is Section 6.1 wherever it
// moved; a paragraph Word does not number is named by its order ("p-3").
// One row of a table as it is read: each cell's words, in the order Word draws
// the row's cells. A cell that a vertical merge continues from the row above
// has no words of its own and is marked: it is changed in the row above.
struct TableRowReading {
	std::vector<std::string> cells;
	std::vector<bool> isContinued;
};

struct LegalBlock {
	std::string id;
	BlockKind kind = BlockKind::Paragraph;
	// The number Word draws in front of it, without its final stop: "6.1",
	// "6"; "" where it draws none.
	std::string number;
	// The heading of the clause it stands under: "Residuals".
	std::string heading;
	// Its words, without the number Word draws. A table's rows are lines, its
	// cells separated by " | ".
	std::string text;
	// Where a person finds it: "6 Residuals > 6.1".
	std::string path;
	// "body", "exhibit:A", "header", "footer" or "comment".
	std::string section;
	// For a comment, the block it is on.
	std::string annotates;
	// For a comment, who wrote it.
	std::string author;
	// Its place in the reading order, from 0.
	std::size_t order = 0;
	// Which of the body's paragraphs or tables it is, from 0 - its location in
	// the source file. Not set for a header, a footer or a comment.
	std::size_t sourceIndex = 0;
	// SHA-256 of its words with spacing normalized: what tells two versions'
	// readings of the same clause apart.
	std::string fingerprint;
	// The tracked changes in it nobody has accepted or rejected, in order.
	std::vector<PendingChange> pendingChanges;
	// Its words before those changes: struck words in, inserted words out.
	// Kept only for a block that has pending changes.
	std::string originalText;
	// The whole block is a pending insertion: before it, the document did not
	// have it at all.
	bool isPendingInsertion = false;
	// For a table, its rows as read, in order: what its text shows, cell by
	// cell, and what a change to it is written against. A row whose cells
	// hold no words, or one struck as a pending revision, is not among them.
	std::vector<TableRowReading> rows;
};

// A term the document defines: "Confidential Information" means ...
struct DefinedTerm {
	std::string term;
	std::string blockId;
	// The sentence that defines it.
	std::string definition;
	// The blocks that use it, other than the one that defines it.
	std::vector<std::string> usedIn;
};

// A place the document points to: "Section 4", "Exhibit B".
struct CrossReference {
	std::string label;
	// "section" or "exhibit".
	std::string kind;
	std::string fromBlockId;
	// The block it points to, where the document has it.
	std::string targetBlockId;
	bool isResolved = false;
};

// An exhibit or schedule the document refers to, and whether it holds it.
struct ExhibitStatus {
	std::string label;
	bool isPresent = false;
	// Its title, where the document gives one.
	std::string title;
	std::vector<std::string> referencedIn;
};

// What the file says about itself outside its text: who wrote it, for whom.
struct DocumentProperty {
	std::string name;
	std::string value;
};

// Words in the document that speak to a model ("AI review tools should ..."):
// part of the document, never an instruction to anyone reading it for us.
struct EmbeddedInstruction {
	std::string blockId;
	std::string text;
};

// An entity a signature block names, and whether the preamble names it as a
// party: a signature page that binds an entity the agreement does not name is
// a finding of the document itself.
struct SigningEntity {
	std::string name;
	std::string blockId;
	bool isNamedInPreamble = false;
};

// One part of the Word package and what was done with it: "read" (its words
// are in the blocks), "used" (it shaped the reading - numbering, styles -
// without words of its own), "not_needed" (formatting only), or "not_read"
// (it may hold something the reading does not understand: a picture, an
// embedded object, custom XML).
struct SourcePart {
	std::string name;
	std::string state;
	std::string note;
};

struct LegalDocument {
	SourceIdentity source;
	std::string title;
	std::vector<LegalBlock> blocks;
	std::vector<DefinedTerm> definitions;
	std::vector<CrossReference> references;
	std::vector<ExhibitStatus> exhibits;
	std::vector<DocumentProperty> properties;
	std::vector<EmbeddedInstruction> embeddedInstructions;
	// What could not be read, or was read with a reservation, in words.
	std::vector<std::string> limitations;
	// How many tracked changes the source holds, not yet accepted or rejected.
	std::size_t pendingRevisions = 0;
	// Who signs, from the signature blocks.
	std::vector<SigningEntity> signatories;
	// The source manifest: every part of the package and what the reading did
	// with it; the addresses outside the file it links to; and content in it
	// the reading saw but could not read (pictures, embedded objects), where.
	std::vector<SourcePart> parts;
	std::vector<std::string> externalTargets;
	std::vector<std::string> unreadContent;
	std::string structureVersion;
};

// What a change to one block touches: the blocks that refer to it, use a term
// it defines, or annotate it - read from the document, never inferred.
std::vector<std::string> dependentBlocksOf(const LegalDocument& document, const std::string& blockId);

} // namespace singularity::document_intelligence
