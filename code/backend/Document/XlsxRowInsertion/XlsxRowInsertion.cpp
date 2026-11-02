#include "Document/XlsxRowInsertion/XlsxRowInsertion.hpp"
#include "Document/XlsxWorkbookView/XlsxWorkbookView.hpp"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace singularity::documents {
namespace {

constexpr long long kMaximumColumns = 16384;
constexpr long long kMaximumRows = static_cast<long long>(kXlsxMaximumRows);
// A part larger than this is not read into memory to be rewritten.
constexpr zip_uint64_t kPartLimit = 64ull * 1024ull * 1024ull;

std::string upper_ascii(std::string text) {
	for (char &character : text) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	return text;
}

std::string trimmed(const std::string &value) {
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

// ---- A1 references ----------------------------------------------------------------

// One end of an A1 reference: "$B$4", "B4", "B" (a whole column), "4" (a whole
// row), each part absolute when "$" marks it. 1-based.
struct ReferencePart {
	bool has_column = false;
	bool is_column_absolute = false;
	long long column = 0;
	bool has_row = false;
	bool is_row_absolute = false;
	long long row = 0;
};

bool parse_reference_part(const std::string &text, ReferencePart &part) {

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
	if (part.has_column == true && (part.column < 1 || part.column > kMaximumColumns)) return false;
	if (part.has_row == true && (part.row < 1 || part.row > kMaximumRows)) return false;
	return true;

}

std::string column_letters(long long column) {
	std::string letters;
	while (column > 0) {
		const long long remainder = (column - 1) % 26;
		letters.insert(letters.begin(), static_cast<char>('A' + remainder));
		column = (column - 1) / 26;
	}
	return letters;
}

// The end written back; "" when it lies off the grid (Excel then shows #REF!).
std::string written_part(const ReferencePart &part) {
	if (part.has_row == true && (part.row < 1 || part.row > kMaximumRows)) return {};
	if (part.has_column == true && (part.column < 1 || part.column > kMaximumColumns)) return {};
	std::string text;
	if (part.has_column == true) {
		if (part.is_column_absolute == true) text.push_back('$');
		text += column_letters(part.column);
	}
	if (part.has_row == true) {
		if (part.is_row_absolute == true) text.push_back('$');
		text += std::to_string(part.row);
	}
	return text;
}

bool is_name_character(char character) {
	const unsigned char byte = static_cast<unsigned char>(character);
	return std::isalnum(byte) != 0 || character == '_' || character == '.' || character == '$' || character == '\\'
		|| byte >= 0x80;
}

// Called with every reference end a formula holds: the sheet it lands on
// (upper-cased; "" where the formula names none and stands on no sheet of its
// own), whether it lies in another workbook, the end itself - which the
// visitor may move - and whether it is the last end of a range. False: the
// formula cannot be moved with certainty.
using ReferenceVisitor = std::function<bool(const std::string &sheet, bool isExternal, ReferencePart &part,
	bool isRangeEnd)>;
// Called with every function name; returns the name to write.
using FunctionVisitor = std::function<std::string(const std::string &name)>;
// Called, where given, with both ends of a range at once instead of `visit`
// with each: a range that loses rows is decided whole, never end by end.
using RangeVisitor = std::function<bool(const std::string &sheet, bool isExternal, ReferencePart &first,
	ReferencePart &last)>;
// Called, where given, with every word that is neither a reference nor a
// function: a defined name, a table a structured reference reads, TRUE, FALSE.
using NameVisitor = std::function<void(const std::string &word)>;

// The formula with every reference as `visit` left it and every function name
// as `function` gives it. Nothing when a visitor refuses, or the formula reads
// a run of sheets ("Q1:Q4!A1"). Text, sheet names, structured and external
// references and error values stay as written - the same reading Excel's own
// shared formulas are translated by (XlsxWorkbookView).
std::optional<std::string> walk_formula(const std::string &formula, const std::string &contextSheet,
		const ReferenceVisitor &visit, const FunctionVisitor &function = nullptr, const RangeVisitor &range = nullptr,
		const NameVisitor &word = nullptr) {

	std::string out;
	const std::size_t size = formula.size();
	std::size_t index = 0;
	std::string qualifier;
	bool isQualified = false;
	bool isExternal = false;
	bool isExternalPending = false;
	const auto forget = [&]() {
		isQualified = false;
		isExternal = false;
		qualifier.clear();
	};
	const auto sheetOfReference = [&]() { return upper_ascii(isQualified == true ? qualifier : contextSheet); };

	while (index < size) {
		const char character = formula[index];

		// "text", with "" standing for one quote.
		if (character == '"') {
			std::size_t end = index + 1;
			while (end < size) {
				if (formula[end] == '"') {
					if (end + 1 < size && formula[end + 1] == '"') {
						end += 2;
						continue;
					}
					break;
				}
				++end;
			}
			end = std::min(end + 1, size);
			out.append(formula, index, end - index);
			index = end;
			forget();
			isExternalPending = false;
			continue;
		}

		// 'Sheet name'! - '[1]Sheet'! in another workbook, 'Q1:Q4'! a run of sheets.
		if (character == '\'') {
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
				name.push_back(formula[end]);
				++end;
			}
			const std::size_t after = end + 1;
			if (after < size && formula[after] == '!') {
				bool external = isExternalPending;
				const std::size_t bracket = name.find(']');
				if (name.find('[') != std::string::npos && bracket != std::string::npos) {
					external = true;
					name = name.substr(bracket + 1);
				}
				if (external == false && name.find(':') != std::string::npos) return std::nullopt;
				qualifier = name;
				isQualified = true;
				isExternal = external;
				isExternalPending = false;
				out.append(formula, index, after + 1 - index);
				index = after + 1;
				continue;
			}
			const std::size_t stop = std::min(after, size);
			out.append(formula, index, stop - index);
			index = stop;
			forget();
			continue;
		}

		// [1] before a sheet names another workbook; Table[Column] is a
		// structured reference.
		if (character == '[') {
			std::size_t end = index;
			int depth = 0;
			while (end < size) {
				if (formula[end] == '[') ++depth;
				if (formula[end] == ']') --depth;
				++end;
				if (depth == 0) break;
			}
			const std::string group = end - index >= 2 ? formula.substr(index + 1, end - index - 2) : std::string();
			const bool isIndex = group.empty() == false
				&& std::all_of(group.begin(), group.end(), [](char digit) { return digit >= '0' && digit <= '9'; });
			const bool followsName = index > 0 && is_name_character(formula[index - 1]);
			out.append(formula, index, end - index);
			index = end;
			forget();
			isExternalPending = isIndex == true && followsName == false;
			continue;
		}

		if (character == '#') {
			std::size_t end = index + 1;
			while (end < size && (std::isalnum(static_cast<unsigned char>(formula[end])) != 0 || formula[end] == '/'
					|| formula[end] == '!' || formula[end] == '?' || formula[end] == '_')) {
				++end;
			}
			out.append(formula, index, end - index);
			index = end;
			forget();
			continue;
		}

		if (is_name_character(character) == false) {
			out.push_back(character);
			++index;
			forget();
			isExternalPending = false;
			continue;
		}

		/* - - - - - - */

		std::size_t end = index;
		while (end < size && is_name_character(formula[end]) == true) ++end;
		const std::string token = formula.substr(index, end - index);
		const char next = end < size ? formula[end] : '\0';

		if (next == '(') {
			out += function != nullptr ? function(token) : token;
			index = end;
			forget();
			isExternalPending = false;
			continue;
		}
		if (next == '!') {
			qualifier = token;
			isQualified = true;
			isExternal = isExternalPending;
			isExternalPending = false;
			out += token;
			out.push_back('!');
			index = end + 1;
			continue;
		}

		if (next == ':') {
			std::size_t secondEnd = end + 1;
			while (secondEnd < size && is_name_character(formula[secondEnd]) == true) ++secondEnd;
			const std::string second = formula.substr(end + 1, secondEnd - end - 1);
			const char afterSecond = secondEnd < size ? formula[secondEnd] : '\0';
			if (afterSecond == '!') {
				// A run of sheets: in another workbook it is left as written.
				if (isExternalPending == false) return std::nullopt;
				out.append(formula, index, secondEnd + 1 - index);
				index = secondEnd + 1;
				isQualified = true;
				isExternal = true;
				isExternalPending = false;
				continue;
			}
			ReferencePart first;
			ReferencePart last;
			const bool isRange = afterSecond != '(' && parse_reference_part(token, first) == true
				&& parse_reference_part(second, last) == true && first.has_column == last.has_column
				&& first.has_row == last.has_row;
			if (isRange == true) {
				const std::string sheet = sheetOfReference();
				const bool isVisited = range != nullptr ? range(sheet, isExternal, first, last)
					: visit(sheet, isExternal, first, false) == true && visit(sheet, isExternal, last, true) == true;
				if (isVisited == false) return std::nullopt;
				const std::string firstText = written_part(first);
				const std::string lastText = written_part(last);
				out += firstText.empty() == true || lastText.empty() == true ? std::string("#REF!")
					: firstText + ":" + lastText;
				index = secondEnd;
				forget();
				isExternalPending = false;
				continue;
			}
		}

		// A number, with its exponent ("1E+5").
		if (std::isdigit(static_cast<unsigned char>(token[0])) != 0 || token[0] == '.') {
			out += token;
			index = end;
			const char last = token.back();
			if ((last == 'E' || last == 'e') && index < size && (formula[index] == '+' || formula[index] == '-')) {
				out.push_back(formula[index]);
				++index;
				while (index < size && std::isdigit(static_cast<unsigned char>(formula[index])) != 0) {
					out.push_back(formula[index]);
					++index;
				}
			}
			forget();
			isExternalPending = false;
			continue;
		}

		ReferencePart cell;
		if (parse_reference_part(token, cell) == true && cell.has_column == true && cell.has_row == true) {
			if (visit(sheetOfReference(), isExternal, cell, false) == false) return std::nullopt;
			const std::string text = written_part(cell);
			out += text.empty() == true ? std::string("#REF!") : text;
			index = end;
			forget();
			isExternalPending = false;
			continue;
		}

		// A defined name, TRUE, FALSE, ...
		if (word != nullptr) word(token);
		out += token;
		index = end;
		forget();
		isExternalPending = false;
	}
	return out;

}

// ---- One insertion ------------------------------------------------------------------

// `count` rows inserted at row `at` of the sheet named `sheet` (upper-cased).
struct Insertion {
	std::string sheet;
	long long at = 0;
	long long count = 0;

	long long moved(long long row) const {
		return row >= at ? row + count : row;
	}
};

// Moves every reference end on the inserted sheet as Excel's Insert Row moves
// it: from the insertion down, by the rows inserted; a range whose last row is
// the sheet's last stays there. A reference that names no sheet, in a formula
// that stands on none, cannot be placed and stops the walk.
ReferenceVisitor moving(const Insertion &insertion) {
	return [&insertion](const std::string &sheet, bool isExternal, ReferencePart &part, bool isRangeEnd) {
		if (isExternal == true || part.has_row == false) return true;
		if (sheet.empty() == true) return false;
		if (sheet != insertion.sheet || part.row < insertion.at) return true;
		if (isRangeEnd == true && part.row == kMaximumRows) return true;
		part.row += insertion.count;
		return true;
	};
}

std::optional<std::string> moved_formula(const std::string &formula, const std::string &contextSheet,
		const Insertion &insertion) {
	return walk_formula(formula, contextSheet, moving(insertion));
}

// Whether a formula reads a row of its own sheet (or any) by its distance
// from the formula's cell - a relative row - so that its text means other cells
// in another row. True too when the formula cannot be read.
bool reads_rows_by_distance(const std::string &formula, const std::string &contextSheet) {
	bool isRelative = false;
	const auto check = [&](const std::string &, bool isExternal, ReferencePart &part, bool) {
		if (isExternal == false && part.has_row == true && part.is_row_absolute == false) isRelative = true;
		return true;
	};
	if (!walk_formula(formula, contextSheet.empty() == true ? std::string("-") : contextSheet, check)) return true;
	return isRelative;
}

// "A1:B5 D7" - a sheet's own list of ranges - with every row moved.
std::string moved_ranges(const std::string &sqref, const Insertion &insertion) {
	std::istringstream stream(sqref);
	std::string item;
	std::string out;
	while (stream >> item) {
		const std::optional<std::string> moved = walk_formula(item, insertion.sheet, moving(insertion));
		if (out.empty() == false) out.push_back(' ');
		out += moved ? *moved : item;
	}
	return out;
}

// The rows (first, last) of each range of "A1:B5 D7".
std::vector<std::pair<long long, long long>> rows_of_ranges(const std::string &sqref) {
	std::vector<std::pair<long long, long long>> rows;
	std::istringstream stream(sqref);
	std::string item;
	while (stream >> item) {
		const std::size_t colon = item.find(':');
		ReferencePart first;
		ReferencePart last;
		if (parse_reference_part(item.substr(0, colon), first) == false) continue;
		if (colon == std::string::npos) last = first;
		else if (parse_reference_part(item.substr(colon + 1), last) == false) continue;
		const long long top = first.has_row == true ? first.row : 1;
		const long long bottom = last.has_row == true ? last.row : kMaximumRows;
		rows.emplace_back(std::min(top, bottom), std::max(top, bottom));
	}
	return rows;
}

// A formula written once for a range of rows - a conditional format's rule, a
// validation's list - is read by each row of it moved by that row's distance
// from the range's first cell. Moving its text is exact only when every row of
// it still reads the same cells once the rows are in: a row that compares with
// a row some distance away, across the insertion, would read another row.
bool anchored_formula_moves_exactly(const std::string &formula, const std::string &formulaSheet, long long anchor,
		const std::vector<std::pair<long long, long long>> &ranges, const Insertion &insertion) {
	const bool formulaMoves = upper_ascii(formulaSheet) == insertion.sheet;
	bool isExact = true;
	const auto check = [&](const std::string &sheet, bool isExternal, ReferencePart &part, bool) {
		if (isExternal == true || part.has_row == false || part.is_row_absolute == true) return true;
		if (sheet != insertion.sheet) return true;
		const long long reference = part.row;
		for (const std::pair<long long, long long> &range : ranges) {
			const long long step = insertion.at - reference + anchor;
			for (const long long row : {range.first, range.second, insertion.at - 1, insertion.at, step - 1, step}) {
				if (row < range.first || row > range.second) continue;
				const long long distance = row - anchor;
				if (reference + distance < 1) continue;
				const long long read = insertion.moved(reference + distance) - insertion.moved(reference);
				const long long moved = formulaMoves == true ? insertion.moved(row) - insertion.moved(anchor) : distance;
				if (read != moved) isExact = false;
			}
		}
		return true;
	};
	if (!walk_formula(formula, formulaSheet, check)) return false;
	return isExact;
}

// ---- The package --------------------------------------------------------------------

std::string local_name(const pugi::xml_node &node) {
	const std::string name = node.name();
	const std::size_t colon = name.find(':');
	return colon == std::string::npos ? name : name.substr(colon + 1);
}

bool is_named(const pugi::xml_node &node, const char *name) {
	return node.type() == pugi::node_element && local_name(node) == name;
}

std::string prefix_of(const pugi::xml_node &node) {
	const std::string name = node.name();
	const std::size_t colon = name.find(':');
	return colon == std::string::npos ? std::string() : name.substr(0, colon + 1);
}

void for_each_element(const pugi::xml_node &node, const std::function<void(pugi::xml_node)> &visit) {
	for (pugi::xml_node child : node.children()) {
		if (child.type() != pugi::node_element) continue;
		visit(child);
		for_each_element(child, visit);
	}
}

pugi::xml_node first_child_named(const pugi::xml_node &node, const char *name) {
	for (pugi::xml_node child : node.children())
		if (is_named(child, name) == true) return child;
	return pugi::xml_node();
}

// An attribute by its local name ("r:id", "id").
pugi::xml_attribute attribute_named(const pugi::xml_node &node, const char *name) {
	for (pugi::xml_attribute attribute : node.attributes()) {
		const std::string full = attribute.name();
		const std::size_t colon = full.find(':');
		if ((colon == std::string::npos ? full : full.substr(colon + 1)) == name) return attribute;
	}
	return pugi::xml_attribute();
}

// A part of the package Singularity would have to rewrite and cannot: too large to
// read into memory, or not XML it can read. Said by the operation in its own
// words (rows inserted, rows deleted, a sheet added).
class PackagePartRefused final : public std::runtime_error {
public:
	explicit PackagePartRefused(const std::string &reason)
		: std::runtime_error(reason)
	{
	}
};

struct ZipCloser {
	void operator()(zip_t *archive) const {
		if (archive != nullptr) zip_discard(archive);
	}
};

struct Relationship {
	std::string id;
	std::string type;
	std::string target;
	bool is_external = false;
};

bool type_ends_with(const Relationship &relationship, const char *suffix) {
	const std::size_t length = std::strlen(suffix);
	return relationship.type.size() >= length
		&& relationship.type.compare(relationship.type.size() - length, length, suffix) == 0;
}

class Package {
public:
	explicit Package(const std::filesystem::path &path) {
		int error = 0;
		archive_.reset(zip_open(path.string().c_str(), 0, &error));
		if (archive_ == nullptr) throw std::runtime_error("The XLSX package could not be opened for update.");
	}

	bool has(const std::string &name) const {
		return zip_name_locate(archive_.get(), name.c_str(), 0) >= 0;
	}

	// A part's XML, read once and rewritten on save when it changed.
	pugi::xml_document &xml(const std::string &name) {
		const auto found = parts_.find(name);
		if (found != parts_.end()) return *found->second;
		zip_stat_t stats;
		zip_stat_init(&stats);
		if (zip_stat(archive_.get(), name.c_str(), 0, &stats) != 0)
			throw std::runtime_error("The XLSX package is missing " + name + ".");
		if (stats.size > kPartLimit) throw PackagePartRefused("the workbook is too large for Singularity to change");
		zip_file_t *file = zip_fopen(archive_.get(), name.c_str(), 0);
		if (file == nullptr) throw std::runtime_error("The XLSX package part " + name + " could not be opened.");
		std::string bytes(static_cast<std::size_t>(stats.size), '\0');
		const zip_int64_t read = bytes.empty() == true ? 0 : zip_fread(file, bytes.data(), stats.size);
		zip_fclose(file);
		if (read < 0 || static_cast<zip_uint64_t>(read) != stats.size)
			throw std::runtime_error("The XLSX package part " + name + " could not be read completely.");
		std::unique_ptr<pugi::xml_document> document = std::make_unique<pugi::xml_document>();
		if (!document->load_buffer(bytes.data(), bytes.size(),
				pugi::parse_default | pugi::parse_ws_pcdata | pugi::parse_declaration, pugi::encoding_utf8))
			throw PackagePartRefused("a part of the workbook Singularity would have to change cannot be read");
		pugi::xml_document &kept = *document;
		parts_.emplace(name, std::move(document));
		return kept;
	}

	// Every part of the package whose name begins with `prefix`.
	std::vector<std::string> names_beginning(const std::string &prefix) const {
		std::vector<std::string> names;
		const zip_int64_t entries = zip_get_num_entries(archive_.get(), 0);
		for (zip_int64_t index = 0; index < entries; ++index) {
			const char *name = zip_get_name(archive_.get(), static_cast<zip_uint64_t>(index), 0);
			if (name != nullptr && std::string(name).rfind(prefix, 0) == 0) names.push_back(name);
		}
		return names;
	}

	void mark_changed(const std::string &name) {
		changed_.insert(name);
	}

