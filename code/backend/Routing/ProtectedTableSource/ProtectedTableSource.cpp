#include "Routing/ProtectedTableSource/ProtectedTableSource.hpp"

#include "Document/CsvExtractor/CsvExtractor.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "Document/XlsxWorkbookView/XlsxWorkbookView.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"

#include <algorithm>
#include <exception>
#include <system_error>

// The Document-facing half of the local table model: each named tabular
// document read whole, within its reader's own safety limits. Only the
// documents the prompt's SOURCE_MATERIAL names are read (task-minimal); which
// of their sheets and columns the model is shown is decided by ProtectedTables
// from the evidence (DESIGN4 §6.2, D14) - every row stays available locally.
namespace singularity::routing::protected_table_source {
namespace {

// Where a document's file is: a Project folder's own file (checked to stay
// inside the folder, never through a link), or Singularity's managed copy.
std::optional<std::filesystem::path> file_of(const std::filesystem::path &workspace_path, const matters::Matter &matter,
		const matters::MatterDocument &document) {
	std::filesystem::path path;
	if (document.source_mode == matters::DocumentSourceMode::Referenced) {
		if (!matter.directory_path || matter.directory_path->empty()
				|| !documents::registry::is_relative_path_safe(*matter.directory_path, document.relative_path))
			return std::nullopt;
		path = std::filesystem::path(*matter.directory_path) / document.relative_path;
	} else {
		if (workspace_path.empty()) return std::nullopt;
		const auto managed = matters::managed_document_path(workspace_path, matter.id, document.stored_name);
		if (!managed) return std::nullopt;
		path = *managed;
	}
	std::error_code error;
	if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return std::nullopt;
	return path;
}

std::vector<std::vector<std::string>> grid_of(const documents::DocumentTable &table, bool headerRow) {
	std::vector<std::vector<std::string>> grid;
	if (headerRow && !table.column_headers.empty()) grid.push_back(table.column_headers);
	for (const documents::DocumentTableRow &row : table.rows) {
		std::vector<std::string> cells;
		for (const documents::DocumentTableCell &cell : row.cells) {
			if (cells.size() < cell.column_index) cells.resize(cell.column_index);
			cells.push_back(cell.text);
		}
		grid.push_back(std::move(cells));
		if (grid.size() > singularity::protected_api::papinian::compute::kRowsPerTable + 1) break;
	}
	return grid;
}

} // namespace

std::vector<tables::TableInput> tables_for(const std::vector<std::string> &document_ids,
		const std::filesystem::path &workspace_path, const matters::Matter &matter) {
	std::vector<tables::TableInput> inputs;
	std::vector<std::string> seen;
	for (const std::string &id : document_ids) {
		if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
		seen.push_back(id);
		const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == id; });
		if (document == matter.documents.end() || !document->available) continue;
		const documents::DocumentFormat format = document->file_type;
		if (format != documents::DocumentFormat::xlsx && format != documents::DocumentFormat::docx
				&& format != documents::DocumentFormat::csv && format != documents::DocumentFormat::tsv)
			continue;
		const std::optional<std::filesystem::path> path = file_of(workspace_path, matter, *document);
		if (!path) continue;
		tables::TableInput input;
		input.document_id = document->id;
		input.title = document->relative_path.empty() ? document->name : document->relative_path;
		const documents::DocumentExtractionRequest request{*path, format};
		// A file that cannot be read gives no table; the prompt's own passages
		// still hold what was retrieved (tables_in_passages).
		try {
			if (format == documents::DocumentFormat::xlsx) {
				documents::XlsxWorkbookViewResult view = documents::read_xlsx_workbook_view(request);
				if (view.status != documents::ExtractionStatus::ready) continue;
				input.workbook = std::move(view.workbook);
			} else if (format == documents::DocumentFormat::docx) {
				std::size_t index = 0;
				for (const documents::DocumentTable &table : documents::extract_docx_tables(request)) {
					tables::WordTable word;
					word.table_id = table.table_id.empty() ? "table-" + std::to_string(index) : table.table_id;
					word.rows = grid_of(table, false);
					++index;
					if (word.rows.size() >= 2) input.word_tables.push_back(std::move(word));
				}
				if (input.word_tables.empty()) continue;
			} else {
				const documents::DocumentTable table = format == documents::DocumentFormat::csv
					? documents::extract_csv_table(request) : documents::extract_tsv_table(request);
				input.csv = grid_of(table, true);
				if (input.csv.size() < 2) continue;
			}
		} catch (const std::exception &) {
			continue;
		}
		inputs.push_back(std::move(input));
	}
	return inputs;
}

TableSource table_source(const std::filesystem::path &workspace_path, const matters::Matter &matter) {
	return [workspace_path, matter](const std::vector<std::string> &document_ids) {
		return tables_for(document_ids, workspace_path, matter);
	};
}

} // namespace singularity::routing::protected_table_source
