#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/ArtifactCapabilityRegistry/ArtifactCapabilityRegistry.hpp"

#include <utf8proc.h>

#include <cctype>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace singularity::documents {

// Shared row-key derivation used by every table-structured format adapter
// (DOCX, PPTX slide tables, CSV): the first cell's trimmed text when it is
// both non-empty and not already used by an earlier row in the same table,
// otherwise a positional fallback - guarantees DocumentTable::find_row() can
// resolve every row deterministically, at the cost of a locator naming the
// fallback key being tied to physical position instead of real content.
[[nodiscard]] inline std::string derive_table_row_key(const std::string &firstCellText,
		std::size_t rowIndex, std::unordered_set<std::string> &usedKeys) {
	if (!firstCellText.empty() && usedKeys.count(firstCellText) == 0) {
		usedKeys.insert(firstCellText);
		return firstCellText;
	}
	const std::string fallback = "row-" + std::to_string(rowIndex);
	usedKeys.insert(fallback);
	return fallback;
}

namespace detail {
[[nodiscard]] inline std::string trim_ascii_space_for_placeholder_check(const std::string &value) {
	std::size_t first = 0;
	while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
	std::size_t last = value.size();
	while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
	return value.substr(first, last - first);
}
} // namespace detail

// THE single, canonical definition (ConnectedDocumentAction.cpp and
// DocxExtractor.cpp both call this rather than keeping their own copies - a
// real live external-API acceptance run this session proved the two
// independent copies drift-risk was real, not hypothetical, the moment a
// second call site needed the exact same rule). A real live external-API
// acceptance run against a real user-authored risk register proved this
// needed to recognize more than an English "[TBD" marker: that document's
// own genuinely-empty rows (reserved for a new, not-yet-described item) used
// the equally standard Russian/European legal-drafting convention "[●]"
// (and multi-token variants like "(1) [●].(2) [●]:(a) [●];(b) [●]." -
// numbered sub-clauses, each blank still bracketed) - never "[TBD". A cell's
// real, substantive text is never composed ENTIRELY of bracketed spans plus
// ordinary list/sentence punctuation - genuine prose always has real words
// outside every bracket (a citation like "[15]" sits inside a real
// sentence). So: strip every bracketed span, then strip common numbering/
// connector punctuation; if nothing real remains, this is an unfilled
// template blank regardless of what marker convention (TBD, ●, ..., ___)
// the bracket itself used. Never fires on plain text with no bracket at all
// (a real one-word or one-letter answer like "x"/"No" is never mistaken for
// a placeholder - proven by a real regression this same session).
[[nodiscard]] inline bool is_template_placeholder(const std::string &value) {
	const std::string trimmed = detail::trim_ascii_space_for_placeholder_check(value);
	if (trimmed.empty() || trimmed.find("[TBD") != std::string::npos) return true;
	std::string withoutBrackets;
	withoutBrackets.reserve(trimmed.size());
	bool insideBracket = false;
	bool sawBracket = false;
	for (char character : trimmed) {
		if (character == '[') { insideBracket = true; sawBracket = true; continue; }
		if (character == ']') { insideBracket = false; continue; }
		if (!insideBracket) withoutBrackets += character;
	}
	if (!sawBracket || insideBracket) return false;
	const std::string remainder = detail::trim_ascii_space_for_placeholder_check(withoutBrackets);
	if (remainder.empty()) return true; // e.g. "[●]" alone, nothing else in the cell.
	constexpr const char *kListingPunctuation = " \t\r\n.,;:()-\xE2\x80\x93\xE2\x80\x94\xE2\x80\xA2\xC2\xB7";
	for (unsigned char character : remainder)
		if (std::strchr(kListingPunctuation, static_cast<char>(character)) == nullptr
				&& std::isdigit(character) == 0 && std::isalpha(character) == 0)
			return false;
	std::string::size_type runStart = std::string::npos;
	for (std::string::size_type index = 0; index <= remainder.size(); ++index) {
		const bool isAsciiLetter = index < remainder.size()
			&& std::isalpha(static_cast<unsigned char>(remainder[index])) != 0;
		if (isAsciiLetter) {
			if (runStart == std::string::npos) runStart = index;
			continue;
		}
		if (runStart != std::string::npos) {
			if (index - runStart > 2) return false;
			runStart = std::string::npos;
		}
	}
	return true;
}