	// A new part, written on save.
	pugi::xml_document &create(const std::string &name, const std::string &text) {
		std::unique_ptr<pugi::xml_document> document = std::make_unique<pugi::xml_document>();
		if (!document->load_buffer(text.data(), text.size(), pugi::parse_default | pugi::parse_ws_pcdata
				| pugi::parse_declaration, pugi::encoding_utf8))
			throw std::runtime_error("The XLSX package part " + name + " could not be made.");
		pugi::xml_document &kept = *document;
		parts_[name] = std::move(document);
		changed_.insert(name);
		return kept;
	}

	void remove(const std::string &name) {
		const zip_int64_t index = zip_name_locate(archive_.get(), name.c_str(), 0);
		if (index >= 0) zip_delete(archive_.get(), static_cast<zip_uint64_t>(index));
		parts_.erase(name);
		changed_.erase(name);
	}

	// The relationships of `part`, their targets resolved to part names.
	std::vector<Relationship> relationships(const std::string &part) {
		const std::size_t slash = part.rfind('/');
		const std::string directory = slash == std::string::npos ? std::string() : part.substr(0, slash + 1);
		const std::string file = slash == std::string::npos ? part : part.substr(slash + 1);
		const std::string relationshipsPart = directory + "_rels/" + file + ".rels";
		std::vector<Relationship> found;
		if (has(relationshipsPart) == false) return found;
		for_each_element(xml(relationshipsPart), [&](pugi::xml_node node) {
			if (is_named(node, "Relationship") == false) return;
			Relationship relationship;
			relationship.id = node.attribute("Id").as_string();
			relationship.type = node.attribute("Type").as_string();
			relationship.is_external = std::string(node.attribute("TargetMode").as_string()) == "External";
			std::string target = node.attribute("Target").as_string();
			if (relationship.is_external == false) target = resolved(directory, target);
			relationship.target = target;
			found.push_back(relationship);
		});
		return found;
	}

	void save() {
		for (const std::string &name : changed_) {
			std::ostringstream stream;
			parts_.at(name)->save(stream, "", pugi::format_raw, pugi::encoding_utf8);
			const std::string bytes = stream.str();
			void *owned = std::malloc(bytes.empty() == true ? 1 : bytes.size());
			if (owned == nullptr) throw std::runtime_error("The XLSX package part buffer could not be allocated.");
			if (bytes.empty() == false) std::memcpy(owned, bytes.data(), bytes.size());
			zip_source_t *source = zip_source_buffer(archive_.get(), owned, bytes.size(), 1);
			if (source == nullptr || zip_file_add(archive_.get(), name.c_str(), source, ZIP_FL_OVERWRITE) < 0) {
				if (source != nullptr) zip_source_free(source);
				else std::free(owned);
				throw std::runtime_error("The XLSX package part " + name + " could not be written.");
			}
		}
		zip_t *archive = archive_.release();
		if (zip_close(archive) != 0) {
			zip_discard(archive);
			throw std::runtime_error("The XLSX update could not be finalized.");
		}
	}

private:
	static std::string resolved(const std::string &directory, const std::string &target) {
		if (target.empty() == false && target.front() == '/') return target.substr(1);
		std::vector<std::string> segments;
		std::string joined = directory + target;
		std::size_t start = 0;
		while (start <= joined.size()) {
			const std::size_t end = joined.find('/', start);
			const std::string segment = joined.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (segment == "..") {
				if (segments.empty() == false) segments.pop_back();
			} else if (segment != "." && segment.empty() == false) {
				segments.push_back(segment);
			}
			if (end == std::string::npos) break;
			start = end + 1;
		}
		std::string path;
		for (const std::string &segment : segments) path += (path.empty() == true ? "" : "/") + segment;
		return path;
	}

	std::unique_ptr<zip_t, ZipCloser> archive_;
	std::map<std::string, std::unique_ptr<pugi::xml_document>> parts_;
	std::set<std::string> changed_;
};

struct Worksheet {
	std::string name;
	std::string part;
	std::size_t index = 0;
};

// ---- Formulas of a sheet --------------------------------------------------------------

// Every shared formula of a sheet written into each of its cells, as Excel
// shows each cell: a shared formula is written once for a range, and moving a
// row out of the middle of that range would change what the rows below read.
void unshare_formulas(pugi::xml_document &sheet) {
	struct Master {
		std::string text;
		long long row = 0;
		long long column = 0;
	};
	std::map<std::string, Master> masters;
	for_each_element(sheet, [&](pugi::xml_node node) {
		if (is_named(node, "f") == false || std::string(node.attribute("t").as_string()) != "shared") return;
		const std::string group = node.attribute("si").as_string();
		ReferencePart place;
		if (parse_reference_part(node.parent().attribute("r").as_string(), place) == false || place.has_row == false
				|| place.has_column == false)
			throw XlsxRowInsertionRefused("rows cannot be inserted: a shared formula does not say which cell it stands "
				"in");
		const std::string text = node.child_value();
		if (text.empty() == false) {
			masters[group] = {text, place.row, place.column};
		} else {
			const auto master = masters.find(group);
			if (master == masters.end())
				throw XlsxRowInsertionRefused("rows cannot be inserted: a shared formula stands before the cell it comes "
					"from");
			node.text().set(xlsx_shared_formula_text(master->second.text, place.row - master->second.row,
				place.column - master->second.column).c_str());
		}
		node.remove_attribute("t");
		node.remove_attribute("si");
		node.remove_attribute("ref");
	});
}

// Moves the formula text of `node` (an <f>, a rule's <formula>, an <xm:f>),
// refusing when it cannot be moved with certainty.
bool move_formula_text(pugi::xml_node node, const std::string &contextSheet, const Insertion &insertion,
		const std::string &sheetName) {
	const std::string text = node.child_value();
	if (text.empty() == true) return false;
	const std::optional<std::string> moved = moved_formula(text, contextSheet, insertion);
	if (!moved)
		throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": a formula reads the same cells of "
			"several sheets at once, or a range Singularity cannot place");
	if (*moved == text) return false;
	node.text().set(moved->c_str());
	return true;
}

// The sheet's own list of ranges an element names - its sqref attribute or,
// in Excel's extensions, its <xm:sqref> child.
std::string ranges_of(const pugi::xml_node &node) {
	if (node.attribute("sqref")) return node.attribute("sqref").as_string();
	const pugi::xml_node child = first_child_named(node, "sqref");
	return child ? std::string(child.child_value()) : std::string();
}

// Conditional formats and data validations of a sheet: each rule's formulas
// moved, refused where a row of the rule would read another row once the new
// rows are in.
bool move_rule_formulas(pugi::xml_node rule, const std::string &sheetOfRule, const Insertion &insertion,
		const std::string &sheetName, const std::string &words) {
	const std::string sqref = ranges_of(rule);
	const std::vector<std::pair<long long, long long>> rows = rows_of_ranges(sqref);
	const long long anchor = rows.empty() == true ? 1 : rows.front().first;
	// Rows inserted among the rule's own rows part them: each keeps reading the
	// cells it read only when the rule compares a row with that row alone.
	// Rows inserted elsewhere move the rule's text as Excel's Insert Row does.
	const bool partsRule = upper_ascii(sheetOfRule) == insertion.sheet
		&& std::any_of(rows.begin(), rows.end(), [&](const std::pair<long long, long long> &range) {
			return range.first < insertion.at && insertion.at <= range.second;
		});
	bool isChanged = false;
	for_each_element(rule, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if (name != "formula" && name != "formula1" && name != "formula2" && name != "f") return;
		// <x14:formula1><xm:f>...</xm:f></x14:formula1>: the text is the inner one.
		if (first_child_named(node, "f")) return;
		const std::string text = node.child_value();
		if (text.empty() == true) return;
		if (partsRule == true && anchored_formula_moves_exactly(text, sheetOfRule, anchor, rows, insertion) == false)
			throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": the " + words + " over "
				+ sqref + " compares each row with another row, and would read other rows once the new row is in");
		if (move_formula_text(node, sheetOfRule, insertion, sheetName) == true) isChanged = true;
	});
	return isChanged;
}

// The formulas of another sheet that read the inserted one.
bool move_formulas_of_other_sheet(pugi::xml_document &sheet, const std::string &sheetOfFormulas,
		const Insertion &insertion, const std::string &sheetName) {
	bool isChanged = false;
	for_each_element(sheet, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if (name == "f" && (is_named(node.parent(), "c") == true || is_named(node.parent(), "sparkline") == true)) {
			if (move_formula_text(node, sheetOfFormulas, insertion, sheetName) == true) isChanged = true;
		} else if (name == "conditionalFormatting" || name == "dataValidation") {
			if (move_rule_formulas(node, sheetOfFormulas, insertion, sheetName,
					name == "dataValidation" ? "data validation" : "conditional format") == true)
				isChanged = true;
		} else if (name == "hyperlink" && node.attribute("location")) {
			const std::optional<std::string> moved = moved_formula(node.attribute("location").as_string(),
				sheetOfFormulas, insertion);
			if (moved && *moved != node.attribute("location").as_string()) {
				node.attribute("location").set_value(moved->c_str());
				isChanged = true;
			}
		}
	});
	return isChanged;
}

// ---- The inserted sheet ---------------------------------------------------------------

struct SheetRow {
	pugi::xml_node node;
	long long number = 0;
};

std::vector<SheetRow> rows_of(const pugi::xml_node &sheetData, const std::string &sheetName) {
	std::vector<SheetRow> rows;
	for (pugi::xml_node row : sheetData.children()) {
		if (is_named(row, "row") == false) continue;
		const long long number = row.attribute("r").as_llong();
		if (number < 1) throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": its rows do not say "
			"where they stand");
		rows.push_back({row, number});
	}
	return rows;
}

// Whether `row` is the sheet's heading: its first row with anything in it, or
// a row frozen above the rest.
bool is_heading_row(long long row, const std::vector<SheetRow> &rows, long long frozenRows) {
	if (row <= frozenRows) return true;
	for (const SheetRow &candidate : rows) {
		bool hasCells = false;
		for (pugi::xml_node cell : candidate.node.children()) if (is_named(cell, "c") == true) hasCells = true;
		if (hasCells == true) return candidate.number == row;
	}
	return false;
}

void move_cell_reference(pugi::xml_node cell, const Insertion &insertion, const std::string &sheetName) {
	ReferencePart place;
	if (parse_reference_part(cell.attribute("r").as_string(), place) == false || place.has_row == false
			|| place.has_column == false)
		throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": its cells do not say where they "
			"stand");
	place.row = insertion.moved(place.row);
	cell.attribute("r").set_value(written_part(place).c_str());
}

// The formula of the cell in `column` of row `number`, as the package holds it
// (without "="); nothing where the cell holds none, or holds an array formula
// or a data table, which are not filled down.
std::optional<std::string> formula_in_row(pugi::xml_node sheetData, long long number, long long column) {
	for (pugi::xml_node row : sheetData.children()) {
		if (is_named(row, "row") == false || row.attribute("r").as_llong() != number) continue;
		for (pugi::xml_node cell : row.children()) {
			if (is_named(cell, "c") == false) continue;
			ReferencePart place;
			if (parse_reference_part(cell.attribute("r").as_string(), place) == false || place.column != column) continue;
			const pugi::xml_node formula = first_child_named(cell, "f");
			const std::string kind = formula.attribute("t").as_string();
			if (!formula || kind == "array" || kind == "dataTable" || std::string(formula.child_value()).empty() == true)
				return std::nullopt;
			return std::string(formula.child_value());
		}
		return std::nullopt;
	}
	return std::nullopt;
}

// A table's own part: its range, its filter and its sort moved; a row inserted
// right below it joins it, as a row typed there joins it in Excel; and a new
// row inside it takes the formula of every column it calculates - the formula
// of that column's cell in the row next to it, moved down (or up) to the new
// row the way Excel fills a calculated column, so every reference of it reads
// the new row's own cells and every fixed one what the table's rows read. The
// column's own formula in the table part moves with the rows too.
void move_table(pugi::xml_document &table, const Insertion &insertion, const std::string &sheetName,
		pugi::xml_node sheetData, std::vector<XlsxFilledFormula> &filled, const std::string &workbookSheetName) {
	pugi::xml_node root;
	for (pugi::xml_node child : table.children()) if (is_named(child, "table") == true) root = child;
	if (!root) return;
	const std::string reference = root.attribute("ref").as_string();
	const std::size_t colon = reference.find(':');
	ReferencePart first;
	ReferencePart last;
	if (colon == std::string::npos || parse_reference_part(reference.substr(0, colon), first) == false
			|| parse_reference_part(reference.substr(colon + 1), last) == false || first.has_row == false
			|| last.has_row == false)
		throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": a table on it does not say where "
			"it stands");
	const long long headerRows = root.attribute("headerRowCount") ? root.attribute("headerRowCount").as_llong() : 1;
	const long long totalRows = root.attribute("totalsRowCount").as_llong();
	const bool joinsBelow = insertion.at == last.row + 1 && totalRows == 0;
	// New rows put in right under the heading become the table's first rows.
	const bool becomesFirstRows = headerRows > 0 && insertion.at == first.row + headerRows;
	first.row = insertion.moved(first.row);
	last.row = insertion.moved(last.row) + (joinsBelow == true ? insertion.count : 0);
	root.attribute("ref").set_value((written_part(first) + ":" + written_part(last)).c_str());
	for_each_element(root, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if ((name == "autoFilter" || name == "sortState" || name == "sortCondition") && node.attribute("ref")) {
			std::string moved = moved_ranges(node.attribute("ref").as_string(), insertion);
			if (joinsBelow == true) {
				const std::size_t separator = moved.find(':');
				ReferencePart end;
				if (separator != std::string::npos && parse_reference_part(moved.substr(separator + 1), end) == true
						&& end.has_row == true) {
					end.row += insertion.count;
					moved = moved.substr(0, separator + 1) + written_part(end);
				}
			}
			node.attribute("ref").set_value(moved.c_str());
		}
	});

	// The formula of each calculated column, in each new row inside the table;
	// and each column's own formulas, read against the moved rows.
	const long long firstDataRow = first.row + headerRows;
	const long long lastDataRow = last.row - totalRows;
	pugi::xml_node columns = first_child_named(root, "tableColumns");
	struct Calculated {
		long long column = 0;
		pugi::xml_node node;
		std::string moved;
		std::string name;
	};
	std::vector<Calculated> calculated;
	long long column = first.column;
	for (pugi::xml_node tableColumn : columns.children()) {
		if (is_named(tableColumn, "tableColumn") == false) continue;
		const pugi::xml_node totals = first_child_named(tableColumn, "totalsRowFormula");
		if (totals && std::string(totals.child_value()).empty() == false)
			move_formula_text(totals, workbookSheetName, insertion, sheetName);
		pugi::xml_node formula = first_child_named(tableColumn, "calculatedColumnFormula");
		if (formula && std::string(formula.child_value()).empty() == false) {
			const std::optional<std::string> moved = moved_formula(formula.child_value(), workbookSheetName, insertion);
			if (!moved)
				throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": a table's column is worked "
					"out by a formula that reads the same cells of several sheets at once");
			if (std::string(formula.attribute("array").as_string()) != "1")
				calculated.push_back({column, formula, *moved, tableColumn.attribute("name").as_string()});
			else
				formula.text().set(moved->c_str());
		}
		++column;
	}
	std::map<long long, std::string> firstRowFormulas;
	for (long long row = insertion.at; row < insertion.at + insertion.count; ++row) {
		if (row < firstDataRow || row > lastDataRow) continue;
		pugi::xml_node rowNode;
		for (pugi::xml_node candidate : sheetData.children())
			if (is_named(candidate, "row") == true && candidate.attribute("r").as_llong() == row) rowNode = candidate;
		if (!rowNode) continue;
		const std::string prefix = prefix_of(rowNode);
		for (const Calculated &entry : calculated) {
			// The column's cell in the row above the new ones, else in the row
			// below them, each already moved; only where neither holds a formula
			// does the column's own formula stand - and only when it reads no row
			// by its distance, which would need the row it was written for.
			std::string text;
			const long long above = insertion.at - 1;
			const long long below = insertion.at + insertion.count;
			std::optional<std::string> neighbour;
			if (above >= firstDataRow) neighbour = formula_in_row(sheetData, above, entry.column);
			if (neighbour) {
				text = xlsx_shared_formula_text(*neighbour, row - above, 0);
			} else {
				if (below <= lastDataRow) neighbour = formula_in_row(sheetData, below, entry.column);
				if (neighbour) text = xlsx_shared_formula_text(*neighbour, row - below, 0);
				else if (reads_rows_by_distance(entry.moved, workbookSheetName) == false) text = entry.moved;
				else
					throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": the table's column "
						+ "\"" + entry.name + "\" is worked out by a formula Singularity cannot place in the new row");
			}
			if (text.find("#REF!") != std::string::npos)
				throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheetName + ": the table's column "
					+ "\"" + entry.name + "\" would read cells off the sheet in the new row");
			if (row == insertion.at) firstRowFormulas[entry.column] = text;
			ReferencePart place;
			place.has_column = true;
			place.column = entry.column;
			place.has_row = true;
			place.row = row;
			const std::string ref = written_part(place);
			pugi::xml_node cell;
			pugi::xml_node cellAfter;
			for (pugi::xml_node candidate : rowNode.children()) {
				if (is_named(candidate, "c") == false) continue;
				ReferencePart at;
				if (parse_reference_part(candidate.attribute("r").as_string(), at) == false) continue;
				if (at.column == entry.column) cell = candidate;
				if (at.column > entry.column && !cellAfter) cellAfter = candidate;
			}
			if (!cell) {
				cell = cellAfter ? rowNode.insert_child_before((prefix + "c").c_str(), cellAfter)
					: rowNode.append_child((prefix + "c").c_str());
				cell.append_attribute("r") = ref.c_str();
			}
			while (cell.first_child()) cell.remove_child(cell.first_child());
			cell.remove_attribute("t");
			cell.append_child((prefix + "f").c_str()).text().set(text.c_str());
			filled.push_back({workbookSheetName, ref, "=" + text});
		}
	}

	// The column's own formula is written for the table's first row: where the
	// new rows are that first row, it is theirs.
	for (const Calculated &entry : calculated) {
		const auto firstRow = firstRowFormulas.find(entry.column);
		const std::string text = becomesFirstRows == true && firstRow != firstRowFormulas.end() ? firstRow->second
			: entry.moved;
		entry.node.text().set(text.c_str());
	}
}

// A drawing's anchors (0-based rows) moved with the rows they stand on.
bool move_drawing_anchors(pugi::xml_document &drawing, const Insertion &insertion) {
	bool isChanged = false;
	for_each_element(drawing, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if (name != "from" && name != "to") return;
		pugi::xml_node row = first_child_named(node, "row");
		if (!row) return;
		const long long index = std::atoll(row.child_value());
		if (index + 1 < insertion.at) return;
		row.text().set(std::to_string(index + insertion.count).c_str());
		isChanged = true;
	});
	return isChanged;
}

// A legacy drawing of comments: each note's row and anchor (0-based).
bool move_note_shapes(pugi::xml_document &shapes, const Insertion &insertion) {
	bool isChanged = false;
	for_each_element(shapes, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if (name == "Row") {
			const long long index = std::atoll(node.child_value());
			if (index + 1 < insertion.at) return;
			node.text().set(std::to_string(index + insertion.count).c_str());
			isChanged = true;
		} else if (name == "Anchor") {
			std::vector<std::string> values;
			std::string current;
			for (const char character : std::string(node.child_value())) {
				if (character == ',') {
					values.push_back(current);
					current.clear();
				} else {
					current.push_back(character);
				}
			}
			values.push_back(current);
			if (values.size() != 8) return;
			for (const std::size_t at : {std::size_t(2), std::size_t(6)}) {
				const long long index = std::atoll(trimmed(values[at]).c_str());
				if (index + 1 >= insertion.at) values[at] = " " + std::to_string(index + insertion.count);
			}
			std::string joined;
			for (std::size_t index = 0; index < values.size(); ++index) joined += (index == 0 ? "" : ",") + values[index];
			if (joined != node.child_value()) {
				node.text().set(joined.c_str());
				isChanged = true;
			}
		}
	});
	return isChanged;
}

