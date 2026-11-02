#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// A local, source-addressable derivative of one extracted Matter document.
// source_start/source_end are half-open byte offsets into the normalized UTF-8
// DocumentExtractionResult::text. `estimated_size` is a conservative local
// token estimate, not a provider-specific tokenizer result.
struct DocumentChunk {
  std::string document_id;
  std::string chunk_id;
  std::string text;
  std::optional<std::size_t> page;
  std::optional<std::string> heading;
  std::size_t source_start = 0;
  std::size_t source_end = 0;
  std::size_t estimated_size = 0;
  // The reading of the document this passage was cut from (its extraction's
  // extractor_version). A citation keeps it, so the passage it was given can
  // be found again after Singularity reads that kind of document better.
  std::string extractor_version;
};

struct DocumentChunkOptions {
  // A byte bound keeps chunking deterministic without depending on a provider
  // tokenizer. The default leaves later context budgeting room for metadata.
  std::size_t maximum_bytes = 4096;
};

// Creates chunks only for AI-ready extraction results. The fingerprint is
// part of every chunk ID, so changed source bytes cannot retain stale IDs.
[[nodiscard]] std::vector<DocumentChunk>
chunk_document(const std::string &document_id,
               const std::string &source_fingerprint,
               const DocumentExtractionResult &extraction,
               const DocumentChunkOptions &options = {});

} // namespace singularity::documents
