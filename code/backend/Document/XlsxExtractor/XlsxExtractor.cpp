#include "Document/XlsxExtractor/XlsxExtractor.hpp"

#include "Document/XlsxWorkbookView/XlsxWorkbookView.hpp"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace singularity::documents {
namespace {

constexpr std::string_view kExtractorVersion = "xlsx-spreadsheetml/3";
// The workbook reading passages were cited from on 24 Sep 2026, before a
// formula said which hidden rows and columns it counts. Kept so such a
// citation still shows the very passage it was given; new work never reads it.
constexpr std::string_view kWorkbookReadingV2Version = "xlsx-spreadsheetml/2";
// The reading every workbook passage cited before 24 Sep 2026 was cut from:
// one "A1<tab>value" line per cell, stored values only. Kept so such a citation
// still shows the very passage it was given; new work never reads it.
constexpr std::string_view kCellsReadingVersion = "xlsx-spreadsheetml/1";
constexpr std::uint64_t kMaximumXmlBytes = 24ULL * 1024ULL * 1024ULL;
constexpr zip_int64_t kMaximumArchiveEntries = 4096;
constexpr std::size_t kMaximumCellsPerSheet = 200000;

class XlsxError final : public std::runtime_error {
public:
	XlsxError(std::string errorCode, std::string publicMessage)
			: std::runtime_error(publicMessage), code(std::move(errorCode)) {}
	std::string code;
};

class ZipArchive final {
public:
	explicit ZipArchive(const std::filesystem::path &path) {
		int errorCode = 0;
		archive_ = zip_open(path.u8string().c_str(), ZIP_RDONLY, &errorCode);
		if (archive_ == nullptr) {
			throw XlsxError("xlsx_invalid_archive", "The XLSX package could not be opened as a ZIP archive.");
		}
	}
	ZipArchive(const ZipArchive &) = delete;
	ZipArchive &operator=(const ZipArchive &) = delete;
	~ZipArchive() {
		if (archive_ != nullptr) zip_discard(archive_);
	}
	[[nodiscard]] zip_t *get() const noexcept { return archive_; }

private:
	zip_t *archive_ = nullptr;
};

std::string_view local_name(std::string_view qualifiedName) {
	const auto separator = qualifiedName.find(':');
	return separator == std::string_view::npos ? qualifiedName : qualifiedName.substr(separator + 1);
}

bool is_element(const pugi::xml_node &node, std::string_view name) {
	return node.type() == pugi::node_element && local_name(node.name()) == name;
}

std::string attribute_value(const pugi::xml_node &node, std::string_view name) {
	for (const auto &attribute : node.attributes()) {
		if (local_name(attribute.name()) == name) return attribute.value();
	}
	return {};
}

std::string trim_ascii(std::string value) {
	const auto notSpace = [](unsigned char byte) { return std::isspace(byte) == 0; };
	const auto first = std::find_if(value.begin(), value.end(), notSpace);
	const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
	if (first >= last) return {};
	return std::string(first, last);
}

std::optional<std::string> read_entry_by_name(zip_t *archive, const std::string &name, std::uint64_t maximumBytes) {
	zip_stat_t entry{};
	zip_stat_init(&entry);
	if (zip_stat(archive, name.c_str(), 0, &entry) != 0) return std::nullopt;
	if ((entry.valid & ZIP_STAT_SIZE) == 0) return std::nullopt;
	if (entry.size > maximumBytes
			|| entry.size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw XlsxError("xlsx_xml_part_size_limit", "An XLSX XML part exceeds the read limit.");
	}
	zip_file_t *rawFile = zip_fopen(archive, name.c_str(), 0);
	if (rawFile == nullptr) return std::nullopt;
	std::string bytes(static_cast<std::size_t>(entry.size), '\0');
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto read = zip_fread(rawFile, bytes.data() + offset,
				static_cast<zip_uint64_t>(bytes.size() - offset));
		if (read <= 0) break;
		offset += static_cast<std::size_t>(read);
	}
	const auto closeResult = zip_fclose(rawFile);
	if (closeResult != 0 || offset != bytes.size()) {
		throw XlsxError("xlsx_xml_read_failed", "An XLSX XML part could not be read completely.");
	}
	return bytes;
}

pugi::xml_document parse_xml(const std::string &bytes, const char *diagnosticCode, const char *publicMessage) {
	if (bytes.find("<!DOCTYPE") != std::string::npos || bytes.find("<!ENTITY") != std::string::npos) {
		throw XlsxError("xlsx_unsafe_xml", "DTD and entity declarations are not accepted in XLSX XML.");
	}
	pugi::xml_document document;
	const auto parsed = document.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8);
	if (!parsed) throw XlsxError(diagnosticCode, publicMessage);
	return document;
}

