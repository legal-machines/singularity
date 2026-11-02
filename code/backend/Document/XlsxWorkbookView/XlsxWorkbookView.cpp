#include "Document/XlsxWorkbookView/XlsxWorkbookView.hpp"
#include "Document/XlsxNumberFormat/XlsxNumberFormat.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pugixml.hpp>
#include <zip.h>

namespace singularity::documents {
namespace {

// XlsxExtractor's own safety limits, so both readings of a workbook stop at the
// same place.
constexpr std::uint64_t kMaximumXmlBytes = 24ULL * 1024ULL * 1024ULL;
constexpr zip_int64_t kMaximumArchiveEntries = 4096;
constexpr std::size_t kMaximumCellsPerSheet = 200000;

// Excel's own grid.
constexpr std::size_t kMaximumRows = 1048576;
constexpr std::size_t kMaximumColumns = 16384;

class WorkbookViewError final : public std::runtime_error {
public:
	WorkbookViewError(std::string errorCode, const std::string& publicMessage)
		: std::runtime_error(publicMessage)
		, code(std::move(errorCode))
	{
	}
	std::string code;
};

class ZipArchive final {
public:
	explicit ZipArchive(const std::filesystem::path& path) {
		int errorCode = 0;
		archive_ = zip_open(path.string().c_str(), ZIP_RDONLY, &errorCode);
		if (archive_ == nullptr) {
			throw WorkbookViewError("xlsx_invalid_archive", "The XLSX package could not be opened as a ZIP archive.");
		}
	}
	ZipArchive(const ZipArchive&) = delete;
	ZipArchive& operator=(const ZipArchive&) = delete;
	~ZipArchive() {
		if (archive_ != nullptr) zip_discard(archive_);
	}

