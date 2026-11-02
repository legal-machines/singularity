#include "DocumentIntelligence/ChangeAccount/ChangeAccount.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>

namespace singularity::document_intelligence {
namespace {

constexpr std::size_t kMostWords = 1500;
// Unchanged words between two edits that still read as one place.
constexpr std::size_t kJoinedAcross = 2;
constexpr std::size_t kMostPhraseBytes = 120;
constexpr std::size_t kMostSaid = 3;
// How a struck clause reads in a draft (RevisionWriter's omitted clause).
const std::string kStruck = "[Intentionally omitted.]";

bool isWordByte(unsigned char byte) {
	return std::isalnum(byte) != 0 || byte >= 0x80 || byte == '\'' || byte == '-' || byte == '_';
}

bool hasLetterOrDigit(const std::string& token) {
	return std::any_of(token.begin(), token.end(), [](unsigned char byte) { return std::isalnum(byte) != 0 || byte >= 0x80; });
}

std::string lowered(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char byte) {
		return static_cast<char>(std::tolower(byte));
	});
	return text;
}

// Words and marks of punctuation, in order; spaces only part them.
std::vector<std::string> tokensOf(const std::string& text) {
	std::vector<std::string> tokens;
	std::size_t at = 0;
	while (at < text.size()) {
		const unsigned char byte = static_cast<unsigned char>(text[at]);
		if (std::isspace(byte) != 0) {
			++at;
			continue;
		}
		if (isWordByte(byte) == true) {
			std::size_t end = at;
			while (end < text.size() && isWordByte(static_cast<unsigned char>(text[end])) == true) ++end;
			tokens.push_back(text.substr(at, end - at));
			at = end;
			continue;
		}
		tokens.push_back(text.substr(at, 1));
		++at;
	}
	return tokens;
}

// A word that names its edit by itself: a long word, a figure of two digits or
// more, or a number said in words ("two" for "two (2) years").
bool isDistinctive(const std::string& word) {
	static const std::set<std::string> numbers = {"one", "two", "six", "ten"};
	if (hasLetterOrDigit(word) == false) return false;
	if (word.size() >= 4 || numbers.count(word) > 0) return true;
	return word.size() >= 2 && std::all_of(word.begin(), word.end(), [](unsigned char byte) { return std::isdigit(byte) != 0; });
}

// Words joined as they are read: no space before a closing mark, none after an
// opening one, and no comma or semicolon hanging at either end. A phrase too
// long to quote whole ends at a word.
std::string phraseOf(const std::vector<std::string>& tokens) {
	const auto isHanging = [](const std::string& token) {
		return token.size() == 1 && std::string(",;:").find(token[0]) != std::string::npos;
	};
	std::size_t first = 0;
	std::size_t last = tokens.size();
	while (first < last && isHanging(tokens[first]) == true) ++first;
	while (last > first && isHanging(tokens[last - 1]) == true) --last;
	std::string phrase;
	for (std::size_t index = first; index < last; ++index) {
		const std::string& token = tokens[index];
		const bool closes = token.size() == 1 && std::string(",.;:)!?]").find(token[0]) != std::string::npos;
		const bool afterOpening = phrase.empty() == false && std::string("([").find(phrase.back()) != std::string::npos;
		if (phrase.empty() == false && closes == false && afterOpening == false) phrase.push_back(' ');
		phrase += token;
	}
	if (phrase.size() <= kMostPhraseBytes) return phrase;
	std::size_t end = phrase.rfind(' ', kMostPhraseBytes);
	if (end == std::string::npos || end < kMostPhraseBytes / 2) {
		end = kMostPhraseBytes;
		while (end > 0 && (static_cast<unsigned char>(phrase[end]) & 0xC0) == 0x80) --end;
	}
	while (end > 0 && std::string(",;: ").find(phrase[end - 1]) != std::string::npos) --end;
	return phrase.substr(0, end) + "…";
}

enum class Step { Same, Out, In };

// One edit in words, as a lawyer reads it in a sentence.
std::string describedEdit(const WordEdit& edit) {
	if (edit.added == kStruck) return "strikes the whole clause, which read “" + edit.removed + "”";
	if (edit.added.empty() == true) return "takes out “" + edit.removed + "”";
	if (edit.removed.empty() == true) return "adds “" + edit.added + "”";
	return "replaces “" + edit.removed + "” with “" + edit.added + "”";
}

