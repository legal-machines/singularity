#include "Document/DocumentStructuralReader/DocumentStructuralReader.hpp"

#include "Document/CsvExtractor/CsvExtractor.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "Document/PptxExtractor/PptxExtractor.hpp"
#include "Document/XlsxExtractor/XlsxExtractor.hpp"
#include "Document/XlsxWorkbookView/XlsxWorkbookView.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::documents {

DocumentSemanticModel read_document_structure(const DocumentExtractionRequest &request) {
	DocumentSemanticModel model;

	const LocalDocumentExtractor extractor;
	const DocumentExtractionResult flattened = extractor.extract(request);
	model.format = flattened.format;
	model.status = flattened.status;
	model.structural_version = flattened.extractor_version;
	model.diagnostics = flattened.diagnostics;
	model.pages = flattened.structure.page_boundaries;

	// A Word document reads as every paragraph it holds, in order, each knowing
	// the section it stands in. Every other format is known by its headings
	// alone until its own adapter reads more.
	if (model.format != DocumentFormat::docx) {
		for (const DocumentHeading &heading : flattened.structure.headings) {
			DocumentParagraph paragraph;
			paragraph.semantic_path = "Body";
			paragraph.heading_level = heading.level;
			paragraph.text = heading.title;
			model.paragraphs.push_back(std::move(paragraph));
		}
	}

	switch (model.format) {
		case DocumentFormat::docx:
			model.paragraphs = extract_docx_paragraphs(request);
			model.tables = extract_docx_tables(request);
			break;
		case DocumentFormat::pptx:
			model.slides = extract_pptx_slides(request);
			break;
		case DocumentFormat::xlsx:
			model.sheets = extract_xlsx_sheets(request);
			break;
		case DocumentFormat::csv: {
			DocumentTable table = extract_csv_table(request);
			if (!table.rows.empty() || !table.column_headers.empty()) model.tables.push_back(std::move(table));
			break;
		}
		case DocumentFormat::tsv: {
			DocumentTable table = extract_tsv_table(request);
			if (!table.rows.empty() || !table.column_headers.empty()) model.tables.push_back(std::move(table));
			break;
		}
		case DocumentFormat::pdf:
		case DocumentFormat::plain_text:
		case DocumentFormat::markdown:
		case DocumentFormat::json:
		case DocumentFormat::xml:
		case DocumentFormat::html:
		case DocumentFormat::unknown:
			break;
	}

	return model;
}