// THE single, canonical definition (MatterRepository.cpp, ConnectedDocumentAction.cpp
// and DirectDocumentWork.cpp all call this rather than keeping their own
// copies - the same drift risk is_template_placeholder()'s own comment
// documents applies here too). A JSON Schema "maxLength" means RFC 8259
// characters (Unicode code points), and a compliant provider honors it as
// exactly that - a genuinely compliant ~1435-character Russian answer (well
// under a real 1500-character schema ceiling) is ~2628 UTF-8 BYTES once
// Cyrillic's 2-byte encoding is counted, so std::string::size() (raw bytes)
// rejects it as "malformed" though it never violated the limit the model was
// actually told to honor - a real live external-API acceptance run this
// session proved this exact gap. Counts real Unicode code points via
// utf8proc (the one library this codebase already delegates all UTF-8
// mechanics to), never bytes, so every script (Cyrillic, CJK, Arabic, Latin)
// gets the same real ceiling any caller promises.
[[nodiscard]] inline std::size_t utf8_character_count(const std::string &text) {
	std::size_t count = 0;
	utf8proc_ssize_t offset = 0;
	const auto *bytes = reinterpret_cast<const utf8proc_uint8_t *>(text.data());
	const auto length = static_cast<utf8proc_ssize_t>(text.size());
	while (offset < length) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, length - offset, &codePoint);
		if (consumed <= 0) { ++offset; ++count; continue; } // a malformed byte still counts as one unit.
		offset += consumed;
		++count;
	}
	return count;
}

// The reusable, format-neutral document semantic layer. This is the ONE
// structural representation every format adapter (DocxExtractor,
// PptxExtractor, XlsxExtractor, the CSV/plain-text readers in
// DocumentExtractor.cpp/DocumentStructuralReader.cpp) produces into, and the
// only thing generic document actions and DocumentPatchValidator resolve a
// DocumentLocator against. It sits beside DocumentExtractionResult (the
// existing flattened-text/derived-content contract MatterRepository already
// caches) rather than replacing it: extraction_status/AI-ready text stays
// exactly as before, this adds the structural view a locator needs.

// A cell's FORMAL role, derived only from generic OOXML formatting evidence
// (cell shading, whether its visible text is entirely bold, merge geometry) -
// never from what the text actually says. This is Office structure, not
// legal meaning: Singularity may know "this cell is a bold, unshaded label" the
// same way it knows a cell's row/column position, but it must never conclude
// what a label or a title MEANS. See DocxExtractor.cpp's build_document_
// table_from_node() for how each value is actually derived, grounded in a
// real client document's own formatting (a title/banner cell used flat
// neutral shading and non-bold text; a severity cell used bold text on a
// saturated theme-accent fill; column-label cells used bold text with no
// shading at all - the same combination a great many real Word documents
// use for exactly these visual roles).
enum class DocumentCellRole {
	Unknown,
	Body,
	HeaderLabel,
	TitleOrBanner,
	MetadataOrSeverity,
};

[[nodiscard]] inline const char *to_string(DocumentCellRole role) noexcept {
	switch (role) {
		case DocumentCellRole::Unknown: return "unknown";
		case DocumentCellRole::Body: return "body";
		case DocumentCellRole::HeaderLabel: return "header_label";
		case DocumentCellRole::TitleOrBanner: return "title_or_banner";
		case DocumentCellRole::MetadataOrSeverity: return "metadata_or_severity";
	}
	return "unknown";
}

struct DocumentTableCell {
	std::size_t column_index = 0;
	std::string column_key;
	std::string text;
	// How many grid columns this one physical cell occupies (a horizontal
	// OOXML merge, gridSpan > 1) - real structural evidence, exposed so the
	// model-visible map can show it and so a merged cell is never mistaken
	// for several independent ones. column_key/column_index already
	// guarantee this cell gets exactly one field handle regardless of span
	// (see DocxExtractor.cpp's own per-physical-<w:tc> iteration).
	std::size_t grid_span = 1;
	// True only for a vertical-merge CONTINUATION cell (OOXML <w:vMerge/>
	// with no "restart" value) - this cell's real content lives on the
	// anchor cell above it in the same grid column, so it must never become
	// its own independently editable field. Format adapters that populate
	// this simply omit a continuation cell from the row entirely (see
	// DocxExtractor.cpp) - the field is kept on DocumentTableCell only so a
	// continuation that could not be resolved to its anchor for some reason
	// still carries a visible, honest reason rather than silently becoming
	// a normal editable field.
	bool is_merge_continuation = false;
	DocumentCellRole role = DocumentCellRole::Body;
	// Real OOXML <w:shd> cell-background evidence, when the cell declares
	// one - "" when absent, never guessed. DirectDocumentWork.cpp's own
	// document-native severity presentation (see its own top comment) reads
	// this to LEARN each severity label's real color from the document's
	// own already-populated/available blocks, rather than inventing a
	// color scheme - a real live design-partner document (the design partner's own
	// risk register) proved severity presentation is real, consistent,
	// per-severity cell shading (w:fill, and for a theme-based fill,
	// w:themeFill/w:themeFillShade too) that a generic writer must
	// reproduce exactly, never approximate with an invented RGB guess.
	std::string shading_fill_hex;
	std::string shading_theme_fill;
	std::string shading_theme_fill_shade;
};

