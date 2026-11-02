#include "WorkProduct/ReviewTableExport/ReviewTableExport.hpp"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>

namespace singularity::review_table_export {
namespace {

std::string xml_of(const pugi::xml_document &document) {
	std::ostringstream stream;
	document.save(stream, "", pugi::format_raw, pugi::encoding_utf8);
	return stream.str();
}

// XML 1.0 holds no control characters but tab, newline and return.
std::string office_text(const std::string &text) {
	std::string clean;
	clean.reserve(text.size());
	for (const char character : text) {
		const unsigned char byte = static_cast<unsigned char>(character);
		if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') continue;
		clean += character;
	}
	return clean;
}

// A package written whole to a file of its own, then read back as bytes.
class Package {
public:
	Package() {
		std::random_device seed;
		path_ = std::filesystem::temp_directory_path()
			/ ("singularity-review-" + std::to_string(seed()) + "-" + std::to_string(seed()) + ".zip");
		int error = 0;
		archive_ = zip_open(path_.u8string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
		if (archive_ == nullptr) throw std::runtime_error("Singularity could not make the file.");
	}
	~Package() {
		if (archive_ != nullptr) zip_discard(archive_);
		std::error_code ignored;
		std::filesystem::remove(path_, ignored);
	}
	Package(const Package &) = delete;
	Package &operator=(const Package &) = delete;

	void add(const std::string &name, const std::string &value) {
		void *owned = std::malloc(value.empty() ? 1 : value.size());
		if (owned == nullptr) throw std::runtime_error("Singularity could not make the file.");
		if (!value.empty()) std::memcpy(owned, value.data(), value.size());
		zip_source_t *source = zip_source_buffer(archive_, owned, value.size(), 1);
		if (source == nullptr || zip_file_add(archive_, name.c_str(), source, ZIP_FL_OVERWRITE) < 0) {
			if (source != nullptr) zip_source_free(source);
			else std::free(owned);
			throw std::runtime_error("Singularity could not make the file.");
		}
	}

	std::string bytes() {
		zip_t *archive = archive_;
		archive_ = nullptr;
		if (zip_close(archive) != 0) {
			zip_discard(archive);
			throw std::runtime_error("Singularity could not finish the file.");
		}
		std::ifstream input(path_, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	}

private:
	std::filesystem::path path_;
	zip_t *archive_ = nullptr;
};

const matters::ReviewRow *row_of(const std::vector<matters::ReviewRow> &rows, const std::string &document_id) {
	for (const matters::ReviewRow &row : rows)
		if (row.document_id == document_id) return &row;
	return nullptr;
}

const matters::ReviewCell *cell_of(const matters::ReviewRow *row, const std::string &column_id) {
	if (row == nullptr) return nullptr;
	for (const matters::ReviewCell &cell : row->cells)
		if (cell.column_id == column_id) return &cell;
	return nullptr;
}

std::string status_of(const std::vector<review_table::RowStatus> &statuses, const std::string &document_id) {
	for (const review_table::RowStatus &status : statuses)
		if (status.document_id == document_id) return status.status;
	return "missing";
}

// What the answer rests on, in words: checked, or not.
std::string cell_state(const matters::ReviewCell *cell, const std::string &row_status) {
	if (row_status == "stale") return "Document changed since";
	if (cell == nullptr) return row_status == "unavailable" ? "Document unavailable" : "Not answered yet";
	if (cell->state == "not_found") return "Not addressed";
	if (cell->state != "answered") return "No answer";
	if (!cell->evidence.empty() && cell->evidence.front().verified)
		return cell->evidence.front().aligned ? "Quote found in the document (shown in the document's own words)"
			: "Quote found in the document";
	return "Quote not found in the document";
}

std::string location_of(const matters::ReviewEvidence &evidence) {
	std::string where;
	if (evidence.page) where = "p. " + std::to_string(*evidence.page);
	if (!evidence.heading.empty()) where += (where.empty() ? "" : ", ") + evidence.heading;
	return where;
}

std::string column_letters(std::size_t index) {
	std::string letters;
	++index;
	while (index > 0) {
		const std::size_t remainder = (index - 1) % 26;
		letters.insert(letters.begin(), static_cast<char>('A' + remainder));
		index = (index - 1) / 26;
	}
	return letters;
}

constexpr const char *kStyles =
	R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
	R"(<fonts count="2"><font><sz val="11"/><name val="Calibri"/><family val="2"/></font><font><b/><sz val="11"/><name val="Calibri"/><family val="2"/></font></fonts>)"
	R"(<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>)"
	R"(<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>)"
	R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)"
	R"(<cellXfs count="3"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)"
	R"(<xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf>)"
	R"(<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf></cellXfs>)"
	R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>)";

// One worksheet: a bold header kept in view, wrapped cells, given widths.
std::string worksheet(const std::vector<std::vector<std::string>> &rows, const std::vector<double> &widths) {
	pugi::xml_document sheet;
	pugi::xml_node root = sheet.append_child("worksheet");
	root.append_attribute("xmlns") = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
	pugi::xml_node view = root.append_child("sheetViews").append_child("sheetView");
	view.append_attribute("workbookViewId") = 0;
	pugi::xml_node pane = view.append_child("pane");
	pane.append_attribute("ySplit") = 1;
	pane.append_attribute("topLeftCell") = "A2";
	pane.append_attribute("activePane") = "bottomLeft";
	pane.append_attribute("state") = "frozen";
	pugi::xml_node columns = root.append_child("cols");
	for (std::size_t index = 0; index < widths.size(); ++index) {
		pugi::xml_node column = columns.append_child("col");
		column.append_attribute("min") = static_cast<unsigned int>(index + 1);
		column.append_attribute("max") = static_cast<unsigned int>(index + 1);
		column.append_attribute("width") = widths[index];
		column.append_attribute("customWidth") = 1;
	}
	pugi::xml_node data = root.append_child("sheetData");
	for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
		pugi::xml_node row = data.append_child("row");
		row.append_attribute("r") = static_cast<unsigned int>(rowIndex + 1);
		for (std::size_t columnIndex = 0; columnIndex < rows[rowIndex].size(); ++columnIndex) {
			const std::string value = office_text(rows[rowIndex][columnIndex]);
			if (value.empty()) continue;
			pugi::xml_node cell = row.append_child("c");
			cell.append_attribute("r") = (column_letters(columnIndex) + std::to_string(rowIndex + 1)).c_str();
			cell.append_attribute("s") = rowIndex == 0 ? 1 : 2;
			cell.append_attribute("t") = "inlineStr";
			pugi::xml_node text = cell.append_child("is").append_child("t");
			text.append_attribute("xml:space") = "preserve";
			text.text().set(value.c_str());
		}
	}
	if (rows.size() > 1 && !rows.front().empty()) {
		const std::string range = "A1:" + column_letters(rows.front().size() - 1) + std::to_string(rows.size());
		root.append_child("autoFilter").append_attribute("ref") = range.c_str();
	}
	return xml_of(sheet);
}

void add_paragraph(pugi::xml_node body, const std::string &text, const char *style = nullptr, bool bold = false) {
	pugi::xml_node paragraph = body.append_child("w:p");
	if (style != nullptr) paragraph.append_child("w:pPr").append_child("w:pStyle").append_attribute("w:val") = style;
	if (text.empty()) return;
	pugi::xml_node run = paragraph.append_child("w:r");
	if (bold) run.append_child("w:rPr").append_child("w:b");
	pugi::xml_node words = run.append_child("w:t");
	words.append_attribute("xml:space") = "preserve";
	words.text().set(office_text(text).c_str());
}

void add_cell(pugi::xml_node row, const std::string &text, int width, bool header) {
	pugi::xml_node cell = row.append_child("w:tc");
	pugi::xml_node properties = cell.append_child("w:tcPr");
	pugi::xml_node cellWidth = properties.append_child("w:tcW");
	cellWidth.append_attribute("w:w") = width;
	cellWidth.append_attribute("w:type") = "dxa";
	add_paragraph(cell, text, nullptr, header);
}

constexpr const char *kWordStyles =
	R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
	R"(<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:cs="Calibri"/><w:sz w:val="20"/></w:rPr></w:rPrDefault>)"
	R"(<w:pPrDefault><w:pPr><w:spacing w:after="80"/></w:pPr></w:pPrDefault></w:docDefaults>)"
	R"(<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>)"
	R"(<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:basedOn w:val="Normal"/><w:pPr><w:spacing w:after="120"/></w:pPr><w:rPr><w:b/><w:sz w:val="32"/></w:rPr></w:style>)"
	R"(<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:pPr><w:keepNext/><w:spacing w:before="240" w:after="120"/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/><w:sz w:val="26"/></w:rPr></w:style>)"
	R"(<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/><w:basedOn w:val="Normal"/><w:pPr><w:keepNext/><w:spacing w:before="200" w:after="80"/><w:outlineLvl w:val="1"/></w:pPr><w:rPr><w:b/><w:sz w:val="22"/></w:rPr></w:style>)"
	R"(<w:style w:type="paragraph" w:styleId="Quote"><w:name w:val="Quote"/><w:basedOn w:val="Normal"/><w:pPr><w:ind w:left="360"/></w:pPr><w:rPr><w:i/></w:rPr></w:style>)"
	R"(<w:style w:type="table" w:default="1" w:styleId="TableNormal"><w:name w:val="Normal Table"/><w:tblPr><w:tblCellMar><w:top w:w="40" w:type="dxa"/><w:left w:w="80" w:type="dxa"/><w:bottom w:w="40" w:type="dxa"/><w:right w:w="80" w:type="dxa"/></w:tblCellMar></w:tblPr></w:style>)"
	R"(</w:styles>)";

} // namespace