	zip_t* get() const { return archive_; }

private:
	zip_t* archive_ = nullptr;
};

/* - - - - - - */

std::string_view local_name(std::string_view qualifiedName) {
	const std::size_t separator = qualifiedName.find(':');
	return separator == std::string_view::npos ? qualifiedName : qualifiedName.substr(separator + 1);
}

bool is_element(const pugi::xml_node& node, std::string_view name) {
	return node.type() == pugi::node_element && local_name(node.name()) == name;
}

pugi::xml_node child_element(const pugi::xml_node& node, std::string_view name) {
	for (const pugi::xml_node& child : node.children()) {
		if (is_element(child, name) == true) return child;
	}
	return pugi::xml_node();
}

pugi::xml_attribute find_attribute(const pugi::xml_node& node, std::string_view name) {
	for (const pugi::xml_attribute& attribute : node.attributes()) {
		if (local_name(attribute.name()) == name) return attribute;
	}
	return pugi::xml_attribute();
}

std::string attribute_value(const pugi::xml_node& node, std::string_view name) {
	const pugi::xml_attribute attribute = find_attribute(node, name);
	return attribute ? attribute.value() : std::string();
}

bool has_attribute(const pugi::xml_node& node, std::string_view name) {
	return static_cast<bool>(find_attribute(node, name));
}

// OOXML booleans: "1"/"true" and "0"/"false".
bool attribute_flag(const pugi::xml_node& node, std::string_view name, bool fallback) {
	const pugi::xml_attribute attribute = find_attribute(node, name);
	if (!attribute) return fallback;
	const std::string value = attribute.value();
	if (value == "1" || value == "true") return true;
	if (value == "0" || value == "false") return false;
	return fallback;
}

// Numbers in the package are written the C way whatever the process locale
// says; a value with anything after the number is not a number.
bool parse_classic_double(const std::string& text, double& value) {
	std::istringstream stream(text);
	stream.imbue(std::locale::classic());
	stream >> value;
	if (stream.fail() == true) return false;
	stream >> std::ws;
	return stream.eof() == true && std::isfinite(value) == true;
}

std::optional<double> attribute_double(const pugi::xml_node& node, std::string_view name) {
	const pugi::xml_attribute attribute = find_attribute(node, name);
	if (!attribute) return std::nullopt;
	double value = 0.0;
	if (parse_classic_double(attribute.value(), value) == false) return std::nullopt;
	return value;
}

std::optional<unsigned int> attribute_index(const pugi::xml_node& node, std::string_view name) {
	const std::optional<double> value = attribute_double(node, name);
	if (value.has_value() == false || *value < 0.0 || *value > 4294967295.0) return std::nullopt;
	return static_cast<unsigned int>(*value);
}

std::string trim_ascii(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

void append_utf8(std::string& text, unsigned int codePoint) {
	if (codePoint < 0x80) {
		text.push_back(static_cast<char>(codePoint));
	} else if (codePoint < 0x800) {
		text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
		text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
	} else if (codePoint < 0x10000) {
		text.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
		text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
		text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
	} else {
		text.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
		text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
		text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
		text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
	}
}

bool is_hex_digit(char character) {
	return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')
		|| (character >= 'A' && character <= 'F');
}

// SpreadsheetML writes a character XML cannot hold as "_xHHHH_" (Excel's line
// breaks arrive as "_x000D_\n"); "_x005F_" escapes a literal underscore.
std::string decode_escaped_characters(const std::string& text) {

	if (text.find("_x") == std::string::npos) return text;

	std::string decoded;
	std::size_t index = 0;
	while (index < text.size()) {
		const bool isEscape = text[index] == '_' && index + 6 < text.size() && text[index + 1] == 'x'
			&& is_hex_digit(text[index + 2]) && is_hex_digit(text[index + 3]) && is_hex_digit(text[index + 4])
			&& is_hex_digit(text[index + 5]) && text[index + 6] == '_';
		if (isEscape == false) {
			decoded.push_back(text[index]);
			++index;
			continue;
		}
		const unsigned int codePoint = static_cast<unsigned int>(std::strtoul(text.substr(index + 2, 4).c_str(), nullptr, 16));
		if (codePoint == 0x0D && index + 7 < text.size() && text[index + 7] == '\n') {
			index += 7; // "\r\n" is one line break.
			continue;
		}
		append_utf8(decoded, codePoint);
		index += 7;
	}
	return decoded;

}

/* - - - - - - */

// A package part's bytes. A part past the read limit (`limit`, the reading's
// own by default) comes back cut at the limit with `is_cut` set, so the caller
// can keep what is whole in it.
struct PackagePart {
	bool is_found = false;
	bool is_cut = false;
	std::string bytes;
};

PackagePart read_part(zip_t* archive, const std::string& name, std::uint64_t limit = kMaximumXmlBytes) {

	PackagePart part;
	if (name.empty() == true) return part;

	zip_stat_t entry;
	zip_stat_init(&entry);
	if (zip_stat(archive, name.c_str(), ZIP_FL_NOCASE, &entry) != 0) return part;
	if ((entry.valid & ZIP_STAT_SIZE) == 0) return part;

	const std::uint64_t wanted = std::min<std::uint64_t>(entry.size, limit);
	zip_file_t* file = zip_fopen(archive, name.c_str(), ZIP_FL_NOCASE);
	if (file == nullptr) return part;
	std::string bytes(static_cast<std::size_t>(wanted), '\0');
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const zip_int64_t read = zip_fread(file, &bytes[offset], static_cast<zip_uint64_t>(bytes.size() - offset));
		if (read <= 0) break;
		offset += static_cast<std::size_t>(read);
	}
	zip_fclose(file);
	if (offset != bytes.size()) {
		throw WorkbookViewError("xlsx_xml_read_failed", "An XLSX XML part could not be read completely.");
	}

	part.is_found = true;
	part.is_cut = entry.size > limit;
	part.bytes = std::move(bytes);
	return part;

}

// What is whole in a part cut at the read limit: everything up to the last
// complete `element`, closed again the way the part itself opened its
// enclosing elements (innermost first). False when not one element is whole.
bool keep_whole_elements(std::string& bytes, const std::string& element, const std::vector<std::string>& enclosing) {

	// The root element's own prefix ("x:worksheet" writes "x:row").
	std::size_t rootStart = 0;
	while ((rootStart = bytes.find('<', rootStart)) != std::string::npos) {
		if (rootStart + 1 < bytes.size() && bytes[rootStart + 1] != '?' && bytes[rootStart + 1] != '!') break;
		++rootStart;
	}
	if (rootStart == std::string::npos) return false;
	const std::size_t rootNameEnd = bytes.find_first_of(" \t\r\n/>", rootStart + 1);
	if (rootNameEnd == std::string::npos) return false;
	const std::string rootName = bytes.substr(rootStart + 1, rootNameEnd - rootStart - 1);
	const std::size_t colon = rootName.find(':');
	const std::string prefix = colon == std::string::npos ? std::string() : rootName.substr(0, colon + 1);

	const std::string closing = "</" + prefix + element + ">";
	const std::size_t last = bytes.rfind(closing);
	if (last == std::string::npos) return false;
	bytes.resize(last + closing.size());
	for (const std::string& name : enclosing) bytes += "</" + prefix + name + ">";
	return true;

}

pugi::xml_document parse_part(const std::string& bytes, const char* errorCode, const char* publicMessage) {
	if (bytes.find("<!DOCTYPE") != std::string::npos || bytes.find("<!ENTITY") != std::string::npos) {
		throw WorkbookViewError("xlsx_unsafe_xml", "DTD and entity declarations are not accepted in XLSX XML.");
	}
	pugi::xml_document document;
	// A text node that is only spaces (<t xml:space="preserve"> </t>) is text.
	const pugi::xml_parse_result parsed = document.load_buffer(bytes.data(), bytes.size(),
		pugi::parse_default | pugi::parse_ws_pcdata_single, pugi::encoding_utf8);
	if (!parsed) throw WorkbookViewError(errorCode, publicMessage);
	return document;
}

pugi::xml_node root_element(const pugi::xml_document& document, std::string_view name) {
	for (const pugi::xml_node& child : document.children()) {
		if (is_element(child, name) == true) return child;
	}
	return pugi::xml_node();
}

/* - - - - - - */

// "xl/workbook.xml" -> "xl/", and a relationship target resolved against it:
// package-relative ("worksheets/sheet1.xml"), package-absolute
// ("/xl/worksheets/sheet1.xml", as openpyxl writes) and "../" all resolve to
// the real part name.
std::string part_directory(const std::string& partName) {
	const std::size_t slash = partName.rfind('/');
	return slash == std::string::npos ? std::string() : partName.substr(0, slash + 1);
}

std::string resolve_target(const std::string& directory, const std::string& target) {

	const std::string joined = !target.empty() && target.front() == '/' ? target.substr(1) : directory + target;
	std::vector<std::string> segments;
	std::size_t start = 0;
	while (start <= joined.size()) {
		const std::size_t slash = joined.find('/', start);
		const std::string segment = joined.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (segment == "..") {
			if (segments.empty() == false) segments.pop_back();
		} else if (segment.empty() == false && segment != ".") {
			segments.push_back(segment);
		}
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	std::string resolved;
	for (const std::string& segment : segments) {
		if (resolved.empty() == false) resolved.push_back('/');
		resolved += segment;
	}
	return resolved;

}

struct Relationship {
	std::string type;
	std::string target;
};

bool type_ends_with(const Relationship& relationship, const char* suffix) {
	const std::size_t length = std::strlen(suffix);
	return relationship.type.size() >= length
		&& relationship.type.compare(relationship.type.size() - length, length, suffix) == 0;
}

std::unordered_map<std::string, Relationship> read_relationships(zip_t* archive, const std::string& partName) {

	std::unordered_map<std::string, Relationship> relationships;
	const std::string directory = part_directory(partName);
	const std::string baseName = partName.substr(directory.size());
	const PackagePart part = read_part(archive, directory + "_rels/" + baseName + ".rels");
	if (part.is_found == false) return relationships;

	const pugi::xml_document document =
		parse_part(part.bytes, "xlsx_malformed_relationships", "An XLSX relationships part is malformed.");
	for (const pugi::xml_node& relationship : root_element(document, "Relationships").children()) {
		if (is_element(relationship, "Relationship") == false) continue;
		if (attribute_value(relationship, "TargetMode") == "External") continue;
		Relationship entry;
		entry.type = attribute_value(relationship, "Type");
		entry.target = resolve_target(directory, attribute_value(relationship, "Target"));
		relationships[attribute_value(relationship, "Id")] = entry;
	}
	return relationships;

}

std::string find_relationship_target(const std::unordered_map<std::string, Relationship>& relationships,
		const char* typeSuffix) {
	for (const auto& entry : relationships) {
		if (type_ends_with(entry.second, typeSuffix) == true) return entry.second.target;
	}
	return std::string();
}

/* - - - - - - */

// Colours as a workbook states them: a theme slot with a tint, a legacy
// palette index, or ARGB.
struct ColorContext {
	// SpreadsheetML's theme index order: 0 lt1, 1 dk1, 2 lt2, 3 dk2,
	// 4..9 accent1..6, 10 hlink, 11 folHlink.
	std::vector<std::string> theme;
	std::vector<std::string> indexed;
};

// The Office theme Excel uses when a workbook carries none of its own.
std::vector<std::string> default_theme_colors() {
	return {"FFFFFF", "000000", "E7E6E6", "44546A", "4472C4", "ED7D31",
		"A5A5A5", "FFC000", "5B9BD5", "70AD47", "0563C1", "954F72"};
}

std::string normalized_hex(const std::string& value) {
	if (value.size() < 6) return std::string();
	std::string hex = value.substr(value.size() - 6);
	for (char& character : hex) {
		if (is_hex_digit(character) == false) return std::string();
		if (character >= 'a' && character <= 'f') character = static_cast<char>(character - 'a' + 'A');
	}
	return hex;
}

std::vector<std::string> read_theme_colors(zip_t* archive, const std::string& partName) {

	std::vector<std::string> colors = default_theme_colors();
	const PackagePart part = read_part(archive, partName);
	if (part.is_found == false || part.is_cut == true) return colors;

	const pugi::xml_document document = parse_part(part.bytes, "xlsx_malformed_theme", "The XLSX theme is malformed.");
	const pugi::xml_node theme = root_element(document, "theme");
	const pugi::xml_node scheme = child_element(child_element(theme, "themeElements"), "clrScheme");
	static const std::unordered_map<std::string, std::size_t> slots = {
		{"lt1", 0}, {"dk1", 1}, {"lt2", 2}, {"dk2", 3}, {"accent1", 4}, {"accent2", 5}, {"accent3", 6},
		{"accent4", 7}, {"accent5", 8}, {"accent6", 9}, {"hlink", 10}, {"folHlink", 11},
	};
	for (const pugi::xml_node& entry : scheme.children()) {
		if (entry.type() != pugi::node_element) continue;
		const auto slot = slots.find(std::string(local_name(entry.name())));
		if (slot == slots.end()) continue;
		std::string hex;
		const pugi::xml_node rgb = child_element(entry, "srgbClr");
		const pugi::xml_node system = child_element(entry, "sysClr");
		if (rgb) hex = normalized_hex(attribute_value(rgb, "val"));
		if (hex.empty() == true && system) {
			hex = normalized_hex(attribute_value(system, "lastClr"));
			if (hex.empty() == true) hex = attribute_value(system, "val") == "window" ? "FFFFFF" : "000000";
		}
		if (hex.empty() == false) colors[slot->second] = hex;
	}
	return colors;

}

// Excel's tint: the colour's HSL lightness moved towards black (a negative
// tint) or white (a positive one) by that fraction.
std::string apply_tint(const std::string& hex, double tint) {

	if (tint == 0.0 || hex.size() != 6) return hex;

	const double red = static_cast<double>(std::strtoul(hex.substr(0, 2).c_str(), nullptr, 16)) / 255.0;
	const double green = static_cast<double>(std::strtoul(hex.substr(2, 2).c_str(), nullptr, 16)) / 255.0;
	const double blue = static_cast<double>(std::strtoul(hex.substr(4, 2).c_str(), nullptr, 16)) / 255.0;
	const double maximum = std::max({red, green, blue});
	const double minimum = std::min({red, green, blue});
	double hue = 0.0;
	double saturation = 0.0;
	double lightness = (maximum + minimum) / 2.0;
	if (maximum != minimum) {
		const double delta = maximum - minimum;
		saturation = lightness > 0.5 ? delta / (2.0 - maximum - minimum) : delta / (maximum + minimum);
		if (maximum == red) hue = (green - blue) / delta + (green < blue ? 6.0 : 0.0);
		else if (maximum == green) hue = (blue - red) / delta + 2.0;
		else hue = (red - green) / delta + 4.0;
		hue /= 6.0;
	}

	/* - - - - - - */

	if (tint < 0.0) lightness = lightness * (1.0 + tint);
	else lightness = lightness * (1.0 - tint) + tint;

	/* - - - - - - */

	double channels[3] = {lightness, lightness, lightness};
	if (saturation != 0.0) {
		const double upper = lightness < 0.5 ? lightness * (1.0 + saturation) : lightness + saturation - lightness * saturation;
		const double lower = 2.0 * lightness - upper;
		const double offsets[3] = {1.0 / 3.0, 0.0, -1.0 / 3.0};
		for (int channel = 0; channel < 3; ++channel) {
			double position = hue + offsets[channel];
			if (position < 0.0) position += 1.0;
			if (position > 1.0) position -= 1.0;
			double value = lower;
			if (position < 1.0 / 6.0) value = lower + (upper - lower) * 6.0 * position;
			else if (position < 1.0 / 2.0) value = upper;
			else if (position < 2.0 / 3.0) value = lower + (upper - lower) * (2.0 / 3.0 - position) * 6.0;
			channels[channel] = value;
		}
	}
	std::string tinted;
	for (const double channel : channels) {
		const long level = std::lround(std::min(1.0, std::max(0.0, channel)) * 255.0);
		static const char* const digits = "0123456789ABCDEF";
		tinted.push_back(digits[(level >> 4) & 0xF]);
		tinted.push_back(digits[level & 0xF]);
	}
	return tinted;

}

// "RRGGBB", or "" for no colour, "auto", the system colours and anything the
// workbook does not resolve.
std::string resolve_color(const pugi::xml_node& color, const ColorContext& context) {

	if (!color) return std::string();
	if (attribute_flag(color, "auto", false) == true) return std::string();

	std::string hex;
	if (has_attribute(color, "rgb") == true) {
		hex = normalized_hex(attribute_value(color, "rgb"));
	} else if (has_attribute(color, "theme") == true) {
		const std::optional<unsigned int> slot = attribute_index(color, "theme");
		if (slot.has_value() == true && *slot < context.theme.size()) hex = context.theme[*slot];
	} else if (has_attribute(color, "indexed") == true) {
		const std::optional<unsigned int> slot = attribute_index(color, "indexed");
		if (slot.has_value() == true && *slot < context.indexed.size()) hex = context.indexed[*slot];
	}
	if (hex.empty() == true) return hex;

	const std::optional<double> tint = attribute_double(color, "tint");
	if (tint.has_value() == true) hex = apply_tint(hex, std::max(-1.0, std::min(1.0, *tint)));
	return hex;

}

/* - - - - - - */

struct FontRecord {
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strike = false;
	std::string color_hex;
	std::optional<double> size_points;
};

struct AlignmentRecord {
	std::string horizontal = "general";
	std::string vertical = "bottom";
	bool wrap = false;
	unsigned int indent = 0;
};

// One xf as the stylesheet writes it. A cell xf may leave a part to its cell
// style: by not naming it, or by saying apply...="0".
struct XfRecord {
	std::optional<unsigned int> number_format_id;
	std::optional<unsigned int> font_id;
	std::optional<unsigned int> fill_id;
	std::optional<unsigned int> border_id;
	std::optional<AlignmentRecord> alignment;
	bool is_number_format_inherited = false;
	bool is_font_inherited = false;
	bool is_fill_inherited = false;
	bool is_border_inherited = false;
	bool is_alignment_inherited = false;
	unsigned int style_xf_id = 0;
};

// A cell xf with everything its cell style gives it already resolved.
struct ResolvedStyle {
	std::string number_format = "General";
	SheetCellStyle style;
};

struct Stylesheet {
	std::vector<ResolvedStyle> cell_styles;
	ResolvedStyle default_style;
};

bool font_flag(const pugi::xml_node& font, std::string_view name) {
	const pugi::xml_node flag = child_element(font, name);
	if (!flag) return false;
	return attribute_flag(flag, "val", true);
}

FontRecord read_font(const pugi::xml_node& font, const ColorContext& colors) {
	FontRecord record;
	record.bold = font_flag(font, "b");
	record.italic = font_flag(font, "i");
	record.strike = font_flag(font, "strike");
	const pugi::xml_node underline = child_element(font, "u");
	record.underline = underline && attribute_value(underline, "val") != "none";
	record.color_hex = resolve_color(child_element(font, "color"), colors);
	const pugi::xml_node size = child_element(font, "sz");
	if (size) record.size_points = attribute_double(size, "val");
	return record;
}

std::string read_fill(const pugi::xml_node& fill, const ColorContext& colors) {
	const pugi::xml_node pattern = child_element(fill, "patternFill");
	if (!pattern || attribute_value(pattern, "patternType") != "solid") return std::string();
	return resolve_color(child_element(pattern, "fgColor"), colors);
}

// An edge's line style; right-to-left stylesheets may name the sides "end"
// and "start".
std::string border_edge(const pugi::xml_node& border, std::string_view name, std::string_view alternative) {
	pugi::xml_node edge = child_element(border, name);
	if (!edge) edge = child_element(border, alternative);
	const std::string style = edge ? attribute_value(edge, "style") : std::string();
	return style == "none" ? std::string() : style;
}

SheetCellBorders read_border(const pugi::xml_node& border) {
	SheetCellBorders borders;
	borders.top = border_edge(border, "top", "top");
	borders.right = border_edge(border, "right", "end");
	borders.bottom = border_edge(border, "bottom", "bottom");
	borders.left = border_edge(border, "left", "start");
	return borders;
}

AlignmentRecord read_alignment(const pugi::xml_node& alignment) {

	AlignmentRecord record;
	static const std::array<const char*, 8> horizontals = {
		"general", "left", "center", "right", "fill", "justify", "centerContinuous", "distributed",
	};
	const std::string horizontal = attribute_value(alignment, "horizontal");
	for (const char* known : horizontals) {
		if (horizontal == known) record.horizontal = horizontal;
	}
	const std::string vertical = attribute_value(alignment, "vertical");
	if (vertical == "top" || vertical == "center" || vertical == "bottom") record.vertical = vertical;
	// Justified and distributed lines start from the top of the cell.
	if (vertical == "justify" || vertical == "distributed") record.vertical = "top";
	record.wrap = attribute_flag(alignment, "wrapText", false);
	record.indent = attribute_index(alignment, "indent").value_or(0);
	return record;

}

XfRecord read_xf(const pugi::xml_node& xf, bool isCellXf) {
	XfRecord record;
	record.number_format_id = attribute_index(xf, "numFmtId");
	record.font_id = attribute_index(xf, "fontId");
	record.fill_id = attribute_index(xf, "fillId");
	record.border_id = attribute_index(xf, "borderId");
	const pugi::xml_node alignment = child_element(xf, "alignment");
	if (alignment) record.alignment = read_alignment(alignment);
	if (isCellXf == true) {
		record.is_number_format_inherited = attribute_flag(xf, "applyNumberFormat", true) == false;
		record.is_font_inherited = attribute_flag(xf, "applyFont", true) == false;
		record.is_fill_inherited = attribute_flag(xf, "applyFill", true) == false;
		record.is_border_inherited = attribute_flag(xf, "applyBorder", true) == false;
		record.is_alignment_inherited = attribute_flag(xf, "applyAlignment", true) == false;
		record.style_xf_id = attribute_index(xf, "xfId").value_or(0);
	}
	return record;
}

// The cell xf's own id, or its cell style's where the xf leaves that part to it.
std::optional<unsigned int> effective_id(const std::optional<unsigned int>& own, bool isInherited,
		const std::optional<unsigned int>* parentId) {
	if ((own.has_value() == false || isInherited == true) && parentId != nullptr && parentId->has_value() == true) {
		return *parentId;
	}
	return own;
}

Stylesheet read_stylesheet(zip_t* archive, const std::string& partName, const ColorContext& themeColors,
		std::string& limitation) {

	Stylesheet stylesheet;
	stylesheet.cell_styles.push_back(stylesheet.default_style);
	const PackagePart part = read_part(archive, partName);
	if (part.is_found == false) return stylesheet;
	if (part.is_cut == true) {
		limitation = "This workbook's formatting is too large to read, so its values are shown unformatted.";
		return stylesheet;
	}

	const pugi::xml_document document = parse_part(part.bytes, "xlsx_malformed_styles", "xl/styles.xml is malformed.");
	const pugi::xml_node root = root_element(document, "styleSheet");

	ColorContext colors = themeColors;
	const pugi::xml_node customPalette = child_element(child_element(root, "colors"), "indexedColors");
	std::size_t paletteIndex = 0;
	for (const pugi::xml_node& color : customPalette.children()) {
		if (is_element(color, "rgbColor") == false) continue;
		const std::string hex = normalized_hex(attribute_value(color, "rgb"));
		if (paletteIndex < colors.indexed.size() && hex.empty() == false) colors.indexed[paletteIndex] = hex;
		++paletteIndex;
	}

	std::unordered_map<unsigned int, std::string> customFormats;
	for (const pugi::xml_node& format : child_element(root, "numFmts").children()) {
		if (is_element(format, "numFmt") == false) continue;
		const std::optional<unsigned int> id = attribute_index(format, "numFmtId");
		if (id.has_value() == true) customFormats[*id] = attribute_value(format, "formatCode");
	}
	std::vector<FontRecord> fonts;
	for (const pugi::xml_node& font : child_element(root, "fonts").children()) {
		if (is_element(font, "font") == true) fonts.push_back(read_font(font, colors));
	}
	std::vector<std::string> fills;
	for (const pugi::xml_node& fill : child_element(root, "fills").children()) {
		if (is_element(fill, "fill") == true) fills.push_back(read_fill(fill, colors));
	}
	std::vector<SheetCellBorders> borders;
	for (const pugi::xml_node& border : child_element(root, "borders").children()) {
		if (is_element(border, "border") == true) borders.push_back(read_border(border));
	}
	std::vector<XfRecord> styleXfs;
	for (const pugi::xml_node& xf : child_element(root, "cellStyleXfs").children()) {
		if (is_element(xf, "xf") == true) styleXfs.push_back(read_xf(xf, false));
	}

	/* - - - - - - */

	// The workbook's own default text (its Normal style's font) is the reader's
	// ink: a cell in it carries no colour and no size of its own.
	const unsigned int defaultFontId = styleXfs.empty() == false ? styleXfs[0].font_id.value_or(0) : 0;
	const FontRecord defaultFont = defaultFontId < fonts.size() ? fonts[defaultFontId] : FontRecord();

	stylesheet.cell_styles.clear();
	for (const pugi::xml_node& node : child_element(root, "cellXfs").children()) {
		if (is_element(node, "xf") == false) continue;
		const XfRecord xf = read_xf(node, true);
		const XfRecord* parent = xf.style_xf_id < styleXfs.size() ? &styleXfs[xf.style_xf_id] : nullptr;

		ResolvedStyle resolved;
		const unsigned int formatId = effective_id(xf.number_format_id, xf.is_number_format_inherited,
			parent != nullptr ? &parent->number_format_id : nullptr).value_or(0);
		const auto custom = customFormats.find(formatId);
		resolved.number_format = custom != customFormats.end() ? custom->second : builtin_number_format(formatId);

		const std::optional<unsigned int> fontId =
			effective_id(xf.font_id, xf.is_font_inherited, parent != nullptr ? &parent->font_id : nullptr);
		if (fontId.has_value() == true && *fontId < fonts.size()) {
			const FontRecord& font = fonts[*fontId];
			resolved.style.bold = font.bold;
			resolved.style.italic = font.italic;
			resolved.style.underline = font.underline;
			resolved.style.strike = font.strike;
			if (font.color_hex != defaultFont.color_hex) resolved.style.font_color_hex = font.color_hex;
			if (font.size_points.has_value() == true && font.size_points != defaultFont.size_points) {
				resolved.style.font_size_points = font.size_points;
			}
		}
		const std::optional<unsigned int> fillId =
			effective_id(xf.fill_id, xf.is_fill_inherited, parent != nullptr ? &parent->fill_id : nullptr);
		if (fillId.has_value() == true && *fillId < fills.size()) resolved.style.fill_color_hex = fills[*fillId];
		const std::optional<unsigned int> borderId =
			effective_id(xf.border_id, xf.is_border_inherited, parent != nullptr ? &parent->border_id : nullptr);
		if (borderId.has_value() == true && *borderId < borders.size()) resolved.style.borders = borders[*borderId];

		std::optional<AlignmentRecord> alignment = xf.alignment;
		if ((alignment.has_value() == false || xf.is_alignment_inherited == true) && parent != nullptr
				&& parent->alignment.has_value() == true) {
			alignment = parent->alignment;
		}
		if (xf.is_alignment_inherited == true && (parent == nullptr || parent->alignment.has_value() == false)) {
			alignment.reset();
		}
		if (alignment.has_value() == true) {
			resolved.style.horizontal = alignment->horizontal;
			resolved.style.vertical = alignment->vertical;
			resolved.style.wrap = alignment->wrap;
			resolved.style.indent = alignment->indent;
		}
		stylesheet.cell_styles.push_back(resolved);
	}
	if (stylesheet.cell_styles.empty() == true) stylesheet.cell_styles.push_back(ResolvedStyle());
	stylesheet.default_style = stylesheet.cell_styles[0];
	return stylesheet;

}

/* - - - - - - */

// Rich text is its runs' text; a phonetic guide (<rPh>) is not part of it.
std::string rich_text(const pugi::xml_node& node) {
	std::string text;
	for (const pugi::xml_node& child : node.children()) {
		if (is_element(child, "t") == true) {
			text += child.child_value();
		} else if (is_element(child, "r") == true) {
			for (const pugi::xml_node& run : child.children()) {
				if (is_element(run, "t") == true) text += run.child_value();
			}
		}
	}
	return decode_escaped_characters(text);
}

std::vector<std::string> read_shared_strings(zip_t* archive, const std::string& partName, std::string& limitation) {

	std::vector<std::string> strings;
	PackagePart part = read_part(archive, partName);
	if (part.is_found == false) return strings;
	if (part.is_cut == true) {
		limitation = "Some of this workbook's text is too large to read, so those cells are shown empty.";
		if (keep_whole_elements(part.bytes, "si", {"sst"}) == false) return strings;
	}

	const pugi::xml_document document =
		parse_part(part.bytes, "xlsx_malformed_shared_strings", "xl/sharedStrings.xml is malformed.");
	for (const pugi::xml_node& item : root_element(document, "sst").children()) {
		if (is_element(item, "si") == true) strings.push_back(rich_text(item));
	}
	return strings;

}

/* - - - - - - */

std::string column_letters(std::size_t column) {
	std::string letters;
	std::size_t number = column + 1;
	while (number > 0) {
		const std::size_t remainder = (number - 1) % 26;
		letters.insert(letters.begin(), static_cast<char>('A' + remainder));
		number = (number - 1) / 26;
	}
	return letters;
}

std::string cell_reference(std::size_t row, std::size_t column) {
	return column_letters(column) + std::to_string(row + 1);
}

// One end of an A1 reference: "$B$4", "B4", "B" (a whole column), "4" (a whole
// row), each part absolute when "$" marks it.
struct ReferencePart {
	bool has_column = false;
	bool is_column_absolute = false;
	long long column = 0; // 1-based
	bool has_row = false;
	bool is_row_absolute = false;
	long long row = 0; // 1-based
};

bool parse_reference_part(const std::string& text, ReferencePart& part) {

	part = ReferencePart();
	std::size_t index = 0;
	if (index < text.size() && text[index] == '$') {
		part.is_column_absolute = true;
		++index;
	}
	const std::size_t lettersStart = index;
	while (index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) != 0) {
		part.column = part.column * 26 + (std::toupper(static_cast<unsigned char>(text[index])) - 'A' + 1);
		++index;
		if (index - lettersStart > 3) return false;
	}
	part.has_column = index > lettersStart;
	if (index < text.size() && text[index] == '$') {
		if (part.has_column == false) {
			// "$4" is an absolute row, not an absolute column.
			part.is_row_absolute = part.is_column_absolute;
			part.is_column_absolute = false;
		} else {
			part.is_row_absolute = true;
		}
		++index;
	} else if (part.has_column == false) {
		part.is_row_absolute = part.is_column_absolute;
		part.is_column_absolute = false;
	}
	const std::size_t digitsStart = index;
	while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
		part.row = part.row * 10 + (text[index] - '0');
		++index;
		if (index - digitsStart > 7) return false;
	}
	part.has_row = index > digitsStart;
	if (index != text.size()) return false;
	if (part.has_column == false && part.has_row == false) return false;
	if (part.has_column == true && (part.column < 1 || part.column > static_cast<long long>(kMaximumColumns))) return false;
	if (part.has_row == true && (part.row < 1 || part.row > static_cast<long long>(kMaximumRows))) return false;
	return true;

}

// A 0-based cell position from a cell's own r attribute ("B4").
bool parse_cell_position(const std::string& text, std::size_t& row, std::size_t& column) {
	ReferencePart part;
	if (parse_reference_part(text, part) == false || part.has_column == false || part.has_row == false) return false;
	row = static_cast<std::size_t>(part.row - 1);
	column = static_cast<std::size_t>(part.column - 1);
	return true;
}

// The reference moved by the offset, its "$" parts held; "" when it falls off
// the grid (Excel then shows #REF!).
std::string shift_reference_part(const ReferencePart& part, long long rowOffset, long long columnOffset) {
	std::string text;
	if (part.has_column == true) {
		const long long column = part.is_column_absolute == true ? part.column : part.column + columnOffset;
		if (column < 1 || column > static_cast<long long>(kMaximumColumns)) return std::string();
		if (part.is_column_absolute == true) text.push_back('$');
		text += column_letters(static_cast<std::size_t>(column - 1));
	}
	if (part.has_row == true) {
		const long long row = part.is_row_absolute == true ? part.row : part.row + rowOffset;
		if (row < 1 || row > static_cast<long long>(kMaximumRows)) return std::string();
		if (part.is_row_absolute == true) text.push_back('$');
		text += std::to_string(row);
	}
	return text;
}

bool is_name_character(char character) {
	const unsigned char byte = static_cast<unsigned char>(character);
	return std::isalnum(byte) != 0 || character == '_' || character == '.' || character == '$' || character == '\\'
		|| byte >= 0x80;
}

// A shared formula is written once, on its first cell; every other cell of the
// range says only "the same, from here". Excel shows each cell the master's
// text with every relative reference - cells, ranges, whole columns and rows,
// other sheets' references - moved by that cell's distance from the master.
// "$" parts, string literals, function names, sheet names, structured and
// external references stay as written.
std::string translate_shared_formula(const std::string& formula, long long rowOffset, long long columnOffset) {

	if (rowOffset == 0 && columnOffset == 0) return formula;

	std::string translated;
	std::size_t index = 0;
	const std::size_t size = formula.size();
	while (index < size) {
		const char character = formula[index];

		// "text", 'Sheet name' (both double their quote to escape it) and
		// [structured or external] references are copied untouched.
		if (character == '"' || character == '\'') {
			std::size_t end = index + 1;
			while (end < size) {
				if (formula[end] == character) {
					if (end + 1 < size && formula[end + 1] == character) {
						end += 2;
						continue;
					}
					break;
				}
				++end;
			}
			end = std::min(end + 1, size);
			translated.append(formula, index, end - index);
			index = end;
			continue;
		}
		if (character == '[') {
			std::size_t end = index;
			int depth = 0;
			while (end < size) {
				if (formula[end] == '[') ++depth;
				if (formula[end] == ']') --depth;
				++end;
				if (depth == 0) break;
			}
			translated.append(formula, index, end - index);
			index = end;
			continue;
		}
		if (character == '#') {
			std::size_t end = index + 1;
			while (end < size && (std::isalnum(static_cast<unsigned char>(formula[end])) != 0 || formula[end] == '/'
					|| formula[end] == '!' || formula[end] == '?' || formula[end] == '_')) {
				++end;
			}
			translated.append(formula, index, end - index);
			index = end;
			continue;
		}
		if (is_name_character(character) == false) {
			translated.push_back(character);
			++index;
			continue;
		}

		/* - - - - - - */

		std::size_t end = index;
		while (end < size && is_name_character(formula[end]) == true) ++end;
		const std::string token = formula.substr(index, end - index);
		const char next = end < size ? formula[end] : '\0';

		// A function name or a sheet name.
		if (next == '(' || next == '!') {
			translated += token;
			index = end;
			continue;
		}

		// A range: two references of the same shape around ":".
		if (next == ':') {
			std::size_t secondEnd = end + 1;
			while (secondEnd < size && is_name_character(formula[secondEnd]) == true) ++secondEnd;
			const std::string second = formula.substr(end + 1, secondEnd - end - 1);
			const char afterSecond = secondEnd < size ? formula[secondEnd] : '\0';
			// "Q1:Q4!A1" names a run of sheets, not a range.
			if (afterSecond == '!') {
				translated.append(formula, index, secondEnd - index);
				index = secondEnd;
				continue;
			}
			ReferencePart first;
			ReferencePart last;
			const bool isRange = afterSecond != '(' && parse_reference_part(token, first) == true
				&& parse_reference_part(second, last) == true && first.has_column == last.has_column
				&& first.has_row == last.has_row;
			if (isRange == true) {
				const std::string shiftedFirst = shift_reference_part(first, rowOffset, columnOffset);
				const std::string shiftedLast = shift_reference_part(last, rowOffset, columnOffset);
				if (shiftedFirst.empty() == true || shiftedLast.empty() == true) translated += "#REF!";
				else translated += shiftedFirst + ":" + shiftedLast;
				index = secondEnd;
				continue;
			}
		}

		// A number, with its exponent ("1E+5").
		if (std::isdigit(static_cast<unsigned char>(token[0])) != 0 || token[0] == '.') {
			translated += token;
			index = end;
			const char last = token.back();
			if ((last == 'E' || last == 'e') && index < size && (formula[index] == '+' || formula[index] == '-')) {
				translated.push_back(formula[index]);
				++index;
				while (index < size && std::isdigit(static_cast<unsigned char>(formula[index])) != 0) {
					translated.push_back(formula[index]);
					++index;
				}
			}
			continue;
		}

		// A single cell.
		ReferencePart cell;
		if (parse_reference_part(token, cell) == true && cell.has_column == true && cell.has_row == true) {
			const std::string shifted = shift_reference_part(cell, rowOffset, columnOffset);
			translated += shifted.empty() == true ? "#REF!" : shifted;
			index = end;
			continue;
		}

		// A defined name, TRUE, FALSE, ...
		translated += token;
		index = end;
	}
	return translated;

}

/* - - - - - - */

// An ISO 8601 date ("2023-03-15", "2023-03-15T10:30:00Z") as the serial the
// workbook's date system gives it.
bool iso_date_serial(const std::string& text, bool date1904, double& serial) {

	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	double second = 0.0;
	if (text.size() < 10 || std::sscanf(text.c_str(), "%4d-%2d-%2d", &year, &month, &day) != 3) return false;
	if (month < 1 || month > 12 || day < 1 || day > 31) return false;
	if (text.size() > 11 && (text[10] == 'T' || text[10] == ' ')) {
		if (std::sscanf(text.c_str() + 11, "%2d:%2d", &hour, &minute) != 2) return false;
		if (text.size() > 17 && text[16] == ':') {
			std::string seconds;
			for (std::size_t index = 17; index < text.size() && (std::isdigit(static_cast<unsigned char>(text[index])) != 0
					|| text[index] == '.'); ++index) {
				seconds.push_back(text[index]);
			}
			if (parse_classic_double(seconds, second) == false) second = 0.0;
		}
	}

	const std::optional<double> excelSerial =
		excel_date_serial(year, month, day, hour * 3600.0 + minute * 60.0 + second, date1904);
	if (excelSerial.has_value() == false) return false;
	serial = *excelSerial;
	return true;

}

// OOXML stores a formula without its "="; a writer that kept it gets no second one.
std::string formula_text(const std::string& stored) {
	return !stored.empty() && stored.front() == '=' ? stored : "=" + stored;
}

struct SharedFormula {
	std::string text;
	std::size_t row = 0;
	std::size_t column = 0;
};

struct SheetContext {
	const std::vector<std::string>* shared_strings = nullptr;
	const Stylesheet* stylesheet = nullptr;
	bool date1904 = false;
};

// A cell's formula as Excel's formula bar shows it, with its "=": a shared
// formula translated to the cell (`sharedFormulas` keeps each group's master as
// the sheet is read in order), a what-if data table as {=TABLE(...)}. Nothing
// where the cell holds none.
std::optional<std::string> read_formula(const pugi::xml_node& node, std::size_t row, std::size_t column,
		std::unordered_map<std::string, SharedFormula>& sharedFormulas) {

	const pugi::xml_node formula = child_element(node, "f");
	if (!formula) return std::nullopt;
	const std::string kind = attribute_value(formula, "t");
	const std::string text = formula.child_value();
	if (kind == "shared") {
		const std::string group = attribute_value(formula, "si");
		if (text.empty() == false) {
			sharedFormulas[group] = {text, row, column};
			return formula_text(text);
		}
		const auto master = sharedFormulas.find(group);
		if (master == sharedFormulas.end()) return std::nullopt;
		const long long rowOffset = static_cast<long long>(row) - static_cast<long long>(master->second.row);
		const long long columnOffset = static_cast<long long>(column) - static_cast<long long>(master->second.column);
		return formula_text(translate_shared_formula(master->second.text, rowOffset, columnOffset));
	}
	if (kind == "dataTable") {
		// Excel shows a what-if table's cells as {=TABLE(row input, column input)}.
		const std::string first = attribute_value(formula, "r1");
		const std::string second = attribute_value(formula, "r2");
		if (attribute_flag(formula, "dt2D", false) == true) return "=TABLE(" + first + "," + second + ")";
		if (attribute_flag(formula, "dtr", false) == true) return "=TABLE(" + first + ",)";
		return "=TABLE(," + first + ")";
	}
	if (text.empty() == false) return formula_text(text);
	return std::nullopt;

}

// The range the array formula or what-if data table of the cell at (row,
// column) fills, where it is one that is kept: an array formula over more than
// its first cell, and a data table whatever its size - even one cell of it
// fills itself from the formulas beside it. The sheet is not yet named.
std::optional<XlsxArrayFormula> array_formula_at(const pugi::xml_node& node, std::size_t row, std::size_t column) {

	const pugi::xml_node formula = child_element(node, "f");
	const std::string formulaKind = formula ? attribute_value(formula, "t") : std::string();
	if (formulaKind != "array" && formulaKind != "dataTable") return std::nullopt;
	const std::string range = attribute_value(formula, "ref");
	const std::size_t colon = range.find(':');
	XlsxArrayFormula array;
	array.is_data_table = formulaKind == "dataTable";
	bool isRange = false;
	if (colon != std::string::npos) {
		isRange = parse_cell_position(range.substr(0, colon), array.first_row, array.first_column) == true
			&& parse_cell_position(range.substr(colon + 1), array.last_row, array.last_column) == true;
	} else if (array.is_data_table == true) {
		isRange = parse_cell_position(range.empty() == true ? cell_reference(row, column) : range,
			array.first_row, array.first_column) == true;
		array.last_row = array.first_row;
		array.last_column = array.first_column;
	}
	if (isRange == false || array.last_row < array.first_row || array.last_column < array.first_column) return std::nullopt;
	if (array.is_data_table == false && array.last_row == array.first_row && array.last_column == array.first_column)
		return std::nullopt;
	return array;

}

SheetRenderCell read_cell(const pugi::xml_node& node, std::size_t row, std::size_t column, const SheetContext& context,
		std::unordered_map<std::string, SharedFormula>& sharedFormulas) {

	SheetRenderCell cell;
	cell.column = column;
	cell.ref = cell_reference(row, column);

	const std::vector<ResolvedStyle>& styles = context.stylesheet->cell_styles;
	const std::optional<unsigned int> styleIndex = attribute_index(node, "s");
	const ResolvedStyle& resolved = styleIndex.has_value() == true && *styleIndex < styles.size()
		? styles[*styleIndex] : context.stylesheet->default_style;
	cell.number_format = resolved.number_format;
	cell.style = resolved.style;

	/* - - - - - - */

	cell.formula = read_formula(node, row, column, sharedFormulas);

	/* - - - - - - */

	const std::string type = attribute_value(node, "t");
	const pugi::xml_node valueNode = child_element(node, "v");
	const std::string value = valueNode ? std::string(valueNode.child_value()) : std::string();
	// A formula whose result is text keeps it even when that text is empty -
	// =IF(C2="","",C2*2) saved as <v></v>: a blank Excel worked out, never a
	// value the file lacks.
	const bool hasValue = valueNode && (value.empty() == false || (type == "str" && cell.formula.has_value() == true));

	if (type == "inlineStr") {
		cell.kind = SheetCellKind::Text;
		const pugi::xml_node inlineText = child_element(node, "is");
		cell.raw_text = inlineText ? rich_text(inlineText) : decode_escaped_characters(value);
	} else if (hasValue == false) {
		cell.kind = SheetCellKind::Empty;
	} else if (type == "s") {
		const std::vector<std::string>& strings = *context.shared_strings;
		double index = -1.0;
		if (parse_classic_double(value, index) == true && index >= 0.0 && index < static_cast<double>(strings.size())) {
			cell.kind = SheetCellKind::Text;
			cell.raw_text = strings[static_cast<std::size_t>(index)];
		}
	} else if (type == "str") {
		cell.kind = SheetCellKind::Text;
		cell.raw_text = decode_escaped_characters(value);
	} else if (type == "b") {
		cell.kind = SheetCellKind::Boolean;
		cell.boolean = trim_ascii(value) == "1" || trim_ascii(value) == "true";
	} else if (type == "e") {
		cell.kind = SheetCellKind::Error;
		cell.raw_text = trim_ascii(value);
	} else if (type == "d") {
		double serial = 0.0;
		if (iso_date_serial(trim_ascii(value), context.date1904, serial) == true) {
			cell.kind = SheetCellKind::Date;
			cell.number = serial;
		} else {
			cell.kind = SheetCellKind::Text;
			cell.raw_text = value;
		}
	} else {
		double number = 0.0;
		if (parse_classic_double(value, number) == true) {
			cell.kind = SheetCellKind::Number;
			cell.number = number;
			cell.stored_value = trim_ascii(value);
		} else {
			cell.kind = SheetCellKind::Text;
			cell.raw_text = value;
		}
	}

	/* - - - - - - */

	FormattedCellValue formatted;
	switch (cell.kind) {
		case SheetCellKind::Number:
			formatted = format_cell_value(cell.number, cell.number_format, context.date1904);
			if (formatted.is_date == true) cell.kind = SheetCellKind::Date;
			break;
		case SheetCellKind::Date: {
			// A cell stored as a date is a date even where its format is not:
			// it is drawn as Excel's short date (and time, when it has one).
			if (is_date_number_format(cell.number_format) == false) {
				const bool hasTime = cell.number != std::floor(cell.number);
				cell.number_format = builtin_number_format(hasTime == true ? 22 : 14);
			}
			formatted = format_cell_value(cell.number, cell.number_format, context.date1904);
			break;
		}
		case SheetCellKind::Text:
			formatted = format_cell_text(cell.raw_text, cell.number_format);
			break;
		case SheetCellKind::Boolean:
			formatted.text = cell.boolean == true ? "TRUE" : "FALSE";
			break;
		case SheetCellKind::Error:
			formatted.text = cell.raw_text;
			break;
		case SheetCellKind::Empty:
			break;
	}
	cell.text = formatted.text;
	cell.color_hex = formatted.color_hex;
	if (formatted.has_fill == true) cell.fill = SheetCellFill{formatted.fill_before, formatted.fill_after};
	return cell;

}

// Excel's column width in characters, as its Column Width dialog shows it,
// from the OOXML width (which counts the cell's 5 px of padding): the default
// font's digit is 7 px wide, so px = Truncate(((256 * width + 18) / 256) * 7)
// and characters = (px - 5) / 7 to two places (px / 12 under one character).
double width_in_characters(double width) {
	if (width <= 0.0) return 0.0;
	const double pixels = std::floor(((256.0 * width + 18.0) / 256.0) * 7.0);
	const double characters = pixels < 12.0 ? pixels / 12.0 : (pixels - 5.0) / 7.0;
	return std::round(characters * 100.0) / 100.0;
}

// The default width from baseColWidth (characters without padding): Excel adds
// the padding and rounds the column up to a multiple of 8 px - 8 characters
// make Excel's standard 64 px, 8.43-character column.
double base_width_in_characters(double baseWidth) {
	const double pixels = std::ceil((baseWidth * 7.0 + 5.0) / 8.0) * 8.0;
	return std::round((pixels - 5.0) / 7.0 * 100.0) / 100.0;
}

struct SheetOutcome {
	SheetRender sheet;
	// Where the sheet stops being shown, when it does: the last whole row.
	bool is_shortened = false;
	std::size_t last_row_shown = 0;
	// Its array formulas over more than one cell, the sheet not yet named.
	std::vector<XlsxArrayFormula> array_formulas;
};

void read_merges(const pugi::xml_node& mergeCells, SheetRender& sheet) {
	for (const pugi::xml_node& merge : mergeCells.children()) {
		if (is_element(merge, "mergeCell") == false) continue;
		const std::string reference = attribute_value(merge, "ref");
		const std::size_t colon = reference.find(':');
		if (colon == std::string::npos) continue;
		SheetRenderMerge range;
		if (parse_cell_position(reference.substr(0, colon), range.first_row, range.first_column) == false) continue;
		if (parse_cell_position(reference.substr(colon + 1), range.last_row, range.last_column) == false) continue;
		if (range.last_row < range.first_row) std::swap(range.first_row, range.last_row);
		if (range.last_column < range.first_column) std::swap(range.first_column, range.last_column);
		sheet.merges.push_back(range);
	}
}

// `isPartCut`: the part was cut at the read limit, so it ends before the sheet does.
SheetOutcome read_worksheet(const pugi::xml_document& document, const SheetContext& context, bool isPartCut) {

	SheetOutcome outcome;
	SheetRender& sheet = outcome.sheet;
	const pugi::xml_node worksheet = root_element(document, "worksheet");
	std::string dimension;
	std::unordered_map<std::string, SharedFormula> sharedFormulas;
	std::size_t cellCount = 0;

	for (const pugi::xml_node& part : worksheet.children()) {
		if (is_element(part, "dimension") == true) {
			dimension = attribute_value(part, "ref");
		} else if (is_element(part, "sheetViews") == true) {
			const pugi::xml_node view = child_element(part, "sheetView");
			sheet.show_grid_lines = attribute_flag(view, "showGridLines", true);
			const pugi::xml_node pane = child_element(view, "pane");
			const std::string state = attribute_value(pane, "state");
			if (pane && (state == "frozen" || state == "frozenSplit")) {
				sheet.frozen_columns = static_cast<std::size_t>(std::max(0.0, attribute_double(pane, "xSplit").value_or(0.0)));
				sheet.frozen_rows = static_cast<std::size_t>(std::max(0.0, attribute_double(pane, "ySplit").value_or(0.0)));
			}
		} else if (is_element(part, "sheetFormatPr") == true) {
			const std::optional<double> defaultWidth = attribute_double(part, "defaultColWidth");
			const std::optional<double> baseWidth = attribute_double(part, "baseColWidth");
			if (defaultWidth.has_value() == true) sheet.default_column_width = width_in_characters(*defaultWidth);
			else if (baseWidth.has_value() == true) sheet.default_column_width = base_width_in_characters(*baseWidth);
			const std::optional<double> defaultHeight = attribute_double(part, "defaultRowHeight");
			if (defaultHeight.has_value() == true && *defaultHeight > 0.0) sheet.default_row_height = *defaultHeight;
		} else if (is_element(part, "cols") == true) {
			for (const pugi::xml_node& col : part.children()) {
				if (is_element(col, "col") == false) continue;
				const std::optional<unsigned int> first = attribute_index(col, "min");
				const std::optional<unsigned int> last = attribute_index(col, "max");
				if (first.has_value() == false || *first < 1 || *first > kMaximumColumns) continue;
				SheetRenderColumn column;
				column.first = *first - 1;
				column.last = std::min<std::size_t>(std::max(*first, last.value_or(*first)), kMaximumColumns) - 1;
				const std::optional<double> width = attribute_double(col, "width");
				if (width.has_value() == true) column.width_characters = width_in_characters(*width);
				column.hidden = attribute_flag(col, "hidden", false);
				sheet.columns.push_back(column);
			}
		} else if (is_element(part, "mergeCells") == true) {
			read_merges(part, sheet);
		} else if (is_element(part, "sheetData") == true) {
			std::size_t runningRow = 0; // 1-based; 0 before the first row
			for (const pugi::xml_node& rowNode : part.children()) {
				if (is_element(rowNode, "row") == false) continue;
				if (outcome.is_shortened == true) break;

				// r is optional in OOXML: a row without it follows the one before,
				// unless its first cell says where it is.
				std::size_t rowNumber = runningRow + 1;
				const std::optional<unsigned int> declaredRow = attribute_index(rowNode, "r");
				if (declaredRow.has_value() == true) {
					rowNumber = *declaredRow;
				} else {
					const pugi::xml_node firstCell = child_element(rowNode, "c");
					std::size_t cellRow = 0;
					std::size_t cellColumn = 0;
					if (firstCell && parse_cell_position(attribute_value(firstCell, "r"), cellRow, cellColumn) == true) {
						rowNumber = cellRow + 1;
					}
				}
				if (rowNumber < 1 || rowNumber > kMaximumRows) continue;
				runningRow = rowNumber;

				SheetRenderRow row;
				row.index = rowNumber - 1;
				row.height_points = attribute_double(rowNode, "ht");
				row.hidden = attribute_flag(rowNode, "hidden", false);

				std::size_t runningColumn = 0; // 1-based; 0 before the first cell
				for (const pugi::xml_node& cellNode : rowNode.children()) {
					if (is_element(cellNode, "c") == false) continue;
					if (cellCount >= kMaximumCellsPerSheet) {
						outcome.is_shortened = true;
						break;
					}
					std::size_t cellRow = 0;
					std::size_t cellColumn = runningColumn;
					if (parse_cell_position(attribute_value(cellNode, "r"), cellRow, cellColumn) == false) {
						cellColumn = runningColumn; // r is optional too: the next column
					}
					if (cellColumn >= kMaximumColumns) continue;
					runningColumn = cellColumn + 1;
					row.cells.push_back(read_cell(cellNode, row.index, cellColumn, context, sharedFormulas));
					++cellCount;
					const std::optional<XlsxArrayFormula> array = array_formula_at(cellNode, row.index, cellColumn);
					if (array.has_value() == true) outcome.array_formulas.push_back(*array);
				}
				// A row cut off part-way is not shown half: it goes with the rest.
				if (outcome.is_shortened == true) break;
				sheet.rows.push_back(std::move(row));
			}
		}
	}

	/* - - - - - - */

	// Rows in order, one entry per row, cells in column order.
	std::stable_sort(sheet.rows.begin(), sheet.rows.end(),
		[](const SheetRenderRow& left, const SheetRenderRow& right) { return left.index < right.index; });
	std::vector<SheetRenderRow> rows;
	for (SheetRenderRow& row : sheet.rows) {
		if (rows.empty() == false && rows.back().index == row.index) {
			for (SheetRenderCell& cell : row.cells) rows.back().cells.push_back(std::move(cell));
			continue;
		}
		rows.push_back(std::move(row));
	}
	for (SheetRenderRow& row : rows) {
		std::stable_sort(row.cells.begin(), row.cells.end(),
			[](const SheetRenderCell& left, const SheetRenderCell& right) { return left.column < right.column; });
		row.cells.erase(std::unique(row.cells.begin(), row.cells.end(),
			[](const SheetRenderCell& left, const SheetRenderCell& right) { return left.column == right.column; }),
			row.cells.end());
	}

	// The used extent: every cell the sheet has, every merged range, and the
	// sheet's own dimension (it also covers formatted cells a writer left out)
	// when it is a real range and not a whole-sheet claim.
	for (const SheetRenderRow& row : rows) {
		if (row.cells.empty() == true) continue;
		sheet.row_count = std::max(sheet.row_count, row.index + 1);
		sheet.column_count = std::max(sheet.column_count, row.cells.back().column + 1);
	}
	for (const SheetRenderMerge& merge : sheet.merges) {
		sheet.row_count = std::max(sheet.row_count, merge.last_row + 1);
		sheet.column_count = std::max(sheet.column_count, merge.last_column + 1);
	}
	const std::size_t colon = dimension.find(':');
	std::size_t dimensionRow = 0;
	std::size_t dimensionColumn = 0;
	const bool isPartial = outcome.is_shortened == true || isPartCut == true;
	if (sheet.row_count > 0 && isPartial == false && colon != std::string::npos
			&& parse_cell_position(dimension.substr(colon + 1), dimensionRow, dimensionColumn) == true
			&& (dimensionRow + 1) * (dimensionColumn + 1) <= kMaximumCellsPerSheet) {
		sheet.row_count = std::max(sheet.row_count, dimensionRow + 1);
		sheet.column_count = std::max(sheet.column_count, dimensionColumn + 1);
	}

	// A row past the extent with nothing in it (a stored height below the last
	// used row) draws nothing.
	sheet.rows.clear();
	for (SheetRenderRow& row : rows) {
		if (row.index < sheet.row_count || row.cells.empty() == false) sheet.rows.push_back(std::move(row));
	}

	outcome.last_row_shown = sheet.rows.empty() == true ? 0 : sheet.rows.back().index + 1;

	// A sheet shown only part of the way ends where its shown rows end: no merge
	// reaches past them and no blank rows stand in for the ones not shown.
	if (isPartial == true) {
		std::vector<SheetRenderMerge> merges;
		for (SheetRenderMerge merge : sheet.merges) {
			if (merge.first_row >= outcome.last_row_shown) continue;
			merge.last_row = std::min(merge.last_row, outcome.last_row_shown - 1);
			merges.push_back(merge);
		}
		sheet.merges = std::move(merges);
		sheet.row_count = std::min(sheet.row_count, outcome.last_row_shown);
	}
	return outcome;

}

std::string quoted_name(const std::string& name) {
	return "\xE2\x80\x9C" + name + "\xE2\x80\x9D";
}

void add_limitation(std::string& limitation, const std::string& sentence) {
	if (sentence.empty() == true) return;
	if (limitation.empty() == false) limitation += " ";
	limitation += sentence;
}

} // namespace

