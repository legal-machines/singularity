#pragma once

// The universal Work Product protocol: the one shape every generative answer
// also returns (professional_response.workProduct), so any answer can become
// retained legal work without anyone re-typing it. A document is a title, a
// presentation kind, a one-paragraph summary and an ordered list of blocks.
// Three forms cover legal work product: text (heading, paragraph, list),
// tables (registers, comparisons, schedules - any columns and rows) and
// quotes (a verbatim passage with its locator). Each block cites the supplied
// source markers it relies on; Singularity resolves those to document labels, so
// retained work never carries a machine handle.
//
// Header-only on purpose: it is pure data shaping over nlohmann::json and is
// used by execution, persistence and the bridges alike.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace singularity::work_product_document {

inline constexpr std::size_t kMaxBlocks = 40;
inline constexpr std::size_t kMaxListItems = 40;
inline constexpr std::size_t kMaxColumns = 8;
inline constexpr std::size_t kMaxRows = 60;
inline constexpr std::size_t kMaxSources = 12;
inline constexpr std::size_t kMaxTextBytes = 20000;
// What a Work Product keeps of the answer it is made from: everything the
// lawyer read, up to bounds no answer reaches. The limits above are the
// schema's, capping what a model writes in one reply; they are never applied
// to work already written - a register of 80 rows is kept as 80 rows, in 12
// columns as 12.
inline constexpr std::size_t kKeptBlocks = 400;
inline constexpr std::size_t kKeptListItems = 400;
inline constexpr std::size_t kKeptColumns = 24;
inline constexpr std::size_t kKeptRows = 2000;

inline const std::vector<std::string> &kinds() {
	static const std::vector<std::string> values{"memo", "table", "quotes", "list", "draft", "note"};
	return values;
}

inline const std::vector<std::string> &block_types() {
	static const std::vector<std::string> values{"paragraph", "heading", "list", "table", "quote"};
	return values;
}

// Strict and fully required, as every provider's structured-output mode needs:
// unused fields are empty strings or empty arrays, never null, and enums are
// never nullable (Anthropic rejects a nullable enum). Only item counts are
// bounded - a grammar-constrained local model rejects string length bounds;
// lengths are enforced by sanitize() instead.
inline nlohmann::json schema() {
	using Json = nlohmann::json;
	const Json string{{"type", "string"}};
	const auto stringArray = [](std::size_t maxItems) {
		return Json{{"type", "array"}, {"maxItems", maxItems}, {"items", {{"type", "string"}}}};
	};
	// Guidance lives in schema descriptions, not in the prompt: providers read
	// them with the schema, and they cost nothing against the context budget.
	const auto described = [](Json value, const char *description) {
		value["description"] = description;
		return value;
	};
	Json block{{"type", "object"}, {"additionalProperties", false},
		{"properties", {
			{"type", described({{"type", "string"}, {"enum", block_types()}},
				"table for anything with rows and columns; quote for verbatim text; list; heading; paragraph")},
			{"text", described(string, "paragraph, heading or quoted text; empty for list and table")},
			{"label", described(string, "quote locator (clause, article, page), table caption or list title")},
			{"items", described(stringArray(kMaxListItems), "list items")},
			{"columns", described(stringArray(kMaxColumns), "table column headings")},
			{"rows", described({{"type", "array"}, {"maxItems", kMaxRows}, {"items", stringArray(kMaxColumns)}},
				"table rows, each as wide as columns")},
			{"sources", described(stringArray(kMaxSources), "supplied source markers this block relies on")},
		}},
		{"required", Json::array({"type", "text", "label", "items", "columns", "rows", "sources"})}};
	return described(Json{{"type", "object"}, {"additionalProperties", false},
		{"properties", {
			{"title", described(string, "precise title of the work product")},
			{"kind", described({{"type", "string"}, {"enum", kinds()}},
				"table for registers and comparisons, quotes for extracts, memo for analysis, draft for usable text")},
			{"summary", described(string, "one-paragraph summary")},
			{"blocks", {{"type", "array"}, {"maxItems", kMaxBlocks}, {"items", std::move(block)}}},
		}},
		{"required", Json::array({"title", "kind", "summary", "blocks"})}},
		"the same answer as a reusable legal work product");
}


