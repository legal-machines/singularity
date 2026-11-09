#include "Document/OfficeTextEdit/OfficeTextEdit.hpp"

#include "Platform/PhraseMatch/PhraseMatch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::documents {
namespace {

bool local_name_is(const pugi::xml_node &node, const char *name) {
	const char *raw = node.name();
	const char *colon = std::strchr(raw, ':');
	return std::strcmp(colon == nullptr ? raw : colon + 1, name) == 0;
}

// The namespace prefix (with its colon, or empty) an element is written with,
// so a new sibling is written the same way.
std::string prefix_of(const pugi::xml_node &node) {
	const char *raw = node.name();
	const char *colon = std::strchr(raw, ':');
	return colon == nullptr ? std::string() : std::string(raw, colon + 1);
}

void visit(const pugi::xml_node &node, const std::function<void(pugi::xml_node)> &each) {
	for (pugi::xml_node child : node.children()) {
		each(child);
		visit(child, each);
	}
}

// One run of text in the document, and where its characters stand in the text
// a reader sees.
struct TextRun {
	pugi::xml_node node;      // the w:t element
	pugi::xml_node paragraph; // the w:p it belongs to
	pugi::xml_node cell;      // the w:tc it stands in, when it stands in a table
	std::size_t start = 0;    // offset in the flat text
	std::size_t length = 0;
};

struct FlatDocument {
	std::string text;
	std::vector<TextRun> runs;
	std::vector<pugi::xml_node> paragraphs;
	// Where each paragraph's words stand in `text`: [first, second).
	std::vector<std::pair<std::size_t, std::size_t>> spans;
};

// The table cell a paragraph stands in, or nothing when it stands in ordinary
// body text.
pugi::xml_node cell_of(pugi::xml_node paragraph) {
	for (pugi::xml_node up = paragraph.parent(); up; up = up.parent()) {
		if (local_name_is(up, "tc")) return up;
		if (local_name_is(up, "body")) break;
	}
	return {};
}

// A walk that does not enter what `skip` names: the reader of a document leaves
// those parts out, and the editor must see the same text the reader shows.
void visit_except(const pugi::xml_node &node, std::initializer_list<const char *> skip,
		const std::function<void(pugi::xml_node)> &each) {
	for (pugi::xml_node child : node.children()) {
		bool skipped = false;
		for (const char *name : skip) if (local_name_is(child, name)) { skipped = true; break; }
		if (skipped) continue;
		each(child);
		visit_except(child, skip, each);
	}
}

FlatDocument flatten(const pugi::xml_document &document) {
	FlatDocument flat;
	// Paragraphs in document order, including those inside table cells: a table
	// cell holds ordinary paragraphs, and a lawyer edits a cell by naming its
	// words exactly as they edit a sentence. The older fallback copy of a text
	// box is not read: the modern copy holds the same words, and reading both
	// would put every word of the box in two places.
	std::vector<pugi::xml_node> paragraphs;
	visit_except(document, {"Fallback"}, [&](pugi::xml_node node) {
		if (local_name_is(node, "p")) paragraphs.push_back(node);
	});
	for (const pugi::xml_node &paragraph : paragraphs) {
		if (!flat.text.empty()) flat.text.push_back('\n');
		const std::size_t paragraphStart = flat.text.size();
		// A text box's paragraphs are read as paragraphs of their own, so they
		// are not read again inside the paragraph that holds the box; nor is a
		// phonetic guide, which is not part of the sentence. A passage someone
		// moved is read where it now stands (w:moveTo), not also at the place it
		// left (w:moveFrom): read twice, it could never be named at all.
		visit_except(paragraph, {"Fallback", "txbxContent", "rt", "moveFrom"}, [&](pugi::xml_node node) {
			// A tab and a line break are what the reader shows between the words
			// they separate - "5.<tab>Indemnification", "By:<tab>____" - so a
			// model quoting the reader's text finds the same passage here. They
			// are separators, not text a change writes into.
			if (local_name_is(node.parent(), "r")) {
				if (local_name_is(node, "tab") || local_name_is(node, "ptab")) { flat.text.push_back('\t'); return; }
				if (local_name_is(node, "cr")) { flat.text.push_back('\n'); return; }
				if (local_name_is(node, "br")) {
					const pugi::xml_attribute type = node.attribute("w:type") ? node.attribute("w:type") : node.attribute("type");
					if (std::strcmp(type.as_string(), "page") != 0) flat.text.push_back('\n');
					return;
				}
			}
			if (!local_name_is(node, "t")) return;
			const std::string value = node.text().as_string();
			TextRun run;
			run.node = node;
			run.paragraph = paragraph;
			run.cell = cell_of(paragraph);
			run.start = flat.text.size();
			run.length = value.size();
			flat.text += value;
			flat.runs.push_back(run);
		});
		flat.paragraphs.push_back(paragraph);
		flat.spans.emplace_back(paragraphStart, flat.text.size());
	}
	return flat;
}

void set_run_text(pugi::xml_node node, const std::string &value) {
	if (!node.attribute("xml:space")) node.append_attribute("xml:space");
	node.attribute("xml:space").set_value("preserve");
	node.text().set(value.c_str());
}

// Writes `value` into `node`, keeping that run's formatting, and writes any
// further lines of it as a line break and a further piece of text in the same
// run - which is how Word itself holds a line break inside a paragraph. A tab
// is written as Word's own tab the same way: Word shows a tab character
// inside a piece of text as a plain space.
void set_run_text_with_lines(pugi::xml_node node, const std::string &value) {
	std::vector<std::string> pieces;
	std::vector<const char *> separators;
	std::string current;
	for (char character : value) {
		if (character == '\n' || character == '\t') {
			pieces.push_back(current);
			separators.push_back(character == '\n' ? "br" : "tab");
			current.clear();
			continue;
		}
		if (character != '\r') current.push_back(character);
	}
	pieces.push_back(current);
	set_run_text(node, pieces.front());
	pugi::xml_node previous = node;
	const std::string prefix = prefix_of(node);
	for (std::size_t index = 1; index < pieces.size(); ++index) {
		pugi::xml_node separator = node.parent().insert_child_after((prefix + separators[index - 1]).c_str(), previous);
		previous = separator;
		// An empty piece is not written: two tabs in a row are two tabs, not
		// two tabs with nothing between them.
		if (pieces[index].empty()) continue;
		pugi::xml_node piece = node.parent().insert_child_after((prefix + "t").c_str(), separator);
		set_run_text(piece, pieces[index]);
		previous = piece;
	}
}

// ---- Marks a model writes, read as formatting ------------------------------

// A mark opens where a word does not run into it from the left and something
// that is not a space follows it, and closes the other way round. That is what
// keeps "5*6", "[*]", "Section_2" and an asterisked footnote as the text they
// are: a legal document is full of characters that look like marks.
bool opens_mark(const std::string &line, std::size_t at, std::size_t length) {
	if (at > 0 && std::isalnum(static_cast<unsigned char>(line[at - 1])) != 0) return false;
	const std::size_t after = at + length;
	return after < line.size() && line[after] != ' ';
}

bool closes_mark(const std::string &line, std::size_t at, std::size_t length) {
	if (at == 0 || line[at - 1] == ' ') return false;
	const std::size_t after = at + length;
	if (after >= line.size()) return true;
	return std::isalnum(static_cast<unsigned char>(line[after])) == 0;
}

// Where this mark closes on the same line, if it does.
std::size_t closing_mark(const std::string &line, std::size_t from, std::size_t length, char character) {
	const std::string mark(length, character);
	std::size_t at = line.find(mark, from);
	while (at != std::string::npos) {
		if (closes_mark(line, at, length)) return at;
		at = line.find(mark, at + 1);
	}
	return std::string::npos;
}

// One line's words, with the marks in it turned into the way the words are
// written. Nesting is handled by recursion, so "**a *b* c**" is bold, and its
// middle word is bold and italic.
std::vector<ProseRun> runs_of_line(const std::string &line, bool bold, bool italic) {
	std::vector<ProseRun> runs;
	std::string plain;
	const auto flush = [&]() {
		if (plain.empty()) return;
		runs.push_back({plain, bold, italic});
		plain.clear();
	};
	const auto take = [&](const std::string &inside, bool nowBold, bool nowItalic) {
		flush();
		for (const ProseRun &run : runs_of_line(inside, nowBold, nowItalic)) runs.push_back(run);
	};
	std::size_t at = 0;
	while (at < line.size()) {
		const char here = line[at];
		if (here == '`') {
			// A model marks a quoted handle with backticks. The handle is real
			// text; the backticks are not.
			const std::size_t close = line.find('`', at + 1);
			if (close != std::string::npos) {
				plain += line.substr(at + 1, close - at - 1);
				at = close + 1;
				continue;
			}
		}
		// Underscores are a line to write on before they are a mark: a run of
		// three or more ("________") is always the blank it draws, and one or two
		// open emphasis only before a letter or digit - «__» ________ 2026 … is
		// a date left blank, not bold text (a live Russian draft lost its blanks
		// to exactly that, 24 Sep 2026).
		if (here == '_' && (at == 0 || line[at - 1] != '_')) {
			std::size_t run = 0;
			while (at + run < line.size() && line[at + run] == '_') ++run;
			const std::size_t after = at + run;
			const unsigned char next = after < line.size() ? static_cast<unsigned char>(line[after]) : 0;
			const bool wordFollows = next != 0 && (std::isalnum(next) != 0
				|| (next >= 0xC3 && next != 0xE2));  // a letter outside ASCII; not «», dashes or quotes
			if (run > 2 || !wordFollows) {
				plain.append(run, '_');
				at = after;
				continue;
			}
		}
		if ((here == '*' || here == '_') && at + 1 < line.size() && line[at + 1] == here
				&& opens_mark(line, at, 2)) {
			const std::size_t close = closing_mark(line, at + 2, 2, here);
			if (close != std::string::npos && close > at + 2) {
				take(line.substr(at + 2, close - at - 2), true, italic);
				at = close + 2;
				continue;
			}
		}
		if ((here == '*' || here == '_') && opens_mark(line, at, 1)) {
			const std::size_t close = closing_mark(line, at + 1, 1, here);
			if (close != std::string::npos && close > at + 1) {
				take(line.substr(at + 1, close - at - 1), bold, true);
				at = close + 1;
				continue;
			}
		}
		plain.push_back(here);
		++at;
	}
	flush();
	return runs;
}

void apply_run_formatting(pugi::xml_node run, const TextFormatting &formatting);

void remove_children_named(pugi::xml_node node, std::initializer_list<const char *> names) {
	std::vector<pugi::xml_node> remove;
	for (pugi::xml_node child : node.children()) {
		for (const char *name : names) {
			if (local_name_is(child, name)) { remove.push_back(child); break; }
		}
	}
	for (pugi::xml_node child : remove) node.remove_child(child);
}

// What a copied paragraph or run must not take from the one it was copied
// from: a section break (a new paragraph carrying one would end a section of
// its own, on a new page) and the revisions recorded on it - each has an id
// no other revision may share, and the copy is a change of its own.
void drop_what_belongs_to_the_model(pugi::xml_node properties) {
	remove_children_named(properties, {"sectPr", "pPrChange", "rPrChange", "ins", "del", "moveFrom", "moveTo"});
	for (pugi::xml_node child : properties.children()) {
		if (local_name_is(child, "rPr")) remove_children_named(child, {"ins", "del", "moveFrom", "moveTo", "rPrChange"});
	}
}

// Whether a paragraph names a list itself (w:numPr with a list other than 0,
// which says "no list") - all that is known of its numbering without the
// package's styles.
bool names_its_own_list(const pugi::xml_node &paragraph) {
	for (pugi::xml_node properties : paragraph.children()) {
		if (!local_name_is(properties, "pPr")) continue;
		for (pugi::xml_node numbering : properties.children()) {
			if (!local_name_is(numbering, "numPr")) continue;
			for (pugi::xml_node id : numbering.children()) {
				if (!local_name_is(id, "numId")) continue;
				const pugi::xml_attribute value = id.attribute("w:val") ? id.attribute("w:val") : id.attribute("val");
				return std::strcmp(value.as_string(), "0") != 0 && *value.as_string() != '\0';
			}
		}
	}
	return false;
}

// The paragraph's words without the first `count` bytes, taken across its runs.
ProseParagraph without_leading_bytes(ProseParagraph paragraph, std::size_t count) {
	for (ProseRun &run : paragraph.runs) {
		if (count == 0) break;
		const std::size_t taken = std::min(count, run.text.size());
		run.text.erase(0, taken);
		count -= taken;
	}
	return paragraph;
}

// `anyValues`: only the number's form must be the drawn one's, not the values
// before its last - "7.1." has the form of a level Word draws "1.1." at.
std::size_t typed_number_like_drawn(const std::string &line, const std::string &drawn, bool anyValues = false);
std::size_t typed_item_mark_length(const std::string &line);
std::size_t typed_clause_number_length(const std::string &line);
bool is_multipart_item_mark(const std::string &line);
std::vector<std::string> number_values(const std::string &text);
bool equal_folded(const std::string &a, const std::string &b);
pugi::xml_node add_property(pugi::xml_node properties, const std::string &name);

// How many bytes at the head of `line` are an item mark in brackets - "(a) ",
// "(iv) ", "(2) ", "b) " - with the space after it; 0 where there is none. A
// line that opens like that is an item of a list, not a clause's own words:
// "Section 7.1 of the Agreement" and "1.5 million shares" never open so.
std::size_t bracketed_item_length(const std::string &line) {
	std::size_t at = 0;
	while (at < line.size() && line[at] == ' ') ++at;
	const bool isOpened = at < line.size() && line[at] == '(';
	if (isOpened) ++at;
	const std::size_t tokenStart = at;
	while (at < line.size() && std::isalnum(static_cast<unsigned char>(line[at])) != 0) ++at;
	const std::string token = line.substr(tokenStart, at - tokenStart);
	if (token.empty() || token.size() > 4 || at >= line.size() || line[at] != ')') return 0;
	const bool isDigits = std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
	// "b", "bb": a letter a list counts with, once or repeated.
	const auto folded = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
	const bool isLetters = token.size() <= 3 && std::isalpha(static_cast<unsigned char>(token[0])) != 0
		&& std::all_of(token.begin(), token.end(), [&](char c) { return folded(c) == folded(token[0]); });
	const bool isRoman = std::all_of(token.begin(), token.end(), [](char c) { return std::strchr("ivxlcdmIVXLCDM", c) != nullptr; });
	if (!isDigits && !isLetters && !isRoman) return 0;
	++at;
	if (at >= line.size() || line[at] != ' ') return 0;
	while (at < line.size() && line[at] == ' ') ++at;
	return at < line.size() ? at : 0;
}

// A paragraph's words written back in the marks a model writes them in: a
// heading's hashes, two stars round bold words, one round italic ones.
std::string marked_words(const ProseParagraph &paragraph) {
	std::string line = paragraph.heading_level > 0 ? std::string(static_cast<std::size_t>(paragraph.heading_level), '#') + " "
		: std::string();
	for (const ProseRun &run : paragraph.runs) {
		if (run.text.empty()) continue;
		const std::string mark = run.bold && run.italic ? "***" : run.bold ? "**" : run.italic ? "*" : "";
		// The marks go round the words, never round the spaces beside them.
		const std::size_t first = run.text.find_first_not_of(' ');
		if (mark.empty() || first == std::string::npos) {
			line += run.text;
			continue;
		}
		const std::size_t last = run.text.find_last_not_of(' ');
		line += run.text.substr(0, first) + mark + run.text.substr(first, last - first + 1) + mark + run.text.substr(last + 1);
	}
	return line;
}

// The model's lines as the paragraphs they become, without the blank lines
// between them: a blank line a model leaves between two clauses is spacing,
// and written as a paragraph it is an empty numbered item in a list. Text of
// nothing but blank lines is written as it was given.
std::vector<ProseParagraph> paragraphs_to_write(const std::string &text) {
	std::vector<ProseParagraph> all = prose_paragraphs(text);
	const auto isBlank = [](const ProseParagraph &paragraph) {
		for (const ProseRun &run : paragraph.runs)
			if (run.text.find_first_not_of(" \t\r") != std::string::npos) return false;
		return true;
	};
	std::vector<ProseParagraph> kept;
	for (const ProseParagraph &paragraph : all) if (!isBlank(paragraph)) kept.push_back(paragraph);
	return kept.empty() ? all : kept;
}

// The paragraph's own list, written into its properties: at `level` of list
// `listId`, or in no list at all where `listId` is "0".
void set_list_of(pugi::xml_node properties, const std::string &listId, std::size_t level) {
	remove_children_named(properties, {"numPr"});
	const std::string prefix = prefix_of(properties);
	pugi::xml_node numbering = add_property(properties, "numPr");
	numbering.append_child((prefix + "ilvl").c_str()).append_attribute("w:val") = static_cast<unsigned int>(level);
	numbering.append_child((prefix + "numId").c_str()).append_attribute("w:val") = listId.c_str();
}

// A paragraph written like `model`: its own properties (style, numbering,
// indentation) and the formatting of its first run.
//
// A paragraph the model marked as a heading is written like the document's
// own headings where it has any, and in bold where it has none. Either way
// the hashes are gone: what is added is a heading, not a line about one.
//
// Where Word numbers the paragraph it is written like, Word numbers the new
// one too - and the clauses after it are numbered on from it, which is how a
// lawyer adding a clause wants it. So a number the model typed at its head in
// the form Word draws them there ("9.3" where Word draws "9.2", "(c)" beside
// "(b)") or a bullet it typed is not written: Word draws the right one. Words
// that only begin like a number - "Section 7.1 of the Agreement", "1.5
// million shares", "A. Smith" - are the clause's own, and stay. A line typed as
// a sub-item - "(a) flood;" beside a clause Word numbers "1.3" - is written at
// the list's own level that draws "(a)", its typed mark left to Word; where no
// level of the list draws one like it, it is written as a paragraph of its own
// with the mark it was typed with, never as one more clause.
//
// What was written, in the marks a model writes (marked_words), goes on
// `written`.
// Where a paragraph written like `like`, beside `at`, stands in the list, and
// the words it is written with: its typed number taken off where Word draws
// one like it.
struct ListPlacement {
	ProseParagraph writing;
	std::optional<std::size_t> deeperLevel;
	std::optional<ListPlace> place;
	bool isOutsideList = false;
	bool movesInList() const { return deeperLevel.has_value() || isOutsideList; }
};

ListPlacement list_placement(pugi::xml_node at, pugi::xml_node like, const ProseParagraph &asWritten, bool before,
		const TextEditRequest &request) {
	ListPlacement result;
	ProseParagraph writing = asWritten;
	// What Word draws in front of the paragraph written like: known where the
	// package's numbering was read, unknown ("") where only the paragraph's
	// own list is.
	const AutomaticNumbering &numbered = request.numbered_automatically;
	const std::optional<std::string> drawn = numbered ? numbered(like)
		: names_its_own_list(like) ? std::optional<std::string>(std::string()) : std::nullopt;
	// Where the new paragraph stands in the list: at the level it was written
	// like, at another level of the same list, or in no list.
	std::optional<std::size_t> deeperLevel;
	std::optional<ListPlace> place;
	bool isOutsideList = false;
	bool isAtOwnLevel = false;
	if (drawn) {
		std::string words;
		for (const ProseRun &run : writing.runs) words += run.text;
		const std::size_t typed = typed_number_like_drawn(words, *drawn);
		const bool isBracketed = bracketed_item_length(words) > 0;
		if (typed > 0 && typed < words.size()) {
			writing = without_leading_bytes(writing, typed);
		} else if (typed == 0 && writing.heading_level == 0
				&& (isBracketed || typed_item_mark_length(words) > 0 || typed_clause_number_length(words) > 0)) {
			// A line typed as an item of another level - "7.1. ..." under a
			// clause Word numbers "7.", "8. ..." after a sub-item "7.1.", "(a)"
			// under "1.3", "1.3 ..." after an item "(a)" - is written at the
			// level of the list that draws a number of that form, its typed
			// number left to Word. The values in front of the last must be those
			// of the clause it is written under or beside: "7.1." belongs under
			// "7.", never under "3.".
			place = request.list_place ? request.list_place(like) : std::nullopt;
			if (place) {
				pugi::xml_node neighbour = before ? at.previous_sibling() : at;
				const auto previousParagraph = [](pugi::xml_node node) {
					while (node && !local_name_is(node, "p")) node = node.previous_sibling();
					return node;
				};
				neighbour = previousParagraph(neighbour);
				const auto isDigits = [](const std::string &value) {
					return !value.empty() && std::isdigit(static_cast<unsigned char>(value[0])) != 0;
				};
				// The nearest numbered paragraph at or before the new one whose
				// number counts its leading values the same way - digits for
				// "7.1.", so an item "(a)" between is passed over - decides.
				const auto agrees = [&](const std::vector<std::string> &typedValues) {
					if (typedValues.size() < 2 || !numbered) return true;
					const std::size_t leading = typedValues.size() - 1;
					std::size_t looked = 0;
					for (pugi::xml_node other = neighbour; other && looked < 64;
							other = previousParagraph(other.previous_sibling()), ++looked) {
						const std::optional<std::string> shown = numbered(other);
						if (!shown || shown->empty()) continue;
						const std::vector<std::string> values = number_values(*shown);
						if (values.size() < leading) continue;
						bool sameKind = true;
						for (std::size_t part = 0; part < leading; ++part)
							if (isDigits(values[part]) != isDigits(typedValues[part])) sameKind = false;
						if (!sameKind) continue;
						for (std::size_t part = 0; part < leading; ++part)
							if (!equal_folded(values[part], typedValues[part])) return false;
						return true;
					}
					return true;
				};
				std::vector<std::size_t> levels{place->level};
				for (std::size_t level = place->level + 1; level < place->first_drawn.size(); ++level) levels.push_back(level);
				for (std::size_t level = place->level; level-- > 0;) levels.push_back(level);
				for (const std::size_t level : levels) {
					if (level >= place->first_drawn.size()) continue;
					const std::optional<std::string> &form = place->first_drawn[level];
					if (!form || form->empty()) continue;
					const std::size_t sub = typed_number_like_drawn(words, *form, true);
					if (sub == 0 || sub >= words.size()) continue;
					const std::vector<std::string> typedValues = number_values(words.substr(0, sub));
					if (typedValues.size() != number_values(*form).size() || !agrees(typedValues)) continue;
					if (level != place->level) deeperLevel = level;
					else isAtOwnLevel = true;
					writing = without_leading_bytes(writing, sub);
					break;
				}
			}
			// Only a mark that cannot be a clause's own words - "(a)", "7.1." -
			// is kept out of the list when no level draws it: "A. Smith" stays
			// the clause it was written as.
			isOutsideList = !deeperLevel && !isAtOwnLevel && (isBracketed || is_multipart_item_mark(words));
		}
	}
	result.writing = writing;
	result.deeperLevel = deeperLevel;
	result.place = place;
	result.isOutsideList = isOutsideList;
	return result;
}

// The paragraph moved to where `placement` puts it in the list.
void apply_list_placement(pugi::xml_node paragraph, const ListPlacement &placement) {
	if (!placement.movesInList()) return;
	pugi::xml_node properties;
	for (pugi::xml_node child : paragraph.children()) if (local_name_is(child, "pPr")) properties = child;
	if (!properties) properties = paragraph.prepend_child((prefix_of(paragraph) + "pPr").c_str());
	if (placement.deeperLevel) {
		// Indented as the level itself indents its items.
		remove_children_named(properties, {"ind"});
		set_list_of(properties, placement.place->list_id, *placement.deeperLevel);
	} else {
		set_list_of(properties, "0", 0);
		// Where the clause's own words begin, so it reads as part of it.
		if (placement.place && placement.place->text_indent_twips) {
			remove_children_named(properties, {"ind"});
			add_property(properties, "ind").append_attribute("w:left") = *placement.place->text_indent_twips;
		}
	}
}

pugi::xml_node paragraph_like(pugi::xml_node at, pugi::xml_node model, const ProseParagraph &asWritten,
		pugi::xml_node headingModel, bool before, const TextEditRequest &request, std::vector<std::string> &written) {
	pugi::xml_node parent = at.parent();
	const std::string prefix = prefix_of(at);
	pugi::xml_node like = asWritten.heading_level > 0 && headingModel ? headingModel : model;
	if (!like) like = at;
	const ListPlacement placement = list_placement(at, like, asWritten, before, request);
	const ProseParagraph &writing = placement.writing;
	pugi::xml_node created = before ? parent.insert_child_before(at.name(), at)
		: parent.insert_child_after(at.name(), at);
	for (pugi::xml_node child : like.children()) {
		if (local_name_is(child, "pPr")) drop_what_belongs_to_the_model(created.append_copy(child));
	}
	apply_list_placement(created, placement);
	// The first run it holds - inside an insertion too: a paragraph written
	// like one this same change has just added takes its look from it.
	pugi::xml_node runModel;
	for (pugi::xml_node child : like.children()) {
		if (local_name_is(child, "r")) { runModel = child; break; }
		if (!local_name_is(child, "ins")) continue;
		for (pugi::xml_node inner : child.children()) if (local_name_is(inner, "r")) { runModel = inner; break; }
		if (runModel) break;
	}
	// A heading the document cannot show as its own is shown as a heading
	// anyway: bold is what a lawyer writing in a plain document would do.
	const bool boldForHeading = writing.heading_level > 0 && !headingModel;
	for (const ProseRun &piece : writing.runs) {
		// A run the typed number was all of is not written as an empty one.
		if (piece.text.empty() && writing.runs.size() > 1) continue;
		pugi::xml_node run = runModel ? created.append_copy(runModel)
			: created.append_child((prefix + "r").c_str());
		// Keep the run's properties, drop whatever text it carried.
		std::vector<pugi::xml_node> remove;
		for (pugi::xml_node child : run.children()) if (!local_name_is(child, "rPr")) remove.push_back(child);
		for (pugi::xml_node child : remove) run.remove_child(child);
		for (pugi::xml_node child : run.children()) if (local_name_is(child, "rPr")) drop_what_belongs_to_the_model(child);
		TextFormatting written_as;
		if (piece.bold || boldForHeading) written_as.bold = true;
		if (piece.italic) written_as.italic = true;
		if (written_as.touches_runs()) apply_run_formatting(run, written_as);
		set_run_text(run.append_child((prefix + "t").c_str()), piece.text);
	}
	written.push_back(marked_words(writing));
	return created;
}

// The paragraph in this document that a heading of this level is written
// like. A document that has no heading of its own returns nothing, and the
// caller writes the heading in bold instead of inventing a style name that
// this document's styles may not define.
pugi::xml_node heading_model(pugi::xml_node body, int level, const HeadingOf &isHeading);

// Whether a paragraph is a heading: the document's styles say so (`isHeading`),
// or it carries a heading style or an outline level itself. Word says it every
// way, and a document written by hand says it any one of them.
bool paragraph_is_heading(pugi::xml_node paragraph, const HeadingOf &isHeading) {
	if (isHeading && isHeading(paragraph)) return true;
	for (pugi::xml_node child : paragraph.children()) {
		if (!local_name_is(child, "pPr")) continue;
		for (pugi::xml_node property : child.children()) {
			if (local_name_is(property, "outlineLvl")) return true;
			if (!local_name_is(property, "pStyle")) continue;
			const std::string style = property.attribute("w:val")
				? property.attribute("w:val").value() : property.attribute("val").value();
			std::string folded;
			for (char character : style)
				if (character != ' ' && character != '-' && character != '_')
					folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
			if (folded.rfind("heading", 0) == 0) return true;
		}
	}
	return false;
}

pugi::xml_node heading_model(pugi::xml_node body, int level, const HeadingOf &isHeading) {
	if (!body || level <= 0) return {};
	pugi::xml_node any;
	const std::string wanted = std::to_string(level);
	for (pugi::xml_node child : body.children()) {
		if (!local_name_is(child, "p") || !paragraph_is_heading(child, isHeading)) continue;
		if (!any) any = child;
		for (pugi::xml_node properties : child.children()) {
			if (!local_name_is(properties, "pPr")) continue;
			for (pugi::xml_node property : properties.children()) {
				if (!local_name_is(property, "pStyle")) continue;
				const std::string style = property.attribute("w:val")
					? property.attribute("w:val").value() : property.attribute("val").value();
				// The level a style names, whatever language the style is
				// named in: what decides is the digit at its end.
				if (!style.empty() && style.substr(style.size() - 1) == wanted) return child;
			}
		}
	}
	// A heading of another level is still a heading: better the document's own
	// heading than a style name this document may not define.
	return any;
}

// The paragraph a newly inserted one should be written like. Text that follows
// a heading is body text, not another heading: a summary inserted under a title
// must read as a summary. So a heading is never the model - the nearest
// ordinary paragraph on the side the text is going is, and where there is none,
// the new paragraph is written plain.
pugi::xml_node paragraph_to_write_like(pugi::xml_node anchor, bool before, const HeadingOf &isHeading) {
	if (!paragraph_is_heading(anchor, isHeading)) return anchor;
	pugi::xml_node parent = anchor.parent();
	std::vector<pugi::xml_node> paragraphs;
	for (pugi::xml_node child : parent.children()) if (local_name_is(child, "p")) paragraphs.push_back(child);
	const auto at = std::find(paragraphs.begin(), paragraphs.end(), anchor);
	if (at == paragraphs.end()) return anchor;
	if (before) {
		for (auto walk = at; walk != paragraphs.begin();) {
			--walk;
			if (!paragraph_is_heading(*walk, isHeading)) return *walk;
		}
		return {};
	}
	for (auto walk = std::next(at); walk != paragraphs.end(); ++walk) {
		if (!paragraph_is_heading(*walk, isHeading)) return *walk;
	}
	return {};
}

// The run a text node belongs to (its w:r parent).
pugi::xml_node run_of(pugi::xml_node textNode) {
	for (pugi::xml_node up = textNode.parent(); up; up = up.parent()) {
		if (local_name_is(up, "r")) return up;
		if (local_name_is(up, "p")) break;
	}
	return {};
}

// A run split so that exactly the bytes [from, to) of one piece of its text
// stand in a run of their own, written like the original. Returns that middle
// run. This is what lets one sentence inside a paragraph be made bold while
// the words around it keep the formatting the author gave them.
//
// A run can hold more than one piece of text, with a line break or a tab
// between them - Word writes an address block that way. Everything else the
// run holds stays on its own side of the split, exactly once: what stood
// before the piece stays in the original run, what stood after it moves to the
// run after the middle one. It is moved, never copied, so a piece of text a
// caller is still holding is still the one in the document.
pugi::xml_node run_for_span(pugi::xml_node textNode, std::size_t from, std::size_t to) {
	pugi::xml_node run = run_of(textNode);
	if (!run || textNode.parent() != run) return {};
	const std::string value = textNode.text().as_string();
	to = std::min(to, value.size());
	from = std::min(from, to);
	bool before = false;
	bool seen = false;
	std::vector<pugi::xml_node> after;
	for (pugi::xml_node child : run.children()) {
		if (child == textNode) seen = true;
		else if (!local_name_is(child, "rPr")) {
			if (seen) after.push_back(child);
			else before = true;
		}
	}
	if (from == 0 && to == value.size() && !before && after.empty()) return run;
	pugi::xml_node parent = run.parent();
	const std::string textName = prefix_of(textNode) + "t";
	// A run written like the original, holding nothing yet.
	const auto emptyRunAfter = [&](pugi::xml_node at) {
		pugi::xml_node piece = parent.insert_copy_after(run, at);
		std::vector<pugi::xml_node> content;
		for (pugi::xml_node child : piece.children()) if (!local_name_is(child, "rPr")) content.push_back(child);
		for (pugi::xml_node child : content) piece.remove_child(child);
		return piece;
	};
	pugi::xml_node middle = emptyRunAfter(run);
	if (to < value.size() || !after.empty()) {
		pugi::xml_node tail = emptyRunAfter(middle);
		if (to < value.size()) set_run_text(tail.append_child(textName.c_str()), value.substr(to));
		for (pugi::xml_node child : after) tail.append_move(child);
	}
	middle.append_move(textNode);
	set_run_text(textNode, value.substr(from, to - from));
	if (from > 0) set_run_text(run.append_child(textName.c_str()), value.substr(0, from));
	// A run left with nothing in it is not kept: an empty run is not how a
	// document holds a change.
	if (from == 0 && !before) parent.remove_child(run);
	return middle;
}

// ---- Revisions, as Word records them ---------------------------------------

// Every revision in a document carries an id of its own. Word does not care
// what the numbers are, only that no two annotations share one - a moved
// passage, a cell's or a row's changed look, a comment's range and a bookmark
// count as much as an insertion - so a new change starts after the highest id
// any of them already holds in the file.
int next_revision_id(pugi::xml_node root) {
	static const char *const kAnnotations[] = {"ins", "del", "moveFrom", "moveTo", "moveFromRangeStart",
		"moveFromRangeEnd", "moveToRangeStart", "moveToRangeEnd", "rPrChange", "pPrChange", "tcPrChange",
		"trPrChange", "tblPrChange", "tblPrExChange", "tblGridChange", "sectPrChange", "numberingChange", "cellIns",
		"cellDel", "cellMerge", "customXmlInsRangeStart", "customXmlInsRangeEnd", "customXmlDelRangeStart",
		"customXmlDelRangeEnd", "customXmlMoveFromRangeStart", "customXmlMoveFromRangeEnd",
		"customXmlMoveToRangeStart", "customXmlMoveToRangeEnd", "commentRangeStart", "commentRangeEnd",
		"bookmarkStart", "bookmarkEnd", "permStart", "permEnd"};
	int highest = 0;
	visit(root, [&](pugi::xml_node node) {
		pugi::xml_attribute id = node.attribute("w:id");
		if (!id) {
			// A document written with another prefix, or none, still names its
			// annotations by their own names.
			const bool isAnnotation = std::any_of(std::begin(kAnnotations), std::end(kAnnotations),
				[&](const char *name) { return local_name_is(node, name); });
			if (!isAnnotation) return;
			for (pugi::xml_attribute attribute : node.attributes()) {
				const char *name = attribute.name();
				const char *colon = std::strchr(name, ':');
				if (std::strcmp(colon != nullptr ? colon + 1 : name, "id") == 0) { id = attribute; break; }
			}
			if (!id) return;
		}
		highest = std::max(highest, id.as_int());
	});
	// Never past the largest id a document can hold.
	return highest < std::numeric_limits<int>::max() ? highest + 1 : highest;
}

const char *const kDateUtcNamespace = "http://schemas.microsoft.com/office/word/2023/wordml/word16du";
const char *const kCompatibilityNamespace = "http://schemas.openxmlformats.org/markup-compatibility/2006";

// The prefix `uri` is declared with on `element` ("mc" for xmlns:mc), "" where
// it is not declared there.
std::string declared_prefix(const pugi::xml_node &element, const char *uri) {
	for (pugi::xml_attribute attribute : element.attributes()) {
		const std::string name = attribute.name();
		if (name.rfind("xmlns:", 0) == 0 && std::strcmp(attribute.value(), uri) == 0) return name.substr(6);
	}
	return std::string();
}

// Whether the document `node` stands in can carry Word's own UTC stamp,
// w16du:dateUtc: its root declares the namespace as Word declares it, and
// marks it ignorable (mc:Ignorable) so a Word that predates the stamp opens
// the file as it always did. Declared here where the root does not yet; a
// document that uses the prefix for something else is left as it is, and its
// revisions carry w:date alone.
bool declare_date_utc(const pugi::xml_node &node) {
	pugi::xml_node root;
	for (pugi::xml_node child : node.root().children()) {
		if (child.type() == pugi::node_element) { root = child; break; }
	}
	if (!root) return false;
	const std::string existing = declared_prefix(root, kDateUtcNamespace);
	if (existing == "w16du") return true;
	if (!existing.empty() || root.attribute("xmlns:w16du")) return false;
	std::string compatibility = declared_prefix(root, kCompatibilityNamespace);
	if (compatibility.empty()) {
		if (root.attribute("xmlns:mc")) return false;
		root.append_attribute("xmlns:mc") = kCompatibilityNamespace;
		compatibility = "mc";
	}
	root.append_attribute("xmlns:w16du") = kDateUtcNamespace;
	const std::string ignorableName = compatibility + ":Ignorable";
	pugi::xml_attribute ignorable = root.attribute(ignorableName.c_str());
	if (!ignorable) ignorable = root.append_attribute(ignorableName.c_str());
	const std::string value = ignorable.value();
	const std::string padded = " " + value + " ";
	if (padded.find(" w16du ") == std::string::npos)
		ignorable.set_value((value.empty() ? std::string("w16du") : value + " w16du").c_str());
	return true;
}

void stamp_revision(pugi::xml_node node, int id, const TrackedChange &tracked) {
	node.append_attribute("w:id") = id;
	node.append_attribute("w:author") = tracked.author.c_str();
	if (tracked.when.empty()) return;
	node.append_attribute("w:date") = tracked.when.c_str();
	if (declare_date_utc(node)) node.append_attribute("w16du:dateUtc") = tracked.when.c_str();
}

// Whether a run holds anything a reader meets: words, a tab, a break, a
// picture, a note's mark, a field. A run of nothing - its properties alone, an
// empty piece of text, the page break Word only remembers from its last layout
// - is not a change anybody made: a revision round it is a mark with nothing
// in it, which Word lists as a change of nothing.
bool run_has_content(const pugi::xml_node &run) {
	for (pugi::xml_node child : run.children()) {
		if (child.type() != pugi::node_element) continue;
		if (local_name_is(child, "rPr") || local_name_is(child, "lastRenderedPageBreak")) continue;
		if (local_name_is(child, "t") || local_name_is(child, "delText")) {
			if (*child.text().as_string() != '\0') return true;
			continue;
		}
		return true;
	}
	return false;
}

// The run, moved inside a revision element of its own. Returns the wrapper -
// none for a run with nothing in it, which is left as it stands.
pugi::xml_node wrap_run(pugi::xml_node run, const char *name, int id, const TrackedChange &tracked) {
	if (!run_has_content(run)) return pugi::xml_node();
	pugi::xml_node parent = run.parent();
	const std::string prefix = prefix_of(run);
	pugi::xml_node wrapper = parent.insert_child_before((prefix + name).c_str(), run);
	stamp_revision(wrapper, id, tracked);
	wrapper.append_move(run);
	return wrapper;
}

// Struck, not removed: the words stay in the file, marked as deleted, so the
// reader sees what was taken out and can put it back. Word reads deleted text
// from w:delText, never from w:t.
void strike_run_text(pugi::xml_node run) {
	const std::string prefix = prefix_of(run);
	std::vector<pugi::xml_node> texts;
	visit(run, [&](pugi::xml_node node) { if (local_name_is(node, "t")) texts.push_back(node); });
	for (pugi::xml_node node : texts) {
		const std::string value = node.text().as_string();
		pugi::xml_node deleted = node.parent().insert_child_before((prefix + "delText").c_str(), node);
		deleted.append_attribute("xml:space") = "preserve";
		deleted.text().set(value.c_str());
		node.parent().remove_child(node);
	}
	// A field's code, struck with its field, is read from w:delInstrText.
	std::vector<pugi::xml_node> codes;
	visit(run, [&](pugi::xml_node node) { if (local_name_is(node, "instrText")) codes.push_back(node); });
	for (pugi::xml_node node : codes) node.set_name((prefix + "delInstrText").c_str());
}

void mark_run_deleted(pugi::xml_node run, int id, const TrackedChange &tracked) {
	if (!run_has_content(run)) return;
	strike_run_text(run);
	wrap_run(run, "del", id, tracked);
}

// Words struck together, as Word strikes them: runs standing side by side in
// one paragraph go into a single w:del - "six " and a bold "(6)" are one
// deletion, not two - and each separate stretch takes the next id from `next`.
// A run with nothing in it is not struck: there is nothing to take out.
template <typename NextId>
void mark_runs_deleted(const std::vector<pugi::xml_node> &runs, NextId &&next, const TrackedChange &tracked) {
	pugi::xml_node open;
	for (pugi::xml_node run : runs) {
		if (!run_has_content(run)) continue;
		if (open && run.previous_sibling() == open) {
			strike_run_text(run);
			open.append_move(run);
			continue;
		}
		mark_run_deleted(run, next(), tracked);
		open = run.parent();
	}
}

void mark_paragraph_mark(pugi::xml_node paragraph, const char *name, int id, const TrackedChange &tracked);

// A paragraph whose mark is itself an insertion, so accepting the change
// accepts the new paragraph and rejecting it takes the whole paragraph away
// rather than leaving an empty line behind. Each run and the mark take an id
// of their own from `next`, in reading order.
template <typename NextId>
void mark_paragraph_inserted(pugi::xml_node paragraph, NextId &&next, const TrackedChange &tracked) {
	std::vector<pugi::xml_node> runs;
	for (pugi::xml_node child : paragraph.children()) if (local_name_is(child, "r")) runs.push_back(child);
	for (pugi::xml_node run : runs) wrap_run(run, "ins", next(), tracked);
	// Where OOXML keeps the mark's revision: first in its run properties, and
	// those before any section break or recorded change of the paragraph's.
	mark_paragraph_mark(paragraph, "ins", next(), tracked);
}

pugi::xml_node run_properties(pugi::xml_node run) {
	for (pugi::xml_node child : run.children()) if (local_name_is(child, "rPr")) return child;
	return run.prepend_child((prefix_of(run) + "rPr").c_str());
}

// The order OOXML writes a run's properties in (CT_RPr, opened by the revision
// marks a paragraph mark's properties carry) and a paragraph's (CT_PPr). Word
// reads both as fixed sequences: a property written out of its place may be
// ignored, or the file offered for repair. So a property is put before the
// first one the sequence places after it, never simply at the end - and never
// after a recorded change, which always comes last.
const std::vector<std::string> &property_order(const pugi::xml_node &properties) {
	static const std::vector<std::string> run{"ins", "del", "moveFrom", "moveTo", "rStyle", "rFonts", "b", "bCs",
		"i", "iCs", "caps", "smallCaps", "strike", "dstrike", "outline", "shadow", "emboss", "imprint", "noProof",
		"snapToGrid", "vanish", "webHidden", "color", "spacing", "w", "kern", "position", "sz", "szCs", "highlight",
		"u", "effect", "bdr", "shd", "fitText", "vertAlign", "rtl", "cs", "em", "lang", "eastAsianLayout",
		"specVanish", "oMath", "rPrChange"};
	static const std::vector<std::string> paragraph{"pStyle", "keepNext", "keepLines", "pageBreakBefore",
		"framePr", "widowControl", "numPr", "suppressLineNumbers", "pBdr", "shd", "tabs", "suppressAutoHyphens",
		"kinsoku", "wordWrap", "overflowPunct", "topLinePunct", "autoSpaceDE", "autoSpaceDN", "bidi",
		"adjustRightInd", "snapToGrid", "spacing", "ind", "contextualSpacing", "mirrorIndents", "suppressOverlap",
		"jc", "textDirection", "textAlignment", "textboxTightWrap", "outlineLvl", "divId", "cnfStyle", "rPr",
		"sectPr", "pPrChange"};
	static const std::vector<std::string> cell{"cnfStyle", "tcW", "gridSpan", "hMerge", "vMerge", "tcBorders", "shd",
		"noWrap", "tcMar", "textDirection", "tcFitText", "vAlign", "hideMark", "headers", "cellIns", "cellDel",
		"cellMerge", "tcPrChange"};
	if (local_name_is(properties, "tcPr")) return cell;
	return local_name_is(properties, "pPr") ? paragraph : run;
}

std::size_t rank_in(const std::vector<std::string> &order, const pugi::xml_node &node) {
	for (std::size_t index = 0; index < order.size(); ++index)
		if (local_name_is(node, order[index].c_str())) return index;
	return std::string::npos;
}

pugi::xml_node add_property(pugi::xml_node properties, const std::string &name) {
	const std::vector<std::string> &order = property_order(properties);
	const auto found = std::find(order.begin(), order.end(), name);
	const std::size_t rank = found == order.end() ? order.size() : static_cast<std::size_t>(found - order.begin());
	const std::string element = prefix_of(properties) + name;
	for (pugi::xml_node child : properties.children()) {
		if (child.type() != pugi::node_element) continue;
		const std::size_t other = rank_in(order, child);
		if (other != std::string::npos && other > rank) return properties.insert_child_before(element.c_str(), child);
	}
	return properties.append_child(element.c_str());
}

void set_toggle(pugi::xml_node properties, const std::string &name, bool on) {
	pugi::xml_node existing;
	for (pugi::xml_node child : properties.children()) if (local_name_is(child, name.c_str())) existing = child;
	if (on) {
		if (!existing) existing = add_property(properties, name);
		if (name == "u") {
			if (!existing.attribute("w:val")) existing.append_attribute("w:val");
			existing.attribute("w:val").set_value("single");
		} else {
			existing.remove_attribute("w:val");
		}
		return;
	}
	if (existing) properties.remove_child(existing);
}

void set_valued(pugi::xml_node properties, const std::string &name, const std::string &value) {
	pugi::xml_node existing;
	for (pugi::xml_node child : properties.children()) if (local_name_is(child, name.c_str())) existing = child;
	if (!existing) existing = add_property(properties, name);
	if (!existing.attribute("w:val")) existing.append_attribute("w:val");
	existing.attribute("w:val").set_value(value.c_str());
}

// Half-points, as OOXML counts a font's size.
std::string half_points(double points) {
	const long long halves = static_cast<long long>(points * 2.0 + (points >= 0 ? 0.5 : -0.5));
	return std::to_string(halves);
}

// Twentieths of a point, as OOXML counts spacing.
std::string twips(double points) {
	const long long value = static_cast<long long>(points * 20.0 + (points >= 0 ? 0.5 : -0.5));
	return std::to_string(value);
}

void apply_run_formatting(pugi::xml_node run, const TextFormatting &formatting) {
	pugi::xml_node properties = run_properties(run);
	if (formatting.bold) set_toggle(properties, "b", *formatting.bold);
	if (formatting.italic) set_toggle(properties, "i", *formatting.italic);
	if (formatting.underline) set_toggle(properties, "u", *formatting.underline);
	if (formatting.color_hex) set_valued(properties, "color", *formatting.color_hex);
	if (formatting.highlight) {
		if (*formatting.highlight == "none") {
			for (pugi::xml_node child : properties.children())
				if (local_name_is(child, "highlight")) { properties.remove_child(child); break; }
		} else {
			set_valued(properties, "highlight", *formatting.highlight);
		}
	}
	if (formatting.size_points) {
		set_valued(properties, "sz", half_points(*formatting.size_points));
		set_valued(properties, "szCs", half_points(*formatting.size_points));
	}
	if (formatting.font_name) {
		pugi::xml_node fonts;
		for (pugi::xml_node child : properties.children()) if (local_name_is(child, "rFonts")) fonts = child;
		if (!fonts) fonts = add_property(properties, "rFonts");
		for (const char *attribute : {"w:ascii", "w:hAnsi", "w:cs"}) {
			if (!fonts.attribute(attribute)) fonts.append_attribute(attribute);
			fonts.attribute(attribute).set_value(formatting.font_name->c_str());
		}
		// A theme font named beside a font wins over it in Word: a document set in
		// the theme's body font would otherwise go on showing it.
		for (const char *theme : {"w:asciiTheme", "w:hAnsiTheme", "w:cstheme"}) fonts.remove_attribute(theme);
	}
}

pugi::xml_node paragraph_properties(pugi::xml_node paragraph) {
	for (pugi::xml_node child : paragraph.children()) if (local_name_is(child, "pPr")) return child;
	return paragraph.prepend_child((prefix_of(paragraph) + "pPr").c_str());
}

void apply_paragraph_formatting(pugi::xml_node paragraph, const TextFormatting &formatting) {
	pugi::xml_node properties = paragraph_properties(paragraph);
	if (formatting.paragraph_style) set_valued(properties, "pStyle", *formatting.paragraph_style);
	// The contract says "justify", as a lawyer does; Word's file says "both",
	// and a w:jc of "justify" is not one Word reads.
	if (formatting.alignment)
		set_valued(properties, "jc", *formatting.alignment == "justify" ? std::string("both") : *formatting.alignment);
	if (formatting.space_before_points || formatting.space_after_points) {
		pugi::xml_node spacing;
		for (pugi::xml_node child : properties.children()) if (local_name_is(child, "spacing")) spacing = child;
		if (!spacing) spacing = add_property(properties, "spacing");
		if (formatting.space_before_points) {
			if (!spacing.attribute("w:before")) spacing.append_attribute("w:before");
			spacing.attribute("w:before").set_value(twips(*formatting.space_before_points).c_str());
		}
		if (formatting.space_after_points) {
			if (!spacing.attribute("w:after")) spacing.append_attribute("w:after");
			spacing.attribute("w:after").set_value(twips(*formatting.space_after_points).c_str());
		}
	}
}

// An element as XML text, for telling whether it is still what it was.
std::string xml_text_of(const pugi::xml_node &node) {
	struct Sink : pugi::xml_writer {
		std::string out;
		void write(const void *data, std::size_t size) override { out.append(static_cast<const char *>(data), size); }
	} sink;
	node.print(sink, "", pugi::format_raw);
	return sink.out;
}

// A formatting change recorded as Word records one with Track Changes on: the
// properties as they stood are kept in a w:rPrChange (a paragraph's in a
// w:pPrChange), last among the new ones, so the partner sees "Formatted: Bold"
// in the redline and can reject it. A change already recorded there keeps its
// record: it holds the formatting the document had before anyone changed it,
// which is what rejecting must restore.
void record_formatting_change(pugi::xml_node properties, const std::function<void()> &apply, int id,
		const TrackedChange &tracked) {
	const bool paragraph = local_name_is(properties, "pPr");
	const char *changeName = paragraph ? "pPrChange" : "rPrChange";
	for (pugi::xml_node child : properties.children()) {
		if (local_name_is(child, changeName)) { apply(); return; }
	}
	pugi::xml_document held;
	const pugi::xml_node before = held.append_copy(properties);
	apply();
	// Asked for what the words already have - bold on words already bold -
	// nothing changed, and a revision of nothing is not recorded.
	if (xml_text_of(before) == xml_text_of(properties)) return;
	pugi::xml_node change = properties.append_child((prefix_of(properties) + changeName).c_str());
	stamp_revision(change, id, tracked);
	pugi::xml_node was = change.append_child(properties.name());
	for (pugi::xml_node child : before.children()) {
		// A paragraph's recorded properties are its own: not its mark's run
		// properties, and not a section break.
		if (paragraph && (local_name_is(child, "rPr") || local_name_is(child, "sectPr"))) continue;
		was.append_copy(child);
	}
}

// Whether the next thing in this paragraph's body or cell is another
// paragraph: only then can this one's mark be struck, the paragraph after it
// taking its place when the deletion is accepted. The last paragraph of a body
// or a cell, or one a table follows, keeps its mark - Word keeps it too.
bool paragraph_follows(pugi::xml_node paragraph) {
	for (pugi::xml_node next = paragraph.next_sibling(); next; next = next.next_sibling()) {
		if (next.type() != pugi::node_element) continue;
		if (local_name_is(next, "p")) return true;
		if (local_name_is(next, "tbl") || local_name_is(next, "sdt") || local_name_is(next, "sectPr")
				|| local_name_is(next, "customXml") || local_name_is(next, "altChunk"))
			return false;
	}
	return false;
}

bool paragraph_has_text(pugi::xml_node paragraph) {
	bool any = false;
	visit(paragraph, [&](pugi::xml_node node) {
		if (local_name_is(node, "t") && *node.text().as_string() != '\0') any = true;
	});
	return any;
}

// Every run a paragraph still shows - words, a note's mark, a picture, a
// field - that is its own: none already struck, none inside a text box a run
// of it holds. A paragraph taken out whole is struck with all of them, so its
// footnote goes with it rather than to the head of the clause after it.
std::vector<pugi::xml_node> runs_of_paragraph(pugi::xml_node paragraph) {
	std::vector<pugi::xml_node> runs;
	visit(paragraph, [&](pugi::xml_node node) {
		if (!local_name_is(node, "r")) return;
		for (pugi::xml_node up = node.parent(); up && up != paragraph; up = up.parent())
			if (local_name_is(up, "del") || local_name_is(up, "moveFrom") || local_name_is(up, "r")) return;
		runs.push_back(node);
	});
	return runs;
}

// The revision of one kind recorded on a paragraph's mark, if any.
pugi::xml_node paragraph_mark_revision(pugi::xml_node paragraph, const char *name) {
	for (pugi::xml_node properties : paragraph.children()) {
		if (!local_name_is(properties, "pPr")) continue;
		for (pugi::xml_node runProperties : properties.children()) {
			if (!local_name_is(runProperties, "rPr")) continue;
			for (pugi::xml_node child : runProperties.children()) if (local_name_is(child, name)) return child;
		}
	}
	return pugi::xml_node();
}

// Paragraphs added after the last paragraph of a body or a cell, recorded as
// Word records Enter pressed at its end: the mark that was last stays last -
// Word cannot take it away - and it is the mark of the paragraph the new ones
// follow that is new. Rejecting the change then takes the new paragraphs away
// whole, and no empty paragraph, or empty numbered item, is left where they
// stood. The last mark now closes the last new paragraph, so its properties as
// they stood are recorded on it (w:pPrChange), and rejecting gives the old
// paragraph back its own place in the list.
//
// `anchor` is the paragraph that was last; `last`, the last one added after it.
template <typename NextId>
void keep_final_mark_last(pugi::xml_node anchor, pugi::xml_node last, NextId &&next, const TrackedChange &tracked) {
	if (!anchor || !last || anchor == last || paragraph_follows(last)) return;
	if (paragraph_mark_revision(anchor, "del")) return;
	pugi::xml_node inserted = paragraph_mark_revision(last, "ins");
	if (!inserted) return;
	const int id = inserted.attribute("w:id").as_int();
	inserted.parent().remove_child(inserted);
	if (!paragraph_mark_revision(anchor, "ins")) mark_paragraph_mark(anchor, "ins", id, tracked);
	pugi::xml_node was = paragraph_properties(anchor);
	pugi::xml_node now = paragraph_properties(last);
	pugi::xml_node recorded;
	for (pugi::xml_node child : now.children()) if (local_name_is(child, "pPrChange")) recorded = child;
	// A section break the old last paragraph closed is closed by the new one.
	for (pugi::xml_node child : was.children()) {
		if (!local_name_is(child, "sectPr")) continue;
		remove_children_named(now, {"sectPr"});
		if (recorded) now.insert_move_before(child, recorded);
		else now.append_move(child);
		break;
	}
	if (recorded) return;
	// What the old paragraph's properties were: as someone's own recorded
	// change of them says, or as they stand.
	pugi::xml_node source = was;
	for (pugi::xml_node child : was.children()) {
		if (!local_name_is(child, "pPrChange")) continue;
		for (pugi::xml_node inner : child.children()) if (local_name_is(inner, "pPr")) source = inner;
	}
	const auto own = [](pugi::xml_node properties) {
		std::string text;
		for (pugi::xml_node child : properties.children()) {
			if (local_name_is(child, "rPr") || local_name_is(child, "sectPr") || local_name_is(child, "pPrChange")) continue;
			text += xml_text_of(child);
		}
		return text;
	};
	if (own(source) == own(now)) return;
	pugi::xml_node change = now.append_child((prefix_of(now) + "pPrChange").c_str());
	stamp_revision(change, next(), tracked);
	pugi::xml_node held = change.append_child(now.name());
	for (pugi::xml_node child : source.children()) {
		if (local_name_is(child, "rPr") || local_name_is(child, "sectPr") || local_name_is(child, "pPrChange")) continue;
		held.append_copy(child);
	}
}

// ---- Table revisions ------------------------------------------------------

// The row's properties, where OOXML keeps them: after any table-property
// exceptions, before the cells.
pugi::xml_node row_properties(pugi::xml_node row) {
	for (pugi::xml_node child : row.children()) if (local_name_is(child, "trPr")) return child;
	const std::string name = prefix_of(row) + "trPr";
	pugi::xml_node exceptions;
	for (pugi::xml_node child : row.children()) if (local_name_is(child, "tblPrEx")) exceptions = child;
	return exceptions ? row.insert_child_after(name.c_str(), exceptions) : row.prepend_child(name.c_str());
}

// w:ins or w:del on the row itself, after its other properties and before any
// recorded change to them.
void mark_row(pugi::xml_node row, const char *name, int id, const TrackedChange &tracked) {
	pugi::xml_node properties = row_properties(row);
	const std::string element = prefix_of(row) + name;
	pugi::xml_node change;
	for (pugi::xml_node child : properties.children()) if (local_name_is(child, "trPrChange")) change = child;
	stamp_revision(change ? properties.insert_child_before(element.c_str(), change)
		: properties.append_child(element.c_str()), id, tracked);
}

// w:ins or w:del on a paragraph mark. In the mark's run properties a revision
// comes first: w:ins, then w:del, then the formatting.
void mark_paragraph_mark(pugi::xml_node paragraph, const char *name, int id, const TrackedChange &tracked) {
	const std::string prefix = prefix_of(paragraph);
	pugi::xml_node properties;
	for (pugi::xml_node child : paragraph.children()) if (local_name_is(child, "pPr")) properties = child;
	if (!properties) properties = paragraph.prepend_child((prefix + "pPr").c_str());
	pugi::xml_node runProperties;
	pugi::xml_node limit;
	for (pugi::xml_node child : properties.children()) {
		if (local_name_is(child, "rPr")) runProperties = child;
		if (!limit && (local_name_is(child, "sectPr") || local_name_is(child, "pPrChange"))) limit = child;
	}
	if (!runProperties) runProperties = limit ? properties.insert_child_before((prefix + "rPr").c_str(), limit)
		: properties.append_child((prefix + "rPr").c_str());
	pugi::xml_node inserted;
	for (pugi::xml_node child : runProperties.children()) if (local_name_is(child, "ins")) inserted = child;
	const std::string element = prefix + name;
	pugi::xml_node mark = std::strcmp(name, "del") == 0 && inserted
		? runProperties.insert_child_after(element.c_str(), inserted)
		: runProperties.prepend_child(element.c_str());
	stamp_revision(mark, id, tracked);
}

// The runs a reader still sees inside `node`: none already struck.
std::vector<pugi::xml_node> visible_runs(pugi::xml_node node) {
	std::vector<pugi::xml_node> runs;
	visit(node, [&](pugi::xml_node child) {
		if (!local_name_is(child, "r")) return;
		for (pugi::xml_node up = child.parent(); up && up != node; up = up.parent())
			if (local_name_is(up, "del")) return;
		runs.push_back(child);
	});
	return runs;
}

std::vector<pugi::xml_node> descendants_named(pugi::xml_node node, const char *name) {
	std::vector<pugi::xml_node> found;
	visit(node, [&](pugi::xml_node child) { if (local_name_is(child, name)) found.push_back(child); });
	return found;
}

// The part of one paragraph a replaced passage covers: [from, to) of the flat
// text, and whether the paragraph keeps words before it (`head`) or after it
// (`tail`). `split` is the second half of a paragraph split where the passage
// ended: it holds the words that followed the passage and nothing of it.
struct ParagraphSegment {
	pugi::xml_node paragraph;
	std::size_t from = 0;
	std::size_t to = 0;
	bool head = false;
	bool tail = false;
	bool split = false;
};

// Every revision `author` recorded from id `first` on, accepted as Word
// accepts one: struck words gone, and a paragraph whose mark is struck joined
// to the paragraph after it; inserted words and marks kept as ordinary text.
// Revisions anybody else recorded are left exactly as they stand.
void accept_own_revisions(pugi::xml_node root, int first, const std::string &author) {
	std::vector<pugi::xml_node> revisions;
	visit(root, [&](pugi::xml_node node) {
		if (!local_name_is(node, "ins") && !local_name_is(node, "del") && !local_name_is(node, "pPrChange")) return;
		if (node.attribute("w:id").as_int() < first || author != node.attribute("w:author").as_string()) return;
		revisions.push_back(node);
	});
	std::vector<pugi::xml_node> joined;
	for (auto it = revisions.rbegin(); it != revisions.rend(); ++it) {
		pugi::xml_node revision = *it;
		pugi::xml_node parent = revision.parent();
		// A paragraph's formatting change, accepted: the paragraph as it now is.
		if (local_name_is(revision, "pPrChange")) {
			parent.remove_child(revision);
			continue;
		}
		if (local_name_is(parent, "rPr")) {
			// A paragraph mark's revision: w:p / w:pPr / w:rPr / w:ins|w:del.
			const pugi::xml_node paragraph = parent.parent().parent();
			if (local_name_is(revision, "del") && local_name_is(paragraph, "p")) joined.push_back(paragraph);
			parent.remove_child(revision);
			continue;
		}
		if (local_name_is(revision, "ins")) {
			while (pugi::xml_node child = revision.first_child()) parent.insert_move_before(child, revision);
		}
		parent.remove_child(revision);
	}
	// In document order, so a run of joined paragraphs ends in the last one.
	for (auto it = joined.rbegin(); it != joined.rend(); ++it) {
		pugi::xml_node paragraph = *it;
		pugi::xml_node next = paragraph.next_sibling();
		while (next && !local_name_is(next, "p")) next = next.next_sibling();
		if (!next) continue;
		pugi::xml_node before;
		for (pugi::xml_node child : next.children()) {
			if (!local_name_is(child, "pPr")) { before = child; break; }
		}
		std::vector<pugi::xml_node> content;
		for (pugi::xml_node child : paragraph.children()) if (!local_name_is(child, "pPr")) content.push_back(child);
		for (pugi::xml_node child : content) {
			if (before) next.insert_move_before(child, before);
			else next.append_move(child);
		}
		paragraph.parent().remove_child(paragraph);
	}
}

} // namespace