// A row the new ones take their look from, and the empty cells that carry it.
void add_new_rows(pugi::xml_node sheetData, const Insertion &insertion, const pugi::xml_node &lookFrom,
		const pugi::xml_node &below, const pugi::xml_node &above) {
	const std::string prefix = prefix_of(sheetData);
	pugi::xml_node rowAfter;
	for (pugi::xml_node candidate : sheetData.children())
		if (is_named(candidate, "row") == true && candidate.attribute("r").as_llong() > insertion.at + insertion.count - 1) {
			rowAfter = candidate;
			break;
		}
	for (long long number = insertion.at; number < insertion.at + insertion.count; ++number) {
		pugi::xml_node row = rowAfter ? sheetData.insert_child_before((prefix + "row").c_str(), rowAfter)
			: sheetData.append_child((prefix + "row").c_str());
		row.append_attribute("r") = number;
		if (lookFrom) {
			for (const char *name : {"s", "customFormat", "ht", "customHeight"})
				if (lookFrom.attribute(name)) row.append_attribute(name) = lookFrom.attribute(name).value();
			for (pugi::xml_node cell : lookFrom.children()) {
				if (is_named(cell, "c") == false) continue;
				const std::string style = cell.attribute("s").as_string();
				if (style.empty() == true || style == "0") continue;
				ReferencePart place;
				if (parse_reference_part(cell.attribute("r").as_string(), place) == false) continue;
				place.row = number;
				pugi::xml_node empty = row.append_child((prefix_of(cell) + "c").c_str());
				empty.append_attribute("r") = written_part(place).c_str();
				empty.append_attribute("s") = style.c_str();
			}
		}
		// Inside a group of rows the new row joins the group.
		if (above && below && above.attribute("outlineLevel")
				&& std::string(above.attribute("outlineLevel").as_string()) == below.attribute("outlineLevel").as_string())
			row.append_attribute("outlineLevel") = above.attribute("outlineLevel").value();
	}
}

std::string sheet_words(const std::string &name) {
	return "\"" + name + "\"";
}

// The relationships part of `part` ("xl/_rels/workbook.xml.rels").
std::string relationships_part_of(const std::string &part) {
	const std::size_t slash = part.rfind('/');
	return (slash == std::string::npos ? std::string() : part.substr(0, slash + 1)) + "_rels/"
		+ part.substr(slash == std::string::npos ? 0 : slash + 1) + ".rels";
}

// Excel keeps the order it last calculated formulas in; with cells moved that
// order names the wrong cells, so it goes and Excel builds it again.
void remove_calculation_chain(Package &package, const std::string &workbookPart,
		const std::vector<Relationship> &workbookRelationships) {
	for (const Relationship &relationship : workbookRelationships) {
		if (type_ends_with(relationship, "/calcChain") == false) continue;
		package.remove(relationship.target);
		const std::string relationshipsPart = relationships_part_of(workbookPart);
		std::vector<pugi::xml_node> gone;
		for_each_element(package.xml(relationshipsPart), [&](pugi::xml_node node) {
			if (is_named(node, "Relationship") == true && relationship.id == node.attribute("Id").as_string())
				gone.push_back(node);
		});
		if (package.has("[Content_Types].xml") == true) {
			for_each_element(package.xml("[Content_Types].xml"), [&](pugi::xml_node node) {
				if (is_named(node, "Override") == true
						&& std::string(node.attribute("PartName").as_string()) == "/" + relationship.target)
					gone.push_back(node);
			});
			package.mark_changed("[Content_Types].xml");
		}
		for (pugi::xml_node node : gone) node.parent().remove_child(node);
		package.mark_changed(relationshipsPart);
		break;
	}
}

// Whether a reading of the workbook holds every cell it has, so a proof made
// from it proves the whole workbook: nothing cut at a safety limit or left
// unread. A chart on a tab of its own, which the reading does not draw, holds
// no cells and leaves the reading whole.
bool reading_is_whole(const XlsxWorkbookViewResult &view) {
	if (view.status == ExtractionStatus::unreadable || view.diagnostics.empty() == false) return false;
	for (const char *gap : {"shown empty", "shown up to row", "too large", "could not be read"})
		if (view.limitation.find(gap) != std::string::npos) return false;
	return true;
}

} // namespace

// ---- Inserting --------------------------------------------------------------------

std::size_t xlsx_row_once_inserted(std::size_t row, std::vector<std::size_t> inserted) {
	std::sort(inserted.begin(), inserted.end());
	std::size_t moved = row;
	for (const std::size_t at : inserted)
		if (moved >= at) ++moved;
	return moved;
}

std::optional<std::string> xlsx_formula_with_rows_inserted(const std::string &formula,
		const std::string &formulaSheet, const std::string &insertedSheet, std::size_t at, std::size_t count) {
	const bool hasSign = formula.empty() == false && formula.front() == '=';
	Insertion insertion{upper_ascii(insertedSheet), static_cast<long long>(at), static_cast<long long>(count)};
	const std::optional<std::string> moved = moved_formula(hasSign == true ? formula.substr(1) : formula,
		formulaSheet, insertion);
	if (!moved) return std::nullopt;
	return hasSign == true ? "=" + *moved : *moved;
}

// The insertion itself; insert_xlsx_rows says a part it cannot rewrite in its
// own words.
static std::vector<XlsxFilledFormula> insert_rows_into(const std::filesystem::path &path,
		const std::vector<XlsxRowInsertion> &rows) {

	std::vector<XlsxFilledFormula> filled;
	if (rows.empty() == true) return filled;
	Package package(path);

	// The workbook and its sheets, by the names a lawyer sees on their tabs.
	std::string workbookPart = "xl/workbook.xml";
	for (const Relationship &relationship : package.relationships(""))
		if (type_ends_with(relationship, "/officeDocument") == true) workbookPart = relationship.target;
	pugi::xml_document &workbook = package.xml(workbookPart);
	const std::vector<Relationship> workbookRelationships = package.relationships(workbookPart);
	std::vector<Worksheet> sheets;
	std::size_t sheetIndex = 0;
	std::vector<std::string> sheetNamesByIndex;
	for_each_element(workbook, [&](pugi::xml_node node) {
		if (is_named(node, "sheet") == false) return;
		const std::string name = node.attribute("name").as_string();
		sheetNamesByIndex.push_back(name);
		const std::string id = attribute_named(node, "id").as_string();
		for (const Relationship &relationship : workbookRelationships)
			if (relationship.id == id && type_ends_with(relationship, "/worksheet") == true)
				sheets.push_back({name, relationship.target, sheetIndex});
		++sheetIndex;
	});
	if (sheets.empty() == true) throw std::runtime_error("The workbook has no sheet rows can be inserted into.");

	// The new rows of each sheet, as runs of rows inserted together.
	std::map<std::string, std::vector<long long>> numbersBySheet;
	for (const XlsxRowInsertion &row : rows) {
		const std::string name = row.sheet.empty() == true ? sheets.front().name : row.sheet;
		const bool known = std::any_of(sheets.begin(), sheets.end(), [&](const Worksheet &sheet) { return sheet.name == name; });
		if (known == false) throw XlsxRowInsertionRefused("rows cannot be inserted: the workbook has no sheet named "
			+ sheet_words(name));
		if (row.row < 1 || row.row > kXlsxMaximumRows)
			throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheet_words(name) + ": row "
				+ std::to_string(row.row) + " is not a row of a sheet");
		std::vector<long long> &numbers = numbersBySheet[name];
		if (std::find(numbers.begin(), numbers.end(), static_cast<long long>(row.row)) != numbers.end())
			throw XlsxRowInsertionRefused("rows cannot be inserted into " + sheet_words(name) + ": row "
				+ std::to_string(row.row) + " is inserted twice");
		numbers.push_back(static_cast<long long>(row.row));
	}

	// Every sheet's formulas each in its own cell, once, before anything moves.
	for (const Worksheet &sheet : sheets) {
		pugi::xml_document &document = package.xml(sheet.part);
		bool hasShared = false;
		for_each_element(document, [&](pugi::xml_node node) {
			if (is_named(node, "f") == true && std::string(node.attribute("t").as_string()) == "shared") hasShared = true;
		});
		if (hasShared == true) {
			unshare_formulas(document);
			package.mark_changed(sheet.part);
		}
	}

	for (std::pair<const std::string, std::vector<long long>> &entry : numbersBySheet) {
		const std::string sheetName = entry.first;
		std::vector<long long> &numbers = entry.second;
		std::sort(numbers.begin(), numbers.end());
		const Worksheet &target = *std::find_if(sheets.begin(), sheets.end(),
			[&](const Worksheet &sheet) { return sheet.name == sheetName; });
		const std::string words = sheet_words(sheetName);
		std::vector<Insertion> insertions;
		for (const long long number : numbers) {
			if (insertions.empty() == false && insertions.back().at + insertions.back().count == number)
				++insertions.back().count;
			else
				insertions.push_back({upper_ascii(sheetName), number, 1});
		}
		const std::vector<Relationship> sheetRelationships = package.relationships(target.part);

		// What the sheet holds that Singularity cannot move, refused before anything moves.
		for (const Relationship &relationship : sheetRelationships) {
			if (type_ends_with(relationship, "/pivotTable") == true)
				throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": it holds a pivot table");
			static const char *known[] = {"/drawing", "/vmlDrawing", "/comments", "/threadedComment", "/table",
				"/hyperlink", "/printerSettings", "/image"};
			const bool isKnown = std::any_of(std::begin(known), std::end(known),
				[&](const char *suffix) { return type_ends_with(relationship, suffix); });
			if (isKnown == false)
				throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": it holds content Singularity does "
					"not move with rows");
			if (type_ends_with(relationship, "/table") == true)
				for (const Relationship &tableRelationship : package.relationships(relationship.target))
					if (type_ends_with(tableRelationship, "/queryTable") == true)
						throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": a table on it is "
							"filled by a data query");
		}

		for (const Insertion &insertion : insertions) {
			pugi::xml_document &sheet = package.xml(target.part);
			pugi::xml_node worksheet;
			for (pugi::xml_node child : sheet.children()) if (is_named(child, "worksheet") == true) worksheet = child;
			const pugi::xml_node sheetData = worksheet ? first_child_named(worksheet, "sheetData") : pugi::xml_node();
			if (!sheetData) throw std::runtime_error("The XLSX worksheet has no sheetData element.");
			const std::vector<SheetRow> sheetRows = rows_of(sheetData, words);

			/* - - - - - - */

			// Refusals: nothing has moved yet.
			const pugi::xml_node protection = first_child_named(worksheet, "sheetProtection");
			if (protection && protection.attribute("sheet").as_bool() == true
					&& (!protection.attribute("insertRows") || protection.attribute("insertRows").as_bool() == true))
				throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": the sheet is protected against "
					"inserted rows");
			const long long lastRow = sheetRows.empty() == true ? 0 : sheetRows.back().number;
			if (lastRow >= insertion.at && lastRow + insertion.count > kMaximumRows)
				throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": its rows would run past the last "
					"row a sheet holds");
			for_each_element(sheetData, [&](pugi::xml_node node) {
				if (is_named(node, "f") == false) return;
				const std::string kind = node.attribute("t").as_string();
				if (kind == "dataTable")
					throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": it holds a what-if data "
						"table");
				if (kind != "array" || !node.attribute("ref")) return;
				const std::vector<std::pair<long long, long long>> covered = rows_of_ranges(node.attribute("ref").as_string());
				if (covered.empty() == false && covered.front().first < insertion.at && insertion.at <= covered.front().second)
					throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": row "
						+ std::to_string(insertion.at) + " falls inside the array formula over "
						+ node.attribute("ref").as_string() + ", which Excel keeps whole");
			});

			/* - - - - - - */

			// The rows that give the new ones their look, read before they move.
			long long frozenRows = 0;
			for_each_element(worksheet, [&](pugi::xml_node node) {
				if (is_named(node, "pane") == false) return;
				const std::string state = node.attribute("state").as_string();
				if (state == "frozen" || state == "frozenSplit") frozenRows = node.attribute("ySplit").as_llong();
			});
			pugi::xml_node above;
			pugi::xml_node below;
			for (const SheetRow &row : sheetRows) {
				if (row.number == insertion.at - 1) above = row.node;
				if (row.number == insertion.at) below = row.node;
			}
			const pugi::xml_node lookFrom = above && is_heading_row(insertion.at - 1, sheetRows, frozenRows) == false
				? above : below;

			// The rows and their cells, from the insertion down.
			for (const SheetRow &row : sheetRows) {
				if (row.number < insertion.at) continue;
				row.node.attribute("r").set_value(row.number + insertion.count);
				for (pugi::xml_node cell : row.node.children())
					if (is_named(cell, "c") == true) move_cell_reference(cell, insertion, words);
			}
			add_new_rows(sheetData, insertion, lookFrom, below, above);

			// Everything else on the sheet that names its cells.
			for_each_element(worksheet, [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if (name == "f" && is_named(node.parent(), "c") == true) {
					if (std::string(node.attribute("t").as_string()) == "array" && node.attribute("ref"))
						node.attribute("ref").set_value(moved_ranges(node.attribute("ref").as_string(), insertion).c_str());
					move_formula_text(node, sheetName, insertion, words);
					return;
				}
				if (name == "conditionalFormatting" || name == "dataValidation") {
					move_rule_formulas(node, sheetName, insertion, words,
						name == "dataValidation" ? "data validation" : "conditional format");
					if (node.attribute("sqref"))
						node.attribute("sqref").set_value(moved_ranges(node.attribute("sqref").as_string(), insertion).c_str());
					return;
				}
				if (name == "sqref" && node.child_value()[0] != '\0') {
					node.text().set(moved_ranges(node.child_value(), insertion).c_str());
					return;
				}
				// A sparkline's source may be this sheet's rows or another's.
				if (name == "f" && is_named(node.parent(), "sparkline") == true) {
					move_formula_text(node, sheetName, insertion, words);
					return;
				}
				for (const char *attribute : {"ref", "sqref", "topLeftCell", "activeCell"}) {
					if (!node.attribute(attribute)) continue;
					// A consolidation source names its own sheet, which may be another.
					if (name == "f" || name == "row" || name == "c" || node.attribute("sheet")) continue;
					node.attribute(attribute).set_value(moved_ranges(node.attribute(attribute).as_string(), insertion).c_str());
				}
				if (name == "hyperlink" && node.attribute("location")) {
					const std::optional<std::string> moved = moved_formula(node.attribute("location").as_string(),
						sheetName, insertion);
					if (moved) node.attribute("location").set_value(moved->c_str());
				}
				if (name == "brk" && is_named(node.parent(), "rowBreaks") == true && node.attribute("id").as_llong() >= insertion.at)
					node.attribute("id").set_value(node.attribute("id").as_llong() + insertion.count);
				// Pictures and controls anchored inside Excel's extensions.
				if ((name == "from" || name == "to") && first_child_named(node, "row")) {
					pugi::xml_node row = first_child_named(node, "row");
					const long long index = std::atoll(row.child_value());
					if (index + 1 >= insertion.at) row.text().set(std::to_string(index + insertion.count).c_str());
				}
			});
			package.mark_changed(target.part);

			// The other sheets' formulas that read this one.
			for (const Worksheet &other : sheets) {
				if (other.part == target.part) continue;
				if (move_formulas_of_other_sheet(package.xml(other.part), other.name, insertion, words) == true)
					package.mark_changed(other.part);
				// And the formulas its tables work their columns out by.
				for (const Relationship &relationship : package.relationships(other.part)) {
					if (type_ends_with(relationship, "/table") == false || relationship.is_external == true
							|| package.has(relationship.target) == false)
						continue;
					bool isChanged = false;
					for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
						const std::string name = local_name(node);
						if (name != "calculatedColumnFormula" && name != "totalsRowFormula") return;
						if (std::string(node.child_value()).empty() == true) return;
						if (move_formula_text(node, other.name, insertion, words) == true) isChanged = true;
					});
					if (isChanged == true) package.mark_changed(relationship.target);
				}
			}

			// Defined names: a print area, a filter, a named range.
			bool isWorkbookChanged = false;
			for_each_element(workbook, [&](pugi::xml_node node) {
				if (is_named(node, "definedName") == false) return;
				const std::string text = node.child_value();
				std::string context;
				if (node.attribute("localSheetId")) {
					const std::size_t index = static_cast<std::size_t>(node.attribute("localSheetId").as_ullong());
					if (index < sheetNamesByIndex.size()) context = sheetNamesByIndex[index];
				}
				const std::optional<std::string> moved = moved_formula(text, context, insertion);
				if (!moved)
					throw XlsxRowInsertionRefused("rows cannot be inserted into " + words + ": the defined name "
						+ sheet_words(node.attribute("name").as_string()) + " names cells without saying on which sheet");
				if (*moved == text) return;
				node.text().set(moved->c_str());
				isWorkbookChanged = true;
			});
			if (isWorkbookChanged == true) package.mark_changed(workbookPart);

			// The sheet's tables, pictures, notes and comments.
			for (const Relationship &relationship : sheetRelationships) {
				if (relationship.is_external == true || package.has(relationship.target) == false) continue;
				if (type_ends_with(relationship, "/table") == true) {
					move_table(package.xml(relationship.target), insertion, words, sheetData, filled, sheetName);
					package.mark_changed(relationship.target);
				} else if (type_ends_with(relationship, "/drawing") == true) {
					if (move_drawing_anchors(package.xml(relationship.target), insertion) == true)
						package.mark_changed(relationship.target);
				} else if (type_ends_with(relationship, "/vmlDrawing") == true) {
					if (move_note_shapes(package.xml(relationship.target), insertion) == true)
						package.mark_changed(relationship.target);
				} else if (type_ends_with(relationship, "/comments") == true
						|| type_ends_with(relationship, "/threadedComment") == true) {
					bool isChanged = false;
					for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
						const std::string name = local_name(node);
						if ((name != "comment" && name != "threadedComment") || !node.attribute("ref")) return;
						const std::string moved = moved_ranges(node.attribute("ref").as_string(), insertion);
						if (moved == node.attribute("ref").as_string()) return;
						node.attribute("ref").set_value(moved.c_str());
						isChanged = true;
					});
					if (isChanged == true) package.mark_changed(relationship.target);
				}
			}

			// Charts that read the sheet - on any sheet or a tab of their own - and
			// the data pivot tables are built from.
			for (const std::string &chart : package.names_beginning("xl/charts/")) {
				const std::string file = chart.substr(chart.rfind('/') + 1);
				if (file.rfind("chart", 0) != 0 || file.size() < 4 || file.compare(file.size() - 4, 4, ".xml") != 0) continue;
				bool isChanged = false;
				for_each_element(package.xml(chart), [&](pugi::xml_node node) {
					if (local_name(node) != "f") return;
					if (move_formula_text(node, std::string(), insertion, words) == true) isChanged = true;
				});
				if (isChanged == true) package.mark_changed(chart);
			}
			for (const Relationship &relationship : workbookRelationships) {
				if (type_ends_with(relationship, "/pivotCacheDefinition") == false || package.has(relationship.target) == false)
					continue;
				bool isChanged = false;
				for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
					if (is_named(node, "worksheetSource") == false || !node.attribute("ref")) return;
					if (upper_ascii(node.attribute("sheet").as_string()) != insertion.sheet) return;
					node.attribute("ref").set_value(moved_ranges(node.attribute("ref").as_string(), insertion).c_str());
					isChanged = true;
				});
				if (isChanged == true) package.mark_changed(relationship.target);
			}
		}
	}

	remove_calculation_chain(package, workbookPart, workbookRelationships);
	package.save();
	return filled;

}

