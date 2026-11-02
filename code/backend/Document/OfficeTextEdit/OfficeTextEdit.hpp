#pragma once

#include "Document/Types/DocumentPatchTypes.hpp"

#include <pugixml.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// Editing a Word document the way a lawyer edits one: by naming the passage to
// change and saying what it becomes.
//
// Everything here works on the text a person reads - the paragraph's own words,
// across however many formatting runs Word happened to split them into - and
// writes back through those same runs. A passage set in bold that is left alone
// stays bold; a sentence replaced inside a paragraph keeps the paragraph, its
// style, its numbering and every other run around it. Nothing outside the
// change is rewritten, so a document that comes back from Singularity is the firm's
// document with one passage different, not a document Singularity regenerated.
//
// The passage is found with context::locate_phrase, the same matcher that
// decides whether a quotation stands in a document. A model never sees the
// document's own bytes - it sees protected text, and its answer is restored
// afterwards - so an anchor that differs in a space, a quotation mark, a dash
// or the case of a word is the same anchor. The WORDS must be the document's
// own, and the change is always applied to the document's bytes, never to the
// anchor as the model wrote it.
//
// An anchor that stands in more than one place is refused, not guessed at.
enum class TextEditKind {
	// The anchor's own text becomes `text`. Where the anchor runs across
	// paragraphs, or `text` is several lines, the change is to clauses and is
	// written clause for clause: each line a paragraph of its own, set against
	// the old paragraph it rewrites; a line with none to rewrite a new paragraph
	// written like the clauses around it, at the level of the list its typed
	// number has the form of; an old paragraph no line rewrites removed whole,
	// its paragraph mark with it. A number typed in the form Word draws is left
	// to Word, and no empty paragraph is left behind.
	Replace,
	// The anchor's own text goes, and a paragraph left empty by it goes too.
	Delete,
	// `text` becomes new paragraph(s) before/after the anchor's paragraph,
	// written in that paragraph's own style.
	InsertParagraphBefore,
	InsertParagraphAfter,
	// `text` becomes new paragraph(s) at the end of the document, written like
	// the last ordinary paragraph there. No anchor: a lawyer adding a closing
	// note names no passage.
	AppendParagraph,
	// The anchor's own text is written differently - bold, another size, a
	// colour, a style, an alignment - and says exactly the same thing. Only
	// what `formatting` names is changed; everything else about the run and
	// the paragraph is left as the document has it.
	Format,
};

// Who a change is by, and when.
//
// A change Singularity writes into a firm's document is not a silent rewrite. It
// is a revision, recorded the way Word records one: the old words are kept and
// struck, the new words stand beside them, both carry an author and a time,
// and a partner opens the file and accepts or rejects each one with Word's own
// buttons. That is how a draft moves between a lawyer, a partner and the other
// side; a document that simply came back different is a document nobody can
// approve.
//
// An empty author writes the change directly, with no revision marks, which is
// what a lawyer asks for when they say "just make it so".
struct TrackedChange {
	std::string author;
	// The moment in UTC, as OOXML writes one: "2026-09-21T04:30:00Z". It is
	// written as w:date and as Word's own UTC stamp beside it (w16du:dateUtc).
	std::string when;

	[[nodiscard]] bool records_revision() const { return !author.empty(); }
};

// The contract's own formatting type, so what a model may ask for and what
// this engine can write are one definition (Document/Types/DocumentPatchTypes).
using TextFormatting = DocumentTextFormatting;

// What Word draws in front of a paragraph it numbers automatically - "9.2",
// "(b)", "Section 4.01", "•" - directly or through the paragraph's style;
// nullopt where it draws nothing, and "" where it numbers the paragraph but
// what it draws could not be worked out (DocxExtractor's
// docx_automatic_numbering, which reads the package's styles and numbering).
using AutomaticNumbering = std::function<std::optional<std::string>(const pugi::xml_node &)>;

// The list a paragraph Word numbers stands in, as a paragraph written beside it
// needs to know it: the list (its w:numId), the level the paragraph stands at,
// what Word draws for the first item of each level of that list ("1.", "1.1",
// "(a)"; nullopt for a level the list does not define), and how far from the
// margin the words of the paragraph's own level begin, where the list says.
struct ListPlace {
	std::string list_id;
	std::size_t level = 0;
	std::vector<std::optional<std::string>> first_drawn;
	std::optional<int> text_indent_twips;
};
using ListPlaceOf = std::function<std::optional<ListPlace>(const pugi::xml_node &)>;

