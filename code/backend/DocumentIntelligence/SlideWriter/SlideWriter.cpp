#include "DocumentIntelligence/SlideWriter/SlideWriter.hpp"
#include "Document/PptxExtractor/PptxExtractor.hpp"
#include "Platform/PhraseMatch/PhraseMatch.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

#include <pugixml.hpp>
#include <zip.h>

namespace singularity::document_intelligence {
namespace {

constexpr std::uint64_t kMostSlideBytes = 16ULL * 1024ULL * 1024ULL;

std::string_view localName(const char* qualified) {
	const std::string_view name(qualified);
	const std::size_t colon = name.find(':');
	return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

bool isElement(const pugi::xml_node& node, std::string_view name) {
	return node.type() == pugi::node_element && localName(node.name()) == name;
}

// One piece of a paragraph's words: the text of one run (its a:t), a field's
// text (a slide number, a date - PowerPoint writes it again, so it is never
// edited), or a line break, read as the space the deck shows there.
struct Piece {
	pugi::xml_node text;
	pugi::xml_node run;
	bool isBreak = false;
	bool isField = false;
	std::size_t start = 0;
	std::size_t length = 0;
};

struct Paragraph {
	pugi::xml_node node;
	std::string text;
	std::vector<Piece> pieces;
};

Paragraph readParagraph(const pugi::xml_node& node) {

	Paragraph paragraph;
	paragraph.node = node;
	for (pugi::xml_node child = node.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (isElement(child, "br") == true) {
			Piece piece;
			piece.isBreak = true;
			piece.start = paragraph.text.size();
			piece.length = 1;
			paragraph.text += ' ';
			paragraph.pieces.push_back(piece);
			continue;
		}
		const bool isRun = isElement(child, "r") == true;
		const bool isField = isElement(child, "fld") == true;
		if (isRun == false && isField == false) continue;
		for (pugi::xml_node text = child.first_child(); text.empty() == false; text = text.next_sibling()) {
			if (isElement(text, "t") == false) continue;
			Piece piece;
			piece.text = text;
			piece.run = child;
			piece.isField = isField;
			piece.start = paragraph.text.size();
			const std::string words = text.child_value();
			piece.length = words.size();
			paragraph.text += words;
			paragraph.pieces.push_back(piece);
		}
	}
	return paragraph;

}

// Every paragraph of a slide's text - its shapes, its groups and the cells of
// its tables - in the order the slide holds them.
void collectParagraphs(const pugi::xml_node& node, std::vector<Paragraph>& paragraphs) {
	for (pugi::xml_node child = node.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (child.type() != pugi::node_element) continue;
		if (isElement(child, "p") == true && isElement(node, "txBody") == true) {
			paragraphs.push_back(readParagraph(child));
			continue;
		}
		collectParagraphs(child, paragraphs);
	}
}

std::vector<Paragraph> paragraphsOf(const pugi::xml_document& slide) {
	std::vector<Paragraph> paragraphs;
	collectParagraphs(slide, paragraphs);
	return paragraphs;
}

class Archive final {
public:
	Archive(const std::filesystem::path& path, int flags) {
		int error = 0;
		archive_ = zip_open(path.u8string().c_str(), flags, &error);
	}
	Archive(const Archive&) = delete;
	Archive& operator=(const Archive&) = delete;
	~Archive() {
		if (archive_ != nullptr) zip_discard(archive_);
	}
	zip_t* get() const { return archive_; }
	// Writes what was replaced; false when it could not be.
	bool close() {
		if (archive_ == nullptr) return false;
		const bool isClosed = zip_close(archive_) == 0;
		if (isClosed == true) archive_ = nullptr;
		return isClosed;
	}

private:
	zip_t* archive_ = nullptr;
};

std::optional<std::string> readEntry(zip_t* archive, const std::string& name) {
	zip_stat_t entry;
	zip_stat_init(&entry);
	if (zip_stat(archive, name.c_str(), 0, &entry) != 0 || (entry.valid & ZIP_STAT_SIZE) == 0) return std::nullopt;
	if (entry.size > kMostSlideBytes) return std::nullopt;
	zip_file_t* file = zip_fopen(archive, name.c_str(), 0);
	if (file == nullptr) return std::nullopt;
	std::string bytes(static_cast<std::size_t>(entry.size), '\0');
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const zip_int64_t read = zip_fread(file, bytes.data() + offset, static_cast<zip_uint64_t>(bytes.size() - offset));
		if (read <= 0) break;
		offset += static_cast<std::size_t>(read);
	}
	zip_fclose(file);
	if (offset != bytes.size()) return std::nullopt;
	return bytes;
}

// A slide's XML, read so it is written back as it was: every space kept, the
// declaration kept. A part that declares a DTD or an entity is not read.
bool parseSlide(const std::string& bytes, pugi::xml_document& document) {
	if (bytes.find("<!DOCTYPE") != std::string::npos || bytes.find("<!ENTITY") != std::string::npos) return false;
	const pugi::xml_parse_result parsed = document.load_buffer(bytes.data(), bytes.size(),
		pugi::parse_default | pugi::parse_declaration | pugi::parse_ws_pcdata, pugi::encoding_utf8);
	return static_cast<bool>(parsed);
}

std::string serialized(const pugi::xml_document& document) {
	std::ostringstream written;
	document.save(written, "", pugi::format_raw, pugi::encoding_utf8);
	return written.str();
}

std::string trimmedWords(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string onOneLine(std::string text) {
	for (char& character : text) {
		if (character == '\n' || character == '\r' || character == '\t') character = ' ';
	}
	return text;
}

// The lines of an edit's new words, each a paragraph of the slide: a bullet is
// one line, so a line the colleague starts is a bullet of its own. A dash or a
// bullet sign before a line is the list's mark, as the deck is shown to the
// colleague, never words: the paragraph draws its own bullet.
std::vector<std::string> linesOf(const std::string& text) {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t end = text.find('\n', start);
		std::string line = trimmedWords(onOneLine(text.substr(start, end == std::string::npos ? std::string::npos : end - start)));
		if (line.empty() == false) lines.push_back(std::move(line));
		if (end == std::string::npos) break;
		start = end + 1;
	}
	if (lines.size() > 1) {
		for (std::string& line : lines) {
			for (const char* mark : {"- ", "* ", "\xE2\x80\xA2 ", "\xE2\x80\x93 ", "\xE2\x80\x94 "}) {
				if (line.rfind(mark, 0) == 0) {
					line = trimmedWords(line.substr(std::strlen(mark)));
					break;
				}
			}
		}
	}
	return lines;
}

// A new paragraph after `after`, at the level of `model` - its bullet, indent
// and spacing - in the formatting of the run that holds most of `model`'s
// words, as pressing Enter at the end of a bullet starts the next one.
pugi::xml_node addParagraphAfter(const pugi::xml_node& after, const pugi::xml_node& model, const std::string& words) {

	const std::string name = model.name();
	const std::size_t colon = name.find(':');
	const std::string prefix = colon == std::string::npos ? std::string() : name.substr(0, colon + 1);
	pugi::xml_node properties;
	pugi::xml_node end;
	pugi::xml_node fullest;
	std::size_t most = 0;
	for (pugi::xml_node child = model.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (isElement(child, "pPr") == true) properties = child;
		if (isElement(child, "endParaRPr") == true) end = child;
		if (isElement(child, "r") == false) continue;
		std::size_t length = 0;
		for (pugi::xml_node text = child.first_child(); text.empty() == false; text = text.next_sibling()) {
			if (isElement(text, "t") == true) length += std::strlen(text.child_value());
		}
		if (fullest.empty() == true || length > most) {
			most = length;
			fullest = child;
		}
	}
	pugi::xml_node style;
	for (pugi::xml_node child = fullest.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (isElement(child, "rPr") == true) style = child;
	}
	pugi::xml_node paragraph = after.parent().insert_child_after(name.c_str(), after);
	if (properties.empty() == false) paragraph.append_copy(properties);
	pugi::xml_node run = paragraph.append_child((prefix + "r").c_str());
	if (style.empty() == false) {
		pugi::xml_node copied = run.append_copy(style);
		// New words are checked for spelling afresh, as PowerPoint checks typed ones.
		copied.remove_attribute("err");
		copied.remove_attribute("dirty");
	}
	run.append_child((prefix + "t").c_str()).text().set(words.c_str());
	if (end.empty() == false) paragraph.append_copy(end);
	return paragraph;

}

// Whether only spaces and a closing mark stand after `from` in a paragraph: the
// edit's words reach the paragraph's end.
bool reachesEnd(const std::string& text, std::size_t from) {
	for (std::size_t index = from; index < text.size(); ++index) {
		if (std::strchr(" \t.,;:!?)\"'", text[index]) == nullptr) return false;
	}
	return true;
}

struct Slide {
	unsigned int number = 0;
	std::string part;
	pugi::xml_document document;
	bool isChanged = false;
	// The slide as the deck holds it, to put back when its edits are refused.
	std::string original;
};

// Where an edit's words stand: the paragraph, and their bytes in it.
struct Place {
	std::size_t slide = 0;
	std::size_t paragraph = 0;
	std::size_t start = 0;
	std::size_t length = 0;
};

// The places `words` stand at, on `only` (0: every slide). Counting stops at
// two: an edit is only ever written where its words stand once.
std::vector<Place> placesOf(std::vector<Slide>& slides, const std::string& words, unsigned int only) {
	std::vector<Place> places;
	for (std::size_t slideIndex = 0; slideIndex < slides.size(); ++slideIndex) {
		if (only != 0 && slides[slideIndex].number != only) continue;
		const std::vector<Paragraph> paragraphs = paragraphsOf(slides[slideIndex].document);
		for (std::size_t index = 0; index < paragraphs.size(); ++index) {
			const platform::PhrasePlacement placement = platform::locate_phrase(paragraphs[index].text, words);
			if (placement.matches == 0) continue;
			places.push_back(Place{slideIndex, index, placement.start, placement.length});
			if (placement.matches > 1 || places.size() > 1) {
				places.push_back(places.back());
				return places;
			}
		}
	}
	return places;
}

// The words of a passage, each with where it stands in it.
struct Token {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string text;
};

std::vector<Token> tokensOf(const std::string& text) {
	std::vector<Token> tokens;
	std::size_t at = 0;
	const auto isSpaceAt = [&](std::size_t index) {
		const char character = text[index];
		if (character == ' ' || character == '\t' || character == '\r' || character == '\n') return std::size_t(1);
		if (static_cast<unsigned char>(character) == 0xC2 && index + 1 < text.size()
				&& static_cast<unsigned char>(text[index + 1]) == 0xA0)
			return std::size_t(2);
		return std::size_t(0);
	};
	while (at < text.size()) {
		if (const std::size_t space = isSpaceAt(at); space > 0) {
			at += space;
			continue;
		}
		Token token;
		token.start = at;
		while (at < text.size() && isSpaceAt(at) == 0) ++at;
		token.end = at;
		token.text = text.substr(token.start, token.end - token.start);
		tokens.push_back(std::move(token));
	}
	return tokens;
}

// An edit narrowed to the words that differ, as a lawyer marks a deck up: the
// words both readings share at the edit's ends stay as the slide writes them,
// in their own runs - a figure in bold keeps its bold when only the figure
// changes. `start` and `length` are bytes of `before`.
struct Narrowed {
	bool isUnchanged = false;
	std::size_t start = 0;
	std::size_t length = 0;
	std::string words;
};

Narrowed narrowedEdit(const std::string& before, const std::string& after) {

	Narrowed narrowed;
	const std::vector<Token> old = tokensOf(before);
	const std::vector<Token> fresh = tokensOf(after);
	const std::size_t n = old.size();
	const std::size_t m = fresh.size();
	std::size_t prefix = 0;
	while (prefix < n && prefix < m && platform::phrases_equal(old[prefix].text, fresh[prefix].text) == true) ++prefix;
	std::size_t suffix = 0;
	while (suffix < n - prefix && suffix < m - prefix
			&& platform::phrases_equal(old[n - 1 - suffix].text, fresh[m - 1 - suffix].text) == true)
		++suffix;
	if (prefix == n && prefix == m) {
		narrowed.isUnchanged = true;
		return narrowed;
	}
	std::string middle;
	for (std::size_t index = prefix; index + suffix < m; ++index) {
		if (middle.empty() == false) middle += ' ';
		middle += fresh[index].text;
	}
	std::size_t start = 0;
	std::size_t end = before.size();
	if (n - prefix - suffix > 0 && middle.empty() == false) {
		start = old[prefix].start;
		end = old[n - suffix - 1].end;
		narrowed.words = middle;
	} else if (n - prefix - suffix == 0) {
		// Words added between two the slide keeps, or at an end of the passage.
		if (prefix > 0 && suffix > 0) {
			start = old[prefix - 1].end;
			end = old[n - suffix].start;
			narrowed.words = " " + middle + " ";
		} else if (prefix > 0) {
			start = old[prefix - 1].end;
			end = start;
			narrowed.words = " " + middle;
		} else {
			start = old[0].start;
			end = start;
			narrowed.words = middle + " ";
		}
	} else {
		// Words taken out, with the space before or after them.
		if (suffix > 0) {
			start = old[prefix].start;
			end = old[n - suffix].start;
		} else if (prefix > 0) {
			start = old[prefix - 1].end;
			end = old[n - 1].end;
		}
	}
	narrowed.start = start;
	narrowed.length = end - start;
	return narrowed;

}

// Writes `words` in place of a paragraph's bytes [start, start + length), in the
// run the replaced words begin in - its font, size, colour and emphasis. Runs
// the replacement empties go; a field or a line break inside it refuses it.
std::string replaceInParagraph(Paragraph& paragraph, std::size_t start, std::size_t length, const std::string& words) {

	const std::size_t end = start + length;
	std::vector<std::size_t> touched;
	if (length == 0) {
		// Words added at one point go into the run that point stands in, or the
		// one it ends.
		for (std::size_t pass = 0; pass < 2 && touched.empty() == true; ++pass) {
			for (std::size_t index = 0; index < paragraph.pieces.size(); ++index) {
				const Piece& piece = paragraph.pieces[index];
				if (piece.isBreak == true || piece.isField == true) continue;
				const std::size_t pieceEnd = piece.start + piece.length;
				const bool isInside = pass == 0 ? piece.start <= start && start < pieceEnd : pieceEnd == start;
				if (isInside == true) {
					touched.push_back(index);
					break;
				}
			}
		}
	} else {
		for (std::size_t index = 0; index < paragraph.pieces.size(); ++index) {
			const Piece& piece = paragraph.pieces[index];
			const std::size_t pieceEnd = piece.start + piece.length;
			if (piece.start < end && pieceEnd > start) touched.push_back(index);
		}
	}
	if (touched.empty() == true) return "not_found";
	for (std::size_t index : touched) {
		if (paragraph.pieces[index].isBreak == true) return "line_break";
		if (paragraph.pieces[index].isField == true) return "field";
	}
	Piece& first = paragraph.pieces[touched.front()];
	Piece& last = paragraph.pieces[touched.back()];
	const std::string firstWords = first.text.child_value();
	const std::string lastWords = last.text.child_value();
	const std::string head = firstWords.substr(0, start - first.start);
	const std::string tail = end - last.start <= lastWords.size() ? lastWords.substr(end - last.start) : std::string();
	if (touched.size() == 1) {
		first.text.text().set((head + words + tail).c_str());
		return "written";
	}
	first.text.text().set((head + words).c_str());
	for (std::size_t position = 1; position + 1 < touched.size(); ++position) {
		Piece& middle = paragraph.pieces[touched[position]];
		middle.text.text().set("");
	}
	last.text.text().set(tail.c_str());
	// A run whose words the replacement took whole goes, as PowerPoint leaves it.
	for (std::size_t position = 1; position < touched.size(); ++position) {
		Piece& piece = paragraph.pieces[touched[position]];
		if (std::string(piece.text.child_value()).empty() == false) continue;
		pugi::xml_node run = piece.run;
		bool isEmpty = true;
		for (pugi::xml_node text = run.first_child(); text.empty() == false; text = text.next_sibling()) {
			if (isElement(text, "t") == true && std::string(text.child_value()).empty() == false) isEmpty = false;
		}
		if (isEmpty == true) run.parent().remove_child(run);
	}
	return "written";

}

// The paragraph taken out whole, as deleting a bullet does - unless it is the
// only paragraph of its text body, which PowerPoint never leaves without one:
// then its words go and the paragraph stays.
bool removeParagraph(Paragraph& paragraph) {
	pugi::xml_node body = paragraph.node.parent();
	std::size_t count = 0;
	for (pugi::xml_node child = body.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (isElement(child, "p") == true) ++count;
	}
	if (count > 1) return body.remove_child(paragraph.node);
	std::vector<pugi::xml_node> runs;
	for (pugi::xml_node child = paragraph.node.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (isElement(child, "r") == true || isElement(child, "br") == true) runs.push_back(child);
	}
	for (pugi::xml_node run : runs) paragraph.node.remove_child(run);
	return true;
}

std::vector<std::vector<std::string>> paragraphTexts(std::vector<Slide>& slides) {
	std::vector<std::vector<std::string>> texts;
	for (Slide& slide : slides) {
		std::vector<std::string> words;
		for (const Paragraph& paragraph : paragraphsOf(slide.document)) words.push_back(paragraph.text);
		texts.push_back(std::move(words));
	}
	return texts;
}

bool loadSlides(const std::filesystem::path& path, std::vector<Slide>& slides, std::string& problem) {

	const std::vector<std::string> parts = documents::pptx_slide_part_names(path);
	if (parts.empty() == true) {
		problem = "The presentation's slides could not be read.";
		return false;
	}
	Archive archive(path, ZIP_RDONLY);
	if (archive.get() == nullptr) {
		problem = "The presentation could not be opened.";
		return false;
	}
	slides.clear();
	slides.reserve(parts.size());
	for (std::size_t index = 0; index < parts.size(); ++index) {
		const std::optional<std::string> bytes = readEntry(archive.get(), parts[index]);
		slides.emplace_back();
		Slide& slide = slides.back();
		slide.number = static_cast<unsigned int>(index + 1);
		slide.part = parts[index];
		if (bytes.has_value() == false || parseSlide(*bytes, slide.document) == false) {
			problem = "Slide " + std::to_string(index + 1) + " of the presentation could not be read.";
			return false;
		}
	}
	return true;

}


// ---- Whether a slide's words still fit it -----------------------
//
// Unicorn rewrote the IC deck's diligence slide as eighteen lines in a box that
// grows with its words: PowerPoint drew them over the footer and the last off
// the slide. A slide's words are measured as PowerPoint sets them - each
// paragraph wrapped to its box at its size, with its spacing - against the room
// the slide gives the box: down to the top of the next shape below it, or to the
// slide's foot. The measure is an estimate, so only a clear excess counts.

constexpr double kEmuPerPoint = 12700.0;
// An average letter of a deck's sans-serif words, as a share of their size.
constexpr double kLetterWidth = 0.48;
// PowerPoint's single line spacing, as a share of the words' size.
constexpr double kLineHeight = 1.2;
// How far past its room a box's words may run before they count as not fitting.
constexpr double kFitTolerance = 1.04;

pugi::xml_node childNamed(const pugi::xml_node& node, std::string_view name) {
	for (pugi::xml_node child = node.first_child(); child.empty() == false; child = child.next_sibling()) {
		if (isElement(child, name) == true) return child;
	}
	return pugi::xml_node();
}

long long numberOf(const pugi::xml_node& node, const char* attribute, long long fallback) {
	const pugi::xml_attribute value = node.attribute(attribute);
	return value.empty() == true ? fallback : value.as_llong(fallback);
}

std::size_t lettersIn(const std::string& text) {
	std::size_t letters = 0;
	for (const char byte : text) {
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++letters;
	}
	return letters;
}

struct Box {
	std::string id;
	long long x = 0;
	long long y = 0;
	long long width = 0;
	long long height = 0;
	bool isKnown = false;
};

// Where a shape stands on its slide, when the slide says so itself: a
// placeholder that takes its place from the layout is not measured.
Box boxOf(const pugi::xml_node& shape) {
	Box box;
	for (pugi::xml_node names = shape.first_child(); names.empty() == false; names = names.next_sibling()) {
		const pugi::xml_node identity = childNamed(names, "cNvPr");
		if (identity.empty() == false) box.id = identity.attribute("id").as_string();
	}
	pugi::xml_node transform = childNamed(childNamed(shape, "spPr"), "xfrm");
	if (transform.empty() == true) transform = childNamed(shape, "xfrm");
	if (transform.empty() == true) transform = childNamed(childNamed(shape, "grpSpPr"), "xfrm");
	const pugi::xml_node offset = childNamed(transform, "off");
	const pugi::xml_node extent = childNamed(transform, "ext");
	if (offset.empty() == true || extent.empty() == true) return box;
	box.x = numberOf(offset, "x", 0);
	box.y = numberOf(offset, "y", 0);
	box.width = numberOf(extent, "cx", 0);
	box.height = numberOf(extent, "cy", 0);
	box.isKnown = box.width > 0;
	return box;
}

double spacingOf(const pugi::xml_node& spacing, double size) {
	const pugi::xml_node points = childNamed(spacing, "spcPts");
	if (points.empty() == false) return static_cast<double>(numberOf(points, "val", 0)) / 100.0;
	const pugi::xml_node share = childNamed(spacing, "spcPct");
	if (share.empty() == false) return static_cast<double>(numberOf(share, "val", 0)) / 100000.0 * size;
	return 0.0;
}

struct Measure {
	double height = 0.0;
	double lines = 0.0;
};

// The height, in points, a text body's words take in a box `width` points wide.
Measure measureWords(const pugi::xml_node& body, double width) {
	Measure measure;
	const pugi::xml_node properties = childNamed(body, "bodyPr");
	const bool wraps = std::string_view(properties.attribute("wrap").as_string("square")) != "none";
	double scale = 1.0;
	const pugi::xml_node shrinks = childNamed(properties, "normAutofit");
	if (shrinks.empty() == false) scale = static_cast<double>(numberOf(shrinks, "fontScale", 100000)) / 100000.0;
	measure.height = static_cast<double>(numberOf(properties, "tIns", 45720) + numberOf(properties, "bIns", 45720)) / kEmuPerPoint;
	const double inner = width
		- static_cast<double>(numberOf(properties, "lIns", 91440) + numberOf(properties, "rIns", 91440)) / kEmuPerPoint;
	double size = 18.0;
	for (pugi::xml_node paragraph = body.first_child(); paragraph.empty() == false; paragraph = paragraph.next_sibling()) {
		if (isElement(paragraph, "p") == false) continue;
		std::string words;
		bool isSized = false;
		for (pugi::xml_node child = paragraph.first_child(); child.empty() == false; child = child.next_sibling()) {
			const bool isEnd = isElement(child, "endParaRPr");
			if (isElement(child, "r") == false && isElement(child, "fld") == false && isEnd == false) continue;
			const pugi::xml_node style = isEnd == true ? child : childNamed(child, "rPr");
			if (isSized == false && style.attribute("sz").empty() == false) {
				size = static_cast<double>(numberOf(style, "sz", 1800)) / 100.0;
				isSized = true;
			}
			for (pugi::xml_node text = child.first_child(); text.empty() == false; text = text.next_sibling()) {
				if (isElement(text, "t") == true) words += text.child_value();
			}
		}
		const double points = size * scale;
		const pugi::xml_node layout = childNamed(paragraph, "pPr");
		const long long level = numberOf(layout, "lvl", 0);
		const double indent = static_cast<double>(numberOf(layout, "marL", level * 457200)) / kEmuPerPoint;
		double lineHeight = points * kLineHeight;
		const pugi::xml_node lineSpacing = childNamed(layout, "lnSpc");
		if (childNamed(lineSpacing, "spcPct").empty() == false) {
			lineHeight = points * kLineHeight * static_cast<double>(numberOf(childNamed(lineSpacing, "spcPct"), "val", 100000)) / 100000.0;
		} else if (childNamed(lineSpacing, "spcPts").empty() == false) {
			lineHeight = static_cast<double>(numberOf(childNamed(lineSpacing, "spcPts"), "val", 0)) / 100.0;
		}
		const double room = inner - indent;
		const double lines = wraps == true && room > 0.0
			? std::max(1.0, std::ceil(static_cast<double>(lettersIn(words)) * kLetterWidth * points / room))
			: 1.0;
		measure.height += lines * lineHeight + spacingOf(childNamed(layout, "spcBef"), points)
			+ spacingOf(childNamed(layout, "spcAft"), points);
		measure.lines += lines;
	}
	return measure;
}

struct Fit {
	std::string shapeId;
	double needed = 0.0;
	double room = 0.0;
	double lines = 0.0;
};

// Each text box of a slide whose words need more room than the slide gives
// them; the slide is `slideHeight` EMU high.
std::vector<Fit> overfullBoxesOf(const pugi::xml_document& slide, long long slideHeight) {
	std::vector<Fit> overfull;
	const pugi::xml_node tree = childNamed(childNamed(childNamed(slide, "sld"), "cSld"), "spTree");
	std::vector<std::pair<Box, pugi::xml_node>> shapes;
	for (pugi::xml_node shape = tree.first_child(); shape.empty() == false; shape = shape.next_sibling()) {
		if (isElement(shape, "sp") == false && isElement(shape, "pic") == false && isElement(shape, "graphicFrame") == false
				&& isElement(shape, "grpSp") == false && isElement(shape, "cxnSp") == false) continue;
		shapes.emplace_back(boxOf(shape), shape);
	}
	for (const auto& [box, shape] : shapes) {
		const pugi::xml_node body = childNamed(shape, "txBody");
		if (box.isKnown == false || body.empty() == true) continue;
		long long foot = slideHeight;
		for (const auto& [other, ignored] : shapes) {
			if (other.isKnown == false || other.id == box.id) continue;
			const bool sharesWidth = other.x < box.x + box.width && box.x < other.x + other.width;
			if (sharesWidth == true && other.y > box.y && other.y < foot) foot = other.y;
		}
		const double room = static_cast<double>(std::max(box.height, foot - box.y)) / kEmuPerPoint;
		const Measure measure = measureWords(body, static_cast<double>(box.width) / kEmuPerPoint);
		if (measure.height > room * kFitTolerance) overfull.push_back(Fit{box.id, measure.height, room, measure.lines});
	}
	return overfull;
}

long long slideHeightOf(const std::filesystem::path& path) {
	Archive archive(path, ZIP_RDONLY);
	if (archive.get() == nullptr) return 6858000;
	const std::optional<std::string> bytes = readEntry(archive.get(), "ppt/presentation.xml");
	pugi::xml_document presentation;
	if (bytes.has_value() == false || parseSlide(*bytes, presentation) == false) return 6858000;
	const pugi::xml_node size = childNamed(childNamed(presentation, "presentation"), "sldSz");
	return std::max<long long>(numberOf(size, "cy", 6858000), 1);
}

// The sign a deck's line begins with when the deck writes its bullets as
// letters ("• ", "– "), not as the paragraph's own bullet: kept when an edit
// rewrites the line, and given to each line it adds.
std::string leadingMark(const std::string& text) {
	const std::size_t first = text.find_first_not_of(" \t");
	if (first == std::string::npos) return std::string();
	for (const char* mark : {"\xE2\x80\xA2 ", "\xE2\x80\x93 ", "\xE2\x80\x94 ", "\xE2\x96\xAA ", "\xE2\x97\xA6 ", "- "}) {
		if (text.compare(first, std::strlen(mark), mark) == 0) return mark;
	}
	return std::string();
}

std::string withMark(const std::string& mark, const std::string& line) {
	if (mark.empty() == true || leadingMark(line).empty() == false) return line;
	return mark + line;
}

std::string withoutMark(const std::string& line) {
	const std::string mark = leadingMark(line);
	if (mark.empty() == true) return line;
	return trimmedWords(line.substr(line.find(mark) + mark.size()));
}

} // namespace

std::vector<std::string> slideLinesOf(const std::string& newText) {
	return linesOf(newText);
}

std::vector<SlideParagraphs> readSlideParagraphs(const std::filesystem::path& path) {

	std::vector<Slide> slides;
	std::string problem;
	std::vector<SlideParagraphs> read;
	if (loadSlides(path, slides, problem) == false) return read;
	for (Slide& slide : slides) {
		SlideParagraphs entry;
		entry.slideNumber = slide.number;
		for (const Paragraph& paragraph : paragraphsOf(slide.document)) {
			const std::string words = trimmedWords(paragraph.text);
			if (words.empty() == false) entry.paragraphs.push_back(words);
		}
		read.push_back(std::move(entry));
	}
	return read;

}

SlideRevision writeRevisedPresentation(const std::filesystem::path& source, const std::filesystem::path& target,
	const std::vector<SlideEdit>& edits) {

	SlideRevision revision;
	revision.path = target;
	std::error_code exists;
	if (std::filesystem::exists(target, exists) == true) {
		revision.problem = "A file of the revised deck's name is already there, so nothing was written over it.";
		return revision;
	}
	std::vector<Slide> slides;
	if (loadSlides(source, slides, revision.problem) == false) return revision;
	const std::vector<std::vector<std::string>> before = paragraphTexts(slides);
	for (Slide& slide : slides) slide.original = serialized(slide.document);

	for (const SlideEdit& edit : edits) {
		SlideEditOutcome outcome;
		outcome.slideNumber = edit.slideNumber;
		const std::string current = trimmedWords(onOneLine(edit.currentText));
		const std::vector<std::string> lines = linesOf(edit.newText);
		const std::string replacement = lines.size() == 1 ? lines.front() : trimmedWords(onOneLine(edit.newText));
		if (current.empty() == true) {
			outcome.state = "not_found";
			outcome.note = "The edit did not say which words of the deck it changes.";
			revision.outcomes.push_back(outcome);
			continue;
		}
		if (platform::phrases_equal(current, replacement) == true) {
			outcome.state = "unchanged";
			outcome.note = "The edit leaves the words as they are.";
			revision.outcomes.push_back(outcome);
			continue;
		}
		std::vector<Place> places = placesOf(slides, current, edit.slideNumber);
		bool isRelocated = false;
		if (places.empty() == true && edit.slideNumber != 0) {
			places = placesOf(slides, current, 0);
			isRelocated = places.size() == 1;
		}
		if (places.empty() == true) {
			outcome.state = "not_found";
			outcome.note = "These words are not in the deck: \"" + current + "\".";
			revision.outcomes.push_back(outcome);
			continue;
		}
		if (places.size() > 1) {
			outcome.state = "ambiguous";
			outcome.note = "These words stand in more than one place in the deck, so the edit could not be anchored: \""
				+ current + "\".";
			revision.outcomes.push_back(outcome);
			continue;
		}
		const Place place = places.front();
		Slide& slide = slides[place.slide];
		std::vector<Paragraph> paragraphs = paragraphsOf(slide.document);
		Paragraph& paragraph = paragraphs[place.paragraph];
		outcome.slideNumber = slide.number;
		outcome.before = trimmedWords(paragraph.text);
		if (isRelocated == true) {
			outcome.note = "The edit named slide " + std::to_string(edit.slideNumber) + "; its words stand on slide "
				+ std::to_string(slide.number) + ".";
		}
		const bool isWholeParagraph = place.start == paragraph.text.find_first_not_of(" \t")
			&& trimmedWords(paragraph.text).size() == place.length;
		// A deck that writes its bullets as letters keeps them: an edit that
		// rewrites the line from its start keeps its sign, and each line it adds
		// takes it.
		const std::string mark = leadingMark(paragraph.text);
		const bool coversMark = mark.empty() == false && place.start <= paragraph.text.find_first_not_of(" \t");
		if (lines.size() > 1 && reachesEnd(paragraph.text, place.start + place.length) == false) {
			// A line started inside a bullet would split it; the words after it
			// would move to a bullet the colleague never wrote.
			outcome.state = "mid_line";
			outcome.note = "These words do not reach the end of their line on the slide, so new lines cannot follow them: quote "
				"the line through its end, then give each new line on a line of its own.";
		} else if (lines.size() > 1) {
			// The bullet as it should read, then each new one after it, at its level.
			const std::string first = coversMark == true ? withMark(mark, lines.front()) : lines.front();
			const Narrowed narrowed = narrowedEdit(paragraph.text.substr(place.start, place.length), first);
			outcome.state = narrowed.isUnchanged == true ? std::string("written")
				: replaceInParagraph(paragraph, place.start + narrowed.start, narrowed.length, narrowed.words);
			if (outcome.state == "written") {
				pugi::xml_node after = paragraph.node;
				for (std::size_t line = 1; line < lines.size(); ++line) {
					after = addParagraphAfter(after, paragraph.node, withMark(mark, lines[line]));
				}
			}
		} else if (replacement.empty() == true && isWholeParagraph == true) {
			outcome.state = removeParagraph(paragraph) == true ? "written" : "not_found";
		} else {
			const Narrowed narrowed = narrowedEdit(paragraph.text.substr(place.start, place.length),
				coversMark == true ? withMark(mark, replacement) : replacement);
			outcome.state = narrowed.isUnchanged == true ? std::string("unchanged")
				: replaceInParagraph(paragraph, place.start + narrowed.start, narrowed.length, narrowed.words);
			if (outcome.state == "unchanged") outcome.note = "The edit leaves the words as they are.";
		}
		if (outcome.state == "line_break") {
			outcome.note = "These words run across a line break of the slide; change each line on its own.";
		} else if (outcome.state == "field") {
			outcome.state = "not_found";
			outcome.note = "These words are a field PowerPoint fills in itself, such as a slide number or a date.";
		}
		if (outcome.state == "written") {
			slide.isChanged = true;
			++revision.written;
		}
		revision.outcomes.push_back(outcome);
	}
	// A slide whose words no longer fit keeps none of its edits: the colleague is
	// told how many lines they need and how many the slide has room for, and
	// says it in fewer words. A slide already over-full keeps edits that do not
	// make it fuller.
	const long long slideHeight = slideHeightOf(source);
	for (Slide& slide : slides) {
		if (slide.isChanged == false) continue;
		const std::vector<Fit> overfull = overfullBoxesOf(slide.document, slideHeight);
		if (overfull.empty() == true) continue;
		pugi::xml_document original;
		if (parseSlide(slide.original, original) == false) continue;
		const std::vector<Fit> wasOverfull = overfullBoxesOf(original, slideHeight);
		const Fit* fuller = nullptr;
		for (const Fit& fit : overfull) {
			const auto was = std::find_if(wasOverfull.begin(), wasOverfull.end(),
				[&](const Fit& old) { return old.shapeId == fit.shapeId; });
			if (was == wasOverfull.end() || fit.needed > was->needed + 0.5) {
				fuller = &fit;
				break;
			}
		}
		if (fuller == nullptr) continue;
		const long long needed = std::llround(fuller->lines);
		const long long room = std::max<long long>(1,
			static_cast<long long>(std::floor(fuller->lines * fuller->room / std::max(fuller->needed, 1.0))));
		const std::string why = "Slide " + std::to_string(slide.number) + "'s words no longer fit it: they need about "
			+ std::to_string(needed) + " lines, and the slide has room for about " + std::to_string(room)
			+ " at this size. Say it in fewer words, or leave a point for another slide. Nothing on this slide was written.";
		for (SlideEditOutcome& outcome : revision.outcomes) {
			if (outcome.slideNumber != slide.number || outcome.state != "written") continue;
			outcome.state = "overfull";
			outcome.note = why;
			--revision.written;
		}
		slide.document.reset();
		(void)parseSlide(slide.original, slide.document);
		slide.isChanged = false;
	}
	if (revision.written == 0) {
		revision.problem = "None of the edits could be written into the deck.";
		for (const SlideEditOutcome& outcome : revision.outcomes) {
			if (outcome.state == "overfull") {
				revision.problem = outcome.note;
				break;
			}
		}
		return revision;
	}

	// What every paragraph of the copy must read: the deck as it was, with
	// exactly the edits written.
	const std::vector<std::vector<std::string>> expected = paragraphTexts(slides);

	// The copy, with only the changed slides replaced.
	std::error_code copied;
	std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, copied);
	if (copied) {
		revision.problem = "The revised deck could not be written beside the original.";
		return revision;
	}
	const auto discard = [&](const std::string& why) {
		std::error_code removed;
		std::filesystem::remove(target, removed);
		revision.isWritten = false;
		revision.problem = why;
		return revision;
	};
	{
		Archive archive(target, 0);
		if (archive.get() == nullptr) return discard("The revised deck could not be opened to write it.");
		for (const Slide& slide : slides) {
			if (slide.isChanged == false) continue;
			const std::string xml = serialized(slide.document);
			const zip_int64_t index = zip_name_locate(archive.get(), slide.part.c_str(), 0);
			if (index < 0) return discard("A slide of the revised deck could not be found to write it.");
			void* buffer = std::malloc(xml.size() == 0 ? 1 : xml.size());
			if (buffer == nullptr) return discard("There was not enough memory to write the revised deck.");
			std::memcpy(buffer, xml.data(), xml.size());
			zip_source_t* replacement = zip_source_buffer(archive.get(), buffer, xml.size(), 1);
			if (replacement == nullptr) {
				std::free(buffer);
				return discard("A slide of the revised deck could not be written.");
			}
			if (zip_file_replace(archive.get(), static_cast<zip_uint64_t>(index), replacement, 0) != 0) {
				zip_source_free(replacement);
				return discard("A slide of the revised deck could not be written.");
			}
		}
		if (archive.close() == false) return discard("The revised deck could not be saved.");
	}