// Text on one line, its runs of spaces as one.
std::string spaced(const std::string& text) {
	std::string line;
	for (const char character : text) {
		if (std::isspace(static_cast<unsigned char>(character)) != 0) {
			if (line.empty() == false && line.back() != ' ') line.push_back(' ');
			continue;
		}
		line.push_back(character);
	}
	while (line.empty() == false && line.back() == ' ') line.pop_back();
	return line;
}

// A clause's words to quote: whole, or up to a word near the most a sentence
// carries.
std::string excerpt(const std::string& text) {
	const std::string line = spaced(text);
	constexpr std::size_t kMostQuoted = 160;
	if (line.size() <= kMostQuoted) return line;
	std::size_t end = line.rfind(' ', kMostQuoted);
	if (end == std::string::npos || end < kMostQuoted / 2) {
		end = kMostQuoted;
		while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) --end;
	}
	return line.substr(0, end) + "…";
}

// The letters and figures of a text alone: what tells words that changed from
// marks that moved.
std::string lettersOf(const std::string& text) {
	std::string letters;
	for (const char character : text) {
		if (std::isalnum(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80) {
			letters.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
		}
	}
	return letters;
}

} // namespace

std::vector<WordEdit> wordEdits(const std::string& before, const std::string& after) {
	const std::vector<std::string> a = tokensOf(before);
	const std::vector<std::string> b = tokensOf(after);
	if (a.size() > kMostWords || b.size() > kMostWords) return {};
	// The run the two share that keeps the most of their words, a word weighing
	// by its length and a mark by one: "negotiating" is kept before "and", as
	// the redline keeps it (ReviewResult).
	const auto weight = [](const std::string& token) {
		return hasLetterOrDigit(token) == true ? static_cast<std::uint32_t>(2 + token.size()) : std::uint32_t{1};
	};
	std::vector<std::vector<std::uint32_t>> common(a.size() + 1, std::vector<std::uint32_t>(b.size() + 1, 0));
	for (std::size_t i = a.size(); i-- > 0;) {
		for (std::size_t j = b.size(); j-- > 0;) {
			common[i][j] = a[i] == b[j] ? common[i + 1][j + 1] + weight(a[i]) : std::max(common[i + 1][j], common[i][j + 1]);
		}
	}
	std::vector<std::pair<Step, std::string>> steps;
	std::size_t i = 0;
	std::size_t j = 0;
	while (i < a.size() || j < b.size()) {
		if (i < a.size() && j < b.size() && a[i] == b[j]) {
			steps.emplace_back(Step::Same, a[i]);
			++i;
			++j;
		} else if (j < b.size() && (i == a.size() || common[i][j + 1] >= common[i + 1][j])) {
			steps.emplace_back(Step::In, b[j]);
			++j;
		} else {
			steps.emplace_back(Step::Out, a[i]);
			++i;
		}
	}

	std::vector<WordEdit> edits;
	std::size_t index = 0;
	while (index < steps.size()) {
		if (steps[index].first == Step::Same) {
			++index;
			continue;
		}
		// One place: edits, and the few unchanged words between edits near
		// enough to read with them.
		std::vector<std::string> out;
		std::vector<std::string> in;
		std::size_t end = index;
		while (end < steps.size()) {
			if (steps[end].first == Step::Same) {
				std::size_t run = end;
				while (run < steps.size() && steps[run].first == Step::Same) ++run;
				if (run == steps.size() || run - end > kJoinedAcross) break;
				for (std::size_t same = end; same < run; ++same) {
					out.push_back(steps[same].second);
					in.push_back(steps[same].second);
				}
				end = run;
				continue;
			}
			(steps[end].first == Step::Out ? out : in).push_back(steps[end].second);
			++end;
		}
		// A bracket the edit opened and the unchanged text closes is read with
		// it: "five (5)" for "thirty (30)", never "five (5".
		const auto opens = [](const std::vector<std::string>& tokens) {
			int depth = 0;
			for (const std::string& token : tokens) {
				if (token == "(") ++depth;
				if (token == ")" && depth > 0) --depth;
			}
			return depth;
		};
		while (end < steps.size() && steps[end].first == Step::Same && steps[end].second == ")" && (opens(out) > 0 || opens(in) > 0)) {
			out.push_back(steps[end].second);
			in.push_back(steps[end].second);
			++end;
		}
		// A replacement that begins or ends on a comma reads with the unchanged
		// word beside it, so the comma is said: “evaluating and negotiating”
		// becomes “evaluating, negotiating and consummating”, never “and
		// negotiating” becomes “negotiating and consummating”. Words only taken
		// out or only put in are quoted without the commas round them.
		const auto isHangingMark = [](const std::string& token) {
			return token.size() == 1 && std::string(",;:").find(token[0]) != std::string::npos;
		};
		const bool isReplacement = std::any_of(out.begin(), out.end(), hasLetterOrDigit)
			&& std::any_of(in.begin(), in.end(), hasLetterOrDigit);
		if (isReplacement == true && (isHangingMark(out.front()) == true || isHangingMark(in.front()) == true)
				&& index > 0 && steps[index - 1].first == Step::Same && hasLetterOrDigit(steps[index - 1].second) == true) {
			out.insert(out.begin(), steps[index - 1].second);
			in.insert(in.begin(), steps[index - 1].second);
		}
		if (isReplacement == true && (isHangingMark(out.back()) == true || isHangingMark(in.back()) == true)
				&& end < steps.size() && steps[end].first == Step::Same && hasLetterOrDigit(steps[end].second) == true) {
			out.push_back(steps[end].second);
			in.push_back(steps[end].second);
		}
		WordEdit edit;
		edit.removed = phraseOf(out);
		edit.added = phraseOf(in);
		std::set<std::string> outWords;
		std::set<std::string> inWords;
		for (const std::string& token : out) outWords.insert(lowered(token));
		for (const std::string& token : in) inWords.insert(lowered(token));
		for (const std::string& word : outWords) {
			if (isDistinctive(word) == true && inWords.count(word) == 0) edit.distinctive.push_back(word);
		}
		for (const std::string& word : inWords) {
			if (isDistinctive(word) == true && outWords.count(word) == 0) edit.distinctive.push_back(word);
		}
		// Punctuation moved and nothing else is no edit a lawyer reads.
		const bool hasWords = std::any_of(out.begin(), out.end(), hasLetterOrDigit)
			|| std::any_of(in.begin(), in.end(), hasLetterOrDigit);
		if (hasWords == true) edits.push_back(edit);
		index = end;
	}
	return edits;
}

