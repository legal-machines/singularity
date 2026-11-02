#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/SheetRenderTypes.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// A document as its own author laid it out, for a lawyer to LOOK at.
//
// Everything else in Document/ reads a document in order to reason about it:
// flattened text for a model, a semantic model for a locator, a patch for a
// change. None of them can be shown to a person - a lawyer who is told a
// document was changed and is then handed a wall of plain text has not been
// shown their document, they have been shown a transcript of it.
//
// This is the reading that keeps the layout: the body's blocks in document
// order, each paragraph's runs with the marks the document itself puts on
// them, each table's rows and cells with the widths and shading the document
// itself declares. It is a READING and nothing else - nothing here is ever
// written back, and a locator never resolves against it. What a change is
// validated against stays DocumentSemanticModel.
//
// Nothing is invented. A mark that a document does not declare is simply
// absent, and a paragraph whose look comes from a style Singularity did not
// resolve is shown as an ordinary paragraph rather than guessed at.

// Whether this piece of the document is what it was before the last change Singularity
// made to it. Nothing here is a guess about intent: a paragraph is Added
// when the previous version had no such paragraph, Changed when it stood in the
// same place saying something else, and Unchanged otherwise. Unknown is what
// every piece carries when there is nothing to compare against - a document Singularity
// has never changed, or one whose kept version is gone.
enum class DocumentRenderChangeMark { Unknown, Unchanged, Added, Changed };

// A tracked change the document itself records: words someone inserted or
// struck while revisions were being tracked, by whom and when, exactly as Word
// keeps them (w:ins, w:del, and a move's two halves, w:moveTo and w:moveFrom).
// This is the file's own redline - not Singularity's comparison with a version it
// kept - and a lawyer accepts or rejects it in Word.
//
// Formatted is the third kind Word records: the same words, written
// differently - made bold, set in another size, centred, given another style
// (w:rPrChange on a run, w:pPrChange on a paragraph, w:tcPrChange on the
// cell they stand in - a cell given another background). Nothing is added or
// taken out, and rejecting it puts the old look back.
enum class DocumentRenderRevisionKind { Inserted, Deleted, Formatted };

// How a run was written before a formatting change, as far as a reader draws
// it: the marks of DocumentRenderRun below, resolved through the run's own
// character style and its paragraph's style exactly as the run's marks now
// are. What the page is shown "as it was", every change rejected, is drawn
// with these.
struct DocumentRenderRunMarks {
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikethrough = false;
	bool caps = false;
	bool small_caps = false;
	// "superscript", "subscript" or "".
	std::string vert_align;
};

struct DocumentRenderRevision {
	DocumentRenderRevisionKind kind = DocumentRenderRevisionKind::Inserted;
	// The revision's own w:id, as the document numbers it.
	std::string id;
	std::string author;
	// As the document writes it ("2026-09-21T04:30:00Z"); "" where it does not.
	// Word has long written its local time here with a "Z" after it, so this is
	// a reading of a clock, not a moment.
	std::string date;
	// Word's own UTC stamp beside it (w16du:dateUtc), which is a moment; ""
	// where the document does not carry one.
	std::string date_utc;
	// For a formatting change: what it did, in the words a lawyer is shown
	// everywhere else a change is described ("bold", "not italic", "14 pt",
	// "aligned center"). Empty for an insertion or a deletion.
	std::vector<std::string> changes;
	// For a run's formatting change (w:rPrChange): its marks as they stood.
	std::optional<DocumentRenderRunMarks> was_run;
	// For a paragraph's (w:pPrChange): its alignment as it stood, "" where
	// the reader's own default applied. Absent for every other revision,
	// and for a cell's (w:tcPrChange), which moves none of its words.
	std::optional<std::string> was_alignment;
};

