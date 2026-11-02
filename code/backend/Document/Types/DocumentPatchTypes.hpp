#pragma once

#include "Document/Types/DocumentLocator.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// The controlled Project-mutation intent contract (phase-24-1-research-
// document-intelligence-project-actions.md, "Controlled Project actions").
// A model never writes files directly - it can only ever produce this intent,
// which DocumentPatchValidator checks against live document state and the
// format's DOCX/CSV writer (the only formats Phase 24.1C implements real
// mutation for) applies.

// What one operation does to a document. Deliberately few, and each one is
// something a lawyer would say out loud about their own document - not an
// implementation step. A model chooses among these; Singularity resolves each
// against the document's live structure and makes the real change.
//
//   replaceCellText   one table cell's text becomes something else
//   replaceText       a passage the document holds becomes something else
//   insertParagraph   new text stands before or after a passage
//   deleteContent     a passage goes, and an emptied paragraph goes with it
//   insertTableRow    a row joins a table, by the columns it fills
//   deleteTableRow    a row leaves a table
//   setFormatting     a passage or its paragraph is WRITTEN differently and
//                     says exactly the same thing
//
// Everything here is addressed by MEANING - a table by its id, a row by its
// own first cell, a column by its header, a passage by its own words - never
// by a position in a file. A locator that does not resolve to exactly one
// place fails closed (DocumentPatchValidator).
enum class DocumentPatchOperationKind {
	ReplaceCellText,
	ReplaceText,
	InsertParagraph,
	DeleteContent,
	InsertTableRow,
	DeleteTableRow,
	SetFormatting,
};

[[nodiscard]] inline const char *to_string(DocumentPatchOperationKind kind) noexcept {
	switch (kind) {
		case DocumentPatchOperationKind::ReplaceCellText: return "replaceCellText";
		case DocumentPatchOperationKind::ReplaceText: return "replaceText";
		case DocumentPatchOperationKind::InsertParagraph: return "insertParagraph";
		case DocumentPatchOperationKind::DeleteContent: return "deleteContent";
		case DocumentPatchOperationKind::InsertTableRow: return "insertTableRow";
		case DocumentPatchOperationKind::DeleteTableRow: return "deleteTableRow";
		case DocumentPatchOperationKind::SetFormatting: return "setFormatting";
	}
	return "replaceCellText";
}

[[nodiscard]] inline std::optional<DocumentPatchOperationKind> patch_operation_kind_from_string(
		const std::string &value) {
	if (value == "replaceCellText") return DocumentPatchOperationKind::ReplaceCellText;
	if (value == "replaceText") return DocumentPatchOperationKind::ReplaceText;
	if (value == "insertParagraph") return DocumentPatchOperationKind::InsertParagraph;
	if (value == "deleteContent") return DocumentPatchOperationKind::DeleteContent;
	if (value == "insertTableRow") return DocumentPatchOperationKind::InsertTableRow;
	if (value == "deleteTableRow") return DocumentPatchOperationKind::DeleteTableRow;
	if (value == "setFormatting") return DocumentPatchOperationKind::SetFormatting;
	return std::nullopt;
}

// Where new content stands in relation to what the operation named.
enum class DocumentPatchPlacement { Before, After, AtEnd };

[[nodiscard]] inline const char *to_string(DocumentPatchPlacement placement) noexcept {
	switch (placement) {
		case DocumentPatchPlacement::Before: return "before";
		case DocumentPatchPlacement::After: return "after";
		case DocumentPatchPlacement::AtEnd: return "atEnd";
	}
	return "after";
}

[[nodiscard]] inline std::optional<DocumentPatchPlacement> patch_placement_from_string(const std::string &value) {
	if (value == "before") return DocumentPatchPlacement::Before;
	if (value == "after") return DocumentPatchPlacement::After;
	if (value == "atEnd") return DocumentPatchPlacement::AtEnd;
	return std::nullopt;
}

// How a passage or a paragraph is written. Every field is optional and means
// "leave this as it is" when unset: a change that makes a warning bold must not
// also decide its font, and a change of style must not silently un-bold what
// the author emphasised.
struct DocumentTextFormatting {
	// The passage's own runs.
	std::optional<bool> bold;
	std::optional<bool> italic;
	std::optional<bool> underline;
	// "C00000" - six hexadecimal digits, as OOXML writes a colour.
	std::optional<std::string> color_hex;
	// An OOXML highlight name ("yellow", "cyan", ...), or "none" to remove one.
	std::optional<std::string> highlight;
	// In points, as a lawyer sets them in Word.
	std::optional<double> size_points;
	std::optional<std::string> font_name;

	// The paragraph the passage stands in.
	std::optional<std::string> paragraph_style;
	// "left", "center", "right", "justify" (OOXML: w:jc).
	std::optional<std::string> alignment;
	std::optional<double> space_before_points;
	std::optional<double> space_after_points;

