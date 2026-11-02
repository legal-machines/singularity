#include "Document/DocumentSourceResolver/DocumentSourceResolver.hpp"

#include "Document/DocumentCache/DocumentCache.hpp"
#include "Document/DocumentChunker/DocumentChunker.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace singularity::document {
namespace {

const matters::MatterDocument* find_document(const matters::Matter& matter, const std::string& document_id) {
	const auto found = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument& entry) { return entry.id == document_id; });
	return found == matter.documents.end() ? nullptr : &(*found);
}

// The version of the document a chunk_id was cut from (document_id:
// fingerprint:start-end); empty when it names none.
std::string cited_fingerprint(const std::string& document_id, const std::string& chunk_id) {
	const std::string prefix = document_id + ":";
	if (chunk_id.rfind(prefix, 0) != 0) return {};
	const std::size_t end = chunk_id.find(':', prefix.size());
	if (end == std::string::npos) return {};
	const std::string fingerprint = chunk_id.substr(prefix.size(), end - prefix.size());
	if (fingerprint.size() != 64 || std::all_of(fingerprint.begin(), fingerprint.end(),
			[](unsigned char c) { return std::isxdigit(c) != 0; }) == false) return {};
	return fingerprint;
}

// Whether the file holds exactly the version `fingerprint` names. A file that
// cannot be read holds none.
bool file_holds(const std::filesystem::path& path, const std::string& fingerprint) {
	try {
		return fingerprint.empty() == false && platform::sha256_hex_digest_of_file(path) == fingerprint;
	} catch (const std::exception&) {
		return false;
	}
}

// A cited passage, exactly as it read when it was cited. The chunk_id names
// the version of the document it was cut from, and `reading` the reading of
// that version: that extraction as the cache kept it - or, while the file is
// still that version (`current` names it), that reading produced again - is
// re-chunked and the chunk_id matched exactly. That gives back the very text
// that was supplied - never the document's current words, never a passage
// found by position or nearness. False on any miss, the passage unshown.
bool fill_cited_passage(ResolvedSource& result, const documents::DocumentCache& cache,
		const std::string& document_id, const std::string& chunk_id, const std::string& reading,
		const std::optional<documents::DocumentExtractionRequest>& current = std::nullopt,
		const std::string& current_fingerprint = std::string()) {
	const std::string fingerprint = cited_fingerprint(document_id, chunk_id);
	if (fingerprint.empty() || reading.empty()) return false;
	std::optional<documents::DocumentExtractionResult> extraction = cache.load(fingerprint, reading);
	if (!extraction && current && fingerprint == current_fingerprint)
		extraction = documents::extract_earlier_reading(*current, reading);
	if (!extraction || !extraction->ai_ready()) return false;
	for (const documents::DocumentChunk& chunk : documents::chunk_document(document_id, fingerprint, *extraction)) {
		if (chunk.chunk_id != chunk_id) continue;
		result.chunk_id = chunk.chunk_id;
		result.page = chunk.page;
		result.heading = chunk.heading;
		result.text_preview = chunk.text;
		return true;
	}
	return false;
}

} // namespace