std::vector<XlsxFilledFormula> insert_xlsx_rows(const std::filesystem::path &path,
		const std::vector<XlsxRowInsertion> &rows) {
	try {
		return insert_rows_into(path, rows);
	} catch (const PackagePartRefused &refused) {
		throw XlsxRowInsertionRefused(std::string("rows cannot be inserted: ") + refused.what());
	}
}

// ---- Proving an insertion ---------------------------------------------------------------

std::optional<std::string> xlsx_rows_inserted_exactly(const std::filesystem::path &before,
		const std::filesystem::path &after, const std::vector<XlsxRowInsertion> &rows,
		const std::vector<XlsxFilledFormula> &filled) {

	const XlsxWorkbookViewResult was = read_xlsx_workbook_view({before, DocumentFormat::xlsx});
	const XlsxWorkbookViewResult now = read_xlsx_workbook_view({after, DocumentFormat::xlsx});
	if (was.status == ExtractionStatus::unreadable || now.status == ExtractionStatus::unreadable)
		return std::string("the workbook could not be read to prove the inserted rows");
	if (reading_is_whole(was) == false || reading_is_whole(now) == false)
		return std::string("the workbook is too large for Singularity to prove the inserted rows");
	if (was.workbook.sheets.size() != now.workbook.sheets.size())
		return std::string("the workbook's sheets changed");

	std::map<std::string, std::vector<std::size_t>> insertedBySheet;
	for (const XlsxRowInsertion &row : rows)
		insertedBySheet[row.sheet.empty() == true ? was.workbook.sheets.front().name : row.sheet].push_back(row.row);
	std::map<std::pair<std::string, std::string>, std::string> filledAt;
	for (const XlsxFilledFormula &formula : filled) filledAt[{formula.sheet, formula.ref}] = formula.formula;

	for (std::size_t index = 0; index < was.workbook.sheets.size(); ++index) {
		const SheetRender &previous = was.workbook.sheets[index];
		const SheetRender &current = now.workbook.sheets[index];
		if (previous.name != current.name) return std::string("the workbook's sheets changed");
		std::vector<std::size_t> inserted = insertedBySheet[previous.name];
		std::sort(inserted.begin(), inserted.end());
		const auto isInserted = [&](std::size_t row) {
			return std::binary_search(inserted.begin(), inserted.end(), row);
		};
		const auto movedRow = [&](std::size_t zeroBased) { return xlsx_row_once_inserted(zeroBased + 1, inserted) - 1; };

		// Every cell of the sheet as it now stands, by its reference.
		std::map<std::string, const SheetRenderCell *> cellsNow;
		std::map<std::size_t, bool> hiddenNow;
		for (const SheetRenderRow &row : current.rows) {
			hiddenNow[row.index] = row.hidden;
			for (const SheetRenderCell &cell : row.cells) cellsNow[cell.ref] = &cell;
		}
		std::size_t proven = 0;
		for (const SheetRenderRow &row : previous.rows) {
			const std::size_t movedTo = movedRow(row.index);
			if (row.hidden == true && hiddenNow[movedTo] == false)
				return "row " + std::to_string(row.index + 1) + " of " + previous.name + " would no longer be hidden";
			for (const SheetRenderCell &cell : row.cells) {
				if (cell.kind == SheetCellKind::Empty && cell.formula.has_value() == false) continue;
				const std::string ref = column_letters(static_cast<long long>(cell.column) + 1) + std::to_string(movedTo + 1);
				const auto found = cellsNow.find(ref);
				if (found == cellsNow.end() || found->second->kind != cell.kind
						|| found->second->stored_value != cell.stored_value || found->second->raw_text != cell.raw_text)
					return "cell " + cell.ref + " of " + previous.name + " would not keep its value at " + ref;
				std::optional<std::string> expected = cell.formula;
				for (const auto &[sheetName, numbers] : insertedBySheet) {
					if (!expected) break;
					std::vector<std::size_t> ordered = numbers;
					std::sort(ordered.begin(), ordered.end());
					// Each run of new rows moves the formula as the one before it left it.
					std::size_t runStart = 0;
					while (runStart < ordered.size() && expected) {
						std::size_t runEnd = runStart;
						while (runEnd + 1 < ordered.size() && ordered[runEnd + 1] == ordered[runEnd] + 1) ++runEnd;
						expected = xlsx_formula_with_rows_inserted(*expected, previous.name, sheetName, ordered[runStart],
							runEnd - runStart + 1);
						runStart = runEnd + 1;
					}
				}
				if (found->second->formula != expected)
					return "the formula in " + cell.ref + " of " + previous.name + " would not read what it read";
				++proven;
			}
		}
		for (const SheetRenderRow &row : current.rows) {
			if (isInserted(row.index + 1) == true && row.hidden == true)
				return "a new row of " + previous.name + " would be hidden";
			for (const SheetRenderCell &cell : row.cells) {
				if (cell.kind == SheetCellKind::Empty && cell.formula.has_value() == false) continue;
				if (isInserted(row.index + 1) == false) continue;
				const auto expected = filledAt.find({previous.name, cell.ref});
				if (expected == filledAt.end() || cell.formula != expected->second || cell.stored_value.empty() == false)
					return "new row " + std::to_string(row.index + 1) + " of " + previous.name + " would not be empty";
				// A formula a table fills into its new row is its column's formula
				// in the row next to the new ones, moved to the new row - read here
				// from the workbook itself, not from what the insertion said.
				const auto formulaAt = [&](std::size_t number) -> std::optional<std::string> {
					const auto found = cellsNow.find(column_letters(static_cast<long long>(cell.column) + 1)
						+ std::to_string(number));
					if (found == cellsNow.end() || !found->second->formula) return std::nullopt;
					return found->second->formula;
				};
				std::size_t above = row.index + 1;
				while (above > 0 && isInserted(above) == true) --above;
				std::size_t below = row.index + 1;
				while (isInserted(below) == true) ++below;
				std::optional<std::string> derived;
				if (above > 0 && formulaAt(above)) {
					derived = "=" + xlsx_shared_formula_text(formulaAt(above)->substr(1),
						static_cast<long long>(row.index + 1) - static_cast<long long>(above), 0);
				} else if (formulaAt(below)) {
					derived = "=" + xlsx_shared_formula_text(formulaAt(below)->substr(1),
						static_cast<long long>(row.index + 1) - static_cast<long long>(below), 0);
				} else if (reads_rows_by_distance(cell.formula->substr(1), previous.name) == false) {
					derived = cell.formula;
				}
				if (derived != cell.formula)
					return "the formula in new row " + std::to_string(row.index + 1) + " of " + previous.name
						+ " would not read its own row";
				++proven;
			}
		}
		std::size_t held = 0;
		for (const SheetRenderRow &row : current.rows)
			for (const SheetRenderCell &cell : row.cells)
				if (cell.kind != SheetCellKind::Empty || cell.formula.has_value() == true) ++held;
		if (held != proven) return previous.name + " would hold cells it did not hold";

		// Merges move with their rows, and a merge the new row falls inside grows.
		std::set<std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>> mergesNow;
		for (const SheetRenderMerge &merge : current.merges)
			mergesNow.insert({merge.first_row, merge.first_column, merge.last_row, merge.last_column});
		if (mergesNow.size() != previous.merges.size()) return "the merged cells of " + previous.name + " would change";
		for (const SheetRenderMerge &merge : previous.merges)
			if (mergesNow.count({movedRow(merge.first_row), merge.first_column, movedRow(merge.last_row), merge.last_column}) == 0)
				return "the merged cells of " + previous.name + " would not move with their rows";
	}
	return std::nullopt;

}

// ---- Formulas Singularity writes -----------------------------------------------------------

namespace {

// Functions newer than Excel 2007: the file format stores them under "_xlfn.".
const std::set<std::string> &newer_functions() {
	static const std::set<std::string> names{
		"ACOT", "ACOTH", "AGGREGATE", "ARABIC", "ARRAYTOTEXT", "BASE", "BETA.DIST", "BETA.INV", "BINOM.DIST",
		"BINOM.DIST.RANGE", "BINOM.INV", "BITAND", "BITLSHIFT", "BITOR", "BITRSHIFT", "BITXOR", "CEILING.MATH",
		"CEILING.PRECISE", "CHISQ.DIST", "CHISQ.DIST.RT", "CHISQ.INV", "CHISQ.INV.RT", "CHISQ.TEST", "COMBINA",
		"CONCAT", "CONFIDENCE.NORM", "CONFIDENCE.T", "COT", "COTH", "COVARIANCE.P", "COVARIANCE.S", "CSC", "CSCH", "DAYS",
		"DECIMAL", "ECMA.CEILING", "ENCODEURL", "ERF.PRECISE", "ERFC.PRECISE", "EXPON.DIST", "F.DIST", "F.DIST.RT",
		"F.INV", "F.INV.RT", "F.TEST", "FILTERXML", "FLOOR.MATH", "FLOOR.PRECISE", "FORECAST.ETS", "FORECAST.ETS.CONFINT",
		"FORECAST.ETS.SEASONALITY", "FORECAST.ETS.STAT", "FORECAST.LINEAR", "FORMULATEXT", "GAMMA", "GAMMA.DIST",
		"GAMMA.INV", "GAMMALN.PRECISE", "GAUSS", "HYPGEOM.DIST", "IFNA", "IFS", "IMAGE", "IMCOSH", "IMCOT", "IMCSC",
		"IMCSCH", "IMSEC", "IMSECH", "IMSINH", "IMTAN", "ISFORMULA", "ISOMITTED", "ISOWEEKNUM", "LOGNORM.DIST",
		"LOGNORM.INV", "MAXIFS", "MINIFS", "MODE.MULT", "MODE.SNGL", "MUNIT", "NEGBINOM.DIST", "NETWORKDAYS.INTL",
		"NORM.DIST", "NORM.INV", "NORM.S.DIST", "NORM.S.INV", "NUMBERVALUE", "PDURATION", "PERCENTILE.EXC",
		"PERCENTILE.INC", "PERCENTOF", "PERCENTRANK.EXC", "PERCENTRANK.INC", "PERMUTATIONA", "PHI", "POISSON.DIST",
		"QUARTILE.EXC", "QUARTILE.INC", "RANK.AVG", "RANK.EQ", "REGEXEXTRACT", "REGEXREPLACE", "REGEXTEST", "RRI", "SEC",
		"SECH", "SHEET", "SHEETS", "SINGLE", "SKEW.P", "STDEV.P", "STDEV.S", "SWITCH", "T.DIST", "T.DIST.2T", "T.DIST.RT",
		"T.INV", "T.INV.2T", "T.TEST", "TEXTAFTER", "TEXTBEFORE", "TEXTJOIN", "UNICHAR", "UNICODE", "VALUETOTEXT", "VAR.P",
		"VAR.S", "WEBSERVICE", "WEIBULL.DIST", "WORKDAY.INTL", "XLOOKUP", "XMATCH", "XOR", "Z.TEST",
	};
	return names;
}

// Functions whose answer fills other cells than their own, or that name their
// own parameters: a workbook holds them only with metadata Singularity does not write.
const std::set<std::string> &spilling_functions() {
	static const std::set<std::string> names{
		"FILTER", "SORT", "SORTBY", "UNIQUE", "SEQUENCE", "RANDARRAY", "LET", "LAMBDA", "MAP", "REDUCE", "SCAN",
		"MAKEARRAY", "BYROW", "BYCOL", "TEXTSPLIT", "VSTACK", "HSTACK", "TOCOL", "TOROW", "WRAPROWS", "WRAPCOLS",
		"TAKE", "DROP", "CHOOSEROWS", "CHOOSECOLS", "EXPAND", "GROUPBY", "PIVOTBY", "TRIMRANGE", "STOCKHISTORY",
		"ANCHORARRAY",
	};
	return names;
}

std::string without_sign(const std::string &typed) {
	const std::string value = trimmed(typed);
	return value.empty() == false && value.front() == '=' ? value.substr(1) : value;
}

std::string function_name_of(const std::string &token) {
	std::string name = upper_ascii(token);
	for (const char *prefix : {"_XLFN.", "_XLWS."})
		if (name.rfind(prefix, 0) == 0) name = name.substr(std::strlen(prefix));
	return name;
}

// The end of the "text" or 'sheet name' opening at `index`, a doubled quote
// standing for one.
std::size_t quoted_end(const std::string &formula, std::size_t index) {
	const char quote = formula[index];
	std::size_t end = index + 1;
	while (end < formula.size()) {
		if (formula[end] == quote) {
			if (end + 1 < formula.size() && formula[end + 1] == quote) {
				end += 2;
				continue;
			}
			break;
		}
		++end;
	}
	return std::min(end + 1, formula.size());
}

// The end of the [group] opening at `index`, with the groups inside it.
std::size_t bracket_end(const std::string &formula, std::size_t index) {
	std::size_t end = index;
	int depth = 0;
	while (end < formula.size()) {
		if (formula[end] == '[') ++depth;
		if (formula[end] == ']') --depth;
		++end;
		if (depth == 0) break;
	}
	return end;
}

// A table's row as Excel shows it - Fees[@Amount] - written as the file format
// holds it: Fees[[#This Row],[Amount]]. Everything else stays as typed.
std::string with_table_rows_written_out(const std::string &formula) {
	std::string out;
	std::size_t index = 0;
	while (index < formula.size()) {
		const char character = formula[index];
		if (character == '"' || character == '\'') {
			const std::size_t end = quoted_end(formula, index);
			out.append(formula, index, end - index);
			index = end;
			continue;
		}
		if (character == '[') {
			const std::size_t end = bracket_end(formula, index);
			const std::string group = formula.substr(index, end - index);
			const bool followsName = index > 0 && is_name_character(formula[index - 1]);
			if (followsName == true && group.size() >= 3 && group[1] == '@' && group.back() == ']') {
				const std::string inner = group.substr(2, group.size() - 3);
				if (inner.empty() == true) out += "[#This Row]";
				else if (inner.front() == '[') out += "[[#This Row]," + inner + "]";
				else out += "[[#This Row],[" + inner + "]]";
			} else {
				out += group;
			}
			index = end;
			continue;
		}
		out.push_back(character);
		++index;
	}
	return out;
}

// What in a formula's shape a workbook cannot hold as typed, in words: "@"
// anywhere but a named table's row (Fees[@Amount]), a range another formula
// fills ("A2#"), or a range calculated with outside any function
// ("=B2:B5*C2:C5", "=Fees[Amount]") - which Excel opens as the value of one
// row of the range, not as the range.
std::optional<std::string> shape_refusal(const std::string &formula) {
	std::vector<bool> brackets;
	const auto inFunction = [&]() { return std::find(brackets.begin(), brackets.end(), true) != brackets.end(); };
	const std::string outside = "it calculates with a whole range outside any function, and Excel would show the "
		"value of one row only; put the range inside a function such as SUM";
	std::size_t index = 0;
	while (index < formula.size()) {
		const char character = formula[index];
		if (character == '"' || character == '\'') {
			index = quoted_end(formula, index);
			continue;
		}
		if (character == '{') {
			const std::size_t end = formula.find('}', index);
			index = end == std::string::npos ? formula.size() : end + 1;
			continue;
		}
		if (character == '[') {
			const std::size_t end = bracket_end(formula, index);
			const std::string group = upper_ascii(formula.substr(index, end - index));
			const bool followsName = index > 0 && is_name_character(formula[index - 1]);
			if (group.size() >= 2 && group[1] == '@' && followsName == false)
				return std::string("\"[@...]\" does not name its table; write it with the table's name, as "
					"Table[@Column]");
			const bool isOneRow = group.size() >= 2 && (group[1] == '@' || group.find("#THIS ROW") != std::string::npos
				|| group.find("#TOTALS") != std::string::npos);
			const bool isWorkbook = group.size() > 2 && std::all_of(group.begin() + 1, group.end() - 1,
				[](char digit) { return digit >= '0' && digit <= '9'; });
			if (followsName == true && isOneRow == false && isWorkbook == false && inFunction() == false) return outside;
			index = end;
			continue;
		}
		if (character == '@')
			return std::string("it uses \"@\", which Excel does not keep in the file as typed; name a table's row as "
				"Table[@Column], and leave \"@\" out before a range");
		if (character == '#') {
			if (index > 0 && (is_name_character(formula[index - 1]) == true || formula[index - 1] == ')'))
				return std::string("it reads with \"#\" the cells another formula fills, which Singularity does not write");
			++index;
			while (index < formula.size() && (std::isalnum(static_cast<unsigned char>(formula[index])) != 0
					|| formula[index] == '/' || formula[index] == '!' || formula[index] == '?' || formula[index] == '_'))
				++index;
			continue;
		}
		if (character == '(') {
			brackets.push_back(index > 0 && is_name_character(formula[index - 1]) == true);
			++index;
			continue;
		}
		if (character == ')') {
			if (brackets.empty() == false) brackets.pop_back();
			++index;
			continue;
		}
		if (is_name_character(character) == false) {
			++index;
			continue;
		}
		std::size_t end = index;
		while (end < formula.size() && is_name_character(formula[end]) == true) ++end;
		if (end < formula.size() && formula[end] == ':') {
			std::size_t secondEnd = end + 1;
			while (secondEnd < formula.size() && is_name_character(formula[secondEnd]) == true) ++secondEnd;
			const char afterSecond = secondEnd < formula.size() ? formula[secondEnd] : '\0';
			ReferencePart first;
			ReferencePart last;
			const bool isRange = afterSecond != '!' && afterSecond != '('
				&& parse_reference_part(formula.substr(index, end - index), first) == true
				&& parse_reference_part(formula.substr(end + 1, secondEnd - end - 1), last) == true
				&& first.has_column == last.has_column && first.has_row == last.has_row;
			if (isRange == true && inFunction() == false) return outside;
			if (isRange == true) {
				index = secondEnd;
				continue;
			}
		}
		index = end;
	}
	return std::nullopt;
}

} // namespace

bool is_xlsx_formula(const std::string &value) {
	const std::string text = trimmed(value);
	return text.size() > 1 && text.front() == '=';
}