std::vector<std::string> read_shared_strings(zip_t *archive) {
	std::vector<std::string> sharedStrings;
	const auto sharedStringsXml = read_entry_by_name(archive, "xl/sharedStrings.xml", kMaximumXmlBytes);
	if (!sharedStringsXml.has_value()) return sharedStrings;
	const auto document =
			parse_xml(*sharedStringsXml, "xlsx_malformed_shared_strings", "xl/sharedStrings.xml is malformed.");
	for (const auto &sst : document.children()) {
		if (!is_element(sst, "sst")) continue;
		for (const auto &si : sst.children()) {
			if (!is_element(si, "si")) continue;
			std::string text;
			for (const auto &node : si.children()) {
				if (is_element(node, "t")) {
					text += node.child_value();
				} else if (is_element(node, "r")) {
					for (const auto &run : node.children()) {
						if (is_element(run, "t")) text += run.child_value();
					}
				}
			}
			sharedStrings.push_back(std::move(text));
		}
	}
	return sharedStrings;
}

// The real workbook-order worksheet part list: xl/workbook.xml's <sheets>
// gives ordered relationship ids, xl/_rels/workbook.xml.rels resolves each
// to its worksheets/sheetN.xml target and each entry's own <sheet name="..">
// attribute gives its display name - never assumed from sheetN.xml file-name
// order.
struct WorksheetPart {
	std::string name;
	std::string part;
};

std::vector<WorksheetPart> ordered_worksheet_parts(zip_t *archive) {
	const auto workbookXml = read_entry_by_name(archive, "xl/workbook.xml", kMaximumXmlBytes);
	if (!workbookXml.has_value()) {
		throw XlsxError("xlsx_missing_workbook_xml", "The XLSX package does not contain xl/workbook.xml.");
	}
	const auto workbookDocument =
			parse_xml(*workbookXml, "xlsx_malformed_workbook_xml", "xl/workbook.xml is malformed.");

	std::vector<std::pair<std::string, std::string>> sheetsInOrder; // name, relationship id
	for (const auto &workbook : workbookDocument.children()) {
		if (!is_element(workbook, "workbook")) continue;
		for (const auto &sheets : workbook.children()) {
			if (!is_element(sheets, "sheets")) continue;
			for (const auto &sheet : sheets.children()) {
				if (!is_element(sheet, "sheet")) continue;
				std::string relationshipId;
				for (const auto &attribute : sheet.attributes()) {
					if (local_name(attribute.name()) == "id") relationshipId = attribute.value();
				}
				sheetsInOrder.emplace_back(attribute_value(sheet, "name"), relationshipId);
			}
		}
	}

	const auto relsXml = read_entry_by_name(archive, "xl/_rels/workbook.xml.rels", kMaximumXmlBytes);
	if (!relsXml.has_value()) {
		throw XlsxError("xlsx_missing_workbook_rels", "The XLSX package does not contain xl/_rels/workbook.xml.rels.");
	}
	const auto relsDocument =
			parse_xml(*relsXml, "xlsx_malformed_workbook_rels", "xl/_rels/workbook.xml.rels is malformed.");
	std::unordered_map<std::string, std::string> targetByRelationshipId;
	for (const auto &relationships : relsDocument.children()) {
		if (!is_element(relationships, "Relationships")) continue;
		for (const auto &relationship : relationships.children()) {
			if (!is_element(relationship, "Relationship")) continue;
			targetByRelationshipId[attribute_value(relationship, "Id")] = attribute_value(relationship, "Target");
		}
	}

	std::vector<WorksheetPart> parts;
	for (const auto &[name, relationshipId] : sheetsInOrder) {
		const auto found = targetByRelationshipId.find(relationshipId);
		if (found == targetByRelationshipId.end()) {
			throw XlsxError("xlsx_unresolved_sheet_relationship", "An XLSX sheet relationship could not be resolved.");
		}
		// A relationship Target is usually package-relative to xl/ (e.g.
		// "worksheets/sheet1.xml"), but OPC also allows a package-absolute
		// target (a leading '/', resolved from the package root, e.g.
		// "/xl/worksheets/sheet1.xml") - a real, valid, spec-conformant form
		// at least one common real-world generator (openpyxl) actually emits
		// for this exact relationship. Naively prepending "xl/" to that form
		// produced "xl//xl/worksheets/sheet1.xml" - never a real zip entry -
		// silently discarding every real worksheet and returning "Text
		// couldn't be extracted." for a genuinely valid, non-corrupt XLSX
		// file. Caught by a real openpyxl-generated fixture, not a
		// hypothetical.
		const std::string &target = found->second;
		const std::string resolvedTarget = !target.empty() && target.front() == '/'
			? target.substr(1) : "xl/" + target;
		parts.push_back({name, resolvedTarget});
	}
	return parts;
}

