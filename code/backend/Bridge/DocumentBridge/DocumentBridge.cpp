#include "Bridge/DocumentBridge/DocumentBridge.hpp"

#include "Bridge/MatterBridge/MatterBridge.hpp"
#include "Document/DocumentActions/DocumentActions.hpp"
#include "Document/DocumentFileActions/DocumentFileActions.hpp"
#include "Document/DocumentSourceResolver/DocumentSourceResolver.hpp"
#include "Document/ProfessionalArtifactAction/ProfessionalArtifactAction.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Privacy/PrivacyActions/PrivacyActions.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>

namespace singularity::bridge::document_bridge {
namespace {

using Json = nlohmann::json;

const char* to_string(document::SourceResolutionStatus status) {
  switch (status) {
    case document::SourceResolutionStatus::Resolved: return "resolved";
    case document::SourceResolutionStatus::Changed: return "changed";
    case document::SourceResolutionStatus::Unavailable: return "unavailable";
  }
  return "unavailable";
}

} // namespace

void validate_import(const nlohmann::json &params) {
  document::require_matter(params.at("matterId").get<std::string>());
}

nlohmann::json import_files(const nlohmann::json &params,
                            const std::vector<std::string> &selected_paths) {
  return matter_bridge::serialize(document::import_files(
      params.at("matterId").get<std::string>(), selected_paths));
}

nlohmann::json remove(const nlohmann::json &params) {
  return matter_bridge::serialize(document::remove(
      params.at("matterId").get<std::string>(),
      params.at("documentId").get<std::string>()));
}

nlohmann::json reveal(const nlohmann::json &params) {
  return document::reveal(params.at("matterId").get<std::string>(),
                          params.at("documentId").get<std::string>());
}

nlohmann::json open(const nlohmann::json& params) {
  return document::open(params.at("matterId").get<std::string>(),
                        params.at("documentId").get<std::string>());
}

// Returns the full Matter, Managed (Phase 03 explicit copy-in, still real
// and still working via import_files()/remove()/reveal() above) and
// Referenced (Phase 08 real-folder) documents both included - the same
// generic "everything on this Matter" shape every other matter.* method
// already returns, useful to more than one future caller. Managed is
// compatibility-only for the Phase 08 Documents surface specifically, never
// equivalent canonical real-folder material: it lives in a separate
// .singularity-workspace/documents/<matterId>/ copy directory, not inside
// matter.directoryPath, so a Managed document's own relativePath is always
// empty (MatterRepository.cpp's document_from_json() default) rather than a
// real location under the Project folder. The one current consumer of this
// method - App/services/documents.js's fetchProjectDocuments() - filters to
// sourceMode "referenced" before treating the result as "what's in this
// Project folder"; a future caller that means to enumerate every managed +
// referenced document for some other purpose should keep both.
nlohmann::json list_documents(const nlohmann::json& params) {
	const std::string matterId = params.at("matterId").get<std::string>();
	const matters::Matter result = matter::list_documents(matterId);

	// Phase 10 (Privacy - entities and Project coverage): once real Document
	// extraction is current, this Project's own local protection
	// preprocessing runs over whatever just became AI-ready or changed - its
	// own separate, already-persisted operation (Privacy/PrivacyActions/
	// PrivacyActions.cpp), never part of this method's own return shape. It is
	// queued, not run here: scoring a new data room takes minutes, and this
	// list must answer now. A missing/unready Papinian model degrades Privacy
	// truthfully (a missing protection model never blocks Matters or local
	// work); the queued pass persists an Unsupported protection
	// state and recovery detail when Papinian itself is unavailable.
	try {
		privacy_actions::request_preprocessing(matterId, /*in_use=*/true);
	} catch (const std::exception &) {
	}

	return matter_bridge::serialize(result);
}

nlohmann::json rename_document(const nlohmann::json& params) {
  return matter_bridge::serialize(document::rename_document(params.at("matterId").get<std::string>(),
      params.at("documentId").get<std::string>(), params.at("expectedVersion").get<int>(),
      params.at("newName").get<std::string>()));
}

nlohmann::json move_document(const nlohmann::json& params) {
  return matter_bridge::serialize(document::move_document(params.at("matterId").get<std::string>(),
      params.at("documentId").get<std::string>(), params.at("expectedVersion").get<int>(),
      params.at("destinationFolderRelativePath").get<std::string>()));
}

// Only ever resolves once the file genuinely reached the OS Trash -
// document::remove_document() throws instead of returning when Trash is
// unsupported or the OS call fails, so this never has a "permanent delete"
// shape to report.
nlohmann::json delete_document(const nlohmann::json& params) {
  return matter_bridge::serialize(document::remove_document(params.at("matterId").get<std::string>(),
      params.at("documentId").get<std::string>(), params.at("expectedVersion").get<int>()));
}

nlohmann::json create_folder(const nlohmann::json& params) {
  return matter_bridge::serialize(document::create_folder(params.at("matterId").get<std::string>(),
      params.at("parentFolderRelativePath").get<std::string>(), params.at("folderName").get<std::string>()));
}

nlohmann::json add_external_file(const nlohmann::json& params) {
  return matter_bridge::serialize(document::add_external_file(params.at("matterId").get<std::string>(),
      params.at("destinationFolderRelativePath").get<std::string>(),
      params.at("sourceAbsolutePath").get<std::string>(), params.value("move", false), params.value("keepBoth", false)));
}

nlohmann::json add_external_folder(const nlohmann::json& params) {
  return matter_bridge::serialize(document::add_external_folder(params.at("matterId").get<std::string>(),
      params.at("destinationFolderRelativePath").get<std::string>(), params.at("sourceAbsolutePath").get<std::string>()));
}

nlohmann::json reprocess_document(const nlohmann::json& params) {
  return matter_bridge::serialize(
      matter::reprocess_document(params.at("matterId").get<std::string>(), params.at("documentId").get<std::string>()));
}

nlohmann::json resolve_document_source(const nlohmann::json& params) {
  const auto chunk_id = params.value("chunkId", std::string());
  // The reading the citation recorded; absent for one recorded before
  // readings were kept.
  const std::string reading = params.contains("extractorVersion") && params.at("extractorVersion").is_string()
      ? params.at("extractorVersion").get<std::string>() : std::string();
  const auto resolved = document::resolve_source(
      params.at("matterId").get<std::string>(), params.at("documentId").get<std::string>(), chunk_id, reading);
  return Json{
      {"status", to_string(resolved.status)},
      {"documentId", resolved.document_id},
      {"documentName", resolved.document_name},
      {"currentVersion", resolved.current_version},
      {"chunkId", resolved.chunk_id},
      {"page", resolved.page ? Json(*resolved.page) : Json(nullptr)},
      {"heading", resolved.heading ? Json(*resolved.heading) : Json(nullptr)},
      {"textPreview", resolved.text_preview},
  };
}


namespace {

Json optional_twips(const std::optional<int> &value) {
  return value ? Json(*value) : Json(nullptr);
}

// One of the document's own tracked changes: who made it, when, and whether
// it put words in, took them out or wrote them differently - and, for the
// last, what it did. null for text that simply stands.
Json serialize_revision(const std::optional<documents::DocumentRenderRevision> &revision) {
  if (!revision) return Json(nullptr);
  const char *kind = revision->kind == documents::DocumentRenderRevisionKind::Deleted ? "deleted"
      : revision->kind == documents::DocumentRenderRevisionKind::Formatted ? "formatted" : "inserted";
  return Json{
      {"kind", kind},
      {"id", revision->id},
      {"author", revision->author},
      {"date", revision->date},
      {"dateUtc", revision->date_utc},
      {"changes", revision->changes},
      // For a formatting change, how the words were written before it - what
      // the page drawn as it stood, every change rejected, shows.
      {"wasRun", revision->was_run ? Json{
          {"bold", revision->was_run->bold},
          {"italic", revision->was_run->italic},
          {"underline", revision->was_run->underline},
          {"strikethrough", revision->was_run->strikethrough},
          {"caps", revision->was_run->caps},
          {"smallCaps", revision->was_run->small_caps},
          {"vertAlign", revision->was_run->vert_align},
      } : Json(nullptr)},
      {"wasAlignment", revision->was_alignment ? Json(*revision->was_alignment) : Json(nullptr)},
  };
}

Json serialize_runs(const std::vector<documents::DocumentRenderRun> &runs) {
  Json result = Json::array();
  for (const documents::DocumentRenderRun &run : runs) {
    result.push_back(Json{
        {"text", run.text},
        {"bold", run.bold},
        {"italic", run.italic},
        {"underline", run.underline},
        {"strikethrough", run.strikethrough},
        {"colorHex", run.color_hex},
        {"highlight", run.highlight},
        {"halfPoints", run.half_points},
        {"isNoteReference", run.is_note_reference},
        {"revision", serialize_revision(run.revision)},
        {"formatRevision", serialize_revision(run.format_revision)},
        {"vertAlign", run.vert_align},
        {"caps", run.caps},
        {"smallCaps", run.small_caps},
        {"noteId", run.note_id},
        {"noteNumber", run.note_number},
        {"commentIds", run.comment_ids},
        {"link", run.link},
        {"pageBreak", run.page_break},
    });
  }
  return result;
}

const char *to_string(documents::DocumentRenderChangeMark mark) {
  switch (mark) {
    case documents::DocumentRenderChangeMark::Unchanged: return "unchanged";
    case documents::DocumentRenderChangeMark::Added: return "added";
    case documents::DocumentRenderChangeMark::Changed: return "changed";
    case documents::DocumentRenderChangeMark::Unknown: return "unknown";
  }
  return "unknown";
}

Json serialize_paragraph(const documents::DocumentRenderParagraph &paragraph) {
  return Json{
      {"headingLevel", paragraph.heading_level ? Json(*paragraph.heading_level) : Json(nullptr)},
      {"styleId", paragraph.style_id},
      {"alignment", paragraph.alignment},
      {"indentLevel", paragraph.indent_level},
      {"inList", paragraph.in_list},
      {"listId", paragraph.list_id},
      {"listLevel", paragraph.list_level},
      {"listBullet", paragraph.list_bullet},
      {"listMarker", paragraph.list_marker},
      {"indentLeftTwips", optional_twips(paragraph.indent_left_twips)},
      {"indentFirstLineTwips", optional_twips(paragraph.indent_first_line_twips)},
      {"indentRightTwips", optional_twips(paragraph.indent_right_twips)},
      {"spacingBeforeTwips", optional_twips(paragraph.spacing_before_twips)},
      {"spacingAfterTwips", optional_twips(paragraph.spacing_after_twips)},
      {"pageBreakBefore", paragraph.page_break_before},
      {"keepNext", paragraph.keep_next},
      {"isTitle", paragraph.is_title},
      {"revision", serialize_revision(paragraph.revision)},
      {"formatRevision", serialize_revision(paragraph.format_revision)},
      {"change", to_string(paragraph.change)},
      {"previousText", paragraph.previous_text},
      {"runs", serialize_runs(paragraph.runs)},
  };
}

Json serialize_paragraphs(const std::vector<documents::DocumentRenderParagraph> &paragraphs) {
  Json result = Json::array();
  for (const documents::DocumentRenderParagraph &paragraph : paragraphs) result.push_back(serialize_paragraph(paragraph));
  return result;
}

Json serialize_blocks(const std::vector<documents::DocumentRenderBlock> &blocks);

Json serialize_table(const documents::DocumentRenderTable &table) {
  Json rows = Json::array();
  for (const documents::DocumentRenderRow &row : table.rows) {
    Json cells = Json::array();
    for (const documents::DocumentRenderCell &cell : row.cells) {
      cells.push_back(Json{
          {"columnIndex", cell.column_index},
          {"gridSpan", cell.grid_span},
          {"rowSpan", cell.row_span},
          {"isMergeContinuation", cell.is_merge_continuation},
          {"shadingFillHex", cell.shading_fill_hex},
          {"change", to_string(cell.change)},
          {"previousText", cell.previous_text},
          {"paragraphs", serialize_paragraphs(cell.paragraphs)},
          // The cell's whole content in order, a table inside it included.
          {"blocks", serialize_blocks(cell.blocks)},
      });
    }
    rows.push_back(Json{{"rowIndex", row.row_index}, {"isHeader", row.is_header},
        {"revision", serialize_revision(row.revision)},
        {"change", to_string(row.change)}, {"cells", cells}});
  }
  return Json{
      {"tableId", table.table_id},
      {"columnWidthsTwips", table.column_widths_twips},
      {"hasBorders", table.has_borders},
      {"rows", rows},
  };
}

Json serialize_blocks(const std::vector<documents::DocumentRenderBlock> &blocks) {
  Json result = Json::array();
  for (const documents::DocumentRenderBlock &block : blocks) {
    if (block.kind == documents::DocumentRenderBlockKind::Table) {
      result.push_back(Json{{"kind", "table"}, {"table", serialize_table(block.table)}});
      continue;
    }
    result.push_back(Json{{"kind", "paragraph"}, {"paragraph", serialize_paragraph(block.paragraph)}});
  }
  return result;
}

// The page the document is set on, in twips; null where it does not say.
Json serialize_page(const std::optional<documents::DocumentRenderPage> &page) {
  if (!page) return Json(nullptr);
  return Json{
      {"widthTwips", page->width_twips},
      {"heightTwips", page->height_twips},
      {"marginTopTwips", page->margin_top_twips},
      {"marginRightTwips", page->margin_right_twips},
      {"marginBottomTwips", page->margin_bottom_twips},
      {"marginLeftTwips", page->margin_left_twips},
      {"orientation", page->orientation},
  };
}

Json serialize_notes(const std::vector<documents::DocumentRenderNote> &notes) {
  Json result = Json::array();
  for (const documents::DocumentRenderNote &note : notes) {
    result.push_back(Json{
        {"id", note.id},
        {"kind", note.kind == documents::DocumentRenderNoteKind::Endnote ? "endnote" : "footnote"},
        {"number", note.number},
        {"paragraphs", serialize_paragraphs(note.paragraphs)},
    });
  }
  return result;
}

Json serialize_comments(const std::vector<documents::DocumentRenderComment> &comments) {
  Json result = Json::array();
  for (const documents::DocumentRenderComment &comment : comments) {
    result.push_back(Json{
        {"id", comment.id},
        {"author", comment.author},
        {"initials", comment.initials},
        {"date", comment.date},
        {"paragraphs", serialize_paragraphs(comment.paragraphs)},
    });
  }
  return result;
}

Json optional_number(const std::optional<double> &value) {
  return value && std::isfinite(*value) ? Json(*value) : Json(nullptr);
}

// A cell's raw value by its kind: a number (a date stays its serial number), the
// text of a text or an error cell, a boolean - null for an empty cell.
Json cell_value(const documents::SheetRenderCell &cell) {
  switch (cell.kind) {
    case documents::SheetCellKind::Number:
    case documents::SheetCellKind::Date:
      return std::isfinite(cell.number) ? Json(cell.number) : Json(nullptr);
    case documents::SheetCellKind::Text:
    case documents::SheetCellKind::Error:
      return Json(cell.raw_text);
    case documents::SheetCellKind::Boolean:
      return Json(cell.boolean);
    case documents::SheetCellKind::Empty:
      break;
  }
  return Json(nullptr);
}

Json serialize_cell_style(const documents::SheetCellStyle &style) {
  return Json{
      {"bold", style.bold},
      {"italic", style.italic},
      {"underline", style.underline},
      {"strike", style.strike},
      {"fontColor", style.font_color_hex},
      {"fillColor", style.fill_color_hex},
      {"horizontal", style.horizontal},
      {"vertical", style.vertical},
      {"wrap", style.wrap},
      {"indent", style.indent},
      {"fontSizePoints", optional_number(style.font_size_points)},
      {"borders", Json{
          {"top", style.borders.top},
          {"right", style.borders.right},
          {"bottom", style.borders.bottom},
          {"left", style.borders.left},
      }},
  };
}

Json serialize_sheet_cell(const documents::SheetRenderCell &cell) {
  return Json{
      {"column", cell.column},
      {"ref", cell.ref},
      {"type", documents::sheet_cell_kind_name(cell.kind)},
      {"value", cell_value(cell)},
      {"text", cell.text},
      {"fill", cell.fill ? Json{{"before", cell.fill->before}, {"after", cell.fill->after}} : Json(nullptr)},
      {"color", cell.color_hex},
      {"formula", cell.formula ? Json(*cell.formula) : Json(nullptr)},
      {"numberFormat", cell.number_format},
      {"style", serialize_cell_style(cell.style)},
      {"change", documents::sheet_cell_change_name(cell.change)},
      {"previousText", cell.previous_text},
  };
}

Json serialize_sheet(const documents::SheetRender &sheet) {
  Json columns = Json::array();
  for (const documents::SheetRenderColumn &column : sheet.columns) {
    columns.push_back(Json{
        {"first", column.first},
        {"last", column.last},
        {"width", optional_number(column.width_characters)},
        {"hidden", column.hidden},
    });
  }
  Json rows = Json::array();
  for (const documents::SheetRenderRow &row : sheet.rows) {
    Json cells = Json::array();
    for (const documents::SheetRenderCell &cell : row.cells) cells.push_back(serialize_sheet_cell(cell));
    rows.push_back(Json{
        {"index", row.index},
        {"height", optional_number(row.height_points)},
        {"hidden", row.hidden},
        {"cells", std::move(cells)},
    });
  }
  Json merges = Json::array();
  for (const documents::SheetRenderMerge &merge : sheet.merges) {
    merges.push_back(Json{
        {"firstRow", merge.first_row},
        {"firstColumn", merge.first_column},
        {"lastRow", merge.last_row},
        {"lastColumn", merge.last_column},
    });
  }
  Json removedCells = Json::array();
  for (const documents::SheetRemovedCell &removed : sheet.removed_cells)
    removedCells.push_back(Json{{"ref", removed.ref}, {"previousText", removed.previous_text}});
  return Json{
      {"name", sheet.name},
      {"state", documents::sheet_visibility_name(sheet.visibility)},
      {"rowCount", sheet.row_count},
      {"columnCount", sheet.column_count},
      {"frozenRows", sheet.frozen_rows},
      {"frozenColumns", sheet.frozen_columns},
      {"showGridLines", sheet.show_grid_lines},
      {"defaultColumnWidth", sheet.default_column_width},
      {"defaultRowHeight", sheet.default_row_height},
      {"columns", std::move(columns)},
      {"rows", std::move(rows)},
      {"merges", std::move(merges)},
      {"removedCells", std::move(removedCells)},
  };
}

// A workbook - an .xlsx, a .csv, a .tsv - as the grid shows it; null for every
// other format and for a workbook that could not be read at all.
Json serialize_workbook(const std::optional<documents::SheetWorkbookRender> &workbook) {
  if (!workbook) return Json(nullptr);
  Json sheets = Json::array();
  for (const documents::SheetRender &sheet : workbook->sheets) sheets.push_back(serialize_sheet(sheet));
  return Json{
      {"date1904", workbook->date1904},
      {"activeSheet", workbook->active_sheet},
      {"sheets", std::move(sheets)},
  };
}

} // namespace

// The document as its author laid it out, for a lawyer to look at inside Singularity.
// Read-only and read fresh: nothing here reconciles, mutates or caches.
nlohmann::json read_document_view(const nlohmann::json &params) {
  const document::DocumentDisplayReading reading = document::read_for_display(
      params.at("matterId").get<std::string>(), params.at("documentId").get<std::string>());
  return Json{
      {"documentId", reading.document_id},
      {"documentName", reading.document_name},
      {"version", reading.version},
      {"fingerprint", reading.fingerprint},
      {"format", documents::to_string(reading.model.format)},
      {"status", reading.model.status == documents::ExtractionStatus::ready ? "read" : "unread"},
      {"limitation", reading.model.limitation},
      // What the last change did, and against which version - 0 when Singularity
      // has never changed this document and every mark is "unknown".
      {"comparedToVersion", reading.model.compared_to_version},
      {"removedTexts", reading.model.removed_texts},
      {"blocks", serialize_blocks(reading.model.blocks)},
      // A Word document's page, the header and footer on every page, its
      // notes and its comments - null and empty for every other format.
      {"page", serialize_page(reading.model.page)},
      {"header", serialize_paragraphs(reading.model.header)},
      {"footer", serialize_paragraphs(reading.model.footer)},
      {"notes", serialize_notes(reading.model.notes)},
      {"comments", serialize_comments(reading.model.comments)},
      // A spreadsheet's workbook, sheet by sheet and cell by cell - `blocks`
      // is then empty; null for every other format.
      {"workbook", serialize_workbook(reading.model.workbook)},
  };
}

nlohmann::json list_versions(const nlohmann::json &params) {
	const documents::DocumentVersions versions = documents::list_document_versions(
		params.at("projectId").get<std::string>(), params.at("documentId").get<std::string>());
	Json kept = Json::array();
	for (const documents::KeptDocumentVersion &version : versions.kept)
		kept.push_back(Json{
			{"version", version.version},
			{"fingerprint", version.fingerprint},
			{"keptAt", version.kept_at.empty() ? Json(nullptr) : Json(version.kept_at)},
			{"isOriginal", version.is_original},
		});
	return Json{
		{"documentId", versions.document_id},
		{"documentName", versions.document_name},
		{"currentVersion", versions.current_version},
		{"currentFingerprint", versions.current_fingerprint},
		{"versions", std::move(kept)},
	};
}

nlohmann::json restore_version(const nlohmann::json &params) {
	const documents::RestoredDocumentVersion restored = documents::restore_document_version(
		params.at("projectId").get<std::string>(), params.at("documentId").get<std::string>(),
		params.at("version").get<int>(), params.at("expectedFingerprint").get<std::string>());
	return Json{
		{"documentId", restored.document_id},
		{"restoredVersion", restored.restored_version},
		{"newVersion", restored.new_version},
		{"newFingerprint", restored.new_fingerprint},
	};
}

} // namespace singularity::bridge::document_bridge
