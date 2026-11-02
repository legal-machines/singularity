#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::platform {

/*
 * Finding the passage a quote means when its words are not the document's own.
 *
 * A model never sees a document's bytes when Papinian Guard protects it: it
 * sees protected text, and what it quotes back is restored - a party named in
 * full ("Summit Staffing Group, Inc.") where the document uses its defined term
 * ("Summit"), a figure written one way where the document writes it another
 * ("$250,000" where it says "two hundred fifty thousand dollars ($250,000)").
 * Word for word, such a quote is not in the document; word by word, in order,
 * it is.
 *
 * align_words() finds the run of the document's words that a quote covers
 * best, in order. It is taken only when nearly every word of that run is in
 * the quote (`of_span`), most of the quote is in it (`of_quote`), at least four
 * words agree, and no other run is as good - so a quote that shares a few
 * words with a passage never lands on it, and one that could mean two passages
 * means neither. The answer is where the run stands in `text`: its own words.
 */

struct AlignedWord {
	std::string text;
	std::size_t start = 0;
	std::size_t end = 0;
};

// Letters and digits, lower case; a figure written with separators is one word
// ("2,000,000" is "2000000"); bytes beyond ASCII are kept as they are.
inline std::vector<AlignedWord> aligned_words(const std::string &text) {
	std::vector<AlignedWord> words;
	std::size_t index = 0;
	while (index < text.size()) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (!(std::isalnum(byte) || byte >= 0x80)) {
			++index;
			continue;
		}
		AlignedWord word;
		word.start = index;
		const bool digits = std::isdigit(byte) != 0;
		while (index < text.size()) {
			const unsigned char next = static_cast<unsigned char>(text[index]);
			if (std::isalnum(next) || next >= 0x80) {
				word.text += static_cast<char>(next >= 0x80 ? next : std::tolower(next));
				++index;
			} else if (digits && (next == ',' || next == '.') && index + 1 < text.size()
					&& std::isdigit(static_cast<unsigned char>(text[index + 1]))) {
				++index;
			} else {
				break;
			}
		}
		word.end = index;
		words.push_back(std::move(word));
	}
	return words;
}

// A figure a document writes out in words ("two hundred fifty thousand
// dollars") is one the restored quote writes in digits: such words never count
// against a run.
inline bool figure_word(const std::string &word) {
	static const char *const words[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
		"eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
		"thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "thousand", "million", "billion",
		"dollars", "dollar", "usd", "percent", "and"};
	for (const char *figure : words)
		if (word == figure) return true;
	return false;
}

