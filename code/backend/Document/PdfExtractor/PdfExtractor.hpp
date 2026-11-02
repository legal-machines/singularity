#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// Platform PDF adapter. Implementations must remain local-only and return page
// offsets into the normalized UTF-8 result text.
[[nodiscard]] DocumentExtractionResult
extract_pdf_document(const DocumentExtractionRequest &request);

[[nodiscard]] std::string pdf_extractor_version();

// Whether this computer can read a scanned page's words (D-114): on Windows,
// when Windows has a text recognition language; elsewhere, not yet.
[[nodiscard]] bool pdf_text_recognition_available();

// A picture file's words - a photographed page, a scan saved as PNG, JPEG or
// TIFF - read by the same text recognition (D-115). Nothing where this computer
// cannot read them, or where the picture holds no words.
[[nodiscard]] std::optional<std::string> recognize_image_text(const std::filesystem::path &path);

// ---- Work on a PDF, always in a copy (D-115) ---------------------------------------
//
// A passage of a PDF a colleague marks: its words as the page holds them, found
// as a lawyer reads them (Platform/PhraseMatch: typography, spacing and case
// never decide, the words do), on its page (0: wherever they stand once), and
// the colleague's note on it.
struct PdfMark {
	unsigned int page = 0;
	std::string quote;
	std::string comment;
};

struct PdfMarkOutcome {
	// "written", "not_found" or "ambiguous".
	std::string state;
	unsigned int page = 0;
	std::string note;
};

struct PdfRevision {
	bool is_written = false;
	std::string problem;
	std::filesystem::path path;
	std::size_t written = 0;
	std::vector<PdfMarkOutcome> outcomes;
};

// An annotated copy: each passage highlighted where it stands, with the note
// beside it as a reader's Comment tool makes one, under `author`'s name - real
// highlight annotations any PDF reader shows, lists and lets the lawyer delete.
// The pages themselves are untouched. Read back before it is kept: every
// highlight on its page with its note, and every page's text as the
// original's. `target` must not exist; nothing is left there on failure.
[[nodiscard]] PdfRevision write_annotated_pdf(const std::filesystem::path &source, const std::filesystem::path &target,
	const std::vector<PdfMark> &marks, const std::string &author);

// A redacted copy: each passage taken out of the page itself, not covered. A
// page with a redaction is drawn again as an image of itself with the passages
// blacked out, its text, drawings and the annotations over a passage removed,
// so no tool can read or copy what was redacted; every other page is as it
// was. Read back before it is kept: none of the redacted passages stands in the
// copy's text any more, the copy has the original's pages, and every page
// without a redaction reads as before. On a page redacted this way the other
// words are part of its image too, and can no longer be searched.
[[nodiscard]] PdfRevision write_redacted_pdf(const std::filesystem::path &source, const std::filesystem::path &target,
	const std::vector<PdfMark> &marks);

} // namespace singularity::documents
