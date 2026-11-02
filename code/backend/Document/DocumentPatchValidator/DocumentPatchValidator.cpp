#include "Document/DocumentPatchValidator/DocumentPatchValidator.hpp"

#include "Platform/PhraseMatch/PhraseMatch.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace singularity::documents {
namespace {

// The document's own text, as the structural read sees it: every paragraph and
// every table cell, one per line, in the order they stand. This is what a
// passage is looked for in, so what the validator judges unique is what the
// writer will later find.
std::string structural_text(const DocumentSemanticModel &model) {
	std::string text;
	for (const DocumentParagraph &paragraph : model.paragraphs) {
		if (!text.empty()) text.push_back('\n');
		text += paragraph.text;
	}
	for (const DocumentTable &table : model.tables) {
		for (const DocumentTableRow &row : table.rows) {
			for (const DocumentTableCell &cell : row.cells) {
				if (!text.empty()) text.push_back('\n');
				text += cell.text;
			}
		}
	}
	return text;
}

// The table an operation names: by id, or the document's only table when it
// has exactly one. Anything else is ambiguous and refused.
const DocumentTable *resolve_table(const DocumentSemanticModel &model,
		const DocumentPatchOperation &operation, DocumentPatchOperationOutcome &outcome) {
	const DocumentTable *table = nullptr;
	if (operation.locator.table_id.has_value()) {
		for (const DocumentTable &candidate : model.tables) {
			if (candidate.table_id != *operation.locator.table_id) continue;
			if (table != nullptr) {
				outcome.blocker = DocumentPatchBlocker::LocatorAmbiguous;
				outcome.detail = "More than one table shares the patch's table id.";
				return nullptr;
			}
			table = &candidate;
		}
	} else if (model.tables.size() == 1) {
		table = &model.tables.front();
	} else {
		outcome.blocker = DocumentPatchBlocker::LocatorAmbiguous;
		outcome.detail = "The patch omitted table_id and the document has more than one table.";
		return nullptr;
	}
	if (table == nullptr) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "The patch's table id does not resolve to a real table.";
	}
	return table;
}

// A passage operation: the words it names must stand in the document, in
// exactly one place. Typography never decides (Platform/PhraseMatch) - the
// words do.
DocumentPatchOperationOutcome resolve_passage_operation(
		const DocumentSemanticModel &currentModel, const DocumentPatchOperation &operation) {
	DocumentPatchOperationOutcome outcome;
	// A paragraph added at the end names no passage: there is nothing to anchor
	// it to, and nothing to be ambiguous about.
	if (operation.kind == DocumentPatchOperationKind::InsertParagraph
			&& operation.placement == DocumentPatchPlacement::AtEnd) {
		if (operation.new_value.empty()) {
			outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
			outcome.detail = "The operation adds a paragraph without saying what it says.";
			return outcome;
		}
		outcome.ok = true;
		return outcome;
	}
	const std::string passage = platform::trimmed_phrase(operation.expected_old_value);
	if (passage.empty()) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "The operation named no passage of the document to act on.";
		return outcome;
	}
	// Only an insertion must bring text with it: a deletion writes nothing, a
	// formatting change writes nothing, and a replacement with no text is a
	// deletion said another way.
	if (operation.kind == DocumentPatchOperationKind::InsertParagraph && operation.new_value.empty()) {
		outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
		outcome.detail = "The operation inserts a paragraph without saying what it says.";
		return outcome;
	}
	const platform::PhrasePlacement placement = platform::locate_phrase(structural_text(currentModel), passage);
	if (placement.matches == 0) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "The passage this operation names is not in the document as it now stands.";
		return outcome;
	}
	if (placement.matches > 1) {
		outcome.blocker = DocumentPatchBlocker::LocatorAmbiguous;
		outcome.detail = "The passage this operation names stands in more than one place in the document.";
		return outcome;
	}
	outcome.ok = true;
	return outcome;
}