// `whole_quote`: the run must end where the quote ends - as an anchor a change
// replaces must, so no word of the passage is left behind it.
[[nodiscard]] inline std::optional<std::pair<std::size_t, std::size_t>> align_words(const std::string &text,
		const std::string &quote, double of_span_needed = 0.85, double of_quote_needed = 0.6, bool whole_quote = false) {
	const std::vector<AlignedWord> quoted = aligned_words(quote);
	const std::vector<AlignedWord> document = aligned_words(text);
	if (quoted.size() < 4 || document.empty()) return std::nullopt;
	double best = 0.0;
	std::size_t bestFirst = 0;
	std::size_t bestLast = 0;
	std::size_t bestFrom = 0;
	bool ambiguous = false;
	for (std::size_t first = 0; first < document.size(); ++first) {
		// The run may begin at any of the quote's first words: a name restored in
		// full can stand where the document has its defined term.
		for (std::size_t from = 0; from < std::min<std::size_t>(8, quoted.size()); ++from) {
			if (document[first].text != quoted[from].text) continue;
			std::size_t matched = 0;
			std::size_t at = first;
			const std::size_t limit = std::min(document.size(), first + quoted.size() * 2);
			for (std::size_t index = from; index < quoted.size(); ++index) {
				std::size_t look = at;
				while (look < limit && look < at + 6 && document[look].text != quoted[index].text) ++look;
				if (look >= limit || look >= at + 6) continue;
				++matched;
				at = look + 1;
				if (whole_quote && index + 1 != quoted.size()) continue;
				// Every place the run could end is weighed: a figure the document
				// writes out in words ends the run best before it, unless the run
				// must reach the quote's end - then those words do not count.
				std::size_t span = look - first + 1;
				if (whole_quote) {
					std::size_t figures = 0;
					for (std::size_t word = first; word <= look; ++word) figures += figure_word(document[word].text) ? 1 : 0;
					span = std::max(matched, span - figures);
				}
				const double ofSpan = static_cast<double>(matched) / static_cast<double>(span);
				const double ofQuote = static_cast<double>(matched) / static_cast<double>(quoted.size());
				if (matched < 4 || ofSpan < of_span_needed || ofQuote < of_quote_needed) continue;
				const double score = ofSpan + ofQuote;
				if (score > best + 1e-9) {
					ambiguous = false;
					best = score;
					bestFirst = first;
					bestLast = look;
					bestFrom = from;
				} else if (score > best - 1e-9 && first != bestFirst) {
					ambiguous = true;
				}
			}
		}
	}
	if (best == 0.0 || ambiguous) return std::nullopt;
	std::size_t start = document[bestFirst].start;
	// A name the quote wrote in full stands, in the document, as the word just
	// before the run - its defined term ("Lessor's" for "Riverside Imaging
	// Finance LLC's") - when nothing ends a sentence between them.
	if (bestFrom > 0 && bestFirst > 0) {
		const std::size_t previous = bestFirst - 1;
		const std::string between = text.substr(document[previous].end, start - document[previous].end);
		if (between.find_first_of(".;:\n") == std::string::npos) start = document[previous].start;
	}
	std::size_t end = document[bestLast].end;
	if (whole_quote) {
		// An anchor ends where its passage ends: the brackets and quotation marks
		// that close it, and the stop the quote itself ends with.
		const auto closes = [&](std::size_t at) -> std::size_t {
			if (at >= text.size()) return 0;
			if (text[at] == ')' || text[at] == ']' || text[at] == '"' || text[at] == '\'') return 1;
			if (text.compare(at, 3, "\xE2\x80\x9D") == 0 || text.compare(at, 3, "\xE2\x80\x99") == 0) return 3;
			return 0;
		};
		for (std::size_t width = closes(end); width > 0; width = closes(end)) end += width;
		const std::size_t lastMark = quote.find_last_not_of(" \t\r\n");
		if (lastMark != std::string::npos && std::string(".;:,").find(quote[lastMark]) != std::string::npos
				&& end < text.size() && text[end] == quote[lastMark])
			++end;
	}
	return std::make_pair(start, end - start);
}

// A figure a document states twice - in words and again in brackets, "two
// hundred fifty thousand dollars ($250,000)" - is one figure: a change that
// replaces the words replaces the brackets after them too, or the document
// would read "...the fees paid in the prior twelve months ($250,000)." When
// `anchor` - the document's own words, found once in `text` - ends on a figure
// and the brackets restating it follow, the anchor takes them in, and the stop
// after them when `replacement` already ends with that stop.
[[nodiscard]] inline std::string with_restated_figure(const std::string &text, const std::string &anchor,
		const std::string &replacement) {
	if (anchor.empty()) return anchor;
	const std::size_t at = text.find(anchor);
	if (at == std::string::npos || text.find(anchor, at + 1) != std::string::npos) return anchor;
	const std::vector<AlignedWord> words = aligned_words(anchor);
	if (words.empty()) return anchor;
	const std::string &last = words.back().text;
	const bool figure = figure_word(last) || std::isdigit(static_cast<unsigned char>(last.back())) != 0;
	if (!figure) return anchor;
	std::size_t end = at + anchor.size();
	std::size_t look = end;
	while (look < text.size() && (text[look] == ' ' || text[look] == '\t')) ++look;
	if (look >= text.size() || text[look] != '(') return anchor;
	const std::size_t close = text.find(')', look);
	if (close == std::string::npos || close - look > 40) return anchor;
	const std::string inside = text.substr(look + 1, close - look - 1);
	if (inside.find_first_of("0123456789") == std::string::npos || inside.find('(') != std::string::npos) return anchor;
	for (const char character : inside)
		if (std::isalpha(static_cast<unsigned char>(character)) != 0 && std::string("USDEURGBPusdeurgbpmilonthsad").find(character) == std::string::npos)
			return anchor;
	end = close + 1;
	const std::size_t stop = replacement.find_last_not_of(" \t\r\n");
	if (stop != std::string::npos && std::string(".;,").find(replacement[stop]) != std::string::npos && end < text.size()
			&& text[end] == replacement[stop])
		++end;
	return text.substr(at, end - at);
}

} // namespace singularity::platform