DocumentSheet read_worksheet(const pugi::xml_document &sheetDocument, const std::string &sheetId,
		const std::vector<std::string> &sharedStrings) {
	DocumentSheet sheet;
	sheet.sheet_id = sheetId;

	pugi::xml_node sheetData;
	for (const auto &worksheet : sheetDocument.children()) {
		if (!is_element(worksheet, "worksheet")) continue;
		for (const auto &child : worksheet.children()) {
			if (is_element(child, "sheetData")) sheetData = child;
		}
	}
	if (!sheetData) return sheet;

	for (const auto &row : sheetData.children()) {
		if (!is_element(row, "row")) continue;
		for (const auto &cell : row.children()) {
			if (!is_element(cell, "c")) continue;
			if (sheet.cells.size() >= kMaximumCellsPerSheet) {
				throw XlsxError("xlsx_cell_count_limit", "The worksheet exceeds the safety cell-count limit.");
			}
			const std::string cellRef = attribute_value(cell, "r");
			if (cellRef.empty()) continue;
			const std::string cellType = attribute_value(cell, "t");

			std::string text;
			if (cellType == "inlineStr") {
				for (const auto &is : cell.children()) {
					if (!is_element(is, "is")) continue;
					for (const auto &node : is.children()) {
						if (is_element(node, "t")) text += node.child_value();
					}
				}
			} else {
				pugi::xml_node valueNode;
				for (const auto &child : cell.children()) {
					if (is_element(child, "v")) valueNode = child;
				}
				if (!valueNode) continue;
				const std::string rawValue = valueNode.child_value();
				if (cellType == "s") {
					std::size_t index = 0;
					try {
						index = static_cast<std::size_t>(std::stoul(rawValue));
					} catch (const std::exception &) {
						continue;
					}
					if (index < sharedStrings.size()) text = sharedStrings[index];
				} else {
					text = rawValue;
				}
			}
			text = trim_ascii(std::move(text));
			if (text.empty()) continue;
			sheet.cells.push_back({cellRef, text});
		}
	}
	return sheet;
}

// ---- The model's reading of a workbook --------------------------------------
//
// A workbook reaches the model the way the lawyer's Excel lays it out, read by
// the one workbook engine (XlsxWorkbookView): every sheet in workbook order,
// hidden ones said to be hidden, one line per row, each cell by its reference.
// A number is given as the value the workbook stores - "B6 = -4320000 (shown in
// $)" - with what its format makes of it in words, never in formatted digits:
// Papinian Guard then protects a workbook's numbers exactly as it did when the
// model read stored values alone, and no rounded display ever hides a value
// (a ratio shown as 3.0x may be 2.996). A date is shown as the date it is, a
// formula stands in braces after the value it last saved - or, where the file
// keeps none, after "calculated when opened in Excel" - and a merged or
// hidden cell says so. Every passage opens with its sheet, its rows and the
// sheet's header row, so a passage read alone still says what its numbers are.
//
// Until 24 Sep 2026 the model read "B6<tab>-4320000": no sheet, no header, no
// formula, no format, and hidden rows among the visible ones.

// A passage's rows stop short of the chunker's 4096 bytes (DocumentChunker),
// room left for the line that opens the passage.
constexpr std::size_t kPassageRowBytes = 3000;
constexpr std::size_t kHeaderLineBytes = 600;
constexpr std::size_t kFormulaCharacters = 160;

std::string column_letters(std::size_t column) {
	std::string letters;
	std::size_t value = column + 1;
	while (value > 0) {
		letters.insert(letters.begin(), static_cast<char>('A' + (value - 1) % 26));
		value = (value - 1) / 26;
	}
	return letters;
}

// A cell's text on one line: line breaks and tabs become spaces, a run of
// spaces becomes one, and the thin and figure spaces a number format pads
// with go.
std::string one_line(const std::string &text) {
	std::string result;
	bool pendingSpace = false;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text.compare(index, 3, "\xE2\x80\x89") == 0 || text.compare(index, 3, "\xE2\x80\x87") == 0) {
			index += 2;
			continue;
		}
		const char character = text[index];
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
			pendingSpace = result.empty() == false;
			continue;
		}
		if (pendingSpace == true) result.push_back(' ');
		pendingSpace = false;
		result.push_back(character);
	}
	return result;
}

