#include "PapinianGuard/SemanticRoles/SemanticRoles.hpp"

#include "Context/ControlText/ControlText.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBudget.hpp"
#include "PapinianGuard/SemanticRoles/RoleLexicon.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

// Semantic roles (DESIGN4 §5): what a protected fact IS, read on this computer
// from the words the material writes around it - never from its value - and
// sent only in words the call's source material already carries. Pure: no
// Papinian Guard function is called from here (the link rule of the protected
// computation sources); the caller masks and judges what is sent.
namespace singularity::protected_api::papinian::roles {

namespace {

namespace lx = lexicon_detail;

bool is_space(char character) { return character == ' ' || character == '\t'; }

std::string trimmed(std::string text) {
	const auto strip = [](const std::string &value, std::size_t &from, std::size_t &to) {
		while (from < to && (is_space(value[from]) || value[from] == '*' || value[from] == '-' || value[from] == '#')) ++from;
		while (to > from && (is_space(value[to - 1]) || value[to - 1] == '*' || value[to - 1] == ',')) --to;
	};
	std::size_t from = 0;
	std::size_t to = text.size();
	strip(text, from, to);
	// No-break spaces at either end.
	while (to - from >= 2 && text.compare(from, 2, "\xC2\xA0") == 0) from += 2;
	while (to - from >= 2 && text.compare(to - 2, 2, "\xC2\xA0") == 0) to -= 2;
	return text.substr(from, to - from);
}

bool has_letter(const std::string &text) {
	for (std::size_t at = 0; at < text.size(); ++at) {
		const unsigned char byte = static_cast<unsigned char>(text[at]);
		if (std::isalpha(byte) != 0) return true;
		if ((byte == 0xD0 || byte == 0xD1) && at + 1 < text.size()) return true;
	}
	return false;
}

bool has_digit(const std::string &text) {
	return std::any_of(text.begin(), text.end(), [](char character) { return character >= '0' && character <= '9'; });
}

bool has_cyrillic(std::string_view text) {
	for (const char byte : text)
		if (static_cast<unsigned char>(byte) == 0xD0 || static_cast<unsigned char>(byte) == 0xD1) return true;
	return false;
}

std::size_t word_count(const std::string &text) { return lx::folded_words(text).size(); }

// The line of text[at] as [start, end).
std::pair<std::size_t, std::size_t> line_of(const std::string &text, std::size_t at) {
	const std::size_t start = at == 0 ? 0 : (text.rfind('\n', at - 1) == std::string::npos ? 0 : text.rfind('\n', at - 1) + 1);
	const std::size_t end = text.find('\n', at);
	return {start, end == std::string::npos ? text.size() : end};
}

// The sentence of text[at] as [start, end): bounded by a line, a full stop or
// a semicolon followed by a space.
std::pair<std::size_t, std::size_t> sentence_of(const std::string &text, std::size_t at) {
	const auto [lineStart, lineEnd] = line_of(text, at);
	std::size_t start = lineStart;
	for (std::size_t index = at; index > lineStart; --index) {
		const char previous = text[index - 1];
		if ((previous == '.' || previous == ';') && index < text.size() && text[index] == ' ') {
			// "4.1" or "No. 5" is no sentence's end.
			if (index >= 2 && std::isdigit(static_cast<unsigned char>(text[index - 2])) != 0) continue;
			start = index;
			break;
		}
	}
	std::size_t end = lineEnd;
	for (std::size_t index = at; index + 1 < lineEnd; ++index)
		if ((text[index] == '.' || text[index] == ';') && text[index + 1] == ' '
				&& (index == 0 || std::isdigit(static_cast<unsigned char>(text[index - 1])) == 0)) {
			end = index + 1;
			break;
		}
	return {start, end};
}

// Quote pairs a definition writes a term in.
const std::pair<std::string, std::string> kQuotes[] = {{"\"", "\""}, {"\xE2\x80\x9C", "\xE2\x80\x9D"}, {"\xC2\xAB", "\xC2\xBB"}};

// 1. A defined term that governs the sentence the fact stands in: '"X" means',
// '(the "X")', '(hereinafter "X")', «X» означает, (далее – «X»), под X понимается.
std::optional<std::string> defined_term(const std::string &text, std::size_t start, std::size_t end) {
	const std::string sentence = text.substr(start, end - start);
	const std::string lowered = lx::fold(sentence);
	std::optional<std::string> best;
	for (const auto &[open, close] : kQuotes) {
		for (std::size_t at = sentence.find(open); at != std::string::npos; at = sentence.find(open, at + open.size())) {
			const std::size_t from = at + open.size();
			const std::size_t to = sentence.find(close, from);
			if (to == std::string::npos || to == from || to - from > 60) continue;
			const std::string term = sentence.substr(from, to - from);
			if (!has_letter(term) || term.find('\n') != std::string::npos) continue;
			std::string after = lx::fold(sentence.substr(to + close.size(), 40));
			while (!after.empty() && (after.front() == ' ' || after.front() == ',')) after.erase(0, 1);
			const std::string before = lx::fold(sentence.substr(at > 24 ? at - 24 : 0, at > 24 ? 24 : at));
			const bool means = after.rfind("means", 0) == 0 || after.rfind("shall mean", 0) == 0
				|| after.rfind("has the meaning", 0) == 0 || after.rfind("is defined as", 0) == 0
				|| after.rfind("\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB0\xD0\xB5\xD1\x82", 0) == 0;  // означает
			const bool bracketed = before.find('(') != std::string::npos
				&& (before.find("the") != std::string::npos || before.find("hereinafter") != std::string::npos
					|| before.find("\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xB5") != std::string::npos  // далее
					|| (!before.empty() && before.back() == '('));
			if (means || bracketed) best = term;
		}
	}
	// (далее — X) without quotes.
	const std::string further = "(\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xB5";  // (далее
	const std::size_t opened = lowered.find(further);
	if (!best && opened != std::string::npos) {
		std::size_t cursor = opened + further.size();
		while (cursor < sentence.size() && (sentence[cursor] == ' ' || sentence[cursor] == '-'
				|| sentence.compare(cursor, 3, "\xE2\x80\x94") == 0 || sentence.compare(cursor, 3, "\xE2\x80\x93") == 0))
			cursor += sentence[cursor] == ' ' || sentence[cursor] == '-' ? 1 : 3;
		const std::size_t close = sentence.find(')', cursor);
		if (close != std::string::npos && close > cursor && close - cursor <= 60) best = trimmed(sentence.substr(cursor, close - cursor));
	}
	return best;
}

// 2. A label before a colon or a dash on the fact's line: "Liability Cap:
// [[Amount1]]", «Цена Договора: …», «Итого — …».
std::optional<std::string> colon_label(const std::string &text, std::size_t fact) {
	const auto [lineStart, lineEnd] = line_of(text, fact);
	(void)lineEnd;
	const std::string before = text.substr(lineStart, fact - lineStart);
	std::size_t separator = std::string::npos;
	std::size_t separatorLength = 0;
	for (const char *mark : {":", " \xE2\x80\x94 ", " \xE2\x80\x93 ", " - "}) {
		const std::size_t found = before.rfind(mark);
		if (found != std::string::npos && (separator == std::string::npos || found > separator)) {
			separator = found;
			separatorLength = std::char_traits<char>::length(mark);
		}
	}
	if (separator == std::string::npos) return std::nullopt;
	// Nothing but a currency's mark between the separator and the fact.
	const std::string gap = trimmed(before.substr(separator + separatorLength));
	if (gap.size() > 4 || has_letter(gap)) return std::nullopt;
	std::string label = before.substr(0, separator);
	// A list's bullet or a clause's number before the label.
	std::size_t from = 0;
	while (from < label.size() && (is_space(label[from]) || label[from] == '-' || label[from] == '*'
			|| std::isdigit(static_cast<unsigned char>(label[from])) != 0 || label[from] == '.' || label[from] == ')'))
		++from;
	label = trimmed(label.substr(from));
	// A workbook reading's cell ("B12 =") or a row line is not a label.
	if (label.rfind("Row ", 0) == 0 || label.find(" = ") != std::string::npos || label.find('|') != std::string::npos)
		return std::nullopt;
	if (label.empty() || label.size() > 60 || !has_letter(label) || word_count(label) > 8) return std::nullopt;
	return label;
}

// The cells of a workbook reading line: "Row 12 (hidden): A12 = July 2026 |
// B12 = 3000 (shown in $)" -> {A12, "July 2026", start}, {B12, "3000 ...", start}.
struct ReadCell {
	std::string ref;
	std::string letters;
	std::string text;
	std::size_t start = 0;
	std::size_t end = 0;
};

std::vector<ReadCell> reading_cells(const std::string &text, std::size_t lineStart, std::size_t lineEnd) {
	std::vector<ReadCell> cells;
	const std::string line = text.substr(lineStart, lineEnd - lineStart);
	if (line.rfind("Row ", 0) != 0) return cells;
	std::size_t at = line.find(": ");
	if (at == std::string::npos) return cells;
	at += 2;
	while (at < line.size()) {
		std::size_t next = line.find(" | ", at);
		if (next == std::string::npos) next = line.size();
		const std::string cell = line.substr(at, next - at);
		const std::size_t equals = cell.find(" = ");
		if (equals != std::string::npos) {
			ReadCell read;
			read.ref = cell.substr(0, equals);
			for (const char character : read.ref)
				if (character >= 'A' && character <= 'Z') read.letters.push_back(character);
			std::string value = cell.substr(equals + 3);
			const std::size_t note = value.find(" (");
			const std::size_t formula = value.find(" {=");
			value = trimmed(value.substr(0, std::min(note, formula)));
			read.text = value;
			read.start = lineStart + at;
			read.end = lineStart + next;
			cells.push_back(std::move(read));
		}
		at = next + 3;
	}
	return cells;
}

// 3. A workbook cell in a reading: its column's header and its row's label.
std::optional<std::pair<std::string, std::string>> reading_role(const std::string &text, std::size_t fact) {
	const auto [lineStart, lineEnd] = line_of(text, fact);
	const std::vector<ReadCell> row = reading_cells(text, lineStart, lineEnd);
	const ReadCell *own = nullptr;
	for (const ReadCell &cell : row)
		if (fact >= cell.start && fact < cell.end) own = &cell;
	if (own == nullptr) return std::nullopt;
	std::string rowLabel;
	for (const ReadCell &cell : row)
		if (&cell != own && !cell.text.empty() && has_letter(cell.text) && cell.text.rfind("[[", 0) != 0) {
			rowLabel = cell.text;
			break;
		}
	// The header row: the first reading line before it whose cells are all words.
	std::string header;
	for (std::size_t cursor = 0; cursor < lineStart;) {
		const std::size_t end = std::min(text.find('\n', cursor), lineStart);
		const std::vector<ReadCell> cells = reading_cells(text, cursor, end == std::string::npos ? lineStart : end);
		bool words = cells.size() >= 2;
		for (const ReadCell &cell : cells) words = words && (cell.text.empty() || (has_letter(cell.text) && !has_digit(cell.text)));
		if (words) {
			for (const ReadCell &cell : cells)
				if (cell.letters == own->letters) header = cell.text;
			break;
		}
		if (end == std::string::npos || end >= lineStart) break;
		cursor = end + 1;
	}
	if (header.empty()) return std::nullopt;
	return std::make_pair(rowLabel.empty() ? header : header + " @ " + rowLabel, own->ref);
}

// 4. A cell of a Word or Markdown table (tab- or pipe-separated): its
// column's header, from the table's first line.
std::optional<std::string> table_header(const std::string &text, std::size_t fact) {
	const auto [lineStart, lineEnd] = line_of(text, fact);
	const std::string line = text.substr(lineStart, lineEnd - lineStart);
	const char separator = line.find('\t') != std::string::npos ? '\t' : line.find('|') != std::string::npos ? '|' : '\0';
	if (separator == '\0' || line.rfind("Row ", 0) == 0) return std::nullopt;
	std::size_t column = 0;
	for (std::size_t at = lineStart; at < fact; ++at)
		if (text[at] == separator) ++column;
	// The first line of the block of lines that carry the separator.
	std::size_t first = lineStart;
	while (first > 0) {
		const auto [previousStart, previousEnd] = line_of(text, first - 1);
		if (text.substr(previousStart, previousEnd - previousStart).find(separator) == std::string::npos) break;
		first = previousStart;
	}
	if (first == lineStart) return std::nullopt;
	const auto [headerStart, headerEnd] = line_of(text, first);
	std::vector<std::string> cells;
	std::string cell;
	for (std::size_t at = headerStart; at <= headerEnd; ++at) {
		if (at == headerEnd || text[at] == separator) {
			cells.push_back(trimmed(cell));
			cell.clear();
		} else {
			cell.push_back(text[at]);
		}
	}
	if (column >= cells.size()) return std::nullopt;
	const std::string header = cells[column];
	if (header.empty() || !has_letter(header) || has_digit(header) || header.size() > 60) return std::nullopt;
	return header;
}

// 5. The caption of the numbered clause the fact stands in: "4.2 Annual
// Escalation." -> {"Annual Escalation", "4.2"}; «7.1. Ответственность сторон».
std::optional<std::pair<std::string, std::string>> clause_caption(const std::string &text, std::size_t fact) {
	std::size_t cursor = line_of(text, fact).first;
	for (int lines = 0; lines < 40; ++lines) {
		const auto [start, end] = line_of(text, cursor);
		std::size_t at = start;
		while (at < end && is_space(text[at])) ++at;
		std::string number;
		while (at < end && (std::isdigit(static_cast<unsigned char>(text[at])) != 0 || text[at] == '.')) number.push_back(text[at++]);
		while (!number.empty() && number.back() == '.') number.pop_back();
		if (!number.empty() && at < end && is_space(text[at])) {
			while (at < end && is_space(text[at])) ++at;
			const unsigned char lead = static_cast<unsigned char>(text[at]);
			const bool capital = (lead >= 'A' && lead <= 'Z')
				|| (lead == 0xD0 && at + 1 < end && static_cast<unsigned char>(text[at + 1]) >= 0x90
					&& static_cast<unsigned char>(text[at + 1]) <= 0xAF);
			if (capital) {
				std::size_t stop = text.find('.', at);
				if (stop == std::string::npos || stop > end) stop = end;
				const std::string caption = trimmed(text.substr(at, stop - at));
				if (!caption.empty() && caption.size() <= 60 && word_count(caption) <= 8 && !has_digit(caption))
					return std::make_pair(caption, number);
			}
		}
		if (start == 0) break;
		cursor = start - 1;
	}
	return std::nullopt;
}

// 6. The noun phrase a closed connector joins to the fact: "a late fee of",
// "a cap equal to", «неустойка в размере», «стоимость составляет».
std::optional<std::string> noun_phrase(const std::string &text, std::size_t fact) {
	const auto [lineStart, lineEnd] = line_of(text, fact);
	(void)lineEnd;
	const std::size_t from = fact > lineStart + 160 ? fact - 160 : lineStart;
	const std::string before = text.substr(from, fact - from);
	const std::string folded = lx::fold(before);
	static const std::vector<std::string> connectors{" in the amount of ", " of not less than ", " not exceeding ",
		" equal to ", " of up to ", " of ", " \xD0\xB2 \xD1\x80\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB5 ",  // в размере
		" \xD0\xB2 \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC\xD0\xB5 ",                                                      // в сумме
		" \xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5 ",                                              // не более
		" \xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5 ",                                              // не менее
		" \xD1\x81\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82 "};                      // составляет
	for (const std::string &connector : connectors) {
		const std::size_t found = folded.rfind(connector);
		if (found == std::string::npos) continue;
		// The fact follows the connector: at most a currency's mark between.
		const std::string gap = trimmed(before.substr(found + connector.size()));
		if (gap.size() > 4 || has_letter(gap)) continue;
		// Up to four words before it, back to punctuation.
		std::string head = before.substr(0, found);
		const std::size_t stop = head.find_last_of(".,;:()\n");
		if (stop != std::string::npos) head = head.substr(stop + 1);
		std::vector<std::string> words;
		std::string word;
		for (std::size_t at = 0; at <= head.size(); ++at) {
			if (at == head.size() || head[at] == ' ') {
				if (!word.empty()) words.push_back(word);
				word.clear();
			} else {
				word.push_back(head[at]);
			}
		}
		// Back from the connector to a determiner, a verb or four words: "shall
		// pay a late fee of" -> "late fee"; «уплачивает неустойку в размере» ->
		// «неустойку».
		static const std::set<std::string> determiners{"the", "a", "an", "any", "such", "this", "that", "each", "its", "their",
			"shall", "will", "must", "may", "pay", "pays", "is", "are", "be"};
		const auto verb = [](const std::string &folded) {
			if (!has_cyrillic(folded) || folded.size() < 8) return false;
			for (const char *ending : {"\xD0\xB5\xD1\x82", "\xD0\xB8\xD1\x82", "\xD1\x8E\xD1\x82", "\xD1\x8F\xD1\x82"})  // ет ит ют ят
				if (folded.compare(folded.size() - 4, 4, ending) == 0) return true;
			return false;
		};
		std::size_t first = words.size();
		while (first > 0 && words.size() - first < 4) {
			const std::string folded = lx::fold(words[first - 1]);
			if (determiners.count(folded) > 0 || verb(folded)) break;
			--first;
		}
		words.erase(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(first));
		std::string phrase;
		for (const std::string &each : words) phrase += (phrase.empty() ? "" : " ") + each;
		phrase = trimmed(phrase);
		if (phrase.empty() || !has_letter(phrase) || has_digit(phrase) || phrase.find("[[") != std::string::npos) continue;
		return phrase;
	}
	return std::nullopt;
}

bool quantity_row(const RequestMappingEntry &row) { return row.value.has_value() && !row.fact_key.empty(); }

// A role's words, and its class read from them.
RoleLabel label_of(std::string words, RoleLabel::Source source, std::string location, std::size_t passage, std::size_t offset,
		std::size_t length, semantic::Language language) {
	RoleLabel label;
	label.role_class = role_class_of(words, language);
	label.words = std::move(words);
	label.source = source;
	label.location = std::move(location);
	label.passage = passage;
	label.offset = offset;
	label.length = length;
	return label;
}

// A cell reference or range alone ("B12", "B4:B18"), with any sheet name in
// front of it dropped; "" for anything else.
std::string bare_reference(std::string_view ref) {
	const std::size_t bang = ref.rfind('!');
	if (bang != std::string_view::npos) ref.remove_prefix(bang + 1);
	const auto cell = [](std::string_view text) {
		std::size_t at = 0;
		while (at < text.size() && at < 3 && text[at] >= 'A' && text[at] <= 'Z') ++at;
		const std::size_t letters = at;
		while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0) ++at;
		return letters > 0 && at > letters && at - letters <= 7 && at == text.size();
	};
	const std::size_t colon = ref.find(':');
	if (colon == std::string_view::npos ? !cell(ref) : !cell(ref.substr(0, colon)) || !cell(ref.substr(colon + 1))) return {};
	return std::string(ref);
}

// "DOC_n", or "" when a document is not known by Singularity's own handle.
std::string document_handle(const std::string &document) {
	if (document.size() <= 4 || document.rfind("DOC_", 0) != 0) return {};
	for (std::size_t at = 4; at < document.size(); ++at)
		if (std::isdigit(static_cast<unsigned char>(document[at])) == 0) return {};
	return document;
}

std::string place(const std::string &document, const std::string &within) {
	const std::string handle = document_handle(document);
	if (handle.empty()) return {};
	return within.empty() ? handle : handle + " " + within;
}

std::string document_of(const std::string &location) {
	const std::size_t space = location.find(' ');
	return location.substr(0, space);
}

} // namespace