std::string cell_text(const matters::ReviewCell *cell, const std::string &row_status) {
	if (cell == nullptr) return row_status == "unavailable" ? "Document unavailable" : "Not answered yet";
	if (cell->state == "not_found") return cell->answer.empty() ? "Not addressed" : "Not addressed. " + cell->answer;
	if (cell->state != "answered") return "No answer";
	std::string text = cell->answer;
	if (!cell->assessment.empty()) {
		const std::string word = cell->assessment == "conforms" ? "Conforms to the firm's position"
			: cell->assessment == "deviates" ? "Deviates from the firm's position" : "Missing";
		text += "\n" + word + (cell->assessment_reason.empty() ? "" : ": " + cell->assessment_reason);
	}
	if (cell->evidence.empty() || !cell->evidence.front().verified) text += "\n(The quote was not found in the document.)";
	if (row_status == "stale") text += "\n(The document changed since this answer.)";
	return text;
}

std::string xlsx(const matters::ReviewTable &table, const std::vector<matters::ReviewRow> &rows,
		const std::vector<review_table::RowStatus> &statuses) {
	std::vector<std::vector<std::string>> review{{"Document"}};
	std::vector<double> reviewWidths{32};
	for (const matters::ReviewColumn &column : table.columns) {
		review.front().push_back(column.heading);
		reviewWidths.push_back(column.answer_kind == "clause" || column.answer_kind == "text" ? 46 : 28);
	}
	std::vector<std::vector<std::string>> evidence{{"Document", "Question", "Answer", "Value", "Status", "Quote",
		"Where", "Confidence", "Basis", "Firm position", "Assessment", "Why"}};
	for (const matters::ReviewDocument &document : table.documents) {
		const matters::ReviewRow *row = row_of(rows, document.document_id);
		const std::string status = status_of(statuses, document.document_id);
		std::vector<std::string> line{document.name};
		for (const matters::ReviewColumn &column : table.columns) {
			const matters::ReviewCell *cell = cell_of(row, column.id);
			line.push_back(cell_text(cell, status));
			const matters::ReviewEvidence *quoted = cell != nullptr && !cell->evidence.empty() ? &cell->evidence.front() : nullptr;
			evidence.push_back({document.name, column.question, cell != nullptr ? cell->answer : std::string(),
				cell != nullptr ? cell->value : std::string(), cell_state(cell, status),
				quoted != nullptr ? quoted->quote : std::string(), quoted != nullptr ? location_of(*quoted) : std::string(),
				cell != nullptr ? cell->confidence : std::string(), cell != nullptr ? cell->basis : std::string(),
				column.position, cell != nullptr ? cell->assessment : std::string(),
				cell != nullptr ? (cell->assessment_reason.empty() ? cell->reasoning : cell->assessment_reason) : std::string()});
		}
		review.push_back(std::move(line));
	}
	Package package;
	package.add("[Content_Types].xml",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/></Types>)");
	package.add("_rels/.rels",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)");
	package.add("xl/workbook.xml",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Review" sheetId="1" r:id="rId1"/><sheet name="Evidence" sheetId="2" r:id="rId2"/></sheets></workbook>)");
	package.add("xl/_rels/workbook.xml.rels",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>)");
	package.add("xl/styles.xml", kStyles);
	package.add("xl/worksheets/sheet1.xml", worksheet(review, reviewWidths));
	package.add("xl/worksheets/sheet2.xml", worksheet(evidence, {28, 40, 40, 20, 24, 60, 20, 12, 10, 40, 14, 50}));
	return package.bytes();
}

