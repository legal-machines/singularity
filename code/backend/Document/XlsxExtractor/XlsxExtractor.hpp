#pragma once

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/Types/DocumentSemantics.hpp"
#include "Document/Types/SheetRenderTypes.hpp"

#include <string>

namespace singularity::documents {

// The model's reading of a workbook, through the one workbook engine
// (XlsxWorkbookView): every sheet in workbook order, one line per row - "Row 12:
// A12 = Adjusted EBITDA | B12 = 3000000 (shown in $) {=B10+B11}" - numbers as the
// workbook stores them with what their format makes of them in words, dates as
// dates, formulas in braces - with the hidden rows and columns a formula counts
// ("{SUM(I4:I11)} (counts hidden row 6)") - hidden rows, columns and sheets said
// to be hidden,
// and every passage opened by its sheet, its rows and the sheet's header row
// (a heading, so the chunker starts a passage there and a citation names the
// rows). Read-only; cell updates are written by ProfessionalArtifactAction.cpp's
// verified XLSX writer (see ArtifactCapabilityRegistry.hpp for its limits).
[[nodiscard]] DocumentExtractionResult extract_xlsx_document(const DocumentExtractionRequest &request);
[[nodiscard]] const char *xlsx_extractor_version() noexcept;

// The reading passages cited before 24 Sep 2026 were cut from - "A1<tab>value"
// per cell, stored values only - exactly as it was, so such a citation still
// shows the very passage it was given (DocumentSourceResolver). Never read for
// new work.
[[nodiscard]] DocumentExtractionResult extract_xlsx_cells_reading(const DocumentExtractionRequest &request);
[[nodiscard]] const char *xlsx_cells_reading_version() noexcept;

// The workbook reading passages were cited from on 24 Sep 2026, before a
// formula said which hidden rows and columns it counts - exactly as it was, so
// such a citation still shows the very passage it was given. Never read for
// new work.
[[nodiscard]] DocumentExtractionResult extract_xlsx_workbook_v2_reading(const DocumentExtractionRequest &request);
[[nodiscard]] const char *xlsx_workbook_v2_reading_version() noexcept;

// One cell as the model reads it ("3000000 (shown in $)", "November 30, 2026"),
// and as Excel displays it ("$3,000,000") - what a model may quote of a cell.
[[nodiscard]] std::string xlsx_cell_value_reading(const SheetRenderCell &cell);
[[nodiscard]] std::string xlsx_cell_display(const SheetRenderCell &cell);

// Structural read: every worksheet in real workbook order (resolved through
// xl/workbook.xml's sheet list and xl/_rels/workbook.xml.rels - never
// assumed from sheetN.xml file-name order), each as cell_ref -> text pairs.
// A blank cell is simply absent, never a synthesized empty entry.
[[nodiscard]] std::vector<DocumentSheet> extract_xlsx_sheets(const DocumentExtractionRequest &request);

} // namespace singularity::documents