std::string role_class_of(std::string_view words, semantic::Language) {
	const std::vector<std::string> folded = lx::folded_words(words);
	if (folded.empty()) return {};
	const LexiconPhrase *best = nullptr;
	for (const LexiconPhrase &phrase : role_lexicon().phrases) {
		if (phrase.role_class == "VOCAB" || phrase.stems.size() > folded.size()) continue;
		bool found = false;
		for (std::size_t at = 0; at + phrase.stems.size() <= folded.size() && !found; ++at) {
			bool all = true;
			for (std::size_t index = 0; index < phrase.stems.size() && all; ++index)
				all = word_of_stem(folded[at + index], phrase.stems[index]);
			found = all;
		}
		if (!found) continue;
		if (best == nullptr || phrase.stems.size() > best->stems.size()) best = &phrase;
	}
	return best == nullptr ? std::string() : best->role_class;
}

std::map<std::string, RoleLabel> assign_roles(const std::vector<singularity::context::business_calendar::RulePassage> &raw_passages,
		const std::vector<RequestMappingEntry> &rows, const std::vector<tables::ProtectedTable> &tables,
		semantic::Language language) {
	std::map<std::string, RoleLabel> roles;
	std::map<std::string, std::size_t> passageBySha;
	for (std::size_t index = 0; index < raw_passages.size(); ++index) passageBySha.emplace(raw_passages[index].sha256, index);
	// A workbook cell of the local table model: its header @ its row's label.
	for (const tables::ProtectedTable &table : tables)
		for (const tables::TableRow &row : table.rows) {
			if (row.kind != tables::RowKind::Body && row.kind != tables::RowKind::Total && row.kind != tables::RowKind::Subtotal) continue;
			for (const tables::TableCell &cell : row.cells) {
				if (cell.fact_key.empty() || roles.count(cell.fact_key) > 0) continue;
				const tables::TableColumn *column = nullptr;
				for (const tables::TableColumn &each : table.columns)
					if (!cell.ref.empty() && cell.ref.rfind(each.letter, 0) == 0
							&& std::isdigit(static_cast<unsigned char>(cell.ref[each.letter.size()])) != 0)
						column = &each;
				if (column == nullptr || column->header.empty()) continue;
				std::string words = column->header;
				if (!row.label.empty() && !has_digit(row.label)) words += " @ " + row.label;
				RoleLabel label = label_of(words, RoleLabel::Source::WorkbookHeader,
					place(table.document_id, bare_reference(cell.ref)), 0, 0, 0, language);
				if (!column->role_class.empty()) label.role_class = column->role_class;
				else label.role_class = role_class_of(column->header, language);
				roles.emplace(cell.fact_key, std::move(label));
			}
		}
	for (const RequestMappingEntry &row : rows) {
		if (!quantity_row(row) || row.derived || roles.count(row.fact_key) > 0) continue;
		const FactProvenance &where = row.provenance;
		const auto passage = passageBySha.find(where.passage_sha256);
		if (passage == passageBySha.end()) continue;
		const std::string &text = raw_passages[passage->second].text;
		const std::string &document = raw_passages[passage->second].document;
		if (where.offset >= text.size() || where.length == 0 || where.offset + where.length > text.size()) continue;
		const std::size_t fact = where.offset;
		const auto [sentenceStart, sentenceEnd] = sentence_of(text, fact);
		const std::optional<std::pair<std::string, std::string>> caption = clause_caption(text, fact);
		const std::string clauseLocation = place(document, caption ? "\xC2\xA7" + caption->second : std::string());
		const std::size_t index = passage->second;
		// 7. The lawyer's own figure.
		if (where.origin == FactProvenance::Origin::Request) {
			const std::optional<std::string> phrase = noun_phrase(text, fact);
			roles.emplace(row.fact_key, label_of(phrase ? *phrase : std::string(), RoleLabel::Source::Request, std::string(), index,
				fact, where.length, language));
			continue;
		}
		if (const std::optional<std::string> term = defined_term(text, sentenceStart, sentenceEnd)) {
			roles.emplace(row.fact_key, label_of(*term, RoleLabel::Source::DefinedTerm, clauseLocation, index, fact, where.length, language));
			continue;
		}
		if (const std::optional<std::pair<std::string, std::string>> cell = reading_role(text, fact)) {
			roles.emplace(row.fact_key, label_of(cell->first, RoleLabel::Source::WorkbookHeader,
				place(document, bare_reference(where.cell.empty() ? cell->second : where.cell)), index, fact, where.length, language));
			continue;
		}
		if (const std::optional<std::string> label = colon_label(text, fact)) {
			roles.emplace(row.fact_key, label_of(*label, RoleLabel::Source::LabelColon, clauseLocation, index, fact, where.length, language));
			continue;
		}
		if (const std::optional<std::string> header = table_header(text, fact)) {
			roles.emplace(row.fact_key, label_of(*header, RoleLabel::Source::TableHeader, place(document, std::string()), index, fact,
				where.length, language));
			continue;
		}
		if (caption) {
			// A caption names the clause; a noun phrase inside it names the figure
			// more closely, and wins when it names a class.
			const std::optional<std::string> phrase = noun_phrase(text, fact);
			if (phrase && !role_class_of(*phrase, language).empty()) {
				roles.emplace(row.fact_key, label_of(*phrase, RoleLabel::Source::NounPhrase, clauseLocation, index, fact, where.length, language));
				continue;
			}
			roles.emplace(row.fact_key, label_of(caption->first, RoleLabel::Source::ClauseCaption, clauseLocation, index, fact,
				where.length, language));
			continue;
		}
		if (const std::optional<std::string> phrase = noun_phrase(text, fact))
			roles.emplace(row.fact_key, label_of(*phrase, RoleLabel::Source::NounPhrase, clauseLocation, index, fact, where.length, language));
	}
	return roles;
}