XlsxWorkbookViewResult read_xlsx_workbook_view(const DocumentExtractionRequest& request) {

	XlsxWorkbookViewResult result;
	const std::string unreadable = "This workbook could not be read.";

	std::error_code filesystemError;
	if (std::filesystem::is_regular_file(request.path, filesystemError) == false || filesystemError) {
		result.diagnostics.push_back({"unreadable_file", "The XLSX document is missing or is not a regular file."});
		result.limitation = unreadable;
		return result;
	}
	const std::uintmax_t archiveSize = std::filesystem::file_size(request.path, filesystemError);
	if (filesystemError || archiveSize > request.maximum_input_bytes) {
		result.diagnostics.push_back({"input_too_large", "The XLSX document exceeds the extraction limit."});
		result.limitation = "This workbook is too large to show here. Open it in Excel.";
		return result;
	}

	try {
		ZipArchive archive(request.path);
		const zip_int64_t entryCount = zip_get_num_entries(archive.get(), 0);
		if (entryCount < 0 || entryCount > kMaximumArchiveEntries) {
			throw WorkbookViewError("xlsx_archive_entry_limit", "The XLSX package has too many parts to read safely.");
		}

		/* - - - - - - */

		// The workbook part, found the way the package names it.
		const std::unordered_map<std::string, Relationship> packageRelationships = read_relationships(archive.get(), "");
		std::string workbookPart = find_relationship_target(packageRelationships, "/officeDocument");
		PackagePart workbookBytes = read_part(archive.get(), workbookPart);
		if (workbookBytes.is_found == false) {
			workbookPart = "xl/workbook.xml";
			workbookBytes = read_part(archive.get(), workbookPart);
		}
		if (workbookBytes.is_found == false || workbookBytes.is_cut == true) {
			throw WorkbookViewError("xlsx_missing_workbook_xml", "The XLSX package does not contain a readable workbook part.");
		}
		const pugi::xml_document workbookDocument =
			parse_part(workbookBytes.bytes, "xlsx_malformed_workbook_xml", "The XLSX workbook part is malformed.");
		const pugi::xml_node workbook = root_element(workbookDocument, "workbook");
		const std::unordered_map<std::string, Relationship> relationships = read_relationships(archive.get(), workbookPart);

		result.workbook.date1904 = attribute_flag(child_element(workbook, "workbookPr"), "date1904", false);
		const pugi::xml_node view = child_element(child_element(workbook, "bookViews"), "workbookView");
		const std::size_t activeTab = attribute_index(view, "activeTab").value_or(0);

		/* - - - - - - */

		// Colours, formats and text shared by every sheet. A part that cannot be
		// read costs only what it carries.
		ColorContext colors;
		colors.theme = default_theme_colors();
		for (unsigned int index = 0; index < 64; ++index) colors.indexed.push_back(excel_indexed_color(index));
		try {
			colors.theme = read_theme_colors(archive.get(), find_relationship_target(relationships, "/theme"));
		} catch (const WorkbookViewError& error) {
			result.diagnostics.push_back({error.code, error.what()});
		}

		Stylesheet stylesheet;
		stylesheet.cell_styles.push_back(stylesheet.default_style);
		try {
			std::string stylesLimitation;
			stylesheet = read_stylesheet(archive.get(), find_relationship_target(relationships, "/styles"), colors,
				stylesLimitation);
			add_limitation(result.limitation, stylesLimitation);
		} catch (const WorkbookViewError& error) {
			result.diagnostics.push_back({error.code, error.what()});
			add_limitation(result.limitation, "This workbook's formatting could not be read, so its values are shown unformatted.");
		}

		std::vector<std::string> sharedStrings;
		try {
			std::string stringsLimitation;
			std::string stringsPart = find_relationship_target(relationships, "/sharedStrings");
			if (stringsPart.empty() == true) stringsPart = part_directory(workbookPart) + "sharedStrings.xml";
			sharedStrings = read_shared_strings(archive.get(), stringsPart, stringsLimitation);
			add_limitation(result.limitation, stringsLimitation);
		} catch (const WorkbookViewError& error) {
			result.diagnostics.push_back({error.code, error.what()});
			add_limitation(result.limitation, "Some of this workbook's text could not be read, so those cells are shown empty.");
		}

		SheetContext context;
		context.shared_strings = &sharedStrings;
		context.stylesheet = &stylesheet;
		context.date1904 = result.workbook.date1904;

		/* - - - - - - */

		for (const pugi::xml_node& entry : child_element(workbook, "sheets").children()) {
			if (is_element(entry, "sheet") == false) continue;
			const std::string name = attribute_value(entry, "name");
			const std::string state = attribute_value(entry, "state");
			const SheetVisibility visibility = state == "hidden" ? SheetVisibility::Hidden
				: state == "veryHidden" ? SheetVisibility::VeryHidden : SheetVisibility::Visible;

			SheetRender empty;
			empty.name = name;
			empty.visibility = visibility;
			const auto relationship = relationships.find(attribute_value(entry, "id"));
			if (relationship == relationships.end()) {
				result.diagnostics.push_back({"xlsx_unresolved_sheet_relationship",
					"An XLSX sheet relationship could not be resolved."});
				add_limitation(result.limitation, quoted_name(name) + " could not be read, so it is shown empty.");
				result.workbook.sheets.push_back(empty);
				continue;
			}
			// A chart sheet (or an old dialog or macro sheet) keeps its tab, with
			// no cells: what it holds is not a grid.
			if (type_ends_with(relationship->second, "/worksheet") == false) {
				if (type_ends_with(relationship->second, "/chartsheet") == true) {
					add_limitation(result.limitation, quoted_name(name) + " holds a chart, which is not shown here.");
				}
				result.workbook.sheets.push_back(empty);
				continue;
			}

			try {
				PackagePart part = read_part(archive.get(), relationship->second.target);
				if (part.is_found == false) {
					throw WorkbookViewError("xlsx_missing_worksheet", "An XLSX worksheet part is missing.");
				}
				// Past the read limit, the sheet is shown as far as its rows are
				// whole; with not one whole row in reach, it is not shown at all.
				if (part.is_cut == true && keep_whole_elements(part.bytes, "row", {"sheetData", "worksheet"}) == false) {
					result.diagnostics.push_back({"xlsx_xml_part_size_limit", "An XLSX worksheet part exceeds the read limit."});
					add_limitation(result.limitation, quoted_name(name)
						+ " is too large to show here. Open the workbook in Excel to see it.");
					result.workbook.sheets.push_back(empty);
					continue;
				}
				const pugi::xml_document sheetDocument =
					parse_part(part.bytes, "xlsx_malformed_worksheet_xml", "An XLSX worksheet part is malformed.");
				SheetOutcome outcome = read_worksheet(sheetDocument, context, part.is_cut);
				outcome.sheet.name = name;
				for (XlsxArrayFormula& array : outcome.array_formulas) {
					array.sheet = name;
					result.array_formulas.push_back(array);
				}
				outcome.sheet.visibility = visibility;
				if (outcome.is_shortened == true || part.is_cut == true) {
					result.diagnostics.push_back({outcome.is_shortened == true ? "xlsx_cell_count_limit" : "xlsx_xml_part_size_limit",
						"A worksheet was shown only up to the safety limit."});
					add_limitation(result.limitation, quoted_name(name) + " is shown up to row "
						+ std::to_string(outcome.last_row_shown)
						+ ": the rest is too large to show here. Open the workbook in Excel to see all of it.");
				}
				result.workbook.sheets.push_back(std::move(outcome.sheet));
			} catch (const WorkbookViewError& error) {
				result.diagnostics.push_back({error.code, error.what()});
				add_limitation(result.limitation, quoted_name(name) + " could not be read, so it is shown empty.");
				result.workbook.sheets.push_back(empty);
			}
		}
		if (result.workbook.sheets.empty() == true) {
			throw WorkbookViewError("xlsx_no_sheets", "No worksheets could be read from the XLSX package.");
		}
		result.workbook.active_sheet = activeTab < result.workbook.sheets.size() ? activeTab : 0;
	} catch (const WorkbookViewError& error) {
		result.status = ExtractionStatus::unreadable;
		result.workbook = SheetWorkbookRender();
		result.array_formulas.clear();
		result.diagnostics.push_back({error.code, error.what()});
		result.limitation = unreadable;
		return result;
	} catch (const std::exception&) {
		result.status = ExtractionStatus::unreadable;
		result.workbook = SheetWorkbookRender();
		result.array_formulas.clear();
		result.diagnostics.push_back({"xlsx_view_failed", "The XLSX document could not be read safely."});
		result.limitation = unreadable;
		return result;
	}

	bool hasCells = false;
	for (const SheetRender& sheet : result.workbook.sheets) {
		for (const SheetRenderRow& row : sheet.rows) {
			if (row.cells.empty() == false) hasCells = true;
		}
	}
	result.status = hasCells == true ? ExtractionStatus::ready : ExtractionStatus::empty;
	return result;

}