DocumentPatchOperationOutcome resolve_row_operation(
		const DocumentSemanticModel &currentModel, const DocumentPatchOperation &operation) {
	DocumentPatchOperationOutcome outcome;
	const DocumentTable *table = resolve_table(currentModel, operation, outcome);
	if (table == nullptr) return outcome;

	if (operation.kind == DocumentPatchOperationKind::DeleteTableRow) {
		if (!operation.locator.row_key.has_value()) {
			outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
			outcome.detail = "The operation did not say which row to remove.";
			return outcome;
		}
		const DocumentTableRow *row = table->find_row(*operation.locator.row_key);
		if (row == nullptr) {
			outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
			outcome.detail = "The row this operation names does not resolve to exactly one row.";
			return outcome;
		}
		// The row's own first cell is the guard: a row that says something else
		// now is not the row the model read.
		if (!operation.expected_old_value.empty() && !row->cells.empty()
				&& row->cells.front().text != operation.expected_old_value) {
			outcome.blocker = DocumentPatchBlocker::OldValueMismatch;
			outcome.detail = "The row's current first cell no longer matches what this operation expected.";
			return outcome;
		}
		outcome.ok = true;
		outcome.resolved_row_index = row->row_index;
		return outcome;
	}

	// insertTableRow: every column it fills must be a column this table has,
	// and no column twice.
	if (operation.cells.empty()) {
		outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
		outcome.detail = "The operation adds a row without saying what any column of it holds.";
		return outcome;
	}
	for (std::size_t index = 0; index < operation.cells.size(); ++index) {
		const std::string &column = operation.cells[index].column_key;
		// Named the way a person names a column - by what the table calls it -
		// or by its positional key. Either resolves; neither is guessed.
		if (!table->column_index_for(column).has_value()) {
			const auto labelled = std::count(table->column_labels.begin(), table->column_labels.end(), column);
			const auto positional = std::count(table->column_headers.begin(), table->column_headers.end(), column);
			outcome.blocker = (labelled + positional) > 1 ? DocumentPatchBlocker::LocatorAmbiguous
														  : DocumentPatchBlocker::LocatorUnresolved;
			outcome.detail = (labelled + positional) > 1
				? "More than one column of the table is called " + column
				: "The operation writes a column the table does not have: " + column;
			return outcome;
		}
		for (std::size_t other = 0; other < index; ++other) {
			if (operation.cells[other].column_key == column) {
				outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
				outcome.detail = "The operation writes the same column twice: " + column;
				return outcome;
			}
		}
	}
	if (operation.placement != DocumentPatchPlacement::AtEnd) {
		if (!operation.locator.row_key.has_value()) {
			outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
			outcome.detail = "The operation did not say which row the new row goes beside.";
			return outcome;
		}
		const DocumentTableRow *row = table->find_row(*operation.locator.row_key);
		if (row == nullptr) {
			outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
			outcome.detail = "The row this operation names does not resolve to exactly one row.";
			return outcome;
		}
		outcome.resolved_row_index = row->row_index;
	}
	outcome.ok = true;
	return outcome;
}

// What a formatting operation may ask for: a colour written the way OOXML
// writes one, an alignment Word has, a real size. A value Singularity cannot write
// is refused here rather than written wrongly.
DocumentPatchOperationOutcome resolve_formatting(const DocumentPatchOperation &operation) {
	DocumentPatchOperationOutcome outcome;
	const DocumentTextFormatting &formatting = operation.formatting;
	if (formatting.empty()) {
		outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
		outcome.detail = "The operation named nothing about how the passage is written.";
		return outcome;
	}
	const auto isHex = [](const std::string &value) {
		if (value.size() != 6) return false;
		return std::all_of(value.begin(), value.end(), [](unsigned char character) {
			return std::isxdigit(character) != 0;
		});
	};
	if (formatting.color_hex && !isHex(*formatting.color_hex)) {
		outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
		outcome.detail = "A colour is six hexadecimal digits, as a document writes one.";
		return outcome;
	}
	if (formatting.alignment) {
		static const std::vector<std::string> allowed{"left", "center", "right", "justify", "both"};
		if (std::find(allowed.begin(), allowed.end(), *formatting.alignment) == allowed.end()) {
			outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
			outcome.detail = "An alignment is left, center, right or justify.";
			return outcome;
		}
	}
	if (formatting.size_points && (*formatting.size_points <= 0.0 || *formatting.size_points > 400.0)) {
		outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
		outcome.detail = "A font size is a real number of points.";
		return outcome;
	}
	for (const std::optional<double> &spacing : {formatting.space_before_points, formatting.space_after_points}) {
		if (spacing && (*spacing < 0.0 || *spacing > 1000.0)) {
			outcome.blocker = DocumentPatchBlocker::InvalidNewValue;
			outcome.detail = "Paragraph spacing is a real number of points.";
			return outcome;
		}
	}
	outcome.ok = true;
	return outcome;
}