std::optional<compute::ArgType> repair_type(const RequestMappingEntry &row, const RoleLabel &label) {
	if (!row.value) return std::nullopt;
	const semantic::Value &value = *row.value;
	const std::vector<std::string> words = lx::folded_words(label.words);
	const auto names = [&](std::initializer_list<const char *> stems) {
		for (const std::string &word : words)
			for (const char *stem : stems)
				if (word_of_stem(word, stem)) return true;
		return false;
	};
	const bool unitless = value.kind == semantic::Kind::Number
		|| (value.kind == semantic::Kind::Money && value.currency.empty());
	if (value.kind == semantic::Kind::Money || value.kind == semantic::Kind::Number) {
		if (!unitless && value.kind == semantic::Kind::Money && !value.currency.empty()) {
			// A figure with its currency written is money, whatever its label says.
			return std::nullopt;
		}
		const bool days = names({"day", "days", "\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9", "\xD0\xB4\xD0\xBD\xD1\x8F", "\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C"});
		const bool months = names({"month", "months", "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86"});
		if (days || months) {
			compute::ArgType type;
			type.kind = compute::TypeKind::Duration;
			type.unit = months && !days ? semantic::DurationUnit::Month : semantic::DurationUnit::Day;
			return type;
		}
		if (label.role_class == "COUNT") {
			compute::ArgType type;
			type.kind = compute::TypeKind::Count;
			return type;
		}
		return std::nullopt;
	}
	if (value.kind == semantic::Kind::Percentage && value.basis != semantic::RateBasis::None
			&& (label.role_class == "LIMIT" || label.role_class == "THRESHOLD")) {
		compute::ArgType type;
		type.kind = compute::TypeKind::Percentage;
		return type;
	}
	return std::nullopt;
}

