#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace singularity::documents {

struct PreparedDocument {
  std::string fingerprint;
  long long size_bytes = 0;
  std::string last_modified;
  DocumentExtractionResult extraction;
  bool cache_hit = false;
};

// Local derived-content store. Cache entries are addressed by the source
// SHA-256 plus the extractor version, so duplicates deliberately share one
// derivative while parser upgrades cannot reuse stale text.
class DocumentCache final {
public:
  DocumentCache(std::filesystem::path workspace_path,
                const DocumentExtractor &extractor);

  [[nodiscard]] PreparedDocument
  prepare(const std::filesystem::path &source_path, DocumentFormat format,
          const std::string &extractor_version) const;

  [[nodiscard]] std::optional<DocumentExtractionResult>
  load(const std::string &fingerprint,
       const std::string &extractor_version) const;

  // Whether a reading of these bytes by this extractor version is kept, found
  // without reading it: a Matter's first look asks this of every document.
  [[nodiscard]] bool holds(const std::string &fingerprint,
                           const std::string &extractor_version) const;

private:
  std::filesystem::path workspace_path_;
  const DocumentExtractor &extractor_;
};

[[nodiscard]] std::string
file_last_modified_token(const std::filesystem::path &path);

} // namespace singularity::documents