ResolvedSource resolve_source(const std::string& matter_id, const std::string& document_id,
		const std::string& chunk_id, const std::string& cited_reading) {
	ResolvedSource result;
	result.document_id = document_id;

	matters::Matter matter = matter::read_project_documents(matter_id);
	const matters::MatterDocument* document = find_document(matter, document_id);
	if (document == nullptr) return result; // Unavailable
	result.document_name = document->name;

	// No real Project-folder path to re-derive current truth from - honest
	// degrade rather than trusting a possibly-stale Managed-copy snapshot.
	if (document->source_mode != matters::DocumentSourceMode::Referenced) return result;
	if (!matter.directory_path || matter.directory_path->empty() ||
			documents::registry::is_relative_path_safe(*matter.directory_path, document->relative_path) == false) {
		return result;
	}

	const std::filesystem::path absolute_path = std::filesystem::path(*matter.directory_path) / document->relative_path;
	// Called for its in-place mutation of matter.documents; the outcome
	// itself is re-derived below via a fresh lookup by document_id.
	(void)documents::registry::reconcile_affected_paths(matter, {absolute_path.string()});

	document = find_document(matter, document_id);
	if (document == nullptr || document->available == false) return result; // Unavailable
	result.current_version = document->version;
	// lifecycle_state (not extraction_status - see MatterDocument.hpp) is
	// what the reconcile above just updated: Changed the instant new
	// content is detected, well before process_pending_documents() ever
	// re-extracts it and updates extraction_status. Reading lifecycle_state
	// here is what actually catches "changed but not yet re-processed"
	// honestly, instead of chasing a cache entry for a fingerprint nothing
	// has extracted yet.
	const std::filesystem::path workspace_path = node::require_active_workspace_path();
	const documents::LocalDocumentExtractor extractor;
	const documents::DocumentCache cache(workspace_path, extractor);
	// The reading the citation was cut from (see resolve_source's comment).
	const std::string reading = !cited_reading.empty() ? cited_reading
		: documents::legacy_reading_version(document->file_type);
	if (document->lifecycle_state == matters::DocumentLifecycleState::Changed) {
		result.status = SourceResolutionStatus::Changed;
		if (chunk_id.empty()) return result;
		// Waiting to be read again is not a change: a file that still holds the
		// very bytes the passage was cut from - a better reading of it pending,
		// or only its date touched - is the document that was cited.
		const std::string cited = cited_fingerprint(document_id, chunk_id);
		const bool unchanged = file_holds(absolute_path, cited);
		const bool found = fill_cited_passage(result, cache, document_id, chunk_id, reading,
				unchanged == true ? std::optional<documents::DocumentExtractionRequest>(
					documents::DocumentExtractionRequest{absolute_path, document->file_type}) : std::nullopt, cited)
			|| (reading != document->extractor_version
				&& fill_cited_passage(result, cache, document_id, chunk_id, document->extractor_version));
		if (found == true && unchanged == true) result.status = SourceResolutionStatus::Resolved;
		return result;
	}
	if (document->lifecycle_state != matters::DocumentLifecycleState::Ready) return result; // Unavailable

	// A citation cut from an earlier reading of this document is found in that
	// reading. The document may be exactly as it was, only read better since:
	// then the citation is Resolved to the very passage the model was given.
	if (!chunk_id.empty() && reading != document->extractor_version
			&& fill_cited_passage(result, cache, document_id, chunk_id, reading,
				documents::DocumentExtractionRequest{absolute_path, document->file_type}, document->fingerprint)) {
		result.status = cited_fingerprint(document_id, chunk_id) == document->fingerprint
			? SourceResolutionStatus::Resolved : SourceResolutionStatus::Changed;
		return result;
	}

	const auto extraction = cache.load(document->fingerprint, document->extractor_version);
	if (!extraction || !extraction->ai_ready()) return result; // Unavailable

	const auto chunks = documents::chunk_document(document_id, document->fingerprint, *extraction);
	if (chunks.empty()) return result; // Unavailable

	const auto found_chunk = chunk_id.empty()
			? chunks.begin()
			: std::find_if(chunks.begin(), chunks.end(),
					[&](const documents::DocumentChunk& chunk) { return chunk.chunk_id == chunk_id; });
	if (found_chunk == chunks.end()) {
		result.status = SourceResolutionStatus::Changed;
		fill_cited_passage(result, cache, document_id, chunk_id, document->extractor_version);
		return result;
	}

	result.status = SourceResolutionStatus::Resolved;
	result.chunk_id = found_chunk->chunk_id;
	result.page = found_chunk->page;
	result.heading = found_chunk->heading;
	result.text_preview = found_chunk->text;
	return result;
}

} // namespace singularity::document