// Whether a paragraph is a heading as the document's own styles say it: on the
// paragraph, or through the style it is written in or one that style is based
// on ("Clause Heading", outline level 1).
using HeadingOf = std::function<bool(const pugi::xml_node &)>;

struct TextEditRequest {
	TextEditKind kind = TextEditKind::Replace;
	std::string anchor;
	std::string text;
	// For Format only.
	TextFormatting formatting;
	// Who the revision belongs to. Left empty, the change is written straight
	// into the text.
	TrackedChange tracked;
	// What Word draws in front of a paragraph. A paragraph inserted like one
	// Word numbers is numbered by Word, so a number the model typed at its head
	// in the same form is not written: "9.3 9.3 Notices" is what a lawyer would
	// otherwise find. Left empty, only numbering the paragraph names itself is
	// known, and only a number in brackets - "(c)" - is left to Word.
	AutomaticNumbering numbered_automatically;
	// The list a numbered paragraph stands in, level by level. A line typed as
	// a sub-item - "(a) flood;" under a clause Word numbers "1.3" - is written
	// at the list's own next level, where Word draws "(a)" itself; one that no
	// level of the list draws is written as a plain paragraph with its typed
	// mark, never as another clause ("1.4 (a) flood;"). Left empty, such a line
	// is written as a plain paragraph.
	ListPlaceOf list_place;
	// Which paragraphs are headings. A paragraph inserted under a heading is
	// written like the body text after it, never as another heading. Left
	// empty, only a heading style's name or an outline level on the paragraph
	// itself says so.
	HeadingOf is_heading;
};

struct TextEditOutcome {
	bool ok = false;
	// How many places the anchor stands at: 0 when the passage is not in the
	// document, more than 1 when it is not unique. Both refuse the edit.
	std::size_t matches = 0;
	// The document's own wording at the anchor, byte for byte - what was
	// actually changed, for the record a lawyer reviews.
	std::string anchor_exact;
	// Why, in the words a lawyer is shown, when the edit was refused.
	std::string reason;
	// What the change wrote, as the document now holds it: a replacement's new
	// words, or each added paragraph's words - a heading with its hashes, bold
	// and italic words in the marks a model writes them in - and never a number
	// Word draws in front of it. What a lawyer is shown of a change is this.
	std::vector<std::string> written;
};

// ---- What a model writes, as a document is written ------------------------
//
// A model writes prose with the light marks it writes everywhere else: two
// stars for bold, one for italic, a hash for a heading. Those are characters.
// A Word document that is handed them shows them as characters, and a firm's
// draft with "**Indemnity Cap**" in it is not a draft - it is the machine's
// notes pasted into a deed. So the marks are read here and become the
// document's own formatting, and a mark a lawyer never typed never reaches
// the page.
//
// Conservative on purpose, because a legal document is full of characters
// that look like marks: "[*]", a defined term written Section_2, an asterisked
// footnote, a line of dashes. A mark is a mark only when it closes on the same
// line, holds something that is not a space, and - for "_" - stands outside a
// word.
struct ProseRun {
	std::string text;
	bool bold = false;
	bool italic = false;
};

struct ProseParagraph {
	std::vector<ProseRun> runs;
	// 0 for an ordinary paragraph; 1-6 for a heading the model marked with
	// that many hashes. A heading is written like the document's own headings
	// where it has any, and in bold where it has none: never as "## Words".
	int heading_level = 0;
};

// The model's text as paragraphs a word processor can write, one per line.
[[nodiscard]] std::vector<ProseParagraph> prose_paragraphs(const std::string &text);

// The same text with its marks taken off and nothing put in their place, for
// a passage replaced inside a sentence: it takes the sentence's own
// formatting, and a star in the middle of a clause is not a formatting
// instruction anybody meant.
[[nodiscard]] std::string prose_plain_text(const std::string &text);

