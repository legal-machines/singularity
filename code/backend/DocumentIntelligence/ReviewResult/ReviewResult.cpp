#include "DocumentIntelligence/ReviewResult/ReviewResult.hpp"
#include "DocumentIntelligence/LegalStructure/LegalStructure.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace singularity::document_intelligence {
namespace {

using Json = nlohmann::json;

std::string trimmed(const std::string& text) {
	std::size_t first = 0;
	while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
	std::size_t last = text.size();
	while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
	return text.substr(first, last - first);
}

std::string stringIn(const Json& value, const char* key) {
	if (value.is_object() == false || value.contains(key) == false || value.at(key).is_string() == false) return std::string();
	return trimmed(value.at(key).get<std::string>());
}

std::vector<std::string> stringsIn(const Json& value, const char* key) {
	std::vector<std::string> items;
	if (value.is_object() == false || value.contains(key) == false || value.at(key).is_array() == false) return items;
	for (const Json& item : value.at(key)) {
		if (item.is_string() == true && trimmed(item.get<std::string>()).empty() == false) items.push_back(trimmed(item.get<std::string>()));
	}
	return items;
}

// The JSON object an answer holds: the whole answer, or what stands between a
// code fence a model put around it.
bool parseAnswer(const std::string& answer, Json& parsed) {

	std::string text = trimmed(answer);
	if (text.rfind("```", 0) == 0) {
		const std::size_t lineEnd = text.find('\n');
		const std::size_t fenceEnd = text.rfind("```");
		if (lineEnd != std::string::npos && fenceEnd != std::string::npos && fenceEnd > lineEnd) {
			text = trimmed(text.substr(lineEnd + 1, fenceEnd - lineEnd - 1));
		}
	}
	const std::size_t first = text.find('{');
	const std::size_t last = text.rfind('}');
	if (first == std::string::npos || last == std::string::npos || last < first) return false;
	try {
		parsed = Json::parse(text.substr(first, last - first + 1));
	} catch (const std::exception&) {
		return false;
	}
	return parsed.is_object();

}

bool isAgreementText(const LegalBlock* block) {
	return block != nullptr && block->kind != BlockKind::Comment && block->kind != BlockKind::HeaderFooter;
}

// A clause's words as compared whole: spacing and typographic quotes made
// plain, and a final full stop or semicolon not counted.
std::string comparableWords(const std::string& text) {
	std::string words = normalizedSpacing(text);
	for (const auto& [typographic, plain] : std::vector<std::pair<std::string, std::string>>{
			{"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""}, {"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}}) {
		for (std::size_t at = words.find(typographic); at != std::string::npos; at = words.find(typographic, at)) {
			words.replace(at, typographic.size(), plain);
		}
	}
	while (words.empty() == false && (words.back() == '.' || words.back() == ';' || words.back() == ' ')) words.pop_back();
	return words;
}

// Whether a change's quoted words are the whole of its clause.
bool isWholeBlock(const std::string& quoted, const std::string& blockText) {
	return comparableWords(quoted).empty() == false && comparableWords(quoted) == comparableWords(blockText);
}

// Words a model writes for a clause it strikes.
bool isOmission(const std::string& proposed) {
	std::string words = comparableWords(proposed);
	std::transform(words.begin(), words.end(), words.begin(), [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
	return words.empty() == true || words == "[intentionally omitted" || words == "intentionally omitted" || words == "[reserved"
		|| words == "reserved" || words == "[intentionally omitted.]" || words == "[reserved.]" || words == "[intentionally omitted]"
		|| words == "[reserved]";
}

// A quotation a model wrote with the clause number Word draws in front of it
// ("5.1 This Agreement ..."): the number is not the clause's words.
std::string withoutLeadingNumber(const std::string& text) {
	static const std::regex leading(R"(^\s*(?:Section\s+)?\d+(?:\.\d+)*\.?\s+)");
	return std::regex_replace(text, leading, "", std::regex_constants::format_first_only);
}

std::vector<std::string> exhibitLabelsIn(const std::string& text) {
	static const std::regex exhibit(R"(\b(Exhibit|Schedule|Annex|Appendix)\s+([A-Z]|\d{1,2})(?![A-Za-z0-9]))");
	std::vector<std::string> labels;
	for (std::sregex_iterator it(text.begin(), text.end(), exhibit), end; it != end; ++it) {
		labels.push_back((*it)[1].str() + " " + (*it)[2].str());
	}
	return labels;
}

std::vector<std::string> sectionLabelsIn(const std::string& text) {
	static const std::regex section(R"(\bSections?\s+(\d+(?:\.\d+)*))");
	std::vector<std::string> labels;
	for (std::sregex_iterator it(text.begin(), text.end(), section), end; it != end; ++it) {
		labels.push_back("Section " + (*it)[1].str());
	}
	return labels;
}

Json checkToJson(SourceCheck check) {
	return Json(toString(check));
}

// A rewrite a model wrote with the clause number Word draws in front of the
// clause: the number is not the clause's words.
std::string withoutClauseNumber(const std::string& text, const std::string& number) {
	std::string words = trimmed(text);
	if (number.empty() == true) return words;
	for (const std::string& written : {number + ". ", number + " ", "Section " + number + ". ", "Section " + number + " "}) {
		if (words.rfind(written, 0) == 0) return trimmed(words.substr(written.size()));
	}
	return words;
}

// The last sentence of a clause, which an addition after it is anchored on.
std::string lastSentenceOf(const std::string& text) {
	const std::string words = trimmed(text);
	std::size_t from = 0;
	for (std::size_t at = 0; at + 2 < words.size(); ++at) {
		if ((words[at] == '.' || words[at] == ';') && words[at + 1] == ' '
				&& std::isupper(static_cast<unsigned char>(words[at + 2])) != 0) {
			from = at + 2;
		}
	}
	return words.substr(from);
}

SourceCheck checkFromString(const std::string& value) {
	if (value == "verified") return SourceCheck::Verified;
	if (value == "relocated") return SourceCheck::Relocated;
	if (value == "ambiguous") return SourceCheck::Ambiguous;
	if (value == "not_found") return SourceCheck::NotFound;
	if (value == "unknown_place") return SourceCheck::UnknownPlace;
	return SourceCheck::NotApplicable;
}


// ---- A clause and its rewrite -------------------------------------------------

struct Token {
	std::size_t start = 0;
	std::size_t length = 0;
	// 'w' a word or figure, 's' spacing, 'p' a mark.
	char kind = 'w';
	// How it compares: typographic quotes and dashes as their plain kin.
	std::string key;
};

std::size_t characterWidth(const std::string& text, std::size_t at) {
	const unsigned char lead = static_cast<unsigned char>(text[at]);
	std::size_t width = 1;
	if (lead >= 0xF0) width = 4;
	else if (lead >= 0xE0) width = 3;
	else if (lead >= 0xC0) width = 2;
	return std::min(width, text.size() - at);
}

std::string comparableCharacter(const std::string& character) {
	if (character == "\xE2\x80\x9C" || character == "\xE2\x80\x9D") return "\"";
	if (character == "\xE2\x80\x98" || character == "\xE2\x80\x99") return "'";
	if (character == "\xE2\x80\x93" || character == "\xE2\x80\x94") return "-";
	if (character == "\xC2\xA0") return " ";
	return character;
}

bool isSpacing(const std::string& character) {
	return comparableCharacter(character) == " "
		|| (character.size() == 1 && std::isspace(static_cast<unsigned char>(character[0])) != 0);
}

bool isMark(const std::string& character) {
	if (character.size() == 1) {
		const unsigned char letter = static_cast<unsigned char>(character[0]);
		return std::isalnum(letter) == 0 && letter != '_' && std::isspace(letter) == 0;
	}
	static const std::set<std::string> marks{"\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99",
		"\xE2\x80\x93", "\xE2\x80\x94", "\xE2\x80\xA6", "\xC2\xA7", "\xC2\xB6"};
	return marks.count(character) > 0;
}

// A text's words, spacing and marks. An apostrophe or a hyphen between two
// letters belongs to its word: "Party's", "non-solicit".
std::vector<Token> tokensOf(const std::string& text) {
	std::vector<Token> tokens;
	std::size_t at = 0;
	while (at < text.size()) {
		const std::size_t width = characterWidth(text, at);
		const std::string character = text.substr(at, width);
		if (isSpacing(character) == true) {
			Token token;
			token.start = at;
			token.kind = 's';
			token.key = " ";
			while (at < text.size() && isSpacing(text.substr(at, characterWidth(text, at))) == true) at += characterWidth(text, at);
			token.length = at - token.start;
			tokens.push_back(token);
			continue;
		}
		const std::string comparable = comparableCharacter(character);
		const bool endsWord = tokens.empty() == false && tokens.back().kind == 'w'
			&& tokens.back().start + tokens.back().length == at;
		if (isMark(character) == true) {
			bool isJoiner = false;
			if ((comparable == "'" || comparable == "-") && endsWord == true && at + width < text.size()) {
				const std::string next = text.substr(at + width, characterWidth(text, at + width));
				isJoiner = isMark(next) == false && isSpacing(next) == false;
			}
			if (isJoiner == true) {
				tokens.back().length += width;
				tokens.back().key += comparable;
			} else {
				Token token;
				token.start = at;
				token.length = width;
				token.kind = 'p';
				token.key = comparable;
				tokens.push_back(token);
			}
			at += width;
			continue;
		}
		if (endsWord == true) {
			tokens.back().length += width;
			tokens.back().key += character;
		} else {
			Token token;
			token.start = at;
			token.length = width;
			token.key = character;
			tokens.push_back(token);
		}
		at += width;
	}
	return tokens;
}

// The tokens the two texts share, in order: the common subsequence that keeps
// the most of their words, a word weighing by its length and spacing or a mark
// by one. So "evaluating and negotiating" made "evaluating, negotiating and
// consummating" keeps "negotiating", and the redline strikes "and" and adds
// "and consummating", as a lawyer marks it up - never a new "negotiating"
// beside the old one struck because the spaces round "and" matched more (a
// live run, 25 Oct 2026). For each token of `before`, the token of `after` it
// is, or npos where it is not in `after`.
std::vector<std::size_t> sharedTokens(const std::vector<Token>& before, const std::vector<Token>& after) {
	const std::size_t n = before.size();
	const std::size_t m = after.size();
	const auto weight = [](const Token& token) {
		return token.kind == 'w' ? static_cast<std::uint32_t>(2 + token.key.size()) : std::uint32_t{1};
	};
	std::vector<std::vector<std::uint32_t>> longest(n + 1, std::vector<std::uint32_t>(m + 1, 0));
	for (std::size_t i = n; i-- > 0;) {
		for (std::size_t j = m; j-- > 0;) {
			longest[i][j] = before[i].key == after[j].key ? longest[i + 1][j + 1] + weight(before[i])
				: std::max(longest[i + 1][j], longest[i][j + 1]);
		}
	}
	std::vector<std::size_t> paired(n, std::string::npos);
	std::size_t i = 0;
	std::size_t j = 0;
	while (i < n && j < m) {
		if (before[i].key == after[j].key) {
			paired[i] = j;
			++i;
			++j;
		} else if (longest[i + 1][j] >= longest[i][j + 1]) {
			++i;
		} else {
			++j;
		}
	}
	return paired;
}

// Inserted words in the document's own way of quoting: where the clause uses
// typographic quotes, a model's plain ones become them.
std::string inDocumentQuotes(const std::string& words, bool isTypographic, char before) {
	if (isTypographic == false) return words;
	std::string written;
	char previous = before;
	for (const char character : words) {
		if (character == '"') {
			const bool opens = previous == ' ' || previous == '(' || previous == '[' || previous == '\0';
			written += opens == true ? "\xE2\x80\x9C" : "\xE2\x80\x9D";
		} else if (character == '\'') {
			const bool opens = previous == ' ' || previous == '(' || previous == '[' || previous == '\0';
			written += opens == true ? "\xE2\x80\x98" : "\xE2\x80\x99";
		} else {
			written += character;
		}
		previous = character;
	}
	return written;
}

struct Region {
	// Token ranges [from, to) of the clause and of its rewrite.
	std::size_t beforeFrom = 0;
	std::size_t beforeTo = 0;
	std::size_t afterFrom = 0;
	std::size_t afterTo = 0;
};

} // namespace

ClauseRewrite rewriteOfClause(const LegalDocument& document, const std::string& clauseText, const std::string& wanted) {

	ClauseRewrite made;
	const std::string rewrite = trimmed(wanted);
	const std::vector<Token> before = tokensOf(clauseText);
	const std::vector<Token> after = tokensOf(rewrite);
	const bool isTypographic = clauseText.find("\xE2\x80\x9C") != std::string::npos
		|| clauseText.find("\xE2\x80\x9D") != std::string::npos || clauseText.find("\xE2\x80\x99") != std::string::npos;
	const auto tokenText = [](const std::string& text, const Token& token) { return text.substr(token.start, token.length); };

	// A clause too long to align word by word is rewritten whole.
	const bool isAligned = before.size() * after.size() <= 4000000;
	std::vector<std::size_t> paired = isAligned == true ? sharedTokens(before, after)
		: std::vector<std::size_t>(before.size(), std::string::npos);

	// What differs: runs of tokens between the shared ones.
	std::vector<Region> regions;
	std::size_t afterAt = 0;
	std::size_t beforeAt = 0;
	for (std::size_t i = 0; i <= before.size(); ++i) {
		const bool isEnd = i == before.size();
		if (isEnd == false && paired[i] == std::string::npos) continue;
		const std::size_t j = isEnd == true ? after.size() : paired[i];
		if (i > beforeAt || j > afterAt) regions.push_back(Region{beforeAt, i, afterAt, j});
		beforeAt = i + 1;
		afterAt = j + 1;
	}
	if (regions.empty() == true) return made;

	const auto onlySpacingAndMarks = [&](std::size_t from, std::size_t to) {
		for (std::size_t index = from; index < to; ++index) if (before[index].kind == 'w') return false;
		return true;
	};
	// Changes a few marks apart are one change: "fifteen (15)" -> "sixty (60)".
	std::vector<Region> joined;
	for (const Region& region : regions) {
		if (joined.empty() == false && onlySpacingAndMarks(joined.back().beforeTo, region.beforeFrom) == true) {
			joined.back().beforeTo = region.beforeTo;
			joined.back().afterTo = region.afterTo;
		} else {
			joined.push_back(region);
		}
	}

	// A change keeps its brackets whole: "fifteen (15)" -> "sixty (60)", never
	// "fifteen (15" -> "sixty (60".
	const auto balance = [&](std::size_t from, std::size_t to, const std::vector<Token>& tokens, const char* open, const char* close) {
		int depth = 0;
		for (std::size_t index = from; index < to; ++index) {
			if (tokens[index].key == open) ++depth;
			if (tokens[index].key == close) --depth;
		}
		return depth;
	};
	for (Region& region : joined) {
		for (const auto& [open, close] : std::vector<std::pair<const char*, const char*>>{{"(", ")"}, {"[", "]"}}) {
			while (region.beforeTo < before.size() && paired[region.beforeTo] != std::string::npos
					&& before[region.beforeTo].key == close
					&& (balance(region.beforeFrom, region.beforeTo, before, open, close) > 0
						|| balance(region.afterFrom, region.afterTo, after, open, close) > 0)) {
				region.afterTo = paired[region.beforeTo] + 1;
				++region.beforeTo;
			}
			while (region.beforeFrom > 0 && paired[region.beforeFrom - 1] != std::string::npos
					&& before[region.beforeFrom - 1].key == open
					&& (balance(region.beforeFrom, region.beforeTo, before, open, close) < 0
						|| balance(region.afterFrom, region.afterTo, after, open, close) < 0)) {
				--region.beforeFrom;
				region.afterFrom = paired[region.beforeFrom];
			}
		}
	}

	// The clause as it should read, in the document's own writing: its shared
	// words as the clause writes them, its new words as the rewrite does.
	const auto written = [&](std::size_t beforeFrom, std::size_t beforeTo, std::size_t afterFrom, std::size_t afterTo) {
		std::string words;
		std::size_t j = afterFrom;
		for (std::size_t i = beforeFrom; i <= beforeTo; ++i) {
			const std::size_t target = i < beforeTo ? paired[i] : afterTo;
			if (i < beforeTo && target == std::string::npos) continue;
			for (; j < target && j < afterTo; ++j) {
				words += inDocumentQuotes(tokenText(rewrite, after[j]), isTypographic, words.empty() == true ? '\0' : words.back());
			}
			if (i < beforeTo) {
				words += tokenText(clauseText, before[i]);
				j = target + 1;
			}
		}
		return words;
	};
	made.text = written(0, before.size(), 0, after.size());

	// Each change anchored on words the document holds once: the change, then a
	// word more on either side until it is found once. A change that reaches
	// another one takes it in.
	std::vector<Region> anchored;
	for (Region region : joined) {
		for (;;) {
			if (anchored.empty() == false && region.beforeFrom < anchored.back().beforeTo) {
				region.beforeFrom = anchored.back().beforeFrom;
				region.afterFrom = anchored.back().afterFrom;
				anchored.pop_back();
				continue;
			}
			const bool hasWords = region.beforeTo > region.beforeFrom;
			// Neither the words a change strikes nor those it writes begin or end
			// with a space: the writer finds the struck words without their spaces,
			// and a space it leaves reads "evaluating , negotiating" (a live
			// draft, 24 Oct 2026).
			const std::string strikes = hasWords == true ? clauseText.substr(before[region.beforeFrom].start,
				before[region.beforeTo - 1].start + before[region.beforeTo - 1].length - before[region.beforeFrom].start)
				: std::string();
			const std::string writes = written(region.beforeFrom, region.beforeTo, region.afterFrom, region.afterTo);
			const auto isSpacedAt = [](const std::string& text, bool isStart) {
				return text.empty() == false
					&& std::isspace(static_cast<unsigned char>(isStart == true ? text.front() : text.back())) != 0;
			};
			const bool isOpenAtStart = isSpacedAt(strikes, true) == true || isSpacedAt(writes, true) == true;
			const bool isOpenAtEnd = isSpacedAt(strikes, false) == true || isSpacedAt(writes, false) == true;
			if (hasWords == true && isOpenAtStart == false && isOpenAtEnd == false) {
				const bool hasWord = std::any_of(before.begin() + static_cast<std::ptrdiff_t>(region.beforeFrom),
					before.begin() + static_cast<std::ptrdiff_t>(region.beforeTo), [](const Token& token) { return token.kind == 'w'; });
				if (hasWord == true && placesHoldingPhrase(document, strikes) == 1) break;
			}
			if (region.beforeFrom == 0 && region.beforeTo == before.size()) break;
			// A word more: on the side a space is left open, else before it where
			// there is one, else after it.
			const bool isLeft = region.beforeFrom > 0 && (isOpenAtStart == true || region.beforeTo == before.size()
				|| (isOpenAtEnd == false && (hasWords == false || (region.beforeFrom % 2) == 0)));
			if (isLeft == true) {
				do {
					--region.beforeFrom;
					region.afterFrom = paired[region.beforeFrom] == std::string::npos ? region.afterFrom : paired[region.beforeFrom];
				} while (region.beforeFrom > 0 && before[region.beforeFrom].kind != 'w');
			} else {
				do {
					region.afterTo = paired[region.beforeTo] == std::string::npos ? region.afterTo : paired[region.beforeTo] + 1;
					++region.beforeTo;
				} while (region.beforeTo < before.size() && before[region.beforeTo - 1].kind != 'w');
			}
		}
		anchored.push_back(region);
	}
	for (const Region& region : anchored) {
		TextEdit edit;
		if (region.beforeTo > region.beforeFrom) {
			edit.currentText = clauseText.substr(before[region.beforeFrom].start,
				before[region.beforeTo - 1].start + before[region.beforeTo - 1].length - before[region.beforeFrom].start);
		}
		edit.proposedText = written(region.beforeFrom, region.beforeTo, region.afterFrom, region.afterTo);
		made.edits.push_back(edit);
	}

	// Made sure: the edits, made on the clause, give the rewrite. Where they do
	// not, the clause is rewritten whole, anchored on itself.
	std::string result = clauseText;
	bool isWhole = true;
	for (auto edit = made.edits.rbegin(); edit != made.edits.rend(); ++edit) {
		const std::size_t at = edit->currentText.empty() == true ? std::string::npos : result.find(edit->currentText);
		if (at == std::string::npos) {
			isWhole = false;
			break;
		}
		result.replace(at, edit->currentText.size(), edit->proposedText);
	}
	if (isWhole == false || comparableWords(result) != comparableWords(made.text)) {
		made.edits = {TextEdit{clauseText, made.text}};
	}

	// The redline: the clause in reading order, each change a phrase struck and
	// the phrase put in its place, every other word kept.
	const auto add = [&](const std::string& text, const char* kind) {
		if (text.empty() == true) return;
		if (made.redline.empty() == false && made.redline.back().kind == kind) made.redline.back().text += text;
		else made.redline.push_back(RedlinePiece{text, kind});
	};
	const auto span = [&](const std::string& text, const std::vector<Token>& tokens, std::size_t from, std::size_t to) {
		if (to <= from) return std::string();
		return text.substr(tokens[from].start, tokens[to - 1].start + tokens[to - 1].length - tokens[from].start);
	};
	std::size_t kept = 0;
	for (const Region& region : joined) {
		add(span(clauseText, before, kept, region.beforeFrom), "kept");
		add(span(clauseText, before, region.beforeFrom, region.beforeTo), "struck");
		add(written(region.beforeFrom, region.beforeTo, region.afterFrom, region.afterTo), "inserted");
		kept = region.beforeTo;
	}
	add(span(clauseText, before, kept, before.size()), "kept");
	return made;

}

namespace {

// A row of a table as a model writes it: its cells split at "|", each
// trimmed. A line wrapped in pipes as Markdown writes a table ("| a | b |")
// is unwrapped where the table has rows of that width and none of the
// wrapped one's.
std::vector<std::string> cellsOfLine(const std::string& line, const std::set<std::size_t>& widths) {
	std::vector<std::string> cells;
	std::size_t start = 0;
	while (true) {
		const std::size_t bar = line.find('|', start);
		cells.push_back(trimmed(line.substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
		if (bar == std::string::npos) break;
		start = bar + 1;
	}
	const bool isOpened = line.empty() == false && line.front() == '|';
	const bool isClosed = line.size() > 1 && line.back() == '|';
	if (widths.count(cells.size()) > 0 || cells.size() < 2) return cells;
	if (isOpened == true && isClosed == true && cells.size() >= 3 && widths.count(cells.size() - 2) > 0) {
		return std::vector<std::string>(cells.begin() + 1, cells.end() - 1);
	}
	if (isOpened == true && widths.count(cells.size() - 1) > 0) return std::vector<std::string>(cells.begin() + 1, cells.end());
	if (isClosed == true && widths.count(cells.size() - 1) > 0) return std::vector<std::string>(cells.begin(), cells.end() - 1);
	return cells;
}

// A line that only draws a Markdown table's rule ("|---|:---:|").
bool isRuleLine(const std::string& line) {
	return line.find('-') != std::string::npos
		&& line.find_first_not_of("|-: \t") == std::string::npos;
}

bool sameRow(const std::vector<std::string>& left, const std::vector<std::string>& right) {
	if (left.size() != right.size()) return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (comparableWords(left[index]) != comparableWords(right[index])) return false;
	}
	return true;
}

// Whether two rows of the same width are one row with some cells changed: its
// first cell the same, or at least half its cells.
bool isSameRowChanged(const std::vector<std::string>& before, const std::vector<std::string>& after) {
	if (before.size() != after.size() || before.empty() == true) return false;
	if (comparableWords(before.front()).empty() == false && comparableWords(before.front()) == comparableWords(after.front())) {
		return true;
	}
	std::size_t shared = 0;
	for (std::size_t index = 0; index < before.size(); ++index) {
		if (comparableWords(before[index]).empty() == false && comparableWords(before[index]) == comparableWords(after[index])) {
			++shared;
		}
	}
	return shared > 0 && shared * 2 >= before.size();
}

std::string rowWords(const std::vector<std::string>& cells) {
	std::string words;
	for (std::size_t index = 0; index < cells.size(); ++index) words += (index > 0 ? " | " : "") + cells[index];
	return words;
}

} // namespace

TableRewrite rewriteOfTable(const std::vector<TableRowReading>& rows, const std::string& rewrite) {

	TableRewrite result;
	if (rows.empty() == true) {
		result.problem = "The table could not be read cell by cell, so it was not changed.";
		return result;
	}
	std::set<std::size_t> widths;
	for (const TableRowReading& row : rows) widths.insert(row.cells.size());
	std::vector<std::vector<std::string>> proposed;
	std::istringstream lines(rewrite);
	for (std::string line; std::getline(lines, line);) {
		line = trimmed(line);
		if (line.empty() == true || isRuleLine(line) == true) continue;
		proposed.push_back(cellsOfLine(line, widths));
	}
	for (const std::vector<std::string>& cells : proposed) result.text += (result.text.empty() == true ? "" : "\n") + rowWords(cells);

	/* - - - - - - */

	// The rows it keeps word for word, in order (the longest such sequence).
	const std::size_t before = rows.size();
	const std::size_t after = proposed.size();
	std::vector<std::vector<std::size_t>> longest(before + 1, std::vector<std::size_t>(after + 1, 0));
	for (std::size_t row = before; row-- > 0;) {
		for (std::size_t line = after; line-- > 0;) {
			longest[row][line] = sameRow(rows[row].cells, proposed[line]) == true ? longest[row + 1][line + 1] + 1
				: std::max(longest[row + 1][line], longest[row][line + 1]);
		}
	}
	std::vector<std::pair<std::size_t, std::size_t>> kept;
	for (std::size_t row = 0, line = 0; row < before && line < after;) {
		if (sameRow(rows[row].cells, proposed[line]) == true) {
			kept.emplace_back(row, line);
			++row;
			++line;
		} else if (longest[row + 1][line] >= longest[row][line + 1]) {
			++row;
		} else {
			++line;
		}
	}
	kept.emplace_back(before, after);

	// Between two kept rows: a row the rewrite keeps with some cells changed is
	// paired with its line, in order; the others are struck or added.
	struct Step {
		std::optional<std::size_t> row;
		std::optional<std::size_t> line;
	};
	std::vector<Step> steps;
	std::size_t row = 0;
	std::size_t line = 0;
	for (const auto& [keptRow, keptLine] : kept) {
		std::vector<std::pair<std::size_t, std::size_t>> paired;
		std::size_t from = row;
		for (std::size_t candidate = line; candidate < keptLine; ++candidate) {
			for (std::size_t other = from; other < keptRow; ++other) {
				if (isSameRowChanged(rows[other].cells, proposed[candidate]) == false) continue;
				paired.emplace_back(other, candidate);
				from = other + 1;
				break;
			}
		}
		for (const auto& [pairedRow, pairedLine] : paired) {
			for (; row < pairedRow; ++row) steps.push_back(Step{row, std::nullopt});
			for (; line < pairedLine; ++line) steps.push_back(Step{std::nullopt, line});
			steps.push_back(Step{row++, line++});
		}
		for (; row < keptRow; ++row) steps.push_back(Step{row, std::nullopt});
		for (; line < keptLine; ++line) steps.push_back(Step{std::nullopt, line});
		if (keptRow < before) steps.push_back(Step{row++, line++});
	}

	/* - - - - - - */

	for (std::size_t index = 0; index < steps.size(); ++index) {
		const Step& step = steps[index];
		if (step.row.has_value() == true && step.line.has_value() == true) {
			const TableRowReading& read = rows[*step.row];
			const std::vector<std::string>& cells = proposed[*step.line];
			for (std::size_t column = 0; column < cells.size(); ++column) {
				if (comparableWords(read.cells[column]) == comparableWords(cells[column])) continue;
				if (read.isContinued[column] == true) {
					result.problem = "The rewrite changes a cell merged with the one above it (\"" + cells[column] + "\"); that cell "
						"is changed in the row above, so the table was not changed.";
					return result;
				}
				result.edits.push_back(TableEdit{"cell", *step.row, column, read.cells[column], cells[column], {}});
			}
			continue;
		}
		if (step.row.has_value() == true) {
			if (*step.row == 0) {
				result.problem = "The rewrite leaves out the table's first row (\"" + rowWords(rows.front().cells) + "\"), which is "
					"never struck, so the table was not changed.";
				return result;
			}
			result.edits.push_back(TableEdit{"remove_row", *step.row, 0, rowWords(rows[*step.row].cells), std::string(), {}});
			continue;
		}
		// A new row stands above the next row the table keeps or strikes, or at
		// its end, and is written like that row.
		std::size_t above = before;
		for (std::size_t next = index + 1; next < steps.size(); ++next) {
			if (steps[next].row.has_value() == true) {
				above = *steps[next].row;
				break;
			}
		}
		const TableRowReading& like = rows[above < before ? above : before - 1];
		const std::vector<std::string>& cells = proposed[*step.line];
		if (cells.size() != like.cells.size()) {
			result.problem = "The new row \"" + rowWords(cells) + "\" has " + std::to_string(cells.size()) + " cells where the "
				"table's rows beside it have " + std::to_string(like.cells.size()) + ": a rewrite never adds or takes away a column, "
				"so the table was not changed.";
			return result;
		}
		for (std::size_t column = 0; column < cells.size(); ++column) {
			if (like.isContinued[column] == true && cells[column].empty() == false) {
				result.problem = "The new row \"" + rowWords(cells) + "\" puts words where the rows beside it continue a merged "
					"cell, so the table was not changed.";
				return result;
			}
		}
		result.edits.push_back(TableEdit{"add_row", above, 0, std::string(), rowWords(cells), cells});
	}
	return result;

}

const char* toString(SourceCheck check) {
	switch (check) {
		case SourceCheck::Verified: return "verified";
		case SourceCheck::Relocated: return "relocated";
		case SourceCheck::Ambiguous: return "ambiguous";
		case SourceCheck::NotFound: return "not_found";
		case SourceCheck::UnknownPlace: return "unknown_place";
		case SourceCheck::NotApplicable: return "not_applicable";
	}
	return "not_applicable";
}

Json reviewAnswerSchema() {

	const Json clauses = Json{{"type", "array"}, {"items", Json{{"type", "string"}}}};
	const Json assessment = Json{
		{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"positionId", "assessment", "clauses", "explanation"})},
		{"properties", Json{
			{"positionId", Json{{"type", "string"}}},
			{"assessment", Json{{"type", "string"}, {"enum", Json::array({"conforms", "deviates", "missing", "not_applicable"})}}},
			{"clauses", clauses},
			{"explanation", Json{{"type", "string"}}}}}};
	const Json change = Json{
		{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"positionId", "clause", "action", "text", "explanation", "confidence"})},
		{"properties", Json{
			{"positionId", Json{{"type", "string"}}},
			{"clause", Json{{"type", "string"}}},
			{"action", Json{{"type", "string"}, {"enum", Json::array({"rewrite", "strike", "add_after", "reject"})}}},
			{"text", Json{{"type", "string"}}},
			{"explanation", Json{{"type", "string"}}},
			{"confidence", Json{{"type", "string"}, {"enum", Json::array({"high", "medium", "low"})}}}}}};
	const Json unresolved = Json{
		{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"kind", "description", "clauses", "why", "options", "recommended", "blocks"})},
		{"properties", Json{
			{"kind", Json{{"type", "string"}, {"enum", Json::array({"missing_document", "unresolved_reference", "needs_lawyer",
				"embedded_instruction", "ambiguity"})}}},
			{"description", Json{{"type", "string"}}},
			{"clauses", clauses},
			{"why", Json{{"type", "string"}}},
			{"options", Json{{"type", "array"}, {"items", Json{{"type", "string"}}}}},
			{"recommended", Json{{"type", "boolean"}}},
			{"blocks", Json{{"type", "boolean"}}}}}};
	const Json comment = Json{
		{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"clause", "text"})},
		{"properties", Json{
			{"clause", Json{{"type", "string"}}},
			{"text", Json{{"type", "string"}}}}}};
	return Json{
		{"type", "object"}, {"additionalProperties", false},
		{"required", Json::array({"summary", "assessments", "changes", "comments", "unresolved"})},
		{"properties", Json{
			{"summary", Json{{"type", "string"}}},
			{"assessments", Json{{"type", "array"}, {"items", assessment}}},
			{"changes", Json{{"type", "array"}, {"items", change}}},
			{"comments", Json{{"type", "array"}, {"items", comment}}},
			{"unresolved", Json{{"type", "array"}, {"items", unresolved}}}}}};

}