namespace {

// Whether a row (w:trPr) or a paragraph mark (w:pPr/w:rPr) already carries a
// revision of this kind: a second one there is not one Word reads.
bool row_marked(pugi::xml_node row, const char *name) {
	for (pugi::xml_node properties : row.children()) {
		if (!local_name_is(properties, "trPr")) continue;
		for (pugi::xml_node child : properties.children()) if (local_name_is(child, name)) return true;
	}
	return false;
}

bool paragraph_mark_marked(pugi::xml_node paragraph, const char *name) {
	for (pugi::xml_node properties : paragraph.children()) {
		if (!local_name_is(properties, "pPr")) continue;
		for (pugi::xml_node runProperties : properties.children()) {
			if (!local_name_is(runProperties, "rPr")) continue;
			for (pugi::xml_node child : runProperties.children()) if (local_name_is(child, name)) return true;
		}
	}
	return false;
}

// A row added or removed carries the rows of every table inside it: each is
// marked the same way. Word otherwise reads the inner rows as kept while
// their words go, and walks the revision round and round - its Reviewing
// Pane and its "Next" change never come to an end.
void track_row(pugi::xml_node row, const char *name, const TrackedChange &tracked) {
	int id = next_revision_id(row.root());
	if (!row_marked(row, name)) mark_row(row, name, id++, tracked);
	for (pugi::xml_node inner : descendants_named(row, "tr"))
		if (!row_marked(inner, name)) mark_row(inner, name, id++, tracked);
	const bool inserting = std::strcmp(name, "ins") == 0;
	for (pugi::xml_node run : visible_runs(row)) {
		if (inserting) wrap_run(run, "ins", id++, tracked);
		else mark_run_deleted(run, id++, tracked);
	}
	for (pugi::xml_node paragraph : descendants_named(row, "p"))
		if (!paragraph_mark_marked(paragraph, name)) mark_paragraph_mark(paragraph, name, id++, tracked);
}

} // namespace