// ---- A new Word document, as a model wrote it ------------------------------
//
// What a model writes as a list or a table becomes one in a new Word document:
// a numbered clause is a paragraph Word numbers, so a lawyer who adds a clause
// in Word does not renumber the rest by hand, and a schedule is a table, not
// lines with bars in them. A list is made only where the numbers typed are the
// ones Word would count; one that is not is written as the lines that were
// typed, since a list that renumbers what the drafter wrote says something
// else. The card's picture of a new document (DocumentChange.jsx, WordCreated)
// draws the same blocks.

// One level of a list: "bullet", "decimal", "lowerLetter" or "lowerRoman"; its
// number punctuated as typed ("%." for "1.", "%)" for "a)", "(%)" for "(iv)");
// and the number it starts at.
struct NewWordListLevel {
	std::string kind;
	std::string pattern;
	int start = 1;
};

struct NewWordBlock {
	enum class Kind { Paragraph, ListItem, Table };
	Kind kind = Kind::Paragraph;
	ProseParagraph paragraph;
	// A blank line stood before it: a little more space above.
	bool spaced = false;
	// A list item: which list (each run of items is a list of its own) and how
	// deep it stands in it.
	std::size_t list = 0;
	std::size_t level = 0;
	// A table: its rows, every one as wide as the widest, the first a heading
	// row where `heading`.
	std::vector<std::vector<std::string>> rows;
	bool heading = false;
};

struct NewWordDocument {
	std::vector<NewWordBlock> blocks;
	// Each list's levels, in the order the lists stand.
	std::vector<std::vector<NewWordListLevel>> lists;
};

// The model's text as a new document's blocks: paragraphs and headings
// (prose_paragraphs), lists typed as "- ", "1.", "(a)" or "(iv)" items, and
// tables typed as "| cell | cell |" lines. `rows`, with `columns` as their
// heading where given, is a table the model gave beside the text: it stands
// after it.
[[nodiscard]] NewWordDocument new_word_document(const std::string &content,
	const std::vector<std::string> &columns = {}, const std::vector<std::vector<std::string>> &rows = {});

// The document's words as the written file holds them with no numbers drawn:
// each paragraph's words, each list item's without its mark, each row's cells
// separated by tabs - one per line.
[[nodiscard]] std::string new_word_document_words(const NewWordDocument &document);

// The title (in the Title style) and then every block, written into `body`: a
// heading in its heading style and outline level, a list item as a paragraph
// numbered by list `list + 1` of new_word_numbering_part(), a table across
// `usableWidthTwips` in the "TableGrid" style with its heading row repeated on
// every page. A document never ends in a table: Word ends it with a paragraph.
void write_new_word_body(pugi::xml_node body, const std::string &title, const NewWordDocument &document,
	int usableWidthTwips);

// word/numbering.xml for the document's lists, or "" where it has none.
[[nodiscard]] std::string new_word_numbering_part(const NewWordDocument &document);

// ---- A passage quoted out of a model's reading -----------------------------
//
// A model reads a Word document with its pending tracked changes marked
// ("[inserted by Jane Roe: twelve (12)]", "[deleted by Jane Roe: six (6)]"),
// its notes referred to ("[footnote 2]") and each clause with the number Word
// draws for it ("9.2 Limitation of Liability") - DocxExtractor's reading. None
// of that is text the document holds, and a model that quotes it back is still
// naming the document's own words.

// The passage as the document's text holds it: an insertion's words kept, a
// struck passage's words left out, a note reference and the marks around them
// gone. Text with no such mark comes back as it was.
[[nodiscard]] std::string passage_without_reading_marks(const std::string &quoted);

// `line` without a clause number or a bullet typed at its head in the form
// Word draws the paragraph's own (`drawn`, as AutomaticNumbering gives it):
// "9.3 " where Word draws "9.2", "(c) " where it draws "(b)", "- " anywhere. A
// number of another form, or a section it refers to - "Section 7.1 of the
// Agreement", "1.5 million shares", "A. Smith" - is the clause's own words, and
// stays. Where `drawn` is "" (not known) only a number in brackets goes. What
// is written into a numbered clause and what the lawyer is shown of it are the
// same text.
[[nodiscard]] std::string without_typed_clause_number(const std::string &line, const std::string &drawn);

// `line` without whatever at its head could be a clause number or a bullet -
// "9.3 ", "(b) ", "iv. ", "Section 12.1 ", "- " - of any form. Never used to
// decide what is written: only to recognise, in a document already written,
// a clause Word numbers written without the number the model typed.
[[nodiscard]] std::string without_typed_clause_number(const std::string &line);