DocumentPatchOperationOutcome resolve_operation(
		const DocumentSemanticModel &currentModel, const DocumentPatchOperation &operation,
		const std::vector<std::vector<std::string>> &equivalentWritings) {
	DocumentPatchOperationOutcome outcome;

	switch (operation.kind) {
		case DocumentPatchOperationKind::ReplaceText:
		case DocumentPatchOperationKind::InsertParagraph:
		case DocumentPatchOperationKind::DeleteContent:
			return resolve_passage_operation(currentModel, operation);
		case DocumentPatchOperationKind::SetFormatting: {
			DocumentPatchOperationOutcome formatting = resolve_formatting(operation);
			if (!formatting.ok) return formatting;
			// "Make that cell bold" names a cell, not a passage: a word inside a
			// table stands in many rows, and the row is what was meant.
			if (operation.locator.row_key && operation.locator.column_key)
				break;
			return resolve_passage_operation(currentModel, operation);
		}
		case DocumentPatchOperationKind::InsertTableRow:
		case DocumentPatchOperationKind::DeleteTableRow:
			return resolve_row_operation(currentModel, operation);
		case DocumentPatchOperationKind::ReplaceCellText:
			break;
	}
	if (!operation.locator.row_key.has_value() || !operation.locator.column_key.has_value()) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "A patch operation is missing its row/column locator.";
		return outcome;
	}

	const DocumentTable *table = nullptr;
	if (operation.locator.table_id.has_value()) {
		for (const DocumentTable &candidate : currentModel.tables) {
			if (candidate.table_id != *operation.locator.table_id) continue;
			if (table != nullptr) {
				outcome.blocker = DocumentPatchBlocker::LocatorAmbiguous;
				outcome.detail = "More than one table shares the patch's table id.";
				return outcome;
			}
			table = &candidate;
		}
	} else if (currentModel.tables.size() == 1) {
		table = &currentModel.tables.front();
	} else {
		outcome.blocker = DocumentPatchBlocker::LocatorAmbiguous;
		outcome.detail = "The patch omitted table_id and the document has more than one table.";
		return outcome;
	}
	if (table == nullptr) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "The patch's table id does not resolve to a real table.";
		return outcome;
	}

	// Named by the table's own heading for the column, or by its positional key.
	const std::optional<std::size_t> resolvedColumn = table->column_index_for(*operation.locator.column_key);
	if (!resolvedColumn.has_value()) {
		const auto labelled = std::count(table->column_labels.begin(), table->column_labels.end(),
				*operation.locator.column_key);
		const auto positional = std::count(table->column_headers.begin(), table->column_headers.end(),
				*operation.locator.column_key);
		outcome.blocker = (labelled + positional) > 1 ? DocumentPatchBlocker::LocatorAmbiguous
													  : DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = (labelled + positional) > 1
			? "More than one column of the table is called " + *operation.locator.column_key
			: "The column this change names is not in that table.";
		return outcome;
	}
	const std::size_t columnIndex = *resolvedColumn;

	const DocumentTableRow *row = table->find_row(*operation.locator.row_key);
	if (row == nullptr) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "The patch's row key does not resolve to exactly one row.";
		return outcome;
	}
	const DocumentTableCell *cell = columnIndex < row->cells.size() ? &row->cells[columnIndex] : nullptr;
	if (cell == nullptr) {
		outcome.blocker = DocumentPatchBlocker::LocatorUnresolved;
		outcome.detail = "The patch's row does not have a cell for the resolved column.";
		return outcome;
	}
	// The cell a model quotes back carries its own typography. What must match
	// is what the cell SAYS (Platform/PhraseMatch) - the same rule a quotation
	// and an anchor are held to.
	// A cell's words are replaced only by a change that names them: an empty
	// cell may be filled without quoting it, a cell with words never. A column
	// resolved one place off - a merged cell, a gridded table - then refuses
	// instead of striking another column's "Open".
	if (!platform::phrases_equal(cell->text, operation.expected_old_value, equivalentWritings)) {
		outcome.blocker = DocumentPatchBlocker::OldValueMismatch;
		outcome.detail = operation.expected_old_value.empty()
			? "The cell holds words this change did not name, so it would not replace them."
			: "The cell's current value no longer matches the patch's expected old value.";
		return outcome;
	}

	outcome.ok = true;
	outcome.resolved_row_index = row->row_index;
	outcome.resolved_column_index = columnIndex;
	return outcome;
}