void track_inserted_row(pugi::xml_node row, const TrackedChange &tracked) {
	track_row(row, "ins", tracked);
}

void track_deleted_row(pugi::xml_node row, const TrackedChange &tracked) {
	track_row(row, "del", tracked);
}

void replace_cell_text_tracked(pugi::xml_node cell, const std::string &text, const TrackedChange &tracked) {
	std::vector<pugi::xml_node> paragraphs;
	for (pugi::xml_node child : cell.children()) if (local_name_is(child, "p")) paragraphs.push_back(child);
	if (paragraphs.empty()) paragraphs.push_back(cell.append_child((prefix_of(cell) + "p").c_str()));
	const std::vector<pugi::xml_node> runs = visible_runs(cell);
	// Written like the cell's own first words: the formatting of the run they
	// stand in, none where they have none - never the bold of a lead-in further
	// down the cell. A cell with no words yet keeps the look it was given.
	pugi::xml_node style;
	bool wordsFound = false;
	for (pugi::xml_node run : runs) {
		bool hasWords = false;
		for (pugi::xml_node child : run.children())
			if (local_name_is(child, "t") && *child.text().as_string() != '\0') hasWords = true;
		if (!hasWords) continue;
		wordsFound = true;
		for (pugi::xml_node child : run.children()) if (local_name_is(child, "rPr")) style = child;
		break;
	}
	if (!wordsFound) {
		for (pugi::xml_node run : runs) {
			for (pugi::xml_node child : run.children()) if (local_name_is(child, "rPr")) style = child;
			if (style) break;
		}
	}
	// Each paragraph's words, as the cell reads them now.
	struct Piece {
		pugi::xml_node node;
		std::size_t start = 0;
		std::size_t length = 0;
	};
	std::vector<Piece> pieces;
	std::vector<std::string> texts;
	for (pugi::xml_node paragraph : paragraphs) {
		std::string was;
		visit(paragraph, [&](pugi::xml_node child) {
			if (!local_name_is(child, "t")) return;
			const std::string value = child.text().as_string();
			if (texts.empty()) pieces.push_back({child, was.size(), value.size()});
			was += value;
		});
		texts.push_back(std::move(was));
	}
	// The same rule the preview of the change is read by, so what a lawyer
	// approves is what Word shows.
	const CellTextRevision revision = cell_text_revision(texts, text);
	if (revision.unchanged) return;
	int id = next_revision_id(cell.root());
	// A value that keeps some of its words keeps them as they stand: only the
	// words that differ are struck and written, exactly as inside a paragraph,
	// so "6 months of fees" becoming "12 months of fees" marks the number
	// alone - the way Word records a lawyer typing over it.
	if (!revision.kept_before.empty() || !revision.kept_after.empty()) {
		const std::size_t from = revision.kept_before.size();
		const std::size_t to = from + revision.removed.size();
		std::vector<pugi::xml_node> struck;
		for (const Piece &piece : pieces) {
			const std::size_t end = piece.start + piece.length;
			if (end <= from || piece.start >= to) continue;
			const pugi::xml_node middle = run_for_span(piece.node, from > piece.start ? from - piece.start : 0,
				to < end ? to - piece.start : piece.length);
			if (middle) struck.push_back(middle);
		}
		if (!struck.empty()) {
			if (!revision.added.empty()) {
				// Written like the first words it replaces, beside them.
				pugi::xml_node last = struck.back();
				pugi::xml_node inserted = last.parent().insert_copy_after(struck.front(), last);
				std::vector<pugi::xml_node> remove;
				for (pugi::xml_node child : inserted.children()) if (!local_name_is(child, "rPr")) remove.push_back(child);
				for (pugi::xml_node child : remove) inserted.remove_child(child);
				for (pugi::xml_node child : inserted.children()) if (local_name_is(child, "rPr")) drop_what_belongs_to_the_model(child);
				set_run_text_with_lines(inserted.append_child((prefix_of(inserted) + "t").c_str()), revision.added);
				mark_runs_deleted(struck, [&]() { return id++; }, tracked);
				wrap_run(inserted, "ins", id++, tracked);
			} else {
				mark_runs_deleted(struck, [&]() { return id++; }, tracked);
			}
			return;
		}
	}
	pugi::xml_node run;
	if (!revision.added.empty()) {
		// Written before the old words are struck, so it takes their formatting
		// as it stands, never a formatting change of theirs. What is written is
		// the value as the document writes it - its own quotation marks, no
		// marks a model typed around a word.
		const std::string prefix = prefix_of(paragraphs.front());
		run = paragraphs.front().append_child((prefix + "r").c_str());
		if (style) {
			pugi::xml_node copy = run.append_copy(style);
			for (pugi::xml_node change : descendants_named(copy, "rPrChange")) change.parent().remove_child(change);
		}
		// A value of several lines keeps its lines, as the preview showed them.
		set_run_text_with_lines(run.append_child((prefix + "t").c_str()), revision.added);
	}
	mark_runs_deleted(runs, [&]() { return id++; }, tracked);
	// A cell of several paragraphs becomes one when the change is accepted.
	for (std::size_t index = 0; index + 1 < paragraphs.size(); ++index)
		mark_paragraph_mark(paragraphs[index], "del", id++, tracked);
	if (run) wrap_run(run, "ins", id++, tracked);
}

void track_inserted_paragraph(pugi::xml_node paragraph, const TrackedChange &tracked) {
	int id = next_revision_id(paragraph.root());
	for (pugi::xml_node run : visible_runs(paragraph)) wrap_run(run, "ins", id++, tracked);
	mark_paragraph_mark(paragraph, "ins", id++, tracked);
}

