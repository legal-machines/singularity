#include "DocumentIntelligence/RevisionWriter/RevisionWriter.hpp"
#include "DocumentIntelligence/LegalStructure/LegalStructure.hpp"
#include "Document/DocxExtractor/DocxExtractor.hpp"
#include "Document/OfficeTextEdit/OfficeTextEdit.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/PhraseMatch/PhraseMatch.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include <pugixml.hpp>
#include <zip.h>

namespace singularity::document_intelligence {
namespace {

// Track Changes left on in the draft (word/settings.xml, w:trackRevisions), so
// whatever anyone writes into it next is tracked too, as a mark-up sent to the
// other side is. The element goes where Word's schema puts it: before the
// first of the settings that follow it.
bool turnOnTrackChanges(const std::filesystem::path& path) {

	static const std::set<std::string> following = {"w:doNotTrackMoves", "w:doNotTrackFormatting", "w:documentProtection",
		"w:autoFormatOverride", "w:styleLockTheme", "w:styleLockQFSet", "w:defaultTabStop", "w:autoHyphenation",
		"w:consecutiveHyphenLimit", "w:hyphenationZone", "w:doNotHyphenateCaps", "w:showEnvelope", "w:summaryLength",
		"w:clickAndTypeStyle", "w:defaultTableStyle", "w:evenAndOddHeaders", "w:bookFoldRevPrinting", "w:bookFoldPrinting",
		"w:bookFoldPrintingSheets", "w:drawingGridHorizontalSpacing", "w:drawingGridVerticalSpacing",
		"w:displayHorizontalDrawingGridEvery", "w:displayVerticalDrawingGridEvery", "w:doNotUseMarginsForDrawingGridOrigin",
		"w:drawingGridHorizontalOrigin", "w:drawingGridVerticalOrigin", "w:doNotShadeFormData", "w:noPunctuationKerning",
		"w:characterSpacingControl", "w:printTwoOnOne", "w:strictFirstAndLastChars", "w:noLineBreaksAfter",
		"w:noLineBreaksBefore", "w:savePreviewPicture", "w:doNotValidateAgainstSchema", "w:saveInvalidXml",
		"w:ignoreMixedContent", "w:alwaysShowPlaceholderText", "w:doNotDemarcateInvalidXml", "w:saveXmlDataOnly",
		"w:useXSLTWhenSaving", "w:saveThroughXslt", "w:showXMLTags", "w:alwaysMergeEmptyNamespace", "w:updateFields",
		"w:hdrShapeDefaults", "w:footnotePr", "w:endnotePr", "w:compat", "w:docVars", "w:rsids", "m:mathPr",
		"w:attachedSchema", "w:themeFontLang", "w:clrSchemeMapping", "w:doNotIncludeSubdocsInStats",
		"w:doNotAutoCompressPictures", "w:forceUpgrade", "w:captions", "w:readModeInkLockDown", "w:smartTagType",
		"sl:schemaLibrary", "w:shapeDefaults", "w:doNotEmbedSmartTags", "w:decimalSymbol", "w:listSeparator"};
	int errorCode = 0;
	zip_t* archive = zip_open(path.u8string().c_str(), 0, &errorCode);
	if (archive == nullptr) return false;
	const zip_int64_t index = zip_name_locate(archive, "word/settings.xml", 0);
	zip_stat_t stat;
	if (index < 0 || zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &stat) != 0) {
		zip_discard(archive);
		return false;
	}
	std::string xml(static_cast<std::size_t>(stat.size), '\0');
	zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), 0);
	const zip_int64_t read = file != nullptr ? zip_fread(file, xml.data(), stat.size) : -1;
	if (file != nullptr) zip_fclose(file);
	pugi::xml_document settingsXml;
	if (read != static_cast<zip_int64_t>(stat.size)
			|| settingsXml.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_declaration) == false) {
		zip_discard(archive);
		return false;
	}
	pugi::xml_node settings = settingsXml.child("w:settings");
	if (settings.empty() == true) {
		zip_discard(archive);
		return false;
	}
	if (settings.child("w:trackRevisions").empty() == false) {
		zip_discard(archive);
		return true;
	}
	pugi::xml_node next;
	for (pugi::xml_node child = settings.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (following.count(child.name()) > 0) {
			next = child;
			break;
		}
	}
	if (next.empty() == false) settings.insert_child_before("w:trackRevisions", next);
	else settings.append_child("w:trackRevisions");
	std::ostringstream written;
	settingsXml.save(written, "", pugi::format_raw);
	const std::string updated = written.str();
	void* buffer = std::malloc(updated.size());
	if (buffer == nullptr) {
		zip_discard(archive);
		return false;
	}
	std::memcpy(buffer, updated.data(), updated.size());
	zip_source_t* source = zip_source_buffer(archive, buffer, updated.size(), 1);
	if (source == nullptr) {
		std::free(buffer);
		zip_discard(archive);
		return false;
	}
	if (zip_file_replace(archive, static_cast<zip_uint64_t>(index), source, 0) != 0) {
		zip_source_free(source);
		zip_discard(archive);
		return false;
	}
	return zip_close(archive) == 0;

}

