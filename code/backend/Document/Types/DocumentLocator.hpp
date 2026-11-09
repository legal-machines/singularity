#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace singularity::documents {

// Format-neutral structural locator. Binds to a document's identity and, where
// relevant to its format, the structural path to one piece of content - a table
// cell, a slide/shape, a sheet cell or a page offset range.
// document/version/fingerprint identity itself lives on DocumentPatch (the
// mutation guard), not here, so one locator can be reused to describe a
// citation without implying a version it was never checked against.
//
// A locator that cannot resolve unambiguously against the document's current
// DocumentSemanticModel must fail closed - see DocumentPatchValidator. The
// optional table_index_hint/row_index_hint/column_index_hint fields are
// adapter-detail only (a cheap first guess for the physical DOM walk); they
// are never trusted as the sole identity guard - row_key/column_key resolved
// against the live structure is.
struct DocumentLocator {
	std::string document_id;

	std::optional<std::string> semantic_path;
	std::optional<std::string> heading;
	std::optional<std::size_t> page;

	// DOCX/XLSX/CSV table or grid identity.
	std::optional<std::string> table_id;
	std::optional<std::string> row_key;
	std::optional<std::string> column_key;

	// PPTX identity.
	std::optional<std::string> slide_id;
	std::optional<std::string> shape_id;

	// XLSX/CSV identity.
	std::optional<std::string> sheet_id;
	std::optional<std::string> cell_ref;

	// PDF/plain-text source offsets, where extraction provides them.
	std::optional<std::size_t> source_start;
	std::optional<std::size_t> source_end;

	std::optional<std::size_t> table_index_hint;
	std::optional<std::size_t> row_index_hint;
	std::optional<std::size_t> column_index_hint;
};

// A short human-readable label for evidence/citation display. Never used as a
// resolution key - only for provenance text shown to a user or recorded on a
// citation.
[[nodiscard]] inline std::string format_locator_label(const DocumentLocator &locator,
		const std::string &documentName) {
	std::string label = documentName;
	if (locator.heading.has_value()) label += " - " + *locator.heading;
	if (locator.table_id.has_value() && locator.row_key.has_value()) {
		label += " - row " + *locator.row_key;
	}
	if (locator.column_key.has_value()) label += " - " + *locator.column_key;
	if (locator.slide_id.has_value()) label += " - " + *locator.slide_id;
	if (locator.sheet_id.has_value() && locator.cell_ref.has_value()) {
		label += " - " + *locator.sheet_id + "!" + *locator.cell_ref;
	}
	if (locator.page.has_value()) label += " - p. " + std::to_string(*locator.page);
	return label;
}

// A content-addressed identity for one structural locator, frozen against a
// specific document fingerprint - the "real persisted identity" a citation
// keeps even after the document is later mutated (mirrors
// context::ResolvedSourceReference::chunk_id's own documented role as the
// real persisted identity, Context/SourceMarkers/SourceMarkers.hpp). Two
// locators that resolve to the same structural path but were captured
// against different fingerprints always produce different ids - this is
// what lets a persisted citation prove it was never silently rebound to a
// newer version of the same document. Production code (not a test) owns
// this construction so every caller gets the same identity convention.
[[nodiscard]] inline std::string content_addressed_locator_id(
		const DocumentLocator &locator, const std::string &fingerprint) {
	std::string path = locator.document_id;
	if (locator.table_id.has_value()) path += "/table/" + *locator.table_id;
	if (locator.row_key.has_value()) path += "/row/" + *locator.row_key;
	if (locator.column_key.has_value()) path += "/column/" + *locator.column_key;
	if (locator.slide_id.has_value()) path += "/slide/" + *locator.slide_id;
	if (locator.shape_id.has_value()) path += "/shape/" + *locator.shape_id;
	if (locator.sheet_id.has_value()) path += "/sheet/" + *locator.sheet_id;
	if (locator.cell_ref.has_value()) path += "/cell/" + *locator.cell_ref;
	if (locator.heading.has_value()) path += "/heading/" + *locator.heading;
	if (locator.page.has_value()) path += "/page/" + std::to_string(*locator.page);
	return path + "@fp:" + fingerprint;
}