void accept_revisions_within(pugi::xml_node node) {
	// Innermost first, so nothing is touched after an element around it went.
	std::vector<pugi::xml_node> revisions;
	visit(node, [&](pugi::xml_node child) {
		if (local_name_is(child, "ins") || local_name_is(child, "del") || local_name_is(child, "rPrChange")
			|| local_name_is(child, "pPrChange") || local_name_is(child, "trPrChange") || local_name_is(child, "tcPrChange"))
			revisions.push_back(child);
	});
	for (auto it = revisions.rbegin(); it != revisions.rend(); ++it) {
		pugi::xml_node revision = *it;
		pugi::xml_node parent = revision.parent();
		const bool isMark = local_name_is(parent, "rPr") || local_name_is(parent, "trPr") || local_name_is(parent, "pPr")
			|| local_name_is(parent, "tcPr");
		if (local_name_is(revision, "ins") && !isMark) {
			while (pugi::xml_node child = revision.first_child()) parent.insert_move_before(child, revision);
		}
		parent.remove_child(revision);
	}
}

namespace {

bool separates_words(char character) {
	return character == ' ' || character == '\t' || character == '\n';
}

bool closes_sentence_part(char character) {
	return character == ',' || character == '.' || character == ';' || character == ':' || character == '!' || character == '?';
}

// Whether the marks from `at` close the word before them - a comma, a full
// stop, a semicolon - with a space or the end after them: "evaluating," is the
// word and its comma. A mark inside a word - "5.1", "U.S" - closes nothing,
// and a bracket belongs to the words it holds: "(5)" is one.
bool closing_marks_at(const std::string &text, std::size_t at) {
	if (at == 0 || at >= text.size() || !closes_sentence_part(text[at])) return false;
	std::size_t end = at;
	while (end < text.size() && closes_sentence_part(text[end])) ++end;
	return end >= text.size() || separates_words(text[end]);
}

// The words of a passage, a word's closing marks counted as a word of their
// own, each by where it stands: [first, second).
std::vector<std::pair<std::size_t, std::size_t>> revision_words(const std::string &text) {
	std::vector<std::pair<std::size_t, std::size_t>> words;
	std::size_t at = 0;
	while (at < text.size()) {
		if (separates_words(text[at])) {
			++at;
			continue;
		}
		const std::size_t from = at;
		while (at < text.size() && !separates_words(text[at]) && !closing_marks_at(text, at)) ++at;
		if (at > from) words.emplace_back(from, at);
		if (at < text.size() && closing_marks_at(text, at)) {
			const std::size_t marks = at;
			while (at < text.size() && closes_sentence_part(text[at])) ++at;
			words.emplace_back(marks, at);
		}
	}
	return words;
}

} // namespace

RevisionSpan word_revision_span(const std::string &was, const std::string &now) {
	// A boundary is the end of the text or a space - a tab or a line break
	// separates words the same way - or a word's closing comma or full stop:
	// "evaluating and" becoming "evaluating," strikes " and" and writes ",",
	// never the word both keep (a live run struck "evaluating and" to write
	// "evaluating,", 26 Oct 2026). Backing off to one is what keeps whole words
	// on both sides of the mark; backing off when the match already ended at
	// one would throw away a word that really is common to both.
	const auto endsCleanly = [&](const std::string &text, std::size_t at) {
		return at >= text.size() || separates_words(text[at]) || closing_marks_at(text, at);
	};
	const auto beginsCleanly = [&](const std::string &text, std::size_t fromEnd) {
		if (fromEnd >= text.size()) return true;
		const std::size_t at = text.size() - fromEnd;
		return separates_words(text[at - 1]) || closing_marks_at(text, at);
	};
	std::size_t shared = 0;
	while (shared < was.size() && shared < now.size() && was[shared] == now[shared]) ++shared;
	while (shared > 0 && !(endsCleanly(was, shared) && endsCleanly(now, shared))) --shared;
	std::size_t tail = 0;
	while (tail < was.size() - shared && tail < now.size() - shared
			&& was[was.size() - 1 - tail] == now[now.size() - 1 - tail]) ++tail;
	while (tail > 0 && !(beginsCleanly(was, tail) && beginsCleanly(now, tail))) --tail;
	return {shared, tail};
}

std::vector<RevisionHunk> word_revision_hunks(const std::string &was, const std::string &now) {

	const std::vector<std::pair<std::size_t, std::size_t>> old = revision_words(was);
	const std::vector<std::pair<std::size_t, std::size_t>> fresh = revision_words(now);
	const std::size_t n = old.size();
	const std::size_t m = fresh.size();
	// A paragraph's words, never a document's: the table below grows with the
	// product of the two counts.
	if (n == 0 || m == 0 || n * m > 160000) return {};
	const auto same = [&](std::size_t i, std::size_t j) {
		return old[i].second - old[i].first == fresh[j].second - fresh[j].first
			&& was.compare(old[i].first, old[i].second - old[i].first, now, fresh[j].first, fresh[j].second - fresh[j].first) == 0;
	};
	// The most of `was` both keep, in order, a word weighing its length: kept
	// "negotiating" outweighs kept "and", as a lawyer reads the change.
	std::vector<std::size_t> kept((n + 1) * (m + 1), 0);
	const auto at = [&](std::size_t i, std::size_t j) -> std::size_t & { return kept[i * (m + 1) + j]; };
	for (std::size_t i = n; i-- > 0;) {
		for (std::size_t j = m; j-- > 0;) {
			std::size_t best = std::max(at(i + 1, j), at(i, j + 1));
			if (same(i, j)) best = std::max(best, old[i].second - old[i].first + at(i + 1, j + 1));
			at(i, j) = best;
		}
	}
	std::size_t weight = 0;
	for (const auto &word : old) weight += word.second - word.first;
	// Mostly rewritten: one mark from the first difference to the last reads
	// better than a confetti of them.
	if (at(0, 0) * 2 < weight) return {};

	std::vector<RevisionHunk> hunks;
	std::size_t i = 0;
	std::size_t j = 0;
	std::size_t wasFrom = 0;
	std::size_t nowFrom = 0;
	bool isOpen = false;
	const auto close = [&](std::size_t wasTo, std::size_t nowTo) {
		if (!isOpen) return;
		RevisionHunk hunk{wasFrom, wasTo, nowFrom, nowTo};
		// A space both sides share stands outside the mark, as it does in Word.
		while (hunk.was_to > hunk.was_from && hunk.now_to > hunk.now_from && separates_words(was[hunk.was_to - 1])
				&& was[hunk.was_to - 1] == now[hunk.now_to - 1]) {
			--hunk.was_to;
			--hunk.now_to;
		}
		while (hunk.was_to > hunk.was_from && hunk.now_to > hunk.now_from && separates_words(was[hunk.was_from])
				&& was[hunk.was_from] == now[hunk.now_from]) {
			++hunk.was_from;
			++hunk.now_from;
		}
		if (hunk.was_to > hunk.was_from || hunk.now_to > hunk.now_from) hunks.push_back(hunk);
		isOpen = false;
	};
	std::size_t wasKept = 0;
	std::size_t nowKept = 0;
	while (i < n || j < m) {
		if (i < n && j < m && same(i, j) && at(i, j) == old[i].second - old[i].first + at(i + 1, j + 1)) {
			close(old[i].first, fresh[j].first);
			wasKept = old[i].second;
			nowKept = fresh[j].second;
			++i;
			++j;
			continue;
		}
		if (!isOpen) {
			isOpen = true;
			wasFrom = wasKept;
			nowFrom = nowKept;
		}
		if (j >= m || (i < n && at(i + 1, j) >= at(i, j + 1))) ++i;
		else ++j;
	}
	close(was.size(), now.size());
	return hunks;

}

namespace {

struct CodePoint {
	std::uint32_t value = 0;
	std::size_t start = 0;
};

std::vector<CodePoint> code_points(const std::string &text) {
	std::vector<CodePoint> points;
	std::size_t at = 0;
	while (at < text.size()) {
		const unsigned char lead = static_cast<unsigned char>(text[at]);
		const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 1;
		std::uint32_t value = length == 1 ? lead : length == 2 ? (lead & 0x1f) : length == 3 ? (lead & 0x0f) : (lead & 0x07);
		for (std::size_t index = 1; index < length && at + index < text.size(); ++index)
			value = (value << 6) | (static_cast<unsigned char>(text[at + index]) & 0x3f);
		points.push_back({value, at});
		at += std::min(length, text.size() - at);
	}
	return points;
}

// Typographic variants of one character, counted as that character.
std::uint32_t typographic_class(std::uint32_t value) {
	switch (value) {
		case 0x2018: case 0x2019: case 0x201b: case 0x02bc: case 0x2032: case 0x0060: return '\'';
		case 0x201c: case 0x201d: case 0x201e: case 0x201f: case 0x2033: return '"';
		case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
		case 0x00a0: case 0x2007: case 0x2009: case 0x202f: return ' ';
		default: return value;
	}
}

} // namespace

std::string with_document_typography(const std::string &was, const std::string &now) {
	const std::vector<CodePoint> before = code_points(was);
	const std::vector<CodePoint> after = code_points(now);
	std::size_t head = 0;
	while (head < before.size() && head < after.size()
			&& typographic_class(before[head].value) == typographic_class(after[head].value)) ++head;
	std::size_t tail = 0;
	while (tail < before.size() - head && tail < after.size() - head
			&& typographic_class(before[before.size() - 1 - tail].value)
				== typographic_class(after[after.size() - 1 - tail].value)) ++tail;
	const std::size_t wasHeadEnd = head < before.size() ? before[head].start : was.size();
	const std::size_t wasTailStart = tail == 0 ? was.size() : before[before.size() - tail].start;
	const std::size_t nowHeadEnd = head < after.size() ? after[head].start : now.size();
	const std::size_t nowTailStart = tail == 0 ? now.size() : after[after.size() - tail].start;
	return was.substr(0, wasHeadEnd) + now.substr(nowHeadEnd, nowTailStart - nowHeadEnd) + was.substr(wasTailStart);
}

CellTextRevision cell_text_revision(const std::vector<std::string> &paragraphs, const std::string &text) {
	CellTextRevision revision;
	std::string plain = prose_plain_text(text);
	// A value that ends its last line with a line break has ended; the break is
	// not an empty line to write into the cell.
	while (!plain.empty() && (plain.back() == '\n' || plain.back() == '\r')) plain.pop_back();
	if (paragraphs.size() == 1 && !text.empty()) {
		const std::string &was = paragraphs.front();
		const std::string now = with_document_typography(was, plain);
		// Word for word what the cell says already: nothing to record.
		if (now == was) {
			revision.unchanged = true;
			revision.kept_before = was;
			return revision;
		}
		const RevisionSpan span = word_revision_span(was, now);
		std::size_t from = span.shared_prefix;
		std::size_t to = was.size() - span.shared_suffix;
		std::string added = now.substr(span.shared_prefix, now.size() - span.shared_prefix - span.shared_suffix);
		// A space both sides share stands outside the mark, as it does in Word.
		while (to > from && was[to - 1] == ' ' && !added.empty() && added.back() == ' ') {
			--to;
			added.pop_back();
		}
		while (to > from && was[from] == ' ' && !added.empty() && added.front() == ' ') {
			++from;
			added.erase(0, 1);
		}
		if (!was.empty() && (span.shared_prefix > 0 || span.shared_suffix > 0) && to > from) {
			revision.kept_before = was.substr(0, from);
			revision.removed = was.substr(from, to - from);
			revision.added = added;
			revision.kept_after = was.substr(to);
			return revision;
		}
		// Nothing kept at either end, or nothing taken out: the cell is struck
		// whole and its new value written whole.
		revision.removed = was;
		revision.added = now;
		return revision;
	}
	// A cell of several paragraphs becomes one: all of it is struck.
	for (const std::string &paragraph : paragraphs) {
		if (paragraph.empty()) continue;
		if (!revision.removed.empty()) revision.removed += ' ';
		revision.removed += paragraph;
	}
	if (!text.empty()) revision.added = plain;
	return revision;
}

bool row_is_deleted_revision(const pugi::xml_node &row) {
	for (pugi::xml_node properties : row.children()) {
		if (!local_name_is(properties, "trPr")) continue;
		for (pugi::xml_node child : properties.children()) if (local_name_is(child, "del")) return true;
	}
	return false;
}

bool make_ordinary_row(pugi::xml_node row) {
	bool isHeading = false;
	for (pugi::xml_node properties : row.children()) {
		if (!local_name_is(properties, "trPr")) continue;
		for (pugi::xml_node child : properties.children()) {
			if (!local_name_is(child, "tblHeader")) continue;
			const pugi::xml_attribute value = child.attribute("w:val") ? child.attribute("w:val") : child.attribute("val");
			const std::string said = value.as_string();
			if (said.empty() || said == "1" || said == "true" || said == "on") isHeading = true;
		}
		if (isHeading) remove_children_named(properties, {"tblHeader", "cnfStyle"});
	}
	if (!isHeading) return false;
	// The heading's own look: its cells' shading, the bold of its words and of
	// its paragraph marks, and the table style's "first row" it claimed.
	visit(row, [&](pugi::xml_node node) {
		if (local_name_is(node, "tcPr")) remove_children_named(node, {"shd", "cnfStyle"});
		else if (local_name_is(node, "pPr")) remove_children_named(node, {"cnfStyle"});
		else if (local_name_is(node, "rPr")) remove_children_named(node, {"b", "bCs"});
	});
	return true;
}

namespace {

// A row's own cells, through the content controls and custom XML a row may
// wrap them in - never the cells of a table inside one of them.
void own_cells(pugi::xml_node node, std::vector<pugi::xml_node> &cells) {
	for (pugi::xml_node child : node.children()) {
		if (local_name_is(child, "tc")) cells.push_back(child);
		else if (local_name_is(child, "sdt") || local_name_is(child, "sdtContent") || local_name_is(child, "customXml"))
			own_cells(child, cells);
	}
}

// How a paragraph writes its first words: the properties of its first run of
// its own that holds words - not a link's, whose look is the link's - else
// those of its paragraph mark, which Word gives to what is typed into an
// empty paragraph.
pugi::xml_node first_words_look(pugi::xml_node paragraph) {
	for (pugi::xml_node run : paragraph.children()) {
		if (!local_name_is(run, "r")) continue;
		pugi::xml_node properties;
		bool words = false;
		for (pugi::xml_node child : run.children()) {
			if (local_name_is(child, "rPr")) properties = child;
			if (local_name_is(child, "t") && *child.text().as_string() != '\0') words = true;
		}
		if (words) return properties;
	}
	for (pugi::xml_node properties : paragraph.children()) {
		if (!local_name_is(properties, "pPr")) continue;
		for (pugi::xml_node child : properties.children()) if (local_name_is(child, "rPr")) return child;
	}
	return {};
}

} // namespace

void empty_row_for_entry(pugi::xml_node row) {
	std::vector<pugi::xml_node> cells;
	own_cells(row, cells);
	for (pugi::xml_node cell : cells) {
		const std::string prefix = prefix_of(cell);
		// The cell is the new entry's own: no merge carried on from the row above.
		for (pugi::xml_node properties : cell.children())
			if (local_name_is(properties, "tcPr")) remove_children_named(properties, {"vMerge", "hMerge"});
		pugi::xml_node paragraph;
		for (pugi::xml_node child : cell.children()) {
			if (local_name_is(child, "p")) { paragraph = child; break; }
		}
		std::vector<pugi::xml_node> gone;
		for (pugi::xml_node child : cell.children())
			if (child != paragraph && !local_name_is(child, "tcPr")) gone.push_back(child);
		for (pugi::xml_node child : gone) cell.remove_child(child);
		if (!paragraph) paragraph = cell.append_child((prefix + "p").c_str());
		pugi::xml_node run = paragraph.append_child((prefix + "r").c_str());
		const pugi::xml_node look = first_words_look(paragraph);
		if (look) {
			pugi::xml_node properties = run.append_copy(look);
			// A paragraph mark's own revision is not the look of new words.
			remove_children_named(properties, {"ins", "del", "moveFrom", "moveTo", "rPrChange"});
		}
		pugi::xml_node text = run.append_child((prefix + "t").c_str());
		text.append_attribute("xml:space") = "preserve";
		gone.clear();
		for (pugi::xml_node child : paragraph.children())
			if (child != run && !local_name_is(child, "pPr")) gone.push_back(child);
		for (pugi::xml_node child : gone) paragraph.remove_child(child);
	}
}

void apply_formatting_within(pugi::xml_node node, const TextFormatting &formatting) {
	apply_formatting_within(node, formatting, TrackedChange{});
}

void apply_formatting_within(pugi::xml_node node, const TextFormatting &formatting, const TrackedChange &tracked) {
	if (!node || formatting.empty()) return;
	const bool records = tracked.records_revision();
	int id = records ? next_revision_id(node.root()) : 0;
	if (formatting.touches_runs()) {
		std::vector<pugi::xml_node> runs;
		visit(node, [&](pugi::xml_node child) { if (local_name_is(child, "r")) runs.push_back(child); });
		for (pugi::xml_node run : runs) {
			if (!records) {
				apply_run_formatting(run, formatting);
				continue;
			}
			// What the run stands inside: a deletion, or an insertion by this very
			// author - the words this change is writing.
			bool isStruck = false;
			bool isOwnInsertion = false;
			for (pugi::xml_node up = run.parent(); up && up != node.parent(); up = up.parent()) {
				if (local_name_is(up, "del") || local_name_is(up, "moveFrom")) isStruck = true;
				if (local_name_is(up, "ins") || local_name_is(up, "moveTo")) {
					const pugi::xml_attribute author = up.attribute("w:author") ? up.attribute("w:author") : up.attribute("author");
					if (tracked.author == author.as_string()) isOwnInsertion = true;
				}
			}
			if (isStruck) continue;
			if (isOwnInsertion) apply_run_formatting(run, formatting);
			else record_formatting_change(run_properties(run), [&]() { apply_run_formatting(run, formatting); }, id++, tracked);
		}
	}
	if (formatting.touches_paragraph()) {
		std::vector<pugi::xml_node> paragraphs;
		if (local_name_is(node, "p")) paragraphs.push_back(node);
		visit(node, [&](pugi::xml_node child) { if (local_name_is(child, "p")) paragraphs.push_back(child); });
		for (pugi::xml_node paragraph : paragraphs) {
			if (records) {
				record_formatting_change(paragraph_properties(paragraph),
					[&]() { apply_paragraph_formatting(paragraph, formatting); }, id++, tracked);
			} else {
				apply_paragraph_formatting(paragraph, formatting);
			}
		}
	}
}

void set_cell_shading(pugi::xml_node cell, const std::string &fill, const std::string &themeFill,
		const std::string &themeFillShade, const TrackedChange &tracked) {
	if (!cell || (fill.empty() && themeFill.empty())) return;
	// A cell's properties come first in the cell.
	pugi::xml_node properties;
	for (pugi::xml_node child : cell.children()) if (local_name_is(child, "tcPr")) properties = child;
	if (!properties) properties = cell.prepend_child((prefix_of(cell) + "tcPr").c_str());
	const auto apply = [&]() {
		pugi::xml_node shading;
		for (pugi::xml_node child : properties.children()) if (local_name_is(child, "shd")) shading = child;
		if (!shading) shading = add_property(properties, "shd");
		const auto set = [&](const char *name, const std::string &value) {
			if (!shading.attribute(name)) shading.append_attribute(name);
			shading.attribute(name).set_value(value.c_str());
		};
		if (!shading.attribute("w:val")) set("w:val", "clear");
		if (!shading.attribute("w:color")) set("w:color", "auto");
		if (!fill.empty()) set("w:fill", fill);
		if (!themeFill.empty()) {
			set("w:themeFill", themeFill);
			if (!themeFillShade.empty()) set("w:themeFillShade", themeFillShade);
		}
	};
	if (!tracked.records_revision()) {
		apply();
		return;
	}
	// Recorded as Word records a change to a cell's look: the properties as
	// they stood, kept last in a w:tcPrChange - unless one is there already,
	// which holds what the cell had before anyone changed it.
	for (pugi::xml_node child : properties.children()) {
		if (local_name_is(child, "tcPrChange")) {
			apply();
			return;
		}
	}
	pugi::xml_document held;
	const pugi::xml_node before = held.append_copy(properties);
	apply();
	// The cell already had this background: nothing changed to record.
	if (xml_text_of(before) == xml_text_of(properties)) return;
	pugi::xml_node change = properties.append_child((prefix_of(properties) + "tcPrChange").c_str());
	stamp_revision(change, next_revision_id(cell.root()), tracked);
	pugi::xml_node was = change.append_child(properties.name());
	for (pugi::xml_node child : before.children()) was.append_copy(child);
}

