#include "Document/CsvExtractor/CsvExtractor.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace singularity::documents {
namespace {

constexpr const char *kExtractorVersion = "csv-rfc4180/2";
constexpr const char *kTsvExtractorVersion = "tsv-delimited/3";
constexpr std::size_t kMaximumFields = 512;
constexpr std::size_t kMaximumRows = 20000;

bool is_valid_utf8(const std::string &value) {
	const auto *bytes = reinterpret_cast<const utf8proc_uint8_t *>(value.data());
	utf8proc_ssize_t offset = 0;
	const auto length = static_cast<utf8proc_ssize_t>(value.size());
	while (offset < length) {
		utf8proc_int32_t codePoint = 0;
		const utf8proc_ssize_t consumed = utf8proc_iterate(bytes + offset, length - offset, &codePoint);
		if (consumed <= 0) return false;
		offset += consumed;
	}
	return true;
}

std::string trim_ascii(std::string value) {
	const auto notSpace = [](unsigned char byte) { return std::isspace(byte) == 0; };
	const auto first = std::find_if(value.begin(), value.end(), notSpace);
	const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
	if (first >= last) return {};
	return std::string(first, last);
}

// One field's raw bytes in the file, quotes included: [begin, end).
struct DelimitedField {
	std::size_t begin = 0;
	std::size_t end = 0;
};

// One parsed row: its field values, where each field's bytes lie, and where
// the row's content ends (its line break, or the end of the file).
struct DelimitedRow {
	std::vector<std::string> values;
	std::vector<DelimitedField> fields;
	std::size_t end = 0;
};

constexpr std::string_view kUtf8ByteOrderMark = "\xEF\xBB\xBF";

// Parses the whole buffer into rows (RFC 4180): a double quote opens a quoted
// field only at the start of a field, where commas and line breaks are data
// and "" is an escaped quote; a quote anywhere else is an ordinary character
// ("…""", 12"). A UTF-8 byte order mark belongs to no field.
// Returns false (rather than throwing) on unterminated quoting or too many
// rows or fields, so callers report an honest diagnostic.
bool parse_delimited_rows(const std::string &content, char delimiter, std::vector<DelimitedRow> &rows) {
	DelimitedRow row;
	std::string field;
	bool inQuotes = false;
	bool quotedField = false;
	std::size_t index = content.compare(0, kUtf8ByteOrderMark.size(), kUtf8ByteOrderMark) == 0
		? kUtf8ByteOrderMark.size() : 0;
	std::size_t fieldBegin = index;
	const auto endField = [&](std::size_t at) {
		row.values.push_back(std::move(field));
		row.fields.push_back({fieldBegin, at});
		field.clear();
		quotedField = false;
	};
	const auto endRow = [&](std::size_t at) {
		row.end = at;
		rows.push_back(std::move(row));
		row = DelimitedRow{};
	};
	while (index < content.size()) {
		const char byte = content[index];
		if (inQuotes) {
			if (byte == '"') {
				if (index + 1 < content.size() && content[index + 1] == '"') {
					field.push_back('"');
					index += 2;
					continue;
				}
				inQuotes = false;
				++index;
				continue;
			}
			field.push_back(byte);
			++index;
			continue;
		}
		if (byte == '"' && index == fieldBegin) {
			inQuotes = true;
			quotedField = true;
			++index;
			continue;
		}
		if (byte == delimiter) {
			endField(index);
			if (row.values.size() > kMaximumFields) return false;
			fieldBegin = ++index;
			continue;
		}
		if (byte == '\n' || (byte == '\r' && index + 1 < content.size() && content[index + 1] == '\n')) {
			endField(index);
			endRow(index);
			if (rows.size() > kMaximumRows) return false;
			index += byte == '\r' ? 2 : 1;
			fieldBegin = index;
			continue;
		}
		if (byte == '\r') {
			++index;
			continue;
		}
		field.push_back(byte);
		++index;
	}
	if (inQuotes) return false;
	if (!field.empty() || !row.values.empty() || quotedField) {
		endField(content.size());
		endRow(content.size());
	}
	return rows.size() <= kMaximumRows;
}

bool parse_delimited(const std::string &content, char delimiter,
		std::vector<std::vector<std::string>> &rows) {
	std::vector<DelimitedRow> parsed;
	if (!parse_delimited_rows(content, delimiter, parsed)) return false;
	for (DelimitedRow &row : parsed) rows.push_back(std::move(row.values));
	return true;
}

std::string quote_field(const std::string &value, char delimiter) {
	const bool needsQuoting = value.find(delimiter) != std::string::npos || value.find('"') != std::string::npos
		|| value.find('\n') != std::string::npos || value.find('\r') != std::string::npos;
	if (!needsQuoting) return value;
	std::string quoted = "\"";
	for (const char byte : value) {
		if (byte == '"') quoted.push_back('"');
		quoted.push_back(byte);
	}
	quoted.push_back('"');
	return quoted;
}

std::string read_file(const std::filesystem::path &path, bool &ok) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		ok = false;
		return {};
	}
	std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	ok = !input.bad();
	return content;
}

} // namespace

