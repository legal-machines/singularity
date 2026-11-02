#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/DocumentPatchTypes.hpp"
#include "Document/Types/DocumentSemantics.hpp"

#include <filesystem>
#include <functional>

namespace singularity::documents {

// A minimal RFC4180-style CSV reader/writer: comma-separated fields, double
// quotes for a field containing a comma/quote/newline, "" as an escaped
// quote. No dialect detection - the one delimiter/quoting convention every
// Singularity-authored CSV export already uses.

[[nodiscard]] DocumentExtractionResult extract_csv_document(const DocumentExtractionRequest &request);
[[nodiscard]] DocumentExtractionResult extract_tsv_document(const DocumentExtractionRequest &request);
[[nodiscard]] const char *csv_extractor_version() noexcept;
[[nodiscard]] const char *tsv_extractor_version() noexcept;

// Structural read: the whole CSV as one DocumentTable (table_id "csv"), first
// row as headers, row_key from derive_table_row_key() on each row's first
// column.
[[nodiscard]] DocumentTable extract_csv_table(const DocumentExtractionRequest &request);
[[nodiscard]] DocumentTable extract_tsv_table(const DocumentExtractionRequest &request);

struct CsvPatchApplyResult {
	bool ok = false;
	DocumentPatchBlocker blocker = DocumentPatchBlocker::None;
	std::string detail;
};

// Real, narrow CSV cell mutation. Re-reads the live file, re-resolves every
// operation's locator and re-checks expected_old_value itself (defensive:
// the caller already validated against a model read from the same bytes,
// but the physical writer never trusts that alone), writes only the changed
// fields into a temporary file, re-parses that temporary file to confirm it
// is still well-formed CSV with the same row/column count, then atomically
// replaces `documentPath`. Never touches the file at all when any operation
// fails to resolve or match.
// `verify_staged`, when given, is asked about the fully written staged file
// before it replaces `documentPath`; a false answer leaves the file untouched.
[[nodiscard]] CsvPatchApplyResult apply_csv_table_cell_patch(
		const std::filesystem::path &documentPath, const DocumentPatch &patch,
		const std::function<bool(const std::filesystem::path &)> &verify_staged = {});

} // namespace singularity::documents