std::string unsaidEdits(const std::string& explanation, const std::vector<WordEdit>& edits) {
	// The explanation's words; a hyphenated one says its parts too ("thirty-day"
	// says "thirty").
	std::set<std::string> said;
	for (const std::string& token : tokensOf(lowered(explanation))) {
		if (hasLetterOrDigit(token) == false) continue;
		said.insert(token);
		std::size_t start = 0;
		while (start <= token.size()) {
			const std::size_t hyphen = token.find('-', start);
			const std::string part = token.substr(start, hyphen == std::string::npos ? std::string::npos : hyphen - start);
			if (part.empty() == false) said.insert(part);
			if (hyphen == std::string::npos) break;
			start = hyphen + 1;
		}
	}
	const auto isSaid = [&](const std::string& word) {
		if (said.count(word) > 0) return true;
		if (word.size() < 7) return false;
		const std::string stem = word.substr(0, 6);
		return std::any_of(said.begin(), said.end(), [&](const std::string& other) { return other.rfind(stem, 0) == 0; });
	};
	const auto wordsOf = [](const std::string& phrase) {
		std::set<std::string> words;
		for (const std::string& token : tokensOf(lowered(phrase))) {
			if (hasLetterOrDigit(token) == true) words.insert(token);
		}
		return words;
	};
	std::vector<std::string> parts;
	std::size_t unsaid = 0;
	for (const WordEdit& edit : edits) {
		bool isMentioned = false;
		if (edit.distinctive.empty() == false) {
			isMentioned = std::any_of(edit.distinctive.begin(), edit.distinctive.end(), isSaid);
		} else {
			// Short words only - "and" for "or": said only where the explanation
			// has one of them, or where they only moved: the "or" a list's new
			// last item takes from the item before it.
			const std::set<std::string> words = wordsOf(edit.removed + " " + edit.added);
			isMentioned = std::any_of(words.begin(), words.end(), [&](const std::string& word) { return said.count(word) > 0; });
			const bool takesOut = edit.added.empty() == true && edit.removed.empty() == false;
			const bool putsIn = edit.removed.empty() == true && edit.added.empty() == false;
			if (isMentioned == false && (takesOut == true || putsIn == true)) {
				isMentioned = std::any_of(edits.begin(), edits.end(), [&](const WordEdit& other) {
					if (&other == &edit) return false;
					const std::set<std::string> there = wordsOf(takesOut == true ? other.added : other.removed);
					return std::all_of(words.begin(), words.end(), [&](const std::string& word) { return there.count(word) > 0; });
				});
			}
		}
		if (isMentioned == true) continue;
		++unsaid;
		if (parts.size() >= kMostSaid) continue;
		parts.push_back(describedEdit(edit));
	}
	if (parts.empty() == true) return std::string();
	std::string sentence = "It also ";
	for (std::size_t index = 0; index < parts.size(); ++index) {
		if (index > 0) sentence += index + 1 == parts.size() && unsaid == parts.size() ? " and " : ", ";
		sentence += parts[index];
	}
	if (unsaid > parts.size()) sentence += ", and makes " + std::to_string(unsaid - parts.size()) + " more edits";
	return sentence + ".";
}