// The order things are written in, when each finds its place by words of the
// source that stand there once (`anchors`) and adds words of its own
// (`inserts`): what was written first may repeat a later one's anchor, and the
// later one could then no longer find its place. A live run (24 Oct 2026): a
// rewrite of Section 5.1 added "with respect to any Confidential Information",
// and Section 8.1's edit, which stood on "to any Confidential", was refused as
// standing in two places. So whatever another's new words would repeat is
// written before it; otherwise the review's own order holds. Where two would
// each repeat the other's anchor, the rest keep the review's order.
std::vector<std::size_t> writingOrder(const std::vector<std::vector<std::string>>& anchors,
	const std::vector<std::vector<std::string>>& inserts) {

	const std::size_t count = anchors.size();
	std::vector<std::vector<std::size_t>> later(count);
	std::vector<std::size_t> waiting(count, 0);
	for (std::size_t first = 0; first < count; ++first) {
		for (std::size_t other = 0; other < count; ++other) {
			if (other == first) continue;
			const bool isRepeated = std::any_of(anchors[first].begin(), anchors[first].end(), [&](const std::string& anchor) {
				return anchor.empty() == false && std::any_of(inserts[other].begin(), inserts[other].end(),
					[&](const std::string& words) { return platform::locate_phrase(words, anchor).matches > 0; });
			});
			if (isRepeated == false) continue;
			later[first].push_back(other);
			++waiting[other];
		}
	}
	std::vector<std::size_t> order;
	std::vector<bool> isPlaced(count, false);
	while (order.size() < count) {
		std::size_t next = count;
		for (std::size_t index = 0; index < count; ++index) {
			if (isPlaced[index] == false && waiting[index] == 0) {
				next = index;
				break;
			}
		}
		if (next == count) {
			for (std::size_t index = 0; index < count; ++index) {
				if (isPlaced[index] == false) order.push_back(index);
			}
			break;
		}
		isPlaced[next] = true;
		order.push_back(next);
		for (const std::size_t other : later[next]) --waiting[other];
	}
	return order;

}

documents::DocumentPatch patchFor(const ProposedChange& change, const std::string& author) {

	// A rewritten clause: its edits in one patch, written together or not at
	// all, so a clause is never left half changed. Each edit's anchor holds a
	// few words around what differs; the writer strikes and inserts only what
	// differs (Document/OfficeTextEdit). An edit whose anchor another edit's new
	// words would repeat goes first.
	if (change.action == "rewrite" && change.edits.empty() == false) {
		documents::DocumentPatch patch;
		std::vector<std::vector<std::string>> anchors;
		std::vector<std::vector<std::string>> inserts;
		for (const TextEdit& edit : change.edits) {
			anchors.push_back({platform::trimmed_phrase(edit.currentText)});
			inserts.push_back(edit.proposedText.empty() == true ? std::vector<std::string>() : std::vector<std::string>{edit.proposedText});
		}
		for (const std::size_t index : writingOrder(anchors, inserts)) {
			const TextEdit& edit = change.edits[index];
			documents::DocumentPatchOperation operation;
			operation.kind = edit.proposedText.empty() == true ? documents::DocumentPatchOperationKind::DeleteContent
				: documents::DocumentPatchOperationKind::ReplaceText;
			operation.new_value = edit.proposedText;
			operation.expected_old_value = edit.currentText;
			operation.rationale_id = change.id;
			patch.operations.push_back(operation);
		}
		patch.revision_author = author;
		return patch;
	}

	documents::DocumentPatchOperation operation;
	if (change.action == "delete") {
		operation.kind = documents::DocumentPatchOperationKind::DeleteContent;
	} else if (change.action == "insert_after") {
		operation.kind = documents::DocumentPatchOperationKind::InsertParagraph;
		operation.placement = documents::DocumentPatchPlacement::After;
		operation.new_value = change.proposedText;
	} else {
		operation.kind = documents::DocumentPatchOperationKind::ReplaceText;
		operation.new_value = change.proposedText;
	}
	// The passage is named by its own words, as the source holds them.
	operation.expected_old_value = change.currentText;
	operation.rationale_id = change.id;

	documents::DocumentPatch patch;
	patch.operations.push_back(operation);
	patch.revision_author = author;
	return patch;

}

// ---- A table as it should read ---------------------------------------------------------

// A cell's words as compared when finding a table: letters and digits only, so
// how a reading joins a cell's paragraphs or spaces its words never decides.
std::string cellKey(const std::string& words) {
	std::string key;
	for (const char character : words) {
		const unsigned char byte = static_cast<unsigned char>(character);
		if (byte >= 0x80 || std::isalnum(byte) != 0) key.push_back(static_cast<char>(std::tolower(byte)));
	}
	return key;
}

// The writer's own cells of a row read as `row`: a cell a vertical merge
// continues is not one of them.
std::vector<std::string> ownCells(const TableRowReading& row) {
	std::vector<std::string> cells;
	for (std::size_t index = 0; index < row.cells.size(); ++index) {
		if (index >= row.isContinued.size() || row.isContinued[index] == false) cells.push_back(row.cells[index]);
	}
	return cells;
}