const char *csv_extractor_version() noexcept { return kExtractorVersion; }
const char *tsv_extractor_version() noexcept { return kTsvExtractorVersion; }

DocumentExtractionResult extract_delimited_document(const DocumentExtractionRequest &request,
		DocumentFormat format, char delimiter, const char *version, const char *label) {
	DocumentExtractionResult result;
	result.format = format;
	result.extractor_version = version;

	std::error_code error;
	if (!std::filesystem::is_regular_file(request.path, error) || error) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"unreadable_file", std::string("The ") + label + " document is missing or unreadable."});
		return result;
	}
	const auto size = std::filesystem::file_size(request.path, error);
	if (error || size > request.maximum_input_bytes) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"input_too_large", std::string("The ") + label + " document exceeds the extraction limit."});
		return result;
	}

	bool readOk = false;
	const std::string content = read_file(request.path, readOk);
	if (!readOk) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"read_failed", std::string("The ") + label + " document could not be read completely."});
		return result;
	}
	if (!is_valid_utf8(content)) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"unsupported_encoding",
			std::string("The ") + label + " document is not valid UTF-8."});
		return result;
	}

	std::vector<std::vector<std::string>> rows;
	if (!parse_delimited(content, delimiter, rows)) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({format == DocumentFormat::csv ? "csv_malformed" : "tsv_malformed",
			std::string("The ") + label + " document has unterminated quoting or too many rows/fields."});
		return result;
	}

	for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
		if (rowIndex != 0) result.text.push_back('\n');
		for (std::size_t column = 0; column < rows[rowIndex].size(); ++column) {
			if (column != 0) result.text.push_back('\t');
			result.text += rows[rowIndex][column];
		}
	}
	result.structure.byte_count = result.text.size();
	result.structure.line_count = rows.size();
	result.structure.paragraph_count = rows.empty() ? 0 : rows.size() - 1;
	const bool hasContent = std::any_of(result.text.begin(), result.text.end(), [](unsigned char byte) {
		return std::isspace(byte) == 0;
	});
	if (!hasContent) {
		result.status = ExtractionStatus::empty;
		result.diagnostics.push_back({"empty_document", std::string("The ") + label + " document contains no rows."});
		return result;
	}
	result.status = ExtractionStatus::ready;
	return result;
}

DocumentExtractionResult extract_csv_document(const DocumentExtractionRequest &request) {
	return extract_delimited_document(request, DocumentFormat::csv, ',', kExtractorVersion, "CSV");
}

DocumentExtractionResult extract_tsv_document(const DocumentExtractionRequest &request) {
	return extract_delimited_document(request, DocumentFormat::tsv, '\t', kTsvExtractorVersion, "TSV");
}