std::vector<ClauseChange> clauseChanges(const LegalDocument& before, const LegalDocument& after) {
	// The agreement's own text: its body and exhibits, never a header, a
	// footer or a comment.
	const auto isAgreement = [](const LegalBlock& block) {
		return block.section != "header" && block.section != "footer" && block.section != "comment"
			&& spaced(block.text).empty() == false;
	};
	// A paragraph Word does not number is named by its order, which moves when
	// a paragraph comes or goes before it: such paragraphs are matched by their
	// words, the numbered clauses by their ids.
	const auto isNumbered = [](const LegalBlock& block) { return block.number.empty() == false; };
	std::map<std::string, const LegalBlock*> earlierClauses;
	std::set<std::string> earlierParagraphs;
	std::set<std::string> laterParagraphs;
	for (const LegalBlock& block : before.blocks) {
		if (isAgreement(block) == false) continue;
		if (isNumbered(block) == true) earlierClauses[block.id] = &block;
		else earlierParagraphs.insert(spaced(block.text));
	}
	for (const LegalBlock& block : after.blocks) {
		if (isAgreement(block) == true && isNumbered(block) == false) laterParagraphs.insert(spaced(block.text));
	}
	std::vector<ClauseChange> changes;
	std::set<std::string> kept;
	for (const LegalBlock& block : after.blocks) {
		if (isAgreement(block) == false) continue;
		ClauseChange change;
		change.blockId = block.id;
		change.number = block.number;
		change.heading = block.heading;
		if (isNumbered(block) == false) {
			if (earlierParagraphs.count(spaced(block.text)) > 0) continue;
			change.kind = "added";
			change.description = "a new passage: “" + excerpt(block.text) + "”";
			changes.push_back(change);
			continue;
		}
		const auto found = earlierClauses.find(block.id);
		if (found == earlierClauses.end()) {
			change.kind = "added";
			change.description = "a new clause: “" + excerpt(block.text) + "”";
			changes.push_back(change);
			continue;
		}
		kept.insert(block.id);
		if (spaced(found->second->text) == spaced(block.text)) continue;
		const std::vector<WordEdit> edits = wordEdits(found->second->text, block.text);
		if (edits.empty() == true) {
			// Marks that moved are no change; a clause too long to compare word
			// by word is said to be rewritten.
			if (lettersOf(found->second->text) == lettersOf(block.text)) continue;
			change.kind = "changed";
			change.description = "rewritten; it now reads “" + excerpt(block.text) + "”";
			changes.push_back(change);
			continue;
		}
		change.kind = "changed";
		for (std::size_t index = 0; index < edits.size() && index < kMostSaid; ++index) {
			if (index > 0) change.description += "; ";
			change.description += describedEdit(edits[index]);
		}
		if (edits.size() > kMostSaid) change.description += "; and " + std::to_string(edits.size() - kMostSaid) + " more edits";
		changes.push_back(change);
	}
	for (const LegalBlock& block : before.blocks) {
		if (isAgreement(block) == false) continue;
		ClauseChange change;
		change.blockId = block.id;
		change.number = block.number;
		change.heading = block.heading;
		change.kind = "removed";
		if (isNumbered(block) == true) {
			if (kept.count(block.id) > 0) continue;
			change.description = "taken out; it read “" + excerpt(block.text) + "”";
		} else {
			if (laterParagraphs.count(spaced(block.text)) > 0) continue;
			change.description = "a passage taken out; it read “" + excerpt(block.text) + "”";
		}
		changes.push_back(change);
	}
	return changes;
}

} // namespace singularity::document_intelligence