bool sendable(std::string_view label, const std::vector<std::string> &source_spans) {
	return sendable_in_words(label, spans_words(source_spans));
}

std::unordered_set<std::string> spans_words(const std::vector<std::string> &source_spans) {
	std::unordered_set<std::string> available;
	for (const std::string &span : source_spans)
		for (std::string &word : lx::folded_words(span)) available.insert(std::move(word));
	return available;
}

bool sendable_in_words(std::string_view label, const std::unordered_set<std::string> &spans_words) {
	const std::vector<std::string> words = lx::folded_words(label);
	if (words.empty()) return false;
	// A symbol in a label ("Payment to [[Org4]]") is folded like a word: it
	// stands in the protected spans exactly as it stands here.
	for (const std::string &word : words)
		if (spans_words.count(word) == 0) return false;
	return true;
}

std::string type_of_row(const RequestMappingEntry &row, const std::optional<RoleLabel> &role) {
	compute::ArgType type;
	if (row.value) {
		if (role)
			if (const std::optional<compute::ArgType> repaired = repair_type(row, *role)) return compute::written(*repaired);
		const semantic::Value &value = *row.value;
		switch (value.kind) {
			case semantic::Kind::Money:
				type.kind = compute::TypeKind::Money;
				type.currency = value.currency;
				type.basis = value.basis;
				break;
			case semantic::Kind::Percentage:
				type.kind = value.basis == semantic::RateBasis::None ? compute::TypeKind::Percentage : compute::TypeKind::Rate;
				type.basis = value.basis;
				break;
			case semantic::Kind::Date:
				type.kind = compute::TypeKind::Date;
				switch (value.date.precision) {
					case singularity::context::temporal::Precision::Day: type.precision = "day"; break;
					case singularity::context::temporal::Precision::Month: type.precision = "month"; break;
					case singularity::context::temporal::Precision::Quarter: type.precision = "quarter"; break;
					case singularity::context::temporal::Precision::Year: type.precision = "year"; break;
				}
				break;
			case semantic::Kind::DayOfYear:
				type.kind = compute::TypeKind::Date;
				type.precision = "day of year";
				break;
			case semantic::Kind::Duration:
				type.kind = compute::TypeKind::Duration;
				type.unit = value.unit;
				break;
			case semantic::Kind::Boolean: type.kind = compute::TypeKind::Boolean; break;
			case semantic::Kind::Count: type.kind = compute::TypeKind::Count; break;
			case semantic::Kind::Number: type.kind = compute::TypeKind::Number; break;
		}
		return compute::written(type);
	}
	type.kind = compute::TypeKind::Entity;
	if (row.type) {
		switch (*row.type) {
			case EntityType::Person:
			case EntityType::Judge: type.entity = "person"; break;
			case EntityType::Organisation:
			case EntityType::LegalEntity:
			case EntityType::Court: type.entity = "organisation"; break;
			case EntityType::Location:
			case EntityType::Address: type.entity = "place"; break;
			default: type.entity = "identifier"; break;
		}
	}
	return compute::written(type);
}