std::optional<std::string> xlsx_formula_refusal(const std::string &typed, const std::string &sheet,
		const std::string &cellRef, const std::vector<std::string> &sheetNames) {

	const std::string formula = without_sign(typed);
	const std::string words = "Singularity does not write this formula into " + cellRef + ": ";
	if (trimmed(formula).empty() == true || formula.front() == '=') return words + "it has nothing to calculate";
	if (formula.size() > 8192) return words + "it is longer than Excel holds";

	// Whole: every quote closed, every bracket closed in its order.
	int depth = 0;
	bool inText = false;
	bool inName = false;
	for (const char character : formula) {
		if (inText == true) {
			if (character == '"') inText = false;
			continue;
		}
		if (inName == true) {
			if (character == '\'') inName = false;
			continue;
		}
		if (character == '"') inText = true;
		else if (character == '\'') inName = true;
		else if (character == '(') ++depth;
		else if (character == ')' && --depth < 0) break;
	}
	if (inText == true || inName == true || depth != 0) return words + "its brackets or quotes are not closed";
	const std::optional<std::string> shape = shape_refusal(formula);
	if (shape) return words + *shape;

	std::set<std::string> known;
	for (const std::string &name : sheetNames) known.insert(upper_ascii(name));
	ReferencePart own;
	if (parse_reference_part(cellRef, own) == false || own.has_row == false || own.has_column == false)
		return words + "the cell is not a cell of a sheet";
	std::optional<std::string> refusal;
	std::vector<std::pair<ReferencePart, bool>> ownSheetEnds;
	const auto readReference = [&](const std::string &referenced, bool isExternal, ReferencePart &part, bool isRangeEnd) {
		if (isExternal == true) {
			refusal = words + "it reads another workbook";
			return false;
		}
		if (known.count(referenced) == 0) {
			refusal = words + "it reads a sheet the workbook does not have";
			return false;
		}
		if (referenced == upper_ascii(sheet)) ownSheetEnds.emplace_back(part, isRangeEnd);
		return true;
	};
	const auto readFunction = [&](const std::string &token) {
		if (spilling_functions().count(function_name_of(token)) > 0 && !refusal)
			refusal = words + upper_ascii(function_name_of(token)) + " fills other cells than its own";
		return token;
	};
	const std::optional<std::string> walked = walk_formula(formula, sheet, readReference, readFunction);
	if (refusal) return refusal;
	if (!walked) return words + "it reads the same cells of several sheets at once";
	// A formula that reads its own cell never finishes.
	for (std::size_t index = 0; index < ownSheetEnds.size(); ++index) {
		const ReferencePart &first = ownSheetEnds[index].first;
		const bool isRange = index + 1 < ownSheetEnds.size() && ownSheetEnds[index + 1].second == true;
		const ReferencePart &last = isRange == true ? ownSheetEnds[index + 1].first : first;
		if (isRange == true) ++index;
		const long long top = first.has_row == true ? std::min(first.row, last.row) : 1;
		const long long bottom = first.has_row == true ? std::max(first.row, last.row) : kMaximumRows;
		const long long left = first.has_column == true ? std::min(first.column, last.column) : 1;
		const long long right = first.has_column == true ? std::max(first.column, last.column) : kMaximumColumns;
		if (own.row >= top && own.row <= bottom && own.column >= left && own.column <= right)
			return words + "it reads its own cell";
	}
	return std::nullopt;

}

std::string xlsx_formula_stored(const std::string &typed) {
	const std::string formula = with_table_rows_written_out(without_sign(typed));
	const auto identity = [](const std::string &, bool, ReferencePart &, bool) { return true; };
	const auto named = [](const std::string &token) {
		const std::string name = function_name_of(token);
		return newer_functions().count(name) > 0 ? "_xlfn." + name : name;
	};
	const std::optional<std::string> stored = walk_formula(formula, std::string("-"), identity, named);
	return stored ? *stored : formula;
}

std::string xlsx_formula_shown(const std::string &typed) {
	const std::string formula = without_sign(typed);
	const auto identity = [](const std::string &, bool, ReferencePart &, bool) { return true; };
	// From the formula as typed, so a table's row reads as Excel shows it (Fees[@Amount]).
	const std::optional<std::string> shown = walk_formula(formula, std::string("-"), identity, function_name_of);
	return "=" + (shown ? *shown : formula);
}


// ---- Rows deleted ---------------------------------------------------------------------

namespace {

// `count` rows deleted from row `at` of the sheet named `name` (`sheet` is the
// same name upper-cased, as references are compared).
struct Deletion {
	std::string sheet;
	std::string name;
	long long at = 0;
	long long count = 0;

	long long last() const {
		return at + count - 1;
	}

	bool covers(long long row) const {
		return row >= at && row <= last();
	}

	long long moved(long long row) const {
		return row > last() ? row - count : row;
	}
};

// What a formula read of the deleted rows: whether a reference of it now finds
// nothing (#REF!), whether any reached into them at all - a range that shrank
// adds up fewer cells, so what it last calculated no longer holds - and which
// deleted rows, of which sheet, the references it lost read.
// A formula reads the deleted rows too through a whole column of their sheet:
// what it adds up loses them without any reference of it changing.
struct DeletedReading {
	bool is_lost = false;
	bool reads_deleted = false;
	// The sheets whose deleted rows its lost references read, in the order it
	// reads them, each with those rows.
	std::vector<std::pair<std::string, std::set<long long>>> rows;
};

void note_lost(DeletedReading *reading, const Deletion &deletion, long long first, long long last) {
	if (reading == nullptr) return;
	reading->is_lost = true;
	reading->reads_deleted = true;
	auto sheet = std::find_if(reading->rows.begin(), reading->rows.end(),
		[&](const std::pair<std::string, std::set<long long>> &entry) { return entry.first == deletion.name; });
	if (sheet == reading->rows.end()) sheet = reading->rows.insert(reading->rows.end(), {deletion.name, {}});
	for (long long row = first; row <= last && sheet->second.size() < 64; ++row) sheet->second.insert(row);
}

// A single cell's reference as Excel's Delete Row leaves it: moved up when it
// stands below the deleted rows, #REF! when it stood in them.
ReferenceVisitor deleting_cells(const Deletion &deletion, DeletedReading *reading) {
	return [&deletion, reading](const std::string &sheet, bool isExternal, ReferencePart &part, bool) {
		if (isExternal == true || part.has_row == false) return true;
		if (sheet.empty() == true) return false;
		if (sheet != deletion.sheet) return true;
		if (deletion.covers(part.row) == true) {
			note_lost(reading, deletion, part.row, part.row);
			part.row = 0;
			return true;
		}
		part.row = deletion.moved(part.row);
		return true;
	};
}

// A range as Excel's Delete Row leaves it: moved up below the deleted rows,
// shrunk by the rows it loses, #REF! when every row of it goes. A range that
// runs to the sheet's last row still runs there.
RangeVisitor deleting_ranges(const Deletion &deletion, DeletedReading *reading) {
	return [&deletion, reading](const std::string &sheet, bool isExternal, ReferencePart &first, ReferencePart &last) {
		if (isExternal == true) return true;
		if (first.has_row == false || last.has_row == false) {
			// "B:B" of the sheet: every row of it, the deleted ones among them.
			if (reading != nullptr && first.has_row == false && last.has_row == false && sheet == deletion.sheet)
				reading->reads_deleted = true;
			return true;
		}
		if (sheet.empty() == true) return false;
		if (sheet != deletion.sheet) return true;
		ReferencePart &top = first.row <= last.row ? first : last;
		ReferencePart &bottom = first.row <= last.row ? last : first;
		if (bottom.row < deletion.at) return true;
		if (top.row > deletion.last()) {
			top.row -= deletion.count;
			if (bottom.row != kMaximumRows) bottom.row -= deletion.count;
			return true;
		}
		if (reading != nullptr) reading->reads_deleted = true;
		if (top.row >= deletion.at && bottom.row <= deletion.last()) {
			note_lost(reading, deletion, top.row, bottom.row);
			top.row = 0;
			bottom.row = 0;
			return true;
		}
		if (top.row >= deletion.at) top.row = deletion.at;
		if (bottom.row != kMaximumRows) bottom.row = bottom.row > deletion.last() ? bottom.row - deletion.count : deletion.at - 1;
		return true;
	};
}

std::optional<std::string> formula_after_deletion(const std::string &formula, const std::string &contextSheet,
		const Deletion &deletion, DeletedReading *reading) {
	return walk_formula(formula, contextSheet, deleting_cells(deletion, reading), nullptr,
		deleting_ranges(deletion, reading));
}

// A formula once every run of rows is deleted - each run of each sheet, the
// run furthest down a sheet first, so every run is numbered as the sheet
// stands now - with what it read of them. Nothing when a run cannot place it.
std::optional<std::string> formula_after_runs(const std::string &formula, const std::string &contextSheet,
		const std::vector<Deletion> &runs, DeletedReading *reading) {
	std::string text = formula;
	for (const Deletion &run : runs) {
		const std::optional<std::string> moved = formula_after_deletion(text, contextSheet, run, reading);
		if (!moved) return std::nullopt;
		text = *moved;
	}
	return text;
}

// "A1:B5 D7" - a sheet's own list of ranges - once its runs of rows are
// deleted; a range every row of which goes leaves the list. `runs` are the
// runs of that sheet alone.
std::string ranges_after_runs(const std::string &sqref, const std::vector<Deletion> &runs) {
	std::string current = sqref;
	for (const Deletion &run : runs) {
		std::istringstream stream(current);
		std::string item;
		std::string out;
		while (stream >> item) {
			const std::optional<std::string> moved = formula_after_deletion(item, run.sheet, run, nullptr);
			if (moved && moved->find("#REF!") != std::string::npos) continue;
			if (out.empty() == false) out.push_back(' ');
			out += moved ? *moved : item;
		}
		current = out;
	}
	return current;
}

// A row's number once the runs of its sheet are deleted; 0 when it goes.
long long row_after_runs(long long row, const std::vector<Deletion> &runs) {
	long long moved = row;
	for (const Deletion &run : runs) {
		if (run.covers(moved) == true) return 0;
		moved = run.moved(moved);
	}
	return moved;
}

// A cell a view names - the active cell, the top-left cell in sight - kept on
// the sheet: one that stood in deleted rows becomes the row that takes their
// place.
std::string view_cell_after_runs(const std::string &ref, const std::vector<Deletion> &runs) {
	ReferencePart part;
	if (parse_reference_part(ref, part) == false || part.has_row == false) return ref;
	for (const Deletion &run : runs) part.row = run.covers(part.row) == true ? run.at : run.moved(part.row);
	const std::string written = written_part(part);
	return written.empty() == true ? ref : written;
}

// A rule's formula - a conditional format's, a validation's - once the rows
// are gone. It is written for the rule's first cell and read by each of its
// cells at that cell's distance: where deleted rows fall among the rule's own
// rows, it keeps its meaning only when each of its rows reads its own row, and
// - where the rule's first row goes with rows above it - only when it reads no
// row by distance at all. `isExact` says whether it does.
std::optional<std::string> rule_formula_after_deletion(const std::string &formula, const std::string &sheetOfRule,
		const std::string &sqref, const Deletion &deletion, DeletedReading *reading, bool &isExact) {
	const std::vector<std::pair<long long, long long>> rows = rows_of_ranges(sqref);
	const bool partsRule = upper_ascii(sheetOfRule) == deletion.sheet
		&& std::any_of(rows.begin(), rows.end(), [&](const std::pair<long long, long long> &range) {
			return range.first <= deletion.last() && range.second >= deletion.at;
		});
	if (partsRule == false) return formula_after_deletion(formula, sheetOfRule, deletion, reading);
	const long long anchor = rows.empty() == true ? 1 : rows.front().first;
	const bool anchorMoves = deletion.covers(anchor) == true && deletion.at < anchor;
	const ReferenceVisitor cells = deleting_cells(deletion, reading);
	const RangeVisitor ranges = deleting_ranges(deletion, reading);
	const auto cellVisitor = [&](const std::string &sheet, bool isExternal, ReferencePart &part, bool isRangeEnd) {
		if (isExternal == true || part.has_row == false) return true;
		if (part.is_row_absolute == false) {
			if (part.row != anchor || anchorMoves == true) isExact = false;
			return true;
		}
		return cells(sheet, isExternal, part, isRangeEnd);
	};
	const auto rangeVisitor = [&](const std::string &sheet, bool isExternal, ReferencePart &first, ReferencePart &last) {
		if (isExternal == true || first.has_row == false || last.has_row == false) return true;
		if (first.is_row_absolute == false || last.is_row_absolute == false) {
			if (first.is_row_absolute != last.is_row_absolute || first.row != anchor || last.row != anchor
					|| anchorMoves == true)
				isExact = false;
			return true;
		}
		return ranges(sheet, isExternal, first, last);
	};
	return walk_formula(formula, sheetOfRule, cellVisitor, nullptr, rangeVisitor);
}

// A rule once every run is deleted: its formula (nothing where a run cannot
// place it) and, through `sqref`, its own ranges - which move with the runs of
// its own sheet alone.
std::optional<std::string> rule_after_runs(const std::string &formula, const std::string &sheetOfRule,
		std::string sqref, const std::vector<Deletion> &runs, DeletedReading *reading, bool &isExact) {
	std::string text = formula;
	for (const Deletion &run : runs) {
		const std::optional<std::string> moved = rule_formula_after_deletion(text, sheetOfRule, sqref, run, reading,
			isExact);
		if (!moved) return std::nullopt;
		text = *moved;
		if (upper_ascii(sheetOfRule) == run.sheet) sqref = ranges_after_runs(sqref, {run});
	}
	return text;
}

// Every run of every sheet, from `rows` numbered as the sheets stand now: the
// runs of one sheet furthest down first, so a run deleted leaves every row
// above it - and every run still to be deleted - where it stands.
std::vector<Deletion> deletion_runs(const std::vector<XlsxRowDeletion> &rows, const std::vector<Worksheet> &sheets) {
	std::map<std::string, std::vector<long long>> numbersBySheet;
	for (const XlsxRowDeletion &row : rows) {
		const std::string name = row.sheet.empty() == true ? sheets.front().name : row.sheet;
		const bool known = std::any_of(sheets.begin(), sheets.end(), [&](const Worksheet &sheet) { return sheet.name == name; });
		if (known == false)
			throw XlsxRowDeletionRefused("rows cannot be deleted: the workbook has no sheet named " + sheet_words(name));
		if (row.row < 1 || row.row > kXlsxMaximumRows)
			throw XlsxRowDeletionRefused("rows cannot be deleted from " + sheet_words(name) + ": row "
				+ std::to_string(row.row) + " is not a row of a sheet");
		std::vector<long long> &numbers = numbersBySheet[name];
		if (std::find(numbers.begin(), numbers.end(), static_cast<long long>(row.row)) != numbers.end())
			throw XlsxRowDeletionRefused("rows cannot be deleted from " + sheet_words(name) + ": row "
				+ std::to_string(row.row) + " is named twice");
		numbers.push_back(static_cast<long long>(row.row));
	}
	std::vector<Deletion> runs;
	for (std::pair<const std::string, std::vector<long long>> &entry : numbersBySheet) {
		std::vector<long long> &numbers = entry.second;
		std::sort(numbers.rbegin(), numbers.rend());
		const std::size_t first = runs.size();
		for (const long long number : numbers) {
			if (runs.size() > first && runs.back().at - 1 == number) {
				--runs.back().at;
				++runs.back().count;
			} else {
				runs.push_back({upper_ascii(entry.first), entry.first, number, 1});
			}
		}
	}
	return runs;
}

std::vector<Deletion> runs_of_sheet(const std::vector<Deletion> &runs, const std::string &name) {
	std::vector<Deletion> own;
	for (const Deletion &run : runs) if (run.name == name) own.push_back(run);
	return own;
}

// The workbook as the package declares it: its part, its relationships, its
// worksheets by the names on their tabs, and the name of every sheet by its
// place among them (chart sheets too - a defined name counts them).
struct OpenedWorkbook {
	std::string part;
	std::vector<Relationship> relationships;
	std::vector<Worksheet> sheets;
	std::vector<std::string> names_by_index;
};

OpenedWorkbook open_workbook(Package &package) {
	OpenedWorkbook opened;
	opened.part = "xl/workbook.xml";
	for (const Relationship &relationship : package.relationships(""))
		if (type_ends_with(relationship, "/officeDocument") == true) opened.part = relationship.target;
	pugi::xml_document &workbook = package.xml(opened.part);
	opened.relationships = package.relationships(opened.part);
	std::size_t index = 0;
	for_each_element(workbook, [&](pugi::xml_node node) {
		if (is_named(node, "sheet") == false) return;
		const std::string name = node.attribute("name").as_string();
		opened.names_by_index.push_back(name);
		const std::string id = attribute_named(node, "id").as_string();
		for (const Relationship &relationship : opened.relationships)
			if (relationship.id == id && type_ends_with(relationship, "/worksheet") == true)
				opened.sheets.push_back({name, relationship.target, index});
		++index;
	});
	return opened;
}

pugi::xml_node worksheet_root(pugi::xml_document &sheet) {
	for (pugi::xml_node child : sheet.children()) if (is_named(child, "worksheet") == true) return child;
	return pugi::xml_node();
}

// Whether a 0-based anchor row of a picture or note stands on a deleted row.
bool anchor_row_deleted(long long index, const std::vector<Deletion> &runs) {
	return row_after_runs(index + 1, runs) == 0;
}

// What stands on the rows and cannot go with them, refused before anything
// moves: a sheet's own content Singularity does not take out, and the things of
// the workbook that would stop working.
void refuse_what_cannot_be_deleted(Package &package, const Worksheet &target, const std::vector<Deletion> &own,
		const OpenedWorkbook &opened) {
	const std::string words = sheet_words(target.name);
	const std::string refused = "rows cannot be deleted from " + words + ": ";
	const std::vector<Relationship> sheetRelationships = package.relationships(target.part);
	for (const Relationship &relationship : sheetRelationships) {
		if (type_ends_with(relationship, "/pivotTable") == true) throw XlsxRowDeletionRefused(refused + "it holds a pivot table");
		static const char *known[] = {"/drawing", "/vmlDrawing", "/comments", "/threadedComment", "/table",
			"/hyperlink", "/printerSettings", "/image"};
		const bool isKnown = std::any_of(std::begin(known), std::end(known),
			[&](const char *suffix) { return type_ends_with(relationship, suffix); });
		if (isKnown == false) throw XlsxRowDeletionRefused(refused + "it holds content Singularity does not take out with rows");
		if (relationship.is_external == true || package.has(relationship.target) == false) continue;
		if (type_ends_with(relationship, "/table") == true) {
			for (const Relationship &tableRelationship : package.relationships(relationship.target))
				if (type_ends_with(tableRelationship, "/queryTable") == true)
					throw XlsxRowDeletionRefused(refused + "a table on it is filled by a data query");
			pugi::xml_node root;
			for (pugi::xml_node child : package.xml(relationship.target).children())
				if (is_named(child, "table") == true) root = child;
			if (!root) continue;
			const std::string name = root.attribute("displayName").as_string();
			const std::vector<std::pair<long long, long long>> spans = rows_of_ranges(root.attribute("ref").as_string());
			if (spans.empty() == true) throw XlsxRowDeletionRefused(refused + "a table on it does not say where it stands");
			const long long headerRows = root.attribute("headerRowCount") ? root.attribute("headerRowCount").as_llong() : 1;
			const long long totalRows = root.attribute("totalsRowCount").as_llong();
			const long long firstData = spans.front().first + headerRows;
			const long long lastData = spans.front().second - totalRows;
			long long deletedData = 0;
			for (const Deletion &run : own) {
				for (long long row = spans.front().first; row < firstData; ++row)
					if (run.covers(row) == true)
						throw XlsxRowDeletionRefused(refused + "row " + std::to_string(row) + " holds the headings of the "
							"table " + sheet_words(name));
				for (long long row = lastData + 1; row <= spans.front().second; ++row)
					if (run.covers(row) == true)
						throw XlsxRowDeletionRefused(refused + "row " + std::to_string(row) + " holds the totals of the "
							"table " + sheet_words(name));
				deletedData += std::max(0LL, std::min(run.last(), lastData) - std::max(run.at, firstData) + 1);
			}
			if (deletedData > 0 && deletedData >= lastData - firstData + 1)
				throw XlsxRowDeletionRefused(refused + "every row of the table " + sheet_words(name) + " would go; a "
					"table keeps at least one row");
		} else if (type_ends_with(relationship, "/drawing") == true) {
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if (name != "from" && name != "to") return;
				const pugi::xml_node row = first_child_named(node, "row");
				if (row && anchor_row_deleted(std::atoll(row.child_value()), own) == true)
					throw XlsxRowDeletionRefused(refused + "a picture, chart or shape stands on a row being deleted; move "
						"it first");
			});
		} else if (type_ends_with(relationship, "/vmlDrawing") == true) {
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				if (local_name(node) != "Row") return;
				if (anchor_row_deleted(std::atoll(node.child_value()), own) == true)
					throw XlsxRowDeletionRefused(refused + "a note or a control stands on a row being deleted");
			});
		} else if (type_ends_with(relationship, "/comments") == true || type_ends_with(relationship, "/threadedComment") == true) {
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if ((name != "comment" && name != "threadedComment") || !node.attribute("ref")) return;
				ReferencePart place;
				if (parse_reference_part(node.attribute("ref").as_string(), place) == true && place.has_row == true
						&& row_after_runs(place.row, own) == 0)
					throw XlsxRowDeletionRefused(refused + "a note on cell " + std::string(node.attribute("ref").as_string())
						+ " stands on a row being deleted; delete or move the note first");
			});
		}
	}

	pugi::xml_node worksheet = worksheet_root(package.xml(target.part));
	if (!worksheet || !first_child_named(worksheet, "sheetData")) throw std::runtime_error("The XLSX worksheet has no sheetData element.");
	const pugi::xml_node protection = first_child_named(worksheet, "sheetProtection");
	if (protection && protection.attribute("sheet").as_bool() == true
			&& (!protection.attribute("deleteRows") || protection.attribute("deleteRows").as_bool() == true))
		throw XlsxRowDeletionRefused(refused + "the sheet is protected against deleted rows");
	for_each_element(worksheet, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if (name == "pane") {
			const std::string state = node.attribute("state").as_string();
			const long long frozen = state == "frozen" || state == "frozenSplit" ? node.attribute("ySplit").as_llong() : 0;
			for (const Deletion &run : own)
				if (run.at <= frozen)
					throw XlsxRowDeletionRefused(refused + "row " + std::to_string(run.at) + " is part of the heading the "
						"sheet keeps frozen at its top");
			return;
		}
		if (name == "f" && is_named(node.parent(), "c") == true) {
			const std::string kind = node.attribute("t").as_string();
			if (kind == "dataTable") throw XlsxRowDeletionRefused(refused + "it holds a what-if data table");
			if (kind != "array" || !node.attribute("ref")) return;
			const std::vector<std::pair<long long, long long>> covered = rows_of_ranges(node.attribute("ref").as_string());
			if (covered.empty() == true) return;
			for (const Deletion &run : own) {
				const bool touches = covered.front().first <= run.last() && covered.front().second >= run.at;
				const bool whole = covered.front().first >= run.at && covered.front().second <= run.last();
				if (touches == true && whole == false)
					throw XlsxRowDeletionRefused(refused + "the rows cut through the array formula over "
						+ std::string(node.attribute("ref").as_string()) + ", which Excel keeps whole");
			}
			return;
		}
		// A filter keeps its heading row: its rows may go, its heading may not.
		if (name == "autoFilter" && is_named(node.parent(), "worksheet") == true && node.attribute("ref")) {
			const std::vector<std::pair<long long, long long>> spans = rows_of_ranges(node.attribute("ref").as_string());
			if (spans.empty() == false && row_after_runs(spans.front().first, own) == 0)
				throw XlsxRowDeletionRefused(refused + "row " + std::to_string(spans.front().first) + " holds the headings "
					"of the sheet's filter");
			return;
		}
		// A sparkline standing on a deleted row would have nowhere to stand.
		if (name == "sparkline") {
			const pugi::xml_node where = first_child_named(node, "sqref");
			if (where && ranges_after_runs(where.child_value(), own).empty() == true)
				throw XlsxRowDeletionRefused(refused + "a sparkline stands on a row being deleted");
			return;
		}
		if ((name == "from" || name == "to") && first_child_named(node, "row")
				&& anchor_row_deleted(std::atoll(first_child_named(node, "row").child_value()), own) == true)
			throw XlsxRowDeletionRefused(refused + "a picture or a control stands on a row being deleted; move it first");
	});

	// The data a pivot table is built from must still be there.
	for (const Relationship &relationship : opened.relationships) {
		if (type_ends_with(relationship, "/pivotCacheDefinition") == false || package.has(relationship.target) == false)
			continue;
		for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
			if (is_named(node, "worksheetSource") == false || !node.attribute("ref")) return;
			if (upper_ascii(node.attribute("sheet").as_string()) != upper_ascii(target.name)) return;
			if (ranges_after_runs(node.attribute("ref").as_string(), own).empty() == true)
				throw XlsxRowDeletionRefused(refused + "a pivot table is built from exactly these rows");
		});
	}
}

