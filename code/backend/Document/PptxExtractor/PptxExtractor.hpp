#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/DocumentSemantics.hpp"

namespace singularity::documents {

// Extracts a PPTX package entirely in memory, the same safety posture as
// DocxExtractor.cpp: only the DrawingML parts needed for visible slide text
// and tables are read; embedded media, animations and external resources
// are never resolved. PPTX is a required golden-path methodology source
// (phase-24-1-research-document-intelligence-project-actions.md) but never a
// patch target in Phase 24.1C - there is no apply_pptx_*_patch function here.

[[nodiscard]] DocumentExtractionResult extract_pptx_document(const DocumentExtractionRequest &request);
[[nodiscard]] const char *pptx_extractor_version() noexcept;

// Structural read: every slide in real presentation order (resolved through
// ppt/presentation.xml's slide id list and ppt/_rels/presentation.xml.rels -
// never assumed from slideN.xml file-name order), each with its text blocks
// (one per shape containing a text body) and any real DrawingML tables
// (<a:tbl>) it contains.
[[nodiscard]] std::vector<DocumentSlide> extract_pptx_slides(const DocumentExtractionRequest &request);

// The package's slide parts ("ppt/slides/slide3.xml"), in presentation order,
// resolved as extract_pptx_slides() resolves them. Empty when the package
// cannot be read.
[[nodiscard]] std::vector<std::string> pptx_slide_part_names(const std::filesystem::path &path);

} // namespace singularity::documents
