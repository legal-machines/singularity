#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "Document/Types/DocumentPatchTypes.hpp"
#include "Document/Types/DocumentSemantics.hpp"

#include <nlohmann/json.hpp>

namespace singularity::documents {

// One step of a change to a document the Project already has, as a person
// reads it before approving it and again after it was written: what it does,
// what it acts on, the words that go and the words that come. Built from the
// change itself and, where the change names a table cell or row rather than
// quoting it, from what the live document holds there - never guessed.
//
//   replace     a passage becomes other words (removed -> added)
//   delete      a passage goes (removed)
//   insert      new text stands before or after a passage, or at the end
//               (added, anchor, placement)
//   cell        one table cell becomes other words (row, column, removed -> added)
//   row_insert  a row joins a table (columns, cells)
//   row_delete  a row leaves a table (row, columns, cells as it reads now)
//   format      the same words, written differently (anchor or row/column,
//               formatting in plain words)
struct DocumentChangeStep {
	std::string kind;
	std::string removed_text;
	std::string added_text;
	// For a replace read against the document: the words at either end that
	// stay as they are, so the step marks only what Word will mark.
	std::string kept_before;
	std::string kept_after;
	std::string anchor_text;
	std::string placement;
	std::string table_id;
	std::string row;
	std::string column;
	std::vector<std::string> columns;
	std::vector<std::string> cells;
	std::vector<std::string> formatting;
	// For a cell change: the table as it stands, row by row (the heading row
	// first where the table has one), and where the changed cell is in it - so
	// the change is shown inside its own table, the way Word shows a tracked
	// change in a cell. Empty for a table too large to show whole.
	std::vector<std::vector<std::string>> table_rows;
	int row_index = -1;
	int column_index = -1;
	bool table_has_heading_row = false;
	// Where the change stands in the document, so changes far apart read as
	// separate places in the document's own order: its position among the
	// body's blocks, the heading it falls under, and its page - exact where the
	// document's own page breaks decide it, otherwise estimated from the
	// document's page size and margins (page_estimated). -1 / "" / 0 unknown.
	int order = -1;
	std::string section;
	int page = 0;
	bool page_estimated = false;
	// For a replacement or deletion: the rest of its paragraph, so the change
	// is read inside its sentence as Word shows it.
	std::string context_before;
	std::string context_after;
	// For an insertion into clauses Word numbers itself: the new clause takes
	// the next number and every clause after it moves up by one - which the
	// lawyer is told before approving, since cross-references typed as text
	// ("see Section 9.3") do not move with them.
	bool renumbers_following = false;
};

namespace change_description_detail {

inline std::string points(double value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%g pt", value);
	return text;
}

inline std::vector<std::string> formatting_words(const DocumentTextFormatting &formatting) {
	std::vector<std::string> words;
	if (formatting.bold) words.push_back(*formatting.bold ? "bold" : "not bold");
	if (formatting.italic) words.push_back(*formatting.italic ? "italic" : "not italic");
	if (formatting.underline) words.push_back(*formatting.underline ? "underlined" : "not underlined");
	if (formatting.color_hex) words.push_back("colour #" + *formatting.color_hex);
	if (formatting.highlight)
		words.push_back(*formatting.highlight == "none" ? std::string("no highlight") : "highlight " + *formatting.highlight);
	if (formatting.size_points) words.push_back(points(*formatting.size_points));
	if (formatting.font_name) words.push_back("font " + *formatting.font_name);
	if (formatting.paragraph_style) words.push_back("style " + *formatting.paragraph_style);
	if (formatting.alignment) words.push_back("aligned " + *formatting.alignment);
	if (formatting.space_before_points) words.push_back(points(*formatting.space_before_points) + " before");
	if (formatting.space_after_points) words.push_back(points(*formatting.space_after_points) + " after");
	return words;
}

inline const DocumentTable *table_named(const DocumentSemanticModel &model, const std::string &tableId) {
	for (const DocumentTable &table : model.tables)
		if (table.table_id == tableId) return &table;
	return nullptr;
}

// A column as the table's own header row names it, falling back to the name
// the change used when the table has no header for it.
inline std::string column_label(const DocumentTable &table, const std::string &name) {
	const std::optional<std::size_t> index = table.column_index_for(name);
	if (index && *index < table.column_labels.size() && !table.column_labels[*index].empty())
		return table.column_labels[*index];
	return name;
}

inline std::vector<std::string> labels_of(const DocumentTable &table) {
	std::vector<std::string> labels;
	for (std::size_t index = 0; index < table.column_headers.size(); ++index)
		labels.push_back(index < table.column_labels.size() && !table.column_labels[index].empty()
			? table.column_labels[index] : table.column_headers[index]);
	return labels;
}

inline const DocumentTableCell *cell_in(const DocumentTable &table, const DocumentTableRow &row,
		const std::string &columnName) {
	const std::optional<std::size_t> index = table.column_index_for(columnName);
	if (!index || *index >= table.column_headers.size()) return nullptr;
	return row.cell_for_column(table.column_headers[*index]);
}

} // namespace change_description_detail

// Every operation of a change, in the order it will be made, described from
// the change alone. complete_change_steps() then adds what only the document
// can say (a cell's current words, a row that is about to go).
inline std::vector<DocumentChangeStep> describe_patch_operations(
		const std::vector<DocumentPatchOperation> &operations) {
	std::vector<DocumentChangeStep> steps;
	for (const DocumentPatchOperation &operation : operations) {
		DocumentChangeStep step;
		step.table_id = operation.locator.table_id.value_or(std::string());
		step.row = operation.locator.row_key.value_or(std::string());
		step.column = operation.locator.column_key.value_or(std::string());
		switch (operation.kind) {
			case DocumentPatchOperationKind::ReplaceText:
				step.kind = "replace";
				step.removed_text = operation.expected_old_value;
				step.added_text = operation.new_value;
				break;
			case DocumentPatchOperationKind::DeleteContent:
				step.kind = "delete";
				step.removed_text = operation.expected_old_value;
				break;
			case DocumentPatchOperationKind::InsertParagraph:
				step.kind = "insert";
				step.added_text = operation.new_value;
				step.anchor_text = operation.expected_old_value;
				step.placement = operation.placement == DocumentPatchPlacement::AtEnd ? "at_end"
					: operation.placement == DocumentPatchPlacement::Before ? "before" : "after";
				break;
			case DocumentPatchOperationKind::ReplaceCellText:
				step.kind = "cell";
				step.removed_text = operation.expected_old_value;
				step.added_text = operation.new_value;
				break;
			case DocumentPatchOperationKind::InsertTableRow:
				step.kind = "row_insert";
				step.placement = operation.placement == DocumentPatchPlacement::AtEnd ? "at_end"
					: operation.placement == DocumentPatchPlacement::Before ? "before" : "after";
				for (const DocumentPatchCellValue &cell : operation.cells) {
					step.columns.push_back(cell.column_key);
					step.cells.push_back(cell.value);
				}
				break;
			case DocumentPatchOperationKind::DeleteTableRow:
				step.kind = "row_delete";
				step.removed_text = operation.expected_old_value;
				break;
			case DocumentPatchOperationKind::SetFormatting:
				step.kind = "format";
				step.anchor_text = operation.expected_old_value;
				step.formatting = change_description_detail::formatting_words(operation.formatting);
				break;
		}
		steps.push_back(std::move(step));
	}
	return steps;
}

// Fills in what the live document says where a step names a table rather than
// quoting it: the cell's current words, the full row a deletion removes, and
// the table's own column names. `model` must be the document as it stands
// BEFORE the change - read after it, a cell's "was" would be its new value.
inline void complete_change_steps(std::vector<DocumentChangeStep> &steps, const DocumentSemanticModel &model) {
	using namespace change_description_detail;
	for (DocumentChangeStep &step : steps) {
		if (step.table_id.empty()) continue;
		const DocumentTable *table = table_named(model, step.table_id);
		if (table == nullptr) continue;
		const DocumentTableRow *row = step.row.empty() ? nullptr : table->find_row(step.row);
		if (step.kind == "cell" || (step.kind == "format" && !step.column.empty())) {
			if (row != nullptr && !step.column.empty()) {
				const DocumentTableCell *cell = cell_in(*table, *row, step.column);
				if (cell != nullptr) {
					if (step.kind == "cell") step.removed_text = cell->text;
					else if (step.anchor_text.empty()) step.anchor_text = cell->text;
				}
				const std::optional<std::size_t> column = table->column_index_for(step.column);
				if (step.kind == "cell" && column) {
					// Where the cell is, whatever the table's size: a Word change's
					// words are read from the document at exactly that cell.
					for (std::size_t index = 0; index < table->rows.size(); ++index)
						if (&table->rows[index] == row) step.row_index = static_cast<int>(index);
					step.column_index = static_cast<int>(*column);
				}
				if (step.kind == "cell" && column && table->rows.size() <= 40) {
					for (std::size_t index = 0; index < table->rows.size(); ++index) {
						const DocumentTableRow &each = table->rows[index];
						std::vector<std::string> texts;
						for (std::size_t header = 0; header < table->column_headers.size(); ++header) {
							const DocumentTableCell *found = each.cell_for_column(table->column_headers[header]);
							texts.push_back(found != nullptr ? found->text : std::string());
						}
						step.table_rows.push_back(std::move(texts));
					}
					// A delimited table's first row is its header. A Word table has a
					// heading row only where the document marks one - read with the
					// document's own layout (ProfessionalArtifactAction's
					// complete_word_steps), never guessed from the first row's words.
					step.table_has_heading_row = model.format != DocumentFormat::docx && !table->column_labels.empty();
				}
			}
			if (!step.column.empty()) step.column = column_label(*table, step.column);
		} else if (step.kind == "row_insert") {
			for (std::string &column : step.columns) column = column_label(*table, column);
		} else if (step.kind == "row_delete" && row != nullptr) {
			step.columns = labels_of(*table);
			step.cells.clear();
			for (std::size_t index = 0; index < table->column_headers.size(); ++index) {
				const DocumentTableCell *cell = row->cell_for_column(table->column_headers[index]);
				step.cells.push_back(cell != nullptr ? cell->text : std::string());
			}
		}
	}
}

inline nlohmann::json document_change_step_json(const DocumentChangeStep &step) {
	nlohmann::json value{{"kind", step.kind}};
	if (!step.removed_text.empty()) value["removedText"] = step.removed_text;
	if (!step.added_text.empty()) value["addedText"] = step.added_text;
	if (!step.kept_before.empty()) value["keptBefore"] = step.kept_before;
	if (!step.kept_after.empty()) value["keptAfter"] = step.kept_after;
	if (!step.anchor_text.empty()) value["anchorText"] = step.anchor_text;
	if (!step.placement.empty()) value["placement"] = step.placement;
	if (!step.table_id.empty()) value["tableId"] = step.table_id;
	if (!step.row.empty()) value["row"] = step.row;
	if (!step.column.empty()) value["column"] = step.column;
	if (!step.columns.empty()) value["columns"] = step.columns;
	if (!step.cells.empty()) value["cells"] = step.cells;
	if (!step.formatting.empty()) value["formatting"] = step.formatting;
	if (step.order >= 0) value["order"] = step.order;
	if (!step.section.empty()) value["section"] = step.section;
	if (step.page > 0) {
		value["page"] = step.page;
		value["pageEstimated"] = step.page_estimated;
	}
	if (!step.context_before.empty()) value["contextBefore"] = step.context_before;
	if (!step.context_after.empty()) value["contextAfter"] = step.context_after;
	if (step.renumbers_following) value["renumbersFollowing"] = true;
	if (!step.table_rows.empty()) {
		value["tableRows"] = step.table_rows;
		value["rowIndex"] = step.row_index;
		value["columnIndex"] = step.column_index;
		value["tableHasHeadingRow"] = step.table_has_heading_row;
	}
	return value;
}

inline DocumentChangeStep document_change_step_from_json(const nlohmann::json &value) {
	DocumentChangeStep step;
	if (!value.is_object()) return step;
	const auto text = [&](const char *key) {
		return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
	};
	const auto list = [&](const char *key) {
		std::vector<std::string> items;
		if (value.contains(key) && value.at(key).is_array())
			for (const nlohmann::json &item : value.at(key)) if (item.is_string()) items.push_back(item.get<std::string>());
		return items;
	};
	step.kind = text("kind");
	step.removed_text = text("removedText");
	step.added_text = text("addedText");
	step.kept_before = text("keptBefore");
	step.kept_after = text("keptAfter");
	step.anchor_text = text("anchorText");
	step.placement = text("placement");
	step.table_id = text("tableId");
	step.row = text("row");
	step.column = text("column");
	step.columns = list("columns");
	step.cells = list("cells");
	step.formatting = list("formatting");
	step.order = value.value("order", -1);
	step.section = text("section");
	step.page = value.value("page", 0);
	step.page_estimated = value.value("pageEstimated", false);
	step.context_before = text("contextBefore");
	step.context_after = text("contextAfter");
	step.renumbers_following = value.value("renumbersFollowing", false);
	if (value.contains("tableRows") && value.at("tableRows").is_array()) {
		for (const nlohmann::json &row : value.at("tableRows")) {
			std::vector<std::string> texts;
			if (row.is_array()) for (const nlohmann::json &cell : row) texts.push_back(cell.is_string() ? cell.get<std::string>() : std::string());
			step.table_rows.push_back(std::move(texts));
		}
		step.row_index = value.value("rowIndex", -1);
		step.column_index = value.value("columnIndex", -1);
		step.table_has_heading_row = value.value("tableHasHeadingRow", false);
	}
	return step;
}

} // namespace singularity::documents