// One stretch of text that is written the same way throughout.
struct DocumentRenderRun {
	std::string text;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikethrough = false;
	// "RRGGBB" as the document writes it, "" where the document says nothing
	// and the reader's own text colour applies. "auto" is left out: it means
	// exactly "whatever the reader uses".
	std::string color_hex;
	// The Word highlighter's own colour name ("yellow", "cyan", ...), "" where
	// the run is not highlighted.
	std::string highlight;
	// Half-points, as OOXML counts them (24 = 12 pt). 0 where the size comes
	// from the style rather than from the run.
	unsigned int half_points = 0;
	// A footnote or endnote reference standing at this point in the text.
	// The mark only: what the note SAYS is not part of the body - it is in
	// DocumentRenderModel::notes, under `note_id`, and `note_number` is the
	// number Word draws for it (1, 2, 3 ... in the order the references
	// stand). "" and 0 on every other run.
	bool is_note_reference = false;
	std::string note_id;
	unsigned int note_number = 0;
	// Inserted or struck in one of the document's own tracked changes; absent
	// for text that simply stands. Struck text is kept here, read from
	// w:delText, so a reader can be shown what was taken out; it is never
	// part of what the paragraph SAYS (see DocumentRenderParagraph::text()).
	std::optional<DocumentRenderRevision> revision;
	// The run written differently in a tracked change (w:rPrChange): the marks
	// above are how it is written now. Absent where its look is its own.
	std::optional<DocumentRenderRevision> format_revision;
	// "superscript", "subscript" or "" for text on the line.
	std::string vert_align;
	// Set in capitals, or in small capitals, by the document's formatting
	// rather than by the letters typed. (A double strike reads as
	// `strikethrough`.)
	bool caps = false;
	bool small_caps = false;
	// The comments whose marked range covers this stretch, by their ids in
	// DocumentRenderModel::comments. A comment anchored at a single point is
	// carried by an empty run standing at that point.
	std::vector<std::string> comment_ids;
	// Where a hyperlink around this text leads, when it leads outside the
	// document. "" for ordinary text and for a link to a place inside the
	// document itself.
	std::string link;
	// A page break the author put here (w:br w:type="page"): a run of its own,
	// with no text. A line break stays "\n" and a tab "\t" inside the text.
	bool page_break = false;

	[[nodiscard]] bool is_deleted() const {
		return revision.has_value() && revision->kind == DocumentRenderRevisionKind::Deleted;
	}
};

struct DocumentRenderParagraph {
	// 1..9 for a heading, nothing for ordinary text. Derived exactly as the
	// structural reader derives it (outline level, directly or through the
	// style chain) - never from how big the text is.
	std::optional<unsigned int> heading_level;
	// The style the paragraph names ("Heading1", "ListParagraph", ...), "" when
	// it names none. Shown as evidence, never interpreted.
	std::string style_id;
	// "left", "center", "right", "both" (justified) - "" where the document
	// does not say and the reader's own default applies.
	std::string alignment;
	// How deep this paragraph is indented, in whole levels of 720 twips (Word's
	// own default tab). 0 for flush text.
	unsigned int indent_level = 0;
	// The list this paragraph belongs to, when it belongs to one: its numbering
	// id and its level - directly, or through the paragraph's style. `list_bullet`
	// is true where the list's own format is a bullet rather than a number. The
	// document itself stores no computed numbers: `list_marker` below is the
	// count, made the way Word makes it.
	bool in_list = false;
	std::string list_id;
	unsigned int list_level = 0;
	bool list_bullet = false;
	// The number or bullet Word draws in front of this paragraph ("1.",
	// "1.1", "(a)", "(iv)", "ARTICLE II", "•"), counted the way Word counts
	// it from numbering.xml: every level's own start, format and text, a
	// restart the document asks for, deeper levels starting again under a
	// new parent, legal numbering, and a list reached through the paragraph's
	// style. "" for a paragraph that is not in a list.
	std::string list_marker;
	// Where the paragraph stands, in twips: its own indentation, else the
	// list level's, else its style's. A negative first line is a hanging
	// indent. Absent where nothing declares one.
	std::optional<int> indent_left_twips;
	std::optional<int> indent_first_line_twips;
	std::optional<int> indent_right_twips;
	// The space before and after it, in twips: its own, else its style's,
	// else the document's default.
	std::optional<int> spacing_before_twips;
	std::optional<int> spacing_after_twips;
	// Starts a new page; is kept on the page with the paragraph after it.
	bool page_break_before = false;
	bool keep_next = false;
	// Written in the document's Title style, or a style built on it.
	bool is_title = false;
	// The paragraph ITSELF recorded as inserted or deleted in a tracked change
	// (its paragraph mark, or a revision wrapped round the whole paragraph) -
	// what lets a reader take a struck paragraph away entirely rather than
	// leave an empty line where it stood.
	std::optional<DocumentRenderRevision> revision;
	// The paragraph written differently in a tracked change (w:pPrChange):
	// another style, alignment or spacing - or, where the paragraph has no
	// such change of its own, the cell it stands in given another look
	// (w:tcPrChange). Its words are the same.
	std::optional<DocumentRenderRevision> format_revision;
	// What became of this paragraph in the last change, and - for a changed one -
	// what it said before. The previous wording is the document's own, from the
	// version Singularity kept before it wrote; never reconstructed.
	DocumentRenderChangeMark change = DocumentRenderChangeMark::Unknown;
	std::string previous_text;
	std::vector<DocumentRenderRun> runs;