namespace {

DocumentRenderParagraph plain_paragraph(std::string text, std::optional<unsigned int> level = std::nullopt) {
	DocumentRenderParagraph paragraph;
	paragraph.heading_level = level;
	DocumentRenderRun run;
	run.text = std::move(text);
	if (!run.text.empty()) paragraph.runs.push_back(std::move(run));
	return paragraph;
}

void append_paragraph_block(std::vector<DocumentRenderBlock> &blocks, DocumentRenderParagraph paragraph) {
	DocumentRenderBlock block;
	block.kind = DocumentRenderBlockKind::Paragraph;
	block.paragraph = std::move(paragraph);
	blocks.push_back(std::move(block));
}

// A table Singularity already read structurally, laid out for a person: the
// header row first where the table has one, then its rows in order.
void append_table_block(std::vector<DocumentRenderBlock> &blocks, const DocumentTable &table) {
	DocumentRenderTable rendered;
	rendered.table_id = table.table_id;
	std::size_t columnCount = table.column_labels.size();
	for (const DocumentTableRow &row : table.rows) columnCount = std::max(columnCount, row.cells.size());
	if (!table.column_labels.empty()) {
		DocumentRenderRow header;
		header.is_header = true;
		for (std::size_t index = 0; index < table.column_labels.size(); ++index) {
			DocumentRenderCell cell;
			cell.column_index = index;
			cell.paragraphs.push_back(plain_paragraph(table.column_labels[index]));
			header.cells.push_back(std::move(cell));
		}
		rendered.rows.push_back(std::move(header));
	}
	for (const DocumentTableRow &row : table.rows) {
		DocumentRenderRow rendered_row;
		rendered_row.row_index = rendered.rows.size();
		for (const DocumentTableCell &cell : row.cells) {
			DocumentRenderCell rendered_cell;
			rendered_cell.column_index = cell.column_index;
			rendered_cell.grid_span = cell.grid_span;
			rendered_cell.is_merge_continuation = cell.is_merge_continuation;
			rendered_cell.shading_fill_hex = cell.shading_fill_hex;
			rendered_cell.paragraphs.push_back(plain_paragraph(cell.text));
			rendered_row.cells.push_back(std::move(rendered_cell));
		}
		rendered.rows.push_back(std::move(rendered_row));
	}
	DocumentRenderBlock block;
	block.kind = DocumentRenderBlockKind::Table;
	block.table = std::move(rendered);
	blocks.push_back(std::move(block));
}

// Flattened text as paragraphs: one per line, blank lines dropped. This is
// what a format with no layout of its own honestly amounts to.
void append_text_blocks(std::vector<DocumentRenderBlock> &blocks, const std::string &text) {
	std::string line;
	const auto flush = [&]() {
		if (!line.empty()) append_paragraph_block(blocks, plain_paragraph(line));
		line.clear();
	};
	for (const char character : text) {
		if (character == '\n') { flush(); continue; }
		if (character != '\r') line.push_back(character);
	}
	flush();
}

// The safety cap the workbook reader holds every sheet to (XlsxWorkbookView):
// a delimited file past it is shown as far as its rows are whole.
constexpr std::size_t kMaximumSheetCells = 200000;

// "A", "Z", "AA", ... - the column letters a lawyer reads in a reference.
std::string column_letters(std::size_t column) {
	std::string letters;
	std::size_t number = column + 1;
	while (number > 0) {
		letters.insert(letters.begin(), static_cast<char>('A' + (number - 1) % 26));
		number = (number - 1) / 26;
	}
	return letters;
}

// A field a delimited file writes as a plain number - "-12", "1250.5", "3e6" -
// and nothing else: currency, grouping and a percent sign stay the text they
// are. A whole number with a leading zero ("007", a reference or a postcode)
// stays text too, as the sheet engine treats one everywhere else. Read the C
// way whatever the process locale says.
bool plain_number(const std::string &text, double &value) {
	std::size_t index = 0;
	if (index < text.size() && (text[index] == '-' || text[index] == '+')) ++index;
	const std::size_t integerBegin = index;
	while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) ++index;
	const std::size_t integerDigits = index - integerBegin;
	std::size_t fractionDigits = 0;
	if (index < text.size() && text[index] == '.') {
		const std::size_t fractionBegin = ++index;
		while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) ++index;
		fractionDigits = index - fractionBegin;
	}
	if (integerDigits == 0 && fractionDigits == 0) return false;
	if (integerDigits > 1 && text[integerBegin] == '0') return false;
	if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
		++index;
		if (index < text.size() && (text[index] == '-' || text[index] == '+')) ++index;
		const std::size_t exponentBegin = index;
		while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) ++index;
		if (index == exponentBegin) return false;
	}
	if (index != text.size()) return false;
	std::istringstream stream(text);
	stream.imbue(std::locale::classic());
	stream >> value;
	return !stream.fail() && std::isfinite(value);
}

// One field of a delimited file as the cell it is. An empty field is no cell
// at all, exactly as Excel opens one.
void append_delimited_cell(SheetRenderRow &row, std::size_t column, const std::string &text) {
	if (text.empty()) return;
	SheetRenderCell cell;
	cell.column = column;
	cell.ref = column_letters(column) + std::to_string(row.index + 1);
	double number = 0.0;
	if (plain_number(text, number)) {
		cell.kind = SheetCellKind::Number;
		cell.number = number;
	} else {
		cell.kind = SheetCellKind::Text;
		cell.raw_text = text;
	}
	// As the file writes it: a delimited file carries no number format, and
	// Singularity does not invent one.
	cell.text = text;
	row.cells.push_back(std::move(cell));
}