std::string quoted_label(std::string_view words) {
	if (has_cyrillic(words)) return "\xC2\xAB" + std::string(words) + "\xC2\xBB";
	return "\"" + std::string(words) + "\"";
}

std::string symbol_line(const std::string &symbol, const std::string &type, const std::string &role_class,
		const std::string &words, const std::string &location, bool request_figure, semantic::Language language) {
	std::string line = symbol + " " + type;
	if (!role_class.empty()) line += " " + role_class;
	if (request_figure) {
		line += language == semantic::Language::Russian ? " \xC2\xAB\xD0\xB2\xD0\xB0\xD1\x88\xD0\xB0 \xD1\x86\xD0\xB8\xD1\x84\xD1\x80\xD0\xB0\xC2\xBB"  // «ваша цифра»
			: " \"your figure\"";
		if (!words.empty()) line += " " + quoted_label(words);
		return line;
	}
	if (!words.empty()) {
		line += " " + quoted_label(words);
		if (!location.empty()) line += " \xC2\xB7 " + location;
	}
	return line;
}

std::vector<CrossCheck> cross_checks(const std::map<std::string, RoleLabel> &roles,
		const std::vector<tables::ProtectedTable> &tables, const OperationProtectedSymbolTable &table) {
	static const std::set<std::string> comparable{"BILLED", "PAID", "PRICE", "PENALTY", "INTEREST", "LIMIT", "THRESHOLD",
		"BALANCE", "TOTAL", "ADJUSTMENT"};
	struct Side {
		std::string name;
		std::string role_class;
		std::string words;
		std::string location;
		std::string document;
		bool column = false;
		std::set<std::string> stems;
	};
	const auto stemsOf = [](const std::string &words) {
		std::set<std::string> stems;
		for (const std::string &word : lx::folded_words(words))
			if (word.size() >= 4) stems.insert(word.substr(0, std::min<std::size_t>(word.size(), 10)));
		return stems;
	};
	std::vector<Side> sides;
	for (const tables::ProtectedTable &each : tables) {
		if (!each.described) continue;
		for (const tables::TableColumn &column : each.columns) {
			if (column.role_class.empty() || comparable.count(column.role_class) == 0 || column.header.empty()) continue;
			if (!each.described_columns.empty()
					&& std::find(each.described_columns.begin(), each.described_columns.end(), column.letter) == each.described_columns.end())
				continue;
			sides.push_back({"[[" + each.handle + "]]." + column.letter, column.role_class, column.header,
				each.document_id, each.document_id, true, stemsOf(column.header)});
		}
	}
	// Facts in the order their symbols were issued (reading order).
	std::vector<std::pair<std::size_t, const RoleLabel *>> facts;
	for (const auto &[key, label] : roles) {
		if (label.role_class.empty() || comparable.count(label.role_class) == 0 || label.words.empty()) continue;
		const auto row = table.fact_by_key.find(key);
		if (row == table.fact_by_key.end() || row->second >= table.mapping.size()) continue;
		// A table cell is the column's side already.
		if (label.source == RoleLabel::Source::WorkbookHeader) continue;
		facts.emplace_back(row->second, &label);
	}
	std::sort(facts.begin(), facts.end());
	for (const auto &[row, label] : facts)
		sides.push_back({table.mapping[row].placeholder, label->role_class, label->words, label->location,
			document_of(label->location), false, stemsOf(label->words)});
	std::vector<CrossCheck> checks;
	std::set<std::pair<std::string, std::string>> paired;
	for (std::size_t first = 0; first < sides.size() && checks.size() < compute::kCrossCheckLines; ++first)
		for (std::size_t second = first + 1; second < sides.size() && checks.size() < compute::kCrossCheckLines; ++second) {
			const Side &a = sides[first];
			const Side &b = sides[second];
			if (a.role_class != b.role_class || a.document == b.document || a.document.empty() || b.document.empty()) continue;
			// Two facts pair only when their labels share a word; a column pairs
			// with any fact of its class.
			if (!a.column && !b.column) {
				bool shared = false;
				for (const std::string &stem : a.stems) shared = shared || b.stems.count(stem) > 0;
				if (!shared) continue;
			}
			if (a.column && b.column) continue;
			if (!paired.insert({a.name, b.name}).second) continue;
			CrossCheck check;
			check.role_class = a.role_class;
			check.sides = {a.name, b.name};
			check.labels = {a.words, b.words};
			check.line = a.name + " " + a.role_class + " " + quoted_label(a.words) + (a.column ? "" : " " + a.location) + " \xC2\xB7 " + b.name
				+ " " + b.role_class + " " + quoted_label(b.words) + (b.column ? "" : " " + b.location);
			checks.push_back(std::move(check));
		}
	return checks;
}