// What a number's format makes of it, in words and never in digits: the
// currency it is shown in, a scale, a percentage or a multiple. Read from the
// format's first section, its quoted text and escapes aside.
std::string number_words(const std::string &format) {
	std::string currency;
	std::size_t trailingCommas = 0;
	bool sawDigit = false;
	bool percent = false;
	bool multiple = false;
	for (std::size_t index = 0; index < format.size(); ++index) {
		const char character = format[index];
		if (character == ';') break;
		if (character == '"') {
			const std::size_t end = format.find('"', index + 1);
			const std::string literal = one_line(format.substr(index + 1, end == std::string::npos ? std::string::npos : end - index - 1));
			if (literal == "x" || literal == "X" || literal == "\xC3\x97") multiple = true;
			// A currency written as quoted text, the way Excel's accounting
			// formats write the dollar sign: _("$"* #,##0_).
			for (const char *symbol : {"$", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD", "USD", "EUR", "GBP", "RUB",
					"\xD1\x80\xD1\x83\xD0\xB1.", "\xD1\x80."})
				if (literal == symbol) currency = literal;
			index = end == std::string::npos ? format.size() : end;
			continue;
		}
		if (character == '\\') {
			if (index + 1 < format.size() && (format[index + 1] == 'x' || format[index + 1] == 'X')) multiple = true;
			if (index + 1 < format.size() && format[index + 1] == '$') currency = "$";
			++index;
			continue;
		}
		if (character == '[') {
			const std::size_t end = format.find(']', index);
			const std::string inner = format.substr(index + 1, end == std::string::npos ? std::string::npos : end - index - 1);
			if (inner.size() > 1 && inner[0] == '$') {
				const std::string symbol = inner.substr(1, inner.find('-') == std::string::npos ? std::string::npos : inner.find('-') - 1);
				if (symbol.empty() == false) currency = symbol;
			}
			index = end == std::string::npos ? format.size() : end;
			continue;
		}
		if (character == '_' || character == '*') {
			++index;
			continue;
		}
		if (character == '%') percent = true;
		else if (character == '$') currency = "$";
		else if (format.compare(index, 3, "\xE2\x82\xAC") == 0) { currency = "\xE2\x82\xAC"; index += 2; }
		else if (format.compare(index, 2, "\xC2\xA3") == 0) { currency = "\xC2\xA3"; index += 1; }
		else if (format.compare(index, 2, "\xC2\xA5") == 0) { currency = "\xC2\xA5"; index += 1; }
		else if (format.compare(index, 3, "\xE2\x82\xBD") == 0) { currency = "\xE2\x82\xBD"; index += 2; }
		else if (character == '0' || character == '#' || character == '?') { sawDigit = true; trailingCommas = 0; }
		else if (character == ',' && sawDigit == true) ++trailingCommas;
	}
	std::vector<std::string> words;
	if (percent == true) words.push_back("as a percentage");
	if (currency.empty() == false) words.push_back("in " + currency);
	if (trailingCommas == 1) words.push_back("in thousands");
	else if (trailingCommas == 2) words.push_back("in millions");
	else if (trailingCommas >= 3) words.push_back("in billions");
	if (multiple == true) words.push_back("as a multiple");
	if (words.empty() == true) return {};
	std::string result = "shown";
	for (const std::string &word : words) result += " " + word;
	return result;
}

std::string stored_number(const SheetRenderCell &cell) {
	if (cell.stored_value.empty() == false) return cell.stored_value;
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << std::setprecision(17) << cell.number;
	return out.str();
}

// A cell's value as the model reads it, without its reference or formula.
std::string cell_value_reading(const SheetRenderCell &cell) {
	switch (cell.kind) {
		case SheetCellKind::Number: {
			const std::string words = number_words(cell.number_format);
			return stored_number(cell) + (words.empty() == true ? std::string() : " (" + words + ")");
		}
		case SheetCellKind::Date:
		case SheetCellKind::Text:
		case SheetCellKind::Boolean:
		case SheetCellKind::Error:
			return one_line(cell.fill.has_value() == true ? cell.fill->before + cell.fill->after : cell.text);
		case SheetCellKind::Empty:
			break;
	}
	return {};
}

bool column_hidden(const SheetRender &sheet, std::size_t column) {
	for (const SheetRenderColumn &range : sheet.columns)
		if (range.hidden == true && column >= range.first && column <= range.last) return true;
	return false;
}

// The row that names the sheet's columns: the last frozen row, or else the
// first of its first ten rows whose cells - two or more - all hold text.
const SheetRenderRow *header_row(const SheetRender &sheet) {
	if (sheet.frozen_rows > 0) {
		for (const SheetRenderRow &row : sheet.rows)
			if (row.index + 1 == sheet.frozen_rows && row.cells.empty() == false) return &row;
	}
	for (const SheetRenderRow &row : sheet.rows) {
		if (row.index >= 10) break;
		std::size_t texts = 0;
		bool onlyText = true;
		for (const SheetRenderCell &cell : row.cells) {
			if (cell.kind == SheetCellKind::Empty && cell.formula.has_value() == false) continue;
			if (cell.kind == SheetCellKind::Text && one_line(cell.text).empty() == false) ++texts;
			else onlyText = false;
		}
		if (onlyText == true && texts >= 2) return &row;
	}
	return nullptr;
}

struct RowReading {
	std::size_t row_number = 0;
	std::string line;
	std::size_t cells = 0;
};