// The place a lost reference is named by, and what kind of thing holds it.
XlsxLostReference lost_reference(const std::string &kind, const std::string &sheet, const std::string &place,
		const std::string &formula, const std::string &after, const DeletedReading &reading) {
	XlsxLostReference lost;
	lost.kind = kind;
	lost.sheet = sheet;
	lost.place = place;
	lost.formula = xlsx_formula_shown(formula);
	lost.formula_after = xlsx_formula_shown(after);
	for (const auto &[rowsSheet, rows] : reading.rows) {
		XlsxLostRows lostRows;
		lostRows.sheet = rowsSheet;
		for (const long long row : rows) lostRows.rows.push_back(static_cast<std::size_t>(row));
		if (lost.rows_sheet.empty() == true) {
			lost.rows_sheet = lostRows.sheet;
			lost.rows = lostRows.rows;
		} else {
			lost.more_rows.push_back(std::move(lostRows));
		}
	}
	return lost;
}

// ---- What else the deletion changes ------------------------------------------------
//
// A formula's value can follow the deleted rows without any A1 reference of it
// reaching them: through a defined name or a table that loses rows, a function
// that decides at run time what it reads (INDIRECT, OFFSET), or one that
// counts rows by where they stand (ROW, ROWS, ADDRESS, CELL). Such a formula
// is worked out again too - never shown with the value it was saved with.

// The defined names and tables (upper-cased) whose cells the deletion changes.
struct DeletionDependence {
	std::set<std::string> names;
};

// What a formula reads besides its A1 references: the functions it calls and
// the other words it holds (names, tables), upper-cased. False when it cannot
// be read.
bool words_of_formula(const std::string &formula, const std::string &contextSheet, std::set<std::string> &functions,
		std::set<std::string> &words) {
	const ReferenceVisitor any = [](const std::string &, bool, ReferencePart &, bool) { return true; };
	const FunctionVisitor function = [&functions](const std::string &name) {
		std::string upper = upper_ascii(name);
		if (upper.rfind("_XLFN.", 0) == 0) upper = upper.substr(6);
		functions.insert(upper);
		return name;
	};
	const NameVisitor word = [&words](const std::string &token) { words.insert(upper_ascii(token)); };
	return walk_formula(formula, contextSheet.empty() == true ? std::string("-") : contextSheet, any, function, nullptr,
		word).has_value();
}

// The formula without the cells a table's row gives it (Fees[[#This Row],
// [Amount]]): its own row's cells, which move with it and hold what they held.
std::string without_table_rows(const std::string &formula) {
	std::string out;
	std::size_t index = 0;
	while (index < formula.size()) {
		const char character = formula[index];
		if (character == '"' || character == '\'') {
			const std::size_t end = quoted_end(formula, index);
			out.append(formula, index, end - index);
			index = end;
			continue;
		}
		if (character == '[') {
			const std::size_t end = bracket_end(formula, index);
			const std::string group = upper_ascii(formula.substr(index, end - index));
			const bool followsName = index > 0 && is_name_character(formula[index - 1]);
			const bool isRow = group.rfind("[#THIS ROW]", 0) == 0 || group.rfind("[[#THIS ROW]", 0) == 0
				|| group.rfind("[@", 0) == 0;
			if (followsName == true && isRow == true) {
				while (out.empty() == false && is_name_character(out.back()) == true) out.pop_back();
				out += "0";
			} else {
				out.append(formula, index, end - index);
			}
			index = end;
			continue;
		}
		out.push_back(character);
		++index;
	}
	return out;
}

// Whether a formula's value follows the deletion other than through an A1
// reference that reaches the deleted rows. `isShifted`: the formula moves, or
// a reference of it does.
bool follows_deletion(const std::string &formula, const std::string &contextSheet, bool isShifted,
		const DeletionDependence &dependence) {
	std::set<std::string> functions;
	std::set<std::string> words;
	if (words_of_formula(without_table_rows(formula), contextSheet, functions, words) == false) return true;
	for (const char *reader : {"INDIRECT", "OFFSET"})
		if (functions.count(reader) > 0) return true;
	if (isShifted == true)
		for (const char *counter : {"ROW", "ROWS", "ADDRESS", "CELL"})
			if (functions.count(counter) > 0) return true;
	for (const std::string &word : words)
		if (dependence.names.count(word) > 0) return true;
	return false;
}

// What the deletion does to every formula of the workbook, found before
// anything changes: the formulas that lose what they read (by the place they
// stand in now), and the formula cells that read a deleted row (by the place
// they stand in once the rows are gone). Refuses a format rule or validation
// whose rows would read other rows once the rows are gone.
// The tables that lose rows or move - a table on a sheet rows are deleted
// from, reaching down to the first of them - and the defined names whose cells
// follow the deletion, through a reference, a table, a function or another
// such name.
DeletionDependence dependence_of(Package &package, const OpenedWorkbook &opened, const std::vector<Deletion> &runs) {
	DeletionDependence dependence;
	for (const Worksheet &sheet : opened.sheets) {
		const std::vector<Deletion> own = runs_of_sheet(runs, sheet.name);
		if (own.empty() == true) continue;
		long long firstDeleted = kMaximumRows;
		for (const Deletion &run : own) firstDeleted = std::min(firstDeleted, run.at);
		for (const Relationship &relationship : package.relationships(sheet.part)) {
			if (type_ends_with(relationship, "/table") == false || relationship.is_external == true
					|| package.has(relationship.target) == false)
				continue;
			for (pugi::xml_node child : package.xml(relationship.target).children()) {
				if (is_named(child, "table") == false) continue;
				const std::vector<std::pair<long long, long long>> spans = rows_of_ranges(child.attribute("ref").as_string());
				const bool reaches = spans.empty() == true || std::any_of(spans.begin(), spans.end(),
					[&](const std::pair<long long, long long> &span) { return span.second >= firstDeleted; });
				if (reaches == false) continue;
				for (const char *attribute : {"displayName", "name"})
					if (child.attribute(attribute)) dependence.names.insert(upper_ascii(child.attribute(attribute).as_string()));
			}
		}
	}
	struct Name {
		std::string name;
		std::string text;
		std::string context;
	};
	std::vector<Name> names;
	for_each_element(package.xml(opened.part), [&](pugi::xml_node node) {
		if (is_named(node, "definedName") == false) return;
		Name name{upper_ascii(node.attribute("name").as_string()), node.child_value(), std::string()};
		if (node.attribute("localSheetId")) {
			const std::size_t index = static_cast<std::size_t>(node.attribute("localSheetId").as_ullong());
			if (index < opened.names_by_index.size()) name.context = opened.names_by_index[index];
		}
		names.push_back(std::move(name));
	});
	// A name that reads another such name follows it: until none is added.
	bool isAdded = true;
	while (isAdded == true) {
		isAdded = false;
		for (const Name &name : names) {
			if (name.name.empty() == true || dependence.names.count(name.name) > 0) continue;
			DeletedReading reading;
			const std::optional<std::string> after = formula_after_runs(name.text, name.context, runs, &reading);
			if (!after || reading.reads_deleted == true
					|| follows_deletion(name.text, name.context, *after != name.text, dependence) == true) {
				dependence.names.insert(name.name);
				isAdded = true;
			}
		}
	}
	return dependence;
}

XlsxRowDeletionResult read_deletion(Package &package, const OpenedWorkbook &opened, const std::vector<Deletion> &runs) {
	XlsxRowDeletionResult result;
	const DeletionDependence dependence = dependence_of(package, opened, runs);
	for (const Worksheet &sheet : opened.sheets) {
		const std::vector<Deletion> own = runs_of_sheet(runs, sheet.name);
		pugi::xml_node worksheet = worksheet_root(package.xml(sheet.part));
		if (!worksheet) continue;
		for_each_element(worksheet, [&](pugi::xml_node node) {
			const std::string name = local_name(node);
			if (name == "f" && is_named(node.parent(), "c") == true) {
				const std::string text = node.child_value();
				if (text.empty() == true) return;
				const std::string ref = node.parent().attribute("r").as_string();
				ReferencePart place;
				if (parse_reference_part(ref, place) == false || place.has_row == false) return;
				// A formula standing on a deleted row goes with it.
				const long long rowAfter = row_after_runs(place.row, own);
				if (rowAfter == 0) return;
				DeletedReading reading;
				const std::optional<std::string> after = formula_after_runs(text, sheet.name, runs, &reading);
				if (!after)
					throw XlsxRowDeletionRefused("rows cannot be deleted: the formula in " + ref + " of "
						+ sheet_words(sheet.name) + " reads the same cells of several sheets at once, or a range Singularity "
						"cannot place");
				if (reading.is_lost == true) result.lost.push_back(lost_reference("cell", sheet.name, ref, text, *after, reading));
				if (reading.reads_deleted == true
						|| follows_deletion(text, sheet.name, rowAfter != place.row || *after != text, dependence) == true) {
					place.row = rowAfter;
					result.recalculated.push_back({sheet.name, written_part(place)});
				}
				return;
			}
			// A sparkline's source: a small chart in a cell, drawn from its rows.
			if (name == "f" && is_named(node.parent(), "sparkline") == true) {
				const std::string text = node.child_value();
				if (text.empty() == true) return;
				DeletedReading reading;
				const std::optional<std::string> after = formula_after_runs(text, sheet.name, runs, &reading);
				if (!after)
					throw XlsxRowDeletionRefused("rows cannot be deleted: a sparkline of " + sheet_words(sheet.name)
						+ " reads the same cells of several sheets at once");
				if (reading.is_lost == true) {
					const pugi::xml_node place = first_child_named(node.parent(), "sqref");
					result.lost.push_back(lost_reference("sparkline", sheet.name, place ? place.child_value() : "", text,
						*after, reading));
				}
				return;
			}
			if (name != "conditionalFormatting" && name != "dataValidation") return;
			const std::string words = name == "dataValidation" ? "data validation" : "conditional format";
			const std::string sqref = ranges_of(node);
			// A rule whose every range goes goes with them.
			if (own.empty() == false && ranges_after_runs(sqref, own).empty() == true) return;
			for_each_element(node, [&](pugi::xml_node formula) {
				const std::string part = local_name(formula);
				if (part != "formula" && part != "formula1" && part != "formula2" && part != "f") return;
				if (first_child_named(formula, "f")) return;
				const std::string text = formula.child_value();
				if (text.empty() == true) return;
				DeletedReading reading;
				bool isExact = true;
				const std::optional<std::string> after = rule_after_runs(text, sheet.name, sqref, runs, &reading, isExact);
				if (!after || isExact == false)
					throw XlsxRowDeletionRefused("rows cannot be deleted from " + sheet_words(sheet.name) + ": the " + words
						+ " over " + sqref + " compares each row with another row, and would read other rows once the rows "
						"are gone");
				if (reading.is_lost == true) result.lost.push_back(lost_reference(words, sheet.name, sqref, text, *after, reading));
			});
		});
		// The formulas the sheet's tables work their columns out by.
		for (const Relationship &relationship : package.relationships(sheet.part)) {
			if (type_ends_with(relationship, "/table") == false || relationship.is_external == true
					|| package.has(relationship.target) == false)
				continue;
			pugi::xml_document &table = package.xml(relationship.target);
			std::string tableName;
			for (pugi::xml_node child : table.children())
				if (is_named(child, "table") == true) tableName = child.attribute("displayName").as_string();
			for_each_element(table, [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if (name != "calculatedColumnFormula" && name != "totalsRowFormula") return;
				const std::string text = node.child_value();
				if (text.empty() == true) return;
				DeletedReading reading;
				const std::optional<std::string> after = formula_after_runs(text, sheet.name, runs, &reading);
				if (!after)
					throw XlsxRowDeletionRefused("rows cannot be deleted: a column of the table " + sheet_words(tableName)
						+ " is worked out by a formula that reads the same cells of several sheets at once");
				if (reading.is_lost == true)
					result.lost.push_back(lost_reference("table column", sheet.name, tableName + "["
						+ std::string(node.parent().attribute("name").as_string()) + "]", text, *after, reading));
			});
		}
	}

	// Defined names: a named range, a print area.
	for_each_element(package.xml(opened.part), [&](pugi::xml_node node) {
		if (is_named(node, "definedName") == false) return;
		const std::string text = node.child_value();
		std::string context;
		if (node.attribute("localSheetId")) {
			const std::size_t index = static_cast<std::size_t>(node.attribute("localSheetId").as_ullong());
			if (index < opened.names_by_index.size()) context = opened.names_by_index[index];
		}
		DeletedReading reading;
		const std::optional<std::string> after = formula_after_runs(text, context, runs, &reading);
		if (!after)
			throw XlsxRowDeletionRefused("rows cannot be deleted: the defined name "
				+ sheet_words(node.attribute("name").as_string()) + " names cells without saying on which sheet");
		// Excel's own names - a print area, a filter - are not formulas a lawyer wrote.
		const std::string name = node.attribute("name").as_string();
		if (reading.is_lost == true && name.rfind("_xlnm.", 0) != 0)
			result.lost.push_back(lost_reference("name", context, name, text, *after, reading));
	});

	// Charts - on any sheet or a tab of their own.
	for (const std::string &chart : package.names_beginning("xl/charts/")) {
		const std::string file = chart.substr(chart.rfind('/') + 1);
		if (file.rfind("chart", 0) != 0 || file.size() < 4 || file.compare(file.size() - 4, 4, ".xml") != 0) continue;
		for_each_element(package.xml(chart), [&](pugi::xml_node node) {
			if (local_name(node) != "f") return;
			const std::string text = node.child_value();
			if (text.empty() == true) return;
			DeletedReading reading;
			const std::optional<std::string> after = formula_after_runs(text, std::string(), runs, &reading);
			if (!after)
				throw XlsxRowDeletionRefused("rows cannot be deleted: a chart reads the same cells of several sheets at once");
			if (reading.is_lost == true) result.lost.push_back(lost_reference("chart", std::string(), std::string(), text, *after, reading));
		});
	}
	return result;
}

// Sets `node`'s text to `formula` once every run is deleted, refusing what
// cannot be placed; whether it changed.
bool delete_in_formula_text(pugi::xml_node node, const std::string &contextSheet, const std::vector<Deletion> &runs) {
	const std::string text = node.child_value();
	if (text.empty() == true) return false;
	const std::optional<std::string> after = formula_after_runs(text, contextSheet, runs, nullptr);
	if (!after) throw XlsxRowDeletionRefused("rows cannot be deleted: a formula reads the same cells of several sheets at once");
	if (*after == text) return false;
	node.text().set(after->c_str());
	return true;
}

// A merged range once its sheet's runs go; "" when it goes, or is left one cell.
std::string merge_after_runs(const std::string &range, const std::vector<Deletion> &own) {
	const std::string after = ranges_after_runs(range, own);
	const std::size_t colon = after.find(':');
	if (after.empty() == true || colon == std::string::npos || after.substr(0, colon) == after.substr(colon + 1)) return {};
	return after;
}

void set_count(pugi::xml_node list, const char *item) {
	std::size_t count = 0;
	for (pugi::xml_node child : list.children()) if (is_named(child, item) == true) ++count;
	if (list.attribute("count")) list.attribute("count").set_value(static_cast<unsigned long long>(count));
}