namespace {

bool starts_with_folded(const std::string &text, std::size_t at, const char *word) {
	const std::size_t length = std::strlen(word);
	if (at + length > text.size()) return false;
	for (std::size_t index = 0; index < length; ++index) {
		if (std::tolower(static_cast<unsigned char>(text[at + index])) != static_cast<unsigned char>(word[index]))
			return false;
	}
	return true;
}

bool equal_folded(const std::string &a, const std::string &b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

bool is_roman(const std::string &token) {
	if (token.empty() || token.size() > 6) return false;
	const bool lower = std::all_of(token.begin(), token.end(), [](char c) { return std::strchr("ivxlcdm", c) != nullptr; });
	const bool upper = std::all_of(token.begin(), token.end(), [](char c) { return std::strchr("IVXLCDM", c) != nullptr; });
	return lower || upper;
}

// How many bytes at the head of `line` are a clause number or a bullet a person
// typed - with the space after it - or 0 where there is none. A number counts
// only with the marks that make it a clause number: "9.3", "9.", "(9)", "9)",
// "(b)", "b)", "iv.", or any number after "Section", "Article", "Clause",
// "Schedule", "Paragraph" or "§". "30 days" begins with words, not a number.
std::size_t typed_clause_number_length(const std::string &line) {
	std::size_t at = 0;
	while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
	const auto followedByWords = [&](std::size_t end) -> std::size_t {
		if (end >= line.size() || (line[end] != ' ' && line[end] != '\t')) return 0;
		while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) ++end;
		return end < line.size() ? end : 0;
	};
	for (const char *bullet : {"-", "*", "+", "\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x97\xA6", "\xE2\x96\xAA"}) {
		const std::size_t length = std::strlen(bullet);
		if (line.compare(at, length, bullet) == 0) {
			const std::size_t end = followedByWords(at + length);
			if (end > 0) return end;
		}
	}
	bool isNamed = false;
	for (const char *word : {"section ", "article ", "clause ", "schedule ", "paragraph ", "\xC2\xA7 ", "\xC2\xA7"}) {
		if (starts_with_folded(line, at, word)) {
			at += std::strlen(word);
			isNamed = true;
			break;
		}
	}
	const bool isOpened = at < line.size() && line[at] == '(';
	if (isOpened) ++at;
	const std::size_t tokenStart = at;
	while (at < line.size() && (std::isalnum(static_cast<unsigned char>(line[at])) != 0 || line[at] == '.')) ++at;
	std::string token = line.substr(tokenStart, at - tokenStart);
	const bool isClosed = at < line.size() && line[at] == ')';
	if (isClosed) ++at;
	if (isOpened && !isClosed) return 0;
	const bool endsWithStop = !token.empty() && token.back() == '.';
	while (!token.empty() && token.back() == '.') token.pop_back();
	if (token.empty()) return 0;
	const bool hasDigit = std::any_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
	bool isNumber = false;
	if (hasDigit) {
		// Digits and the stops between them, never two stops together.
		const bool digitsOnly = std::all_of(token.begin(), token.end(), [](unsigned char c) {
			return std::isdigit(c) != 0 || c == '.';
		});
		const bool isDotted = token.find('.') != std::string::npos;
		isNumber = digitsOnly && token.find("..") == std::string::npos
			&& (isNamed || isDotted || endsWithStop || isOpened || isClosed);
	} else {
		const bool isLetter = token.size() == 1 && std::isalpha(static_cast<unsigned char>(token[0])) != 0;
		isNumber = (isLetter || is_roman(token)) && (isNamed || isClosed || endsWithStop);
	}
	if (!isNumber) return 0;
	return followedByWords(at);
}

// How many bytes at the head of `line` are a number typed in the form Word
// draws for the paragraph (`drawn`) - with the space after it - or a bullet,
// or 0 where there is none.
//
// The form is the drawn number's own: its digits and letters stand for any
// value, everything else must be there as Word draws it. "9.3" is typed where
// Word draws "9.2", "(c)" where it draws "(b)", "Section 4.02" where it draws
// "Section 4.01". A number of several parts must also sit in the same place:
// beside "9.2", "9.3" is the next clause, "7.1" is a section the clause names.
// Where what Word draws is not known (""), only a number in brackets counts;
// where Word draws a bullet, no number does.
std::size_t typed_number_like_drawn(const std::string &line, const std::string &drawnAsGiven, bool anyValues) {
	// The number alone, without the space Word leaves around it.
	const std::size_t drawnFirst = drawnAsGiven.find_first_not_of(" \t");
	const std::string drawn = drawnFirst == std::string::npos ? std::string()
		: drawnAsGiven.substr(drawnFirst, drawnAsGiven.find_last_not_of(" \t") - drawnFirst + 1);
	const bool drawsNothingKnown = drawnAsGiven.empty();
	std::size_t at = 0;
	while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
	const auto followedByWords = [&](std::size_t end) -> std::size_t {
		if (end >= line.size() || (line[end] != ' ' && line[end] != '\t')) return 0;
		while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) ++end;
		return end < line.size() ? end : 0;
	};
	for (const char *bullet : {"-", "*", "+", "\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x97\xA6", "\xE2\x96\xAA"}) {
		const std::size_t length = std::strlen(bullet);
		if (line.compare(at, length, bullet) == 0) {
			const std::size_t end = followedByWords(at + length);
			if (end > 0) return end;
		}
	}
	const auto isAlpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
	const auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
	const auto alphaRun = [&](const std::string &text, std::size_t from) {
		std::size_t end = from;
		while (end < text.size() && isAlpha(text[end])) ++end;
		return end - from;
	};
	// A value a list counts in letters: "b", "aa", "iv", "XII".
	const auto isLetterValue = [](const std::string &token) {
		if (token.empty()) return false;
		const bool repeated = std::all_of(token.begin(), token.end(), [&](char c) { return c == token[0]; });
		return (repeated && token.size() <= 3) || is_roman(token);
	};
	if (drawsNothingKnown) {
		if (at >= line.size() || line[at] != '(') return 0;
		std::size_t end = at + 1;
		while (end < line.size() && std::isalnum(static_cast<unsigned char>(line[end])) != 0) ++end;
		const std::string token = line.substr(at + 1, end - at - 1);
		if (end >= line.size() || line[end] != ')' || token.empty()) return 0;
		const bool isValue = std::all_of(token.begin(), token.end(), [&](char c) { return isDigit(c); })
			|| isLetterValue(token);
		return isValue ? followedByWords(end + 1) : 0;
	}
	std::vector<std::string> drawnValues;
	std::vector<std::string> typedValues;
	std::size_t index = 0;
	while (index < drawn.size()) {
		const char c = drawn[index];
		if (isDigit(c)) {
			std::size_t end = index;
			while (end < drawn.size() && isDigit(drawn[end])) ++end;
			std::size_t typedEnd = at;
			while (typedEnd < line.size() && isDigit(line[typedEnd])) ++typedEnd;
			if (typedEnd == at) return 0;
			drawnValues.push_back(drawn.substr(index, end - index));
			typedValues.push_back(line.substr(at, typedEnd - at));
			index = end;
			at = typedEnd;
			continue;
		}
		if (isAlpha(c)) {
			const std::string word = drawn.substr(index, alphaRun(drawn, index));
			const std::string typed = line.substr(at, alphaRun(line, at));
			if (isLetterValue(word)) {
				// A value: any letters counted the same way ("c" beside "b").
				if (!isLetterValue(typed) || (is_roman(word) && word.size() > 1 && !is_roman(typed))) return 0;
				drawnValues.push_back(word);
				typedValues.push_back(typed);
			} else if (!equal_folded(typed, word)) {
				// A word Word draws with the number - "Section", "Article" - is
				// typed as it is drawn.
				return 0;
			}
			index += word.size();
			at += typed.size();
			continue;
		}
		if (c == ' ' || c == '\t') {
			while (index < drawn.size() && (drawn[index] == ' ' || drawn[index] == '\t')) ++index;
			if (at >= line.size() || (line[at] != ' ' && line[at] != '\t')) return 0;
			while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
			continue;
		}
		if (at >= line.size() || line[at] != c) return 0;
		++index;
		++at;
	}
	if (drawnValues.empty()) return 0;
	// "9.3." beside "9.2": the closing stop is how the number was typed.
	if (drawn.back() != '.' && at < line.size() && line[at] == '.') ++at;
	for (std::size_t part = 0; !anyValues && part + 1 < drawnValues.size(); ++part) {
		if (!equal_folded(typedValues[part], drawnValues[part])) return 0;
	}
	return followedByWords(at);
}

// The token of an item mark typed at the head of `line` - "7.1" of "7.1. ",
// "a" of "(a) ", "iv" of "iv. " - and how many bytes the mark and the space
// after it take; 0 where the line opens with no such mark. A mark is closed
// by a stop or a bracket, so "1.5 million shares", "Section 7.1" and "30 days"
// are words, not marks.
std::pair<std::string, std::size_t> typed_item_mark(const std::string &line) {
	std::size_t at = 0;
	while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
	const bool isOpened = at < line.size() && line[at] == '(';
	if (isOpened) ++at;
	const std::size_t tokenStart = at;
	while (at < line.size() && (std::isalnum(static_cast<unsigned char>(line[at])) != 0 || line[at] == '.')) ++at;
	std::string token = line.substr(tokenStart, at - tokenStart);
	const bool isClosed = at < line.size() && line[at] == ')';
	if (isClosed) ++at;
	if (isOpened && !isClosed) return {std::string(), 0};
	const bool endsWithStop = !token.empty() && token.back() == '.';
	if (!isClosed && !endsWithStop) return {std::string(), 0};
	while (!token.empty() && token.back() == '.') token.pop_back();
	if (token.empty() || token.front() == '.' || token.find("..") != std::string::npos) return {std::string(), 0};
	const bool isDotted = std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0 || c == '.'; });
	const bool isLetters = token.size() <= 3 && std::isalpha(static_cast<unsigned char>(token[0])) != 0
		&& std::all_of(token.begin(), token.end(), [&](char c) { return c == token[0]; });
	if (!isDotted && !isLetters && !is_roman(token)) return {std::string(), 0};
	if (at >= line.size() || (line[at] != ' ' && line[at] != '\t')) return {std::string(), 0};
	while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
	return at < line.size() ? std::make_pair(token, at) : std::make_pair(std::string(), std::size_t{0});
}

std::size_t typed_item_mark_length(const std::string &line) {
	return typed_item_mark(line).second;
}

// A mark of several numbers - "7.1.", "3.2)" - which only a list item opens
// with.
bool is_multipart_item_mark(const std::string &line) {
	const auto [token, length] = typed_item_mark(line);
	return length > 0 && token.find('.') != std::string::npos;
}

// The values a number is counted in, in order: "7", "1" of "7.1."; "4", "01"
// of "Section 4.01"; "b" of "(b)". A word drawn with the number is not one.
std::vector<std::string> number_values(const std::string &text) {
	std::vector<std::string> values;
	std::size_t at = 0;
	while (at < text.size()) {
		const unsigned char here = static_cast<unsigned char>(text[at]);
		if (std::isdigit(here) != 0) {
			std::size_t end = at;
			while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) ++end;
			values.push_back(text.substr(at, end - at));
			at = end;
			continue;
		}
		if (std::isalpha(here) != 0) {
			std::size_t end = at;
			while (end < text.size() && std::isalpha(static_cast<unsigned char>(text[end])) != 0) ++end;
			const std::string word = text.substr(at, end - at);
			const bool repeated = word.size() <= 3 && std::all_of(word.begin(), word.end(), [&](char c) { return c == word[0]; });
			if (repeated || is_roman(word)) values.push_back(word);
			at = end;
			continue;
		}
		++at;
	}
	return values;
}

// Where the words of a reading mark begin, when one opens at `at`: the ": "
// of "[inserted: ", "[deleted: ", "[inserted by Jane Roe: " or "[deleted by
// Jane Roe: " - exactly those, as DocxExtractor writes them. npos otherwise.
//
// A lawyer's own brackets say the same words: "12.3 [Deleted].", "[Inserted at
// Closing] Schedule: assets", "Clause 9 is [deleted] and replaced as follows:".
// None of those is a mark, and none of their words may go.
std::size_t reading_mark_colon(const std::string &text, std::size_t at) {
	std::size_t after = std::string::npos;
	if (starts_with_folded(text, at, "[inserted")) after = at + std::strlen("[inserted");
	else if (starts_with_folded(text, at, "[deleted")) after = at + std::strlen("[deleted");
	if (after == std::string::npos) return std::string::npos;
	if (text.compare(after, 2, ": ") == 0) return after;
	if (!starts_with_folded(text, after, " by ")) return std::string::npos;
	// The author's name: on one line, short, with no bracket in it.
	constexpr std::size_t kLongestAuthor = 120;
	const std::size_t name = after + std::strlen(" by ");
	const std::size_t colon = text.find(": ", name);
	if (colon == std::string::npos || colon == name || colon - name > kLongestAuthor) return std::string::npos;
	for (std::size_t index = name; index < colon; ++index)
		if (text[index] == '[' || text[index] == ']' || text[index] == '\n' || text[index] == '\r')
			return std::string::npos;
	return colon;
}

} // namespace

std::string without_typed_clause_number(const std::string &line, const std::string &drawn) {
	const std::size_t typed = typed_number_like_drawn(line, drawn);
	return typed == 0 ? line : line.substr(typed);
}

std::string without_typed_clause_number(const std::string &line) {
	const std::size_t typed = typed_clause_number_length(line);
	return typed == 0 ? line : line.substr(typed);
}

std::string passage_without_reading_marks(const std::string &quoted) {
	if (quoted.find('[') == std::string::npos) return quoted;
	std::string passage;
	std::size_t at = 0;
	while (at < quoted.size()) {
		if (quoted[at] != '[') {
			passage.push_back(quoted[at++]);
			continue;
		}
		const std::size_t colon = reading_mark_colon(quoted, at);
		if (colon != std::string::npos) {
			// "[inserted by Jane Roe: words]": the words run from the mark's
			// ": " to the bracket that closes it - a placeholder's own "[...]"
			// inside the words is part of them.
			std::size_t depth = 1;
			std::size_t end = colon + 2;
			for (; end < quoted.size(); ++end) {
				if (quoted[end] == '[') ++depth;
				else if (quoted[end] == ']' && --depth == 0) break;
			}
			// A mark the text never closes is not a mark: the words stay.
			if (end < quoted.size()) {
				if (starts_with_folded(quoted, at, "[inserted")) passage += quoted.substr(colon + 2, end - colon - 2);
				at = end + 1;
				continue;
			}
		}
		if (starts_with_folded(quoted, at, "[footnote ") || starts_with_folded(quoted, at, "[endnote ")) {
			const std::size_t close = quoted.find(']', at);
			const std::size_t space = quoted.find(' ', at);
			const bool numbered = close != std::string::npos && space < close && close > space + 1
				&& std::all_of(quoted.begin() + static_cast<std::ptrdiff_t>(space + 1),
					quoted.begin() + static_cast<std::ptrdiff_t>(close),
					[](unsigned char c) { return std::isdigit(c) != 0; });
			if (numbered) {
				at = close + 1;
				continue;
			}
		}
		passage.push_back(quoted[at++]);
	}
	return passage;
}

std::string revision_timestamp_now() {
	const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &seconds);
#else
	gmtime_r(&seconds, &utc);
#endif
	char stamp[32];
	std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
		utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
	return stamp;
}