// The hidden rows and columns of `sheet` a formula counts, in words - "hidden
// row 6", "hidden rows 6 and 10", "hidden column C" - or nothing. A total that
// sums a range counts its hidden rows as well: a claim hidden because it was
// withdrawn still inflates the total a lawyer reads (Russian Golden, 24 Sep
// 2026), and nothing on the screen says so. Only this sheet's own ranges are
// read; a reference to another sheet names rows of that sheet.
std::string hidden_counted_by(const SheetRender &sheet, const std::string &formula, std::size_t own_row) {
	// The row a formula stands in is said to be hidden already, on its own line.
	std::set<std::size_t> hiddenRows;
	for (const SheetRenderRow &row : sheet.rows)
		if (row.hidden == true && row.index != own_row) hiddenRows.insert(row.index);
	std::set<std::size_t> hiddenColumns;
	for (const SheetRenderColumn &range : sheet.columns)
		if (range.hidden == true)
			for (std::size_t column = range.first; column <= range.last && column < range.first + 64; ++column)
				hiddenColumns.insert(column);
	if (hiddenRows.empty() == true && hiddenColumns.empty() == true) return {};

	const auto isWordByte = [](char character) {
		return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '.';
	};
	// "$C$12" at `at`: its column and row, and where it ends.
	const auto cellAt = [&](std::size_t at, std::size_t &column, std::size_t &row) -> std::size_t {
		std::size_t cursor = at;
		if (cursor < formula.size() && formula[cursor] == '$') ++cursor;
		const std::size_t lettersStart = cursor;
		while (cursor < formula.size() && std::isalpha(static_cast<unsigned char>(formula[cursor])) != 0
				&& cursor - lettersStart < 3)
			++cursor;
		if (cursor == lettersStart) return 0;
		column = 0;
		for (std::size_t index = lettersStart; index < cursor; ++index)
			column = column * 26 + static_cast<std::size_t>(std::toupper(static_cast<unsigned char>(formula[index])) - 'A' + 1);
		--column;
		if (cursor < formula.size() && formula[cursor] == '$') ++cursor;
		const std::size_t digitsStart = cursor;
		while (cursor < formula.size() && std::isdigit(static_cast<unsigned char>(formula[cursor])) != 0 && cursor - digitsStart < 7)
			++cursor;
		if (cursor == digitsStart) return 0;
		row = static_cast<std::size_t>(std::stoul(formula.substr(digitsStart, cursor - digitsStart)));
		if (row == 0) return 0;
		--row;
		if (cursor < formula.size() && (isWordByte(formula[cursor]) || formula[cursor] == '(' || formula[cursor] == '!')) return 0;
		return cursor;
	};
	// SUBTOTAL(101-111, ...) and AGGREGATE with options 1, 3, 5 or 7 leave
	// hidden rows out: such a formula counts none of them.
	std::string upper;
	for (const char character : formula) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
	const auto argumentsAfter = [&](const std::string &function) -> std::vector<long> {
		std::vector<long> numbers;
		const std::size_t at = upper.find(function + "(");
		if (at == std::string::npos) return numbers;
		std::size_t cursor = at + function.size() + 1;
		for (int argument = 0; argument < 2 && cursor < upper.size(); ++argument) {
			while (cursor < upper.size() && upper[cursor] == ' ') ++cursor;
			const std::size_t digits = cursor;
			while (cursor < upper.size() && std::isdigit(static_cast<unsigned char>(upper[cursor])) != 0) ++cursor;
			if (cursor == digits) break;
			numbers.push_back(std::stol(upper.substr(digits, cursor - digits)));
			while (cursor < upper.size() && upper[cursor] == ' ') ++cursor;
			if (cursor >= upper.size() || (upper[cursor] != ',' && upper[cursor] != ';')) break;
			++cursor;
		}
		return numbers;
	};
	const std::vector<long> subtotal = argumentsAfter("SUBTOTAL");
	const std::vector<long> aggregate = argumentsAfter("AGGREGATE");
	const bool leavesHiddenRowsOut = (subtotal.empty() == false && subtotal[0] >= 101 && subtotal[0] <= 111)
		|| (aggregate.size() >= 2 && (aggregate[1] == 1 || aggregate[1] == 3 || aggregate[1] == 5 || aggregate[1] == 7));
	if (leavesHiddenRowsOut == true) hiddenRows.clear();
	if (hiddenRows.empty() == true && hiddenColumns.empty() == true) return {};
	std::set<std::size_t> countedRows;
	std::set<std::size_t> countedColumns;
	bool quoted = false;
	for (std::size_t at = 0; at < formula.size(); ++at) {
		if (formula[at] == '"') quoted = !quoted;
		if (quoted == true) continue;
		if (at > 0 && (isWordByte(formula[at - 1]) || formula[at - 1] == '!' || formula[at - 1] == '\'')) continue;
		std::size_t firstColumn = 0, firstRow = 0;
		const std::size_t end = cellAt(at, firstColumn, firstRow);
		if (end == 0) continue;
		std::size_t lastColumn = firstColumn, lastRow = firstRow;
		std::size_t next = end;
		if (end < formula.size() && formula[end] == ':') {
			const std::size_t rangeEnd = cellAt(end + 1, lastColumn, lastRow);
			if (rangeEnd != 0) next = rangeEnd;
			else { lastColumn = firstColumn; lastRow = firstRow; }
		}
		if (lastRow < firstRow) std::swap(lastRow, firstRow);
		if (lastColumn < firstColumn) std::swap(lastColumn, firstColumn);
		for (const std::size_t row : hiddenRows)
			if (row >= firstRow && row <= lastRow) countedRows.insert(row);
		for (const std::size_t column : hiddenColumns)
			if (column >= firstColumn && column <= lastColumn) countedColumns.insert(column);
		at = next - 1;
	}
	const auto listed = [](const std::vector<std::string> &names) {
		std::string words;
		for (std::size_t index = 0; index < names.size() && index < 6; ++index) {
			if (index > 0) words += index + 1 == names.size() ? " and " : ", ";
			words += names[index];
		}
		if (names.size() > 6) words += " and " + std::to_string(names.size() - 6) + " more";
		return words;
	};
	std::vector<std::string> parts;
	if (countedRows.empty() == false) {
		std::vector<std::string> names;
		for (const std::size_t row : countedRows) names.push_back(std::to_string(row + 1));
		parts.push_back(std::string("hidden row") + (names.size() == 1 ? " " : "s ") + listed(names));
	}
	if (countedColumns.empty() == false) {
		std::vector<std::string> names;
		for (const std::size_t column : countedColumns) names.push_back(column_letters(column));
		parts.push_back(std::string("hidden column") + (names.size() == 1 ? " " : "s ") + listed(names));
	}
	if (parts.empty() == true) return {};
	return parts.size() == 1 ? parts.front() : parts.front() + " and " + parts.back();
}