std::string docx(const matters::ReviewTable &table, const std::vector<matters::ReviewRow> &rows,
		const std::vector<review_table::RowStatus> &statuses) {
	// Wide tables are set on a landscape page.
	const bool landscape = table.columns.size() > 3;
	const int pageWidth = landscape ? 15840 : 12240;
	const int pageHeight = landscape ? 12240 : 15840;
	const int margin = 1080;
	const int textWidth = pageWidth - 2 * margin;
	pugi::xml_document document;
	pugi::xml_node declaration = document.append_child(pugi::node_declaration);
	declaration.append_attribute("version") = "1.0";
	declaration.append_attribute("encoding") = "UTF-8";
	declaration.append_attribute("standalone") = "yes";
	pugi::xml_node root = document.append_child("w:document");
	root.append_attribute("xmlns:w") = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
	pugi::xml_node body = root.append_child("w:body");
	add_paragraph(body, table.title, "Title");
	std::string about = std::to_string(table.documents.size())
		+ (table.documents.size() == 1 ? " document, " : " documents, ") + std::to_string(table.columns.size())
		+ (table.columns.size() == 1 ? " question." : " questions.");
	if (!table.model_id.empty()) about += " Answered by " + table.model_id + ".";
	about += " Every answer quotes the document; Singularity checked each quote against the document itself.";
	add_paragraph(body, about);

	pugi::xml_node grid = body.append_child("w:tbl");
	pugi::xml_node properties = grid.append_child("w:tblPr");
	pugi::xml_node width = properties.append_child("w:tblW");
	width.append_attribute("w:w") = 5000;
	width.append_attribute("w:type") = "pct";
	pugi::xml_node borders = properties.append_child("w:tblBorders");
	for (const char *side : {"w:top", "w:left", "w:bottom", "w:right", "w:insideH", "w:insideV"}) {
		pugi::xml_node border = borders.append_child(side);
		border.append_attribute("w:val") = "single";
		border.append_attribute("w:sz") = 4;
		border.append_attribute("w:color") = "BFBFBF";
	}
	properties.append_child("w:tblLayout").append_attribute("w:type") = "fixed";
	const int firstWidth = std::max(1800, textWidth / static_cast<int>(table.columns.size() + 2));
	const int otherWidth = (textWidth - firstWidth) / static_cast<int>(std::max<std::size_t>(1, table.columns.size()));
	pugi::xml_node columns = grid.append_child("w:tblGrid");
	columns.append_child("w:gridCol").append_attribute("w:w") = firstWidth;
	for (std::size_t index = 0; index < table.columns.size(); ++index)
		columns.append_child("w:gridCol").append_attribute("w:w") = otherWidth;
	pugi::xml_node header = grid.append_child("w:tr");
	header.append_child("w:trPr").append_child("w:tblHeader");
	add_cell(header, "Document", firstWidth, true);
	for (const matters::ReviewColumn &column : table.columns) add_cell(header, column.heading, otherWidth, true);
	for (const matters::ReviewDocument &entry : table.documents) {
		const matters::ReviewRow *row = row_of(rows, entry.document_id);
		const std::string status = status_of(statuses, entry.document_id);
		pugi::xml_node line = grid.append_child("w:tr");
		add_cell(line, entry.name, firstWidth, false);
		for (const matters::ReviewColumn &column : table.columns)
			add_cell(line, cell_text(cell_of(row, column.id), status), otherWidth, false);
	}

	add_paragraph(body, "Evidence", "Heading1");
	for (const matters::ReviewDocument &entry : table.documents) {
		const matters::ReviewRow *row = row_of(rows, entry.document_id);
		const std::string status = status_of(statuses, entry.document_id);
		add_paragraph(body, entry.name, "Heading2");
		for (const matters::ReviewColumn &column : table.columns) {
			const matters::ReviewCell *cell = cell_of(row, column.id);
			add_paragraph(body, column.question, nullptr, true);
			add_paragraph(body, cell_text(cell, status));
			if (cell == nullptr || cell->evidence.empty() || cell->evidence.front().quote.empty()) continue;
			const matters::ReviewEvidence &quoted = cell->evidence.front();
			add_paragraph(body, "\xE2\x80\x9C" + quoted.quote + "\xE2\x80\x9D", "Quote");
			const std::string where = location_of(quoted);
			add_paragraph(body, (where.empty() ? std::string() : where + ". ") + cell_state(cell, status) + ".");
		}
	}
	pugi::xml_node section = body.append_child("w:sectPr");
	pugi::xml_node size = section.append_child("w:pgSz");
	size.append_attribute("w:w") = pageWidth;
	size.append_attribute("w:h") = pageHeight;
	if (landscape) size.append_attribute("w:orient") = "landscape";
	pugi::xml_node margins = section.append_child("w:pgMar");
	for (const char *side : {"w:top", "w:right", "w:bottom", "w:left"}) margins.append_attribute(side) = margin;
	margins.append_attribute("w:header") = 720;
	margins.append_attribute("w:footer") = 720;
	margins.append_attribute("w:gutter") = 0;

	Package package;
	package.add("[Content_Types].xml",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/></Types>)");
	package.add("_rels/.rels",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)");
	package.add("word/_rels/document.xml.rels",
		R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>)");
	package.add("word/styles.xml", kWordStyles);
	package.add("word/document.xml", xml_of(document));
	return package.bytes();
}

} // namespace singularity::review_table_export
