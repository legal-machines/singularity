#pragma once

#include "WorkProduct/ReviewTable/ReviewTable.hpp"

#include <string>
#include <vector>

namespace singularity::review_table_export {

/*
 * A Review Table as files a lawyer sends on: a real Excel workbook - a
 * "Review" sheet with a row per document and a column per question, and an
 * "Evidence" sheet with every quote, where it stands and whether Singularity
 * found it in the document - and a Word document holding the same table and
 * its evidence. The bytes of the file.
 */
std::string xlsx(const matters::ReviewTable &table, const std::vector<matters::ReviewRow> &rows,
	const std::vector<review_table::RowStatus> &statuses);
std::string docx(const matters::ReviewTable &table, const std::vector<matters::ReviewRow> &rows,
	const std::vector<review_table::RowStatus> &statuses);

// What a cell says in a file: its answer, "Not addressed", or why there is none.
std::string cell_text(const matters::ReviewCell *cell, const std::string &row_status);

} // namespace singularity::review_table_export