// The moment a revision is recorded, in UTC: "2026-09-26T06:22:20Z". "Z" means
// UTC, and a stamp that says it must be UTC: a change made at 00:22 in Moscow
// is 21:22Z, and Word, which turns the stamp into the reader's own time, shows
// it at 00:22 there and at 17:22 in New York. Word is also given its own UTC
// stamp (w16du:dateUtc), which the Word of today reads first.
[[nodiscard]] std::string revision_timestamp_now();

// ---- The package's own properties ------------------------------------------
//
// What Word shows under File > Info and Finder under Get Info: the document's
// title, who wrote it, who last changed it and when. A document Singularity writes
// names the lawyer as its author, and a document Singularity changes names them as
// the last person to change it: the name the lawyer's revisions carry
// (AppSettings' tracked_change_author), never nobody.

// docProps/core.xml for a new document: `title`, `author` as the creator and
// the last to modify it, and `when` (UTC, revision_timestamp_now()) as the
// moment it was created and modified. An empty author is left out.
[[nodiscard]] std::string new_core_properties_part(const std::string &title, const std::string &author,
	const std::string &when);

// docProps/app.xml for a new document: the application that wrote it.
[[nodiscard]] std::string new_app_properties_part();

// A document's own docProps/core.xml with who changed it last and when set to
// `modifiedBy` and `when`, and every other property - its author, its title,
// when it was created - exactly as it stood. An empty `modifiedBy` leaves the
// last modifier as it was. Returns "" when the part cannot be read, and the
// caller then leaves it untouched.
[[nodiscard]] std::string core_properties_modified(const std::string &coreXml, const std::string &modifiedBy,
	const std::string &when);

// The body's text as a reader sees it: every paragraph's runs joined, one
// paragraph per line, in document order. Table cells read as paragraphs, in
// the order Word lays them out.
[[nodiscard]] std::string wordprocessing_text(const pugi::xml_document &document);

// Writes `formatting` onto everything inside `node` - a table cell or a
// paragraph: every run in it takes what the formatting names, and the paragraph
// properties it names are set on every paragraph in it. Used where the change
// is to a whole object rather than to a passage inside one ("make that cell
// bold"), so a caller never has to reach into runs itself.
void apply_formatting_within(pugi::xml_node node, const TextFormatting &formatting);
// The same, recorded as a formatting revision when `tracked` records one, so a
// partner sees the change in Word's redline and can reject it. Words already
// struck are not re-formatted, and words this same author's change is inserting
// are simply written that way: they are a revision already.
void apply_formatting_within(pugi::xml_node node, const TextFormatting &formatting, const TrackedChange &tracked);

// How much of a replaced passage stays as it was, at either end, in whole
// words: a redline marks only the words that actually differ. The same rule
// the writer uses, so a preview of a change marks exactly what Word will.
struct RevisionSpan {
	std::size_t shared_prefix = 0;
	std::size_t shared_suffix = 0;
};
[[nodiscard]] RevisionSpan word_revision_span(const std::string &was, const std::string &now);

// The stretches of a paragraph's words that differ, each marked on its own -
// "evaluating~~ and~~, negotiating ++and consummating++", never the whole
// phrase struck and written again. The words both readings keep, in order,
// stay unmarked: the most of `was` both keep, a word weighing its length, a
// word's closing comma or full stop a word of its own. Each stretch is bytes
// [was_from, was_to) of `was` that become [now_from, now_to) of `now`, a space
// both share outside it. Nothing when the passage was mostly rewritten - less
// than half of it kept - or is too long to compare: one mark from the first
// difference to the last (word_revision_span) reads better then.
struct RevisionHunk {
	std::size_t was_from = 0;
	std::size_t was_to = 0;
	std::size_t now_from = 0;
	std::size_t now_to = 0;
};
[[nodiscard]] std::vector<RevisionHunk> word_revision_hunks(const std::string &was, const std::string &now);

// `now` with the document's own typography wherever it says the same as `was`:
// a model writes a clause back with a curly apostrophe for the document's
// straight one, or a different dash, and that is not a change. Only quotation
// marks, apostrophes, dashes and space characters are equated; the words
// themselves must be the same. The result is what is written and compared,
// so word_revision_span marks only the words that really change and the words
// kept stay byte for byte as the document had them.
[[nodiscard]] std::string with_document_typography(const std::string &was, const std::string &now);