RowReading row_reading(const SheetRender &sheet, const SheetRenderRow &row,
		const std::map<std::pair<std::size_t, std::size_t>, std::string> &merges, bool notesHiddenCounted = true) {
	RowReading reading;
	reading.row_number = row.index + 1;
	std::string cells;
	for (const SheetRenderCell &cell : row.cells) {
		std::string value = cell_value_reading(cell);
		if (value.empty() == true && cell.formula.has_value() == false) continue;
		// "B5 = 12000000": the reference stands apart from the value, so a
		// protected value is replaced without the reference that says where it
		// stands. On the on-device name finder (24 Sep 2026) "B5 12000000" and
		// "B5: 12000000" were taken as one identifier; "B5 = 12000000" never was.
		std::string entry = cell.ref + " =";
		// A formula the file keeps no value for - one Singularity's change left to
		// Excel, or one a program that does not calculate wrote - is read as
		// what it is, never as a figure: Excel works it out when it opens it.
		if (value.empty() == false) entry += " " + value;
		else if (cell.kind == SheetCellKind::Empty) entry += " (calculated when opened in Excel)";
		// A formula Excel worked out to empty text: a blank, as saved.
		else entry += " (blank)";
		if (cell.formula.has_value() == true) {
			std::string formula = one_line(*cell.formula);
			const std::string counted = notesHiddenCounted == true ? hidden_counted_by(sheet, formula, row.index) : std::string();
			if (formula.size() > kFormulaCharacters) formula = formula.substr(0, kFormulaCharacters) + "...";
			entry += " {" + formula + "}";
			if (counted.empty() == false) entry += " (counts " + counted + ")";
		}
		const auto merged = merges.find({row.index, cell.column});
		if (merged != merges.end()) entry += " (merged " + merged->second + ")";
		if (column_hidden(sheet, cell.column) == true) entry += " (hidden column)";
		if (cells.empty() == false) cells += " | ";
		cells += entry;
		++reading.cells;
	}
	if (reading.cells == 0) return reading;
	reading.line = "Row " + std::to_string(reading.row_number) + (row.hidden == true ? " (hidden)" : "") + ": " + cells;
	return reading;
}

std::string sheet_state(const SheetRender &sheet) {
	if (sheet.visibility == SheetVisibility::Hidden) return " (hidden sheet)";
	if (sheet.visibility == SheetVisibility::VeryHidden) return " (very hidden sheet)";
	return {};
}

struct ModelReading {
	std::vector<std::string> lines;
	std::vector<DocumentHeading> headings;
	std::size_t cells = 0;
};