// ---- Formulas after a change --------------------------------------------------
//
// When Singularity writes a cell, every formula that reads it - directly or
// through another formula - still carries the value Excel saved before. Those
// formulas are found here, and the ones made only of numbers, cell references,
// + - * / ^ %, brackets and SUM are calculated again, as Excel calculates them;
// every other one is named as one whose saved value no longer holds, so its
// writer can leave it without one (Excel calculates it when the workbook is
// opened). A formula whose references cannot all be read - a defined name,
// INDIRECT, OFFSET, another workbook, a table's columns - may read the change,
// so it is counted among them.

namespace {

// A rectangle of one sheet, 0-based and inclusive; a whole column or row runs
// to the end of the grid.
struct FormulaArea {
	std::size_t sheet = 0;
	std::size_t first_row = 0;
	std::size_t first_column = 0;
	std::size_t last_row = 0;
	std::size_t last_column = 0;
};

enum class FormulaTokenKind { Number, Text, Area, Function, Operator, Open, Close, Comma, Unreadable };

struct FormulaToken {
	FormulaTokenKind kind = FormulaTokenKind::Unreadable;
	std::string text;
	double number = 0.0;
	FormulaArea area;
	// A single cell rather than a range.
	bool is_cell = false;
};

std::string upper_ascii(std::string text) {
	for (char& character : text) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	return text;
}

// A formula's tokens, each reference resolved to its sheet (`own` where the
// reference names none). A reference, name or construct that cannot be
// resolved is an Unreadable token: whatever it reads is unknown.
std::vector<FormulaToken> formula_tokens(const std::string& formula, std::size_t own,
		const std::unordered_map<std::string, std::size_t>& sheetsByName) {

	std::vector<FormulaToken> tokens;
	std::size_t index = formula.empty() == false && formula.front() == '=' ? 1 : 0;
	const std::size_t size = formula.size();
	const auto unreadable = [&](const std::string& text) {
		FormulaToken token;
		token.kind = FormulaTokenKind::Unreadable;
		token.text = text;
		tokens.push_back(token);
	};
	// "B4", "$B$4:$C$9", "B:B", "4:4" at `at`, on `sheet`.
	const auto readArea = [&](std::size_t at, std::size_t sheet, FormulaToken& token) -> std::size_t {
		std::size_t end = at;
		while (end < size && is_name_character(formula[end]) == true) ++end;
		ReferencePart first;
		if (end == at || parse_reference_part(formula.substr(at, end - at), first) == false) return 0;
		ReferencePart last = first;
		std::size_t next = end;
		if (end < size && formula[end] == ':') {
			std::size_t secondEnd = end + 1;
			while (secondEnd < size && is_name_character(formula[secondEnd]) == true) ++secondEnd;
			ReferencePart second;
			if (parse_reference_part(formula.substr(end + 1, secondEnd - end - 1), second) == true
					&& second.has_column == first.has_column && second.has_row == first.has_row) {
				last = second;
				next = secondEnd;
			}
		}
		if (next == end && (first.has_column == false || first.has_row == false)) return 0;
		if (next < size && (formula[next] == '(' || formula[next] == '!' || formula[next] == '[')) return 0;
		token.kind = FormulaTokenKind::Area;
		token.is_cell = next == end;
		token.area.sheet = sheet;
		token.area.first_column = first.has_column == true ? static_cast<std::size_t>(first.column - 1) : 0;
		token.area.last_column = last.has_column == true ? static_cast<std::size_t>(last.column - 1) : kMaximumColumns - 1;
		token.area.first_row = first.has_row == true ? static_cast<std::size_t>(first.row - 1) : 0;
		token.area.last_row = last.has_row == true ? static_cast<std::size_t>(last.row - 1) : kMaximumRows - 1;
		if (token.area.last_column < token.area.first_column) std::swap(token.area.first_column, token.area.last_column);
		if (token.area.last_row < token.area.first_row) std::swap(token.area.first_row, token.area.last_row);
		return next;
	};

	while (index < size) {
		const char character = formula[index];
		if (character == ' ' || character == '\n' || character == '\r' || character == '\t') {
			++index;
			continue;
		}
		if (character == '"') {
			std::size_t end = index + 1;
			std::string text;
			while (end < size) {
				if (formula[end] == '"') {
					if (end + 1 < size && formula[end + 1] == '"') {
						text.push_back('"');
						end += 2;
						continue;
					}
					break;
				}
				text.push_back(formula[end++]);
			}
			FormulaToken token;
			token.kind = FormulaTokenKind::Text;
			token.text = text;
			tokens.push_back(token);
			index = std::min(end + 1, size);
			continue;
		}
		if (character == '\'') {
			// 'Deal Terms'!B4
			std::size_t end = index + 1;
			std::string name;
			while (end < size) {
				if (formula[end] == '\'') {
					if (end + 1 < size && formula[end + 1] == '\'') {
						name.push_back('\'');
						end += 2;
						continue;
					}
					break;
				}
				name.push_back(formula[end++]);
			}
			const auto sheet = sheetsByName.find(upper_ascii(name));
			FormulaToken token;
			const std::size_t areaEnd = end + 1 < size && formula[end + 1] == '!' && sheet != sheetsByName.end()
				? readArea(end + 2, sheet->second, token) : 0;
			if (areaEnd == 0) {
				unreadable(name);
				return tokens;
			}
			tokens.push_back(token);
			index = areaEnd;
			continue;
		}
		if (character == '#') {
			// An error written into the formula (#REF!): it reads nothing.
			std::size_t end = index + 1;
			while (end < size && (std::isalnum(static_cast<unsigned char>(formula[end])) != 0 || formula[end] == '/'
					|| formula[end] == '!' || formula[end] == '?' || formula[end] == '_')) {
				++end;
			}
			unreadable(formula.substr(index, end - index));
			index = end;
			continue;
		}
		if (character == '[' || character == '{' || character == '@') {
			unreadable(std::string(1, character));
			return tokens;
		}
		if (character == '(' || character == ')' || character == ',') {
			FormulaToken token;
			token.kind = character == '(' ? FormulaTokenKind::Open : character == ')' ? FormulaTokenKind::Close : FormulaTokenKind::Comma;
			tokens.push_back(token);
			++index;
			continue;
		}
		if (is_name_character(character) == false) {
			FormulaToken token;
			token.kind = FormulaTokenKind::Operator;
			token.text = std::string(1, character);
			// "<=", ">=", "<>".
			if ((character == '<' || character == '>') && index + 1 < size && (formula[index + 1] == '=' || formula[index + 1] == '>')) {
				token.text.push_back(formula[++index]);
			}
			tokens.push_back(token);
			++index;
			continue;
		}

		std::size_t end = index;
		while (end < size && is_name_character(formula[end]) == true) ++end;
		const std::string word = formula.substr(index, end - index);
		const char next = end < size ? formula[end] : '\0';
		if (next == '(') {
			FormulaToken token;
			token.kind = FormulaTokenKind::Function;
			token.text = upper_ascii(word);
			// A function that decides at run time what it reads - and a data
			// table, which reads whatever its neighbouring formulas read.
			if (token.text == "INDIRECT" || token.text == "OFFSET" || token.text == "_XLFN.LET" || token.text == "LET"
					|| token.text == "TABLE") {
				unreadable(token.text);
				return tokens;
			}
			tokens.push_back(token);
			index = end;
			continue;
		}
		if (next == '!') {
			const auto sheet = sheetsByName.find(upper_ascii(word));
			FormulaToken token;
			const std::size_t areaEnd = sheet != sheetsByName.end() ? readArea(end + 1, sheet->second, token) : 0;
			if (areaEnd == 0) {
				unreadable(word);
				return tokens;
			}
			tokens.push_back(token);
			index = areaEnd;
			continue;
		}
		FormulaToken area;
		const std::size_t areaEnd = readArea(index, own, area);
		if (areaEnd != 0) {
			tokens.push_back(area);
			index = areaEnd;
			continue;
		}
		if (std::isdigit(static_cast<unsigned char>(word[0])) != 0 || word[0] == '.') {
			std::string number = word;
			index = end;
			const char last = number.back();
			if ((last == 'E' || last == 'e') && index < size && (formula[index] == '+' || formula[index] == '-')) {
				number.push_back(formula[index++]);
				while (index < size && std::isdigit(static_cast<unsigned char>(formula[index])) != 0) number.push_back(formula[index++]);
			}
			FormulaToken token;
			token.kind = FormulaTokenKind::Number;
			token.text = number;
			if (parse_classic_double(number, token.number) == false) {
				unreadable(number);
				return tokens;
			}
			tokens.push_back(token);
			continue;
		}
		// TRUE and FALSE read nothing; any other word is a defined name, and what
		// it names is unknown here.
		const std::string upper = upper_ascii(word);
		if (upper != "TRUE" && upper != "FALSE") {
			unreadable(word);
			return tokens;
		}
		FormulaToken token;
		token.kind = FormulaTokenKind::Unreadable;
		token.text = upper;
		tokens.push_back(token);
		index = end;
	}
	return tokens;

}

// Whether every reference of the tokens is known: no name, no other workbook,
// no reference decided at run time.
bool references_known(const std::vector<FormulaToken>& tokens) {
	for (const FormulaToken& token : tokens) {
		if (token.kind != FormulaTokenKind::Unreadable) continue;
		if (token.text == "TRUE" || token.text == "FALSE" || (token.text.empty() == false && token.text[0] == '#')) continue;
		return false;
	}
	return true;
}

// A cell as a calculation reads it.
struct FormulaValue {
	enum class Kind { Number, Empty, Text, Other } kind = Kind::Empty;
	double number = 0.0;
};

enum class Calculation { Done, Pending, Unsupported };

// Calculates the SUM/arithmetic subset of Excel's grammar over the tokens:
// numbers, cells, + - * / ^, unary signs, %, brackets and SUM of cells, ranges
// and expressions. `lookup` gives a cell's value, or says it is still being
// calculated (Pending) or cannot be known (Unsupported).
class FormulaCalculator {
public:
	using Lookup = std::function<Calculation(std::size_t sheet, std::size_t row, std::size_t column, FormulaValue& value)>;
	using Cells = std::function<Calculation(const FormulaArea& area, double& sum)>;

