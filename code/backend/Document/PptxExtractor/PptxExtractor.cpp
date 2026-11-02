#include "Document/PptxExtractor/PptxExtractor.hpp"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace singularity::documents {
namespace {

constexpr std::string_view kExtractorVersion = "pptx-drawingml/1";
constexpr std::uint64_t kMaximumXmlBytes = 16ULL * 1024ULL * 1024ULL;
constexpr zip_int64_t kMaximumArchiveEntries = 4096;

class PptxError final : public std::runtime_error {
public:
	PptxError(std::string errorCode, std::string publicMessage)
			: std::runtime_error(publicMessage), code(std::move(errorCode)) {}
	std::string code;
};

class ZipArchive final {
public:
	explicit ZipArchive(const std::filesystem::path &path) {
		int errorCode = 0;
		archive_ = zip_open(path.u8string().c_str(), ZIP_RDONLY, &errorCode);
		if (archive_ == nullptr) {
			throw PptxError("pptx_invalid_archive", "The PPTX package could not be opened as a ZIP archive.");
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

std::string_view name_prefix(std::string_view qualifiedName) {
	const auto separator = qualifiedName.find(':');
	return separator == std::string_view::npos ? std::string_view() : qualifiedName.substr(0, separator);
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

// Distinguishes a relationship-id attribute (r:id, e.g. on <p:sldId r:id=...>)
// from a same-local-name, unprefixed "id" attribute on the very same element
// (e.g. <p:sldId id="256" r:id="rId7"/> - the numeric slide id is not the
// relationship id).
std::string relationship_id_attribute(const pugi::xml_node &node) {
	for (const auto &attribute : node.attributes()) {
		if (local_name(attribute.name()) == "id" && name_prefix(attribute.name()) == "r") return attribute.value();
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
		throw PptxError("pptx_xml_part_size_limit", "A PPTX XML part exceeds the read limit.");
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
		throw PptxError("pptx_xml_read_failed", "A PPTX XML part could not be read completely.");
	}
	return bytes;
}

pugi::xml_document parse_xml(const std::string &bytes, const char *diagnosticCode, const char *publicMessage) {
	if (bytes.find("<!DOCTYPE") != std::string::npos || bytes.find("<!ENTITY") != std::string::npos) {
		throw PptxError("pptx_unsafe_xml", "DTD and entity declarations are not accepted in PPTX XML.");
	}
	pugi::xml_document document;
	const auto parsed = document.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8);
	if (!parsed) throw PptxError(diagnosticCode, publicMessage);
	return document;
}

void append_text_body(const pugi::xml_node &textBody, std::string &output) {
	bool firstParagraph = true;
	for (const auto &paragraph : textBody.children()) {
		if (!is_element(paragraph, "p")) continue;
		std::string paragraphText;
		for (const auto &node : paragraph.children()) {
			if (is_element(node, "r")) {
				for (const auto &child : node.children()) {
					if (is_element(child, "t")) paragraphText += child.child_value();
				}
			} else if (is_element(node, "br")) {
				paragraphText.push_back(' ');
			}
		}
		paragraphText = trim_ascii(std::move(paragraphText));
		if (paragraphText.empty()) continue;
		if (!firstParagraph) output.push_back('\n');
		output += paragraphText;
		firstParagraph = false;
	}
}

pugi::xml_node find_text_body(const pugi::xml_node &shape) {
	for (const auto &child : shape.children()) {
		if (is_element(child, "txBody")) return child;
	}
	return {};
}

pugi::xml_node find_table(const pugi::xml_node &graphicFrame) {
	for (const auto &graphic : graphicFrame.children()) {
		if (!is_element(graphic, "graphic")) continue;
		for (const auto &graphicData : graphic.children()) {
			if (!is_element(graphicData, "graphicData")) continue;
			for (const auto &candidate : graphicData.children()) {
				if (is_element(candidate, "tbl")) return candidate;
			}
		}
	}
	return {};
}

DocumentTable read_table(const pugi::xml_node &tableNode, const std::string &tableId) {
	DocumentTable table;
	table.table_id = tableId;

	std::vector<pugi::xml_node> rowNodes;
	for (const auto &row : tableNode.children()) {
		if (is_element(row, "tr")) rowNodes.push_back(row);
	}
	if (rowNodes.empty()) return table;

	for (const auto &cell : rowNodes.front().children()) {
		if (!is_element(cell, "tc")) continue;
		std::string cellText;
		const auto textBody = find_text_body(cell);
		if (textBody) append_text_body(textBody, cellText);
		table.column_headers.push_back(trim_ascii(std::move(cellText)));
	}

	std::unordered_set<std::string> usedKeys;
	for (std::size_t rowIndex = 1; rowIndex < rowNodes.size(); ++rowIndex) {
		DocumentTableRow row;
		row.row_index = rowIndex;
		std::vector<std::string> cellTexts;
		for (const auto &cell : rowNodes[rowIndex].children()) {
			if (!is_element(cell, "tc")) continue;
			std::string cellText;
			const auto textBody = find_text_body(cell);
			if (textBody) append_text_body(textBody, cellText);
			cellTexts.push_back(trim_ascii(std::move(cellText)));
		}
		const std::string firstCell = cellTexts.empty() ? std::string() : cellTexts.front();
		row.row_key = derive_table_row_key(firstCell, rowIndex, usedKeys);
		for (std::size_t column = 0; column < cellTexts.size(); ++column) {
			DocumentTableCell cell;
			cell.column_index = column;
			cell.column_key = column < table.column_headers.size() ? table.column_headers[column] : std::string();
			cell.text = cellTexts[column];
			row.cells.push_back(std::move(cell));
		}
		table.rows.push_back(std::move(row));
	}
	return table;
}

DocumentSlide read_slide(const pugi::xml_document &slideDocument, const std::string &slideId,
		unsigned int slideNumber) {
	DocumentSlide slide;
	slide.slide_id = slideId;
	slide.slide_number = slideNumber;

	pugi::xml_node spTree;
	for (const auto &sld : slideDocument.children()) {
		if (!is_element(sld, "sld")) continue;
		for (const auto &cSld : sld.children()) {
			if (!is_element(cSld, "cSld")) continue;
			for (const auto &tree : cSld.children()) {
				if (is_element(tree, "spTree")) spTree = tree;
			}
		}
	}
	if (!spTree) return slide;

	std::size_t shapeCounter = 0;
	std::size_t tableCounter = 0;
	for (const auto &shape : spTree.children()) {
		if (is_element(shape, "sp")) {
			++shapeCounter;
			const auto textBody = find_text_body(shape);
			if (!textBody) continue;
			std::string text;
			append_text_body(textBody, text);
			if (text.empty()) continue;
			DocumentSlideTextBlock block;
			block.shape_id = "shape-" + std::to_string(shapeCounter);
			block.text = text;
			slide.text_blocks.push_back(std::move(block));
			continue;
		}
		if (is_element(shape, "graphicFrame")) {
			const auto tableNode = find_table(shape);
			if (!tableNode) continue;
			++tableCounter;
			slide.tables.push_back(
					read_table(tableNode, slideId + "-table-" + std::to_string(tableCounter)));
		}
	}
	return slide;
}

// The real presentation-order slide part list: ppt/presentation.xml's
// <p:sldIdLst> gives the ordered relationship ids, ppt/_rels/presentation.xml.rels
// resolves each to its slides/slideN.xml target. Never assumes slideN.xml
// file-name order is presentation order - PowerPoint does not guarantee that.
std::vector<std::string> ordered_slide_parts(zip_t *archive) {
	const auto presentationXml = read_entry_by_name(archive, "ppt/presentation.xml", kMaximumXmlBytes);
	if (!presentationXml.has_value()) throw PptxError("pptx_missing_presentation_xml",
			"The PPTX package does not contain ppt/presentation.xml.");
	const auto presentationDocument =
			parse_xml(*presentationXml, "pptx_malformed_presentation_xml", "ppt/presentation.xml is malformed.");

	std::vector<std::string> relationshipIdsInOrder;
	for (const auto &presentation : presentationDocument.children()) {
		if (!is_element(presentation, "presentation")) continue;
		for (const auto &sldIdLst : presentation.children()) {
			if (!is_element(sldIdLst, "sldIdLst")) continue;
			for (const auto &sldId : sldIdLst.children()) {
				if (!is_element(sldId, "sldId")) continue;
				relationshipIdsInOrder.push_back(relationship_id_attribute(sldId));
			}
		}
	}

	const auto relsXml = read_entry_by_name(archive, "ppt/_rels/presentation.xml.rels", kMaximumXmlBytes);
	if (!relsXml.has_value()) throw PptxError("pptx_missing_presentation_rels",
			"The PPTX package does not contain ppt/_rels/presentation.xml.rels.");
	const auto relsDocument =
			parse_xml(*relsXml, "pptx_malformed_presentation_rels", "ppt/_rels/presentation.xml.rels is malformed.");

	std::unordered_map<std::string, std::string> targetByRelationshipId;
	for (const auto &relationships : relsDocument.children()) {
		if (!is_element(relationships, "Relationships")) continue;
		for (const auto &relationship : relationships.children()) {
			if (!is_element(relationship, "Relationship")) continue;
			targetByRelationshipId[attribute_value(relationship, "Id")] = attribute_value(relationship, "Target");
		}
	}

	std::vector<std::string> parts;
	for (const std::string &relationshipId : relationshipIdsInOrder) {
		const auto found = targetByRelationshipId.find(relationshipId);
		if (found == targetByRelationshipId.end()) {
			throw PptxError("pptx_unresolved_slide_relationship",
					"A PPTX slide relationship could not be resolved.");
		}
		// Targets are stored relative to ppt/ (e.g. "slides/slide3.xml").
		parts.push_back("ppt/" + found->second);
	}
	return parts;
}

} // namespace

const char *pptx_extractor_version() noexcept { return kExtractorVersion.data(); }

std::vector<std::string> pptx_slide_part_names(const std::filesystem::path &path) {
	try {
		ZipArchive archive(path);
		const auto entryCount = zip_get_num_entries(archive.get(), 0);
		if (entryCount < 0 || entryCount > kMaximumArchiveEntries) return {};
		return ordered_slide_parts(archive.get());
	} catch (const std::exception &) {
		return {};
	}
}

std::vector<DocumentSlide> extract_pptx_slides(const DocumentExtractionRequest &request) {
	std::vector<DocumentSlide> slides;
	try {
		ZipArchive archive(request.path);
		const auto entryCount = zip_get_num_entries(archive.get(), 0);
		if (entryCount < 0 || entryCount > kMaximumArchiveEntries) return slides;

		const std::vector<std::string> parts = ordered_slide_parts(archive.get());
		for (std::size_t index = 0; index < parts.size(); ++index) {
			const auto slideXml = read_entry_by_name(archive.get(), parts[index], kMaximumXmlBytes);
			if (!slideXml.has_value()) continue;
			const auto slideDocument =
					parse_xml(*slideXml, "pptx_malformed_slide_xml", "A PPTX slide part is malformed.");
			slides.push_back(read_slide(slideDocument, "slide-" + std::to_string(index + 1),
					static_cast<unsigned int>(index + 1)));
		}
	} catch (const PptxError &) {
		return {};
	} catch (const std::exception &) {
		return {};
	}
	return slides;
}

DocumentExtractionResult extract_pptx_document(const DocumentExtractionRequest &request) {
	DocumentExtractionResult result;
	result.format = DocumentFormat::pptx;
	result.extractor_version = std::string(kExtractorVersion);

	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(request.path, filesystemError) || filesystemError) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"unreadable_file", "The PPTX document is missing or is not a regular file."});
		return result;
	}
	const auto archiveSize = std::filesystem::file_size(request.path, filesystemError);
	if (filesystemError || archiveSize > request.maximum_input_bytes) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"input_too_large", "The PPTX document exceeds the extraction limit."});
		return result;
	}

	std::vector<std::string> lines;
	std::vector<DocumentHeading> headings;
	std::size_t nextLine = 1;
	std::size_t paragraphCount = 0;
	try {
		const std::vector<DocumentSlide> slides = extract_pptx_slides(request);
		if (slides.empty()) {
			result.status = ExtractionStatus::unreadable;
			result.diagnostics.push_back({"pptx_no_slides", "No slides could be read from the PPTX package."});
			return result;
		}
		for (const DocumentSlide &slide : slides) {
			if (!slide.text_blocks.empty()) {
				headings.push_back({nextLine, 1, "Slide " + std::to_string(slide.slide_number)});
			}
			for (const DocumentSlideTextBlock &block : slide.text_blocks) {
				lines.push_back(block.text);
				++nextLine;
				++paragraphCount;
			}
			for (const DocumentTable &table : slide.tables) {
				for (const std::string &header : table.column_headers) lines.push_back(header);
				if (!table.column_headers.empty()) { lines.emplace_back(); ++nextLine; }
				for (const DocumentTableRow &row : table.rows) {
					std::string rendered;
					for (std::size_t column = 0; column < row.cells.size(); ++column) {
						if (column != 0) rendered.push_back('\t');
						rendered += row.cells[column].text;
					}
					lines.push_back(std::move(rendered));
					++nextLine;
				}
			}
			lines.emplace_back();
			++nextLine;
		}
	} catch (const std::exception &) {
		result.status = ExtractionStatus::unreadable;
		result.diagnostics.push_back({"pptx_extraction_failed", "The PPTX document could not be extracted safely."});
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
	result.structure.paragraph_count = paragraphCount;
	result.structure.headings = std::move(headings);

	const bool hasContent = std::any_of(result.text.begin(), result.text.end(), [](unsigned char byte) {
		return std::isspace(byte) == 0;
	});
	if (!hasContent) {
		result.status = ExtractionStatus::empty;
		result.diagnostics.push_back({"empty_document", "The PPTX document contains no extractable text."});
		return result;
	}
	result.status = ExtractionStatus::ready;
	return result;
}

} // namespace singularity::documents