std::string resolveCitedPlace(const LegalDocument& document, const std::string& cited) {

	std::string place = trimmed(cited);
	if (place.size() >= 2 && place.front() == '[' && place.back() == ']') place = trimmed(place.substr(1, place.size() - 2));
	if (place.empty() == true) return std::string();
	if (findBlock(document, place) != nullptr) return place;
	static const std::regex sectionWords(R"(^(?:Sections?|Clauses?|Articles?|Paragraphs?|\xC2\xA7)\s*)");
	place = std::regex_replace(place, sectionWords, "");
	while (place.empty() == false && place.back() == '.') place.pop_back();
	if (place.empty() == true) return std::string();
	// A clause by its number; a heading only where no clause has that number.
	for (const LegalBlock& block : document.blocks) {
		if (block.kind == BlockKind::Clause && block.number == place) return block.id;
	}
	for (const LegalBlock& block : document.blocks) {
		if (block.kind == BlockKind::Heading && block.number == place) return block.id;
	}
	return std::string();

}

// Whether the lawyer's words ask for this clause to go: "delete Section 5.2",
// "strike clause 7.3", "Section 9.2 is to be removed".
static bool asksToTakeOut(const std::string& request, const LegalBlock& block) {
	std::string number = block.number;
	while (number.empty() == false && number.back() == '.') number.pop_back();
	if (request.empty() == true || number.empty() == true) return false;
	std::string lowered;
	for (const char character : request) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	std::string escaped;
	for (const char character : number) {
		if (character == '.') escaped += "\\.";
		else escaped.push_back(character);
	}
	const std::string place = "(?:section|clause|paragraph|§)\\s*" + escaped + "(?![0-9.])";
	const std::regex asked("(?:delete|strike|remove|omit|take out|cut|drop)(?:\\s+\\S+){0,4}?\\s+" + place);
	const std::regex said(place + "[^.;]{0,40}?\\b(?:deleted|struck|removed|omitted|taken out|dropped)\\b");
	return std::regex_search(lowered, asked) == true || std::regex_search(lowered, said) == true;
}