struct DocumentTableRow {
	std::size_t row_index = 0;
	// The first cell's trimmed text when non-empty; otherwise a
	// positional fallback ("row-<n>") - see DocxExtractor.cpp's
	// extract_docx_tables(). A locator naming a fallback key can still resolve,
	// but never as reliably as a real content key.
	std::string row_key;
	std::vector<DocumentTableCell> cells;

	[[nodiscard]] const DocumentTableCell *cell_for_column(const std::string &columnKey) const noexcept {
		for (const DocumentTableCell &cell : cells) {
			if (cell.column_key == columnKey) return &cell;
		}
		return nullptr;
	}
};

struct DocumentTable {
	std::string table_id;
	// The positional identity of each column ("column-1", "column-2", ...).
	// Never derived from any one row's text, so a locator written against it
	// keeps resolving when a header cell is reworded.
	std::vector<std::string> column_headers;
	// What the table's own first row calls each column ("Item", "Owner",
	// "Status"), when it has one. This is how a person - and a model reasoning
	// about a document - names a column; `column_headers` is how Singularity
	// addresses it. Empty where the table has no header row.
	std::vector<std::string> column_labels;
	std::vector<DocumentTableRow> rows;

	// The column a caller means, whether it named the table's own label for it
	// or its positional key. Nothing when the name resolves to no column, or -
	// deliberately - to more than one: a change is never written to a column
	// Singularity had to guess.
	[[nodiscard]] std::optional<std::size_t> column_index_for(const std::string &name) const {
		std::optional<std::size_t> found;
		for (std::size_t index = 0; index < column_labels.size(); ++index) {
			if (column_labels[index] != name) continue;
			if (found.has_value()) return std::nullopt;
			found = index;
		}
		if (found.has_value()) return found;
		for (std::size_t index = 0; index < column_headers.size(); ++index) {
			if (column_headers[index] != name) continue;
			if (found.has_value()) return std::nullopt;
			found = index;
		}
		return found;
	}

	[[nodiscard]] const DocumentTableRow *find_row(const std::string &rowKey) const noexcept {
		const DocumentTableRow *found = nullptr;
		for (const DocumentTableRow &row : rows) {
			if (row.row_key != rowKey) continue;
			if (found != nullptr) return nullptr; // ambiguous - more than one row shares this key.
			found = &row;
		}
		return found;
	}
};

struct DocumentParagraph {
	std::string semantic_path;
	std::optional<unsigned int> heading_level;
	std::string text;
};

struct DocumentSlideTextBlock {
	std::string shape_id;
	std::string text;
};

struct DocumentSlide {
	std::string slide_id;
	unsigned int slide_number = 0;
	std::vector<DocumentSlideTextBlock> text_blocks;
	std::vector<DocumentTable> tables;
};

struct DocumentSheetCell {
	std::string cell_ref;
	std::string text;
};

struct DocumentSheet {
	std::string sheet_id;
	std::vector<DocumentSheetCell> cells;
};

// One document's complete structural read. `pages` reuses the existing
// DocumentPageBoundary contract PdfExtractor already populates - PDF stays
// read-only evidence with page/offset addressing, never a new page model.
struct DocumentSemanticModel {
	DocumentFormat format = DocumentFormat::unknown;
	ExtractionStatus status = ExtractionStatus::unsupported;
	std::string structural_version;
	std::vector<DocumentParagraph> paragraphs;
	std::vector<DocumentTable> tables;
	std::vector<DocumentSlide> slides;
	std::vector<DocumentSheet> sheets;
	std::vector<DocumentPageBoundary> pages;
	std::vector<ExtractionDiagnostic> diagnostics;
};

// The Phase 24.1 minimum format-capability table (see the phase spec's
// "Supported format roles for the golden path"). Read only through
// document_format_capability() below so every consumer (generic actions,
// DocumentPatchValidator, DocumentPatchTool) shares one truth about what a
// format may be used for - never re-derived ad hoc from a file extension.
struct DocumentFormatCapability {
	bool supports_structured_read = false;
	bool supports_methodology_citation = false;
	bool supports_cell_or_paragraph_mutation = false;
};

[[nodiscard]] inline DocumentFormatCapability document_format_capability(DocumentFormat format) noexcept {
	const ArtifactFormatCapability capability = artifact_format_capability(format);
	return {capability.read && capability.structured_read, capability.read,
		capability.structured_patch};
}

} // namespace singularity::documents
