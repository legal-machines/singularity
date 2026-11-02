#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/OfficeTextEdit/OfficeTextEdit.hpp"
#include "Document/Types/DocumentPatchTypes.hpp"
#include "Document/Types/DocumentRenderTypes.hpp"
#include "Document/Types/DocumentSemantics.hpp"

#include <pugixml.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::documents {

// The document as a model reads it, entirely in memory: the body in order, each
// paragraph with the number or bullet Word draws in front of it ("9.2", "(b)"),
// its tables row by row, and then - each labelled - the page header and
// footer (and a first page's or even pages' own), the footnotes and endnotes
// and the comments in the margin, each with the passage it is on.
//
// A tracked change nobody has accepted or rejected is read as what it is: a
// proposal - in the body, a table, a text box, a note alike. Words someone
// inserted read "[inserted by Jane Roe: twelve (12)]", words someone struck
// read "[deleted by Jane Roe: six (6)]", and a document that holds any such
// change opens with one line saying so and by whom. Read any other way, the
// other side's proposed cap is the agreed cap.
//
// Relationships, embedded objects and external resources are never resolved.
[[nodiscard]] DocumentExtractionResult
extract_docx_document(const DocumentExtractionRequest &request);

[[nodiscard]] const char *docx_extractor_version() noexcept;

// The reading Singularity gave models until 25 Sep 2026 ("docx-wordprocessingml/1"),
// produced again exactly as it was, so a passage cited from it is found again:
// the body alone, struck words left out, no numbers Word draws, no header,
// notes or comments - and, as it always did, a text box read in both the copies
// Word keeps of it, a moved passage at both its places and a row struck as a
// revision still read. Never what a change is proven against.
[[nodiscard]] DocumentExtractionResult
extract_docx_body_reading(const DocumentExtractionRequest &request);

[[nodiscard]] const char *docx_body_reading_version() noexcept;

// The body alone, as it reads once every tracked change in it is accepted:
// struck words, rows and moved-away passages gone, inserted and moved-in ones
// kept, each text box read once. What a written change is proven against - the
// words a change put in are in it and the words it took out are not, whoever
// accepts the redline later - read the way the writer (OfficeTextEdit) reads
// the document it changes.
[[nodiscard]] DocumentExtractionResult
extract_docx_accepted_reading(const DocumentExtractionRequest &request);

// What Word draws in front of `paragraph` - "9.2", "(b)", a bullet - where it
// numbers the paragraph automatically, directly or through its style, in the
// package whose styles.xml and numbering.xml are given (either may be empty).
// The number is counted as the reading counts it, over the body the paragraph
// stands in. nullopt where Word draws nothing; "" where it numbers the
// paragraph with a list Singularity cannot read.
// A caller that inserts a paragraph written like another needs to know this:
// a number typed into a paragraph Word already numbers is printed twice
// (OfficeTextEdit's AutomaticNumbering).
[[nodiscard]] std::function<std::optional<std::string>(const pugi::xml_node &)> docx_automatic_numbering(
    const std::string &stylesXml, const std::string &numberingXml);

// Everything a change written into a paragraph of this package needs to know of
// its styles and numbering, given to the writer (OfficeTextEdit's
// TextEditRequest): what Word draws in front of each paragraph
// (docx_automatic_numbering), the list each numbered one stands in level by
// level, and which paragraphs are headings - on the paragraph, or through its
// style or the style that one is based on.
void read_docx_numbering_into(TextEditRequest &request, const std::string &stylesXml,
                              const std::string &numberingXml);

// Every paragraph of `body` Word numbers, in reading order, with what it draws
// in front of it - counted in one pass, as the reading counts. Comparing it
// before and after a change says whether the change renumbers anything.
[[nodiscard]] std::vector<std::pair<pugi::xml_node, std::string>> docx_drawn_numbers(
    const pugi::xml_node &body, const std::string &stylesXml, const std::string &numberingXml);

// Structural read of every WordprocessingML table (word/document.xml's
// w:tbl elements, in document order) as a DocumentTable: first row as
// headers, row_key from derive_table_row_key() on each row's first cell.
// table_id is "table-<n>" by document order (n starting at 0) - the one
// physical detail a locator's table_id may rely on, since DOCX has no
// native per-table identity attribute.
[[nodiscard]] std::vector<DocumentTable> extract_docx_tables(const DocumentExtractionRequest &request);

// Every paragraph of the body, in document order, as a reader sees them: the
// text they hold, the heading level where they are a heading, and the section
// they stand in (`semantic_path` - the nearest heading above them, or "Body"
// before the first one). Paragraphs inside table cells are not here; a cell is
// addressed as a cell.
//
// This is what a locator resolves a passage against, and what a model reasons
// over when it decides which part of a document to change. Without it the
// structural view of a Word document holds only its headings and its tables,
// and a change anchored to an ordinary sentence cannot be validated at all.
[[nodiscard]] std::vector<DocumentParagraph> extract_docx_paragraphs(const DocumentExtractionRequest &request);

// The document as its author laid it out, for a person to look at: the body's
// paragraphs and tables in document order, with the emphasis, the alignment,
// the lists and the cell shading the document itself declares. See
// Document/Types/DocumentRenderTypes.hpp - a reading only, never written back
// and never resolved against.
[[nodiscard]] DocumentRenderModel render_docx_document(const DocumentExtractionRequest &request);

struct DocxPatchApplyResult {
  bool ok = false;
  DocumentPatchBlocker blocker = DocumentPatchBlocker::None;
  std::string detail;
};

// A change to table cells alone (phase-24-1-research-document-intelligence-
// project-actions.md, "Overwrite semantics"): the register a firm keeps, its
// finding written into the cell the model named. Re-reads the live file,
// resolves every operation's locator against the current word/document.xml
// exactly as extract_docx_tables() names rows and columns, and re-checks
// expected_old_value itself - the physical writer never trusts the caller's
// validation alone. Each cell is then written as a tracked change by the
// patch's revision_author: its old words struck beside the new ones, a new
// background recorded as a change to the cell's look - the same tracked writer
// every other Word change goes through, so a partner accepts or rejects it in
// Word. Nothing outside the named cells is touched. Writes to a temporary
// package first, reopens it, and only then atomically replaces `documentPath`;
// when any operation fails to resolve, or the staged package fails its checks,
// the real file is never touched at all.
// `verify_staged`, when given, is asked about the fully written staged package
// before it replaces `documentPath` (e.g. that exactly the patched cells now
// hold their new values); a false answer leaves the document untouched.
[[nodiscard]] DocxPatchApplyResult apply_docx_table_cell_patch(
    const std::filesystem::path &documentPath, const DocumentPatch &patch,
    const std::function<bool(const std::filesystem::path &)> &verify_staged = {});

// Every other kind of change a lawyer makes to a Word document: a passage
// replaced or deleted, a paragraph inserted before or after a passage, a row
// added to or removed from a table. Same discipline as the cell writer above -
// the live file is re-read, every operation resolved against it, nothing
// touched until all of them resolve, then a staged package is written, reopened,
// structurally validated, checked by `verify_staged` and only then put in place
// atomically. The passage each operation names is found by what it SAYS
// (Platform/PhraseMatch), and the change is written through the document's own
// runs, so formatting outside the change is never rewritten.
[[nodiscard]] DocxPatchApplyResult apply_docx_document_patch(
    const std::filesystem::path &documentPath, const DocumentPatch &patch,
    const std::function<bool(const std::filesystem::path &)> &verify_staged = {},
    const std::vector<std::vector<std::string>> &equivalent_writings = {});

} // namespace singularity::documents
