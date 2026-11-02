#include "Context/QuoteVerification/QuoteVerification.hpp"

#include "Platform/PhraseMatch/PhraseMatch.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

namespace singularity::context {
namespace {

// The quotation marks legal writing actually uses, as the bytes they are.
struct QuotePair {
	std::string opening;
	std::string closing;
};

const std::vector<QuotePair> &quote_pairs() {
	static const std::vector<QuotePair> pairs{
		{"\"", "\""},
		{"\xe2\x80\x9c", "\xe2\x80\x9d"}, // “ ”
		{"\xc2\xab", "\xc2\xbb"},         // « »
	};
	return pairs;
}

std::size_t word_count(const std::string &text) {
	std::size_t words = 0;
	bool inside = false;
	for (const char character : text) {
		const bool space = character == ' ' || character == '\t' || character == '\n' || character == '\r';
		if (!space && !inside) { ++words; inside = true; }
		else if (space) inside = false;
	}
	return words;
}

// The quotation's own words, with an ellipsis read as a gap between them.
std::vector<std::string> quotation_parts(const std::string &quote) {
	static const std::vector<std::string> ellipses{"\xe2\x80\xa6", "..."};
	std::vector<std::string> parts{quote};
	for (const std::string &ellipsis : ellipses) {
		std::vector<std::string> split;
		for (const std::string &part : parts) {
			std::size_t from = 0;
			for (std::size_t at = part.find(ellipsis); at != std::string::npos;
					at = part.find(ellipsis, from)) {
				split.push_back(part.substr(from, at - from));
				from = at + ellipsis.size();
			}
			split.push_back(part.substr(from));
		}
		parts = std::move(split);
	}
	std::vector<std::string> kept;
	for (std::string &part : parts) {
		const auto first = part.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) continue;
		const auto last = part.find_last_not_of(" \t\r\n");
		kept.push_back(part.substr(first, last - first + 1));
	}
	return kept;
}

// Every citation marker standing in `paragraph`, as the tokens it names.
// "[SOURCE:DOC_<n>:CHUNK_<n>]" exactly - the shape SourceMarkerRegistry writes
// and the only one a citation is ever read from.
std::vector<std::pair<std::string, std::string>> markers_in(const std::string &paragraph) {
	static const std::string opening = "[SOURCE:DOC_";
	static const std::string middle = ":CHUNK_";
	std::vector<std::pair<std::string, std::string>> tokens;
	for (std::size_t at = paragraph.find(opening); at != std::string::npos;
			at = paragraph.find(opening, at + 1)) {
		std::size_t cursor = at + opening.size();
		const std::size_t documentStart = cursor;
		while (cursor < paragraph.size() && std::isdigit(static_cast<unsigned char>(paragraph[cursor])) != 0) ++cursor;
		if (cursor == documentStart) continue;
		const std::string document = paragraph.substr(documentStart, cursor - documentStart);
		if (paragraph.compare(cursor, middle.size(), middle) != 0) continue;
		cursor += middle.size();
		const std::size_t chunkStart = cursor;
		while (cursor < paragraph.size() && std::isdigit(static_cast<unsigned char>(paragraph[cursor])) != 0) ++cursor;
		if (cursor == chunkStart || cursor >= paragraph.size() || paragraph[cursor] != ']') continue;
		tokens.emplace_back("DOC_" + document, "CHUNK_" + paragraph.substr(chunkStart, cursor - chunkStart));
	}
	return tokens;
}

} // namespace

std::vector<std::string> quotations_in(const std::string &answer) {
	std::vector<std::string> quotations;
	for (std::size_t at = 0; at < answer.size();) {
		const QuotePair *opened = nullptr;
		for (const QuotePair &pair : quote_pairs())
			if (answer.compare(at, pair.opening.size(), pair.opening) == 0) { opened = &pair; break; }
		if (opened == nullptr) { ++at; continue; }
		const std::size_t contentStart = at + opened->opening.size();
		const std::size_t closing = answer.find(opened->closing, contentStart);
		if (closing == std::string::npos) { ++at; continue; }
		std::string quote = answer.substr(contentStart, closing - contentStart);
		// A phrase, not a word in inverted commas and not a defined term.
		if (word_count(quote) >= 4 && quote.find('\n') == std::string::npos) quotations.push_back(std::move(quote));
		at = closing + opened->closing.size();
	}
	return quotations;
}

std::string quote_documents_exactly(const std::string &answer,
		const std::vector<QuotedPassage> &checked) {
	std::string repaired = answer;
	for (const QuotedPassage &passage : checked) {
		if (!passage.verified || !passage.repaired || passage.exact.empty()) continue;
		if (passage.quote.empty()) continue;
		// Replaced where the answer quoted it, never anywhere else: the
		// quotation stands between quotation marks, which is how it was read.
		std::size_t at = 0;
		while ((at = repaired.find(passage.quote, at)) != std::string::npos) {
			const bool opensAQuotation = at > 0 && [&] {
				for (const QuotePair &pair : quote_pairs())
					if (at >= pair.opening.size()
							&& repaired.compare(at - pair.opening.size(), pair.opening.size(), pair.opening) == 0)
						return true;
				return false;
			}();
			if (opensAQuotation) {
				repaired.replace(at, passage.quote.size(), passage.exact);
				at += passage.exact.size();
				continue;
			}
			at += passage.quote.size();
		}
	}
	return repaired;
}

