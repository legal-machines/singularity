#pragma once

#include "Matter/Types/Matter.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Where the local table model's tables come from (DESIGN4 §6.2): each tabular
// document a prompt's SOURCE_MATERIAL names, read whole on this computer -
// every sheet with its hidden rows and columns, formulas, saved values and
// merged ranges (XlsxWorkbookView), a Word document's tables
// (extract_docx_tables), or a CSV - within each reader's own safety limits and
// kRowsPerTable. The Document-facing half of ProtectedTables: Papinian never
// reads a file. Declared by the seam (lane S); lane T writes it, and lane K
// binds it on InferenceRequest::protected_table_source.
namespace singularity::routing::protected_table_source {

namespace tables = singularity::protected_api::papinian::tables;

// The tables of the documents `document_ids` names ("DOC_3" handles resolved
// by the caller to Project document ids), for the Project at
// `workspace_path`.
[[nodiscard]] std::vector<tables::TableInput> tables_for(const std::vector<std::string> &document_ids,
	const std::filesystem::path &workspace_path, const matters::Matter &matter);

// The same as InferenceRequest carries it.
using TableSource = std::function<std::vector<tables::TableInput>(const std::vector<std::string> &)>;
[[nodiscard]] TableSource table_source(const std::filesystem::path &workspace_path, const matters::Matter &matter);

} // namespace singularity::routing::protected_table_source