void read_sheet(const SheetRender &sheet, ModelReading &reading, bool notesHiddenCounted = true) {
	std::map<std::pair<std::size_t, std::size_t>, std::string> merges;
	for (const SheetRenderMerge &merge : sheet.merges)
		merges[{merge.first_row, merge.first_column}] = column_letters(merge.first_column) + std::to_string(merge.first_row + 1)
			+ ":" + column_letters(merge.last_column) + std::to_string(merge.last_row + 1);

	std::vector<RowReading> rows;
	for (const SheetRenderRow &row : sheet.rows) {
		RowReading line = row_reading(sheet, row, merges, notesHiddenCounted);
		if (line.cells > 0) rows.push_back(std::move(line));
	}
	if (rows.empty() == true) {
		reading.headings.push_back({reading.lines.size() + 1, 1, sheet.name});
		reading.lines.push_back("Sheet \"" + sheet.name + "\"" + sheet_state(sheet) + ": empty.");
		reading.lines.emplace_back();
		return;
	}

	const SheetRenderRow *header = header_row(sheet);
	std::string headerCells;
	if (header != nullptr) {
		const RowReading headerLine = row_reading(sheet, *header, merges, notesHiddenCounted);
		headerCells = headerLine.line.substr(headerLine.line.find(": ") + 2);
		if (headerCells.size() > kHeaderLineBytes) {
			const std::size_t cut = headerCells.rfind(" | ", kHeaderLineBytes);
			headerCells = headerCells.substr(0, cut == std::string::npos ? kHeaderLineBytes : cut) + " | ...";
		}
	}

	// Rows fall into passages of about kPassageRowBytes, a row never split.
	std::vector<std::pair<std::size_t, std::size_t>> passages;
	std::size_t first = 0;
	std::size_t bytes = 0;
	for (std::size_t index = 0; index < rows.size(); ++index) {
		if (index > first && bytes + rows[index].line.size() + 1 > kPassageRowBytes) {
			passages.push_back({first, index});
			first = index;
			bytes = 0;
		}
		bytes += rows[index].line.size() + 1;
	}
	passages.push_back({first, rows.size()});

	const bool single = passages.size() == 1;
	for (const auto &[begin, end] : passages) {
		const std::size_t firstRow = rows[begin].row_number;
		const std::size_t lastRow = rows[end - 1].row_number;
		const std::string span = "rows " + std::to_string(firstRow) + "-" + std::to_string(lastRow);
		std::string opening = "Sheet \"" + sheet.name + "\"" + sheet_state(sheet) + ", " + span;
		if (single == false) opening += " of " + std::to_string(rows.back().row_number);
		if (header != nullptr && header->index + 1 < firstRow)
			opening += "; header row " + std::to_string(header->index + 1) + ": " + headerCells;
		reading.headings.push_back({reading.lines.size() + 1, 1, single == true ? sheet.name : sheet.name + ", " + span});
		reading.lines.push_back(std::move(opening));
		for (std::size_t index = begin; index < end; ++index) {
			reading.lines.push_back(rows[index].line);
			reading.cells += rows[index].cells;
		}
	}
	reading.lines.emplace_back();
}

DocumentExtractionResult finished(DocumentExtractionResult result, const std::vector<std::string> &lines,
		std::vector<DocumentHeading> headings, std::size_t cellCount, const char *emptyMessage) {
	std::string text;
	for (std::size_t index = 0; index < lines.size(); ++index) {
		if (index != 0) text.push_back('\n');
		text += lines[index];
	}
	result.text = text;
	result.structure.byte_count = result.text.size();
	result.structure.line_count = lines.size();
	result.structure.paragraph_count = cellCount;
	result.structure.headings = std::move(headings);

	const bool hasContent = std::any_of(result.text.begin(), result.text.end(), [](unsigned char byte) {
		return std::isspace(byte) == 0;
	});
	if (!hasContent || cellCount == 0) {
		result.status = ExtractionStatus::empty;
		result.diagnostics.push_back({"empty_document", emptyMessage});
		return result;
	}
	result.status = ExtractionStatus::ready;
	return result;
}

std::optional<DocumentExtractionResult> refused_input(const DocumentExtractionRequest &request, DocumentExtractionResult &result) {
	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(request.path, filesystemError) || filesystemError) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"unreadable_file", "The XLSX document is missing or is not a regular file."});
		return result;
	}
	const auto archiveSize = std::filesystem::file_size(request.path, filesystemError);
	if (filesystemError || archiveSize > request.maximum_input_bytes) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"input_too_large", "The XLSX document exceeds the extraction limit."});
		return result;
	}
	return std::nullopt;
}

} // namespace

const char *xlsx_extractor_version() noexcept { return kExtractorVersion.data(); }

const char *xlsx_cells_reading_version() noexcept { return kCellsReadingVersion.data(); }

const char *xlsx_workbook_v2_reading_version() noexcept { return kWorkbookReadingV2Version.data(); }

std::string xlsx_cell_value_reading(const SheetRenderCell &cell) { return cell_value_reading(cell); }

std::string xlsx_cell_display(const SheetRenderCell &cell) {
	return one_line(cell.fill.has_value() == true ? cell.fill->before + cell.fill->after : cell.text);
}

std::vector<DocumentSheet> extract_xlsx_sheets(const DocumentExtractionRequest &request) {
	std::vector<DocumentSheet> sheets;
	try {
		ZipArchive archive(request.path);
		const auto entryCount = zip_get_num_entries(archive.get(), 0);
		if (entryCount < 0 || entryCount > kMaximumArchiveEntries) return sheets;

		const std::vector<std::string> sharedStrings = read_shared_strings(archive.get());
		const std::vector<WorksheetPart> parts = ordered_worksheet_parts(archive.get());
		for (const WorksheetPart &part : parts) {
			// A sheet past the safety limits is left out alone: the workbook's
			// other sheets are still read, so a cell of a small sheet beside a
			// very large one can be read and changed - and the large one, never
			// read here, can be named by no change.
			try {
				const auto sheetXml = read_entry_by_name(archive.get(), part.part, kMaximumXmlBytes);
				if (!sheetXml.has_value()) continue;
				const auto sheetDocument =
						parse_xml(*sheetXml, "xlsx_malformed_worksheet_xml", "An XLSX worksheet part is malformed.");
				const std::string sheetId = part.name.empty() ? part.part : part.name;
				sheets.push_back(read_worksheet(sheetDocument, sheetId, sharedStrings));
			} catch (const XlsxError &error) {
				if (error.code != "xlsx_xml_part_size_limit" && error.code != "xlsx_cell_count_limit") throw;
			}
		}
	} catch (const XlsxError &) {
		return {};
	} catch (const std::exception &) {
		return {};
	}
	return sheets;
}