ValidatedReview validateReviewAnswer(const std::string& answer, const LegalDocument& document,
	const std::vector<ReviewPosition>& positions, const std::string& request) {

	ValidatedReview review;
	Json parsed;
	if (parseAnswer(answer, parsed) == false) {
		review.problems.push_back("The model's answer is not the JSON the review asked for, so none of it was taken.");
		return review;
	}
	const bool hasLists = parsed.contains("changes") == true && parsed.at("changes").is_array() == true
		&& parsed.contains("assessments") == true && parsed.at("assessments").is_array() == true;
	if (hasLists == false) {
		review.problems.push_back("The model's answer has no assessments or changes in the form the review asked for, "
			"so none of it was taken.");
		return review;
	}
	review.isWellFormed = true;
	review.summary = stringIn(parsed, "summary");

	std::map<std::string, const ReviewPosition*> positionById;
	for (const ReviewPosition& position : positions) positionById[position.id] = &position;

	/* - - - - - - */

	// Assessments: each position, once.
	std::set<std::string> assessed;
	for (const Json& entry : parsed.at("assessments")) {
		const std::string positionId = stringIn(entry, "positionId");
		const auto position = positionById.find(positionId);
		if (position == positionById.end()) {
			review.setAside.push_back("An assessment of \"" + positionId + "\", which is not a position of the Playbook.");
			continue;
		}
		if (assessed.insert(positionId).second == false) {
			review.setAside.push_back("A second assessment of " + position->second->topic + "; the first was kept.");
			continue;
		}
		PositionAssessment item;
		item.positionId = positionId;
		item.topic = position->second->topic;
		item.assessment = stringIn(entry, "assessment");
		if (item.assessment != "conforms" && item.assessment != "deviates" && item.assessment != "missing"
			&& item.assessment != "not_applicable") {
			item.assessment = "not_assessed";
		}
		item.explanation = stringIn(entry, "explanation");
		std::vector<std::string> unknown;
		for (const std::string& cited : stringsIn(entry, "clauses")) {
			const std::string blockId = resolveCitedPlace(document, cited);
			if (blockId.empty() == true) unknown.push_back(cited);
			else if (std::find(item.blockIds.begin(), item.blockIds.end(), blockId) == item.blockIds.end()) item.blockIds.push_back(blockId);
		}
		if (item.blockIds.empty() == false) item.check = SourceCheck::Verified;
		else if (unknown.empty() == false) item.check = SourceCheck::UnknownPlace;
		if (unknown.empty() == false) {
			std::string names;
			for (const std::string& cited : unknown) names += (names.empty() == true ? "" : ", ") + cited;
			item.checkNote = "Cited places the document does not have: " + names + ".";
		}
		review.assessments.push_back(item);
	}
	for (const ReviewPosition& position : positions) {
		if (assessed.count(position.id) > 0) continue;
		PositionAssessment item;
		item.positionId = position.id;
		item.topic = position.topic;
		item.assessment = "not_assessed";
		item.explanation = "The review did not assess this position.";
		review.assessments.push_back(item);
	}

	/* - - - - - - */

	// Changes: each one's words found in the source, before it is believed.
	std::size_t number = 0;
	std::set<std::string> rewritten;
	for (const Json& entry : parsed.at("changes")) {
		// A clause as it should read: Singularity finds what differs, word by
		// word, and anchors each difference itself - the model never quotes.
		const std::string verb = stringIn(entry, "action");
		if (verb == "rewrite" || verb == "strike" || verb == "add_after" || verb == "reject") {
			ProposedChange change;
			change.positionId = stringIn(entry, "positionId");
			change.citedClause = stringIn(entry, "clause");
			change.explanation = stringIn(entry, "explanation");
			change.confidence = stringIn(entry, "confidence");
			const std::string text = entry.is_object() == true && entry.contains("text") == true && entry.at("text").is_string() == true
				? entry.at("text").get<std::string>() : std::string();
			const auto position = positionById.find(change.positionId);
			if (position == positionById.end()) {
				review.setAside.push_back("A change for \"" + change.positionId + "\", which is not a position of the Playbook.");
				continue;
			}
			change.severity = position->second->severity;
			const std::string blockId = resolveCitedPlace(document, change.citedClause);
			const LegalBlock* block = findBlock(document, blockId);
			if (block == nullptr || isAgreementText(block) == false || block->kind == BlockKind::Signature) {
				review.setAside.push_back("A change to " + position->second->topic + " that names no clause of the agreement (\""
					+ change.citedClause + "\").");
				continue;
			}
			change.blockId = block->id;
			change.clauseNumber = block->number;
			std::string words = withoutClauseNumber(text, block->number);
			std::string action = verb;
			// The other side's pending change undone: the clause reads as it did
			// before it - their inserted words out, their struck words back - and a
			// clause they added whole goes. A live run struck a whole payment clause
			// to "undo" the other side's change, for want of this (25 Oct 2026).
			bool isRejectedInsertion = false;
			if (action == "reject") {
				if (block->isPendingInsertion == true) {
					isRejectedInsertion = true;
				} else if (block->pendingChanges.empty() == false && trimmed(block->originalText).empty() == false) {
					words = block->originalText;
					action = "rewrite";
				} else {
					review.setAside.push_back("A change to " + position->second->topic + " that rejects a pending change in "
						+ (block->number.empty() == false ? "Section " + block->number : std::string("a clause"))
						+ ", which has none.");
					continue;
				}
			}
			if (action == "rewrite" && isOmission(words) == true) action = "strike";
			// A clause the other side changed, struck whole where the request does
			// not ask for it to go, is their change undone: striking it takes the
			// agreed words out with theirs. Two live runs struck Section 5.2 to
			// "[Intentionally omitted.]" when the request said to restore its sixty
			// days, with "reject" at hand (25 Oct 2026).
			std::string undoneNote;
			if (action == "strike" && block->isPendingInsertion == false && block->pendingChanges.empty() == false
					&& trimmed(block->originalText).empty() == false && asksToTakeOut(request, *block) == false) {
				words = block->originalText;
				action = "rewrite";
				undoneNote = "Taken as undoing the other side's pending change in it: the request does not ask for the clause to "
					"go, and striking it would take the agreed words out with theirs.";
			}
			// A clause the other side added whole, struck, is their insertion taken
			// out: never left as an omitted clause the agreed text never had.
			if (action == "strike" && block->isPendingInsertion == true) isRejectedInsertion = true;
			if (isRejectedInsertion == true) {
				// Their whole new clause goes, number and all: it was never agreed.
				if (rewritten.count(block->id) > 0) {
					review.setAside.push_back("A change to " + position->second->topic + " in a clause another change already "
						"rewrites; only the first was taken.");
					continue;
				}
				change.action = "delete";
				change.currentText = block->text;
				change.proposedText = std::string();
				change.checkNote = "The other side's added clause is taken out.";
				rewritten.insert(block->id);
				change.id = "chg-" + std::to_string(++number);
				change.check = placesHoldingPhrase(document, change.currentText) == 1 ? SourceCheck::Verified : SourceCheck::Ambiguous;
				change.isWritable = change.check == SourceCheck::Verified;
				if (change.isWritable == false) {
					change.checkNote = "The clause's words stand in more than one place in the document, so the change could not "
						"be anchored and was not written.";
				}
				review.changes.push_back(change);
				continue;
			}
			if (action != "strike" && trimmed(words).empty() == true) {
				review.setAside.push_back("A change to " + position->second->topic + " that did not say what "
					+ (action == "add_after" ? std::string("it adds.") : "the clause becomes."));
				continue;
			}
			if (action != "add_after" && rewritten.count(block->id) > 0) {
				review.setAside.push_back("A change to " + position->second->topic + " in a clause another change already "
					"rewrites; only the first was taken.");
				continue;
			}
			if (action == "strike") {
				// Struck whole: a numbered clause keeps its number, as a lawyer marks
				// a struck clause up; any other paragraph goes.
				const bool isNumbered = block->number.empty() == false
					&& (block->kind == BlockKind::Clause || block->kind == BlockKind::Heading);
				change.action = isNumbered == true ? "replace" : "delete";
				change.currentText = block->text;
				change.proposedText = isNumbered == true ? kOmittedClause : std::string();
				if (isNumbered == true) {
					change.checkNote = "Struck as \"" + kOmittedClause + "\", so the numbering and the cross-references stay "
						"as they are.";
				}
			} else if (action == "add_after") {
				change.action = "insert_after";
				std::string anchor = lastSentenceOf(block->text);
				if (placesHoldingPhrase(document, anchor) != 1) anchor = trimmed(block->text);
				change.currentText = anchor;
				change.proposedText = trimmed(words);
			} else if (block->kind == BlockKind::Table) {
				// A table as it should read: each cell changed, row added and row
				// struck is its own tracked change, written together or not at all.
				const TableRewrite made = rewriteOfTable(block->rows, words);
				if (made.problem.empty() == true && made.edits.empty() == true) {
					review.setAside.push_back("A change to " + position->second->topic + " that leaves the table as it is.");
					continue;
				}
				change.action = "table";
				change.currentText = block->text;
				change.proposedText = made.problem.empty() == true ? made.text : trimmed(words);
				change.tableEdits = made.edits;
				change.tableRows = block->rows;
				change.checkNote = made.problem;
				change.id = "chg-" + std::to_string(++number);
				change.check = SourceCheck::Verified;
				change.isWritable = made.problem.empty() == true;
				rewritten.insert(block->id);
				review.changes.push_back(change);
				continue;
			} else {
				const ClauseRewrite made = rewriteOfClause(document, block->text, words);
				if (made.edits.empty() == true) {
					review.setAside.push_back("A change to " + position->second->topic + " that leaves the clause as it is.");
					continue;
				}
				change.action = "rewrite";
				change.currentText = block->text;
				change.proposedText = made.text;
				change.edits = made.edits;
				change.redline = made.redline;
				change.checkNote = undoneNote;
			}
			if (change.action != "insert_after") rewritten.insert(block->id);
			change.id = "chg-" + std::to_string(++number);
			bool isAnchored = true;
			if (change.action == "rewrite") {
				for (const TextEdit& edit : change.edits) {
					if (placesHoldingPhrase(document, edit.currentText) != 1) isAnchored = false;
				}
			} else {
				isAnchored = placesHoldingPhrase(document, change.currentText) == 1;
			}
			change.check = isAnchored == true ? SourceCheck::Verified : SourceCheck::Ambiguous;
			if (isAnchored == false) {
				change.checkNote = "The clause's words stand in more than one place in the document, so the change could not "
					"be anchored and was not written.";
			}
			change.isWritable = isAnchored;
			review.changes.push_back(change);
			continue;
		}

		ProposedChange change;
		change.positionId = stringIn(entry, "positionId");
		change.action = stringIn(entry, "action");
		change.citedClause = stringIn(entry, "clause");
		change.currentText = entry.is_object() == true && entry.contains("currentText") == true && entry.at("currentText").is_string() == true
			? entry.at("currentText").get<std::string>() : std::string();
		change.proposedText = entry.is_object() == true && entry.contains("proposedText") == true && entry.at("proposedText").is_string() == true
			? entry.at("proposedText").get<std::string>() : std::string();
		change.explanation = stringIn(entry, "explanation");
		change.confidence = stringIn(entry, "confidence");
		const auto position = positionById.find(change.positionId);
		if (position == positionById.end()) {
			review.setAside.push_back("A change for \"" + change.positionId + "\", which is not a position of the Playbook.");
			continue;
		}
		change.severity = position->second->severity;
		if (change.action != "replace" && change.action != "delete" && change.action != "insert_after") {
			review.setAside.push_back("A change to " + position->second->topic + " of a kind the review does not make (\""
				+ change.action + "\").");
			continue;
		}
		if (trimmed(change.currentText).empty() == true || (change.action != "delete" && trimmed(change.proposedText).empty() == true)) {
			review.setAside.push_back("A change to " + position->second->topic + " that did not say which words it changes "
				"or what they become.");
			continue;
		}
		if (change.action == "replace" && trimmed(change.currentText) == trimmed(change.proposedText)) {
			review.setAside.push_back("A change to " + position->second->topic + " that leaves the words as they are.");
			continue;
		}
		change.id = "chg-" + std::to_string(++number);

		const std::string cited = resolveCitedPlace(document, change.citedClause);
		std::size_t places = placesHoldingPhrase(document, change.currentText);
		std::vector<std::string> holders = blocksHoldingPhrase(document, change.currentText);
		if (places == 0) {
			// The words with the clause number Word draws in front of them.
			const std::string unnumbered = withoutLeadingNumber(change.currentText);
			if (unnumbered != change.currentText && placesHoldingPhrase(document, unnumbered) == 1) {
				change.currentText = unnumbered;
				places = 1;
				holders = blocksHoldingPhrase(document, unnumbered);
			}
		}
		if (places == 0) {
			change.check = SourceCheck::NotFound;
			change.blockId = cited;
			change.checkNote = "The words this change replaces are not the document's: it quotes text the document "
				"does not hold, so it was not written.";
		} else if (places > 1) {
			change.check = SourceCheck::Ambiguous;
			change.blockId = cited;
			change.checkNote = "The words this change replaces stand in more than one place in the document, so it "
				"could not be anchored and was not written.";
		} else if (holders.size() == 1 && holders.front() == cited) {
			change.check = SourceCheck::Verified;
			change.blockId = cited;
		} else {
			change.check = SourceCheck::Relocated;
			change.blockId = holders.empty() == true ? cited : holders.front();
			change.checkNote = change.citedClause.empty() == true
				? "The change named no place; its words stand in " + change.blockId + "."
				: "The change named " + change.citedClause + "; its words stand in " + change.blockId + ".";
		}
		const LegalBlock* block = findBlock(document, change.blockId);
		if (block != nullptr) change.clauseNumber = block->number;
		change.isWritable = (change.check == SourceCheck::Verified || change.check == SourceCheck::Relocated)
			&& isAgreementText(block) == true && block->kind != BlockKind::Signature;
		// A numbered clause struck whole keeps its number: its words become
		// "[Intentionally omitted.]", as a lawyer marks a struck clause up, so the
		// agreement's numbering and every cross-reference to it stay as they are
		// and no heading is left with nothing under it.
		if (change.isWritable == true && block != nullptr && block->number.empty() == false
				&& (block->kind == BlockKind::Clause || block->kind == BlockKind::Heading)
				&& (change.action == "delete" || (change.action == "replace" && isOmission(change.proposedText)))
				&& isWholeBlock(change.currentText, block->text)) {
			change.action = "replace";
			change.proposedText = kOmittedClause;
			change.checkNote = (change.checkNote.empty() == true ? std::string() : change.checkNote + " ")
				+ "Struck as \"" + kOmittedClause + "\", so the numbering and the cross-references stay as they are.";
		}
		review.changes.push_back(change);
	}

	/* - - - - - - */

	// Comments for the margin: each on a clause the agreement has, one a clause,
	// in the words the mark-up's reader sees.
	if (parsed.contains("comments") == true && parsed.at("comments").is_array() == true) {
		std::set<std::string> commented;
		for (const Json& entry : parsed.at("comments")) {
			const std::string cited = stringIn(entry, "clause");
			const std::string words = trimmed(stringIn(entry, "text"));
			if (words.empty() == true) continue;
			const LegalBlock* block = findBlock(document, resolveCitedPlace(document, cited));
			if (block == nullptr || isAgreementText(block) == false || block->kind == BlockKind::Signature) {
				review.setAside.push_back("A comment for the margin that names no clause of the agreement (\"" + cited + "\").");
				continue;
			}
			if (commented.insert(block->id).second == false) {
				review.setAside.push_back("A second comment for the margin of " + placeInLawyersWords(document, block->id)
					+ "; the first was kept.");
				continue;
			}
			MarginComment comment;
			comment.blockId = block->id;
			comment.clauseNumber = block->number;
			comment.text = inLawyersWords(document, words);
			comment.anchorText = block->text;
			review.comments.push_back(comment);
		}
	}

	/* - - - - - - */

	// What is not resolved: the model's points, then what the structure shows.
	if (parsed.contains("unresolved") == true && parsed.at("unresolved").is_array() == true) {
		for (const Json& entry : parsed.at("unresolved")) {
			UnresolvedPoint point;
			point.kind = stringIn(entry, "kind");
			if (point.kind != "missing_document" && point.kind != "unresolved_reference" && point.kind != "needs_lawyer"
				&& point.kind != "embedded_instruction" && point.kind != "ambiguity") {
				point.kind = "ambiguity";
			}
			point.description = stringIn(entry, "description");
			if (point.description.empty() == true) continue;
			if (point.kind == "needs_lawyer") {
				point.why = stringIn(entry, "why");
				for (const std::string& option : stringsIn(entry, "options")) {
					if (option.empty() == false && point.options.size() < 6) point.options.push_back(option);
				}
				point.isRecommended = point.options.size() > 1 && entry.contains("recommended") == true
					&& entry.at("recommended").is_boolean() == true && entry.at("recommended").get<bool>() == true;
				point.isBlocking = entry.contains("blocks") == true && entry.at("blocks").is_boolean() == true
					&& entry.at("blocks").get<bool>() == true;
			}
			for (const std::string& cited : stringsIn(entry, "clauses")) {
				const std::string blockId = resolveCitedPlace(document, cited);
				if (blockId.empty() == false) point.blockIds.push_back(blockId);
			}
			point.foundBy = "model";
			if (point.kind == "missing_document") {
				for (const std::string& label : exhibitLabelsIn(point.description)) {
					for (const ExhibitStatus& exhibit : document.exhibits) {
						if (exhibit.label == label && exhibit.isPresent == false) point.isConfirmedByStructure = true;
					}
				}
			}
			review.unresolved.push_back(point);
		}
	}
	for (const ExhibitStatus& exhibit : document.exhibits) {
		if (exhibit.isPresent == true) continue;
		bool isSaid = false;
		for (UnresolvedPoint& point : review.unresolved) {
			const std::vector<std::string> labels = exhibitLabelsIn(point.description);
			if (std::find(labels.begin(), labels.end(), exhibit.label) == labels.end()) continue;
			isSaid = true;
			point.foundBy = "both";
			point.isConfirmedByStructure = true;
		}
		if (isSaid == true) continue;
		UnresolvedPoint point;
		point.kind = "missing_document";
		point.description = exhibit.label + " is referred to but not attached, so it was not reviewed.";
		point.blockIds = exhibit.referencedIn;
		point.foundBy = "structure";
		point.isConfirmedByStructure = true;
		review.unresolved.push_back(point);
	}
	std::set<std::string> dangling;
	for (const CrossReference& reference : document.references) {
		if (reference.kind != "section" || reference.isResolved == true) continue;
		if (dangling.insert(reference.label).second == false) continue;
		bool isSaid = false;
		for (UnresolvedPoint& point : review.unresolved) {
			const std::vector<std::string> labels = sectionLabelsIn(point.description);
			if (std::find(labels.begin(), labels.end(), reference.label) == labels.end()) continue;
			isSaid = true;
			point.foundBy = point.foundBy == "structure" ? "structure" : "both";
			point.isConfirmedByStructure = true;
		}
		if (isSaid == true) continue;
		UnresolvedPoint point;
		point.kind = "unresolved_reference";
		point.description = reference.label + " is referred to but does not exist in the agreement.";
		point.blockIds.push_back(reference.fromBlockId);
		point.foundBy = "structure";
		point.isConfirmedByStructure = true;
		review.unresolved.push_back(point);
	}
	for (const EmbeddedInstruction& instruction : document.embeddedInstructions) {
		bool isSaid = false;
		for (UnresolvedPoint& point : review.unresolved) {
			if (point.kind != "embedded_instruction") continue;
			isSaid = true;
			point.foundBy = "both";
			point.isConfirmedByStructure = true;
			if (std::find(point.blockIds.begin(), point.blockIds.end(), instruction.blockId) == point.blockIds.end()) {
				point.blockIds.push_back(instruction.blockId);
			}
		}
		if (isSaid == true) continue;
		const LegalBlock* block = findBlock(document, instruction.blockId);
		UnresolvedPoint point;
		point.kind = "embedded_instruction";
		point.description = (block != nullptr && block->kind == BlockKind::Comment
			? "A comment by " + (block->author.empty() == true ? std::string("someone") : block->author) + " on "
				+ block->annotates + " speaks to AI review tools"
			: std::string("Words in the document speak to AI review tools"))
			+ ". It was read as part of the document and not followed.";
		point.blockIds.push_back(instruction.blockId);
		point.foundBy = "structure";
		point.isConfirmedByStructure = true;
		review.unresolved.push_back(point);
	}

	// What the lawyer reads names places as the agreement numbers them, never
	// by the ids the material gave the model to cite them by.
	review.summary = inLawyersWords(document, review.summary);
	for (PositionAssessment& assessment : review.assessments) {
		assessment.explanation = inLawyersWords(document, assessment.explanation);
	}
	for (ProposedChange& change : review.changes) change.explanation = inLawyersWords(document, change.explanation);
	for (UnresolvedPoint& point : review.unresolved) point.description = inLawyersWords(document, point.description);
	return review;

}