// The one JSON shape a structural locator uses everywhere it crosses a
// boundary that speaks JSON: a ToolBroker ToolIntent's arguments
// (DocumentPatchTool) and a structured model result
// (generic document action proposals). Only document_id is required; every other
// field is copied through when present, left unset otherwise - never
// defaulted to a guessed value.
[[nodiscard]] inline nlohmann::json document_locator_to_json(const DocumentLocator &locator) {
	nlohmann::json value{{"documentId", locator.document_id}};
	if (locator.semantic_path.has_value()) value["semanticPath"] = *locator.semantic_path;
	if (locator.heading.has_value()) value["heading"] = *locator.heading;
	if (locator.page.has_value()) value["page"] = *locator.page;
	if (locator.table_id.has_value()) value["tableId"] = *locator.table_id;
	if (locator.row_key.has_value()) value["rowKey"] = *locator.row_key;
	if (locator.column_key.has_value()) value["columnKey"] = *locator.column_key;
	if (locator.slide_id.has_value()) value["slideId"] = *locator.slide_id;
	if (locator.shape_id.has_value()) value["shapeId"] = *locator.shape_id;
	if (locator.sheet_id.has_value()) value["sheetId"] = *locator.sheet_id;
	if (locator.cell_ref.has_value()) value["cellRef"] = *locator.cell_ref;
	if (locator.source_start.has_value()) value["sourceStart"] = *locator.source_start;
	if (locator.source_end.has_value()) value["sourceEnd"] = *locator.source_end;
	return value;
}

// Returns std::nullopt (rather than throwing) on any structurally invalid
// input - "documentId" missing/non-string, or any present optional field
// with the wrong JSON type - so a caller can report one clear blocker
// instead of a raw JSON parse exception.
[[nodiscard]] inline std::optional<DocumentLocator> document_locator_from_json(const nlohmann::json &value) {
	if (!value.is_object() || !value.contains("documentId") || !value.at("documentId").is_string()) {
		return std::nullopt;
	}
	DocumentLocator locator;
	locator.document_id = value.at("documentId").get<std::string>();
	const auto optionalString = [&](const char *key, std::optional<std::string> &target) {
		if (!value.contains(key)) return true;
		if (!value.at(key).is_string()) return false;
		target = value.at(key).get<std::string>();
		return true;
	};
	const auto optionalIndex = [&](const char *key, std::optional<std::size_t> &target) {
		if (!value.contains(key)) return true;
		if (!value.at(key).is_number_unsigned()) return false;
		target = value.at(key).get<std::size_t>();
		return true;
	};
	if (!optionalString("semanticPath", locator.semantic_path)) return std::nullopt;
	if (!optionalString("heading", locator.heading)) return std::nullopt;
	if (!optionalIndex("page", locator.page)) return std::nullopt;
	if (!optionalString("tableId", locator.table_id)) return std::nullopt;
	if (!optionalString("rowKey", locator.row_key)) return std::nullopt;
	if (!optionalString("columnKey", locator.column_key)) return std::nullopt;
	if (!optionalString("slideId", locator.slide_id)) return std::nullopt;
	if (!optionalString("shapeId", locator.shape_id)) return std::nullopt;
	if (!optionalString("sheetId", locator.sheet_id)) return std::nullopt;
	if (!optionalString("cellRef", locator.cell_ref)) return std::nullopt;
	if (!optionalIndex("sourceStart", locator.source_start)) return std::nullopt;
	if (!optionalIndex("sourceEnd", locator.source_end)) return std::nullopt;
	return locator;
}

} // namespace singularity::documents