	// What the paragraph SAYS: its text with every tracked insertion accepted
	// and every tracked deletion gone. This is what a change is measured by,
	// so a struck word the document still carries never reads as a word it
	// still says.
	[[nodiscard]] std::string text() const {
		std::string joined;
		for (const DocumentRenderRun &run : runs) {
			if (!run.is_deleted()) joined += run.text;
		}
		return joined;
	}
};

struct DocumentRenderBlock;

struct DocumentRenderCell {
	std::size_t column_index = 0;
	// How many grid columns this one cell covers, and whether it is the
	// continuation of a cell that began in an earlier row. A viewer that
	// ignores either draws a table the document does not have.
	std::size_t grid_span = 1;
	bool is_merge_continuation = false;
	// On the cell that begins a vertical merge: how many rows it covers,
	// itself included. 1 for every other cell.
	std::size_t row_span = 1;
	// "RRGGBB" where the cell declares a background, "" otherwise. A theme
	// fill Singularity cannot resolve to a real colour is left empty rather than
	// approximated.
	std::string shading_fill_hex;
	DocumentRenderChangeMark change = DocumentRenderChangeMark::Unknown;
	std::string previous_text;
	// The cell's own paragraphs, in order - what a change to the cell is
	// measured by.
	std::vector<DocumentRenderParagraph> paragraphs;
	// Everything in the cell, in order, a table inside it included: what a
	// reader is shown.
	std::vector<DocumentRenderBlock> blocks;
};

struct DocumentRenderRow {
	std::size_t row_index = 0;
	// The row Word repeats at the top of each page: a header row, said by the
	// document itself (w:tblHeader), not guessed from bold text.
	bool is_header = false;
	// The row itself inserted or deleted in one of the document's own tracked
	// changes (w:trPr/w:ins, w:trPr/w:del). A deleted row is still shown - it
	// is still in the file until someone accepts the deletion - but it is no
	// longer a row of the table as the table reads.
	std::optional<DocumentRenderRevision> revision;
	// Added for a row the previous version did not have at all.
	DocumentRenderChangeMark change = DocumentRenderChangeMark::Unknown;
	std::vector<DocumentRenderCell> cells;
};