// The sheet the rows go from: the rows themselves, and everything on it that
// names its rows.
void delete_rows_of_sheet(Package &package, const Worksheet &target, const std::vector<Deletion> &own) {
	pugi::xml_document &sheet = package.xml(target.part);
	pugi::xml_node worksheet = worksheet_root(sheet);
	pugi::xml_node sheetData = first_child_named(worksheet, "sheetData");

	// The rows, and every cell below them.
	std::vector<pugi::xml_node> gone;
	for (pugi::xml_node row : sheetData.children()) {
		if (is_named(row, "row") == false) continue;
		const long long number = row.attribute("r").as_llong();
		if (number < 1)
			throw XlsxRowDeletionRefused("rows cannot be deleted from " + sheet_words(target.name) + ": its rows do not say "
				"where they stand");
		const long long after = row_after_runs(number, own);
		if (after == 0) {
			gone.push_back(row);
			continue;
		}
		if (after == number) continue;
		row.attribute("r").set_value(after);
		for (pugi::xml_node cell : row.children()) {
			if (is_named(cell, "c") == false) continue;
			ReferencePart place;
			if (parse_reference_part(cell.attribute("r").as_string(), place) == false || place.has_row == false)
				throw XlsxRowDeletionRefused("rows cannot be deleted from " + sheet_words(target.name) + ": its cells do not "
					"say where they stand");
			place.row = after;
			cell.attribute("r").set_value(written_part(place).c_str());
		}
	}
	for (pugi::xml_node row : gone) sheetData.remove_child(row);

	// Everything else on the sheet that names its rows.
	std::vector<pugi::xml_node> removed;
	for_each_element(worksheet, [&](pugi::xml_node node) {
		const std::string name = local_name(node);
		if (name == "row" || name == "c") return;
		if (name == "f" && is_named(node.parent(), "c") == true) {
			if (std::string(node.attribute("t").as_string()) == "array" && node.attribute("ref"))
				node.attribute("ref").set_value(ranges_after_runs(node.attribute("ref").as_string(), own).c_str());
			return;
		}
		if (name == "conditionalFormatting" || name == "dataValidation") {
			const std::string sqref = ranges_of(node);
			const std::string after = ranges_after_runs(sqref, own);
			if (after.empty() == true) {
				removed.push_back(node);
				return;
			}
			if (node.attribute("sqref")) node.attribute("sqref").set_value(after.c_str());
			else if (first_child_named(node, "sqref")) first_child_named(node, "sqref").text().set(after.c_str());
			return;
		}
		if (name == "mergeCell" && node.attribute("ref")) {
			const std::string after = merge_after_runs(node.attribute("ref").as_string(), own);
			if (after.empty() == true) removed.push_back(node);
			else node.attribute("ref").set_value(after.c_str());
			return;
		}
		if (name == "dimension" && node.attribute("ref")) {
			const std::string after = ranges_after_runs(node.attribute("ref").as_string(), own);
			node.attribute("ref").set_value(after.empty() == true ? "A1" : after.c_str());
			return;
		}
		if (name == "pane" && node.attribute("topLeftCell")) {
			node.attribute("topLeftCell").set_value(view_cell_after_runs(node.attribute("topLeftCell").as_string(), own).c_str());
			return;
		}
		if (name == "selection") {
			std::string active = node.attribute("activeCell").as_string();
			if (active.empty() == false) {
				active = view_cell_after_runs(active, own);
				node.attribute("activeCell").set_value(active.c_str());
			}
			if (node.attribute("sqref")) {
				const std::string after = ranges_after_runs(node.attribute("sqref").as_string(), own);
				node.attribute("sqref").set_value(after.empty() == true ? (active.empty() == true ? "A1" : active.c_str())
					: after.c_str());
			}
			return;
		}
		if (name == "hyperlink" && node.attribute("ref")) {
			const std::string after = ranges_after_runs(node.attribute("ref").as_string(), own);
			if (after.empty() == true) removed.push_back(node);
			else node.attribute("ref").set_value(after.c_str());
			return;
		}
		if (name == "brk" && is_named(node.parent(), "rowBreaks") == true) {
			const long long after = row_after_runs(node.attribute("id").as_llong(), own);
			if (after == 0) removed.push_back(node);
			else node.attribute("id").set_value(after);
			return;
		}
		// A sparkline's own place; its source is a formula of the workbook.
		if (name == "sqref" && is_named(node.parent(), "sparkline") == true) {
			node.text().set(ranges_after_runs(node.child_value(), own).c_str());
			return;
		}
		// Pictures and controls anchored inside Excel's extensions.
		if ((name == "from" || name == "to") && first_child_named(node, "row")) {
			pugi::xml_node row = first_child_named(node, "row");
			const long long after = row_after_runs(std::atoll(row.child_value()) + 1, own);
			if (after > 0) row.text().set(std::to_string(after - 1).c_str());
			return;
		}
		for (const char *attribute : {"ref", "sqref"}) {
			if (!node.attribute(attribute) || node.attribute("sheet")) continue;
			const std::string after = ranges_after_runs(node.attribute(attribute).as_string(), own);
			if (after.empty() == true) removed.push_back(node);
			else node.attribute(attribute).set_value(after.c_str());
		}
	});
	std::sort(removed.begin(), removed.end());
	removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
	std::set<pugi::xml_node> parents;
	for (pugi::xml_node node : removed) {
		pugi::xml_node parent = node.parent();
		if (!parent) continue;
		parents.insert(parent);
		parent.remove_child(node);
	}
	// A list left empty goes - and with it an extension left holding nothing;
	// one that counts its items counts them again.
	const auto holdsNothing = [](pugi::xml_node node) {
		for (pugi::xml_node child : node.children()) if (child.type() == pugi::node_element) return false;
		return true;
	};
	for (pugi::xml_node parent : parents) {
		const std::string name = local_name(parent);
		if (name != "hyperlinks" && name != "mergeCells" && name != "dataValidations" && name != "rowBreaks"
				&& name != "conditionalFormattings")
			continue;
		if (holdsNothing(parent) == false) {
			if (name == "mergeCells") set_count(parent, "mergeCell");
			if (name == "dataValidations") set_count(parent, "dataValidation");
			if (name == "rowBreaks") {
				set_count(parent, "brk");
				if (parent.attribute("manualBreakCount"))
					parent.attribute("manualBreakCount").set_value(parent.attribute("count").as_ullong());
			}
			continue;
		}
		pugi::xml_node emptied = parent;
		while (emptied.parent() && holdsNothing(emptied) == true) {
			pugi::xml_node above = emptied.parent();
			above.remove_child(emptied);
			const std::string aboveName = local_name(above);
			if (aboveName != "ext" && aboveName != "extLst") break;
			emptied = above;
		}
	}
	package.mark_changed(target.part);

	// The sheet's tables, pictures, notes and comments.
	for (const Relationship &relationship : package.relationships(target.part)) {
		if (relationship.is_external == true || package.has(relationship.target) == false) continue;
		if (type_ends_with(relationship, "/table") == true) {
			bool isChanged = false;
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if ((name != "table" && name != "autoFilter" && name != "sortState" && name != "sortCondition")
						|| !node.attribute("ref"))
					return;
				const std::string after = ranges_after_runs(node.attribute("ref").as_string(), own);
				if (after == node.attribute("ref").as_string() || after.empty() == true) return;
				node.attribute("ref").set_value(after.c_str());
				isChanged = true;
			});
			if (isChanged == true) package.mark_changed(relationship.target);
		} else if (type_ends_with(relationship, "/drawing") == true) {
			bool isChanged = false;
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if (name != "from" && name != "to") return;
				pugi::xml_node row = first_child_named(node, "row");
				if (!row) return;
				const long long index = std::atoll(row.child_value());
				const long long after = row_after_runs(index + 1, own);
				if (after == 0 || after - 1 == index) return;
				row.text().set(std::to_string(after - 1).c_str());
				isChanged = true;
			});
			if (isChanged == true) package.mark_changed(relationship.target);
		} else if (type_ends_with(relationship, "/vmlDrawing") == true) {
			bool isChanged = false;
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if (name == "Row") {
					const long long index = std::atoll(node.child_value());
					const long long after = row_after_runs(index + 1, own);
					if (after == 0 || after - 1 == index) return;
					node.text().set(std::to_string(after - 1).c_str());
					isChanged = true;
				} else if (name == "Anchor") {
					std::vector<std::string> values;
					std::string current;
					for (const char character : std::string(node.child_value())) {
						if (character == ',') {
							values.push_back(current);
							current.clear();
						} else {
							current.push_back(character);
						}
					}
					values.push_back(current);
					if (values.size() != 8) return;
					for (const std::size_t at : {std::size_t(2), std::size_t(6)}) {
						const long long index = std::atoll(trimmed(values[at]).c_str());
						const long long after = row_after_runs(index + 1, own);
						if (after > 0 && after - 1 != index) values[at] = " " + std::to_string(after - 1);
					}
					std::string joined;
					for (std::size_t index = 0; index < values.size(); ++index) joined += (index == 0 ? "" : ",") + values[index];
					if (joined == node.child_value()) return;
					node.text().set(joined.c_str());
					isChanged = true;
				}
			});
			if (isChanged == true) package.mark_changed(relationship.target);
		} else if (type_ends_with(relationship, "/comments") == true
				|| type_ends_with(relationship, "/threadedComment") == true) {
			bool isChanged = false;
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if ((name != "comment" && name != "threadedComment") || !node.attribute("ref")) return;
				const std::string after = ranges_after_runs(node.attribute("ref").as_string(), own);
				if (after.empty() == true || after == node.attribute("ref").as_string()) return;
				node.attribute("ref").set_value(after.c_str());
				isChanged = true;
			});
			if (isChanged == true) package.mark_changed(relationship.target);
		}
	}
}

// The shared strings only deleted cells used: emptied, so what the rows said
// does not stay in the file once they are gone. Counted before the rows go.
struct SharedStringUses {
	std::string part;
	std::map<std::size_t, std::size_t> total;
	std::map<std::size_t, std::size_t> deleted;
};

SharedStringUses count_shared_strings(Package &package, const OpenedWorkbook &opened, const std::vector<Deletion> &runs) {
	SharedStringUses uses;
	for (const Relationship &relationship : opened.relationships)
		if (type_ends_with(relationship, "/sharedStrings") == true && package.has(relationship.target) == true)
			uses.part = relationship.target;
	if (uses.part.empty() == true) return uses;
	for (const Worksheet &sheet : opened.sheets) {
		const std::vector<Deletion> own = runs_of_sheet(runs, sheet.name);
		for_each_element(package.xml(sheet.part), [&](pugi::xml_node node) {
			if (is_named(node, "c") == false || std::string(node.attribute("t").as_string()) != "s") return;
			const pugi::xml_node value = first_child_named(node, "v");
			if (!value) return;
			const std::size_t index = static_cast<std::size_t>(std::atoll(value.child_value()));
			++uses.total[index];
			ReferencePart place;
			if (own.empty() == false && parse_reference_part(node.attribute("r").as_string(), place) == true
					&& place.has_row == true && row_after_runs(place.row, own) == 0)
				++uses.deleted[index];
		});
	}
	return uses;
}

void release_shared_strings(Package &package, const SharedStringUses &uses) {
	if (uses.part.empty() == true || uses.deleted.empty() == true) return;
	pugi::xml_document &strings = package.xml(uses.part);
	pugi::xml_node list;
	for (pugi::xml_node child : strings.children()) if (is_named(child, "sst") == true) list = child;
	if (!list) return;
	std::vector<pugi::xml_node> items;
	for (pugi::xml_node item : list.children()) if (is_named(item, "si") == true) items.push_back(item);
	std::size_t references = 0;
	for (const auto &entry : uses.total) references += entry.second;
	for (const auto &entry : uses.deleted) {
		references -= entry.second;
		if (entry.first >= items.size() || uses.total.at(entry.first) != entry.second) continue;
		pugi::xml_node item = items[entry.first];
		while (item.first_child()) item.remove_child(item.first_child());
		item.append_child((prefix_of(list) + "t").c_str());
	}
	if (list.attribute("count")) list.attribute("count").set_value(static_cast<unsigned long long>(references));
	package.mark_changed(uses.part);
}

std::vector<Deletion> opened_runs(const OpenedWorkbook &opened, const std::vector<XlsxRowDeletion> &rows) {
	if (opened.sheets.empty() == true) throw std::runtime_error("The workbook has no sheet rows can be deleted from.");
	return deletion_runs(rows, opened.sheets);
}

// Every sheet's formulas each in its own cell, once, before anything moves.
void unshare_every_sheet(Package &package, const OpenedWorkbook &opened) {
	for (const Worksheet &sheet : opened.sheets) {
		pugi::xml_document &document = package.xml(sheet.part);
		bool hasShared = false;
		for_each_element(document, [&](pugi::xml_node node) {
			if (is_named(node, "f") == true && std::string(node.attribute("t").as_string()) == "shared") hasShared = true;
		});
		if (hasShared == false) continue;
		try {
			unshare_formulas(document);
		} catch (const XlsxRowInsertionRefused &) {
			throw XlsxRowDeletionRefused("rows cannot be deleted: a shared formula of " + sheet_words(sheet.name)
				+ " does not say which cells it fills");
		}
		package.mark_changed(sheet.part);
	}
}

XlsxRowDeletionResult delete_rows_in(const std::filesystem::path &path, const std::vector<XlsxRowDeletion> &rows,
		bool isOnlyRead) {

	XlsxRowDeletionResult result;
	if (rows.empty() == true) return result;
	Package package(path);
	const OpenedWorkbook opened = open_workbook(package);
	const std::vector<Deletion> runs = opened_runs(opened, rows);
	unshare_every_sheet(package, opened);

	/* - - - - - - */

	// Refusals and what the deletion does, found before anything moves.
	std::set<std::string> deletedSheets;
	for (const Deletion &run : runs) deletedSheets.insert(run.name);
	for (const Worksheet &sheet : opened.sheets)
		if (deletedSheets.count(sheet.name) > 0) refuse_what_cannot_be_deleted(package, sheet, runs_of_sheet(runs, sheet.name), opened);
	result = read_deletion(package, opened, runs);
	if (isOnlyRead == true) return result;
	const SharedStringUses sharedStrings = count_shared_strings(package, opened, runs);

	/* - - - - - - */

	// Every formula of every sheet, as the rows leave it.
	for (const Worksheet &sheet : opened.sheets) {
		bool isChanged = false;
		for_each_element(package.xml(sheet.part), [&](pugi::xml_node node) {
			const std::string name = local_name(node);
			if (name == "f" && (is_named(node.parent(), "c") == true || is_named(node.parent(), "sparkline") == true)) {
				if (delete_in_formula_text(node, sheet.name, runs) == true) isChanged = true;
				return;
			}
			if (name == "conditionalFormatting" || name == "dataValidation") {
				const std::string sqref = ranges_of(node);
				for_each_element(node, [&](pugi::xml_node formula) {
					const std::string part = local_name(formula);
					if (part != "formula" && part != "formula1" && part != "formula2" && part != "f") return;
					if (first_child_named(formula, "f")) return;
					const std::string text = formula.child_value();
					if (text.empty() == true) return;
					bool isExact = true;
					const std::optional<std::string> after = rule_after_runs(text, sheet.name, sqref, runs, nullptr, isExact);
					if (!after || *after == text) return;
					formula.text().set(after->c_str());
					isChanged = true;
				});
				return;
			}
			if (name == "hyperlink" && node.attribute("location")) {
				const std::optional<std::string> after = formula_after_runs(node.attribute("location").as_string(),
					sheet.name, runs, nullptr);
				if (after && *after != node.attribute("location").as_string()) {
					node.attribute("location").set_value(after->c_str());
					isChanged = true;
				}
			}
		});
		if (isChanged == true) package.mark_changed(sheet.part);
		// The formulas its tables work their columns out by.
		for (const Relationship &relationship : package.relationships(sheet.part)) {
			if (type_ends_with(relationship, "/table") == false || relationship.is_external == true
					|| package.has(relationship.target) == false)
				continue;
			bool isTableChanged = false;
			for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
				const std::string name = local_name(node);
				if (name != "calculatedColumnFormula" && name != "totalsRowFormula") return;
				if (delete_in_formula_text(node, sheet.name, runs) == true) isTableChanged = true;
			});
			if (isTableChanged == true) package.mark_changed(relationship.target);
		}
	}

	// The rows themselves, and what stands on their sheets.
	for (const Worksheet &sheet : opened.sheets)
		if (deletedSheets.count(sheet.name) > 0) delete_rows_of_sheet(package, sheet, runs_of_sheet(runs, sheet.name));

	// Defined names, charts, and the data pivot tables are built from.
	bool isWorkbookChanged = false;
	for_each_element(package.xml(opened.part), [&](pugi::xml_node node) {
		if (is_named(node, "definedName") == false) return;
		std::string context;
		if (node.attribute("localSheetId")) {
			const std::size_t index = static_cast<std::size_t>(node.attribute("localSheetId").as_ullong());
			if (index < opened.names_by_index.size()) context = opened.names_by_index[index];
		}
		if (delete_in_formula_text(node, context, runs) == true) isWorkbookChanged = true;
	});
	if (isWorkbookChanged == true) package.mark_changed(opened.part);
	for (const std::string &chart : package.names_beginning("xl/charts/")) {
		const std::string file = chart.substr(chart.rfind('/') + 1);
		if (file.rfind("chart", 0) != 0 || file.size() < 4 || file.compare(file.size() - 4, 4, ".xml") != 0) continue;
		bool isChanged = false;
		for_each_element(package.xml(chart), [&](pugi::xml_node node) {
			if (local_name(node) != "f") return;
			if (delete_in_formula_text(node, std::string(), runs) == true) isChanged = true;
		});
		if (isChanged == true) package.mark_changed(chart);
	}
	for (const Relationship &relationship : opened.relationships) {
		if (type_ends_with(relationship, "/pivotCacheDefinition") == false || package.has(relationship.target) == false)
			continue;
		bool isChanged = false;
		for_each_element(package.xml(relationship.target), [&](pugi::xml_node node) {
			if (is_named(node, "worksheetSource") == false || !node.attribute("ref")) return;
			for (const std::string &name : deletedSheets) {
				if (upper_ascii(node.attribute("sheet").as_string()) != upper_ascii(name)) continue;
				node.attribute("ref").set_value(ranges_after_runs(node.attribute("ref").as_string(), runs_of_sheet(runs, name)).c_str());
				isChanged = true;
			}
		});
		if (isChanged == true) package.mark_changed(relationship.target);
	}

	/* - - - - - - */

	// A formula that read a deleted row keeps no value it was saved with: what
	// it calculates to is worked out again, or by Excel when it opens the file.
	for (const Worksheet &sheet : opened.sheets) {
		std::set<std::string> refs;
		for (const XlsxRecalculatedCell &cell : result.recalculated) if (cell.sheet == sheet.name) refs.insert(cell.ref);
		if (refs.empty() == true) continue;
		for_each_element(package.xml(sheet.part), [&](pugi::xml_node node) {
			if (is_named(node, "c") == false || refs.count(node.attribute("r").as_string()) == 0) return;
			const pugi::xml_node value = first_child_named(node, "v");
			if (value) node.remove_child(value);
			node.remove_attribute("t");
		});
		package.mark_changed(sheet.part);
	}
	release_shared_strings(package, sharedStrings);
	remove_calculation_chain(package, opened.part, opened.relationships);
	package.save();
	return result;

}

} // namespace

