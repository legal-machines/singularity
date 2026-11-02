#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace singularity::agents {

// One paragraph of a document as an extractor read it: its words and where a
// person finds them ("Paragraph 12", "Page 3").
struct TextBlock {
	std::string text;
	std::string location;
};

// One place where a later version of a document differs from an earlier one:
// the paragraphs that left and the paragraphs that came, in order, with the
// unchanged paragraph just before them so a reader knows where it is.
struct TextChange {
	std::vector<TextBlock> before;
	std::vector<TextBlock> after;
	std::vector<TextBlock> context;
	// More paragraphs changed here than a report quotes; they are counted, not
	// silently dropped.
	std::size_t omitted_before = 0;
	std::size_t omitted_after = 0;
};

struct TextDiffLimits {
	// A change quotes at most this many paragraphs on each side.
	std::size_t paragraphs_per_change = 12;
	// At most this many changes are listed for one document.
	std::size_t changes_per_document = 40;
	// Past this many edits the two versions are too far apart to align
	// paragraph by paragraph; the document is then said to be rewritten.
	std::size_t edit_ceiling = 4000;
};

struct TextDiffResult {
	std::vector<TextChange> changes;
	std::size_t omitted_changes = 0;
	bool is_rewritten = false;
};

// Paragraph-level comparison of two versions of one document's text. Two
// paragraphs are the same when their words are, whatever the spacing between
// them; nothing else is judged here.
TextDiffResult diff_text_blocks(const std::vector<TextBlock> &before, const std::vector<TextBlock> &after,
	const TextDiffLimits &limits = TextDiffLimits());

// The words of a paragraph as they are compared: runs of whitespace become one
// space, and the ends are trimmed.
std::string normalize_block_text(const std::string &text);

} // namespace singularity::agents