std::vector<std::string> table_block(const std::vector<tables::ProtectedTable> &tables,
		const std::function<std::string(const std::string &)> &label) {
	std::vector<std::string> lines;
	std::map<std::string, std::size_t> undescribed;
	std::vector<std::string> documents;
	const auto join = [](const std::vector<std::size_t> &rows) {
		std::string text;
		for (std::size_t index = 0; index < rows.size(); ++index) {
			std::size_t end = index;
			while (end + 1 < rows.size() && rows[end + 1] == rows[end] + 1) ++end;
			text += (text.empty() ? "" : ", ") + std::to_string(rows[index]);
			if (end > index) text += "\xE2\x80\x93" + std::to_string(rows[end]);
			index = end;
		}
		return text;
	};
	for (const tables::ProtectedTable &table : tables) {
		if (!table.described) {
			const std::string &document = table.document_handle.empty() ? table.document_id : table.document_handle;
			if (undescribed[document]++ == 0) documents.push_back(document);
			continue;
		}
		std::string head = "[[" + table.handle + "]]";
		const std::string title = table.title.empty() ? std::string() : label(table.title);
		if (!title.empty()) head += " " + quoted_label(title);
		// The document as the prompt names it ("DOC_4"), so the model ties the
		// table to its source; the internal id only when no handle is known.
		head += " (" + (table.document_handle.empty() ? table.document_id : table.document_handle);
		const std::string sheet = table.sheet.empty() ? std::string() : label(table.sheet);
		if (!sheet.empty()) head += ", sheet " + quoted_label(sheet);
		if (table.sheet_hidden) head += ", hidden sheet";
		head += ")";
		std::vector<std::size_t> body;
		std::vector<std::size_t> hidden;
		for (const tables::TableRow &row : table.rows) {
			if (row.kind != tables::RowKind::Body) continue;
			body.push_back(row.number);
			if (row.hidden) hidden.push_back(row.number);
		}
		if (table.header_row > 0) head += ": header row " + std::to_string(table.header_row);
		if (!body.empty()) {
			head += (table.header_row > 0 ? "; " : ": ") + std::string("body rows ") + join(body);
			if (!hidden.empty()) head += " (hidden: " + join(hidden) + ")";
		}
		for (const auto &[row, summary] : table.total_rows) {
			(void)summary;
			head += "; total row " + std::to_string(row);
		}
		lines.push_back(head);
		std::string columns;
		for (const tables::TableColumn &column : table.columns) {
			if (!table.described_columns.empty()
					&& std::find(table.described_columns.begin(), table.described_columns.end(), column.letter) == table.described_columns.end())
				continue;
			std::string item = column.letter;
			const std::string header = column.header.empty() ? std::string() : label(column.header);
			if (!header.empty()) item += " " + quoted_label(header);
			if (!column.type.empty()) item += " " + column.type;
			if (!column.role_class.empty()) item += " " + column.role_class;
			if (column.hidden) item += " (hidden column)";
			if (column.formula && column.formula->consistent && !column.formula->inputs.empty()) {
				const char *mark = column.formula->op == "subtract" ? " \xE2\x88\x92 " : column.formula->op == "multiply" ? " \xC3\x97 "
					: column.formula->op == "divide" ? " / " : " + ";
				std::string formula;
				for (const std::string &input : column.formula->inputs) formula += (formula.empty() ? "" : mark) + input;
				item += " = " + formula + " (formula)";
			}
			columns += (columns.empty() ? "  " : " \xC2\xB7 ") + item;
		}
		if (!columns.empty()) lines.push_back(columns);
		for (const auto &[row, summary] : table.total_rows)
			if (!summary.empty()) lines.push_back("  row " + std::to_string(row) + " = " + summary);
		// The example is one a model may take literally: a cell that holds a
		// figure (a fake-provider run named the example D14 of an empty column
		// and was told "that cell holds no figure").
		std::string example;
		for (auto row = table.rows.rbegin(); row != table.rows.rend() && example.empty(); ++row) {
			if (row->kind != tables::RowKind::Body) continue;
			for (auto cell = row->cells.rbegin(); cell != row->cells.rend(); ++cell)
				if (cell->value && !cell->ref.empty()
						&& (table.described_columns.empty() || std::any_of(table.described_columns.begin(),
							table.described_columns.end(), [&](const std::string &letter) {
								return cell->ref.rfind(letter, 0) == 0 && cell->ref.size() > letter.size()
									&& std::isdigit(static_cast<unsigned char>(cell->ref[letter.size()]));
							}))) {
					example = cell->ref;
					break;
				}
		}
		// A table no cell of which holds a figure (a Word table of terms and
		// positions) has no cell to read: no example a model would try (the
		// fake-provider run of 28 Sep 2026 read B4 of such a table in every task
		// and was told "that cell holds no figure").
		if (!example.empty()) lines.push_back("  cells by reference: [[" + table.handle + "]]." + example);
	}
	for (const std::string &document : documents) {
		try {
			std::string line = context::control_text::render(context::control_text::Template::TablesMoreSheets,
				{{context::control_text::Slot::Kind::Handle, document},
				 {context::control_text::Slot::Kind::Count, std::to_string(undescribed[document])},
				 {context::control_text::Slot::Kind::Handle, document}});
			while (!line.empty() && line.back() == '\n') line.pop_back();
			lines.push_back(line);
		} catch (const std::exception &) {
		}
	}
	return lines;
}

} // namespace singularity::protected_api::papinian::roles