// A CSV or TSV as the one sheet it is, named after its file: every field where
// the file puts it, the first row included - a header row is the file's own
// first row, not a caption to take away from it. A plain number is a number,
// so a selection of them sums; everything else is text. A delimited file has
// no look of its own, so no cell carries one.
SheetWorkbookRender delimited_workbook(const DocumentTable &table, const std::string &name,
		std::string &limitation) {
	SheetRender sheet;
	sheet.name = name;

	std::vector<SheetRenderRow> rows;
	{
		// The structural read keeps the first row apart as the column headers;
		// in the file, and so in the sheet, it is row 1.
		SheetRenderRow header;
		header.index = 0;
		for (std::size_t column = 0; column < table.column_headers.size(); ++column)
			append_delimited_cell(header, column, table.column_headers[column]);
		rows.push_back(std::move(header));
	}
	for (const DocumentTableRow &source : table.rows) {
		SheetRenderRow row;
		row.index = source.row_index;
		for (const DocumentTableCell &cell : source.cells) append_delimited_cell(row, cell.column_index, cell.text);
		rows.push_back(std::move(row));
	}

	std::size_t cellCount = 0;
	for (SheetRenderRow &row : rows) {
		if (row.cells.empty()) continue;
		if (cellCount + row.cells.size() > kMaximumSheetCells) {
			limitation = "This file is shown up to row " + std::to_string(sheet.row_count)
				+ ": the rest is too large to show here. Open it in Excel to see all of it.";
			break;
		}
		cellCount += row.cells.size();
		sheet.row_count = row.index + 1;
		sheet.column_count = std::max(sheet.column_count, row.cells.back().column + 1);
		sheet.rows.push_back(std::move(row));
	}

	SheetWorkbookRender workbook;
	workbook.sheets.push_back(std::move(sheet));
	return workbook;
}

} // namespace

DocumentRenderModel read_document_render_model(const DocumentExtractionRequest &request) {
	// A workbook reads as its workbook and nothing else. The "A1 value"
	// transcript a model reads is not something to show a person, so it is not
	// even made here: the format is resolved exactly as the extractor resolves it.
	if (request.format_hint.value_or(detect_document_format(request.path)) == DocumentFormat::xlsx) {
		XlsxWorkbookViewResult view = read_xlsx_workbook_view(request);
		DocumentRenderModel model;
		model.format = DocumentFormat::xlsx;
		model.status = view.status;
		model.limitation = std::move(view.limitation);
		model.diagnostics = std::move(view.diagnostics);
		if (view.status != ExtractionStatus::unreadable) model.workbook = std::move(view.workbook);
		return model;
	}

	const LocalDocumentExtractor extractor;
	const DocumentExtractionResult flattened = extractor.extract(request);

	if (flattened.format == DocumentFormat::docx) {
		DocumentRenderModel model = render_docx_document(request);
		if (model.status != ExtractionStatus::ready && model.limitation.empty())
			model.limitation = "This Word document could not be read.";
		return model;
	}

	DocumentRenderModel model;
	model.format = flattened.format;
	model.status = flattened.status;
	model.diagnostics = flattened.diagnostics;

	if (flattened.status != ExtractionStatus::ready) {
		model.limitation = "Singularity has not read this file yet.";
		return model;
	}

	switch (flattened.format) {
		case DocumentFormat::csv:
		case DocumentFormat::tsv: {
			const DocumentTable table = flattened.format == DocumentFormat::csv
				? extract_csv_table(request) : extract_tsv_table(request);
			model.workbook = delimited_workbook(table, request.path.stem().string(), model.limitation);
			break;
		}
		case DocumentFormat::pptx: {
			const DocumentSemanticModel structure = read_document_structure(request);
			for (const DocumentSlide &slide : structure.slides) {
				append_paragraph_block(model.blocks,
					plain_paragraph("Slide " + std::to_string(slide.slide_number), 2U));
				for (const DocumentSlideTextBlock &text : slide.text_blocks)
					append_paragraph_block(model.blocks, plain_paragraph(text.text));
				for (const DocumentTable &table : slide.tables) append_table_block(model.blocks, table);
			}
			if (model.blocks.empty()) append_text_blocks(model.blocks, flattened.text);
			break;
		}
		case DocumentFormat::pdf:
			append_text_blocks(model.blocks, flattened.text);
			model.limitation = "A PDF is evidence Singularity reads, not a document it lays out: this is its text, page by page as it was extracted.";
			break;
		default:
			append_text_blocks(model.blocks, flattened.text);
			model.limitation = "Singularity reads this file as text: it carries no layout of its own.";
			break;
	}

	return model;
}