namespace {

const char *const kCorePropertiesNamespace = "http://schemas.openxmlformats.org/package/2006/metadata/core-properties";
const char *const kDublinCoreNamespace = "http://purl.org/dc/elements/1.1/";
const char *const kDublinCoreTermsNamespace = "http://purl.org/dc/terms/";
const char *const kSchemaInstanceNamespace = "http://www.w3.org/2001/XMLSchema-instance";

std::string package_part_text(const pugi::xml_document &document) {
	struct Sink : pugi::xml_writer {
		std::string out;
		void write(const void *data, std::size_t size) override { out.append(static_cast<const char *>(data), size); }
	} sink;
	document.save(sink, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
	return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" + sink.out;
}

// The prefix `uri` is declared with on `root`, declared as `wanted` where it
// is not declared yet. "" where `wanted` already names something else.
std::string prefix_for(pugi::xml_node root, const char *uri, const char *wanted) {
	const std::string existing = declared_prefix(root, uri);
	if (!existing.empty()) return existing;
	const std::string name = std::string("xmlns:") + wanted;
	if (root.attribute(name.c_str())) return std::string();
	root.append_attribute(name.c_str()) = uri;
	return wanted;
}

// A moment property (dcterms:created, dcterms:modified), set to `when` and
// typed as W3CDTF, which is how Word reads it as a date.
void set_moment(pugi::xml_node property, const std::string &schemaInstance, const std::string &terms,
		const std::string &when) {
	const std::string typeName = schemaInstance + ":type";
	if (!property.attribute(typeName.c_str())) property.append_attribute(typeName.c_str());
	property.attribute(typeName.c_str()).set_value((terms + ":W3CDTF").c_str());
	property.text().set(when.c_str());
}

} // namespace

std::string new_core_properties_part(const std::string &title, const std::string &author, const std::string &when) {
	pugi::xml_document document;
	pugi::xml_node root = document.append_child("cp:coreProperties");
	root.append_attribute("xmlns:cp") = kCorePropertiesNamespace;
	root.append_attribute("xmlns:dc") = kDublinCoreNamespace;
	root.append_attribute("xmlns:dcterms") = kDublinCoreTermsNamespace;
	root.append_attribute("xmlns:xsi") = kSchemaInstanceNamespace;
	if (!title.empty()) root.append_child("dc:title").text().set(title.c_str());
	if (!author.empty()) root.append_child("dc:creator").text().set(author.c_str());
	if (!author.empty()) root.append_child("cp:lastModifiedBy").text().set(author.c_str());
	if (!when.empty()) {
		set_moment(root.append_child("dcterms:created"), "xsi", "dcterms", when);
		set_moment(root.append_child("dcterms:modified"), "xsi", "dcterms", when);
	}
	return package_part_text(document);
}

std::string new_app_properties_part() {
	return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		"<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
		"xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">"
		"<Application>Singularity</Application><DocSecurity>0</DocSecurity></Properties>";
}

std::string core_properties_modified(const std::string &coreXml, const std::string &modifiedBy,
		const std::string &when) {

	pugi::xml_document document;
	if (!document.load_string(coreXml.c_str(), pugi::parse_default | pugi::parse_ws_pcdata)) return std::string();
	pugi::xml_node root = document.document_element();
	if (!root || !local_name_is(root, "coreProperties")) return std::string();

	// A property the part already has is rewritten where it stands; one it
	// lacks is added, under the prefix the part declares for it.
	const auto property = [&](const char *name, const char *uri, const char *wanted) {
		for (pugi::xml_node child : root.children()) if (local_name_is(child, name)) return child;
		const std::string prefix = prefix_for(root, uri, wanted);
		if (prefix.empty()) return pugi::xml_node();
		return root.append_child((prefix + ":" + name).c_str());
	};
	if (!modifiedBy.empty()) {
		pugi::xml_node lastModifiedBy = property("lastModifiedBy", kCorePropertiesNamespace, "cp");
		if (lastModifiedBy) lastModifiedBy.text().set(modifiedBy.c_str());
	}
	if (!when.empty()) {
		pugi::xml_node modified = property("modified", kDublinCoreTermsNamespace, "dcterms");
		const std::string schemaInstance = prefix_for(root, kSchemaInstanceNamespace, "xsi");
		const std::string terms = prefix_for(root, kDublinCoreTermsNamespace, "dcterms");
		if (modified && !schemaInstance.empty() && !terms.empty()) set_moment(modified, schemaInstance, terms, when);
	}
	return package_part_text(document);

}

std::vector<ProseParagraph> prose_paragraphs(const std::string &text) {
	std::vector<ProseParagraph> paragraphs;
	std::vector<std::string> lines;
	std::string current;
	for (char character : text) {
		if (character == '\n') { lines.push_back(current); current.clear(); continue; }
		if (character != '\r') current.push_back(character);
	}
	lines.push_back(current);
	for (std::string line : lines) {
		ProseParagraph paragraph;
		std::size_t at = 0;
		while (at < line.size() && line[at] == ' ') ++at;
		// A rule across the page is a thing Markdown has and a legal document
		// does not. It is dropped, never written as dashes. A line of
		// underscores is not one: it is where a signature or a blank goes.
		std::string bare = line.substr(at);
		const bool isRule = bare.size() >= 3
			&& bare.find_first_not_of(bare.front()) == std::string::npos
			&& (bare.front() == '-' || bare.front() == '*');
		if (isRule) continue;
		int hashes = 0;
		while (at + static_cast<std::size_t>(hashes) < line.size() && line[at + static_cast<std::size_t>(hashes)] == '#') ++hashes;
		if (hashes > 0 && hashes <= 6 && at + static_cast<std::size_t>(hashes) < line.size()
				&& line[at + static_cast<std::size_t>(hashes)] == ' ') {
			paragraph.heading_level = hashes;
			line = line.substr(at + static_cast<std::size_t>(hashes) + 1);
		} else if (line.compare(at, 2, "> ") == 0) {
			// A quote marker says nothing in a document of the firm's own.
			line = line.substr(at + 2);
		} else if (line.compare(at, 2, "* ") == 0 || line.compare(at, 2, "+ ") == 0) {
			// A bullet a lawyer would have typed is a hyphen. A star is the
			// machine's, and it is not left standing at the head of a clause.
			line = line.substr(0, at) + "- " + line.substr(at + 2);
		}
		for (const ProseRun &run : runs_of_line(line, false, false)) paragraph.runs.push_back(run);
		if (paragraph.runs.empty()) paragraph.runs.push_back(ProseRun{});
		paragraphs.push_back(paragraph);
	}
	if (paragraphs.empty()) paragraphs.push_back(ProseParagraph{{ProseRun{}}, 0});
	return paragraphs;
}

std::string prose_plain_text(const std::string &text) {
	std::string plain;
	bool first = true;
	for (const ProseParagraph &paragraph : prose_paragraphs(text)) {
		if (!first) plain.push_back('\n');
		first = false;
		for (const ProseRun &run : paragraph.runs) plain += run.text;
	}
	return plain;
}

std::string wordprocessing_text(const pugi::xml_document &document) {
	return flatten(document).text;
}

TextEditOutcome apply_wordprocessing_edit(pugi::xml_document &document, const TextEditRequest &request) {
	TextEditOutcome outcome;
	// What is written is the document's own kind of text: a model that copies
	// a pending change's brackets out of its reading into its new words means
	// the words, never the brackets.
	const std::string requestText = passage_without_reading_marks(request.text);
	// Every revision written by this one change gets its own id, starting
	// after whatever the document already holds.
	int revision = next_revision_id(document);
	const auto nextRevision = [&revision]() { return revision++; };
	const bool records = request.tracked.records_revision();
	if (request.kind == TextEditKind::AppendParagraph) {
		// The end of the body, after the last paragraph that is not the section
		// mark: written like the last ordinary paragraph, so an added note reads
		// like the document rather than like a machine.
		pugi::xml_node body;
		for (pugi::xml_node node : document.children()) {
			if (!local_name_is(node, "document")) continue;
			for (pugi::xml_node child : node.children()) if (local_name_is(child, "body")) body = child;
		}
		if (!body) {
			outcome.reason = "The document has no body to add to.";
			return outcome;
		}
		pugi::xml_node last;
		for (pugi::xml_node child : body.children()) if (local_name_is(child, "p")) last = child;
		const std::vector<ProseParagraph> written = paragraphs_to_write(requestText);
		if (!last) {
			for (const ProseParagraph &paragraph : written) {
				outcome.written.push_back(marked_words(paragraph));
				pugi::xml_node node = body.append_child("w:p");
				for (const ProseRun &piece : paragraph.runs) {
					pugi::xml_node run = node.append_child("w:r");
					TextFormatting written_as;
					if (piece.bold || paragraph.heading_level > 0) written_as.bold = true;
					if (piece.italic) written_as.italic = true;
					if (written_as.touches_runs()) apply_run_formatting(run, written_as);
					set_run_text(run.append_child("w:t"), piece.text);
				}
				if (records) mark_paragraph_inserted(node, nextRevision, request.tracked);
			}
			outcome.ok = true;
			return outcome;
		}
		const pugi::xml_node model = paragraph_to_write_like(last, false, request.is_heading);
		pugi::xml_node at = last;
		for (const ProseParagraph &paragraph : written) {
			at = paragraph_like(at, model, paragraph, heading_model(body, paragraph.heading_level, request.is_heading),
				false, request, outcome.written);
			// Text added with a way of being written ("and make that line
			// italic") is one instruction: the paragraph is written that way.
			apply_formatting_within(at, request.formatting);
			if (records) mark_paragraph_inserted(at, nextRevision, request.tracked);
		}
		if (records) keep_final_mark_last(last, at, nextRevision, request.tracked);
		outcome.ok = true;
		return outcome;
		}
		if (request.anchor.empty()) {
		outcome.reason = "The change named no passage to anchor it to.";
		return outcome;
	}
	// Whitespace around an anchor belongs to the sentence that carried it, not
	// to the passage: a model quoting a sentence often takes the space before it.
	const std::string anchor = platform::trimmed_phrase(request.anchor);
	const FlatDocument flat = flatten(document);
	platform::PhrasePlacement placement = platform::locate_phrase(flat.text, anchor);
	// A passage quoted out of the model's reading of the document may carry
	// what that reading adds - a pending change's brackets, a note's number,
	// the number Word draws in front of a clause. The document's own words are
	// tried as they were quoted first, and only then without those.
	if (placement.matches == 0) {
		std::vector<std::string> forms;
		const std::string unmarked = platform::trimmed_phrase(passage_without_reading_marks(anchor));
		if (!unmarked.empty() && unmarked != anchor) forms.push_back(unmarked);
		std::string unnumbered;
		std::size_t lineStart = 0;
		const std::string &base = unmarked.empty() ? anchor : unmarked;
		while (lineStart <= base.size()) {
			const std::size_t lineEnd = std::min(base.find('\n', lineStart), base.size());
			if (lineStart > 0) unnumbered.push_back('\n');
			unnumbered += without_typed_clause_number(base.substr(lineStart, lineEnd - lineStart));
			lineStart = lineEnd + 1;
		}
		unnumbered = platform::trimmed_phrase(unnumbered);
		if (!unnumbered.empty() && unnumbered != base) forms.push_back(unnumbered);
		for (const std::string &form : forms) {
			placement = platform::locate_phrase(flat.text, form);
			if (placement.matches == 0) continue;
			// Found only once the number typed in front of it was set aside: it
			// is that clause only where Word draws that number in front of it.
			// The same sentence under another number is another clause - "9.2
			// The Supplier shall..." is never anchored to clause 12.2.
			if (form == unnumbered && placement.matches == 1) {
				const std::string firstLine = base.substr(0, std::min(base.find('\n'), base.size()));
				const std::string typedNumber = firstLine.substr(0, typed_clause_number_length(firstLine));
				std::optional<std::string> drawn;
				for (std::size_t index = 0; index < flat.spans.size(); ++index)
					if (flat.spans[index].first <= placement.start && placement.start < std::max(flat.spans[index].second, flat.spans[index].first + 1)) {
						if (request.numbered_automatically) drawn = request.numbered_automatically(flat.paragraphs[index]);
						break;
					}
				// Known to be another number: refused. Unknown - no numbering read,
				// or a paragraph Word does not number - it is the sentence's place.
				if (!typedNumber.empty() && drawn && !drawn->empty() && number_values(*drawn) != number_values(typedNumber)) {
					outcome.reason = "The passage this change is anchored to is numbered differently in the document, "
						"so where to make the change is not decided by it.";
					return outcome;
				}
			}
			break;
		}
	}
	outcome.matches = placement.matches;
	if (placement.matches == 0) {
		outcome.reason = "The passage this change is anchored to is not in the document.";
		return outcome;
	}
	if (placement.matches > 1) {
		outcome.reason = "The passage this change is anchored to stands in more than one place in the "
			"document, so where to make the change is not decided by it.";
		return outcome;
	}
	const std::size_t start = placement.start;
	const std::size_t end = placement.start + placement.length;
	outcome.anchor_exact = flat.text.substr(start, placement.length);

	// A clause Word numbers, rewritten with the number the model read in front
	// of it ("1.2 Neither party ... or wilful misconduct."), is rewritten
	// without it: Word draws "1.2" itself, and the number written into the text
	// would print twice and strike the whole clause for the few words that
	// changed. A number the clause really holds as text is left as it is.
	std::string replacementSource = requestText;
	if (request.kind == TextEditKind::Replace && request.numbered_automatically) {
		for (std::size_t index = 0; index < flat.paragraphs.size(); ++index) {
			const auto [from, to] = flat.spans[index];
			if (from != start || to == from) continue;
			const std::optional<std::string> drawn = request.numbered_automatically(flat.paragraphs[index]);
			if (drawn && !drawn->empty()) {
				const std::size_t typed = typed_number_like_drawn(replacementSource, *drawn);
				if (typed > 0 && typed < replacementSource.size()
						&& typed_number_like_drawn(flat.text.substr(from, to - from), *drawn) == 0)
					replacementSource.erase(0, typed);
			}
			break;
		}
	}

	if (request.kind == TextEditKind::InsertParagraphBefore
			|| request.kind == TextEditKind::InsertParagraphAfter) {
		pugi::xml_node anchorParagraph;
		for (const TextRun &run : flat.runs) {
			if (run.start + run.length > start) { anchorParagraph = run.paragraph; break; }
		}
		if (!anchorParagraph) {
			outcome.reason = "The passage this change is anchored to has no paragraph of its own.";
			return outcome;
		}
		// Several lines become several paragraphs, in order; inserting before
		// keeps them in the order they were written.
		const std::vector<ProseParagraph> written = paragraphs_to_write(requestText);
		const bool before = request.kind == TextEditKind::InsertParagraphBefore;
		const pugi::xml_node model = paragraph_to_write_like(anchorParagraph, before, request.is_heading);
		pugi::xml_node body = anchorParagraph.parent();
		while (body && !local_name_is(body, "body")) body = body.parent();
		if (before) {
			for (const ProseParagraph &paragraph : written) {
				pugi::xml_node made = paragraph_like(anchorParagraph, model, paragraph,
					heading_model(body, paragraph.heading_level, request.is_heading), true, request, outcome.written);
				apply_formatting_within(made, request.formatting);
				if (records) mark_paragraph_inserted(made, nextRevision, request.tracked);
			}
		} else {
			pugi::xml_node at = anchorParagraph;
			for (const ProseParagraph &paragraph : written) {
				at = paragraph_like(at, model, paragraph, heading_model(body, paragraph.heading_level, request.is_heading),
					false, request, outcome.written);
				apply_formatting_within(at, request.formatting);
				if (records) mark_paragraph_inserted(at, nextRevision, request.tracked);
			}
			if (records) keep_final_mark_last(anchorParagraph, at, nextRevision, request.tracked);
			}
			outcome.ok = true;
		return outcome;
	}

	if (request.kind == TextEditKind::Format) {
		if (request.formatting.empty()) {
			outcome.reason = "The change named nothing about how the passage is written.";
			return outcome;
		}
		// Exactly the passage, and nothing around it: a run the passage covers
		// only part of is split, so the words on either side keep the formatting
		// they were given.
		std::vector<pugi::xml_node> paragraphs;
		if (request.formatting.touches_runs()) {
			std::vector<const TextRun *> covered;
			for (const TextRun &run : flat.runs) {
				const std::size_t runEnd = run.start + run.length;
				if (runEnd <= start || run.start >= end) continue;
				covered.push_back(&run);
			}
			if (covered.empty()) {
				outcome.reason = "The passage this change is anchored to could not be located in the document's text.";
				return outcome;
			}
			for (const TextRun *run : covered) {
				const std::size_t runEnd = run->start + run->length;
				const std::size_t from = start > run->start ? start - run->start : 0;
				const std::size_t to = end < runEnd ? end - run->start : run->length;
				pugi::xml_node target = run_for_span(run->node, from, to);
				// Formatting a firm's document is a change like any other: with
				// revisions on, it is recorded, never made silently.
				if (target && records) {
					record_formatting_change(run_properties(target),
						[&]() { apply_run_formatting(target, request.formatting); }, nextRevision(), request.tracked);
				} else if (target) {
					apply_run_formatting(target, request.formatting);
				}
				if (std::find(paragraphs.begin(), paragraphs.end(), run->paragraph) == paragraphs.end())
					paragraphs.push_back(run->paragraph);
			}
		}
		if (request.formatting.touches_paragraph()) {
			if (paragraphs.empty()) {
				for (const TextRun &run : flat.runs) {
					const std::size_t runEnd = run.start + run.length;
					if (runEnd <= start || run.start >= end) continue;
					if (std::find(paragraphs.begin(), paragraphs.end(), run.paragraph) == paragraphs.end())
						paragraphs.push_back(run.paragraph);
				}
			}
			for (pugi::xml_node paragraph : paragraphs) {
				if (records) {
					record_formatting_change(paragraph_properties(paragraph),
						[&]() { apply_paragraph_formatting(paragraph, request.formatting); }, nextRevision(),
						request.tracked);
				} else {
					apply_paragraph_formatting(paragraph, request.formatting);
				}
			}
		}
		outcome.ok = true;
		return outcome;
	}

	// Replace and Delete: every run the passage covers is rewritten, keeping
	// what each run holds outside the passage and the formatting it is written
	// in. The new text goes into the first covered run, so it reads in that
	// run's own formatting - a replaced sentence inside a bold lead-in keeps
	// the lead-in bold and the sentence plain, exactly as the paragraph was.
	std::vector<const TextRun *> covered;
	for (const TextRun &run : flat.runs) {
		const std::size_t runEnd = run.start + run.length;
		if (runEnd <= start || run.start >= end) continue;
		covered.push_back(&run);
	}
	if (covered.empty()) {
		outcome.reason = "The passage this change is anchored to could not be located in the document's text.";
		return outcome;
	}
	// A passage that runs across table cells is a row, not a sentence. Writing
	// it back as one piece of text would empty the other cells and collapse the
	// row into its first cell - the document would still hold every word and
	// would no longer be a table. So a change across cells is made cell by
	// cell, and only when the new text says which part belongs in which cell by
	// having exactly as many lines as there are cells. Anything else is
	// refused: a table is structure a lawyer relies on, and guessing at it is
	// worse than not writing.
	std::vector<pugi::xml_node> coveredCells;
	for (const TextRun *run : covered) {
		if (!run->cell) continue;
		if (std::find(coveredCells.begin(), coveredCells.end(), run->cell) == coveredCells.end())
			coveredCells.push_back(run->cell);
	}
	std::vector<std::string> cellTexts;
	if (coveredCells.size() > 1) {
		if (request.kind == TextEditKind::Replace) {
			std::string current;
			for (char character : replacementSource) {
				if (character == '\n') { cellTexts.push_back(current); current.clear(); continue; }
				if (character != '\r') current.push_back(character);
			}
			cellTexts.push_back(current);
			if (cellTexts.size() != coveredCells.size()) {
				outcome.reason = "The passage this change is anchored to runs across "
					+ std::to_string(coveredCells.size())
					+ " cells of a table, and the new text is not written as one line per cell, so which cell "
					"each part belongs in is not decided. Anchor the change to one cell's own words, or give "
					"the row as one line per cell.";
				return outcome;
			}
		}
	}

	// A passage replaced inside a sentence takes the sentence's own way of
	// being written, so the marks a model put around it are taken off rather
	// than turned into runs of their own: a star in the middle of a clause was
	// never an instruction about that clause.
	const std::string replacement = request.kind == TextEditKind::Delete
		? std::string() : prose_plain_text(replacementSource);
	if (request.kind == TextEditKind::Replace) outcome.written.push_back(replacement);

	// ---- Clauses are paragraphs ----------------------------------------------
	//
	// A replacement whose old words run across several paragraphs, or whose new
	// words are several lines - "6. ...\n7. ...\n7.1. ..." - is a change to
	// clauses, and it is written clause for clause: each new line is a
	// paragraph of its own, set against the old paragraph it rewrites, a line
	// with no old paragraph to rewrite is a new paragraph written like the
	// clauses around it, and an old paragraph no line rewrites goes whole, its
	// paragraph mark with it. Written as one piece of text it would read, once
	// accepted, as one run-on clause with empty numbered items where the old
	// ones stood (a Russian claim on 26 Sep 2026: "...…
	// …:7.1. …").
	std::vector<std::string> newLines;
	std::vector<ParagraphSegment> segments;
	if (request.kind == TextEditKind::Replace && cellTexts.empty()) {
		std::string current;
		const auto keep = [&]() {
			if (prose_plain_text(current).find_first_not_of(" \t\r") != std::string::npos) newLines.push_back(current);
			current.clear();
		};
		for (char character : replacementSource) {
			if (character == '\n') keep();
			else if (character != '\r') current.push_back(character);
		}
		keep();
		for (std::size_t index = 0; index < flat.paragraphs.size(); ++index) {
			const auto [from, to] = flat.spans[index];
			if (from >= end || to < start) continue;
			ParagraphSegment segment;
			segment.paragraph = flat.paragraphs[index];
			segment.from = std::max(from, start);
			segment.to = std::min(to, end);
			segment.head = start > from;
			segment.tail = end < to;
			segments.push_back(segment);
		}
	}
	bool byParagraphs = !newLines.empty() && (newLines.size() >= 2 || segments.size() >= 2);
	for (const ParagraphSegment &segment : segments) {
		// Only paragraphs standing side by side in one body, cell or box, and
		// only where a line of the old text is a paragraph - a line break inside
		// a paragraph (an address block) is a line, not a clause.
		if (segment.paragraph.parent() != segments.front().paragraph.parent()
				|| flat.text.substr(segment.from, segment.to - segment.from).find('\n') != std::string::npos)
			byParagraphs = false;
	}
	// Written directly, the change is the same change with its revisions
	// accepted: one rule for which paragraph becomes which, recorded or not.
	if (byParagraphs && !records) {
		static const char *const kDirectAuthor = "\x01Singularity direct change";
		TextEditRequest direct = request;
		direct.tracked = TrackedChange{kDirectAuthor, std::string()};
		const int first = next_revision_id(document);
		TextEditOutcome made = apply_wordprocessing_edit(document, direct);
		if (made.ok) accept_own_revisions(document, first, kDirectAuthor);
		return made;
	}

	// ---- The same change, recorded as a revision -----------------------------
	//
	// Nothing is taken out of the file: the old words stay, struck, and the new
	// words stand beside them. Word shows one redline, and the partner who
	// opens the document accepts or rejects it there. The words the passage
	// covers are first given runs of their own, so exactly what changed is
	// what is marked - not the sentence around it.
	if (records) {
		// A redline says what changed, and nothing else. A model asked to alter
		// a notice period rewrites the whole clause, and marking the whole
		// clause struck would hand a partner a paragraph to re-read for one
		// number. So the revision is narrowed to the words that actually
		// differ - whole words, cut at spaces, because half a word struck is
		// not something a lawyer can read.
		struct Revision {
			std::size_t from = 0;
			std::size_t to = 0;
			std::string text;
		};
		const auto narrowed = [&](std::size_t from, std::size_t to, const std::string &text) {
			Revision revision{from, to, text};
			const std::string was = flat.text.substr(from, to - from);
			revision.text = with_document_typography(was, revision.text);
			const RevisionSpan span = word_revision_span(was, revision.text);
			if (span.shared_prefix > 0 || span.shared_suffix > 0) {
				revision.from = from + span.shared_prefix;
				revision.to = to - span.shared_suffix;
				revision.text = revision.text.substr(span.shared_prefix,
					revision.text.size() - span.shared_prefix - span.shared_suffix);
			}
			// A space both sides share stands outside the mark, as it does in Word:
			// "the ~~six (6)~~ twelve (12) months", never the spaces struck too. A
			// tab or a line break both sides share stays outside it the same way.
			const auto separator = [](char character) { return character == ' ' || character == '\t' || character == '\n'; };
			while (revision.to > revision.from && !revision.text.empty() && separator(revision.text.back())
					&& flat.text[revision.to - 1] == revision.text.back()) {
				--revision.to;
				revision.text.pop_back();
			}
			while (revision.to > revision.from && !revision.text.empty() && separator(revision.text.front())
					&& flat.text[revision.from] == revision.text.front()) {
				++revision.from;
				revision.text.erase(0, 1);
			}
			return revision;
		};
		// The words in [from, to) given runs of their own, in document order -
		// only those of `cell` when one is named.
		const auto struckAmong = [&](const std::vector<TextRun> &runs, std::size_t from, std::size_t to, pugi::xml_node cell) {
			std::vector<pugi::xml_node> struck;
			// No words between: nothing is struck, and no run of nothing is cut
			// out of the words around the point (words are only added there).
			if (from >= to) return struck;
			for (const TextRun &run : runs) {
				if (cell && run.cell != cell) continue;
				const std::size_t runEnd = run.start + run.length;
				if (run.length == 0 || runEnd <= from || run.start >= to) continue;
				pugi::xml_node middle = run_for_span(run.node, from > run.start ? from - run.start : 0,
					to < runEnd ? to - run.start : run.length);
				if (middle) struck.push_back(middle);
			}
			return struck;
		};
		const auto struckBetween = [&](std::size_t from, std::size_t to, pugi::xml_node cell) {
			return struckAmong(flat.runs, from, to, cell);
		};
		// The replacement is written like the words it replaces, beside them.
		const auto writeBeside = [&](pugi::xml_node at, pugi::xml_node like, const std::string &text, bool before) {
			if (text.empty()) return;
			pugi::xml_node parent = at.parent();
			pugi::xml_node run = before ? parent.insert_copy_before(like, at) : parent.insert_copy_after(like, at);
			std::vector<pugi::xml_node> remove;
			for (pugi::xml_node child : run.children()) if (!local_name_is(child, "rPr")) remove.push_back(child);
			for (pugi::xml_node child : remove) run.remove_child(child);
			for (pugi::xml_node child : run.children()) if (local_name_is(child, "rPr")) drop_what_belongs_to_the_model(child);
			// Several lines stay several lines: Word shows a line feed inside a
			// piece of text as a space, so the lines the lawyer approved would
			// otherwise read as one run-on sentence.
			set_run_text_with_lines(run.append_child((prefix_of(run) + "t").c_str()), text);
			wrap_run(run, "ins", nextRevision(), request.tracked);
		};
		// A replacement is two revisions, as Word records one: the old words
		// struck, then the new words inserted after them, with the ids in the
		// order the reader meets them.
		const auto strikeThenWrite = [&](const std::vector<pugi::xml_node> &struck, const std::string &text) {
			// The insertion is written beside the old words before they are
			// struck, so it takes their formatting as it stands; its id is
			// taken after theirs.
			std::vector<int> deletions;
			std::size_t stretches = 0;
			for (std::size_t index = 0; index < struck.size(); ++index)
				if (index == 0 || struck[index].previous_sibling() != struck[index - 1]) ++stretches;
			for (std::size_t index = 0; index < stretches; ++index) deletions.push_back(nextRevision());
			writeBeside(struck.back(), struck.front(), text, false);
			std::size_t used = 0;
			mark_runs_deleted(struck, [&]() { return deletions[used++]; }, request.tracked);
		};
		// Words added where nothing is taken out go in at the point the
		// passage reached, inside the run that holds that point - the first
		// the passage covers there, so words that follow a run go after it -
		// exactly where a lawyer would have typed them.
		const auto insertAmong = [&](const std::vector<const TextRun *> &runs, std::size_t at, const std::string &text,
				pugi::xml_node cell) {
			for (const TextRun *run : runs) {
				if (cell && run->cell != cell) continue;
				if (run->length == 0 || at < run->start || at > run->start + run->length) continue;
				const std::size_t offset = at - run->start;
				if (offset == 0) {
					pugi::xml_node whole = run_for_span(run->node, 0, run->length);
					if (!whole) return false;
					writeBeside(whole, whole, text, true);
					return true;
				}
				pugi::xml_node head = run_for_span(run->node, 0, offset);
				if (!head) return false;
				writeBeside(head, head, text, false);
				return true;
			}
			return false;
		};
		const auto insertAt = [&](std::size_t at, const std::string &text, pugi::xml_node cell) {
			return insertAmong(covered, at, text, cell);
		};
		if (!cellTexts.empty()) {
			// Row by row: each cell keeps its own words, and only the words that
			// differ in it are struck and written beside them - so a table reads
			// as a table, and a word a cell keeps is never written into it twice.
			for (std::size_t index = 0; index < coveredCells.size() && index < cellTexts.size(); ++index) {
				const pugi::xml_node cell = coveredCells[index];
				std::size_t cellStart = std::string::npos;
				std::size_t cellEnd = 0;
				for (const TextRun *run : covered) {
					if (run->cell != cell || run->length == 0) continue;
					cellStart = std::min(cellStart, std::max(run->start, start));
					cellEnd = std::max(cellEnd, std::min(run->start + run->length, end));
				}
				if (cellStart == std::string::npos || cellStart >= cellEnd) {
					// A cell with no words yet - only an empty piece of text - takes
					// its line as an insertion, written like that piece: a blank the
					// row fills in is filled in, never silently left blank.
					const std::string line = prose_plain_text(cellTexts[index]);
					if (line.empty()) continue;
					pugi::xml_node blank;
					for (const TextRun *run : covered) {
						if (run->cell == cell) { blank = run_of(run->node); break; }
					}
					if (!blank) {
						outcome.reason = "The passage this change is anchored to has no run of its own to mark.";
						return outcome;
					}
					writeBeside(blank, blank, line, false);
					continue;
				}
				const Revision revision = narrowed(cellStart, cellEnd, prose_plain_text(cellTexts[index]));
				if (revision.from >= revision.to && revision.text.empty()) continue;
				const std::vector<pugi::xml_node> struck = struckBetween(revision.from, revision.to, cell);
				if (struck.empty()) {
					if (!insertAt(revision.from, revision.text, cell)) {
						outcome.reason = "The passage this change is anchored to has no run of its own to mark.";
						return outcome;
					}
					continue;
				}
				strikeThenWrite(struck, revision.text);
			}
			outcome.ok = true;
			return outcome;
		}
		if (byParagraphs) {
			std::optional<TextEditOutcome> made = [&]() -> std::optional<TextEditOutcome> {
				// What Word draws in front of a paragraph, as paragraph_like reads it.
				// Read once per paragraph, as the document stood before this change:
				// counting a number walks the whole body, and the lines are set
				// against the numbers the model read.
				std::map<const void *, std::optional<std::string>> drawnBefore;
				const auto drawnOf = [&](pugi::xml_node paragraph) -> std::optional<std::string> {
					const auto known = drawnBefore.find(paragraph.internal_object());
					if (known != drawnBefore.end()) return known->second;
					const std::optional<std::string> drawn = request.numbered_automatically
						? request.numbered_automatically(paragraph)
						: names_its_own_list(paragraph) ? std::optional<std::string>(std::string()) : std::nullopt;
					drawnBefore.emplace(paragraph.internal_object(), drawn);
					return drawn;
				};
				const auto oldWords = [&](const ParagraphSegment &segment) {
					return segment.split ? std::string() : flat.text.substr(segment.from, segment.to - segment.from);
				};
				// A line as it is written into a paragraph: plain words, without a
				// number typed in the form Word draws that paragraph's own (the D08
				// rule: "7.1. " where Word draws "7.1."). A number the old words
				// themselves open with is the clause's own text, and stays.
				const auto lineIn = [&](std::size_t line, const ParagraphSegment &segment) {
					std::string plain = prose_plain_text(newLines[line]);
					const std::optional<std::string> drawn = drawnOf(segment.paragraph);
					if (drawn) {
						const std::size_t typed = typed_number_like_drawn(plain, *drawn);
						if (typed > 0 && typed < plain.size() && typed_number_like_drawn(oldWords(segment), *drawn) == 0)
							plain.erase(0, typed);
					}
					return plain;
				};
				// A line may rewrite a numbered paragraph only where it is typed as
				// an item of that paragraph's own level, or typed with no number:
				// "7.1. ..." never rewrites clause "7.".
				const auto fits = [&](std::size_t old, std::size_t line) {
					const std::optional<std::string> drawn = drawnOf(segments[old].paragraph);
					if (!drawn || drawn->empty()) return true;
					const std::string plain = prose_plain_text(newLines[line]);
					// "Section 7.1 of the Agreement" opens with words, not a mark.
					const std::size_t first = plain.find_first_not_of(" \t");
					const bool opensWithNumber = first != std::string::npos
						&& std::isdigit(static_cast<unsigned char>(plain[first])) != 0 && typed_clause_number_length(plain) > 0;
					if (typed_item_mark_length(plain) == 0 && !opensWithNumber) return true;
					return typed_number_like_drawn(plain, *drawn) > 0;
				};
				// How much of a paragraph a line keeps, in whole words at either end.
				const auto kept = [&](std::size_t old, std::size_t line) -> long long {
					const std::string was = oldWords(segments[old]);
					const std::string now = with_document_typography(was, lineIn(line, segments[old]));
					if (now == was) return 2 * static_cast<long long>(was.size()) + 2;
					const RevisionSpan span = word_revision_span(was, now);
					return static_cast<long long>(span.shared_prefix + span.shared_suffix) + 1;
				};

				// A paragraph that keeps words before the passage takes the first
				// line; one that keeps words after it, the last. A passage inside one
				// paragraph with words on both sides is split where it ends, as Enter
				// splits a paragraph in Word: the words that followed it stand in a
				// paragraph of their own and take the last line.
				const bool splits = segments.size() == 1 && segments.front().head && segments.front().tail;
				const TextRun *lastRun = nullptr;
				if (splits) {
					for (const TextRun *run : covered)
						if (run->paragraph == segments.front().paragraph && run->length > 0 && run->start < end) lastRun = run;
					if (lastRun == nullptr || run_of(lastRun->node) != lastRun->node.parent()
							|| run_of(lastRun->node).parent() != segments.front().paragraph)
						return std::nullopt;
					segments.front().tail = false;
					ParagraphSegment rest;
					rest.tail = true;
					rest.split = true;
					segments.push_back(rest);
				}
				const std::size_t n = segments.size();
				const std::size_t m = newLines.size();
				enum class Step { Rewrite, Remove, Add };
				struct Planned {
					Step step;
					std::size_t old = 0;
					std::size_t line = 0;
				};
				std::vector<Planned> plan;
				std::vector<Planned> closing;
				std::size_t oldFrom = 0, oldTo = n, lineFrom = 0, lineTo = m;
				if (segments.front().head) {
					plan.push_back({Step::Rewrite, 0, 0});
					oldFrom = 1;
					lineFrom = 1;
				}
				if (segments.back().tail && oldTo > oldFrom && lineTo > lineFrom) {
					closing.push_back({Step::Rewrite, n - 1, m - 1});
					oldTo = n - 1;
					lineTo = m - 1;
				} else if (segments.back().tail) {
					return std::nullopt;
				}
				// The paragraphs between: each line set against the old paragraph it
				// keeps the most of, in order, so a clause added in the middle is
				// added there and the clauses around it keep their words.
				const std::size_t a = oldTo - oldFrom;
				const std::size_t b = lineTo - lineFrom;
				// A paragraph's own properties - its style, list and indents - as text.
				const auto ownProperties = [](pugi::xml_node paragraph) {
					std::string text;
					for (pugi::xml_node properties : paragraph.children()) {
						if (!local_name_is(properties, "pPr")) continue;
						for (pugi::xml_node child : properties.children()) {
							if (local_name_is(child, "rPr") || local_name_is(child, "sectPr") || local_name_is(child, "pPrChange"))
								continue;
							text += xml_text_of(child);
						}
					}
					return text;
				};
				// The last paragraph of a body or a cell keeps its mark (Word
				// requires one). It goes only as Word takes it: the clause before it
				// gives up its mark instead, and its words close on the last mark -
				// so only where the two are written alike.
				const auto keepsLastMark = [&](std::size_t old) {
					return old + 1 == n && !paragraph_follows(segments[old].paragraph);
				};
				const auto removable = [&](std::size_t old) {
					if (!keepsLastMark(old)) return true;
					return old > 0 && ownProperties(segments[old - 1].paragraph) == ownProperties(segments[old].paragraph);
				};
				constexpr long long kNever = std::numeric_limits<long long>::min() / 4;
				std::vector<std::vector<long long>> best(a + 1, std::vector<long long>(b + 1, kNever));
				std::vector<std::vector<Step>> how(a + 1, std::vector<Step>(b + 1, Step::Add));
				best[0][0] = 0;
				for (std::size_t i = 0; i <= a; ++i) {
					for (std::size_t j = 0; j <= b; ++j) {
						if (i == 0 && j == 0) continue;
						if (i > 0 && j > 0 && best[i - 1][j - 1] > kNever && fits(oldFrom + i - 1, lineFrom + j - 1)) {
							const long long score = best[i - 1][j - 1] + kept(oldFrom + i - 1, lineFrom + j - 1);
							if (score > best[i][j]) { best[i][j] = score; how[i][j] = Step::Rewrite; }
						}
						if (i > 0 && best[i - 1][j] > kNever && removable(oldFrom + i - 1) && best[i - 1][j] > best[i][j]) {
							best[i][j] = best[i - 1][j];
							how[i][j] = Step::Remove;
						}
						if (j > 0 && best[i][j - 1] > kNever && best[i][j - 1] > best[i][j]) {
							best[i][j] = best[i][j - 1];
							how[i][j] = Step::Add;
						}
					}
				}
				if (best[a][b] <= kNever) return std::nullopt;
				std::vector<Planned> middle;
				for (std::size_t i = a, j = b; i > 0 || j > 0;) {
					const Step step = how[i][j];
					if (step == Step::Rewrite) { middle.push_back({step, oldFrom + i - 1, lineFrom + j - 1}); --i; --j; }
					else if (step == Step::Remove) { middle.push_back({step, oldFrom + i - 1, 0}); --i; }
					else { middle.push_back({step, 0, lineFrom + j - 1}); --j; }
				}
				plan.insert(plan.end(), middle.rbegin(), middle.rend());
				plan.insert(plan.end(), closing.begin(), closing.end());
				// A last paragraph taken out gives its mark to the clause the plan
				// keeps before it, which must be written like it and must be an old
				// clause: an added one's look is not known until it is written.
				for (std::size_t index = 0; index < plan.size(); ++index) {
					if (plan[index].step != Step::Remove || !keepsLastMark(plan[index].old)) continue;
					if (index + 1 < plan.size()) return std::nullopt;
					std::size_t kept = index;
					while (kept > 0 && plan[kept - 1].step == Step::Remove) --kept;
					if (kept == 0 || plan[kept - 1].step != Step::Rewrite
							|| ownProperties(segments[plan[kept - 1].old].paragraph) != ownProperties(segments[plan[index].old].paragraph))
						return std::nullopt;
				}

				// ---- Written ------------------------------------------------------
				if (splits) {
					pugi::xml_node paragraph = segments.front().paragraph;
					run_for_span(lastRun->node, 0, end - lastRun->start);
					const pugi::xml_node splitAfter = run_of(lastRun->node);
					pugi::xml_node rest = paragraph.parent().insert_child_after(paragraph.name(), paragraph);
					// The paragraph's own mark - its properties, a section break, any
					// revision already recorded on it - stays with the words after the
					// passage; the paragraph before it ends in a new mark, inserted.
					for (pugi::xml_node child : paragraph.children()) {
						if (!local_name_is(child, "pPr")) continue;
						rest.append_move(child);
						drop_what_belongs_to_the_model(paragraph.prepend_copy(rest.first_child()));
						break;
					}
					for (pugi::xml_node node = splitAfter.next_sibling(); node;) {
						const pugi::xml_node next = node.next_sibling();
						rest.append_move(node);
						node = next;
					}
					mark_paragraph_mark(paragraph, "ins", nextRevision(), request.tracked);
					segments.back().paragraph = rest;
				}
				pugi::xml_node body = segments.front().paragraph.parent();
				while (body && !local_name_is(body, "body")) body = body.parent();
				TextEditOutcome result;
				result.ok = true;
				result.matches = outcome.matches;
				result.anchor_exact = outcome.anchor_exact;
				std::vector<std::string> writtenLines;
				// The paragraph last written or kept, in document order.
				pugi::xml_node cursor;
				// The paragraphs this change adds, in document order.
				std::vector<pugi::xml_node> added;
				for (std::size_t index = 0; index < plan.size(); ++index) {
					const Planned &planned = plan[index];
					if (planned.step == Step::Add) {
						// Written like the clause before it, or - at the head of the
						// passage - like the clause after it.
						pugi::xml_node model = cursor;
						for (std::size_t other = index + 1; !model && other < plan.size(); ++other)
							if (plan[other].step != Step::Add) model = segments[plan[other].old].paragraph;
						if (!model) model = segments.front().paragraph;
						const std::vector<ProseParagraph> line = prose_paragraphs(newLines[planned.line]);
						const ProseParagraph &asWritten = line.front();
						pugi::xml_node like = asWritten.heading_level == 0
							? paragraph_to_write_like(model, false, request.is_heading) : model;
						if (!like) like = model;
						std::vector<std::string> made;
						const pugi::xml_node created = cursor
							? paragraph_like(cursor, like, asWritten, heading_model(body, asWritten.heading_level, request.is_heading),
								false, request, made)
							: paragraph_like(segments.front().paragraph, like, asWritten,
								heading_model(body, asWritten.heading_level, request.is_heading), true, request, made);
						mark_paragraph_inserted(created, nextRevision, request.tracked);
						if (!made.empty()) writtenLines.push_back(made.back());
						cursor = created;
						added.push_back(created);
						continue;
					}
					const ParagraphSegment &segment = segments[planned.old];
					cursor = segment.paragraph;
					if (planned.step == Step::Remove) {
						// Taken out whole: every run it shows - a footnote's mark, a
						// picture, a field - with its words, and its mark with them.
						mark_runs_deleted(runs_of_paragraph(segment.paragraph), nextRevision, request.tracked);
						if (!keepsLastMark(planned.old)) {
							mark_paragraph_mark(segment.paragraph, "del", nextRevision(), request.tracked);
							continue;
						}
						// The last mark stays; the clause kept before it gives up its own.
						for (pugi::xml_node before = segment.paragraph.previous_sibling(); before; before = before.previous_sibling()) {
							if (!local_name_is(before, "p") || paragraph_mark_revision(before, "del")) continue;
							mark_paragraph_mark(before, "del", nextRevision(), request.tracked);
							break;
						}
						continue;
					}
					// The last line, where the paragraph it closes on keeps words after
					// the passage, is typed as an item of another level - "(a)" after
					// words of clause "2." - or stands in a paragraph the split has just
					// made: that paragraph goes where an added one would, at the level
					// that draws the mark or out of the list, never "3. (a) ...".
					std::string text = lineIn(planned.line, segment);
					if (index + 1 == plan.size() && !closing.empty() && (segment.split || !fits(planned.old, planned.line))) {
						pugi::xml_node before = segment.paragraph.previous_sibling();
						while (before && !local_name_is(before, "p")) before = before.previous_sibling();
						const std::vector<ProseParagraph> asTyped = prose_paragraphs(newLines[planned.line]);
						if (before && !asTyped.empty() && asTyped.front().heading_level == 0) {
							const ListPlacement placement = list_placement(before, segment.paragraph, asTyped.front(), false, request);
							if (placement.movesInList()) {
								std::string words;
								for (const ProseRun &run : placement.writing.runs) words += run.text;
								text = words;
								const pugi::xml_node moved = segment.paragraph;
								record_formatting_change(paragraph_properties(moved),
									[&]() { apply_list_placement(moved, placement); }, nextRevision(), request.tracked);
							}
						}
					}
					writtenLines.push_back(text);
					if (segment.split || segment.from == segment.to) {
						// Nothing of the passage stands in the paragraph: the line goes
						// in front of the words it keeps, written like them.
						pugi::xml_node like;
						for (pugi::xml_node run : visible_runs(segment.paragraph)) {
							if (run_has_content(run)) { like = run; break; }
						}
						if (like) {
							writeBeside(like, like, text, true);
						} else {
							pugi::xml_node paragraph = segment.paragraph;
							pugi::xml_node run = paragraph.append_child((prefix_of(paragraph) + "r").c_str());
							set_run_text_with_lines(run.append_child((prefix_of(segment.paragraph) + "t").c_str()), text);
							wrap_run(run, "ins", nextRevision(), request.tracked);
						}
						continue;
					}
					const Revision revision = narrowed(segment.from, segment.to, text);
					if (revision.from >= revision.to && revision.text.empty()) continue;
					const std::vector<pugi::xml_node> struck = struckBetween(revision.from, revision.to, pugi::xml_node());
					if (struck.empty()) {
						if (!insertAt(revision.from, revision.text, pugi::xml_node())) {
							result.ok = false;
							result.reason = "The passage this change is anchored to has no run of its own to mark.";
							return result;
						}
						continue;
					}
					strikeThenWrite(struck, revision.text);
				}
				// Paragraphs added after the last one of the body or cell: the last
				// mark stays last, as with Enter pressed at its end in Word.
				if (!added.empty() && !paragraph_follows(added.back())) {
					pugi::xml_node anchor = added.back();
					while (anchor && (!local_name_is(anchor, "p") || std::find(added.begin(), added.end(), anchor) != added.end()))
						anchor = anchor.previous_sibling();
					keep_final_mark_last(anchor, added.back(), nextRevision, request.tracked);
				}
				std::string joined;
				for (const std::string &line : writtenLines) joined += (joined.empty() ? "" : "\n") + line;
				result.written.push_back(joined);
				return result;
			}();
			if (made) return *made;
		}
		std::size_t revisionStart = start;
		std::size_t revisionEnd = end;
		std::string revisionText = replacement;
		if (request.kind == TextEditKind::Replace && !replacement.empty()) {
			// Words a sentence keeps between its changes stay unmarked: each
			// stretch that differs is a revision of its own, as a lawyer marks a
			// clause up by hand. Inside one paragraph; a change across paragraphs
			// is marked from its first difference to its last.
			const std::string was = flat.text.substr(start, end - start);
			const std::string now = with_document_typography(was, replacement);
			const std::vector<RevisionHunk> hunks = was.find('\n') == std::string::npos && now.find('\n') == std::string::npos
				? word_revision_hunks(was, now) : std::vector<RevisionHunk>();
			if (hunks.size() > 1) {
				// The last first, each on the document as the ones after it left
				// it, read again: the text before a revision never moves, so each
				// stretch before it stands where it stood.
				std::ptrdiff_t moved = 0;
				for (auto hunk = hunks.rbegin(); hunk != hunks.rend(); ++hunk) {
					const FlatDocument fresh = flatten(document);
					const std::size_t from = start + hunk->was_from;
					const std::size_t to = start + hunk->was_to;
					const std::size_t passageEnd = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(end) + moved);
					const std::string text = now.substr(hunk->now_from, hunk->now_to - hunk->now_from);
					if (from == to) {
						// Words added where nothing goes: written in at that point.
						std::vector<const TextRun *> around;
						for (const TextRun &run : fresh.runs)
							if (run.start + run.length > start && run.start < passageEnd) around.push_back(&run);
						if (!insertAmong(around, from, text, pugi::xml_node())) {
							outcome.reason = "The passage this change is anchored to has no run of its own to mark.";
							return outcome;
						}
					} else {
						const std::vector<pugi::xml_node> struck = struckAmong(fresh.runs, from, to, pugi::xml_node());
						if (struck.empty()) {
							outcome.reason = "The passage this change is anchored to has no run of its own to mark.";
							return outcome;
						}
						strikeThenWrite(struck, text);
					}
					moved += static_cast<std::ptrdiff_t>(text.size()) - static_cast<std::ptrdiff_t>(to - from);
				}
				outcome.ok = true;
				return outcome;
			}
			const Revision revision = narrowed(start, end, replacement);
			// Word for word the same: there is nothing to record.
			if (revision.from >= revision.to && revision.text.empty()) {
				outcome.ok = true;
				return outcome;
			}
			revisionStart = revision.from;
			revisionEnd = revision.to;
			revisionText = revision.text;
		}
		// A deletion closes the gap it leaves: the space before the deleted
		// words and the space after them were one separation, so one of them is
		// struck with the words and the sentence reads with a single space once
		// the deletion is accepted.
		if (request.kind == TextEditKind::Delete && revisionStart > 0 && revisionEnd < flat.text.size()
				&& flat.text[revisionStart - 1] == ' ' && flat.text[revisionEnd] == ' ')
			++revisionEnd;
		const std::vector<pugi::xml_node> struck = struckBetween(revisionStart, revisionEnd, pugi::xml_node());
		if (struck.empty()) {
			// Nothing was struck because nothing differs across whole words: the
			// new words go in where the passage stood, and nothing comes out.
			if (request.kind == TextEditKind::Replace && !revisionText.empty()
					&& insertAt(revisionStart, revisionText, pugi::xml_node())) {
				outcome.ok = true;
				return outcome;
			}
			outcome.reason = "The passage this change is anchored to has no run of its own to mark.";
			return outcome;
		}
		strikeThenWrite(struck, revisionText);
		// A paragraph the deletion takes whole goes with its words: its mark is
		// struck too, so accepting the deletion leaves no blank line - or empty
		// numbered item - where the clause stood.
		if (request.kind == TextEditKind::Delete) {
			for (std::size_t index = 0; index < flat.paragraphs.size(); ++index) {
				const auto [from, to] = flat.spans[index];
				if (to <= from || from < revisionStart || to > revisionEnd) continue;
				if (paragraph_follows(flat.paragraphs[index]))
					mark_paragraph_mark(flat.paragraphs[index], "del", nextRevision(), request.tracked);
			}
		}
		// A replacement that runs across paragraphs replaces the breaks between
		// them too: the mark of each paragraph whose end the struck words cross
		// is struck, so accepting the change leaves the new words where the old
		// ones stood - never an empty line, or an orphaned "5.", above them. The
		// paragraph that holds the new words keeps its own mark.
		if (request.kind == TextEditKind::Replace) {
			pugi::xml_node receiving;
			for (pugi::xml_node up = struck.back().parent(); up; up = up.parent())
				if (local_name_is(up, "p")) { receiving = up; break; }
			for (std::size_t index = 0; index < flat.paragraphs.size(); ++index) {
				const std::size_t markAt = flat.spans[index].second;
				if (markAt < revisionStart || markAt >= revisionEnd || flat.paragraphs[index] == receiving) continue;
				if (paragraph_follows(flat.paragraphs[index]))
					mark_paragraph_mark(flat.paragraphs[index], "del", nextRevision(), request.tracked);
			}
		}
		outcome.ok = true;
		return outcome;
	}
	bool written = false;
	std::vector<pugi::xml_node> touchedParagraphs;
	for (const TextRun *run : covered) {
		const std::size_t runEnd = run->start + run->length;
		const std::size_t from = start > run->start ? start - run->start : 0;
		const std::size_t to = end < runEnd ? end - run->start : run->length;
		const std::string value = run->node.text().as_string();
		std::string kept = value.substr(0, from);
		const std::string tail = to <= value.size() ? value.substr(to) : std::string();
		if (!cellTexts.empty()) {
			// Cell by cell: each cell's covered text becomes that cell's own new
			// line, and the first run of each cell is the one that carries it.
			const std::size_t cellIndex = static_cast<std::size_t>(std::distance(coveredCells.begin(),
				std::find(coveredCells.begin(), coveredCells.end(), run->cell)));
			bool firstRunOfCell = true;
			for (const TextRun *earlier : covered) {
				if (earlier == run) break;
				if (earlier->cell == run->cell) { firstRunOfCell = false; break; }
			}
			std::string cellKept = kept;
			if (firstRunOfCell && cellIndex < cellTexts.size()) cellKept += cellTexts[cellIndex];
			cellKept += tail;
			if (cellKept.find_first_of("\n\t") != std::string::npos) set_run_text_with_lines(run->node, cellKept);
			else set_run_text(run->node, cellKept);
			if (std::find(touchedParagraphs.begin(), touchedParagraphs.end(), run->paragraph)
					== touchedParagraphs.end())
				touchedParagraphs.push_back(run->paragraph);
			continue;
		}
		if (!written) {
			// A deletion closes the gap it leaves: the space before the deleted
			// words and the space after them were one separation, and a lawyer
			// who deletes a sentence does not want two spaces where it stood.
			if (request.kind == TextEditKind::Delete && !kept.empty() && kept.back() == ' '
					&& !tail.empty() && tail.front() == ' ')
				kept.pop_back();
			kept += replacement;
			written = true;
		}
		kept += tail;
		if (kept.find_first_of("\n\t") != std::string::npos) set_run_text_with_lines(run->node, kept);
		else set_run_text(run->node, kept);
		if (std::find(touchedParagraphs.begin(), touchedParagraphs.end(), run->paragraph)
				== touchedParagraphs.end())
			touchedParagraphs.push_back(run->paragraph);
	}
	// A paragraph a deletion emptied goes with the text it held: a lawyer who
	// deletes a sentence that was a paragraph does not want the blank line.
	if (request.kind == TextEditKind::Delete) {
		// And a paragraph does not end in the space that separated the deleted
		// words from what came before them.
		for (pugi::xml_node paragraph : touchedParagraphs) {
			pugi::xml_node last;
			visit(paragraph, [&](pugi::xml_node node) {
				if (local_name_is(node, "t") && *node.text().as_string() != '\0') last = node;
			});
			if (!last) continue;
			std::string value = last.text().as_string();
			while (!value.empty() && value.back() == ' ') value.pop_back();
			set_run_text(last, value);
		}
		for (pugi::xml_node paragraph : touchedParagraphs) {
			if (paragraph_has_text(paragraph)) continue;
			// Never the last paragraph of a body or cell: Word requires one.
			pugi::xml_node parent = paragraph.parent();
			std::size_t paragraphs = 0;
			for (pugi::xml_node child : parent.children()) if (local_name_is(child, "p")) ++paragraphs;
			if (paragraphs <= 1) continue;
			parent.remove_child(paragraph);
		}
	}
	outcome.ok = true;
	return outcome;
}

