#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace singularity::matters {

enum class DocumentSourceMode { Managed, Referenced };
enum class DocumentExtractionState { NotExtracted, Ready, Empty, OcrRequired, Unsupported, Unreadable };

// The six user-facing Document states Phase 08 owns (phase-08-documents-filesystem-states.md
// Sec.9). A Managed document derives this from extraction_status (see
// document_lifecycle_state() in MatterRepository.cpp); a Referenced document
// (source_mode == Referenced, real Project-folder file) stores it directly -
// DocumentRegistry.cpp is the only writer, since only it observes the real
// filesystem transitions (new/changed/missing) this enum distinguishes.
enum class DocumentLifecycleState { Ready, Processing, Changed, NeedsAttention, Unsupported, Unavailable };

// A real filesystem identity hint for a Referenced document - device/inode on
// POSIX (see DocumentRegistry.cpp) - used only to recognize the same physical
// file after a rename/move within the Project folder. Never sent to the
// frontend (see document_json() in MatterBridge.cpp): it is an internal
// continuity signal, not user-facing identity.
//
// device_id/file_id alone are not sufficient: an OS can reuse a freed inode
// number for a later, entirely unrelated file (a real, documented
// possibility on APFS, not hypothetical). creation_time_token is the
// strongest additional stable per-object signal the platform's stat()
// contract exposes - macOS's st_birthtimespec, the file object's real
// creation time, immutable across rename/move/content edit and different
// for a genuinely new file even when its inode number was just reused.
// Included directly in the identity match key (see
// Document/DocumentRegistry/DocumentRegistry.cpp's reconcile_observed_files()),
// never on its own: unset on a platform whose stat() contract offers no such
// field, which degrades identity to device_id/file_id alone, same as before.
struct FilesystemIdentityHint {
	std::optional<std::uint64_t> device_id;
	std::optional<std::uint64_t> file_id;
	std::optional<std::string> creation_time_token;
};

struct MatterDocument {
  std::string id;
  std::string matter_id;
  std::string name;
  std::string stored_name;
  documents::DocumentFormat file_type = documents::DocumentFormat::unknown;
  DocumentSourceMode source_mode = DocumentSourceMode::Managed;
  std::string added_at;
  // Whose computer first recorded it in the Matter: the seat of the operation
  // that brought it in, as the Project's operations replay
  // (Persistence/ProjectStore). Derived on every load and never stored; empty
  // where no operation says so, as for one kept in the Project's Base.
  std::string added_by_actor_id;
  long long size_bytes = 0;
  std::string last_modified;
  std::string fingerprint;
  DocumentExtractionState extraction_status = DocumentExtractionState::NotExtracted;
  long long extracted_character_count = 0;
  std::optional<long long> page_count;
  std::string extractor_version;

	// Referenced-only (source_mode == DocumentSourceMode::Referenced) fields -
	// see Document/DocumentRegistry/DocumentRegistry.hpp for what maintains
	// them. Left at their defaults for Managed documents.
	std::string relative_path;
	int version = 0;
	DocumentLifecycleState lifecycle_state = DocumentLifecycleState::Processing;
	bool available = true;
	std::string unavailable_since;
	std::optional<std::string> attention_detail;
	FilesystemIdentityHint filesystem_identity;
};

// The document as the lawyer finds it in the Project: its folder and file
// name ("12 Transaction Documents/Draft v3.docx"), or its name where nothing
// else places it. What a prompt names above the document's passages.
[[nodiscard]] inline std::string placed_name(const MatterDocument &document) {
	return document.relative_path.empty() ? document.name : document.relative_path;
}

[[nodiscard]] const char *to_string(DocumentSourceMode mode) noexcept;
[[nodiscard]] const char *to_string(DocumentExtractionState status) noexcept;
[[nodiscard]] const char* to_string(DocumentLifecycleState state) noexcept;
[[nodiscard]] DocumentExtractionState to_document_extraction_state(documents::ExtractionStatus status) noexcept;

// The single Document state vocabulary the frontend renders, regardless of
// source_mode: derived from extraction_status for a Managed document (Phase
// 03's existing lifecycle), or read directly from lifecycle_state/available
// for a Referenced one (Phase 08's real filesystem truth). Never a second
// competing enum - see Sec.9 of phase-08-documents-filesystem-states.md.
[[nodiscard]] DocumentLifecycleState document_lifecycle_state(const MatterDocument& document) noexcept;

} // namespace singularity::matters