namespace {

// Two pieces of text say the same thing when their words do: the runs of
// whitespace a word processor leaves between them are not a change a lawyer
// made. Case and punctuation ARE the words.
std::string comparable(const std::string &text) {
	std::string result;
	bool pendingSpace = false;
	for (const char character : text) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			pendingSpace = !result.empty();
			continue;
		}
		if (pendingSpace) result.push_back(' ');
		pendingSpace = false;
		result.push_back(character);
	}
	return result;
}

// The rows of a table, by what their first cell says - the same identity
// derive_table_row_key() gives a row, so a row a change named is the row a
// reader sees marked.
std::string row_key_of(const DocumentRenderRow &row) {
	if (row.cells.empty() || row.cells.front().paragraphs.empty()) return {};
	return comparable(row.cells.front().paragraphs.front().text());
}

// A row the document records as deleted in a tracked change. It is still in
// the file, and shown struck, but it is no longer a row of the table as the
// table reads - exactly as the locator and the writer treat it - so it is
// neither matched nor marked.
bool row_is_struck(const DocumentRenderRow &row) {
	return row.revision.has_value() && row.revision->kind == DocumentRenderRevisionKind::Deleted;
}

// How a paragraph is WRITTEN, character by character: which of its characters
// are bold, italic, underlined, struck through. Built per character rather
// than per run, because two versions of the same sentence are commonly split
// into different runs while being written identically - and because a lawyer
// who asked for one word in italic changed the document, whatever the runs did.
std::string formatting_signature(const DocumentRenderParagraph &paragraph) {
	std::string signature;
	for (const DocumentRenderRun &run : paragraph.runs) {
		// Words struck in a tracked change are not written anywhere any more:
		// like text(), the signature is of what the paragraph says.
		if (run.is_deleted()) continue;
		const std::string text = comparable(run.text);
		if (text.empty()) continue;
		const char marks = static_cast<char>('0' + (run.bold ? 1 : 0) + (run.italic ? 2 : 0)
			+ (run.underline ? 4 : 0) + (run.strikethrough ? 8 : 0));
		signature.append(text.size(), marks);
	}
	return signature;
}

std::string cell_formatting_signature(const DocumentRenderCell &cell) {
	std::string signature;
	for (const DocumentRenderParagraph &paragraph : cell.paragraphs) signature += formatting_signature(paragraph);
	return signature;
}

// What a cell of the previous version said, its paragraphs one after another,
// as the previous version itself wrote them.
std::string previous_cell_text(const DocumentRenderCell &cell) {
	std::string previousText;
	for (const DocumentRenderParagraph &paragraph : cell.paragraphs) {
		if (!previousText.empty()) previousText += " ";
		previousText += paragraph.text();
	}
	return previousText;
}

std::string cell_text_of(const DocumentRenderCell &cell) {
	std::string joined;
	for (const DocumentRenderParagraph &paragraph : cell.paragraphs) {
		if (!joined.empty()) joined += " ";
		joined += paragraph.text();
	}
	return comparable(joined);
}