std::string placeInLawyersWords(const LegalDocument& document, const std::string& blockId) {

	const LegalBlock* block = findBlock(document, blockId);
	if (block == nullptr) return std::string();
	std::string number = block->number;
	while (number.empty() == false && number.back() == '.') number.pop_back();
	if (block->id.rfind("x-", 0) == 0) {
		const std::string rest = block->id.substr(2);
		const std::string label = rest.substr(0, rest.find('-'));
		if (label.empty() == false) return "Exhibit " + label;
	}
	switch (block->kind) {
		case BlockKind::Clause:
			if (number.empty() == false) return "Section " + number;
			break;
		case BlockKind::Heading:
			if (number.empty() == false) return "the heading of Section " + number;
			break;
		case BlockKind::Comment:
			return block->author.empty() == true ? std::string("a comment") : "the comment by " + block->author;
		case BlockKind::Signature:
			return "the signature page";
		case BlockKind::HeaderFooter:
			return "the page header or footer";
		case BlockKind::Table:
			return "a table";
		case BlockKind::Title:
			return "the title";
		default:
			break;
	}
	// An unnumbered paragraph, by its first words.
	std::string words;
	int count = 0;
	std::istringstream reader(block->text);
	for (std::string word; reader >> word && count < 6; ++count) words += (words.empty() == true ? "" : " ") + word;
	return words.empty() == true ? std::string("a paragraph") : "the paragraph that begins \"" + words + "\"";

}

