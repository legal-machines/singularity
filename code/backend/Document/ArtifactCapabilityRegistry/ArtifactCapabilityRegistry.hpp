#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include <string_view>

namespace singularity::documents {

// The single format capability truth. Routing asks this registry; adapters
// prove the flags. Adding a format extends this table and its adapter without
// adding file-type branches to professional intent resolution.
struct ArtifactFormatCapability {
	DocumentFormat format = DocumentFormat::unknown;
	bool read = false;
	bool structured_read = false;
	bool create = false;
	bool append_content = false;
	bool replace_content = false;
	bool update_cells = false;
	bool structured_patch = false;
	// A workbook's own structure beside its values: whole rows inserted the way
	// Excel inserts them (Document/XlsxRowInsertion), and formulas written into
	// cells - calculated in place where Singularity calculates them, and by the
	// spreadsheet application when the workbook is opened.
	bool insert_rows = false;
	bool write_formulas = false;
	// Whole rows taken out the way Excel's Delete Row takes them, every
	// reference into them becoming #REF! as Excel makes it and named to the
	// lawyer first; and new, empty sheets added after the last tab.
	bool delete_rows = false;
	bool add_sheets = false;
	std::string_view limitation;

	// Whether a model may work ON this format the way a lawyer works on a
	// document: a passage replaced or deleted, a paragraph inserted, a row
	// added or removed, a passage written differently. Today that is Word,
	// because Word is the format Singularity has a real structural writer for
	// (Document/OfficeTextEdit + DocxExtractor). Everything else keeps the
	// narrower capabilities it already had.
	[[nodiscard]] constexpr bool document_operations() const noexcept {
		return format == DocumentFormat::docx;
	}

	[[nodiscard]] constexpr bool can_update() const noexcept {
		return append_content || replace_content || update_cells || document_operations();
	}
};

[[nodiscard]] constexpr ArtifactFormatCapability artifact_format_capability(
		DocumentFormat format) noexcept {
	switch (format) {
		case DocumentFormat::docx:
			return {format, true, true, true, true, true, false, true, false, false, false, false,
				"A replacement anchors on the exact text of one run; every update is committed only after the staged "
				"document is proven to hold exactly that change."};
		case DocumentFormat::pdf:
			return {format, true, true, false, false, false, false, false, false, false, false, false,
				"A PDF is never rewritten in place. Work a colleague is given on one is written into a copy to review: "
				"highlights with notes, or a redaction that takes the passages out for good (D-115); nothing else of a PDF "
				"is edited."};
		case DocumentFormat::xlsx:
			return {format, true, true, true, false, false, true, false, true, true, true, true,
				"Cells holding formulas or calculated values are never overwritten; a number stays a number and text "
				"stays text. A formula Singularity writes holds its value where Singularity calculates it (sums and "
				"arithmetic) and is otherwise calculated when the workbook is opened in Excel. An inserted or deleted "
				"row moves every formula, name, merged cell, format rule, validation, table and hidden row below it, "
				"or is refused where that cannot be done exactly; a reference into a deleted row becomes #REF!, as "
				"in Excel, and is named before the change is approved. A new sheet stands after the last tab."};
		case DocumentFormat::csv:
		case DocumentFormat::tsv:
			return {format, true, true, true, true, true, false,
				format == DocumentFormat::csv, false, false, false, false, {}};
		case DocumentFormat::plain_text:
			return {format, true, false, true, true, true, false, false, false, false, false, false, {}};
		case DocumentFormat::markdown:
			return {format, true, true, true, true, true, false, false, false, false, false, false, {}};
		case DocumentFormat::json:
		case DocumentFormat::xml:
			return {format, true, false, true, false, true, false, false, false, false, false, false,
				"This structured text format can be created or replaced as a validated whole document; "
				"Singularity does not claim a structural object mutation adapter for it."};
		case DocumentFormat::html:
			return {format, true, false, true, true, true, false, false, false, false, false, false,
				"HTML is read and updated as tolerant UTF-8 text; Singularity does not claim DOM-structured mutation."};
		case DocumentFormat::pptx:
			return {format, true, true, false, false, false, false, false, false, false, false, false,
				"A deck is never rewritten in place. Work a colleague is given on one is written into a copy to review: the "
				"words on its slides and in their tables, each in the slide's own formatting (D-115); layout, charts and "
				"pictures are not edited."};
		case DocumentFormat::unknown:
			return {format, false, false, false, false, false, false, false, false, false, false, false,
				"No format adapter recognizes this file."};
	}
	return {};
}

} // namespace singularity::documents
