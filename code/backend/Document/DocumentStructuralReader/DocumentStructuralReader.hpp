#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/DocumentRenderTypes.hpp"
#include "Document/Types/DocumentSemantics.hpp"

namespace singularity::documents {

// The one format-neutral entry point every Document Intelligence consumer
// (generic document actions, DocumentPatchValidator, DocumentPatchTool) reads
// a DocumentSemanticModel through - never a format check plus a direct call
// into one specific extractor. Dispatches by the same detected/hinted format
// LocalDocumentExtractor already resolves, then adds the structural view
// (tables/slides/sheets) the matching format-specific reader produces on top
// of it. A format with no structural reader (plain_text/markdown/pdf/unknown)
// still gets paragraphs/headings and, for PDF, page boundaries - see
// DocumentSemanticModel's own comment on why PDF stays page/offset-addressed
// rather than gaining a synthetic table model.
[[nodiscard]] DocumentSemanticModel read_document_structure(const DocumentExtractionRequest &request);

// The one entry point for the reading a PERSON looks at, for every format the
// Project holds. A Word document reads with its own layout intact (see
// DocxExtractor::render_docx_document); a spreadsheet - an .xlsx, a .csv, a
// .tsv - reads as its workbook (XlsxWorkbookView; a delimited file as its one
// sheet, header row included), with no blocks; a slide deck reads as the tables
// and blocks its own adapter already produces; everything
// else reads as its text, and says so in `limitation` rather than pretending
// to a layout Singularity never read. Never written back, never resolved against -
// a change is still validated against read_document_structure() above.
[[nodiscard]] DocumentRenderModel read_document_render_model(const DocumentExtractionRequest &request);

// Marks what the last change did, by comparing this reading with a reading of
// the version Singularity kept before it wrote. A paragraph the previous version
// did not have is Added; one that stood in the same place saying something else
// is Changed and carries what it said; a table row that is new is Added, and a
// cell whose text moved is Changed. What the previous version held and this one
// does not becomes `removed_texts`. A workbook is compared cell by cell - by
// sheet name and reference, on the text each cell shows - and what a sheet no
// longer has becomes that sheet's `removed_cells`.
//
// It is a comparison of what the document SAYS, nothing more: formatting is not
// diffed, and nothing is inferred about why a change was made. `current` is
// left entirely alone when there is nothing to compare - every mark then stays
// Unknown, which is what a viewer must show rather than "unchanged".
void mark_document_changes(DocumentRenderModel &current, const DocumentRenderModel &previous,
	int previous_version);

} // namespace singularity::documents