DocumentTable extract_delimited_table(const DocumentExtractionRequest &request,
		DocumentFormat format, char delimiter) {
	DocumentTable table;
	table.table_id = format == DocumentFormat::csv ? "csv" : "tsv";
	bool readOk = false;
	const std::string content = read_file(request.path, readOk);
	if (!readOk) return table;
	std::vector<std::vector<std::string>> rows;
	if (!parse_delimited(content, delimiter, rows) || rows.empty()) return table;
	for (const std::string &header : rows.front()) table.column_headers.push_back(trim_ascii(header));
	std::unordered_set<std::string> usedKeys;
	for (std::size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
		const std::vector<std::string> &rawRow = rows[rowIndex];
		DocumentTableRow row;
		row.row_index = rowIndex;
		const std::string firstCell = rawRow.empty() ? std::string() : trim_ascii(rawRow.front());
		row.row_key = derive_table_row_key(firstCell, rowIndex, usedKeys);
		for (std::size_t column = 0; column < rawRow.size(); ++column) {
			DocumentTableCell cell;
			cell.column_index = column;
			cell.column_key = column < table.column_headers.size() ? table.column_headers[column] : std::string();
			cell.text = trim_ascii(rawRow[column]);
			row.cells.push_back(std::move(cell));
		}
		table.rows.push_back(std::move(row));
	}
	return table;
}

DocumentTable extract_csv_table(const DocumentExtractionRequest &request) {
	return extract_delimited_table(request, DocumentFormat::csv, ',');
}

DocumentTable extract_tsv_table(const DocumentExtractionRequest &request) {
	return extract_delimited_table(request, DocumentFormat::tsv, '\t');
}