	FormulaCalculator(const std::vector<FormulaToken>& tokens, Lookup lookup, Cells sumOf)
		: tokens_(tokens), lookup_(std::move(lookup)), sumOf_(std::move(sumOf)) {}

	Calculation calculate(double& result) {
		position_ = 0;
		const Calculation outcome = additive(result);
		if (outcome != Calculation::Done) return outcome;
		if (position_ != tokens_.size() || std::isfinite(result) == false) return Calculation::Unsupported;
		return Calculation::Done;
	}

private:
	bool is_operator(const char* text) const {
		return position_ < tokens_.size() && tokens_[position_].kind == FormulaTokenKind::Operator && tokens_[position_].text == text;
	}

	Calculation additive(double& result) {
		Calculation outcome = multiplicative(result);
		while (outcome == Calculation::Done && (is_operator("+") == true || is_operator("-") == true)) {
			const bool isAdding = tokens_[position_++].text == "+";
			double right = 0.0;
			outcome = multiplicative(right);
			result = isAdding == true ? result + right : result - right;
		}
		return outcome;
	}

	Calculation multiplicative(double& result) {
		Calculation outcome = power(result);
		while (outcome == Calculation::Done && (is_operator("*") == true || is_operator("/") == true)) {
			const bool isMultiplying = tokens_[position_++].text == "*";
			double right = 0.0;
			outcome = power(right);
			if (outcome != Calculation::Done) break;
			// Excel shows #DIV/0!: nothing to write.
			if (isMultiplying == false && right == 0.0) return Calculation::Unsupported;
			result = isMultiplying == true ? result * right : result / right;
		}
		return outcome;
	}