std::string inLawyersWords(const LegalDocument& document, const std::string& text) {

	static const std::regex placeId(R"(\[?\b((?:sig|hf|s|h|x|t|c|p)-[A-Za-z0-9][A-Za-z0-9.\-]*)\]?)");
	std::string result;
	std::size_t written = 0;
	for (auto found = std::sregex_iterator(text.begin(), text.end(), placeId); found != std::sregex_iterator(); ++found) {
		const std::smatch& match = *found;
		std::string id = match[1].str();
		std::string after;
		// "s-6.1." at the end of a sentence: the full stop is the sentence's.
		while (findBlock(document, id) == nullptr && id.empty() == false && (id.back() == '.' || id.back() == '-')) {
			after.insert(after.begin(), id.back());
			id.pop_back();
		}
		std::string name = placeInLawyersWords(document, id);
		if (name.empty() == true) continue;
		const std::size_t start = static_cast<std::size_t>(match.position(0));
		std::string before = text.substr(written, start - written);
		// "Section s-6.1" is "Section 6.1"; "Comment c-0" is "the comment".
		std::string trimmedBefore = before;
		while (trimmedBefore.empty() == false && trimmedBefore.back() == ' ') trimmedBefore.pop_back();
		std::string lastWord;
		for (std::size_t index = trimmedBefore.size(); index > 0; --index) {
			if (std::isalpha(static_cast<unsigned char>(trimmedBefore[index - 1])) == 0) break;
			lastWord.insert(lastWord.begin(), trimmedBefore[index - 1]);
		}
		std::string lowered = lastWord;
		std::transform(lowered.begin(), lowered.end(), lowered.begin(),
			[](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
		if ((lowered == "section" || lowered == "sections" || lowered == "clause" || lowered == "clauses")
			&& name.rfind("Section ", 0) == 0) {
			name = name.substr(8);
		} else if (lowered == "comment" && name.find("comment") != std::string::npos) {
			before = trimmedBefore.substr(0, trimmedBefore.size() - lastWord.size());
			name = std::isupper(static_cast<unsigned char>(lastWord.front())) != 0 ? "The comment" : "the comment";
		} else if (name.rfind("the ", 0) == 0 || name.rfind("a ", 0) == 0) {
			// At the start of a sentence the name opens with a capital.
			const bool isSentenceStart = trimmedBefore.empty() == true || trimmedBefore.back() == '.'
				|| trimmedBefore.back() == '!' || trimmedBefore.back() == '?' || trimmedBefore.back() == ':';
			if (isSentenceStart == true) name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
		}
		result += before + name + after;
		written = start + static_cast<std::size_t>(match.length(0));
	}
	result += text.substr(written);
	return result;

}

Json validatedReviewToJson(const ValidatedReview& review) {

	Json assessments = Json::array();
	for (const PositionAssessment& item : review.assessments) {
		assessments.push_back(Json{{"positionId", item.positionId}, {"topic", item.topic}, {"assessment", item.assessment},
			{"blockIds", item.blockIds}, {"explanation", item.explanation}, {"check", checkToJson(item.check)},
			{"checkNote", item.checkNote}});
	}
	Json changes = Json::array();
	for (const ProposedChange& change : review.changes) {
		Json item = Json{{"id", change.id}, {"positionId", change.positionId}, {"action", change.action},
			{"citedClause", change.citedClause}, {"blockId", change.blockId}, {"clauseNumber", change.clauseNumber},
			{"currentText", change.currentText}, {"proposedText", change.proposedText}, {"explanation", change.explanation},
			{"confidence", change.confidence}, {"severity", change.severity}, {"check", checkToJson(change.check)},
			{"checkNote", change.checkNote}, {"isWritable", change.isWritable}, {"writeState", change.writeState},
			{"writeNote", change.writeNote}};
		if (change.readBack.empty() == false) item["readBack"] = change.readBack;
		if (change.edits.empty() == false) {
			Json edits = Json::array();
			for (const TextEdit& edit : change.edits) edits.push_back(Json{{"currentText", edit.currentText}, {"proposedText", edit.proposedText}});
			item["edits"] = std::move(edits);
		}
		if (change.redline.empty() == false) {
			Json redline = Json::array();
			for (const RedlinePiece& piece : change.redline) redline.push_back(Json{{"text", piece.text}, {"kind", piece.kind}});
			item["redline"] = std::move(redline);
		}
		if (change.tableEdits.empty() == false) {
			Json edits = Json::array();
			for (const TableEdit& edit : change.tableEdits) {
				edits.push_back(Json{{"kind", edit.kind}, {"row", edit.row}, {"column", edit.column}, {"currentText", edit.currentText},
					{"proposedText", edit.proposedText}, {"cells", edit.cells}});
			}
			item["tableEdits"] = std::move(edits);
			Json rows = Json::array();
			for (const TableRowReading& row : change.tableRows) {
				Json continued = Json::array();
				for (std::size_t index = 0; index < row.isContinued.size(); ++index) {
					if (row.isContinued[index] == true) continued.push_back(index);
				}
				rows.push_back(Json{{"cells", row.cells}, {"continued", continued}});
			}
			item["tableRows"] = std::move(rows);
		}
		changes.push_back(std::move(item));
	}
	Json unresolved = Json::array();
	for (const UnresolvedPoint& point : review.unresolved) {
		Json item{{"kind", point.kind}, {"description", point.description}, {"blockIds", point.blockIds},
			{"foundBy", point.foundBy}, {"isConfirmedByStructure", point.isConfirmedByStructure}};
		if (point.kind == "needs_lawyer") {
			item["why"] = point.why;
			item["options"] = point.options;
			item["isRecommended"] = point.isRecommended;
			item["isBlocking"] = point.isBlocking;
		}
		unresolved.push_back(std::move(item));
	}
	Json result = Json{{"isWellFormed", review.isWellFormed}, {"problems", review.problems}, {"summary", review.summary},
		{"assessments", std::move(assessments)}, {"changes", std::move(changes)}, {"unresolved", std::move(unresolved)},
		{"setAside", review.setAside}};
	if (review.comments.empty() == false) {
		Json comments = Json::array();
		for (const MarginComment& comment : review.comments) {
			comments.push_back(Json{{"blockId", comment.blockId}, {"clauseNumber", comment.clauseNumber}, {"text", comment.text},
				{"anchorText", comment.anchorText}, {"writeState", comment.writeState}, {"writeNote", comment.writeNote}});
		}
		result["comments"] = std::move(comments);
	}
	return result;

}

ValidatedReview validatedReviewFromJson(const Json& value) {

	ValidatedReview review;
	review.isWellFormed = value.value("isWellFormed", false);
	review.problems = value.value("problems", std::vector<std::string>());
	review.summary = value.value("summary", std::string());
	review.setAside = value.value("setAside", std::vector<std::string>());
	for (const Json& entry : value.value("assessments", Json::array())) {
		PositionAssessment item;
		item.positionId = entry.value("positionId", std::string());
		item.topic = entry.value("topic", std::string());
		item.assessment = entry.value("assessment", std::string());
		item.blockIds = entry.value("blockIds", std::vector<std::string>());
		item.explanation = entry.value("explanation", std::string());
		item.check = checkFromString(entry.value("check", std::string()));
		item.checkNote = entry.value("checkNote", std::string());
		review.assessments.push_back(item);
	}
	for (const Json& entry : value.value("changes", Json::array())) {
		ProposedChange change;
		change.id = entry.value("id", std::string());
		change.positionId = entry.value("positionId", std::string());
		change.action = entry.value("action", std::string());
		change.citedClause = entry.value("citedClause", std::string());
		change.blockId = entry.value("blockId", std::string());
		change.clauseNumber = entry.value("clauseNumber", std::string());
		change.currentText = entry.value("currentText", std::string());
		change.proposedText = entry.value("proposedText", std::string());
		change.explanation = entry.value("explanation", std::string());
		change.confidence = entry.value("confidence", std::string());
		change.severity = entry.value("severity", std::string());
		change.check = checkFromString(entry.value("check", std::string()));
		change.checkNote = entry.value("checkNote", std::string());
		change.isWritable = entry.value("isWritable", false);
		change.writeState = entry.value("writeState", std::string("not_written"));
		change.writeNote = entry.value("writeNote", std::string());
		change.readBack = entry.value("readBack", std::string());
		for (const Json& edit : entry.value("edits", Json::array())) {
			change.edits.push_back(TextEdit{edit.value("currentText", std::string()), edit.value("proposedText", std::string())});
		}
		for (const Json& piece : entry.value("redline", Json::array())) {
			change.redline.push_back(RedlinePiece{piece.value("text", std::string()), piece.value("kind", std::string())});
		}
		for (const Json& edit : entry.value("tableEdits", Json::array())) {
			change.tableEdits.push_back(TableEdit{edit.value("kind", std::string()), edit.value("row", static_cast<std::size_t>(0)),
				edit.value("column", static_cast<std::size_t>(0)), edit.value("currentText", std::string()),
				edit.value("proposedText", std::string()), edit.value("cells", std::vector<std::string>())});
		}
		for (const Json& row : entry.value("tableRows", Json::array())) {
			TableRowReading reading;
			reading.cells = row.value("cells", std::vector<std::string>());
			reading.isContinued.assign(reading.cells.size(), false);
			for (const Json& index : row.value("continued", Json::array())) {
				if (index.is_number_unsigned() == true && index.get<std::size_t>() < reading.isContinued.size()) {
					reading.isContinued[index.get<std::size_t>()] = true;
				}
			}
			change.tableRows.push_back(std::move(reading));
		}
		review.changes.push_back(change);
	}
	for (const Json& entry : value.value("unresolved", Json::array())) {
		UnresolvedPoint point;
		point.kind = entry.value("kind", std::string());
		point.description = entry.value("description", std::string());
		point.blockIds = entry.value("blockIds", std::vector<std::string>());
		point.foundBy = entry.value("foundBy", std::string());
		point.isConfirmedByStructure = entry.value("isConfirmedByStructure", false);
		review.unresolved.push_back(point);
	}
	for (const Json& entry : value.value("comments", Json::array())) {
		MarginComment comment;
		comment.blockId = entry.value("blockId", std::string());
		comment.clauseNumber = entry.value("clauseNumber", std::string());
		comment.text = entry.value("text", std::string());
		comment.anchorText = entry.value("anchorText", std::string());
		comment.writeState = entry.value("writeState", std::string("not_written"));
		comment.writeNote = entry.value("writeNote", std::string());
		review.comments.push_back(comment);
	}
	return review;

}

} // namespace singularity::document_intelligence