namespace detail {

inline std::string trimmed(const nlohmann::json &value, std::size_t maxBytes = kMaxTextBytes) {
	if (!value.is_string()) return {};
	std::string text = value.get<std::string>();
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) return {};
	const auto end = text.find_last_not_of(" \t\r\n");
	text = text.substr(begin, end - begin + 1);
	if (text.size() > maxBytes) {
		std::size_t cut = maxBytes;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
		text.resize(cut);
	}
	return text;
}

inline nlohmann::json string_list(const nlohmann::json &value, std::size_t maxItems, bool keepEmpty = false) {
	nlohmann::json result = nlohmann::json::array();
	if (!value.is_array()) return result;
	for (const auto &entry : value) {
		if (result.size() >= maxItems) break;
		std::string text = trimmed(entry);
		if (text.empty() && !keepEmpty) continue;
		result.push_back(std::move(text));
	}
	return result;
}

} // namespace detail

// What the model writes of the Work Product beside its answer: its title,
// presentation kind and summary - a few words. The body is the answer itself,
// split into blocks by blocks_from_markdown(): a model asked to write it twice
// spent as many output tokens again on every answer and every repair.
inline nlohmann::json heading_schema() {
	using Json = nlohmann::json;
	const Json string{{"type", "string"}};
	const auto described = [](Json value, const char *description) {
		value["description"] = description;
		return value;
	};
	return described(Json{{"type", "object"}, {"additionalProperties", false},
		{"properties", {
			{"title", described(string, "precise title of the work product")},
			{"kind", described({{"type", "string"}, {"enum", kinds()}},
				"table for registers and comparisons, quotes for extracts, memo for analysis, draft for usable text")},
			{"summary", described(string, "one-paragraph summary")},
		}},
		{"required", Json::array({"title", "kind", "summary"})}},
		"how this answer is kept as legal work: its title, kind and summary; its body is the answer");
}