	Calculation power(double& result) {
		Calculation outcome = unary(result);
		while (outcome == Calculation::Done && is_operator("^") == true) {
			++position_;
			double exponent = 0.0;
			outcome = unary(exponent);
			if (outcome != Calculation::Done) break;
			// Excel shows #NUM! for 0^0, where std::pow gives 1.
			if (result == 0.0 && exponent == 0.0) return Calculation::Unsupported;
			result = std::pow(result, exponent);
			if (std::isfinite(result) == false) return Calculation::Unsupported;
		}
		return outcome;
	}

	// A sign binds tighter than ^ in Excel: -2^2 is 4.
	Calculation unary(double& result) {
		if (is_operator("-") == true || is_operator("+") == true) {
			const bool isNegative = tokens_[position_++].text == "-";
			const Calculation outcome = unary(result);
			if (isNegative == true) result = -result;
			return outcome;
		}
		Calculation outcome = primary(result);
		while (outcome == Calculation::Done && is_operator("%") == true) {
			++position_;
			result /= 100.0;
		}
		return outcome;
	}

	Calculation primary(double& result) {
		if (position_ >= tokens_.size()) return Calculation::Unsupported;
		const FormulaToken& token = tokens_[position_];
		if (token.kind == FormulaTokenKind::Number) {
			++position_;
			result = token.number;
			return Calculation::Done;
		}
		if (token.kind == FormulaTokenKind::Open) {
			++position_;
			const Calculation outcome = additive(result);
			if (outcome != Calculation::Done) return outcome;
			if (position_ >= tokens_.size() || tokens_[position_].kind != FormulaTokenKind::Close) return Calculation::Unsupported;
			++position_;
			return Calculation::Done;
		}
		if (token.kind == FormulaTokenKind::Area && token.is_cell == true) {
			++position_;
			FormulaValue value;
			const Calculation outcome = lookup_(token.area.sheet, token.area.first_row, token.area.first_column, value);
			if (outcome != Calculation::Done) return outcome;
			// An empty cell is 0 to arithmetic; text or an error is not a number.
			if (value.kind == FormulaValue::Kind::Empty) result = 0.0;
			else if (value.kind == FormulaValue::Kind::Number) result = value.number;
			else return Calculation::Unsupported;
			return Calculation::Done;
		}
		if (token.kind == FormulaTokenKind::Function && token.text == "SUM") return sum(result);
		return Calculation::Unsupported;
	}