// Where a column read as `column` of `row` stands among the row's own cells.
std::size_t ownColumn(const TableRowReading& row, std::size_t column) {
	std::size_t own = 0;
	for (std::size_t index = 0; index < column && index < row.isContinued.size(); ++index) {
		if (row.isContinued[index] == false) ++own;
	}
	return own;
}

// A table's edits as the writer names their places (Document/DocxExtractor):
// the table found as Word holds it now - the one table whose rows with words
// are, cell by cell, the rows the reading gave it - each row by the key the
// writer gives it, each column by its place. Cells first, a row's first cell
// last of its row's, so every row is still named as it stands; then the new
// rows, each above the row it stands above, in order; then the rows struck.
std::optional<documents::DocumentPatch> tablePatchFor(const std::filesystem::path& staged, const ProposedChange& change,
	const std::string& author, std::string& problem) {

	documents::DocumentExtractionRequest request;
	request.path = staged;
	std::vector<documents::DocumentTable> tables;
	try {
		tables = documents::extract_docx_tables(request);
	} catch (const std::exception&) {
		problem = "The table could not be read as Word holds it, so it was not changed.";
		return std::nullopt;
	}
	const documents::DocumentTable* found = nullptr;
	std::vector<const documents::DocumentTableRow*> rowsRead;
	for (const documents::DocumentTable& table : tables) {
		std::vector<const documents::DocumentTableRow*> withWords;
		for (const documents::DocumentTableRow& row : table.rows) {
			const bool hasWords = std::any_of(row.cells.begin(), row.cells.end(),
				[](const documents::DocumentTableCell& cell) { return cellKey(cell.text).empty() == false; });
			if (hasWords == true) withWords.push_back(&row);
		}
		if (withWords.size() != change.tableRows.size()) continue;
		bool isSame = true;
		for (std::size_t index = 0; index < withWords.size() && isSame == true; ++index) {
			const std::vector<std::string> own = ownCells(change.tableRows[index]);
			if (own.size() != withWords[index]->cells.size()) {
				isSame = false;
				break;
			}
			for (std::size_t column = 0; column < own.size(); ++column) {
				if (cellKey(own[column]) != cellKey(withWords[index]->cells[column].text)) isSame = false;
			}
		}
		if (isSame == false) continue;
		if (found != nullptr) {
			problem = "The document has more than one table that reads exactly like this one, so it was not changed.";
			return std::nullopt;
		}
		found = &table;
		rowsRead = withWords;
	}
	if (found == nullptr) {
		problem = "The table could not be found as Word holds it, so it was not changed.";
		return std::nullopt;
	}

	documents::DocumentPatch patch;
	patch.revision_author = author;
	const auto locatorOf = [&](std::size_t row) {
		documents::DocumentLocator locator;
		locator.table_id = found->table_id;
		locator.row_key = rowsRead[row]->row_key;
		return locator;
	};
	std::vector<const TableEdit*> cells;
	for (const TableEdit& edit : change.tableEdits) {
		if (edit.kind == "cell" && edit.column > 0) cells.push_back(&edit);
	}
	for (const TableEdit& edit : change.tableEdits) {
		if (edit.kind == "cell" && edit.column == 0) cells.push_back(&edit);
	}
	for (const TableEdit* edit : cells) {
		if (edit->row >= rowsRead.size()) {
			problem = "A cell the change names is not in the table, so it was not changed.";
			return std::nullopt;
		}
		const std::size_t column = ownColumn(change.tableRows[edit->row], edit->column);
		if (column >= rowsRead[edit->row]->cells.size()) {
			problem = "A cell the change names is not in the table, so it was not changed.";
			return std::nullopt;
		}
		documents::DocumentPatchOperation operation;
		operation.kind = documents::DocumentPatchOperationKind::ReplaceCellText;
		operation.locator = locatorOf(edit->row);
		// The column as the writer reads it for this very cell: a row continuing
		// a merged cell counts that cell in its place.
		operation.locator.column_key = rowsRead[edit->row]->cells[column].column_key;
		operation.expected_old_value = rowsRead[edit->row]->cells[column].text;
		operation.new_value = edit->proposedText;
		operation.rationale_id = change.id;
		patch.operations.push_back(operation);
	}
	for (const TableEdit& edit : change.tableEdits) {
		if (edit.kind != "add_row") continue;
		documents::DocumentPatchOperation operation;
		operation.kind = documents::DocumentPatchOperationKind::InsertTableRow;
		const bool isAtEnd = edit.row >= rowsRead.size();
		const TableRowReading& like = change.tableRows[isAtEnd == true ? change.tableRows.size() - 1 : edit.row];
		if (isAtEnd == true) {
			operation.locator.table_id = found->table_id;
			operation.placement = documents::DocumentPatchPlacement::AtEnd;
		} else {
			operation.locator = locatorOf(edit.row);
			operation.placement = documents::DocumentPatchPlacement::Before;
		}
		// Each value at its own position in the row, which the writer counts as
		// the reading does: a merged cell's continuation keeps its place.
		for (std::size_t column = 0; column < edit.cells.size(); ++column) {
			if (column < like.isContinued.size() && like.isContinued[column] == true) continue;
			operation.cells.push_back(documents::DocumentPatchCellValue{"column-" + std::to_string(column + 1),
				edit.cells[column]});
		}
		operation.rationale_id = change.id;
		patch.operations.push_back(operation);
	}
	for (const TableEdit& edit : change.tableEdits) {
		if (edit.kind != "remove_row") continue;
		if (edit.row == 0 || edit.row >= rowsRead.size()) {
			problem = "A row the change strikes is not one the table can lose, so it was not changed.";
			return std::nullopt;
		}
		documents::DocumentPatchOperation operation;
		operation.kind = documents::DocumentPatchOperationKind::DeleteTableRow;
		operation.locator = locatorOf(edit.row);
		operation.rationale_id = change.id;
		patch.operations.push_back(operation);
	}
	if (patch.operations.empty() == true) {
		problem = "The change leaves the table as it is, so it was not written.";
		return std::nullopt;
	}
	return patch;

}