// Whether an operation acts on a table cell or row rather than on a passage.
bool addresses_table(const DocumentPatchOperation &operation) {
	switch (operation.kind) {
		case DocumentPatchOperationKind::ReplaceCellText:
		case DocumentPatchOperationKind::InsertTableRow:
		case DocumentPatchOperationKind::DeleteTableRow:
			return true;
		case DocumentPatchOperationKind::SetFormatting:
			return operation.locator.row_key.has_value() && operation.locator.column_key.has_value();
		case DocumentPatchOperationKind::ReplaceText:
		case DocumentPatchOperationKind::InsertParagraph:
		case DocumentPatchOperationKind::DeleteContent:
			return false;
	}
	return false;
}

// Whether an operation's own words stand in `table`: the row and the columns
// it names are that table's, exactly as resolve_operation() will resolve them.
// With `quotedValue`, a cell must also say what the operation quotes.
bool operation_fits_table(const DocumentTable &table, const DocumentPatchOperation &operation,
		bool quotedValue, const std::vector<std::vector<std::string>> &equivalentWritings) {
	const std::optional<std::string> &rowKey = operation.locator.row_key;
	if (operation.kind == DocumentPatchOperationKind::InsertTableRow) {
		if (operation.cells.empty()) return false;
		for (const DocumentPatchCellValue &cell : operation.cells)
			if (!table.column_index_for(cell.column_key).has_value()) return false;
		if (operation.placement == DocumentPatchPlacement::AtEnd) return true;
		return rowKey.has_value() && table.find_row(*rowKey) != nullptr;
	}
	if (operation.kind == DocumentPatchOperationKind::DeleteTableRow)
		return rowKey.has_value() && table.find_row(*rowKey) != nullptr;
	if (!rowKey.has_value() || !operation.locator.column_key.has_value()) return false;
	const DocumentTableRow *row = table.find_row(*rowKey);
	const std::optional<std::size_t> column = table.column_index_for(*operation.locator.column_key);
	if (row == nullptr || !column.has_value() || *column >= row->cells.size()) return false;
	if (!quotedValue || operation.kind != DocumentPatchOperationKind::ReplaceCellText
			|| operation.expected_old_value.empty())
		return true;
	return platform::phrases_equal(row->cells[*column].text, operation.expected_old_value, equivalentWritings);
}

// One line of a table's description: a value as a reader would write it, cut
// where it runs long.
std::string described_value(const std::string &text) {
	constexpr std::size_t kLongest = 60;
	std::string line;
	for (char character : text) line.push_back(character == '\n' || character == '\t' ? ' ' : character);
	if (line.size() <= kLongest) return line;
	std::size_t cut = kLongest;
	// Never inside one UTF-8 character.
	while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
	return line.substr(0, cut) + "...";
}

// Text compared the way a reader compares it: runs of whitespace are one space.
std::string normalized_text(const std::string &text) {
	std::string result;
	bool pendingSpace = false;
	for (unsigned char character : text) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			pendingSpace = !result.empty();
			continue;
		}
		if (pendingSpace) result.push_back(' ');
		pendingSpace = false;
		result.push_back(static_cast<char>(character));
	}
	return result;
}

} // namespace