// The longest run of paragraphs the two versions share, in order: everything
// outside it is what the change touched. The ordinary dynamic-programming
// answer - a document's paragraphs are few enough that the exact answer is
// affordable, and an approximation would mark untouched paragraphs as changed.
std::vector<std::pair<std::size_t, std::size_t>> longest_common_paragraphs(
		const std::vector<std::string> &current, const std::vector<std::string> &previous) {
	const std::size_t rows = current.size();
	const std::size_t columns = previous.size();
	// A safety bound: a document of tens of thousands of paragraphs is not
	// compared paragraph by paragraph, it is simply left unmarked.
	if (rows == 0 || columns == 0 || rows * columns > 16000000) return {};
	std::vector<std::vector<std::uint32_t>> lengths(rows + 1, std::vector<std::uint32_t>(columns + 1, 0));
	for (std::size_t row = rows; row-- > 0;) {
		for (std::size_t column = columns; column-- > 0;) {
			lengths[row][column] = current[row] == previous[column]
				? lengths[row + 1][column + 1] + 1
				: std::max(lengths[row + 1][column], lengths[row][column + 1]);
		}
	}
	std::vector<std::pair<std::size_t, std::size_t>> pairs;
	std::size_t row = 0;
	std::size_t column = 0;
	while (row < rows && column < columns) {
		if (current[row] == previous[column]) { pairs.emplace_back(row, column); ++row; ++column; continue; }
		if (lengths[row + 1][column] >= lengths[row][column + 1]) ++row; else ++column;
	}
	return pairs;
}

// A cell by where it stands, as one key: its row and its column.
std::uint64_t cell_key(std::size_t row, std::size_t column) {
	return (static_cast<std::uint64_t>(row) << 32) | static_cast<std::uint64_t>(column);
}

// The sheet of the previous version a sheet is compared with: the one of the
// same name. A CSV or TSV is one sheet named after its file, and the version
// Singularity kept is a file of another name - so its one sheet is this one.
const SheetRender *previous_sheet_for(const SheetRender &sheet, const SheetWorkbookRender &previous,
		bool isDelimited) {
	for (const SheetRender &candidate : previous.sheets)
		if (candidate.name == sheet.name) return &candidate;
	if (isDelimited && previous.sheets.size() == 1) return &previous.sheets.front();
	return nullptr;
}

// A sheet cell by cell, by what each cell SHOWS: the text its number format
// draws, which is what a lawyer reads and what they would call a change - the
// same value shown as "50%" where it showed "0.5" was changed. A cell whose
// text is what the previous version showed at the same place is Unchanged; one
// that shows something where nothing was is Added; one that shows something
// else is Changed and carries what it showed. What the previous version showed
// where this one has no cell at all is listed as removed.
void mark_sheet_changes(SheetRender &sheet, const SheetRender *before) {
	if (before == nullptr) {
		// A sheet the previous version did not have: everything written on it is new.
		for (SheetRenderRow &row : sheet.rows)
			for (SheetRenderCell &cell : row.cells)
				cell.change = cell.text.empty() ? SheetCellChangeMark::Unchanged : SheetCellChangeMark::Added;
		return;
	}
	std::unordered_map<std::uint64_t, const SheetRenderCell *> previousCells;
	for (const SheetRenderRow &row : before->rows)
		for (const SheetRenderCell &cell : row.cells) previousCells.emplace(cell_key(row.index, cell.column), &cell);
	for (SheetRenderRow &row : sheet.rows) {
		for (SheetRenderCell &cell : row.cells) {
			const auto found = previousCells.find(cell_key(row.index, cell.column));
			std::string previousText;
			if (found != previousCells.end()) {
				previousText = found->second->text;
				previousCells.erase(found);
			}
			if (cell.text == previousText) { cell.change = SheetCellChangeMark::Unchanged; continue; }
			if (previousText.empty()) { cell.change = SheetCellChangeMark::Added; continue; }
			cell.change = SheetCellChangeMark::Changed;
			cell.previous_text = std::move(previousText);
		}
	}
	// In the previous version's own order, row by row.
	for (const SheetRenderRow &row : before->rows) {
		for (const SheetRenderCell &cell : row.cells) {
			if (cell.text.empty() || previousCells.count(cell_key(row.index, cell.column)) == 0) continue;
			sheet.removed_cells.push_back(SheetRemovedCell{cell.ref, cell.text});
		}
	}
}

} // namespace