// ---- Comments in the margin (D-117) ----------------------------------------------------

constexpr const char* kCommentsRelationship = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments";
constexpr const char* kCommentsContentType = "application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml";

std::optional<std::string> entryText(zip_t* archive, const std::string& name) {
	const zip_int64_t index = zip_name_locate(archive, name.c_str(), 0);
	zip_stat_t stat;
	if (index < 0 || zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &stat) != 0) return std::nullopt;
	std::string bytes(static_cast<std::size_t>(stat.size), '\0');
	zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), 0);
	if (file == nullptr) return std::nullopt;
	const zip_int64_t read = zip_fread(file, bytes.data(), stat.size);
	zip_fclose(file);
	if (read != static_cast<zip_int64_t>(stat.size)) return std::nullopt;
	return bytes;
}

// A part written in place, or added where the package has none.
bool putEntry(zip_t* archive, const std::string& name, const std::string& bytes) {
	void* buffer = std::malloc(bytes.empty() == true ? 1 : bytes.size());
	if (buffer == nullptr) return false;
	if (bytes.empty() == false) std::memcpy(buffer, bytes.data(), bytes.size());
	zip_source_t* source = zip_source_buffer(archive, buffer, bytes.size(), 1);
	if (source == nullptr) {
		std::free(buffer);
		return false;
	}
	const zip_int64_t index = zip_name_locate(archive, name.c_str(), 0);
	const bool isPut = index >= 0 ? zip_file_replace(archive, static_cast<zip_uint64_t>(index), source, 0) == 0
		: zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) >= 0;
	if (isPut == false) zip_source_free(source);
	return isPut;
}

std::string xmlOf(const pugi::xml_document& xml) {
	std::ostringstream written;
	xml.save(written, "", pugi::format_raw);
	return written.str();
}

std::string localName(const pugi::xml_node& node) {
	const std::string name = node.name();
	const std::size_t colon = name.find(':');
	return colon == std::string::npos ? name : name.substr(colon + 1);
}

// The words of a paragraph as Word shows it with every revision accepted: its
// insertions in, its deletions out; a text box's words are not the paragraph's.
void acceptedWordsOf(const pugi::xml_node& node, std::string& words) {
	for (pugi::xml_node child = node.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (child.type() != pugi::node_element) continue;
		const std::string name = localName(child);
		if (name == "del" || name == "moveFrom" || name == "pPr" || name == "rPr" || name == "drawing" || name == "pict"
			|| name == "AlternateContent" || name == "object" || name == "instrText" || name == "delText") {
			continue;
		}
		if (name == "t") {
			words += child.text().as_string();
		} else if (name == "tab" || name == "br" || name == "cr") {
			words += " ";
		} else if (name == "noBreakHyphen") {
			words += "-";
		} else {
			acceptedWordsOf(child, words);
		}
	}
}

bool isInTextBox(const pugi::xml_node& node) {
	for (pugi::xml_node up = node.parent(); up.empty() == false; up = up.parent()) {
		if (localName(up) == "txbxContent") return true;
	}
	return false;
}

int highestId(const pugi::xml_node& root, const std::set<std::string>& names) {
	int highest = -1;
	std::vector<pugi::xml_node> stack = {root};
	while (stack.empty() == false) {
		const pugi::xml_node node = stack.back();
		stack.pop_back();
		for (pugi::xml_node child = node.first_child(); child.empty() == false; child = child.next_sibling()) {
			if (child.type() != pugi::node_element) continue;
			if (names.count(localName(child)) > 0) {
				for (const pugi::xml_attribute attribute : child.attributes()) {
					const std::string attributeName = attribute.name();
					if (attributeName == "w:id" || attributeName == "id") highest = std::max(highest, attribute.as_int(-1));
				}
			}
			stack.push_back(child);
		}
	}
	return highest;
}

std::string initialsOf(const std::string& author) {
	std::string initials;
	std::istringstream reader(author);
	for (std::string word; reader >> word && initials.size() < 3;) {
		if (std::isalpha(static_cast<unsigned char>(word.front())) != 0) {
			initials.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(word.front()))));
		}
	}
	return initials;
}

