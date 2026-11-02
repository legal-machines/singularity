#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace singularity::document_intelligence {

// One passage of a deck as it should read: the words a slide holds now, on the
// slide they stand on (0 when the colleague did not say), and the words they
// become. A passage is a run of words inside one paragraph of a slide - a
// title, a bullet, a text box, a cell of a slide's table. New words of more
// than one line add a paragraph for each line after the first, after the
// passage's paragraph and at its level - its bullet, indent and formatting -
// so a passage that adds lines must reach the end of its paragraph.
struct SlideEdit {
	unsigned int slideNumber = 0;
	std::string currentText;
	std::string newText;
	// Why, in the colleague's words, for the lawyer.
	std::string explanation;
};

struct SlideEditOutcome {
	// "written"; or why not: "not_found", "ambiguous", "unchanged",
	// "line_break" (the words run across a line break of the slide),
	// "mid_line" (new lines given after words that do not end their paragraph).
	std::string state;
	unsigned int slideNumber = 0;
	// The paragraph as the deck read before, and as the copy reads it now -
	// read back from the copy, never assumed; each paragraph an edit added
	// follows on a line of its own.
	std::string before;
	std::string after;
	std::string note;
};

struct SlideRevision {
	bool isWritten = false;
	std::string problem;
	std::filesystem::path path;
	std::string sha256;
	std::size_t written = 0;
	std::vector<SlideEditOutcome> outcomes;
};

// A revised deck: a copy of `source` at `target` with each edit written where
// its words stand once in the deck (on its slide, when the edit names one),
// in the formatting of the words it replaces - their run's font, size, colour
// and emphasis - so the slide's layout, theme and design stay the deck's own.
// Words are found as a lawyer reads them: typography, spacing and case never
// decide, the words do (Platform/PhraseMatch). An edit that cannot be anchored
// once is refused, said why, and never takes the others with it.
//
// The copy is then read back - by this writer's own reading of every paragraph,
// and by the extractor that reads decks for the Matter - and kept only if each
// written paragraph reads as meant and every other paragraph of every slide
// reads exactly as it did. The source is never opened for writing; `target`
// must not exist; nothing is left at `target` unless at least one edit was
// written and proven.
SlideRevision writeRevisedPresentation(const std::filesystem::path& source, const std::filesystem::path& target,
	const std::vector<SlideEdit>& edits);

// The lines an edit's new words write, as writeRevisedPresentation reads them:
// each a paragraph of the slide, without the dash that marks a list's line.
std::vector<std::string> slideLinesOf(const std::string& newText);

// Every paragraph of every slide as this writer reads it, slide by slide in
// presentation order: what a colleague is shown and quotes back.
struct SlideParagraphs {
	unsigned int slideNumber = 0;
	std::vector<std::string> paragraphs;
};
std::vector<SlideParagraphs> readSlideParagraphs(const std::filesystem::path& path);

} // namespace singularity::document_intelligence