// Whether a rewritten document holds exactly a patch of the document
// operations - a passage replaced, deleted or written differently, a paragraph
// inserted, a row added or removed.
//
// The question this answers is not "did the change land" (the writer knows
// that) but "did ANYTHING ELSE change". Every paragraph and every table cell
// the patch did not name must still read exactly what it read before, and the
// tables must still be the same tables with the row counts the patch asked
// for. This is what caught a row rewritten as one span: the words were all
// still in the document, and five cells had gone.
bool patch_change_is_contained(const DocumentSemanticModel &before, const DocumentSemanticModel &after,
		const DocumentPatch &patch) {
	try {
		if (after.status != ExtractionStatus::ready && after.status != ExtractionStatus::empty) return false;
		if (after.tables.size() != before.tables.size()) return false;

		// What the patch intends, counted per table and over the body.
		std::map<std::string, long long> rowDelta;
		// The cells this patch names: those may read differently afterwards, and
		// every other cell of the table may not.
		std::set<std::pair<std::string, std::string>> changedCells;
		long long paragraphDelta = 0;
		// A replacement of several paragraphs, or by several lines, is written
		// clause for clause (OfficeTextEdit): each new line may become a
		// paragraph of its own, and each old paragraph no line rewrites goes.
		// The body may gain up to one paragraph per line given, and lose up to
		// one per paragraph replaced beyond the first - never more.
		long long replacedLines = 0;
		long long joinedParagraphs = 0;
		bool touchesText = false;
		for (const DocumentPatchOperation &operation : patch.operations) {
			const std::string tableId = operation.locator.table_id.value_or(
				before.tables.size() == 1 ? before.tables.front().table_id : std::string());
			switch (operation.kind) {
				case DocumentPatchOperationKind::InsertTableRow: rowDelta[tableId] += 1; break;
				case DocumentPatchOperationKind::DeleteTableRow: rowDelta[tableId] -= 1; break;
				case DocumentPatchOperationKind::InsertParagraph: {
					paragraphDelta += 1;
					for (char character : operation.new_value) if (character == '\n') paragraphDelta += 1;
					touchesText = true;
					break;
				}
				case DocumentPatchOperationKind::DeleteContent:
					paragraphDelta -= 1; // at most one paragraph may go; checked as a bound below
					touchesText = true;
					break;
				case DocumentPatchOperationKind::ReplaceText: {
					touchesText = true;
					const long long breaks = std::count(operation.new_value.begin(), operation.new_value.end(), '\n');
					if (breaks > 0) replacedLines += breaks + 1;
					joinedParagraphs += std::count(operation.expected_old_value.begin(), operation.expected_old_value.end(), '\n');
					break;
				}
				case DocumentPatchOperationKind::ReplaceCellText:
					changedCells.insert({operation.locator.row_key.value_or(std::string()),
						operation.locator.column_key.value_or(std::string())});
					break;
				case DocumentPatchOperationKind::SetFormatting:
					break;
			}
		}

		for (std::size_t index = 0; index < before.tables.size(); ++index) {
			const DocumentTable &previous = before.tables[index];
			const DocumentTable &current = after.tables[index];
			if (previous.table_id != current.table_id) return false;
			const long long expected = static_cast<long long>(previous.rows.size()) + rowDelta[previous.table_id];
			if (static_cast<long long>(current.rows.size()) != expected) return false;
			// Every row the patch did not add or remove still reads what it read.
			if (rowDelta[previous.table_id] == 0) {
				for (std::size_t row = 0; row < previous.rows.size(); ++row) {
					if (previous.rows[row].cells.size() != current.rows[row].cells.size()) return false;
					if (touchesText) continue;
					for (std::size_t cell = 0; cell < previous.rows[row].cells.size(); ++cell) {
						// A cell the patch named may read differently now; every
						// other one must read exactly what it read.
						const std::string columnLabel = cell < previous.column_labels.size()
							? previous.column_labels[cell] : std::string();
						const std::string columnKey = cell < previous.column_headers.size()
							? previous.column_headers[cell] : std::string();
						if (changedCells.count({previous.rows[row].row_key, columnLabel}) > 0
								|| changedCells.count({previous.rows[row].row_key, columnKey}) > 0)
							continue;
						if (normalized_text(previous.rows[row].cells[cell].text)
								!= normalized_text(current.rows[row].cells[cell].text))
							return false;
					}
				}
				continue;
			}
			// A row was added or removed: every row that survived must still hold
			// the same cells, and no row may have lost a cell.
			const std::size_t columns = previous.rows.empty() ? 0 : previous.rows.front().cells.size();
			for (const DocumentTableRow &row : current.rows) {
				if (row.cells.size() != columns) return false;
			}
			std::multiset<std::string> previousRows;
			std::multiset<std::string> currentRows;
			const auto rowText = [](const DocumentTableRow &row) {
				std::string joined;
				for (const DocumentTableCell &cell : row.cells) joined += normalized_text(cell.text) + "\x1f";
				return joined;
			};
			for (const DocumentTableRow &row : previous.rows) previousRows.insert(rowText(row));
			for (const DocumentTableRow &row : current.rows) currentRows.insert(rowText(row));
			std::multiset<std::string> lost;
			for (const std::string &row : previousRows) if (currentRows.count(row) < previousRows.count(row)) lost.insert(row);
			if (static_cast<long long>(lost.size()) > std::max<long long>(0, -rowDelta[previous.table_id]))
				return false;
		}

		// The body's own paragraphs: as many as the patch intended, and every
		// one the patch did not name still reads what it read. A deletion may
		// leave the paragraph in place (a sentence removed from a longer one),
		// so the count is checked as a bound rather than an equality.
		const long long paragraphsBefore = static_cast<long long>(before.paragraphs.size());
		const long long paragraphsAfter = static_cast<long long>(after.paragraphs.size());
		if (paragraphDelta >= 0) {
			if (paragraphsAfter < paragraphsBefore + paragraphDelta - joinedParagraphs
					|| paragraphsAfter > paragraphsBefore + paragraphDelta + replacedLines)
				return false;
		} else if (paragraphsAfter < paragraphsBefore + paragraphDelta - joinedParagraphs
				|| paragraphsAfter > paragraphsBefore + replacedLines) {
			return false;
		}
		if (!touchesText) {
			std::multiset<std::string> previousText;
			std::multiset<std::string> currentText;
			for (const DocumentParagraph &paragraph : before.paragraphs)
				previousText.insert(normalized_text(paragraph.text));
			for (const DocumentParagraph &paragraph : after.paragraphs)
				currentText.insert(normalized_text(paragraph.text));
			if (previousText != currentText) return false;
		}
		return true;
	} catch (const std::exception &) {
		return false;
	}
}