// Each comment on the one paragraph of the body whose words hold its clause's
// words; each placed comment marked "written" until it is read back. Returns
// how many were placed; the package is left as it was where none could be.
std::size_t placeMarginComments(const std::filesystem::path& path, std::vector<MarginComment>& comments, const std::string& author) {

	int errorCode = 0;
	zip_t* archive = zip_open(path.u8string().c_str(), 0, &errorCode);
	if (archive == nullptr) {
		for (MarginComment& comment : comments) {
			comment.writeState = "refused";
			comment.writeNote = "The draft could not be opened to add the comment.";
		}
		return 0;
	}
	const auto refuseAll = [&](const std::string& why) {
		for (MarginComment& comment : comments) {
			if (comment.writeState == "not_written") {
				comment.writeState = "refused";
				comment.writeNote = why;
			}
		}
		zip_discard(archive);
		return std::size_t{0};
	};
	const std::optional<std::string> documentBytes = entryText(archive, "word/document.xml");
	const std::optional<std::string> relationshipBytes = entryText(archive, "word/_rels/document.xml.rels");
	const std::optional<std::string> typeBytes = entryText(archive, "[Content_Types].xml");
	pugi::xml_document documentXml;
	pugi::xml_document relationshipXml;
	pugi::xml_document typeXml;
	// Every space kept as the package has it, as the document's other writers
	// read it (Document/DocxExtractor).
	const unsigned int options = pugi::parse_default | pugi::parse_declaration | pugi::parse_ws_pcdata;
	if (documentBytes.has_value() == false || relationshipBytes.has_value() == false || typeBytes.has_value() == false
		|| documentXml.load_buffer(documentBytes->data(), documentBytes->size(), options) == false
		|| relationshipXml.load_buffer(relationshipBytes->data(), relationshipBytes->size(), options) == false
		|| typeXml.load_buffer(typeBytes->data(), typeBytes->size(), options) == false) {
		return refuseAll("The draft is not a Word package the comment could be added to.");
	}
	const pugi::xml_node root = documentXml.document_element();
	const std::string rootName = root.name();
	const std::string w = rootName.find(':') == std::string::npos ? std::string() : rootName.substr(0, rootName.find(':') + 1);

	// The comments part: the package's own, or a new one it is told of.
	pugi::xml_node relationships = relationshipXml.document_element();
	std::string commentsPart;
	for (pugi::xml_node relationship : relationships.children()) {
		const std::string type = relationship.attribute("Type").as_string();
		if (type.size() < 9 || type.compare(type.size() - 9, 9, "/comments") != 0) continue;
		std::string target = relationship.attribute("Target").as_string();
		commentsPart = target.empty() == false && target.front() == '/' ? target.substr(1) : "word/" + target;
	}
	pugi::xml_document commentsXml;
	bool isNewPart = false;
	if (commentsPart.empty() == false) {
		const std::optional<std::string> commentBytes = entryText(archive, commentsPart);
		if (commentBytes.has_value() == false || commentsXml.load_buffer(commentBytes->data(), commentBytes->size(), options) == false) {
			return refuseAll("The draft's comments could not be read, so no comment was added.");
		}
	} else {
		isNewPart = true;
		commentsPart = "word/comments.xml";
		pugi::xml_node declaration = commentsXml.append_child(pugi::node_declaration);
		declaration.append_attribute("version") = "1.0";
		declaration.append_attribute("encoding") = "UTF-8";
		declaration.append_attribute("standalone") = "yes";
		pugi::xml_node made = commentsXml.append_child((w + "comments").c_str());
		const std::string xmlns = w.empty() == true ? std::string("xmlns") : "xmlns:" + w.substr(0, w.size() - 1);
		made.append_attribute(xmlns.c_str()) = root.attribute(xmlns.c_str()).as_string(
			"http://schemas.openxmlformats.org/wordprocessingml/2006/main");
	}
	pugi::xml_node commentsRoot = commentsXml.document_element();

	// Word's own comment styles, where the document has them.
	bool hasCommentStyles = false;
	if (const std::optional<std::string> styles = entryText(archive, "word/styles.xml"); styles.has_value() == true) {
		hasCommentStyles = styles->find("w:styleId=\"CommentReference\"") != std::string::npos
			&& styles->find("w:styleId=\"CommentText\"") != std::string::npos;
	}

	// The body's paragraphs, each with its words as Word shows them.
	std::vector<std::pair<pugi::xml_node, std::string>> paragraphs;
	{
		std::vector<pugi::xml_node> stack = {root};
		while (stack.empty() == false) {
			const pugi::xml_node node = stack.back();
			stack.pop_back();
			std::vector<pugi::xml_node> children;
			for (pugi::xml_node child = node.first_child(); child.empty() == false; child = child.next_sibling()) {
				if (child.type() == pugi::node_element) children.push_back(child);
			}
			for (auto child = children.rbegin(); child != children.rend(); ++child) {
				if (localName(*child) == "p" && isInTextBox(*child) == false) {
					std::string words;
					acceptedWordsOf(*child, words);
					paragraphs.push_back({*child, words});
				}
				stack.push_back(*child);
			}
		}
	}
	// The one paragraph whose words are the clause's, else the one paragraph that
	// holds them: a title's words held again in the paragraph after it ("DATA
	// PROCESSING ADDENDUM", "This Data Processing Addendum is ...") are still
	// the title's own paragraph.
	const auto paragraphHolding = [&](const std::string& phrase) {
		pugi::xml_node whole;
		std::size_t wholes = 0;
		pugi::xml_node found;
		std::size_t count = 0;
		for (const auto& [node, words] : paragraphs) {
			if (platform::phrases_equal(words, phrase) == true) {
				whole = node;
				++wholes;
			}
			if (platform::locate_phrase(words, phrase).matches == 1 || platform::phrases_equal(words, phrase) == true) {
				found = node;
				++count;
			}
		}
		if (wholes == 1) return whole;
		return count == 1 ? found : pugi::xml_node();
	};

	int next = std::max(highestId(root, {"commentRangeStart", "commentRangeEnd", "commentReference"}),
		highestId(commentsRoot, {"comment"})) + 1;
	const std::string when = documents::revision_timestamp_now();
	const std::string initials = initialsOf(author);
	std::size_t placed = 0;
	for (MarginComment& comment : comments) {
		const std::string anchor = platform::trimmed_phrase(comment.anchorText);
		pugi::xml_node paragraph = anchor.empty() == true ? pugi::xml_node() : paragraphHolding(anchor);
		if (paragraph.empty() == true && anchor.empty() == false) {
			// A clause read with words Word keeps apart (a field, a symbol):
			// its opening words, where they stand once.
			std::istringstream reader(anchor);
			std::string opening;
			int count = 0;
			for (std::string word; reader >> word && count < 12; ++count) opening += (opening.empty() == true ? "" : " ") + word;
			if (count >= 6) paragraph = paragraphHolding(opening);
		}
		if (paragraph.empty() == true) {
			comment.writeState = "refused";
			comment.writeNote = "Its clause could not be found once in the draft as Word holds it, so the comment was not added.";
			continue;
		}
		const std::string id = std::to_string(next++);
		pugi::xml_node properties = paragraph.child((w + "pPr").c_str());
		pugi::xml_node start = properties.empty() == false ? paragraph.insert_child_after((w + "commentRangeStart").c_str(), properties)
			: paragraph.prepend_child((w + "commentRangeStart").c_str());
		start.append_attribute((w + "id").c_str()) = id.c_str();
		paragraph.append_child((w + "commentRangeEnd").c_str()).append_attribute((w + "id").c_str()) = id.c_str();
		pugi::xml_node reference = paragraph.append_child((w + "r").c_str());
		if (hasCommentStyles == true) {
			reference.append_child((w + "rPr").c_str()).append_child((w + "rStyle").c_str())
				.append_attribute((w + "val").c_str()) = "CommentReference";
		}
		reference.append_child((w + "commentReference").c_str()).append_attribute((w + "id").c_str()) = id.c_str();

		pugi::xml_node made = commentsRoot.append_child((w + "comment").c_str());
		made.append_attribute((w + "id").c_str()) = id.c_str();
		made.append_attribute((w + "author").c_str()) = author.c_str();
		made.append_attribute((w + "date").c_str()) = when.c_str();
		if (initials.empty() == false) made.append_attribute((w + "initials").c_str()) = initials.c_str();
		std::vector<std::string> lines;
		std::istringstream text(comment.text);
		for (std::string line; std::getline(text, line);) {
			if (platform::trimmed_phrase(line).empty() == false) lines.push_back(platform::trimmed_phrase(line));
		}
		for (std::size_t index = 0; index < lines.size(); ++index) {
			pugi::xml_node line = made.append_child((w + "p").c_str());
			if (hasCommentStyles == true) {
				line.append_child((w + "pPr").c_str()).append_child((w + "pStyle").c_str())
					.append_attribute((w + "val").c_str()) = "CommentText";
			}
			if (index == 0) {
				pugi::xml_node mark = line.append_child((w + "r").c_str());
				if (hasCommentStyles == true) {
					mark.append_child((w + "rPr").c_str()).append_child((w + "rStyle").c_str())
						.append_attribute((w + "val").c_str()) = "CommentReference";
				}
				mark.append_child((w + "annotationRef").c_str());
			}
			pugi::xml_node words = line.append_child((w + "r").c_str()).append_child((w + "t").c_str());
			words.append_attribute("xml:space") = "preserve";
			words.text().set(lines[index].c_str());
		}
		comment.writeState = "written";
		comment.writeNote.clear();
		++placed;
	}
	if (placed == 0) {
		zip_discard(archive);
		return 0;
	}

	if (isNewPart == true) {
		int highestRelationship = 0;
		for (pugi::xml_node relationship : relationships.children()) {
			const std::string id = relationship.attribute("Id").as_string();
			if (id.rfind("rId", 0) == 0) highestRelationship = std::max(highestRelationship, std::atoi(id.c_str() + 3));
		}
		pugi::xml_node relationship = relationships.append_child("Relationship");
		relationship.append_attribute("Id") = ("rId" + std::to_string(highestRelationship + 1)).c_str();
		relationship.append_attribute("Type") = kCommentsRelationship;
		relationship.append_attribute("Target") = "comments.xml";
		pugi::xml_node types = typeXml.document_element();
		bool isTyped = false;
		for (pugi::xml_node type : types.children("Override")) {
			if (std::string(type.attribute("PartName").as_string()) == "/word/comments.xml") isTyped = true;
		}
		if (isTyped == false) {
			pugi::xml_node type = types.append_child("Override");
			type.append_attribute("PartName") = "/word/comments.xml";
			type.append_attribute("ContentType") = kCommentsContentType;
		}
	}
	const bool isSaved = putEntry(archive, "word/document.xml", xmlOf(documentXml))
		&& putEntry(archive, commentsPart, xmlOf(commentsXml))
		&& (isNewPart == false || (putEntry(archive, "word/_rels/document.xml.rels", xmlOf(relationshipXml))
			&& putEntry(archive, "[Content_Types].xml", xmlOf(typeXml))));
	if (isSaved == false || zip_close(archive) != 0) {
		if (isSaved == false) zip_discard(archive);
		for (MarginComment& comment : comments) {
			if (comment.writeState != "written") continue;
			comment.writeState = "refused";
			comment.writeNote = "The comment could not be saved into the draft.";
		}
		return 0;
	}
	return placed;

}