	[[nodiscard]] bool touches_runs() const {
		return bold || italic || underline || color_hex || highlight || size_points || font_name;
	}
	[[nodiscard]] bool touches_paragraph() const {
		return paragraph_style || alignment || space_before_points || space_after_points;
	}
	[[nodiscard]] bool empty() const { return !touches_runs() && !touches_paragraph(); }
};

// One cell of a row an operation writes, named by the column's own header.
struct DocumentPatchCellValue {
	std::string column_key;
	std::string value;
};

struct DocumentPatchOperation {
	DocumentPatchOperationKind kind = DocumentPatchOperationKind::ReplaceCellText;
	DocumentLocator locator;
	std::string expected_old_value;
	std::string new_value;
	// Links this operation to its generic, MatterTask-owned action rationale.
	// The rationale may describe any professional document task; DocumentPatch
	// itself remains concerned only with safe structural mutation.
	std::string rationale_id;
	// Optional real OOXML cell-background replacement, applied alongside the
	// text replacement above when set (see DocumentTableCell's own shading_*
	// fields for where this evidence comes from). Never invented - the one
	// production caller (DirectDocumentWork.cpp's own document-native
	// severity presentation) only ever sets this to shading it already read
	// from another real cell in the same live document declaring the same
	// semantic value (e.g. an existing "Низкий"-labeled cell's own real
	// green). Empty (the default) means "leave this cell's existing shading
	// untouched" - exactly today's behavior for every other caller.
	std::string new_shading_fill_hex;
	std::string new_shading_theme_fill;
	std::string new_shading_theme_fill_shade;

	// Where insertParagraph/insertTableRow put what they add, in relation to
	// the passage or row the locator names. AtEnd means the end of the
	// document's table or body, and then no anchor is needed.
	DocumentPatchPlacement placement = DocumentPatchPlacement::After;
	// The row an insertTableRow writes, by column header. A column the table
	// does not have is a refusal, never a silently dropped value.
	std::vector<DocumentPatchCellValue> cells;
	// How a setFormatting operation writes what it names. Every unset field
	// means "as the document already has it".
	DocumentTextFormatting formatting;
};

struct DocumentPatch {
	std::string document_id;
	int expected_version = 0;
	std::string expected_fingerprint;
	std::vector<DocumentPatchOperation> operations;
	// Who the tracked changes this patch writes are recorded as
	// (AppSettings::revision_author, set by the caller that can read it).
	std::string revision_author = "Singularity";
};

// Every reason DocumentPatchValidator/the format writer can refuse a patch -
// see that validator's own comment for what each one checks. `None` is only
// ever the value on a result whose `ok` is true.
enum class DocumentPatchBlocker {
	None,
	DocumentIdentityMismatch,
	VersionMismatch,
	FingerprintMismatch,
	UnsupportedFormatCapability,
	LocatorUnresolved,
	LocatorAmbiguous,
	OldValueMismatch,
	InvalidNewValue,
	OutputValidationFailed,
	AtomicReplaceFailed,
};

[[nodiscard]] inline const char *to_string(DocumentPatchBlocker blocker) noexcept {
	switch (blocker) {
		case DocumentPatchBlocker::None: return "none";
		case DocumentPatchBlocker::DocumentIdentityMismatch: return "document_identity_mismatch";
		case DocumentPatchBlocker::VersionMismatch: return "version_mismatch";
		case DocumentPatchBlocker::FingerprintMismatch: return "fingerprint_mismatch";
		case DocumentPatchBlocker::UnsupportedFormatCapability: return "unsupported_format_capability";
		case DocumentPatchBlocker::LocatorUnresolved: return "locator_unresolved";
		case DocumentPatchBlocker::LocatorAmbiguous: return "locator_ambiguous";
		case DocumentPatchBlocker::OldValueMismatch: return "old_value_mismatch";
		case DocumentPatchBlocker::InvalidNewValue: return "invalid_new_value";
		case DocumentPatchBlocker::OutputValidationFailed: return "output_validation_failed";
		case DocumentPatchBlocker::AtomicReplaceFailed: return "atomic_replace_failed";
	}
	return "unsupported_format_capability";
}

// One operation's resolution against the live DocumentSemanticModel. The
// physical indices are adapter detail for the writer - never re-used as an
// identity guard on their own (see DocumentLocator's own comment).
struct DocumentPatchOperationOutcome {
	bool ok = false;
	DocumentPatchBlocker blocker = DocumentPatchBlocker::None;
	std::string detail;
	std::optional<std::size_t> resolved_row_index;
	std::optional<std::size_t> resolved_column_index;
};

struct DocumentPatchValidationResult {
	bool ok = false;
	DocumentPatchBlocker blocker = DocumentPatchBlocker::None;
	std::string detail;
	std::vector<DocumentPatchOperationOutcome> operations;
};

} // namespace singularity::documents