namespace detail {

inline std::string trim_line(const std::string &line) {
	const auto begin = line.find_first_not_of(" \t\r");
	if (begin == std::string::npos) return {};
	const auto end = line.find_last_not_of(" \t\r");
	return line.substr(begin, end - begin + 1);
}

// The supplied source markers a stretch of the answer cites, and its text
// without them: the block lists its sources, the text reads clean.
inline std::string take_markers(const std::string &text, nlohmann::json &sources) {
	std::string clean;
	std::size_t at = 0;
	while (at < text.size()) {
		const std::size_t open = text.find("[SOURCE:", at);
		if (open == std::string::npos) {
			clean += text.substr(at);
			break;
		}
		const std::size_t close = text.find(']', open);
		if (close == std::string::npos) {
			clean += text.substr(at);
			break;
		}
		clean += text.substr(at, open - at);
		const std::string marker = text.substr(open, close - open + 1);
		if (std::find(sources.begin(), sources.end(), marker) == sources.end() && sources.size() < kMaxSources)
			sources.push_back(marker);
		at = close + 1;
	}
	// Spaces the markers leave behind, before punctuation and doubled.
	std::string tidy;
	for (std::size_t index = 0; index < clean.size(); ++index) {
		const char value = clean[index];
		if (value == ' ' && (tidy.empty() || tidy.back() == ' ')) continue;
		if ((value == '.' || value == ',' || value == ';' || value == ':' || value == ')') && !tidy.empty() && tidy.back() == ' ')
			tidy.pop_back();
		tidy += value;
	}
	return trim_line(tidy);
}

// A table line, with or without the outer pipes a writer may leave out
// ("Clause | Deadline"): any pipe that is not written as text (\|).
inline bool is_table_row(const std::string &line) {
	for (std::size_t index = 0; index < line.size(); ++index) {
		if (line[index] == '\\') {
			++index;
			continue;
		}
		if (line[index] == '|') return true;
	}
	return false;
}

inline std::vector<std::string> table_cells(const std::string &line) {
	std::vector<std::string> cells;
	std::string cell;
	std::size_t begin = line.empty() == false && line.front() == '|' ? 1 : 0;
	std::size_t end = line.size();
	if (end > begin && line[end - 1] == '|' && (end < 2 || line[end - 2] != '\\')) --end;
	for (std::size_t index = begin; index < end; ++index) {
		if (line[index] == '\\' && index + 1 < end && line[index + 1] == '|') {
			cell += '|';
			++index;
			continue;
		}
		if (line[index] == '|') {
			cells.push_back(trim_line(cell));
			cell.clear();
			continue;
		}
		cell += line[index];
	}
	cells.push_back(trim_line(cell));
	return cells;
}

// The line under a table's headings: one rule of dashes (a colon at either end
// for alignment) for each heading, as GitHub writes it.
inline bool is_table_rule(const std::string &line, std::size_t columns) {
	if (line.find('-') == std::string::npos) return false;
	const std::vector<std::string> cells = table_cells(line);
	if (cells.size() != columns) return false;
	for (const std::string &cell : cells) {
		if (cell.empty()) return false;
		const std::size_t first = cell.front() == ':' ? 1 : 0;
		const std::size_t last = cell.size() > 1 && cell.back() == ':' ? cell.size() - 1 : cell.size();
		if (first >= last || cell.find_first_not_of('-', first) < last) return false;
	}
	return true;
}

// "- item", "* item", "• item", "1. item", "1) item": the item, or nothing.
inline std::optional<std::string> list_item(const std::string &line) {
	if (line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') return line.substr(2);
	if (line.rfind("\u2022 ", 0) == 0) return line.substr(4);
	std::size_t digits = 0;
	while (digits < line.size() && digits < 3 && line[digits] >= '0' && line[digits] <= '9') ++digits;
	if (digits > 0 && digits + 1 < line.size() && (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ')
		return line.substr(digits + 2);
	return std::nullopt;
}

inline std::string unbold(std::string text) {
	if (text.size() >= 4 && text.rfind("**", 0) == 0 && text.compare(text.size() - 2, 2, "**") == 0)
		text = text.substr(2, text.size() - 4);
	return text;
}

} // namespace detail

// The answer, as the blocks of its Work Product: headings, paragraphs, lists,
// tables and quotes, each with the source markers it cites. Built by code from
// the answer the lawyer reads, so the kept work says exactly what the answer
// said - never a second rendering of it by the model.
inline nlohmann::json blocks_from_markdown(const std::string &markdown) {
	using Json = nlohmann::json;
	std::vector<std::string> lines;
	{
		std::string line;
		for (const char value : markdown) {
			if (value == '\n') {
				lines.push_back(line);
				line.clear();
			} else {
				line += value;
			}
		}
		lines.push_back(line);
	}
	Json blocks = Json::array();
	const auto block = [](const char *type) {
		return Json{{"type", type}, {"text", ""}, {"label", ""}, {"items", Json::array()}, {"columns", Json::array()},
			{"rows", Json::array()}, {"sources", Json::array()}};
	};
	std::string paragraph;
	const auto closeParagraph = [&]() {
		if (detail::trim_line(paragraph).empty()) {
			paragraph.clear();
			return;
		}
		Json entry = block("paragraph");
		entry["text"] = detail::take_markers(paragraph, entry["sources"]);
		if (!entry["text"].get<std::string>().empty()) blocks.push_back(std::move(entry));
		paragraph.clear();
	};
	for (std::size_t index = 0; index < lines.size() && blocks.size() < kKeptBlocks; ++index) {
		const std::string line = detail::trim_line(lines[index]);
		if (line.empty()) {
			closeParagraph();
			continue;
		}
		if (line.front() == '#') {
			closeParagraph();
			std::size_t level = 0;
			while (level < line.size() && line[level] == '#') ++level;
			Json entry = block("heading");
			entry["text"] = detail::take_markers(detail::unbold(detail::trim_line(line.substr(level))), entry["sources"]);
			if (!entry["text"].get<std::string>().empty()) blocks.push_back(std::move(entry));
			continue;
		}
		if (detail::is_table_row(line) && index + 1 < lines.size()
				&& detail::is_table_rule(detail::trim_line(lines[index + 1]), detail::table_cells(line).size())) {
			closeParagraph();
			Json entry = block("table");
			for (const std::string &cell : detail::table_cells(line)) {
				if (entry["columns"].size() >= kKeptColumns) break;
				entry["columns"].push_back(detail::take_markers(cell, entry["sources"]));
			}
			index += 2;
			for (; index < lines.size() && detail::is_table_row(detail::trim_line(lines[index])); ++index) {
				if (entry["rows"].size() >= kKeptRows) continue;
				Json cells = Json::array();
				for (const std::string &cell : detail::table_cells(detail::trim_line(lines[index]))) {
					if (cells.size() >= kKeptColumns) break;
					cells.push_back(detail::take_markers(cell, entry["sources"]));
				}
				entry["rows"].push_back(std::move(cells));
			}
			--index;
			blocks.push_back(std::move(entry));
			continue;
		}
		if (line.front() == '>') {
			closeParagraph();
			Json entry = block("quote");
			std::string quoted;
			for (; index < lines.size(); ++index) {
				const std::string next = detail::trim_line(lines[index]);
				if (next.empty() || next.front() != '>') break;
				quoted += (quoted.empty() ? "" : " ") + detail::trim_line(next.substr(1));
			}
			--index;
			entry["text"] = detail::take_markers(quoted, entry["sources"]);
			if (!entry["text"].get<std::string>().empty()) blocks.push_back(std::move(entry));
			continue;
		}
		if (detail::list_item(line)) {
			closeParagraph();
			Json entry = block("list");
			for (; index < lines.size(); ++index) {
				const std::string next = detail::trim_line(lines[index]);
				const std::optional<std::string> item = detail::list_item(next);
				if (!item) break;
				if (entry["items"].size() < kKeptListItems)
					entry["items"].push_back(detail::take_markers(*item, entry["sources"]));
			}
			--index;
			if (!entry["items"].empty()) blocks.push_back(std::move(entry));
			continue;
		}
		paragraph += (paragraph.empty() ? "" : " ") + line;
	}
	closeParagraph();
	return blocks;
}

// A document Singularity can keep, or nullopt when the value carries nothing
// usable. Unknown kinds and types fall back to memo/paragraph, empty blocks
// are dropped, and a table's rows are made exactly as wide as its columns.
inline std::optional<nlohmann::json> sanitize(const nlohmann::json &value) {
	using Json = nlohmann::json;
	if (!value.is_object()) return std::nullopt;
	Json document{{"title", detail::trimmed(value.value("title", Json()), 400)},
		{"kind", "memo"}, {"summary", detail::trimmed(value.value("summary", Json()))},
		{"blocks", Json::array()}};
	const std::string kind = detail::trimmed(value.value("kind", Json()), 32);
	if (std::find(kinds().begin(), kinds().end(), kind) != kinds().end()) document["kind"] = kind;
	if (value.contains("blocks") && value.at("blocks").is_array()) {
		for (const Json &raw : value.at("blocks")) {
			if (document["blocks"].size() >= kKeptBlocks) break;
			if (!raw.is_object()) continue;
			std::string type = detail::trimmed(raw.value("type", Json()), 16);
			if (std::find(block_types().begin(), block_types().end(), type) == block_types().end()) type = "paragraph";
			Json block{{"type", type}, {"text", detail::trimmed(raw.value("text", Json()))},
				{"label", detail::trimmed(raw.value("label", Json()), 400)},
				{"items", Json::array()}, {"columns", Json::array()}, {"rows", Json::array()},
				{"sources", detail::string_list(raw.value("sources", Json()), kMaxSources)}};
			if (type == "list") {
				block["items"] = detail::string_list(raw.value("items", Json()), kKeptListItems);
				if (block["items"].empty()) continue;
			} else if (type == "table") {
				block["columns"] = detail::string_list(raw.value("columns", Json()), kKeptColumns, true);
				const std::size_t width = block["columns"].size();
				if (width == 0) continue;
				if (raw.contains("rows") && raw.at("rows").is_array()) {
					for (const Json &row : raw.at("rows")) {
						if (block["rows"].size() >= kKeptRows) break;
						Json cells = detail::string_list(row, kKeptColumns, true);
						bool anyText = false;
						for (const auto &cell : cells) anyText = anyText || !cell.get<std::string>().empty();
						if (!anyText) continue;
						while (cells.size() < width) cells.push_back("");
						while (cells.size() > width) cells.erase(cells.size() - 1);
						block["rows"].push_back(std::move(cells));
					}
				}
				if (block["rows"].empty()) continue;
			} else if (block["text"].get<std::string>().empty()) {
				continue;
			}
			document["blocks"].push_back(std::move(block));
		}
	}
	if (document["blocks"].empty() && document["summary"].get<std::string>().empty()) return std::nullopt;
	return document;
}

// Every human-readable string in a sanitized document, for rendering any
// stray source handle into its plain document label.
inline std::vector<std::string *> text_fields(nlohmann::json &document) {
	std::vector<std::string *> fields;
	const auto add = [&](nlohmann::json &value) {
		if (auto *text = value.get_ptr<std::string *>()) fields.push_back(text);
	};
	add(document["title"]);
	add(document["summary"]);
	for (auto &block : document["blocks"]) {
		add(block["text"]);
		add(block["label"]);
		for (auto &item : block["items"]) add(item);
		for (auto &column : block["columns"]) add(column);
		for (auto &row : block["rows"]) for (auto &cell : row) add(cell);
	}
	return fields;
}

// The document as Markdown - the text a retained Work Product keeps, so copy,
// edit, versions and export work unchanged. The title is not repeated: the
// Work Product carries it.
inline std::string markdown(const nlohmann::json &document) {
	const auto cell = [](std::string text) {
		std::string escaped;
		for (char value : text) {
			if (value == '|') escaped += "\\|";
			else if (value == '\n' || value == '\r') escaped += ' ';
			else escaped += value;
		}
		return escaped;
	};
	std::string out;
	const auto paragraph = [&](const std::string &text) {
		if (text.empty()) return;
		if (!out.empty()) out += "\n\n";
		out += text;
	};
	paragraph(document.value("summary", std::string()));
	if (!document.contains("blocks")) return out;
	for (const auto &block : document.at("blocks")) {
		const std::string type = block.value("type", std::string("paragraph"));
		const std::string text = block.value("text", std::string());
		const std::string label = block.value("label", std::string());
		if (type == "heading") {
			paragraph("## " + text);
		} else if (type == "list") {
			std::string list;
			if (!label.empty()) list += "**" + label + "**\n";
			for (const auto &item : block.at("items")) list += "- " + item.get<std::string>() + "\n";
			if (!list.empty()) list.pop_back();
			paragraph(list);
		} else if (type == "table") {
			std::string table;
			if (!label.empty()) table += "**" + label + "**\n\n";
			const auto &columns = block.at("columns");
			table += "|";
			for (const auto &column : columns) table += " " + cell(column.get<std::string>()) + " |";
			table += "\n|";
			for (std::size_t index = 0; index < columns.size(); ++index) table += " --- |";
			for (const auto &row : block.at("rows")) {
				table += "\n|";
				for (const auto &value : row) table += " " + cell(value.get<std::string>()) + " |";
			}
			paragraph(table);
		} else if (type == "quote") {
			std::string quote;
			std::size_t start = 0;
			while (start <= text.size()) {
				const std::size_t end = text.find('\n', start);
				quote += "> " + text.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
				if (end == std::string::npos) break;
				start = end + 1;
			}
			if (!label.empty()) quote += "> \xE2\x80\x94 " + label + "\n";
			quote.pop_back();
			paragraph(quote);
		} else {
			if (!label.empty()) paragraph("**" + label + "**");
			paragraph(text);
		}
		if (block.contains("sourceLabels") && block.at("sourceLabels").is_array() && !block.at("sourceLabels").empty()) {
			std::string sources;
			for (const auto &entry : block.at("sourceLabels")) {
				if (!sources.empty()) sources += "; ";
				sources += entry.get<std::string>();
			}
			paragraph("*" + sources + "*");
		}
	}
	return out;
}

} // namespace singularity::work_product_document