// Each placed comment found again, by its author and words, on its clause -
// read as the Matter reads the draft, from a copy under the draft's own kind
// of name.
std::size_t readBackComments(const std::filesystem::path& staged, std::vector<MarginComment>& comments, const std::string& author) {

	std::error_code error;
	const std::filesystem::path copy = std::filesystem::temp_directory_path(error)
		/ ("lm-comment-check-" + platform::secure_random_hex(6) + ".docx");
	std::size_t confirmed = 0;
	StructureReading reading;
	if (error.value() == 0 && std::filesystem::copy_file(staged, copy, error) == true) reading = readDocxStructure(takeInSource(copy));
	std::filesystem::remove(copy, error);
	for (MarginComment& comment : comments) {
		if (comment.writeState != "written") continue;
		bool isOnClause = false;
		if (reading.isRead == true) {
			for (const LegalBlock& block : reading.document.blocks) {
				if (block.kind != BlockKind::Comment || block.author != author || platform::phrases_equal(block.text, comment.text) == false) {
					continue;
				}
				// On the paragraph it was placed on, known by its words: as the draft
				// reads them, or as they read before this draft's own changes. An id
				// alone can move - a comment above takes a place in the reading's
				// order - and a change in the paragraph moves its words (M7, the
				// DPA's opening, 30 Oct 2026).
				const LegalBlock* clause = findBlock(reading.document, block.annotates);
				const bool isClauseWords = clause != nullptr && (platform::phrases_equal(clause->text, comment.anchorText) == true
					|| (clause->originalText.empty() == false && platform::phrases_equal(clause->originalText, comment.anchorText) == true));
				if (block.annotates == comment.blockId || isClauseWords == true) isOnClause = true;
			}
		}
		if (isOnClause == true) {
			++confirmed;
			continue;
		}
		comment.writeState = "refused";
		comment.writeNote = "The comment did not read back on its clause, so it is not counted as written.";
	}
	return confirmed;

}