// ---- A new Word document ----------------------------------------------------

namespace {

std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> lines;
	std::string current;
	for (char character : text) {
		if (character == '\n') {
			lines.push_back(current);
			current.clear();
			continue;
		}
		if (character != '\r') current.push_back(character);
	}
	lines.push_back(current);
	return lines;
}

// One item of a list as it was typed: how deep it is set, what kind of mark it
// has, the number that mark stands for, and its words. A clause numbered the
// way contracts number them - "1.2", "3.1.4" - also has the numbers of the
// clauses it stands under (`parents`: 1, and 3 and 1), and its pattern has a
// place for each ("%.%", "%.%.%").
struct TypedListItem {
	std::size_t indent = 0;
	std::string kind;
	std::string pattern;
	int value = 0;
	std::string token;
	std::string words;
	std::vector<int> parents;
};

int roman_value(const std::string &token) {
	int total = 0;
	int previous = 0;
	for (auto it = token.rbegin(); it != token.rend(); ++it) {
		const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(*it)));
		const int value = c == 'i' ? 1 : c == 'v' ? 5 : c == 'x' ? 10 : c == 'l' ? 50 : c == 'c' ? 100
			: c == 'd' ? 500 : c == 'm' ? 1000 : 0;
		if (value == 0) return 0;
		total += value < previous ? -value : value;
		previous = std::max(previous, value);
	}
	return total;
}

bool is_lower_roman(const std::string &token) {
	return !token.empty() && token.size() <= 6
		&& token.find_first_not_of("ivxlcdm") == std::string::npos && roman_value(token) > 0;
}