struct DocumentRenderTable {
	// The same identity a locator uses ("table-0", "table-1", ...), so what a
	// lawyer sees and what a change names are the same table. A table inside
	// a cell is counted where it stands, exactly as the locator counts it.
	// "" for a table outside the body (in a header, a note or a comment),
	// which no change can name.
	std::string table_id;
	// Each grid column's declared width in twips, in order; empty where the
	// table declares no grid and the viewer must share the width evenly.
	std::vector<unsigned int> column_widths_twips;
	// Whether the table draws any border at all - declared on the table or
	// by its table style ("Table Grid") - as opposed to a signature block or
	// a layout table laid out with none.
	bool has_borders = false;
	std::vector<DocumentRenderRow> rows;
};

enum class DocumentRenderBlockKind { Paragraph, Table };

struct DocumentRenderBlock {
	DocumentRenderBlockKind kind = DocumentRenderBlockKind::Paragraph;
	DocumentRenderParagraph paragraph;
	DocumentRenderTable table;
};

// The page the document is set on: its last section's page size and margins,
// in twips (1440 to the inch). Word's own page, so a Letter contract reads on
// a Letter page and an A4 one on A4.
struct DocumentRenderPage {
	unsigned int width_twips = 0;
	unsigned int height_twips = 0;
	int margin_top_twips = 0;
	int margin_right_twips = 0;
	int margin_bottom_twips = 0;
	int margin_left_twips = 0;
	// "portrait" or "landscape".
	std::string orientation;
};

enum class DocumentRenderNoteKind { Footnote, Endnote };

// A footnote or an endnote, as it reads at the foot of the page or the end of
// the document. Only notes the body actually refers to are here - a separator
// line is not a note - numbered in the order their references stand.
struct DocumentRenderNote {
	std::string id;
	DocumentRenderNoteKind kind = DocumentRenderNoteKind::Footnote;
	unsigned int number = 0;
	std::vector<DocumentRenderParagraph> paragraphs;
};

// A comment someone left in the margin, with who left it and when.
struct DocumentRenderComment {
	std::string id;
	std::string author;
	std::string initials;
	// As the document writes it; "" where it does not.
	std::string date;
	std::vector<DocumentRenderParagraph> paragraphs;
};

struct DocumentRenderModel {
	DocumentFormat format = DocumentFormat::unknown;
	ExtractionStatus status = ExtractionStatus::unsupported;
	// The body in document order: a paragraph and the table that follows it
	// stand here in the order the author put them, which is the one thing a
	// separate list of paragraphs and a separate list of tables cannot say.
	std::vector<DocumentRenderBlock> blocks;
	// The page it is set on; absent where the document does not say (and for
	// every format that has no pages of its own).
	std::optional<DocumentRenderPage> page;
	// What stands at the top and the foot of every page: the body section's
	// default header and footer, as paragraphs. A field such as a page number
	// reads as the value Word last wrote for it.
	std::vector<DocumentRenderParagraph> header;
	std::vector<DocumentRenderParagraph> footer;
	// Footnotes first, then endnotes, each in the order they are referred to.
	std::vector<DocumentRenderNote> notes;
	// In the order their marks stand in the document; a comment whose mark
	// Singularity did not meet is listed after them rather than left out.
	std::vector<DocumentRenderComment> comments;
	// A spreadsheet - an .xlsx, a .csv, a .tsv - is not a run of paragraphs: it
	// reads as its workbook, sheet by sheet and cell by cell, and `blocks` is
	// then empty. Absent for every other format, and for a workbook Singularity
	// could not read at all (`limitation` says so).
	std::optional<SheetWorkbookRender> workbook;
	// Which version of this document the marks above are against, and what that
	// version held that this one no longer does. 0 means nothing was compared:
	// Singularity has never changed this document, or the version it kept is gone,
	// and every mark is Unknown.
	int compared_to_version = 0;
	std::vector<std::string> removed_texts;
	// Why a document is shown as text only, in the words a lawyer is shown.
	// Empty when the reading is complete.
	std::string limitation;
	std::vector<ExtractionDiagnostic> diagnostics;
};

} // namespace singularity::documents