DocumentPatchValidationResult validate_document_patch(const matters::MatterDocument &targetDocument,
		const DocumentSemanticModel &currentModel, const DocumentPatch &patch,
		const std::vector<std::vector<std::string>> &equivalent_writings) {
	DocumentPatchValidationResult result;

	if (patch.document_id != targetDocument.id) {
		result.blocker = DocumentPatchBlocker::DocumentIdentityMismatch;
		result.detail = "The patch's document id does not match the active target document.";
		return result;
	}
	if (patch.expected_version != targetDocument.version) {
		result.blocker = DocumentPatchBlocker::VersionMismatch;
		result.detail = "The patch's expected version no longer matches the document's current version.";
		return result;
	}
	if (patch.expected_fingerprint != targetDocument.fingerprint) {
		result.blocker = DocumentPatchBlocker::FingerprintMismatch;
		result.detail = "The patch's expected fingerprint no longer matches the document's current fingerprint.";
		return result;
	}
	if (document_format_capability(targetDocument.file_type).supports_cell_or_paragraph_mutation == false) {
		result.blocker = DocumentPatchBlocker::UnsupportedFormatCapability;
		result.detail = "This document format does not support validated cell/paragraph mutation.";
		return result;
	}
	if (currentModel.status != ExtractionStatus::ready && currentModel.status != ExtractionStatus::empty) {
		result.blocker = DocumentPatchBlocker::UnsupportedFormatCapability;
		result.detail = "The document's current structural read did not succeed; no patch can be validated against it.";
		return result;
	}
	if (patch.operations.empty()) {
		result.blocker = DocumentPatchBlocker::LocatorUnresolved;
		result.detail = "A patch with no operations cannot be validated.";
		return result;
	}

	bool allOk = true;
	for (const DocumentPatchOperation &operation : patch.operations) {
		DocumentPatchOperationOutcome outcome = resolve_operation(currentModel, operation, equivalent_writings);
		if (outcome.ok == false && allOk == true) {
			result.blocker = outcome.blocker;
			result.detail = outcome.detail;
		}
		allOk = allOk && outcome.ok;
		result.operations.push_back(std::move(outcome));
	}

	result.ok = allOk;
	return result;
}