	// SUM(...): a cell or range adds the numbers it holds and passes over text
	// and empty cells, as Excel's SUM does; any other argument is a number.
	Calculation sum(double& result) {
		++position_;
		if (position_ >= tokens_.size() || tokens_[position_].kind != FormulaTokenKind::Open) return Calculation::Unsupported;
		++position_;
		result = 0.0;
		if (position_ < tokens_.size() && tokens_[position_].kind == FormulaTokenKind::Close) {
			++position_;
			return Calculation::Done;
		}
		while (true) {
			const FormulaToken& token = tokens_[position_];
			const bool isLoneArea = token.kind == FormulaTokenKind::Area && position_ + 1 < tokens_.size()
				&& (tokens_[position_ + 1].kind == FormulaTokenKind::Comma || tokens_[position_ + 1].kind == FormulaTokenKind::Close);
			double part = 0.0;
			Calculation outcome = Calculation::Done;
			if (isLoneArea == true) {
				outcome = sumOf_(token.area, part);
				++position_;
			} else {
				outcome = additive(part);
			}
			if (outcome != Calculation::Done) return outcome;
			result += part;
			if (position_ >= tokens_.size()) return Calculation::Unsupported;
			if (tokens_[position_].kind == FormulaTokenKind::Close) {
				++position_;
				return Calculation::Done;
			}
			if (tokens_[position_].kind != FormulaTokenKind::Comma) return Calculation::Unsupported;
			++position_;
			if (position_ >= tokens_.size()) return Calculation::Unsupported;
		}
	}

	const std::vector<FormulaToken>& tokens_;
	Lookup lookup_;
	Cells sumOf_;
	std::size_t position_ = 0;
};

// A number as a workbook stores it: the shortest text that reads back as the
// same double, never more than 17 significant digits.
std::string stored_number_text(double value) {
	if (value == 0.0) return "0";
	for (int precision = 15; precision <= 17; ++precision) {
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << std::setprecision(precision) << value;
		double back = 0.0;
		if (parse_classic_double(stream.str(), back) == true && back == value) return stream.str();
	}
	std::ostringstream stream;
	stream.imbue(std::locale::classic());
	stream << std::setprecision(17) << value;
	return stream.str();
}


// A formula by where it stands, with what it reads.
struct FormulaPlace {
	std::size_t sheet = 0;
	std::size_t row = 0;
	std::size_t column = 0;
	std::vector<FormulaToken> tokens;
	bool is_known = true;
};

using CellPlace = std::tuple<std::size_t, std::size_t, std::size_t>;

// Which of `formulas` read one of the `changed` places - directly or through
// another formula - found at about the cost of the formulas that do, not the
// square of the workbook. A formula whose references cannot all be read may
// read the change, so it is among them; so is a formula standing in a changed
// place. An array formula or a data table (`arrayAt`, by its first cell) that
// is reached reaches every cell of its range with it: its range is added to
// `reachedRanges`.
std::vector<bool> formulas_reached(const std::vector<FormulaPlace>& formulas, std::size_t sheetCount,
		const std::map<CellPlace, FormulaArea>& arrayAt, std::vector<CellPlace> pending,
		std::vector<FormulaArea>& reachedRanges) {

	const auto isInside = [](const FormulaArea& area, std::size_t sheet, std::size_t row, std::size_t column) {
		return area.sheet == sheet && row >= area.first_row && row <= area.last_row && column >= area.first_column
			&& column <= area.last_column;
	};

	// Who reads a place: each formula by the single cells it names, and by the
	// ranges, whole columns and whole rows it names - a narrow range under each
	// of its columns, a wide one under its sheet - so a place finds its readers
	// without asking every formula of the workbook.
	const auto placeKey = [](std::size_t sheet, std::size_t row, std::size_t column) {
		return (static_cast<std::uint64_t>(sheet) << 34) | (static_cast<std::uint64_t>(row) << 14)
			| static_cast<std::uint64_t>(column);
	};
	constexpr std::size_t kNarrowColumns = 16;
	using AreaReader = std::pair<FormulaArea, std::size_t>;
	std::unordered_map<std::uint64_t, std::vector<std::size_t>> cellReaders;
	std::vector<std::unordered_map<std::size_t, std::vector<AreaReader>>> columnReaders(sheetCount);
	std::vector<std::vector<AreaReader>> wideReaders(sheetCount);
	for (std::size_t index = 0; index < formulas.size(); ++index) {
		for (const FormulaToken& token : formulas[index].tokens) {
			if (token.kind != FormulaTokenKind::Area || token.area.sheet >= sheetCount) continue;
			const FormulaArea& area = token.area;
			if (token.is_cell == true) {
				cellReaders[placeKey(area.sheet, area.first_row, area.first_column)].push_back(index);
			} else if (area.last_column - area.first_column < kNarrowColumns) {
				for (std::size_t column = area.first_column; column <= area.last_column; ++column)
					columnReaders[area.sheet][column].emplace_back(area, index);
			} else {
				wideReaders[area.sheet].emplace_back(area, index);
			}
		}
	}

	// The changed cells, then every formula that reads one of them or reads
	// such a formula, each place looked up once.
	std::vector<bool> isAffected(formulas.size(), false);
	if (pending.empty() == true) return isAffected;
	const auto reach = [&](std::size_t index) {
		if (isAffected[index] == true) return;
		isAffected[index] = true;
		pending.emplace_back(formulas[index].sheet, formulas[index].row, formulas[index].column);
		const auto array = arrayAt.find({formulas[index].sheet, formulas[index].row, formulas[index].column});
		if (array != arrayAt.end()) reachedRanges.push_back(array->second);
	};
	for (std::size_t index = 0; index < formulas.size(); ++index)
		if (formulas[index].is_known == false) reach(index);
	// A formula written by the change is calculated with the ones that read it.
	{
		std::map<CellPlace, std::size_t> formulaAt;
		for (std::size_t index = 0; index < formulas.size(); ++index)
			formulaAt[{formulas[index].sheet, formulas[index].row, formulas[index].column}] = index;
		const std::vector<CellPlace> written = pending;
		for (const CellPlace& place : written) {
			const auto found = formulaAt.find(place);
			if (found != formulaAt.end()) reach(found->second);
		}
	}
	std::size_t rangesLooked = 0;
	while (pending.empty() == false || rangesLooked < reachedRanges.size()) {
		if (pending.empty() == false) {
			const std::size_t sheet = std::get<0>(pending.back());
			const std::size_t row = std::get<1>(pending.back());
			const std::size_t column = std::get<2>(pending.back());
			pending.pop_back();
			if (sheet >= sheetCount) continue;
			const auto readers = cellReaders.find(placeKey(sheet, row, column));
			if (readers != cellReaders.end())
				for (const std::size_t index : readers->second) reach(index);
			const auto narrow = columnReaders[sheet].find(column);
			if (narrow != columnReaders[sheet].end())
				for (const AreaReader& reader : narrow->second)
					if (row >= reader.first.first_row && row <= reader.first.last_row) reach(reader.second);
			for (const AreaReader& reader : wideReaders[sheet])
				if (isInside(reader.first, sheet, row, column) == true) reach(reader.second);
			continue;
		}
		// A reached range: every formula that reads any cell of it.
		const FormulaArea range = reachedRanges[rangesLooked++];
		for (std::size_t index = 0; index < formulas.size(); ++index) {
			if (isAffected[index] == true) continue;
			for (const FormulaToken& token : formulas[index].tokens) {
				if (token.kind != FormulaTokenKind::Area) continue;
				if (range.sheet == token.area.sheet && range.first_row <= token.area.last_row
						&& token.area.first_row <= range.last_row && range.first_column <= token.area.last_column
						&& token.area.first_column <= range.last_column) {
					reach(index);
					break;
				}
			}
		}
	}
	return isAffected;

}

} // namespace

