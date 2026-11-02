#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

enum class DocumentFormat {
  unknown,
  plain_text,
  markdown,
  pdf,
  docx,
  pptx,
  xlsx,
  csv,
	tsv,
	json,
	xml,
	html,
};

enum class ExtractionStatus {
  ready,
  empty,
  ocr_required,
  unsupported,
  unreadable,
};

struct ExtractionDiagnostic {
  std::string code;
  std::string message;
};

struct DocumentHeading {
  std::size_t line = 0;
  unsigned int level = 0;
  std::string title;
};

// Offsets refer to the normalized UTF-8 `DocumentExtractionResult::text`.
// `page_number` is 1-based so it can be used directly in user-facing legal
// source references. PDF extractors populate these fields when page boundaries
// are reliable; plain-text formats intentionally leave them empty.
struct DocumentPageBoundary {
  std::size_t page_number = 1;
  std::size_t text_offset = 0;
  std::size_t text_length = 0;
};

struct DocumentStructure {
  std::size_t byte_count = 0;
  std::size_t line_count = 0;
  std::size_t paragraph_count = 0;
  std::optional<std::size_t> page_count;
  std::vector<DocumentPageBoundary> page_boundaries;
  std::vector<DocumentHeading> headings;
};

struct DocumentExtractionRequest {
  std::filesystem::path path;
  std::optional<DocumentFormat> format_hint;
  std::size_t maximum_input_bytes = 64U * 1024U * 1024U;
};

struct DocumentExtractionResult {
  DocumentFormat format = DocumentFormat::unknown;
  ExtractionStatus status = ExtractionStatus::unsupported;
  // Stable parser/algorithm identifier used with the source fingerprint as
  // the derived-content cache key. Empty only when no extractor ran.
  std::string extractor_version;
  std::string text;
  DocumentStructure structure;
  std::vector<ExtractionDiagnostic> diagnostics;

  [[nodiscard]] bool ai_ready() const noexcept {
    return status == ExtractionStatus::ready;
  }
};

class DocumentExtractor {
public:
  virtual ~DocumentExtractor() = default;

  [[nodiscard]] virtual DocumentExtractionResult
  extract(const DocumentExtractionRequest &request) const = 0;
};

// The packaged, local extractor boundary. Format-specific implementations can
// be added behind this class without introducing parser concerns into Matter
// persistence or the native bridge.
class LocalDocumentExtractor final : public DocumentExtractor {
public:
  [[nodiscard]] DocumentExtractionResult
  extract(const DocumentExtractionRequest &request) const override;
};

[[nodiscard]] DocumentFormat
detect_document_format(const std::filesystem::path &path);

[[nodiscard]] const char *to_string(DocumentFormat format) noexcept;
[[nodiscard]] std::optional<DocumentFormat> document_format_from_string(
	const std::string &value) noexcept;
[[nodiscard]] const char *to_string(ExtractionStatus status) noexcept;

[[nodiscard]] std::string local_extractor_version(DocumentFormat format);

// An earlier reading of a document that Singularity can still produce, by the
// version passages cited from it were cut under - so a citation made before a
// reading improved still shows the passage it was given (DocumentSourceResolver).
// Nothing for the current version, or for one Singularity can no longer produce.
[[nodiscard]] std::optional<DocumentExtractionResult>
extract_earlier_reading(const DocumentExtractionRequest &request, const std::string &extractor_version);

// The reading a format had before citations kept theirs (24 Sep 2026) - what
// a citation that recorded none was cut from.
[[nodiscard]] std::string legacy_reading_version(DocumentFormat format);

} // namespace singularity::documents