std::size_t settle_table_locators(const DocumentSemanticModel &currentModel,
		std::vector<DocumentPatchOperation> &operations,
		const std::vector<std::vector<std::string>> &equivalent_writings) {
	std::size_t settled = 0;
	for (DocumentPatchOperation &operation : operations) {
		if (!addresses_table(operation)) continue;
		// An id that names a table is the table, exactly as given - a model that
		// was shown the id meant it, and one id naming two tables stays refused.
		if (operation.locator.table_id.has_value()) {
			const auto named = std::count_if(currentModel.tables.begin(), currentModel.tables.end(),
				[&](const DocumentTable &table) { return table.table_id == *operation.locator.table_id; });
			if (named > 0) continue;
		}

		/* - - - - - - */

		// The tables the operation's words stand in: first with the value a cell
		// operation quotes, which tells two alike tables apart; then without it,
		// so a misquoted value is refused as a misquote, not as a missing table.
		const DocumentTable *found = nullptr;
		for (const bool quotedValue : {true, false}) {
			std::size_t fits = 0;
			for (const DocumentTable &table : currentModel.tables) {
				if (!operation_fits_table(table, operation, quotedValue, equivalent_writings)) continue;
				++fits;
				found = &table;
			}
			if (fits == 1) break;
			found = nullptr;
			if (fits > 1) break;
		}
		if (found == nullptr) continue;
		operation.locator.table_id = found->table_id;
		++settled;
	}
	return settled;
}

namespace {

// The words of a row name as compared for resemblance: letters and digits,
// lower-cased where ASCII, of three bytes or more.
std::set<std::string> row_name_words(const std::string &name) {
	std::set<std::string> words;
	std::string word;
	const auto flush = [&] {
		if (word.size() >= 3) words.insert(word);
		word.clear();
	};
	for (char character : name) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte >= 0x80 || std::isalnum(byte) != 0) word.push_back(static_cast<char>(std::tolower(byte)));
		else flush();
	}
	flush();
	return words;
}

} // namespace

std::string describe_document_tables(const DocumentSemanticModel &model, std::size_t maximumRowsPerTable,
		const std::vector<std::string> &namedRows) {
	std::set<std::string> namedWords;
	for (const std::string &name : namedRows)
		for (const std::string &word : row_name_words(name)) namedWords.insert(word);
	std::string text;
	for (const DocumentTable &table : model.tables) {
		if (!text.empty()) text.push_back('\n');
		text += "tableId \"" + table.table_id + "\" - columns: ";
		for (std::size_t column = 0; column < table.column_headers.size(); ++column) {
			const bool labelled = column < table.column_labels.size() && !table.column_labels[column].empty();
			if (column > 0) text += " | ";
			text += described_value(labelled ? table.column_labels[column] : table.column_headers[column]);
		}
		text += "; rows (by their first cell): ";
		std::size_t shown = 0;
		for (std::size_t row = 0; row < table.rows.size(); ++row) {
			bool resembles = false;
			if (row >= maximumRowsPerTable)
				for (const std::string &word : row_name_words(table.rows[row].row_key))
					if (namedWords.count(word) != 0) resembles = true;
			if (row >= maximumRowsPerTable && !resembles) continue;
			if (shown > 0) text += ", ";
			text += "\"" + described_value(table.rows[row].row_key) + "\"";
			++shown;
		}
		if (table.rows.size() > shown) text += " and " + std::to_string(table.rows.size() - shown) + " more";
		text += ".";
	}
	return text.empty() ? std::string("The document has no tables.") : text;
}

} // namespace singularity::documents