// A line typed as an item of a list: "- words", "1. words", "2) words",
// "(a) words", "b) words", "(iv) words". Nothing for any other line.
std::optional<TypedListItem> typed_list_item(const std::string &line) {
	TypedListItem item;
	std::size_t at = 0;
	while (at < line.size() && line[at] == ' ') ++at;
	item.indent = at;
	const auto wordsAfter = [&](std::size_t end) {
		if (end >= line.size() || line[end] != ' ') return false;
		while (end < line.size() && line[end] == ' ') ++end;
		if (end >= line.size()) return false;
		item.words = line.substr(end);
		return true;
	};
	for (const char *bullet : {"-", "*", "+", "\xE2\x80\xA2"}) {
		const std::size_t length = std::strlen(bullet);
		if (line.compare(at, length, bullet) == 0 && wordsAfter(at + length)) {
			item.kind = "bullet";
			return item;
		}
	}
	// "1.2 words", "3.1.4. words": a clause under the clauses its first numbers
	// name. At most four numbers deep, each of at most four digits.
	{
		std::vector<int> numbers;
		std::size_t end = at;
		while (true) {
			std::size_t digits = end;
			while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])) != 0) ++digits;
			if (digits == end || digits - end > 4) break;
			numbers.push_back(std::stoi(line.substr(end, digits - end)));
			end = digits;
			if (end + 1 < line.size() && line[end] == '.' && std::isdigit(static_cast<unsigned char>(line[end + 1])) != 0) {
				++end;
				continue;
			}
			break;
		}
		if (numbers.size() >= 2 && numbers.size() <= 4) {
			std::string closer;
			if (end < line.size() && (line[end] == '.' || line[end] == ')')) closer = line.substr(end++, 1);
			if (wordsAfter(end)) {
				item.kind = "decimal";
				item.value = numbers.back();
				item.token = line.substr(at, end - at - closer.size());
				item.parents.assign(numbers.begin(), numbers.end() - 1);
				item.pattern = "%";
				for (std::size_t part = 1; part < numbers.size(); ++part) item.pattern += ".%";
				item.pattern += closer;
				return item;
			}
			item.words.clear();
		}
	}
	const bool isOpened = at < line.size() && line[at] == '(';
	const std::size_t tokenStart = isOpened ? at + 1 : at;
	std::size_t tokenEnd = tokenStart;
	while (tokenEnd < line.size() && std::isalnum(static_cast<unsigned char>(line[tokenEnd])) != 0) ++tokenEnd;
	if (tokenEnd == tokenStart || tokenEnd >= line.size()) return std::nullopt;
	item.token = line.substr(tokenStart, tokenEnd - tokenStart);
	const char closer = line[tokenEnd];
	if (isOpened && closer != ')') return std::nullopt;
	if (!isOpened && closer != '.' && closer != ')') return std::nullopt;
	item.pattern = isOpened ? "(%)" : closer == '.' ? "%." : "%)";
	if (std::all_of(item.token.begin(), item.token.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
		if (item.token.size() > 4) return std::nullopt;
		item.kind = "decimal";
		item.value = std::stoi(item.token);
	} else if (item.pattern != "%." && item.token.size() == 1 && std::islower(static_cast<unsigned char>(item.token[0])) != 0) {
		// A single letter; "(i)" may yet prove to be the first of a roman list.
		item.kind = "lowerLetter";
		item.value = item.token[0] - 'a' + 1;
	} else if (item.pattern != "%." && is_lower_roman(item.token)) {
		item.kind = "lowerRoman";
		item.value = roman_value(item.token);
	} else {
		return std::nullopt;
	}
	if (!wordsAfter(tokenEnd + 1)) return std::nullopt;
	return item;
}

// A line of a Markdown table: "| a | b |". Its cells, trimmed; nothing for a
// line that is not one.
std::optional<std::vector<std::string>> table_line_cells(const std::string &line) {
	const std::size_t first = line.find_first_not_of(" \t");
	const std::size_t last = line.find_last_not_of(" \t");
	if (first == std::string::npos || line[first] != '|' || last == first) return std::nullopt;
	const std::string inner = line.substr(first + 1, (line[last] == '|' ? last : last + 1) - first - 1);
	std::vector<std::string> cells;
	std::size_t start = 0;
	while (true) {
		const std::size_t bar = inner.find('|', start);
		const std::string cell = inner.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
		const std::size_t from = cell.find_first_not_of(" \t");
		const std::size_t to = cell.find_last_not_of(" \t");
		cells.push_back(from == std::string::npos ? std::string() : cell.substr(from, to - from + 1));
		if (bar == std::string::npos) break;
		start = bar + 1;
	}
	if (cells.size() < 2) return std::nullopt;
	return cells;
}

// "|---|:---:|": the line under a Markdown table's heading row.
bool is_table_rule(const std::vector<std::string> &cells) {
	return std::all_of(cells.begin(), cells.end(), [](const std::string &cell) {
		std::string bare = cell;
		bare.erase(std::remove(bare.begin(), bare.end(), ':'), bare.end());
		return bare.size() >= 3 && bare.find_first_not_of('-') == std::string::npos;
	});
}

// Whether an item is a clause numbered the way contracts number them - "1.",
// "1.2", "3.1.4" - whose depth its own numbers say, whatever its indentation.
bool is_clause_item(const TypedListItem &item) {
	return item.kind == "decimal" && !item.pattern.empty() && item.pattern.front() == '%';
}

// A run of typed items as a list Word numbers: each level takes the kind and
// punctuation of its first item, and the numbers typed must be the ones Word
// would count - each level going up by one, starting again under each new
// item above it, and a clause "2.3" standing under clause "2". Nothing for a
// run that does not count that way.
//
// Items stand as deep as they are indented; where the run numbers clauses as
// "1.2", a clause stands as deep as its numbers say, and any other item - an
// "(a)" - one level under the item it follows.
std::optional<std::vector<NewWordListLevel>> list_levels(std::vector<TypedListItem> &items,
		std::vector<std::size_t> &levels) {
	constexpr std::size_t kDeepest = 4;
	levels.clear();
	const bool isDotted = std::any_of(items.begin(), items.end(), [](const TypedListItem &item) {
		return !item.parents.empty();
	});
	if (!isDotted) {
		std::vector<std::size_t> indents;
		for (const TypedListItem &item : items) {
			while (!indents.empty() && indents.back() > item.indent) indents.pop_back();
			if (indents.empty() || indents.back() < item.indent) indents.push_back(item.indent);
			if (indents.size() > kDeepest) return std::nullopt;
			levels.push_back(indents.size() - 1);
		}
	} else {
		struct Open {
			std::size_t indent = 0;
			std::size_t level = 0;
			bool clause = false;
		};
		std::vector<Open> open;
		for (const TypedListItem &item : items) {
			std::size_t level = 0;
			if (is_clause_item(item)) {
				level = item.parents.size();
				while (!open.empty() && open.back().level >= level) open.pop_back();
				open.push_back({item.indent, level, true});
			} else {
				while (!open.empty() && !open.back().clause && open.back().indent >= item.indent) open.pop_back();
				if (open.empty()) return std::nullopt;
				level = open.back().level + 1;
				open.push_back({item.indent, level, false});
			}
			if (level >= kDeepest) return std::nullopt;
			levels.push_back(level);
		}
	}
	// "(i)" is roman where its level also holds a "(ii)" or a "(v)"; a letter
	// otherwise.
	for (std::size_t level = 0; level < kDeepest; ++level) {
		bool isRoman = false;
		for (std::size_t index = 0; index < items.size(); ++index)
			if (levels[index] == level && items[index].kind == "lowerRoman") isRoman = true;
		if (!isRoman) continue;
		for (std::size_t index = 0; index < items.size(); ++index) {
			if (levels[index] != level || items[index].kind != "lowerLetter" || !is_lower_roman(items[index].token)) continue;
			items[index].kind = "lowerRoman";
			items[index].value = roman_value(items[index].token);
		}
	}
	std::vector<NewWordListLevel> definitions(kDeepest);
	std::vector<bool> defined(kDeepest, false);
	std::vector<int> expected(kDeepest, 0);
	// The number each level last counted, 0 where it has counted none since
	// the level above it last did.
	std::vector<int> current(kDeepest, 0);
	for (std::size_t index = 0; index < items.size(); ++index) {
		const TypedListItem &item = items[index];
		const std::size_t level = levels[index];
		if (!defined[level]) {
			definitions[level].kind = item.kind;
			definitions[level].pattern = item.pattern;
			definitions[level].start = item.kind == "bullet" ? 1 : item.value;
			defined[level] = true;
			expected[level] = definitions[level].start;
		}
		const NewWordListLevel &definition = definitions[level];
		if (item.kind != definition.kind || item.pattern != definition.pattern) return std::nullopt;
		if (item.kind != "bullet" && item.value != expected[level]) return std::nullopt;
		// "2.3" stands under clause 2 and its clause 2.x above it, as counted.
		for (std::size_t part = 0; part < item.parents.size(); ++part)
			if (part >= level || current[part] == 0 || item.parents[part] != current[part]) return std::nullopt;
		current[level] = item.value;
		expected[level] += 1;
		// Every level below this one starts again under it.
		for (std::size_t deeper = level + 1; deeper < kDeepest; ++deeper) {
			current[deeper] = 0;
			if (defined[deeper]) expected[deeper] = definitions[deeper].start;
		}
	}
	std::size_t used = 0;
	for (std::size_t level = 0; level < kDeepest; ++level) if (defined[level]) used = level + 1;
	// A level no item stands at, between two that items do, is not a list.
	for (std::size_t level = 0; level < used; ++level) if (!defined[level]) return std::nullopt;
	definitions.resize(used);
	return definitions;
}

bool is_blank_line(const std::string &line) {
	return line.find_first_not_of(" \t") == std::string::npos;
}

} // namespace

NewWordDocument new_word_document(const std::string &content, const std::vector<std::string> &columns,
		const std::vector<std::vector<std::string>> &rows) {
	NewWordDocument document;
	const std::vector<std::string> lines = content.empty() ? std::vector<std::string>{} : lines_of(content);
	bool isAfterBlank = false;
	// The last number the latest list counted at its first level.
	int lastTopValue = 0;
	const auto addParagraph = [&](const std::string &line) {
		for (const ProseParagraph &paragraph : prose_paragraphs(line)) {
			NewWordBlock block;
			block.paragraph = paragraph;
			block.spaced = isAfterBlank && paragraph.heading_level == 0;
			document.blocks.push_back(std::move(block));
		}
		isAfterBlank = false;
	};
	std::size_t index = 0;
	while (index < lines.size()) {
		const std::string &line = lines[index];
		if (is_blank_line(line)) {
			isAfterBlank = true;
			++index;
			continue;
		}
		// A table: every line of it a row between bars.
		if (table_line_cells(line)) {
			NewWordBlock table;
			table.kind = NewWordBlock::Kind::Table;
			std::size_t width = 0;
			while (index < lines.size()) {
				const auto cells = table_line_cells(lines[index]);
				if (!cells) break;
				++index;
				if (is_table_rule(*cells)) {
					if (table.rows.size() == 1) table.heading = true;
					continue;
				}
				width = std::max(width, cells->size());
				table.rows.push_back(*cells);
			}
			for (std::vector<std::string> &row : table.rows) row.resize(width);
			if (!table.rows.empty()) document.blocks.push_back(std::move(table));
			isAfterBlank = false;
			continue;
		}
		// A list: its items one after another; a blank line between two of them
		// does not end it.
		if (typed_list_item(line)) {
			std::vector<TypedListItem> items;
			std::vector<std::string> typed;
			// Whether a blank line stood before each item: space above it, as
			// between any two paragraphs.
			std::vector<bool> spaced;
			bool isBlankBefore = isAfterBlank;
			std::size_t end = index;
			while (end < lines.size()) {
				if (is_blank_line(lines[end])) {
					std::size_t next = end;
					while (next < lines.size() && is_blank_line(lines[next])) ++next;
					if (next < lines.size() && typed_list_item(lines[next])) {
						end = next;
						isBlankBefore = true;
						continue;
					}
					break;
				}
				const auto item = typed_list_item(lines[end]);
				if (!item) break;
				// A bulleted list followed straight away by a numbered one is two
				// lists: another kind of item at the list's own depth begins one.
				// Clauses numbered "1.", "1.1", "1.2" are one list, and an "(a)"
				// typed under one of them is an item of it, not a list of its own.
				const bool isClauses = !item->parents.empty()
					|| std::any_of(items.begin(), items.end(), [](const TypedListItem &typedItem) { return !typedItem.parents.empty(); });
				const bool staysInClauses = isClauses && (is_clause_item(*item) || item->kind != "bullet");
				if (!items.empty() && item->indent <= items.front().indent
						&& (item->kind != items.front().kind || item->pattern != items.front().pattern)
						&& !(item->kind == "lowerRoman" && items.front().kind == "lowerLetter") && !staysInClauses)
					break;
				items.push_back(*item);
				typed.push_back(lines[end]);
				spaced.push_back(isBlankBefore);
				isBlankBefore = false;
				++end;
			}
			std::vector<std::size_t> levels;
			const auto definitions = list_levels(items, levels);
			if (!definitions) {
				for (std::size_t item = 0; item < typed.size(); ++item) {
					isAfterBlank = spaced[item];
					addParagraph(typed[item]);
				}
			} else {
				// A list that goes on counting where the list before it stopped -
				// clauses 1 and 2 under one heading, 3 and 4 under the next - is
				// that same list, so Word renumbers across both when a clause is
				// added to either.
				std::size_t list = document.lists.size();
				if (!document.lists.empty() && levels.front() == 0 && lastTopValue > 0
						&& items.front().value == lastTopValue + 1) {
					std::vector<NewWordListLevel> &previous = document.lists.back();
					bool isSame = true;
					for (std::size_t level = 0; level < std::min(previous.size(), definitions->size()); ++level)
						if (previous[level].kind != (*definitions)[level].kind
								|| previous[level].pattern != (*definitions)[level].pattern)
							isSame = false;
					if (isSame) {
						list = document.lists.size() - 1;
						for (std::size_t level = previous.size(); level < definitions->size(); ++level)
							previous.push_back((*definitions)[level]);
					}
				}
				if (list == document.lists.size()) document.lists.push_back(*definitions);
				lastTopValue = 0;
				for (std::size_t item = 0; item < items.size(); ++item)
					if (levels[item] == 0) lastTopValue = items[item].kind == "bullet" ? 0 : items[item].value;
				for (std::size_t item = 0; item < items.size(); ++item) {
					NewWordBlock block;
					block.kind = NewWordBlock::Kind::ListItem;
					block.list = list;
					block.level = levels[item];
					block.paragraph = prose_paragraphs(items[item].words).front();
					block.spaced = spaced[item];
					document.blocks.push_back(std::move(block));
				}
				isAfterBlank = false;
			}
			index = end;
			continue;
		}
		addParagraph(line);
		++index;
	}
	// A table given beside the text stands after it, its columns as its heading.
	if (!rows.empty()) {
		NewWordBlock table;
		table.kind = NewWordBlock::Kind::Table;
		table.heading = !columns.empty();
		if (table.heading) table.rows.push_back(columns);
		for (const std::vector<std::string> &row : rows) table.rows.push_back(row);
		std::size_t width = 0;
		for (const std::vector<std::string> &row : table.rows) width = std::max(width, row.size());
		for (std::vector<std::string> &row : table.rows) row.resize(width);
		document.blocks.push_back(std::move(table));
	}
	return document;
}

std::string new_word_document_words(const NewWordDocument &document) {
	std::string words;
	const auto add = [&](const std::string &text) {
		if (!words.empty()) words.push_back('\n');
		words += text;
	};
	for (const NewWordBlock &block : document.blocks) {
		if (block.kind == NewWordBlock::Kind::Table) {
			for (const std::vector<std::string> &row : block.rows) {
				std::string line;
				for (std::size_t index = 0; index < row.size(); ++index)
					line += (index == 0 ? "" : "\t") + prose_plain_text(row[index]);
				add(line);
			}
			continue;
		}
		std::string text;
		for (const ProseRun &run : block.paragraph.runs) text += run.text;
		add(text);
	}
	return words;
}

void write_new_word_body(pugi::xml_node body, const std::string &title, const NewWordDocument &document,
		int usableWidthTwips) {
	// Its marks become the document's own bold, italics and headings, and a
	// mark a lawyer never typed never reaches the page. The title and each
	// heading carry their style and, as well, the look and outline level a
	// reader that ignores styles still shows: bold, a little larger, the title
	// centred.
	const auto addRuns = [](pugi::xml_node node, const ProseParagraph &paragraph, bool isTitle, bool isBold) {
		const int halfPoints = isTitle ? 32 : paragraph.heading_level == 1 ? 28 : paragraph.heading_level > 1 ? 24 : 0;
		for (const ProseRun &piece : paragraph.runs) {
			pugi::xml_node run = node.append_child("w:r");
			const bool bold = piece.bold || isTitle || isBold || paragraph.heading_level > 0;
			if (bold || piece.italic || halfPoints > 0) {
				pugi::xml_node properties = run.append_child("w:rPr");
				if (bold) properties.append_child("w:b");
				if (piece.italic) properties.append_child("w:i");
				if (halfPoints > 0) properties.append_child("w:sz").append_attribute("w:val") = halfPoints;
			}
			set_run_text(run.append_child("w:t"), piece.text);
		}
	};
	const auto addProse = [&](const ProseParagraph &paragraph, bool isTitle, bool isSpaced) {
		pugi::xml_node node = body.append_child("w:p");
		const bool isOutlined = isTitle || paragraph.heading_level > 0;
		// The title at the top of the outline and each part below it at the
		// level its heading gives it, so Word's navigation and Singularity's own
		// reader see the document as a lawyer does.
		if (isOutlined || isSpaced) {
			pugi::xml_node properties = node.append_child("w:pPr");
			if (isOutlined) {
				properties.append_child("w:pStyle").append_attribute("w:val") = isTitle ? "Title"
					: paragraph.heading_level == 1 ? "Heading1" : paragraph.heading_level == 2 ? "Heading2" : "Heading3";
				properties.append_child("w:keepNext");
			}
			// Where the model left a blank line: a little more space above.
			if (isSpaced) properties.append_child("w:spacing").append_attribute("w:before") = 160;
			if (isTitle) properties.append_child("w:jc").append_attribute("w:val") = "center";
			// The title is the first level; a part written "# Parties" sits one
			// below it, whatever heading style its look comes from.
			if (isOutlined)
				properties.append_child("w:outlineLvl").append_attribute("w:val") =
					isTitle ? 0 : std::min(paragraph.heading_level, 8);
		}
		addRuns(node, paragraph, isTitle, false);
	};
	// A list item: a paragraph Word numbers, at its level of its own list.
	const auto addItem = [&](const NewWordBlock &block) {
		pugi::xml_node node = body.append_child("w:p");
		pugi::xml_node properties = node.append_child("w:pPr");
		pugi::xml_node numbering = properties.append_child("w:numPr");
		numbering.append_child("w:ilvl").append_attribute("w:val") = static_cast<unsigned int>(block.level);
		numbering.append_child("w:numId").append_attribute("w:val") = static_cast<unsigned int>(block.list + 1);
		pugi::xml_node spacing = properties.append_child("w:spacing");
		if (block.spaced) spacing.append_attribute("w:before") = 160;
		spacing.append_attribute("w:after") = 80;
		addRuns(node, block.paragraph, false, false);
	};
	// A table across the page, its columns sharing the width, its heading row
	// repeated on each page and set in bold.
	const auto addTable = [&](const NewWordBlock &block) {
		const std::size_t width = block.rows.empty() ? 0 : block.rows.front().size();
		if (width == 0) return;
		const int column = usableWidthTwips / static_cast<int>(width);
		pugi::xml_node table = body.append_child("w:tbl");
		pugi::xml_node properties = table.append_child("w:tblPr");
		properties.append_child("w:tblStyle").append_attribute("w:val") = "TableGrid";
		pugi::xml_node tableWidth = properties.append_child("w:tblW");
		tableWidth.append_attribute("w:w") = usableWidthTwips;
		tableWidth.append_attribute("w:type") = "dxa";
		pugi::xml_node grid = table.append_child("w:tblGrid");
		for (std::size_t index = 0; index < width; ++index) grid.append_child("w:gridCol").append_attribute("w:w") = column;
		for (std::size_t rowIndex = 0; rowIndex < block.rows.size(); ++rowIndex) {
			pugi::xml_node row = table.append_child("w:tr");
			const bool isHeading = block.heading && rowIndex == 0;
			if (isHeading) row.append_child("w:trPr").append_child("w:tblHeader");
			for (const std::string &text : block.rows[rowIndex]) {
				pugi::xml_node cell = row.append_child("w:tc");
				pugi::xml_node cellWidth = cell.append_child("w:tcPr").append_child("w:tcW");
				cellWidth.append_attribute("w:w") = column;
				cellWidth.append_attribute("w:type") = "dxa";
				ProseParagraph words;
				for (const ProseParagraph &line : prose_paragraphs(text))
					for (const ProseRun &run : line.runs) words.runs.push_back(run);
				addRuns(cell.append_child("w:p"), words, false, isHeading);
			}
		}
	};
	if (!title.empty()) for (const ProseParagraph &line : prose_paragraphs(title)) addProse(line, true, false);
	for (const NewWordBlock &block : document.blocks) {
		if (block.kind == NewWordBlock::Kind::Table) addTable(block);
		else if (block.kind == NewWordBlock::Kind::ListItem) addItem(block);
		else addProse(block.paragraph, false, block.spaced);
	}
	if (!document.blocks.empty() && document.blocks.back().kind == NewWordBlock::Kind::Table) body.append_child("w:p");
}

std::string new_word_numbering_part(const NewWordDocument &document) {
	if (document.lists.empty()) return {};
	std::string xml = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
		R"(<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)";
	for (std::size_t list = 0; list < document.lists.size(); ++list) {
		xml += "<w:abstractNum w:abstractNumId=\"" + std::to_string(list) + "\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
		for (std::size_t level = 0; level < document.lists[list].size(); ++level) {
			const NewWordListLevel &definition = document.lists[list][level];
			const bool isBullet = definition.kind == "bullet";
			std::string text = "\xE2\x80\xA2";
			std::size_t places = 1;
			if (!isBullet) {
				// Each place for a number is the number of a level: the last this
				// level's own, those before it the levels above ("%1.%2" for
				// "1.2").
				text.clear();
				places = static_cast<std::size_t>(std::count(definition.pattern.begin(), definition.pattern.end(), '%'));
				std::size_t place = 0;
				for (char character : definition.pattern) {
					if (character != '%') {
						text.push_back(character);
						continue;
					}
					const std::size_t named = level + 1 + place + 1 > places ? level + 1 + place + 1 - places : 1;
					text += "%" + std::to_string(named);
					++place;
				}
			}
			// Each level hangs its number in the margin, half an inch deeper
			// than the one above it; a number of several parts ("1.2") hangs
			// further, so it never runs into the words.
			xml += "<w:lvl w:ilvl=\"" + std::to_string(level) + "\"><w:start w:val=\"" + std::to_string(definition.start)
				+ "\"/><w:numFmt w:val=\"" + definition.kind + "\"/><w:lvlText w:val=\"" + text + "\"/><w:lvlJc w:val=\"left\"/>"
				"<w:pPr><w:ind w:left=\"" + std::to_string(720 * (level + 1)) + "\" w:hanging=\""
				+ std::to_string(places > 1 ? 720 : 360) + "\"/></w:pPr></w:lvl>";
		}
		xml += "</w:abstractNum>";
	}
	for (std::size_t list = 0; list < document.lists.size(); ++list)
		xml += "<w:num w:numId=\"" + std::to_string(list + 1) + "\"><w:abstractNumId w:val=\"" + std::to_string(list)
			+ "\"/></w:num>";
	return xml + "</w:numbering>";
}

} // namespace singularity::documents
