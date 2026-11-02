#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace singularity::document {

enum class SourceResolutionStatus {
	// The exact chunk cited still matches this Document's current content -
	// resolved to a real local preview.
	Resolved,
	// This Document's real current content no longer matches what the
	// citation was made against (Document/DocumentChunker.hpp: a changed
	// file's chunk_ids are never reused, since the fingerprint is part of
	// every chunk_id) - never silently bound to new bytes.
	Changed,
	// The Document, its file, or its cached extraction is no longer
	// available to resolve from (deleted, unavailable, not yet processed).
	Unavailable,
};

struct ResolvedSource {
	SourceResolutionStatus status = SourceResolutionStatus::Unavailable;
	std::string document_id;
	std::string document_name;
	int current_version = 0;
	// Set when status == Resolved, and for a Changed citation whose cited
	// version's extraction is still kept: then they are the passage exactly as
	// it read when it was cited, never the document's current words. Real facts from
	// Document/DocumentChunker.hpp, never invented when unknown. chunk_id is
	// the exact, content-addressed identity of the chunk actually resolved -
	// the same identifying token a real citation carries (see
	// context::ResolvedSourceReference), so a caller that resolved with an
	// empty chunk_id (Phase 09's own "Preview source") can still learn
	// exactly which chunk it landed on.
	std::string chunk_id;
	std::optional<std::size_t> page;
	std::optional<std::string> heading;
	std::string text_preview;
};

// Resolves a real citation (document_id + chunk_id, exactly
// context::ResolvedSourceReference's own identifying fields - the same
// object a real Chat task's source_references already persists) back to a
// local source preview. `chunk_id` empty resolves the Document's first real
// chunk instead - Phase 09's own "Preview source" entry point, which has no
// historical reference to compare against and so is always Resolved once
// any chunk exists.
//
// Re-derives current truth through the same real, non-persisting targeted
// reconciliation Phase 09's other source-sensitive actions use
// (documents::registry::reconcile_affected_paths(), see
// Routing/MatterRequestExecution.cpp's validate_expected_document_versions())
// before deciding Resolved vs. Changed, so a file that changed moments ago -
// ahead of the asynchronous filesystem watcher - is still caught. Degrades
// honestly to Unavailable rather than inventing a page/heading/preview it
// cannot prove; never a new provenance system, only Phase 08's existing
// identity/version/hash/chunk truth.
//
// A citation is found in the reading it was cut from: `cited_reading` is the
// extractor version its reference recorded (context::ResolvedSourceReference::
// extractor_version). Empty is a citation recorded before readings were kept,
// which was cut from the reading its format had then (legacy_reading_version).
// A document Singularity has since read better is still the document it was: its
// citation resolves to the very passage the model was given (24 Sep 2026: the
// workbook reading improved, and every earlier workbook citation would
// otherwise have read "changed" with nothing to show).
ResolvedSource resolve_source(const std::string& matter_id, const std::string& document_id,
                              const std::string& chunk_id, const std::string& cited_reading = std::string());

} // namespace singularity::document