	// Read back: every paragraph of every slide as meant, and the deck still
	// one the Matter reads.
	std::vector<Slide> written;
	std::string problem;
	if (loadSlides(target, written, problem) == false) return discard("The revised deck could not be read back: " + problem);
	if (paragraphTexts(written) != expected) {
		return discard("The revised deck did not read back exactly as the edits meant, so it was not kept.");
	}
	if (before.size() != expected.size()) return discard("The revised deck no longer has the original's slides.");
	documents::DocumentExtractionRequest request;
	request.path = target;
	if (documents::extract_pptx_document(request).status != documents::ExtractionStatus::ready) {
		return discard("The revised deck could not be read as a presentation, so it was not kept.");
	}
	// Each written edit's paragraph, as the copy now reads it.
	{
		std::vector<std::vector<std::string>> readBack = paragraphTexts(written);
		std::size_t outcomeIndex = 0;
		for (SlideEditOutcome& outcome : revision.outcomes) {
			++outcomeIndex;
			if (outcome.state != "written") continue;
			const std::size_t slideIndex = outcome.slideNumber == 0 ? 0 : outcome.slideNumber - 1;
			if (slideIndex >= readBack.size()) continue;
			const std::vector<std::string> lines = linesOf(edits[outcomeIndex - 1].newText);
			// The paragraph that now holds the new words, and each bullet added
			// after it, one per line; or none where the edit took a paragraph out.
			outcome.after.clear();
			const std::vector<std::string>& paragraphs = readBack[slideIndex];
			for (std::size_t at = 0; lines.empty() == false && at < paragraphs.size(); ++at) {
				if (platform::locate_phrase(paragraphs[at], lines.front()).matches == 0) continue;
				bool isEach = at + lines.size() <= paragraphs.size();
				for (std::size_t line = 1; isEach == true && line < lines.size(); ++line) {
					isEach = platform::phrases_equal(withoutMark(trimmedWords(paragraphs[at + line])), lines[line]);
				}
				if (isEach == false) continue;
				for (std::size_t line = 0; line < lines.size(); ++line) {
					outcome.after += (line == 0 ? std::string() : std::string("\n")) + trimmedWords(paragraphs[at + line]);
				}
				break;
			}
		}
	}
	revision.isWritten = true;
	revision.sha256 = platform::sha256_hex_digest_of_file(target);
	return revision;

}

} // namespace singularity::document_intelligence