std::vector<XlsxFormulaResult> formulas_reading_changed_cells(const SheetWorkbookRender& workbook,
		const std::vector<XlsxCellAddress>& changed, const std::vector<XlsxArrayFormula>& arrays) {

	std::unordered_map<std::string, std::size_t> sheetsByName;
	for (std::size_t index = 0; index < workbook.sheets.size(); ++index) sheetsByName[upper_ascii(workbook.sheets[index].name)] = index;

	// Every cell by its place, and every formula with its tokens.
	std::vector<std::map<std::pair<std::size_t, std::size_t>, const SheetRenderCell*>> cells(workbook.sheets.size());
	std::vector<FormulaPlace> formulas;
	std::vector<const SheetRenderCell*> formulaCells;
	for (std::size_t sheet = 0; sheet < workbook.sheets.size(); ++sheet) {
		for (const SheetRenderRow& row : workbook.sheets[sheet].rows) {
			for (const SheetRenderCell& cell : row.cells) {
				cells[sheet][{row.index, cell.column}] = &cell;
				if (cell.formula.has_value() == false) continue;
				FormulaPlace formula;
				formula.sheet = sheet;
				formula.row = row.index;
				formula.column = cell.column;
				formula.tokens = formula_tokens(*cell.formula, sheet, sheetsByName);
				formula.is_known = references_known(formula.tokens);
				formulas.push_back(std::move(formula));
				formulaCells.push_back(&cell);
			}
		}
	}

	// The array formulas by their first cell, each with its whole range.
	std::map<CellPlace, FormulaArea> arrayAt;
	for (const XlsxArrayFormula& array : arrays) {
		const auto sheet = sheetsByName.find(upper_ascii(array.sheet));
		if (sheet == sheetsByName.end()) continue;
		FormulaArea range;
		range.sheet = sheet->second;
		range.first_row = array.first_row;
		range.first_column = array.first_column;
		range.last_row = array.last_row;
		range.last_column = array.last_column;
		arrayAt[{range.sheet, range.first_row, range.first_column}] = range;
	}
	const auto isInside = [](const FormulaArea& area, std::size_t sheet, std::size_t row, std::size_t column) {
		return area.sheet == sheet && row >= area.first_row && row <= area.last_row && column >= area.first_column
			&& column <= area.last_column;
	};

	std::vector<CellPlace> pending;
	for (const XlsxCellAddress& address : changed) {
		const auto sheet = address.sheet.empty() == true && workbook.sheets.empty() == false
			? sheetsByName.find(upper_ascii(workbook.sheets[0].name)) : sheetsByName.find(upper_ascii(address.sheet));
		std::size_t row = 0;
		std::size_t column = 0;
		if (sheet == sheetsByName.end() || parse_cell_position(address.ref, row, column) == false) continue;
		pending.emplace_back(sheet->second, row, column);
	}
	if (pending.empty() == true) return {};
	std::vector<FormulaArea> reachedRanges;
	const std::vector<bool> isAffected = formulas_reached(formulas, workbook.sheets.size(), arrayAt, pending, reachedRanges);

	/* - - - - - - */

	std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::size_t> affectedAt;
	for (std::size_t index = 0; index < formulas.size(); ++index)
		if (isAffected[index] == true) affectedAt[{formulas[index].sheet, formulas[index].row, formulas[index].column}] = index;
	std::vector<Calculation> states(formulas.size(), Calculation::Pending);
	std::vector<double> results(formulas.size(), 0.0);

	const auto lookup = [&](std::size_t sheet, std::size_t row, std::size_t column, FormulaValue& value) {
		// A cell an array formula spills into holds what that formula last
		// calculated: unknown once the formula reads the change, and unknown
		// when the file keeps no value for it.
		for (const auto& array : arrayAt) {
			if (isInside(array.second, sheet, row, column) == false) continue;
			if (row == array.second.first_row && column == array.second.first_column) break;
			for (const FormulaArea& range : reachedRanges)
				if (isInside(range, sheet, row, column) == true) return Calculation::Unsupported;
			if (sheet >= cells.size()) return Calculation::Unsupported;
			const auto spilled = cells[sheet].find({row, column});
			if (spilled == cells[sheet].end() || spilled->second->kind == SheetCellKind::Empty) return Calculation::Unsupported;
			break;
		}
		const auto affected = affectedAt.find({sheet, row, column});
		if (affected != affectedAt.end()) {
			if (states[affected->second] != Calculation::Done) return states[affected->second];
			value.kind = FormulaValue::Kind::Number;
			value.number = results[affected->second];
			return Calculation::Done;
		}
		value = FormulaValue();
		if (sheet >= cells.size()) return Calculation::Unsupported;
		const auto found = cells[sheet].find({row, column});
		if (found == cells[sheet].end()) return Calculation::Done;
		const SheetRenderCell& cell = *found->second;
		switch (cell.kind) {
			case SheetCellKind::Number:
			case SheetCellKind::Date:
				value.kind = FormulaValue::Kind::Number;
				value.number = cell.number;
				return Calculation::Done;
			case SheetCellKind::Text:
				value.kind = FormulaValue::Kind::Text;
				return Calculation::Done;
			case SheetCellKind::Empty:
				// A formula the file keeps no result for is unknown, not empty.
				if (cell.formula.has_value() == true) return Calculation::Unsupported;
				return Calculation::Done;
			case SheetCellKind::Boolean:
			case SheetCellKind::Error:
				break;
		}
		value.kind = FormulaValue::Kind::Other;
		return Calculation::Done;
	};
	const auto sumOf = [&](const FormulaArea& area, double& sum) {
		sum = 0.0;
		if (area.sheet >= cells.size()) return Calculation::Unsupported;
		// Every cell the range holds, the changed formulas among them, in
		// the order Excel adds them: row by row.
		for (auto found = cells[area.sheet].lower_bound({area.first_row, 0});
				found != cells[area.sheet].end() && found->first.first <= area.last_row; ++found) {
			if (found->first.second < area.first_column || found->first.second > area.last_column) continue;
			FormulaValue value;
			const Calculation outcome = lookup(area.sheet, found->first.first, found->first.second, value);
			if (outcome != Calculation::Done) return outcome;
			if (value.kind == FormulaValue::Kind::Number) sum += value.number;
			else if (value.kind == FormulaValue::Kind::Other && found->second->kind == SheetCellKind::Error) return Calculation::Unsupported;
		}
		return Calculation::Done;
	};

	// Each formula is calculated once, after every changed formula it reads -
	// in the order the chain runs, however long it is. A formula that reads
	// itself, or a formula caught in such a loop, never finishes.
	std::vector<std::map<std::pair<std::size_t, std::size_t>, std::size_t>> affectedIn(workbook.sheets.size());
	for (const auto& entry : affectedAt)
		affectedIn[std::get<0>(entry.first)][{std::get<1>(entry.first), std::get<2>(entry.first)}] = entry.second;
	std::vector<std::vector<std::size_t>> readersOf(formulas.size());
	std::vector<std::size_t> unfinishedInputs(formulas.size(), 0);
	for (const auto& entry : affectedAt) {
		const std::size_t index = entry.second;
		for (const FormulaToken& token : formulas[index].tokens) {
			if (token.kind != FormulaTokenKind::Area || token.area.sheet >= affectedIn.size()) continue;
			const FormulaArea& area = token.area;
			const auto& onSheet = affectedIn[area.sheet];
			if (token.is_cell == true) {
				const auto found = onSheet.find({area.first_row, area.first_column});
				if (found == onSheet.end()) continue;
				readersOf[found->second].push_back(index);
				++unfinishedInputs[index];
				continue;
			}
			for (auto found = onSheet.lower_bound({area.first_row, area.first_column});
					found != onSheet.end() && found->first.first <= area.last_row; ++found) {
				if (found->first.second < area.first_column || found->first.second > area.last_column) continue;
				readersOf[found->second].push_back(index);
				++unfinishedInputs[index];
			}
		}
	}
	std::vector<std::size_t> ready;
	for (const auto& entry : affectedAt)
		if (unfinishedInputs[entry.second] == 0) ready.push_back(entry.second);
	while (ready.empty() == false) {
		const std::size_t index = ready.back();
		ready.pop_back();
		// An array formula calculates a range at once; Singularity calculates
		// one value, so it leaves the whole range to Excel.
		if (formulas[index].is_known == false
				|| arrayAt.count({formulas[index].sheet, formulas[index].row, formulas[index].column}) > 0) {
			states[index] = Calculation::Unsupported;
		} else {
			FormulaCalculator calculator(formulas[index].tokens, lookup, sumOf);
			double result = 0.0;
			const Calculation outcome = calculator.calculate(result);
			states[index] = outcome;
			results[index] = result;
		}
		for (const std::size_t reader : readersOf[index])
			if (--unfinishedInputs[reader] == 0) ready.push_back(reader);
	}

	std::vector<XlsxFormulaResult> recalculated;
	for (const auto& entry : affectedAt) {
		const std::size_t index = entry.second;
		XlsxFormulaResult result;
		result.sheet = workbook.sheets[formulas[index].sheet].name;
		result.ref = formulaCells[index]->ref;
		if (states[index] == Calculation::Done) result.stored = stored_number_text(results[index] == 0.0 ? 0.0 : results[index]);
		recalculated.push_back(result);
	}
	// Every other cell of a reached array formula's range, with no value.
	for (const FormulaArea& range : reachedRanges) {
		for (auto found = cells[range.sheet].lower_bound({range.first_row, 0});
				found != cells[range.sheet].end() && found->first.first <= range.last_row; ++found) {
			if (found->first.second < range.first_column || found->first.second > range.last_column) continue;
			if (found->first.first == range.first_row && found->first.second == range.first_column) continue;
			if (found->second->formula.has_value() == true) continue;
			XlsxFormulaResult result;
			result.sheet = workbook.sheets[range.sheet].name;
			result.ref = found->second->ref;
			recalculated.push_back(result);
		}
	}
	return recalculated;

}

std::optional<std::vector<XlsxFormulaResult>> package_formulas_reading_changed_cells(
		const std::filesystem::path& path, const std::vector<XlsxCellAddress>& changed) {

	// A sheet part the search reads whole; past it, no total of the workbook
	// can be kept true. Every cell of every reached array's range is named, up
	// to a bound past which the search gives up the same way.
	constexpr std::uint64_t kMaximumSheetBytes = 64ULL * 1024ULL * 1024ULL;
	constexpr std::size_t kMaximumNamedCells = 2000000;

	try {
		ZipArchive archive(path);
		const zip_int64_t entryCount = zip_get_num_entries(archive.get(), 0);
		if (entryCount < 0 || entryCount > kMaximumArchiveEntries) return std::nullopt;
		const std::unordered_map<std::string, Relationship> packageRelationships = read_relationships(archive.get(), "");
		std::string workbookPart = find_relationship_target(packageRelationships, "/officeDocument");
		PackagePart workbookBytes = read_part(archive.get(), workbookPart);
		if (workbookBytes.is_found == false) {
			workbookPart = "xl/workbook.xml";
			workbookBytes = read_part(archive.get(), workbookPart);
		}
		if (workbookBytes.is_found == false || workbookBytes.is_cut == true) return std::nullopt;
		const pugi::xml_document workbookDocument =
			parse_part(workbookBytes.bytes, "xlsx_malformed_workbook_xml", "The XLSX workbook part is malformed.");
		const pugi::xml_node workbook = root_element(workbookDocument, "workbook");
		const std::unordered_map<std::string, Relationship> relationships = read_relationships(archive.get(), workbookPart);

		// Every tab in workbook order - a chart sheet too, which a formula may
		// name - and the part of each worksheet.
		std::vector<std::string> names;
		std::vector<std::string> parts;
		for (const pugi::xml_node& entry : child_element(workbook, "sheets").children()) {
			if (is_element(entry, "sheet") == false) continue;
			names.push_back(attribute_value(entry, "name"));
			const auto relationship = relationships.find(attribute_value(entry, "id"));
			if (relationship == relationships.end()) return std::nullopt;
			parts.push_back(type_ends_with(relationship->second, "/worksheet") == true ? relationship->second.target : std::string());
		}
		if (names.empty() == true) return std::nullopt;
		std::unordered_map<std::string, std::size_t> sheetsByName;
		for (std::size_t index = 0; index < names.size(); ++index) sheetsByName[upper_ascii(names[index])] = index;

		/* - - - - - - */

		// Every formula of every sheet, with where it stands.
		std::vector<FormulaPlace> formulas;
		std::map<CellPlace, FormulaArea> arrayAt;
		for (std::size_t sheet = 0; sheet < names.size(); ++sheet) {
			if (parts[sheet].empty() == true) continue;
			const PackagePart part = read_part(archive.get(), parts[sheet], kMaximumSheetBytes);
			if (part.is_found == false || part.is_cut == true) return std::nullopt;
			const pugi::xml_document document =
				parse_part(part.bytes, "xlsx_malformed_worksheet_xml", "An XLSX worksheet part is malformed.");
			std::unordered_map<std::string, SharedFormula> sharedFormulas;
			const pugi::xml_node sheetData = child_element(root_element(document, "worksheet"), "sheetData");
			std::size_t runningRow = 0;
			for (const pugi::xml_node& rowNode : sheetData.children()) {
				if (is_element(rowNode, "row") == false) continue;
				std::size_t rowNumber = runningRow + 1;
				const std::optional<unsigned int> declaredRow = attribute_index(rowNode, "r");
				if (declaredRow.has_value() == true) rowNumber = *declaredRow;
				if (rowNumber < 1 || rowNumber > kMaximumRows) continue;
				runningRow = rowNumber;
				std::size_t runningColumn = 0;
				for (const pugi::xml_node& cellNode : rowNode.children()) {
					if (is_element(cellNode, "c") == false) continue;
					std::size_t cellRow = rowNumber - 1;
					std::size_t cellColumn = runningColumn;
					if (parse_cell_position(attribute_value(cellNode, "r"), cellRow, cellColumn) == false) {
						cellRow = rowNumber - 1;
						cellColumn = runningColumn;
					}
					if (cellColumn >= kMaximumColumns) continue;
					runningColumn = cellColumn + 1;
					const std::optional<std::string> text = read_formula(cellNode, cellRow, cellColumn, sharedFormulas);
					if (text.has_value() == false) continue;
					FormulaPlace formula;
					formula.sheet = sheet;
					formula.row = cellRow;
					formula.column = cellColumn;
					formula.tokens = formula_tokens(*text, sheet, sheetsByName);
					formula.is_known = references_known(formula.tokens);
					formulas.push_back(std::move(formula));
					const std::optional<XlsxArrayFormula> array = array_formula_at(cellNode, cellRow, cellColumn);
					if (array.has_value() == false) continue;
					FormulaArea range;
					range.sheet = sheet;
					range.first_row = array->first_row;
					range.first_column = array->first_column;
					range.last_row = array->last_row;
					range.last_column = array->last_column;
					arrayAt[{sheet, range.first_row, range.first_column}] = range;
				}
			}
		}

		/* - - - - - - */

		std::vector<CellPlace> pending;
		for (const XlsxCellAddress& address : changed) {
			const auto sheet = sheetsByName.find(upper_ascii(address.sheet.empty() == true ? names.front() : address.sheet));
			std::size_t row = 0;
			std::size_t column = 0;
			if (sheet == sheetsByName.end() || parse_cell_position(address.ref, row, column) == false) continue;
			pending.emplace_back(sheet->second, row, column);
		}
		std::vector<XlsxFormulaResult> named;
		if (pending.empty() == true) return named;
		std::vector<FormulaArea> reachedRanges;
		const std::vector<bool> isAffected = formulas_reached(formulas, names.size(), arrayAt, pending, reachedRanges);
		for (std::size_t index = 0; index < formulas.size(); ++index) {
			if (isAffected[index] == false) continue;
			XlsxFormulaResult result;
			result.sheet = names[formulas[index].sheet];
			result.ref = cell_reference(formulas[index].row, formulas[index].column);
			named.push_back(result);
		}
		// Every other cell of a reached array's range: what it holds is what the
		// array last calculated.
		for (const FormulaArea& range : reachedRanges) {
			for (std::size_t row = range.first_row; row <= range.last_row; ++row) {
				for (std::size_t column = range.first_column; column <= range.last_column; ++column) {
					if (row == range.first_row && column == range.first_column) continue;
					if (named.size() >= kMaximumNamedCells) return std::nullopt;
					XlsxFormulaResult result;
					result.sheet = names[range.sheet];
					result.ref = cell_reference(row, column);
					named.push_back(result);
				}
			}
		}
		return named;
	} catch (const std::exception&) {
		return std::nullopt;
	}

}

std::string xlsx_shared_formula_text(const std::string& master, long long rowOffset, long long columnOffset) {
	return translate_shared_formula(master, rowOffset, columnOffset);
}

} // namespace singularity::documents