CsvPatchApplyResult apply_csv_table_cell_patch(
		const std::filesystem::path &documentPath, const DocumentPatch &patch,
		const std::function<bool(const std::filesystem::path &)> &verify_staged) {
	CsvPatchApplyResult result;
	const auto refuse = [&](DocumentPatchBlocker blocker, const char *detail) {
		result.blocker = blocker;
		result.detail = detail;
		return result;
	};

	bool readOk = false;
	const std::string content = read_file(documentPath, readOk);
	if (!readOk)
		return refuse(DocumentPatchBlocker::LocatorUnresolved, "The CSV document could not be re-read for mutation.");
	std::vector<DelimitedRow> rows;
	if (!parse_delimited_rows(content, ',', rows) || rows.empty())
		return refuse(DocumentPatchBlocker::LocatorUnresolved,
			"The CSV document is not well-formed enough to locate a target cell.");

	std::vector<std::string> headers = rows.front().values;
	for (std::string &header : headers) header = trim_ascii(header);
	// Row keys exactly as the structural read derives them, so a locator the
	// validator resolved resolves to the same row here.
	std::vector<std::string> rowKeys(rows.size());
	{
		std::unordered_set<std::string> usedKeys;
		for (std::size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
			const std::string firstCell = rows[rowIndex].values.empty() ? std::string() : trim_ascii(rows[rowIndex].values.front());
			rowKeys[rowIndex] = derive_table_row_key(firstCell, rowIndex, usedKeys);
		}
	}

	// row -> column -> new value
	std::map<std::size_t, std::map<std::size_t, std::string>> changes;
	for (const DocumentPatchOperation &operation : patch.operations) {
		if (!operation.locator.row_key.has_value() || !operation.locator.column_key.has_value())
			return refuse(DocumentPatchBlocker::LocatorUnresolved, "A CSV patch operation is missing its row/column locator.");
		const auto columnIterator = std::find(headers.begin(), headers.end(), *operation.locator.column_key);
		if (columnIterator == headers.end())
			return refuse(DocumentPatchBlocker::LocatorUnresolved, "The CSV patch column header could not be resolved.");
		const std::size_t columnIndex = static_cast<std::size_t>(columnIterator - headers.begin());
		const auto rowIterator = std::find(rowKeys.begin() + 1, rowKeys.end(), *operation.locator.row_key);
		if (rowIterator == rowKeys.end())
			return refuse(DocumentPatchBlocker::LocatorUnresolved, "The CSV patch row key could not be resolved.");
		const std::size_t rowIndex = static_cast<std::size_t>(rowIterator - rowKeys.begin());
		const DelimitedRow &row = rows[rowIndex];
		const std::string current = columnIndex < row.values.size() ? trim_ascii(row.values[columnIndex]) : std::string();
		if (current != operation.expected_old_value)
			return refuse(DocumentPatchBlocker::OldValueMismatch,
				"The CSV cell's current value no longer matches the patch's expected old value.");
		if (!changes[rowIndex].emplace(columnIndex, operation.new_value).second)
			return refuse(DocumentPatchBlocker::LocatorAmbiguous, "Two operations change the same CSV cell.");
	}

	// Only the changed fields' bytes are replaced; a change beyond a short row's
	// last field appends the missing fields at the end of that row. Everything
	// else - other rows, other fields, quoting, line breaks, a byte order mark -
	// stays byte-for-byte.
	struct Splice {
		std::size_t begin = 0;
		std::size_t end = 0;
		std::string text;
	};
	std::vector<Splice> splices;
	for (const auto &[rowIndex, columns] : changes) {
		const DelimitedRow &row = rows[rowIndex];
		std::string appended;
		std::size_t nextMissing = row.fields.size();
		for (const auto &[columnIndex, value] : columns) {
			if (columnIndex < row.fields.size()) {
				splices.push_back({row.fields[columnIndex].begin, row.fields[columnIndex].end, quote_field(value, ',')});
				continue;
			}
			for (; nextMissing < columnIndex; ++nextMissing) appended.push_back(',');
			appended += "," + quote_field(value, ',');
			nextMissing = columnIndex + 1;
		}
		if (!appended.empty()) splices.push_back({row.end, row.end, std::move(appended)});
	}
	std::sort(splices.begin(), splices.end(), [](const Splice &left, const Splice &right) {
		return left.begin != right.begin ? left.begin > right.begin : left.end > right.end;
	});
	std::string rendered = content;
	for (const Splice &splice : splices) rendered.replace(splice.begin, splice.end - splice.begin, splice.text);

	// Postcondition on the staged bytes themselves: the same rows, every
	// unchanged field byte-identical, and exactly the approved values in the
	// changed cells.
	std::vector<DelimitedRow> reparsed;
	bool exact = parse_delimited_rows(rendered, ',', reparsed) && reparsed.size() == rows.size();
	for (std::size_t rowIndex = 0; exact && rowIndex < rows.size(); ++rowIndex) {
		const DelimitedRow &before = rows[rowIndex];
		const DelimitedRow &after = reparsed[rowIndex];
		const auto rowChanges = changes.find(rowIndex);
		if (rowChanges == changes.end()) {
			exact = content.compare(before.fields.front().begin, before.end - before.fields.front().begin, rendered,
				after.fields.front().begin, after.end - after.fields.front().begin) == 0;
			continue;
		}
		const std::size_t width = std::max(before.values.size(), rowChanges->second.rbegin()->first + 1);
		exact = after.values.size() == width;
		for (std::size_t column = 0; exact && column < width; ++column) {
			const auto change = rowChanges->second.find(column);
			if (change != rowChanges->second.end()) {
				exact = after.values[column] == change->second;
			} else if (column < before.values.size()) {
				exact = content.compare(before.fields[column].begin, before.fields[column].end - before.fields[column].begin,
					rendered, after.fields[column].begin, after.fields[column].end - after.fields[column].begin) == 0;
			} else {
				exact = after.values[column].empty();
			}
		}
	}
	if (!exact)
		return refuse(DocumentPatchBlocker::OutputValidationFailed,
			"The rewritten CSV output would change more than the approved cells.");

	const std::filesystem::path temporaryPath = platform::staged_sibling_path(documentPath);
	try {
		platform::write_file_durably(temporaryPath, rendered);
	} catch (const std::exception &) {
		platform::discard_staged_file(temporaryPath);
		return refuse(DocumentPatchBlocker::AtomicReplaceFailed, "The temporary CSV output could not be written completely.");
	}
	bool reReadOk = false;
	if (read_file(temporaryPath, reReadOk) != rendered || !reReadOk) {
		platform::discard_staged_file(temporaryPath);
		return refuse(DocumentPatchBlocker::OutputValidationFailed, "The rewritten CSV output failed verification.");
	}
	if (verify_staged && !verify_staged(temporaryPath)) {
		platform::discard_staged_file(temporaryPath);
		return refuse(DocumentPatchBlocker::OutputValidationFailed,
			"The rewritten CSV output does not hold exactly the approved change.");
	}

	try {
		platform::commit_staged_file(temporaryPath, documentPath, platform::StagedCommitMode::ReplaceExisting);
	} catch (const std::exception &) {
		platform::discard_staged_file(temporaryPath);
		return refuse(DocumentPatchBlocker::AtomicReplaceFailed,
			"The validated CSV output could not replace the Project file atomically.");
	}

	result.ok = true;
	return result;
}

} // namespace singularity::documents