void mark_document_changes(DocumentRenderModel &current, const DocumentRenderModel &previous,
		int previous_version) {
	// A workbook is compared cell by cell, never paragraph by paragraph. With no
	// workbook on either side - one that could not be read - there is nothing to
	// compare, and every mark stays Unknown.
	if (current.workbook.has_value() || previous.workbook.has_value()) {
		if (!current.workbook.has_value() || !previous.workbook.has_value()) return;
		const bool isDelimited = current.format == DocumentFormat::csv || current.format == DocumentFormat::tsv;
		for (SheetRender &sheet : current.workbook->sheets)
			mark_sheet_changes(sheet, previous_sheet_for(sheet, *previous.workbook, isDelimited));
		current.compared_to_version = previous_version;
		return;
	}

	if (previous.blocks.empty() && current.blocks.empty()) return;
	if (previous.status != ExtractionStatus::ready || current.status != ExtractionStatus::ready) return;

	// The body first. Paragraphs that stand in the shared run are unchanged;
	// the rest are what the change touched, and an unmatched paragraph that
	// sits where an unmatched one used to sit is the same paragraph reworded.
	std::vector<std::size_t> currentParagraphBlocks;
	std::vector<std::string> currentTexts;
	for (std::size_t index = 0; index < current.blocks.size(); ++index) {
		if (current.blocks[index].kind != DocumentRenderBlockKind::Paragraph) continue;
		currentParagraphBlocks.push_back(index);
		currentTexts.push_back(comparable(current.blocks[index].paragraph.text()));
	}
	std::vector<std::string> previousTexts;
	for (const DocumentRenderBlock &block : previous.blocks) {
		if (block.kind != DocumentRenderBlockKind::Paragraph) continue;
		previousTexts.push_back(comparable(block.paragraph.text()));
	}
	const auto shared = longest_common_paragraphs(currentTexts, previousTexts);
	std::vector<bool> currentMatched(currentTexts.size(), false);
	std::vector<bool> previousMatched(previousTexts.size(), false);
	for (const auto &[currentIndex, previousIndex] : shared) {
		currentMatched[currentIndex] = true;
		previousMatched[previousIndex] = true;
	}
	// How far into the previous version each unmatched current paragraph sits,
	// so a reworded paragraph is paired with the one it replaced rather than
	// with the first unmatched one anywhere in the document.
	// The previous version's paragraphs in order, so a matched one can be asked
	// how it was written.
	std::vector<const DocumentRenderParagraph *> previousParagraphs;
	for (const DocumentRenderBlock &block : previous.blocks)
		if (block.kind == DocumentRenderBlockKind::Paragraph) previousParagraphs.push_back(&block.paragraph);
	std::size_t previousCursor = 0;
	std::size_t sharedCursor = 0;
	for (std::size_t index = 0; index < currentTexts.size(); ++index) {
		DocumentRenderParagraph &paragraph = current.blocks[currentParagraphBlocks[index]].paragraph;
		if (currentMatched[index]) {
			// The same words, written differently, is a change - it is what "make
			// that line italic" DOES. Without this, a formatting change read as
			// "nothing changed since version N", which is worse than saying
			// nothing at all.
			const std::size_t previousIndex = sharedCursor < shared.size() && shared[sharedCursor].first == index
				? shared[sharedCursor].second : previousParagraphs.size();
			const bool writtenDifferently = previousIndex < previousParagraphs.size()
				&& formatting_signature(paragraph) != formatting_signature(*previousParagraphs[previousIndex]);
			paragraph.change = writtenDifferently
				? DocumentRenderChangeMark::Changed : DocumentRenderChangeMark::Unchanged;
			// Its words are what they were, so what it said before is what it
			// says now: a reader of the previous version sees the paragraph, and
			// nothing in it reads as added.
			if (writtenDifferently) paragraph.previous_text = previousParagraphs[previousIndex]->text();
			if (sharedCursor < shared.size() && shared[sharedCursor].first == index)
				previousCursor = shared[sharedCursor++].second + 1;
			continue;
		}
		// The nearest unmatched previous paragraph at or after where this one
		// stands: the one it took the place of, if any.
		std::size_t candidate = previousTexts.size();
		for (std::size_t look = previousCursor; look < previousTexts.size(); ++look) {
			if (previousMatched[look]) break;
			candidate = look;
			break;
		}
		if (candidate < previousTexts.size() && !currentTexts[index].empty()) {
			paragraph.change = DocumentRenderChangeMark::Changed;
			// The previous wording, as the previous version itself wrote it -
			// not the comparable form used to decide.
			std::size_t seen = 0;
			for (const DocumentRenderBlock &block : previous.blocks) {
				if (block.kind != DocumentRenderBlockKind::Paragraph) continue;
				if (seen++ != candidate) continue;
				paragraph.previous_text = block.paragraph.text();
				break;
			}
			previousMatched[candidate] = true;
			previousCursor = candidate + 1;
			continue;
		}
		paragraph.change = DocumentRenderChangeMark::Added;
	}
	// What the previous version said and this one no longer does.
	{
		std::size_t seen = 0;
		for (const DocumentRenderBlock &block : previous.blocks) {
			if (block.kind != DocumentRenderBlockKind::Paragraph) continue;
			const std::size_t index = seen++;
			if (previousMatched[index]) continue;
			const std::string text = block.paragraph.text();
			if (!comparable(text).empty()) current.removed_texts.push_back(text);
		}
	}

	// Then the tables, row by row and cell by cell. A row is addressed the way a
	// change addresses it - by what its first cell says - so the row a lawyer
	// sees marked is the row that was named.
	for (DocumentRenderBlock &block : current.blocks) {
		if (block.kind != DocumentRenderBlockKind::Table) continue;
		const DocumentRenderTable *before = nullptr;
		for (const DocumentRenderBlock &previousBlock : previous.blocks) {
			if (previousBlock.kind != DocumentRenderBlockKind::Table) continue;
			if (previousBlock.table.table_id == block.table.table_id) { before = &previousBlock.table; break; }
		}
		if (before == nullptr) {
			for (DocumentRenderRow &row : block.table.rows) {
				row.change = DocumentRenderChangeMark::Added;
				for (DocumentRenderCell &cell : row.cells) cell.change = DocumentRenderChangeMark::Added;
			}
			continue;
		}
		std::vector<bool> previousRowMatched(before->rows.size(), false);
		for (DocumentRenderRow &row : block.table.rows) {
			if (row_is_struck(row)) continue;
			const std::string key = row_key_of(row);
			const DocumentRenderRow *previousRow = nullptr;
			for (std::size_t index = 0; index < before->rows.size(); ++index) {
				if (previousRowMatched[index] || row_is_struck(before->rows[index])
						|| row_key_of(before->rows[index]) != key) continue;
				previousRowMatched[index] = true;
				previousRow = &before->rows[index];
				break;
			}
			if (previousRow == nullptr) {
				row.change = DocumentRenderChangeMark::Added;
				for (DocumentRenderCell &cell : row.cells) cell.change = DocumentRenderChangeMark::Added;
				continue;
			}
			row.change = DocumentRenderChangeMark::Unchanged;
			for (DocumentRenderCell &cell : row.cells) {
				const DocumentRenderCell *previousCell = nullptr;
				for (const DocumentRenderCell &candidate : previousRow->cells)
					if (candidate.column_index == cell.column_index) { previousCell = &candidate; break; }
				if (previousCell == nullptr) { cell.change = DocumentRenderChangeMark::Added; continue; }
				if (cell_text_of(*previousCell) == cell_text_of(cell)) {
					// Same words, written differently: still a change.
					if (cell_formatting_signature(*previousCell) != cell_formatting_signature(cell)) {
						cell.change = DocumentRenderChangeMark::Changed;
						cell.previous_text = previous_cell_text(*previousCell);
						row.change = DocumentRenderChangeMark::Changed;
					} else {
						cell.change = DocumentRenderChangeMark::Unchanged;
					}
					continue;
				}
				cell.change = DocumentRenderChangeMark::Changed;
				cell.previous_text = previous_cell_text(*previousCell);
				row.change = DocumentRenderChangeMark::Changed;
			}
		}
		for (std::size_t index = 0; index < before->rows.size(); ++index) {
			if (previousRowMatched[index] || row_is_struck(before->rows[index])) continue;
			const std::string key = row_key_of(before->rows[index]);
			if (!key.empty()) current.removed_texts.push_back(key);
		}
	}

	current.compared_to_version = previous_version;
}

} // namespace singularity::documents