// What the writer said, for a lawyer: never a path or a part's name.
std::string refusalWords(const documents::DocxPatchApplyResult& result) {

	switch (result.blocker) {
		case documents::DocumentPatchBlocker::LocatorAmbiguous:
			return "Its words stand in more than one place in the document, so it was not written.";
		case documents::DocumentPatchBlocker::LocatorUnresolved:
			return "Its words could not be found in the document as Word holds it, so it was not written.";
		case documents::DocumentPatchBlocker::InvalidNewValue:
			return "Its new words could not be written into this document, so it was not written.";
		default:
			return "It could not be written into the document, so it was not written.";
	}

}

} // namespace

RevisionOutcome writeRevisedDocument(const std::filesystem::path& source, const std::filesystem::path& target,
	std::vector<ProposedChange>& changes, const std::string& author) {
	std::vector<MarginComment> none;
	return writeRevisedDocument(source, target, changes, none, author);
}

RevisionOutcome writeRevisedDocument(const std::filesystem::path& source, const std::filesystem::path& target,
	std::vector<ProposedChange>& changes, std::vector<MarginComment>& comments, const std::string& author) {

	RevisionOutcome outcome;
	std::error_code error;
	if (std::filesystem::exists(target, error) == true) {
		outcome.problem = "A file with the revised draft's name already exists, so nothing was written over it.";
		return outcome;
	}
	const std::filesystem::path staged = target.parent_path()
		/ std::filesystem::u8path("." + target.filename().u8string() + "." + platform::secure_random_hex(6) + ".writing");
	// The source's bytes only: a read-only source still gives a draft that can
	// be written (Platform/AtomicFile).
	try {
		platform::stage_copy_of_file(source, staged);
	} catch (const std::exception&) {
		std::filesystem::remove(staged, error);
		outcome.problem = "The source could not be copied for the revised draft, so nothing was written.";
		return outcome;
	}

	/* - - - - - - */

	// The comments first, on the source's own words: a clause's words stand as
	// the review read them until the changes are written.
	std::size_t placedComments = 0;
	if (comments.empty() == false) placedComments = placeMarginComments(staged, comments, author);

	// Each change in the order that keeps every anchor where the source has it
	// once (writingOrder).
	std::vector<std::vector<std::string>> anchors;
	std::vector<std::vector<std::string>> inserts;
	for (const ProposedChange& change : changes) {
		anchors.emplace_back();
		inserts.emplace_back();
		// A table's cells and rows are named by their places, not by words.
		if (change.isWritable == false || change.action == "table") continue;
		if (change.action == "rewrite" && change.edits.empty() == false) {
			for (const TextEdit& edit : change.edits) {
				anchors.back().push_back(platform::trimmed_phrase(edit.currentText));
				if (edit.proposedText.empty() == false) inserts.back().push_back(edit.proposedText);
			}
			continue;
		}
		anchors.back().push_back(platform::trimmed_phrase(change.currentText));
		if (change.action != "delete" && change.proposedText.empty() == false) inserts.back().push_back(change.proposedText);
	}
	for (const std::size_t index : writingOrder(anchors, inserts)) {
		ProposedChange& change = changes[index];
		if (change.isWritable == false) {
			change.writeState = "not_written";
			if (change.writeNote.empty() == true) change.writeNote = change.checkNote;
			continue;
		}
		documents::DocxPatchApplyResult result;
		if (change.action == "table") {
			std::string problem;
			const std::optional<documents::DocumentPatch> patch = tablePatchFor(staged, change, author, problem);
			if (patch.has_value() == false) {
				change.writeState = "refused";
				change.writeNote = problem;
				++outcome.refused;
				continue;
			}
			result = documents::apply_docx_document_patch(staged, *patch);
		} else {
			result = documents::apply_docx_document_patch(staged, patchFor(change, author));
		}
		if (result.ok == true) {
			change.writeState = "written";
			change.writeNote.clear();
			++outcome.written;
			continue;
		}
		change.writeState = "refused";
		change.writeNote = change.action == "table"
			? "The table could not be changed as Word holds it, so none of its changes was written."
			: refusalWords(result);
		++outcome.refused;
	}

	if (outcome.written == 0 && placedComments == 0) {
		std::filesystem::remove(staged, error);
		outcome.problem = "None of the proposed changes could be written into the document, so no revised draft was made.";
		return outcome;
	}
	outcome.isTrackingOn = turnOnTrackChanges(staged);
	if (placedComments > 0) {
		outcome.commented = readBackComments(staged, comments, author);
		if (outcome.written == 0 && outcome.commented == 0) {
			std::filesystem::remove(staged, error);
			outcome.problem = "The comments did not read back on their clauses, so no revised draft was made.";
			return outcome;
		}
	}

	/* - - - - - - */

	// Read back as a Word document, every revision accepted: each change's new
	// words are in it, or it is not called written.
	documents::DocumentExtractionRequest request;
	request.path = staged;
	const documents::DocumentExtractionResult accepted = documents::extract_docx_accepted_reading(request);
	if (accepted.status != documents::ExtractionStatus::ready) {
		std::filesystem::remove(staged, error);
		outcome.problem = "The revised draft did not read back as a Word document, so it was not kept.";
		for (ProposedChange& change : changes) {
			if (change.writeState != "written") continue;
			change.writeState = "refused";
			change.writeNote = "The revised draft did not read back as a Word document, so it was not kept.";
		}
		outcome.written = 0;
		return outcome;
	}
	for (ProposedChange& change : changes) {
		if (change.writeState != "written" || change.action == "delete") continue;
		if (change.action == "table") {
			// Each cell's new words, and each new row's, are in the table as Word
			// shows it.
			bool isRead = true;
			for (const TableEdit& edit : change.tableEdits) {
				std::vector<std::string> words;
				if (edit.kind == "cell") words.push_back(edit.proposedText);
				if (edit.kind == "add_row") words = edit.cells;
				for (const std::string& cell : words) {
					const std::string phrase = platform::trimmed_phrase(cell);
					if (phrase.empty() == false && platform::locate_phrase(accepted.text, phrase).matches == 0) isRead = false;
				}
			}
			if (isRead == false) {
				change.writeNote = "Written as tracked changes; some of the table's new words read back differently than "
					"proposed, so check it in Word.";
			}
			continue;
		}
		if (platform::locate_phrase(accepted.text, platform::trimmed_phrase(change.proposedText)).matches == 0) {
			change.writeNote = "Written as a tracked change; its new words read back differently than proposed, so check "
				"it in Word.";
		}
	}

	std::filesystem::rename(staged, target, error);
	if (error) {
		std::filesystem::remove(staged, error);
		outcome.problem = "The revised draft could not be put in its place, so it was not kept.";
		for (ProposedChange& change : changes) {
			if (change.writeState != "written") continue;
			change.writeState = "refused";
			change.writeNote = outcome.problem;
		}
		outcome.written = 0;
		return outcome;
	}
	outcome.isWritten = true;
	outcome.path = target;
	outcome.sha256 = platform::sha256_hex_digest_of_file(target);
	return outcome;

}

} // namespace singularity::document_intelligence