XlsxRowDeletionResult delete_xlsx_rows(const std::filesystem::path &path, const std::vector<XlsxRowDeletion> &rows) {
	try {
		return delete_rows_in(path, rows, false);
	} catch (const PackagePartRefused &refused) {
		throw XlsxRowDeletionRefused(std::string("rows cannot be deleted: ") + refused.what());
	}
}

std::vector<XlsxLostReference> xlsx_references_lost(const std::filesystem::path &path,
		const std::vector<XlsxRowDeletion> &rows) {
	try {
		return delete_rows_in(path, rows, true).lost;
	} catch (const PackagePartRefused &refused) {
		throw XlsxRowDeletionRefused(std::string("rows cannot be deleted: ") + refused.what());
	}
}

std::size_t xlsx_row_once_deleted(std::size_t row, std::vector<std::size_t> deleted) {
	std::sort(deleted.begin(), deleted.end());
	if (std::binary_search(deleted.begin(), deleted.end(), row) == true) return 0;
	std::size_t moved = row;
	for (const std::size_t at : deleted)
		if (at < row) --moved;
	return moved;
}

std::optional<std::string> xlsx_formula_with_rows_deleted(const std::string &formula,
		const std::string &formulaSheet, const std::string &deletedSheet, std::size_t at, std::size_t count) {
	const bool hasSign = formula.empty() == false && formula.front() == '=';
	const Deletion deletion{upper_ascii(deletedSheet), deletedSheet, static_cast<long long>(at), static_cast<long long>(count)};
	const std::optional<std::string> after = formula_after_deletion(hasSign == true ? formula.substr(1) : formula,
		formulaSheet, deletion, nullptr);
	if (!after) return std::nullopt;
	return hasSign == true ? "=" + *after : *after;
}

std::optional<std::string> xlsx_rows_deleted_exactly(const std::filesystem::path &before,
		const std::filesystem::path &after, const std::vector<XlsxRowDeletion> &rows) {

	const XlsxWorkbookViewResult was = read_xlsx_workbook_view({before, DocumentFormat::xlsx});
	const XlsxWorkbookViewResult now = read_xlsx_workbook_view({after, DocumentFormat::xlsx});
	if (was.status == ExtractionStatus::unreadable || now.status == ExtractionStatus::unreadable)
		return std::string("the workbook could not be read to prove the deleted rows");
	if (reading_is_whole(was) == false || reading_is_whole(now) == false)
		return std::string("the workbook is too large for Singularity to prove the deleted rows");
	if (was.workbook.sheets.size() != now.workbook.sheets.size() || was.workbook.sheets.empty() == true)
		return std::string("the workbook's sheets changed");
	std::vector<Worksheet> sheets;
	for (std::size_t index = 0; index < was.workbook.sheets.size(); ++index)
		sheets.push_back({was.workbook.sheets[index].name, std::string(), index});
	std::vector<Deletion> runs;
	// The formula cells whose values no longer hold, by sheet and by where they
	// stand once the rows are gone: found in the workbook as it was, as the
	// deletion itself finds them.
	std::set<std::pair<std::string, std::string>> recalculated;
	try {
		runs = deletion_runs(rows, sheets);
		for (const XlsxRecalculatedCell &cell : delete_rows_in(before, rows, true).recalculated)
			recalculated.insert({cell.sheet, cell.ref});
	} catch (const std::exception &refused) {
		return std::string(refused.what());
	}

	for (std::size_t index = 0; index < was.workbook.sheets.size(); ++index) {
		const SheetRender &previous = was.workbook.sheets[index];
		const SheetRender &current = now.workbook.sheets[index];
		if (previous.name != current.name) return std::string("the workbook's sheets changed");
		const std::vector<Deletion> own = runs_of_sheet(runs, previous.name);
		std::map<std::string, const SheetRenderCell *> cellsNow;
		std::map<std::size_t, bool> hiddenNow;
		for (const SheetRenderRow &row : current.rows) {
			hiddenNow[row.index] = row.hidden;
			for (const SheetRenderCell &cell : row.cells) cellsNow[cell.ref] = &cell;
		}
		std::size_t proven = 0;
		for (const SheetRenderRow &row : previous.rows) {
			const long long movedTo = row_after_runs(static_cast<long long>(row.index) + 1, own);
			if (movedTo == 0) continue;
			if (row.hidden == true && hiddenNow[static_cast<std::size_t>(movedTo - 1)] == false)
				return "row " + std::to_string(row.index + 1) + " of " + previous.name + " would no longer be hidden";
			for (const SheetRenderCell &cell : row.cells) {
				if (cell.kind == SheetCellKind::Empty && cell.formula.has_value() == false) continue;
				const std::string ref = column_letters(static_cast<long long>(cell.column) + 1) + std::to_string(movedTo);
				const auto found = cellsNow.find(ref);
				if (found == cellsNow.end()) return "cell " + cell.ref + " of " + previous.name + " would not be at " + ref;
				DeletedReading reading;
				std::optional<std::string> expected = cell.formula;
				if (expected) {
					const std::optional<std::string> text = formula_after_runs(expected->substr(1), previous.name, runs, &reading);
					if (!text) return "the formula in " + cell.ref + " of " + previous.name + " could not be placed";
					expected = "=" + *text;
				}
				if (found->second->formula != expected)
					return "the formula in " + cell.ref + " of " + previous.name + " would not read what Excel makes it read";
				// A formula whose value no longer holds keeps none - it is worked out
				// again - and every other cell keeps exactly what it held.
				if (cell.formula.has_value() == true && recalculated.count({previous.name, ref}) > 0) {
					if (found->second->kind != SheetCellKind::Empty)
						return "the formula in " + cell.ref + " of " + previous.name + " would keep a value that no longer holds";
					++proven;
					continue;
				}
				const bool isValueHeld = found->second->kind == cell.kind && found->second->stored_value == cell.stored_value
					&& found->second->raw_text == cell.raw_text;
				if (isValueHeld == false)
					return "cell " + cell.ref + " of " + previous.name + " would not keep its value at " + ref;
				++proven;
			}
		}
		std::size_t held = 0;
		for (const SheetRenderRow &row : current.rows)
			for (const SheetRenderCell &cell : row.cells)
				if (cell.kind != SheetCellKind::Empty || cell.formula.has_value() == true) ++held;
		if (held != proven) return previous.name + " would hold cells it did not hold";

		// Merges move with their rows, shrink by the rows they lose, and go when
		// every row of them goes.
		std::set<std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>> mergesNow;
		for (const SheetRenderMerge &merge : current.merges)
			mergesNow.insert({merge.first_row, merge.first_column, merge.last_row, merge.last_column});
		std::set<std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>> expectedMerges;
		for (const SheetRenderMerge &merge : previous.merges) {
			const std::string range = column_letters(static_cast<long long>(merge.first_column) + 1)
				+ std::to_string(merge.first_row + 1) + ":" + column_letters(static_cast<long long>(merge.last_column) + 1)
				+ std::to_string(merge.last_row + 1);
			const std::string moved = merge_after_runs(range, own);
			if (moved.empty() == true) continue;
			const std::vector<std::pair<long long, long long>> spans = rows_of_ranges(moved);
			if (spans.empty() == true) continue;
			expectedMerges.insert({static_cast<std::size_t>(spans.front().first - 1), merge.first_column,
				static_cast<std::size_t>(spans.front().second - 1), merge.last_column});
		}
		if (mergesNow != expectedMerges) return "the merged cells of " + previous.name + " would not move with their rows";
	}
	return std::nullopt;

}

// ---- Sheets added ----------------------------------------------------------------------

namespace {

const char *kWorksheetType = "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml";
const char *kRelationshipsNamespace = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

std::string ascii_folded(const std::string &name) {
	std::string folded = name;
	for (char &character : folded) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return folded;
}

// The length Excel measures a name in: UTF-16 units - a character past the
// Basic Multilingual Plane is two.
std::size_t utf16_length(const std::string &text) {
	std::size_t length = 0;
	for (const unsigned char byte : text) {
		if ((byte & 0xC0) == 0x80) continue;
		length += byte >= 0xF0 ? 2 : 1;
	}
	return length;
}

} // namespace

std::optional<std::string> xlsx_sheet_name_refusal(const std::string &name, const std::vector<std::string> &existing,
		XlsxNameFold fold) {
	const std::string refused = "the sheet " + sheet_words(name) + " cannot be added: ";
	if (trimmed(name).empty() == true) return std::string("a sheet cannot be added without a name");
	if (utf16_length(name) > 31) return refused + "Excel allows a sheet name of 31 characters at most";
	for (const unsigned char character : name)
		if (character < 0x20 || std::strchr(":\\/?*[]", static_cast<char>(character)) != nullptr)
			return refused + "Excel does not allow : \\ / ? * [ ] in a sheet name";
	if (name.front() == '\'' || name.back() == '\'')
		return refused + "Excel does not allow a sheet name to begin or end with an apostrophe";
	const auto folded = [&](const std::string &text) { return fold != nullptr ? fold(text) : ascii_folded(text); };
	const std::string mine = folded(name);
	if (mine == folded("History")) return refused + "Excel keeps the name \"History\" for itself";
	for (const std::string &other : existing)
		if (folded(other) == mine || ascii_folded(other) == ascii_folded(name))
			return refused + "the workbook already has a sheet named " + sheet_words(other);
	return std::nullopt;
}

void add_xlsx_sheets(const std::filesystem::path &path, const std::vector<std::string> &names, XlsxNameFold fold) {

	if (names.empty() == true) return;
	try {
		Package package(path);
		const OpenedWorkbook opened = open_workbook(package);
		if (opened.sheets.empty() == true) throw std::runtime_error("The workbook has no worksheet to add a sheet beside.");
		std::vector<std::string> existing = opened.names_by_index;
		for (const std::string &name : names) {
			const std::optional<std::string> refusal = xlsx_sheet_name_refusal(name, existing, fold);
			if (refusal) throw XlsxSheetAdditionRefused(*refusal);
			existing.push_back(name);
		}

		pugi::xml_document &workbook = package.xml(opened.part);
		pugi::xml_node root;
		for (pugi::xml_node child : workbook.children()) if (is_named(child, "workbook") == true) root = child;
		pugi::xml_node sheetList = root ? first_child_named(root, "sheets") : pugi::xml_node();
		if (!sheetList) throw std::runtime_error("The XLSX workbook lists no sheets.");
		// The prefix the workbook names relationships by ("r:id").
		std::string relationshipPrefix;
		for (pugi::xml_attribute attribute : root.attributes()) {
			const std::string attributeName = attribute.name();
			if (attributeName.rfind("xmlns:", 0) == 0 && std::string(attribute.value()) == kRelationshipsNamespace)
				relationshipPrefix = attributeName.substr(6);
		}
		if (relationshipPrefix.empty() == true) {
			relationshipPrefix = root.attribute("xmlns:r") ? "lmr" : "r";
			root.append_attribute(("xmlns:" + relationshipPrefix).c_str()) = kRelationshipsNamespace;
		}
		unsigned long long highestSheetId = 0;
		for (pugi::xml_node sheet : sheetList.children())
			if (is_named(sheet, "sheet") == true) highestSheetId = std::max(highestSheetId, sheet.attribute("sheetId").as_ullong());

		const std::string relationshipsPart = relationships_part_of(opened.part);
		pugi::xml_document &relationships = package.xml(relationshipsPart);
		pugi::xml_node relationshipList;
		for (pugi::xml_node child : relationships.children()) if (is_named(child, "Relationships") == true) relationshipList = child;
		if (!relationshipList) throw std::runtime_error("The XLSX workbook relationships are malformed.");
		std::set<std::string> relationshipIds;
		for (pugi::xml_node relationship : relationshipList.children())
			if (is_named(relationship, "Relationship") == true) relationshipIds.insert(relationship.attribute("Id").as_string());

		pugi::xml_document &types = package.xml("[Content_Types].xml");
		pugi::xml_node typeList;
		for (pugi::xml_node child : types.children()) if (is_named(child, "Types") == true) typeList = child;
		if (!typeList) throw std::runtime_error("The XLSX content types are malformed.");

		// Where the new parts go: beside the workbook's first worksheet.
		const std::string workbookDirectory = opened.part.substr(0, opened.part.rfind('/') + 1);
		const std::string firstPart = opened.sheets.front().part;
		std::string directory = firstPart.substr(0, firstPart.rfind('/') + 1);
		if (directory.rfind(workbookDirectory, 0) != 0) directory = workbookDirectory + "worksheets/";

		// The look every new sheet starts with: the workbook's default row height.
		std::string rowHeight = "15";
		for_each_element(package.xml(firstPart), [&](pugi::xml_node node) {
			if (is_named(node, "sheetFormatPr") == true && node.attribute("defaultRowHeight"))
				rowHeight = node.attribute("defaultRowHeight").as_string();
		});

		std::vector<std::string> added;
		for (const std::string &name : names) {
			std::size_t number = 1;
			while (package.has(directory + "sheet" + std::to_string(number) + ".xml") == true
					|| std::find(added.begin(), added.end(), directory + "sheet" + std::to_string(number) + ".xml") != added.end())
				++number;
			const std::string part = directory + "sheet" + std::to_string(number) + ".xml";
			added.push_back(part);
			std::size_t relationshipNumber = 1;
			while (relationshipIds.count("rId" + std::to_string(relationshipNumber)) > 0) ++relationshipNumber;
			const std::string relationshipId = "rId" + std::to_string(relationshipNumber);
			relationshipIds.insert(relationshipId);

			pugi::xml_node sheet = sheetList.append_child((prefix_of(sheetList) + "sheet").c_str());
			sheet.append_attribute("name") = name.c_str();
			sheet.append_attribute("sheetId") = ++highestSheetId;
			sheet.append_attribute((relationshipPrefix + ":id").c_str()) = relationshipId.c_str();

			pugi::xml_node relationship = relationshipList.append_child((prefix_of(relationshipList) + "Relationship").c_str());
			relationship.append_attribute("Id") = relationshipId.c_str();
			relationship.append_attribute("Type") = (std::string(kRelationshipsNamespace) + "/worksheet").c_str();
			relationship.append_attribute("Target") = part.substr(workbookDirectory.size()).c_str();

			pugi::xml_node type = typeList.append_child((prefix_of(typeList) + "Override").c_str());
			type.append_attribute("PartName") = ("/" + part).c_str();
			type.append_attribute("ContentType") = kWorksheetType;

			package.create(part, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
				"<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\""
				+ std::string(kRelationshipsNamespace) + "\"><dimension ref=\"A1\"/><sheetViews><sheetView "
				"workbookViewId=\"0\"/></sheetViews><sheetFormatPr defaultRowHeight=\"" + rowHeight + "\"/><sheetData/>"
				"<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
				"</worksheet>");
		}
		package.mark_changed(opened.part);
		package.mark_changed(relationshipsPart);
		package.mark_changed("[Content_Types].xml");

		// The workbook's list of its titles, where it keeps one: the new names
		// join the group its worksheets are named in.
		if (package.has("docProps/app.xml") == true) {
			pugi::xml_document &properties = package.xml("docProps/app.xml");
			pugi::xml_node pairs;
			pugi::xml_node titles;
			for_each_element(properties, [&](pugi::xml_node node) {
				if (local_name(node) == "HeadingPairs") pairs = first_child_named(node, "vector");
				if (local_name(node) == "TitlesOfParts") titles = first_child_named(node, "vector");
			});
			std::vector<pugi::xml_node> titleItems;
			if (titles) for (pugi::xml_node item : titles.children()) if (is_named(item, "lpstr") == true) titleItems.push_back(item);
			std::vector<pugi::xml_node> variants;
			if (pairs) for (pugi::xml_node item : pairs.children()) if (is_named(item, "variant") == true) variants.push_back(item);
			std::size_t offset = 0;
			for (std::size_t pair = 0; pair + 1 < variants.size(); pair += 2) {
				pugi::xml_node count = first_child_named(variants[pair + 1], "i4");
				if (!count) break;
				const std::size_t size = static_cast<std::size_t>(count.text().as_ullong());
				if (offset + size > titleItems.size()) break;
				bool isWorksheets = size > 0;
				for (std::size_t item = offset; item < offset + size; ++item) {
					const std::string title = titleItems[item].child_value();
					if (std::none_of(opened.sheets.begin(), opened.sheets.end(),
							[&](const Worksheet &worksheet) { return worksheet.name == title; }))
						isWorksheets = false;
				}
				if (isWorksheets == true) {
					pugi::xml_node after = titleItems[offset + size - 1];
					for (const std::string &name : names) {
						after = titles.insert_child_after((prefix_of(after) + "lpstr").c_str(), after);
						after.text().set(name.c_str());
					}
					count.text().set(static_cast<unsigned long long>(size + names.size()));
					titles.attribute("size").set_value(static_cast<unsigned long long>(titleItems.size() + names.size()));
					package.mark_changed("docProps/app.xml");
					break;
				}
				offset += size;
			}
		}
		package.save();
	} catch (const PackagePartRefused &refused) {
		throw XlsxSheetAdditionRefused(std::string("a sheet cannot be added: ") + refused.what());
	}

}

std::optional<std::string> xlsx_sheets_added_exactly(const std::filesystem::path &before,
		const std::filesystem::path &after, const std::vector<std::string> &names) {
	const XlsxWorkbookViewResult was = read_xlsx_workbook_view({before, DocumentFormat::xlsx});
	const XlsxWorkbookViewResult now = read_xlsx_workbook_view({after, DocumentFormat::xlsx});
	if (was.status == ExtractionStatus::unreadable || now.status == ExtractionStatus::unreadable)
		return std::string("the workbook could not be read to prove the new sheets");
	if (reading_is_whole(was) == false || reading_is_whole(now) == false)
		return std::string("the workbook is too large for Singularity to prove the new sheets");
	if (now.workbook.sheets.size() != was.workbook.sheets.size() + names.size())
		return std::string("the workbook would not hold exactly the new sheets");
	for (std::size_t index = 0; index < was.workbook.sheets.size(); ++index) {
		const SheetRender &previous = was.workbook.sheets[index];
		const SheetRender &current = now.workbook.sheets[index];
		if (previous.name != current.name || previous.visibility != current.visibility
				|| previous.rows.size() != current.rows.size() || previous.merges.size() != current.merges.size())
			return "the sheet " + sheet_words(previous.name) + " would change";
		for (std::size_t row = 0; row < previous.rows.size(); ++row) {
			const SheetRenderRow &a = previous.rows[row];
			const SheetRenderRow &b = current.rows[row];
			if (a.index != b.index || a.hidden != b.hidden || a.cells.size() != b.cells.size())
				return "the sheet " + sheet_words(previous.name) + " would change";
			for (std::size_t cell = 0; cell < a.cells.size(); ++cell)
				if (a.cells[cell].ref != b.cells[cell].ref || a.cells[cell].kind != b.cells[cell].kind
						|| a.cells[cell].stored_value != b.cells[cell].stored_value
						|| a.cells[cell].raw_text != b.cells[cell].raw_text || a.cells[cell].formula != b.cells[cell].formula)
					return "cell " + a.cells[cell].ref + " of " + sheet_words(previous.name) + " would change";
		}
	}
	for (std::size_t index = 0; index < names.size(); ++index) {
		const SheetRender &added = now.workbook.sheets[was.workbook.sheets.size() + index];
		if (added.name != names[index] || added.visibility != SheetVisibility::Visible)
			return "the new sheet " + sheet_words(names[index]) + " would not stand after the last one";
		for (const SheetRenderRow &row : added.rows)
			if (row.cells.empty() == false) return "the new sheet " + sheet_words(names[index]) + " would not be empty";
	}
	return std::nullopt;
}

} // namespace singularity::documents