// ---- Table changes, as Word records them ----------------------------------
//
// A table edit is a revision like any other: a row Singularity adds is an inserted
// row, a row it removes is a deleted row still in the file, and a cell it
// rewrites keeps its old words struck beside the new ones - so a partner can
// reject any of them in Word. Each call needs `tracked` to record a revision.

// The row, marked as inserted (w:trPr/w:ins) with every run in it inserted.
// The rows of a table inside it are marked inserted with it.
void track_inserted_row(pugi::xml_node row, const TrackedChange &tracked);

// The row, marked as deleted (w:trPr/w:del) with every word in it struck -
// removed only when the revision is accepted. The rows of a table inside it
// are marked deleted with it: Word otherwise never comes to the end of the
// revision when it walks the document's changes.
void track_deleted_row(pugi::xml_node row, const TrackedChange &tracked);

// How a cell's words are recorded when the cell becomes `text` - the writer's
// own rule, so a preview of a cell change marks exactly what Word will. A cell
// of one paragraph that keeps whole words at either end keeps them, and only
// the words between are struck and written; otherwise the whole cell is struck
// and the whole value written. Either way the value is written as the document
// writes it: its own quotation marks and dashes, no marks a model typed.
// `paragraphs` is the text of each of the cell's own paragraphs as it reads now.
struct CellTextRevision {
	std::string kept_before;
	std::string removed;
	std::string added;
	std::string kept_after;
	// The value says, word for word, what the cell says already: nothing is
	// recorded, and the cell reads as kept_before.
	bool unchanged = false;
};
[[nodiscard]] CellTextRevision cell_text_revision(const std::vector<std::string> &paragraphs,
	const std::string &text);

// The cell's words struck and `text` inserted in their place, in the
// formatting of the first words it replaces - by cell_text_revision()'s rule.
void replace_cell_text_tracked(pugi::xml_node cell, const std::string &text, const TrackedChange &tracked);

// The cell's background (w:tcPr/w:shd) set to `fill` ("RRGGBB") and, where
// given, the theme colour and shade it is drawn from - recorded as a change to
// the cell's properties (w:tcPrChange) when `tracked` records revisions, so a
// partner can reject it in Word like any other change. Every other property of
// the cell is left as it stands.
void set_cell_shading(pugi::xml_node cell, const std::string &fill, const std::string &themeFill,
	const std::string &themeFillShade, const TrackedChange &tracked);

// The paragraph, marked as inserted: its runs and its paragraph mark.
void track_inserted_paragraph(pugi::xml_node paragraph, const TrackedChange &tracked);

// `node` as it reads with every revision in it accepted: struck words gone,
// inserted ones kept as ordinary text, no revision marks left. Used on a copy
// of a row before it is written as a new one.
void accept_revisions_within(pugi::xml_node node);

// A row Word shows as deleted but not yet accepted: it is not part of the
// table as a reader reads it.
[[nodiscard]] bool row_is_deleted_revision(const pugi::xml_node &row);

// A copy of a table's heading row (w:tblHeader), made into an ordinary row of
// it: no longer repeated at the top of every page, and without the heading's
// own look - its cells' shading and its bold. A register that has only its
// heading row is given its first entry this way, not a second heading.
// Returns whether `row` was a heading row.
bool make_ordinary_row(pugi::xml_node row);

// A copy of a table's row, emptied for a new entry. Each cell keeps its own
// properties and one paragraph, written as the cell's first words were, with
// one empty run (w:r/w:t) for the entry's words. Nothing else of the row it
// was copied from stays: not its words, not a merge with the row above, and
// nothing that belongs to that row alone or cannot stand twice in a document -
// a table inside a cell, a link, a bookmark, a comment's anchor, a field, a
// content control.
void empty_row_for_entry(pugi::xml_node row);

// Applies one edit to a parsed word/document.xml. The document is left exactly
// as it was when the outcome is not ok.
[[nodiscard]] TextEditOutcome apply_wordprocessing_edit(pugi::xml_document &document,
	const TextEditRequest &request);

} // namespace singularity::documents