// The mark a quotation ends with belongs to the sentence that carries it. A
// document heads a list with "the following additional conditions:" and an
// answer quoting it closes its own sentence with a full stop; the words are the
// document's, and reporting the firm's own quotation as not found in its own
// document is worse than useless - a lawyer then re-checks a quotation that was
// right. Only a final run of sentence punctuation is ever dropped, and only
// after the quotation has failed to match as written.
std::string without_trailing_sentence_punctuation(const std::string &part) {
	std::size_t end = part.size();
	while (end > 0) {
		const char character = part[end - 1];
		if (character == '.' || character == ',' || character == ';' || character == ':'
				|| character == '!' || character == '?' || platform::phrase_match_detail::is_space(character)) {
			--end;
			continue;
		}
		break;
	}
	return end == part.size() ? part : part.substr(0, end);
}

std::vector<QuotedPassage> verify_quotations(const std::string &answer,
		const std::vector<ResolvedSourceReference> &cited,
		const std::function<std::optional<std::string>(const ResolvedSourceReference &)> &passage_text,
		const std::vector<ResolvedSourceReference> &supplied) {
	std::vector<QuotedPassage> checked;
	if (cited.empty()) return checked;
	// Read each cited passage at most once.
	std::map<std::string, std::optional<std::string>> textByMarker;
	const auto textOf = [&](const ResolvedSourceReference &reference) -> const std::optional<std::string> & {
		const std::string key = reference.marker();
		const auto found = textByMarker.find(key);
		if (found != textByMarker.end()) return found->second;
		return textByMarker.emplace(key, passage_text(reference)).first->second;
	};

	// Paragraph by paragraph, so a quotation is checked against what the answer
	// cites beside it rather than against everything it ever cited.
	std::size_t paragraphStart = 0;
	while (paragraphStart <= answer.size()) {
		std::size_t paragraphEnd = answer.find("\n\n", paragraphStart);
		if (paragraphEnd == std::string::npos) paragraphEnd = answer.size();
		const std::string paragraph = answer.substr(paragraphStart, paragraphEnd - paragraphStart);
		const auto markers = markers_in(paragraph);
		std::vector<const ResolvedSourceReference *> nearby;
		for (const auto &[documentToken, chunkToken] : markers)
			for (const ResolvedSourceReference &reference : cited)
				if (reference.document_token == documentToken && reference.chunk_token == chunkToken)
					nearby.push_back(&reference);
		const bool attributed = !nearby.empty();
		if (nearby.empty())
			for (const ResolvedSourceReference &reference : cited) nearby.push_back(&reference);

		for (const std::string &quote : quotations_in(paragraph)) {
			QuotedPassage result;
			result.quote = quote;
			result.attributed = attributed;
			const std::vector<std::string> parts = quotation_parts(quote);
			// Where the words are looked for: what the paragraph cites, then every
			// other passage this operation supplied.
			std::vector<const ResolvedSourceReference *> candidates = nearby;
			const std::size_t citedCount = candidates.size();
			for (const ResolvedSourceReference &reference : supplied)
				if (std::none_of(candidates.begin(), candidates.end(),
						[&](const ResolvedSourceReference *known) { return known->chunk_id == reference.chunk_id; }))
					candidates.push_back(&reference);
			for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
				const ResolvedSourceReference *reference = candidates[candidateIndex];
				const auto &text = textOf(*reference);
				if (!text) continue;
				std::size_t from = 0;
				std::size_t firstStart = std::string::npos;
				std::size_t lastEnd = 0;
				bool allFound = !parts.empty();
				for (std::size_t index = 0; index < parts.size(); ++index) {
					const std::string &part = parts[index];
					auto at = platform::find_phrase(*text, part, from);
					if (!at && index + 1 == parts.size()) {
						const std::string trimmed = without_trailing_sentence_punctuation(part);
						if (trimmed.size() != part.size() && word_count(trimmed) > 0)
							at = platform::find_phrase(*text, trimmed, from);
					}
					if (!at) { allFound = false; break; }
					if (firstStart == std::string::npos) firstStart = at->first;
					lastEnd = at->first + at->second;
					from = lastEnd;
				}
				if (!allFound) continue;
				result.document_id = reference->document_id;
				result.chunk_id = reference->chunk_id;
				result.document_name = reference->document_name;
				result.verified = true;
				result.start = firstStart;
				result.length = lastEnd - firstStart;
				result.exact = text->substr(firstStart, lastEnd - firstStart);
				result.repaired = result.exact != quote;
				result.rebound = candidateIndex >= citedCount;
				break;
			}
			if (!result.verified && nearby.size() == 1) {
				// Unverified, and honest about what it was checked against. Only
				// when the paragraph cites one passage: naming the first of
				// several tells a lawyer a quotation was not found in a document
				// it was never said to come from, which reads as a worse fault
				// than the one there is.
				result.document_id = nearby.front()->document_id;
				result.chunk_id = nearby.front()->chunk_id;
				result.document_name = nearby.front()->document_name;
			}
			checked.push_back(std::move(result));
		}
		if (paragraphEnd == answer.size()) break;
		paragraphStart = paragraphEnd + 2;
	}
	return checked;
}

} // namespace singularity::context