// The reading citations made before 24 Sep 2026 were cut from, byte for byte
// as it was then (see kCellsReadingVersion).
DocumentExtractionResult extract_xlsx_cells_reading(const DocumentExtractionRequest &request) {
	DocumentExtractionResult result;
	result.format = DocumentFormat::xlsx;
	result.extractor_version = std::string(kCellsReadingVersion);

	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(request.path, filesystemError) || filesystemError) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"unreadable_file", "The XLSX document is missing or is not a regular file."});
		return result;
	}
	const auto archiveSize = std::filesystem::file_size(request.path, filesystemError);
	if (filesystemError || archiveSize > request.maximum_input_bytes) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"input_too_large", "The XLSX document exceeds the extraction limit."});
		return result;
	}

	std::vector<std::string> lines;
	std::vector<DocumentHeading> headings;
	std::size_t nextLine = 1;
	std::size_t cellCount = 0;
	try {
		const std::vector<DocumentSheet> sheets = extract_xlsx_sheets(request);
		if (sheets.empty()) {
			result.status = ExtractionStatus::unreadable;
			result.diagnostics.push_back({"xlsx_no_sheets", "No worksheets could be read from the XLSX package."});
			return result;
		}
		for (const DocumentSheet &sheet : sheets) {
			if (!sheet.cells.empty()) headings.push_back({nextLine, 1, sheet.sheet_id});
			for (const DocumentSheetCell &cell : sheet.cells) {
				lines.push_back(cell.cell_ref + "\t" + cell.text);
				++nextLine;
				++cellCount;
			}
			lines.emplace_back();
			++nextLine;
		}
	} catch (const std::exception &) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"xlsx_extraction_failed", "The XLSX document could not be extracted safely."});
		return result;
	}

	std::string text;
	for (std::size_t index = 0; index < lines.size(); ++index) {
		if (index != 0) text.push_back('\n');
		text += lines[index];
	}
	result.text = text;
	result.structure.byte_count = result.text.size();
	result.structure.line_count = lines.size();
	result.structure.paragraph_count = cellCount;
	result.structure.headings = std::move(headings);

	const bool hasContent = std::any_of(result.text.begin(), result.text.end(), [](unsigned char byte) {
		return std::isspace(byte) == 0;
	});
	if (!hasContent) {
		result.status = ExtractionStatus::empty;
		result.diagnostics.push_back({"empty_document", "The XLSX document contains no populated cells."});
		return result;
	}
	result.status = ExtractionStatus::ready;
	return result;
}

namespace {

DocumentExtractionResult extract_workbook_reading(const DocumentExtractionRequest &request, bool notesHiddenCounted);

} // namespace

DocumentExtractionResult extract_xlsx_document(const DocumentExtractionRequest &request) {
	return extract_workbook_reading(request, true);
}

// The reading passages were cited from on 24 Sep 2026, byte for byte as it was
// then (see kWorkbookReadingV2Version).
DocumentExtractionResult extract_xlsx_workbook_v2_reading(const DocumentExtractionRequest &request) {
	return extract_workbook_reading(request, false);
}

namespace {

DocumentExtractionResult extract_workbook_reading(const DocumentExtractionRequest &request, bool notesHiddenCounted) {
	DocumentExtractionResult result;
	result.format = DocumentFormat::xlsx;
	result.extractor_version = std::string(notesHiddenCounted == true ? kExtractorVersion : kWorkbookReadingV2Version);
	if (const auto refused = refused_input(request, result)) return *refused;

	XlsxWorkbookViewResult view;
	try {
		view = read_xlsx_workbook_view(request);
	} catch (const std::exception &) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"xlsx_extraction_failed", "The XLSX document could not be extracted safely."});
		return result;
	}
	if (view.status == ExtractionStatus::unreadable || view.workbook.sheets.empty() == true) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"xlsx_no_sheets", "No worksheets could be read from the XLSX package."});
		return result;
	}

	ModelReading reading;
	for (const SheetRender &sheet : view.workbook.sheets) read_sheet(sheet, reading, notesHiddenCounted);
	// What the reading had to leave out, in the words the lawyer is shown.
	if (view.limitation.empty() == false) reading.lines.push_back("Note: " + view.limitation);
	return finished(std::move(result), reading.lines, std::move(reading.headings), reading.cells,
		"The XLSX document contains no populated cells.");
}

} // namespace

} // namespace singularity::documents
