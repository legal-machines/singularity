#include "PapinianGuard/ProtectedRelations/ProtectedRelations.hpp"

#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

namespace singularity::protected_api::papinian::relations {
namespace {

namespace tables = singularity::protected_api::papinian::tables;

namespace temporal = singularity::context::temporal;
using VKind = semantic::Kind;
using semantic::Rational;
using semantic::Value;

// ---------------------------------------------------------------------------
// Reading the protected text

// ASCII and Cyrillic letters in lower case, byte for byte otherwise.
std::string fold(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(text[index]);
		if (byte < 0x80) {
			result += static_cast<char>(std::tolower(byte));
			continue;
		}
		if (index + 1 < text.size()) {
			const unsigned char next = static_cast<unsigned char>(text[index + 1]);
			if (byte == 0xD0 && next >= 0x90 && next <= 0x9F) {  // А-П
				result += static_cast<char>(0xD0);
				result += static_cast<char>(next + 0x20);
				++index;
				continue;
			}
			if (byte == 0xD0 && next >= 0xA0 && next <= 0xAF) {  // Р-Я
				result += static_cast<char>(0xD1);
				result += static_cast<char>(next - 0x20);
				++index;
				continue;
			}
			if (byte == 0xD0 && next == 0x81) {  // Ё
				result += "\xD1\x91";
				++index;
				continue;
			}
		}
		result += static_cast<char>(byte);
	}
	return result;
}

bool contains_any(const std::string &folded, std::initializer_list<const char *> words) {
	return std::any_of(words.begin(), words.end(), [&](const char *word) { return folded.find(word) != std::string::npos; });
}

// Whether a letter - ASCII or Cyrillic - is written at `position`, or ends right before it.
bool letter_at(const std::string &text, std::size_t position) {
	if (position >= text.size()) return false;
	const unsigned char byte = static_cast<unsigned char>(text[position]);
	return std::isalpha(byte) != 0 || byte == 0xD0 || byte == 0xD1;
}

bool letter_before(const std::string &text, std::size_t position) {
	if (position == 0) return false;
	const unsigned char byte = static_cast<unsigned char>(text[position - 1]);
	if (byte < 0x80) return std::isalpha(byte) != 0;
	return position >= 2 && (byte & 0xC0) == 0x80
		&& (static_cast<unsigned char>(text[position - 2]) == 0xD0 || static_cast<unsigned char>(text[position - 2]) == 0xD1);
}

// Where `word` stands in `folded` as words stand - "rent" in "Base Rent", never
// in "current" or "Parent"; a stem written "цен*" in «цена», «цены», never in
// «процент». An entry ending in '*' is a stem any ending may follow; any other
// is the whole word (or phrase), or it with a plural "s"/"es".
std::size_t word_position(const std::string &folded, std::string_view word) {
	const bool stem = !word.empty() && word.back() == '*';
	const std::string_view body = stem ? word.substr(0, word.size() - 1) : word;
	if (body.empty()) return std::string::npos;
	for (std::size_t at = folded.find(body); at != std::string::npos; at = folded.find(body, at + 1)) {
		if (letter_before(folded, at)) continue;
		std::size_t end = at + body.size();
		if (stem) return at;
		if (!letter_at(folded, end)) return at;
		if (folded.compare(end, 1, "s") == 0 && !letter_at(folded, end + 1)) return at;
		if (folded.compare(end, 2, "es") == 0 && !letter_at(folded, end + 2)) return at;
	}
	return std::string::npos;
}

bool has_word(const std::string &folded, std::string_view word) { return word_position(folded, word) != std::string::npos; }

bool any_whole_word(const std::string &folded, std::initializer_list<std::string_view> words) {
	return std::any_of(words.begin(), words.end(), [&](std::string_view word) { return has_word(folded, word); });
}

// At most `bytes` of `text`, never cutting a letter in two.
std::string utf8_prefix(const std::string &text, std::size_t bytes) {
	if (text.size() <= bytes) return text;
	std::size_t end = bytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
	return text.substr(0, end);
}

// A symbol written in the text, and the row it stands for.
struct Marker {
	std::size_t start = 0;
	std::size_t end = 0;
	std::size_t row = 0;
	std::string text;
};

struct Rows {
	const std::vector<RequestMappingEntry> *mapping = nullptr;
	std::map<std::string, std::size_t> by_placeholder;

	const RequestMappingEntry &operator[](std::size_t row) const { return (*mapping)[row]; }
	[[nodiscard]] const std::string &symbol(std::size_t row) const { return (*mapping)[row].placeholder; }
	[[nodiscard]] const Value *value(std::size_t row) const {
		const RequestMappingEntry &entry = (*mapping)[row];
		return entry.value ? &*entry.value : nullptr;
	}
	[[nodiscard]] bool is(std::size_t row, VKind kind) const {
		const Value *held = value(row);
		return held != nullptr && held->kind == kind;
	}
	[[nodiscard]] bool is_day(std::size_t row) const {
		const Value *held = value(row);
		return held != nullptr && held->kind == VKind::Date && held->date.precision == temporal::Precision::Day;
	}
	// A day, or a month read from its first day: a monthly ledger's row ("Jul
	// 2026") is billed for the month that day opens (live E1, 26 Sep 2026).
	[[nodiscard]] bool is_day_or_month(std::size_t row) const {
		const Value *held = value(row);
		return held != nullptr && held->kind == VKind::Date
			&& (held->date.precision == temporal::Precision::Day || held->date.precision == temporal::Precision::Month);
	}
	[[nodiscard]] bool is_party(std::size_t row) const {
		const auto &type = (*mapping)[row].type;
		return type && (*type == EntityType::Person || *type == EntityType::Organisation || *type == EntityType::LegalEntity
			|| *type == EntityType::Judge || *type == EntityType::Court);
	}
	[[nodiscard]] bool is_contact(std::size_t row) const {
		const auto &type = (*mapping)[row].type;
		return type && (*type == EntityType::Email || *type == EntityType::Phone || *type == EntityType::Address
			|| *type == EntityType::BankAccount);
	}
};

std::vector<Marker> markers_in(const std::string &text, const Rows &rows) {
	std::vector<Marker> found;
	for (std::size_t open = text.find("[["); open != std::string::npos; open = text.find("[[", open + 2)) {
		const std::size_t close = text.find("]]", open + 2);
		if (close == std::string::npos || close - open > 48) continue;
		std::string inner = text.substr(open + 2, close - open - 2);
		const std::size_t dot = inner.find('.');
		const std::string base = "[[" + (dot == std::string::npos ? inner : inner.substr(0, dot)) + "]]";
		const auto row = rows.by_placeholder.find(base);
		if (row == rows.by_placeholder.end()) continue;
		found.push_back({open, close + 2, row->second, text.substr(open, close + 2 - open)});
	}
	return found;
}

// A line of a passage, split at sentence ends where a new sentence plainly
// begins: ". " before a capital, after a word longer than an abbreviation.
struct Sentence {
	std::size_t passage = 0;
	std::size_t line = 0;
	std::string text;
	std::string folded;
	std::vector<Marker> markers;
	bool rule = false;
	bool cells = false;
};

bool starts_capital(const std::string &text, std::size_t position) {
	if (position >= text.size()) return false;
	const unsigned char byte = static_cast<unsigned char>(text[position]);
	if (byte >= 'A' && byte <= 'Z') return true;
	// A sentence that begins with a protected name: "... note. [[Org2]] owes ...".
	if (byte == '[' && position + 1 < text.size() && text[position + 1] == '[') return true;
	if (byte == 0xD0 && position + 1 < text.size()) {
		const unsigned char next = static_cast<unsigned char>(text[position + 1]);
		return (next >= 0x90 && next <= 0xAF) || next == 0x81;
	}
	return false;
}

std::vector<std::string> split_sentences(const std::string &line) {
	std::vector<std::string> sentences;
	std::size_t from = 0;
	for (std::size_t stop = line.find(". "); stop != std::string::npos; stop = line.find(". ", stop + 1)) {
		std::size_t wordStart = stop;
		while (wordStart > from && line[wordStart - 1] != ' ') --wordStart;
		if (stop - wordStart <= 3 || !starts_capital(line, stop + 2)) continue;
		sentences.push_back(line.substr(from, stop + 1 - from));
		from = stop + 2;
	}
	if (from < line.size()) sentences.push_back(line.substr(from));
	return sentences;
}

bool reads_as_rule(const std::string &folded) {
	return contains_any(folded, {"within", "no later than", "not later than", "deadline", "notice", "days after",
		"days of", "days before", "business day", "cure", "objection", "expire", "due", "increase", "effective",
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB5",          // не позднее
		"\xD0\xB2 \xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",                  // в течение
		"\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA",                                                  // срок
		"\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9",                                                  // дней
		"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD1\x80\xD0\xBE\xD1\x87",                          // просроч
		"\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB"});                        // уведомл
}

// Content words, cut to a stem of five letters: what a question and a line share.
std::set<std::string> content_stems(const std::string &folded) {
	static const std::set<std::string> ignored{"this", "that", "with", "what", "which", "from", "have", "does", "under",
		"there", "their", "about", "would", "should", "shall", "these", "those", "into", "than", "then", "when", "where"};
	std::set<std::string> stems;
	std::size_t index = 0;
	while (index < folded.size()) {
		while (index < folded.size() && !(std::isalpha(static_cast<unsigned char>(folded[index])) || static_cast<unsigned char>(folded[index]) >= 0x80))
			++index;
		std::size_t end = index;
		std::size_t letters = 0;
		std::size_t stemEnd = index;
		while (end < folded.size() && (std::isalpha(static_cast<unsigned char>(folded[end])) || static_cast<unsigned char>(folded[end]) >= 0x80)) {
			const unsigned char byte = static_cast<unsigned char>(folded[end]);
			if ((byte & 0xC0) != 0x80) {
				++letters;
				if (letters <= 5) {
					stemEnd = end + 1;
					while (stemEnd < folded.size() && (static_cast<unsigned char>(folded[stemEnd]) & 0xC0) == 0x80) ++stemEnd;
				}
			}
			++end;
		}
		if (letters >= 4) {
			const std::string word = folded.substr(index, end - index);
			if (ignored.count(word) == 0) stems.insert(folded.substr(index, stemEnd - index));
		}
		index = end;
	}
	return stems;
}

// ---------------------------------------------------------------------------
// Workbook readings: "Row 16: A16 = [[E20]] | B16 = [[E35]] (shown in $) | ..."

struct Cell {
	std::string ref;
	std::string column;
	unsigned row = 0;
	std::string raw;
	std::optional<std::size_t> symbol;
	std::optional<Value> literal;
	std::string note;
	std::string formula;
	[[nodiscard]] bool text() const { return !symbol && !literal && !raw.empty(); }
};

struct SheetRow {
	unsigned number = 0;
	bool hidden = false;
	std::vector<Cell> cells;
	[[nodiscard]] const Cell *at(const std::string &column) const {
		for (const Cell &cell : cells)
			if (cell.column == column) return &cell;
		return nullptr;
	}
	[[nodiscard]] std::string label() const {
		for (const Cell &cell : cells)
			if (cell.text()) return cell.raw;
		return {};
	}
};

struct Sheet {
	std::size_t passage = 0;
	std::vector<SheetRow> rows;
	std::optional<std::size_t> header;
	std::map<std::string, std::string> headers;  // column -> its header, folded
};

// A workbook cell as ProtectedTables reads it, with its symbol or its literal
// value read here against the operation's rows.
Cell cell_of(const tables::PassageCell &read, const Rows &rows) {
	Cell cell;
	cell.column = read.column;
	cell.row = read.row;
	cell.ref = read.ref;
	cell.raw = read.raw;
	cell.note = read.note;
	cell.formula = read.formula;
	const std::string &value = cell.raw;
	const std::vector<Marker> markers = markers_in(value, rows);
	if (markers.size() == 1 && markers.front().start == 0 && markers.front().end == value.size()) {
		cell.symbol = markers.front().row;
	} else if (markers.empty() && !value.empty()
			&& value.find_first_not_of("0123456789.,- ") == std::string::npos) {
		semantic::ParseHints hints;
		hints.currency = semantic::currency_named(cell.note);
		if (!hints.currency.empty()) hints.expect = VKind::Money;
		cell.literal = semantic::parse_value(value, hints);
		if (!cell.literal) cell.literal = semantic::parse_value(value);
	}
	return cell;
}

// The workbook readings of the passages (the grammar lives in ProtectedTables,
// the table model's own reading), with each cell's symbol, and each sheet's
// header: its first row of two or more cells that are all text.
std::vector<Sheet> sheets_in(const std::vector<std::string> &passages, const Rows &rows, const Limits &limits) {
	std::vector<Sheet> sheets;
	for (const tables::PassageSheet &read : tables::passage_sheets(passages, limits.tables, limits.rows_per_table)) {
		Sheet sheet{read.passage, {}, std::nullopt, {}};
		for (const tables::PassageRow &row : read.rows) {
			SheetRow own{row.number, row.hidden, {}};
			for (const tables::PassageCell &cell : row.cells) own.cells.push_back(cell_of(cell, rows));
			sheet.rows.push_back(std::move(own));
		}
		sheets.push_back(std::move(sheet));
	}
	for (Sheet &sheet : sheets) {
		std::sort(sheet.rows.begin(), sheet.rows.end(), [](const SheetRow &a, const SheetRow &b) { return a.number < b.number; });
		for (std::size_t index = 0; index < sheet.rows.size(); ++index) {
			const SheetRow &row = sheet.rows[index];
			if (row.cells.size() < 2) continue;
			if (!std::all_of(row.cells.begin(), row.cells.end(), [](const Cell &cell) { return cell.text(); })) continue;
			sheet.header = index;
			for (const Cell &cell : row.cells) sheet.headers[cell.column] = fold(cell.raw);
			break;
		}
	}
	return sheets;
}

// What a row of a sheet is: one of the records the sheet lists (Body), the
// total of them all (Total), or a row that sums or states something of some of
// them - a subtotal, a balance carried, a conditional sum (Summary), which is
// neither added in nor compared with the total of the rows.
enum class RowKind { Body, Total, Summary };

// The rows an "=SUM(B4:B18)" adds: its first and last row, when it names one range.
std::optional<std::pair<unsigned, unsigned>> sum_range(const std::string &foldedFormula) {
	static const std::regex range("^=sum\\(\\$?[a-z]{1,3}\\$?([0-9]{1,7}):\\$?[a-z]{1,3}\\$?([0-9]{1,7})\\)$");
	std::smatch match;
	if (!std::regex_match(foldedFormula, match, range)) return std::nullopt;
	const unsigned first = static_cast<unsigned>(std::strtoul(match[1].str().c_str(), nullptr, 10));
	const unsigned last = static_cast<unsigned>(std::strtoul(match[2].str().c_str(), nullptr, 10));
	return std::make_pair(std::min(first, last), std::max(first, last));
}

RowKind row_kind(const SheetRow &row) {
	// The row's own label - its first cell - not a note further along it.
	const std::string label = !row.cells.empty() && row.cells.front().text() ? fold(row.cells.front().raw) : std::string();
	// Part of the rows, or a position rather than a sum: "Subtotal", "Q1 subtotal",
	// «Промежуточный итог», "Balance", "Balance carried forward", «Сальдо», «Остаток».
	if (any_whole_word(label, {"subtotal", "subtotals", "sub-total", "balance", "balances", "carried", "brought",
			"\xD0\xBF\xD1\x80\xD0\xBE\xD0\xBC\xD0\xB5\xD0\xB6\xD1\x83\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD*",  // промежуточн*
			"\xD1\x81\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xBE",                                          // сальдо
			"\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD0\xBE\xD0\xBA",                                  // остаток
			"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD\xD0\xBE\xD1\x81*"}))                               // перенос*
		return RowKind::Summary;
	bool sums = false;
	for (const Cell &cell : row.cells) {
		const std::string formula = fold(cell.formula);
		if (formula.empty()) continue;
		// A conditional or partial sum is never the total of the rows.
		if (formula.rfind("=subtotal(", 0) == 0 || formula.rfind("=sumif", 0) == 0 || formula.rfind("=sumproduct(", 0) == 0
				|| formula.rfind("=aggregate(", 0) == 0)
			return RowKind::Summary;
		if (formula.rfind("=sum(", 0) == 0) sums = true;
	}
	// "Total", "Grand total", "Sum", «Итого», «Итог», «Всего».
	if (any_whole_word(label, {"total", "totals", "sum",
			"\xD0\xB8\xD1\x82\xD0\xBE\xD0\xB3*",                  // итог* (итого, итоги)
			"\xD0\xB2\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE"}))          // всего
		return RowKind::Total;
	return sums ? RowKind::Total : RowKind::Body;
}

bool total_row(const SheetRow &row) { return row_kind(row) != RowKind::Body; }

// ---------------------------------------------------------------------------
// Building the output

// The intents each kind of relation answers.
std::set<Intent> serves_of(Kind kind) {
	switch (kind) {
		case Kind::ColumnTotal: return {Intent::Total, Intent::SheetCheck};
		case Kind::RowBalance: return {Intent::Shortfall, Intent::Total, Intent::SheetCheck};
		case Kind::HiddenRow: return {Intent::Total, Intent::SheetCheck};
		case Kind::Escalation: return {Intent::Escalation, Intent::Shortfall};
		case Kind::StepOnDueDate: return {Intent::Escalation, Intent::Shortfall};
		case Kind::EscalatedVsLedger: return {Intent::Shortfall, Intent::Escalation, Intent::SheetCheck};
		case Kind::DaysBetween: return {Intent::BusinessDays, Intent::DaysBetween, Intent::Deadline};
		case Kind::Deadline: return {Intent::Deadline};
		case Kind::NextBusinessDay: return {Intent::Deadline, Intent::BusinessDays};
		case Kind::DateOrder: return {Intent::Chronology};
		case Kind::RuleVsCell: return {Intent::Deadline, Intent::Compare, Intent::SheetCheck};
		case Kind::NamedSum: return {Intent::Total};
		case Kind::ValueEquality: return {Intent::Compare, Intent::SheetCheck};
		case Kind::ContactOwner: return {Intent::Contact};
	}
	return {};
}

struct Builder {
	const Input &input;
	Rows rows;
	std::vector<Relation> relations;
	std::vector<Derived> derived;
	std::map<std::string, std::size_t> derivedByKey;
	std::set<std::string> questionStems;
	std::set<std::size_t> questionRows;
	std::set<std::size_t> firstPassageRows;
	// The governing business-day rule, by the passage whose rule applies
	// (none: the material as a whole).
	std::map<std::optional<std::size_t>, business_calendar::GoverningRules> governing;

	std::string mark(std::size_t index) const {
		return std::string(1, kDerivedMark) + std::to_string(index) + std::string(1, kDerivedMark);
	}

	// A derived fact is keyed by how it was worked out, never by its value: two
	// derivations of one value are two facts (owner correction, 26 Sep 2026).
	std::string add(Value value, EntityType type, const std::string &derivation) {
		const std::string key = derivation.empty() ? "value|" + semantic::value_key(value) : derivation;
		const auto found = derivedByKey.find(key);
		if (found != derivedByKey.end()) return mark(found->second);
		derived.push_back({std::move(value), type, key});
		derivedByKey.emplace(key, derived.size() - 1);
		return mark(derived.size() - 1);
	}

	std::string money(const Value &value, const std::string &derivation) {
		Value paid = value;
		paid.amount = paid.amount.rounded(semantic::minor_unit_places(paid.currency));
		paid.source_decimals = semantic::minor_unit_places(paid.currency);
		paid.basis = semantic::RateBasis::None;
		return add(paid, EntityType::MoneyAmount, derivation);
	}

	std::string date(long long day, const std::string &derivation) {
		return add(semantic::make_date(temporal::TemporalValue{day, day, temporal::Precision::Day}), EntityType::Date, derivation);
	}

	std::string duration(long long count, semantic::DurationUnit unit, const std::string &derivation) {
		return add(semantic::make_duration(Rational(count), unit), EntityType::Number, derivation);
	}

	// The business-day rule that governs where the passage `operative` applies
	// one (its document's own definition first), or across the material.
	const business_calendar::GoverningRules &rules(std::optional<std::size_t> operative) {
		if (operative && *operative >= input.raw_passages.size()) operative.reset();
		auto found = governing.find(operative);
		if (found != governing.end()) return found->second;
		business_calendar::GoverningInput given;
		given.passages = input.raw_passages;
		given.operative = operative;
		given.matter_rule = input.matter_rule;
		return governing.emplace(operative, business_calendar::governing_rule(given)).first->second;
	}

	Relation &emit(Kind kind, std::string text, const std::vector<std::size_t> &operands, double priority,
			std::optional<business_calendar::BusinessDayRule> rule = std::nullopt) {
		double rank = priority;
		for (const std::size_t row : operands) {
			if (questionRows.count(row) != 0) { rank += 3.0; break; }
		}
		for (const std::size_t row : operands) {
			if (firstPassageRows.count(row) != 0) { rank += 2.0; break; }
		}
		const std::set<std::string> stems = content_stems(fold(text));
		if (std::any_of(stems.begin(), stems.end(), [&](const std::string &stem) { return questionStems.count(stem) != 0; }))
			rank += 1.0;
		Relation relation;
		relation.kind = kind;
		relation.text = std::move(text);
		relation.rank = rank;
		for (const std::size_t row : operands)
			if (std::find(relation.operand_rows.begin(), relation.operand_rows.end(), row) == relation.operand_rows.end())
				relation.operand_rows.push_back(row);
		relation.serves = serves_of(kind);
		relation.rule = std::move(rule);
		relations.push_back(std::move(relation));
		return relations.back();
	}
};

// ---------------------------------------------------------------------------
// (a) Tables and ledgers

// What a money column of a ledger holds, by its header's words: the amount
// due, the amount paid, an offset against what is due - or Unclear, when the
// header shifts or negates such a word ("Unpaid", "Prepaid", "Overdue", «Не
// оплачено», «Переплата») or names two roles at once. Debit and Credit are
// kept apart: which of them is the payment depends on whose ledger it is.
enum class Role { None, Due, Paid, Offset, Debit, Credit, Unclear };

Role column_role(const std::string &header) {
	// Whole words, or stems ('*'), in the header's folded words.
	static const std::vector<std::string> dueWords{"billed", "bill", "due", "charged", "charge", "invoiced", "invoice",
		"accrued", "accrual", "owed", "owing", "payable", "scheduled",
		"\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB*",                          // начисл*
		"\xD0\xB2\xD1\x8B\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB*",                  // выставл*
		"\xD0\xB3\xD1\x80\xD0\xB0\xD1\x84\xD0\xB8\xD0\xBA*"};                         // график*
	static const std::vector<std::string> paidWords{"paid", "pay", "received", "receipt", "payment", "remitted",
		"remittance", "collected", "settled",
		"\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x87*",                                  // оплач*
		"\xD1\x83\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x87*",                                  // уплач*
		"\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82*",                                  // оплат*
		"\xD1\x83\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82*",                                  // уплат*
		"\xD0\xBF\xD0\xBE\xD1\x81\xD1\x82\xD1\x83\xD0\xBF*",                          // поступ*
		"\xD0\xBF\xD0\xBE\xD0\xBB\xD1\x83\xD1\x87\xD0\xB5\xD0\xBD*",                  // получен*
		"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB*",          // перечисл*
		"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB6*", "\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD1\x91\xD0\xB6*"};  // платеж*, платёж*
	static const std::vector<std::string> offsetWords{"offset", "set-off", "setoff", "credit memo", "credit note",
		"\xD0\xB7\xD0\xB0\xD1\x87\xD1\x91\xD1\x82*", "\xD0\xB7\xD0\xB0\xD1\x87\xD0\xB5\xD1\x82*"};  // зачёт*, зачет*
	static const std::vector<std::string> positionWords{"balance", "outstanding", "remaining", "arrears",
		"\xD1\x81\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xBE",                           // сальдо
		"\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB3*",                                          // долг*
		"\xD0\xB7\xD0\xB0\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB5\xD0\xBD*",          // задолжен*
		"\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD1\x82*"};                                 // остат*
	// A word that turns a role into another, glued on or standing before it.
	static const std::vector<std::string> shifts{"un", "non-", "non", "pre", "over", "under", "not ",
		"\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xBE",                                           // недо
		"\xD0\xBD\xD0\xB5 ", "\xD0\xBD\xD0\xB5",                                      // не
		"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5",                                           // пере
		"\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4"};                                          // пред
	const auto any = [](const std::string &text, const std::vector<std::string> &words) {
		return std::any_of(words.begin(), words.end(), [&](const std::string &word) { return has_word(text, word); });
	};
	// A role word with a shift before it: "unpaid", "prepaid", "overdue", «неоплачено», «не оплачено».
	for (std::size_t at = 0; at < header.size(); ++at) {
		if (letter_before(header, at)) continue;
		for (const std::string &shift : shifts) {
			if (header.compare(at, shift.size(), shift) != 0) continue;
			const std::string rest = header.substr(at + shift.size());
			for (const std::vector<std::string> *words : {&dueWords, &paidWords})
				for (const std::string &word : *words) {
					const std::size_t found = word_position(rest, word);
					if (found == 0) return Role::Unclear;
				}
		}
	}
	// "Payment due", «К оплате», «К уплате», "Amount due": the amount due, whose
	// payment word names what is owed.
	std::string rest = header;
	bool due = false;
	for (const char *phrase : {"payments due", "payment due", "amount due", "amounts due", "due payment",
			"\xD0\xBA \xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5", "\xD0\xBA \xD1\x83\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5"}) {
		const std::size_t at = word_position(rest, phrase);
		if (at == std::string::npos) continue;
		due = true;
		rest.replace(at, std::char_traits<char>::length(phrase), std::string(std::char_traits<char>::length(phrase), ' '));
	}
	due = due || any(rest, dueWords);
	const bool paid = any(rest, paidWords);
	const bool offset = any(rest, offsetWords);
	const bool debit = has_word(rest, "debit") || has_word(rest, "\xD0\xB4\xD0\xB5\xD0\xB1\xD0\xB5\xD1\x82*");  // дебет*
	const bool credit = has_word(rest, "credit") || has_word(rest, "credits") || has_word(rest, "credited")
		|| has_word(rest, "\xD0\xBA\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82*");                                      // кредит*
	if (any(rest, positionWords)) return (due || paid || offset) ? Role::Unclear : Role::None;
	const int named = (due ? 1 : 0) + (paid ? 1 : 0) + (offset ? 1 : 0) + (debit ? 1 : 0) + (credit && !offset ? 1 : 0);
	if (named > 1) return Role::Unclear;
	if (offset) return Role::Offset;
	if (debit) return Role::Debit;
	if (credit) return Role::Credit;
	if (due) return Role::Due;
	if (paid) return Role::Paid;
	return Role::None;
}

// A cell's amount: its symbol's money, or a literal in the column's currency.
std::optional<Value> cell_money(const Cell *cell, const Rows &rows, const std::string &currency) {
	if (cell == nullptr) return std::nullopt;
	if (cell->symbol) {
		const Value *value = rows.value(*cell->symbol);
		if (value != nullptr && value->kind == VKind::Money) return *value;
		return std::nullopt;
	}
	if (cell->literal && (cell->literal->kind == VKind::Money || cell->literal->kind == VKind::Number)) {
		Value value = semantic::make_money(cell->literal->amount, currency);
		return value;
	}
	return std::nullopt;
}

std::string cell_word(const Cell *cell, const Rows &rows) {
	if (cell == nullptr) return "empty";
	if (cell->symbol) return rows.symbol(*cell->symbol);
	return cell->raw.empty() ? "empty" : cell->raw;
}

std::string row_ranges(const std::vector<unsigned> &numbers) {
	std::string written;
	std::size_t index = 0;
	while (index < numbers.size()) {
		std::size_t last = index;
		while (last + 1 < numbers.size() && numbers[last + 1] == numbers[last] + 1) ++last;
		if (!written.empty()) written += ", ";
		written += std::to_string(numbers[index]);
		if (last > index) written += "-" + std::to_string(numbers[last]);
		index = last + 1;
	}
	return written;
}

// The currency a money column is counted in: its symbols' one currency.
std::string column_currency(const Sheet &sheet, const std::string &column, const Rows &rows) {
	std::set<std::string> currencies;
	for (const SheetRow &row : sheet.rows) {
		const Cell *cell = row.at(column);
		if (cell == nullptr) continue;
		if (cell->symbol) {
			const Value *value = rows.value(*cell->symbol);
			if (value != nullptr && value->kind == VKind::Money) currencies.insert(value->currency);
		} else if (!cell->note.empty()) {
			const std::string named = semantic::currency_named(cell->note);
			if (!named.empty()) currencies.insert(named);
		}
	}
	currencies.erase("");
	return currencies.size() == 1 ? *currencies.begin() : std::string();
}

void table_relations(Builder &out, const std::vector<Sheet> &sheets) {
	const Rows &rows = out.rows;
	for (const Sheet &sheet : sheets) {
		const std::size_t bodyStart = sheet.header ? *sheet.header + 1 : 0;
		std::vector<const SheetRow *> body;
		std::vector<const SheetRow *> totals;
		for (std::size_t index = bodyStart; index < sheet.rows.size(); ++index) {
			const RowKind kind = row_kind(sheet.rows[index]);
			if (kind == RowKind::Body) body.push_back(&sheet.rows[index]);
			else if (kind == RowKind::Total) totals.push_back(&sheet.rows[index]);
		}
		if (body.size() < 2) continue;
		// A "=SUM(...)" row that adds only some of the rows is a subtotal: neither
		// a record nor the total, so it is compared with nothing.
		totals.erase(std::remove_if(totals.begin(), totals.end(), [&](const SheetRow *row) {
			for (const Cell &cell : row->cells) {
				const std::optional<std::pair<unsigned, unsigned>> range = sum_range(fold(cell.formula));
				if (!range) continue;
				for (const SheetRow *record : body)
					if (record->at(cell.column) != nullptr && (record->number < range->first || record->number > range->second))
						return true;
			}
			return false;
		}), totals.end());
		std::set<std::string> columns;
		for (const SheetRow *row : body)
			for (const Cell &cell : row->cells) columns.insert(cell.column);
		std::map<Role, std::string> roles;
		// A money column whose part in the balance its header does not say -
		// "Base Rent" beside "CAM Charges" and "Amount Paid", "Late Fee", «Пени» -
		// is a charge or a credit Singularity cannot place: a balance that left it out
		// told a tenant who owed money that it had overpaid (reviewer, 26 Sep 2026).
		bool unplaced = false;
		for (const std::string &column : columns) {
			const std::string currency = column_currency(sheet, column, rows);
			std::size_t moneyCells = 0;
			std::size_t symbols = 0;
			std::size_t cells = 0;
			for (const SheetRow *row : body) {
				const Cell *cell = row->at(column);
				if (cell == nullptr) continue;
				++cells;
				if (cell_money(cell, rows, currency)) ++moneyCells;
				if (cell->symbol && rows.is(*cell->symbol, VKind::Money)) ++symbols;
			}
			if (symbols != 0 && currency.empty()) unplaced = true;
			if (currency.empty() || moneyCells == 0 || moneyCells * 10 < cells * 6) continue;
			const auto header = sheet.headers.find(column);
			const Role role = header == sheet.headers.end() ? Role::None : column_role(header->second);
			if (symbols == 0) {
				if (role == Role::None) unplaced = true;
				continue;
			}
			// Two columns of one role, or a role Singularity cannot tell, leave the
			// ledger's balance to the model's own reading: none is worked out.
			if (role == Role::None) unplaced = true;
			else if (roles.count(role) != 0) roles[Role::Unclear] = column;
			else roles[role] = column;
			if (symbols < 2) continue;
			// The column's total over its body rows, hidden rows included and named.
			Rational sum;
			std::vector<unsigned> counted;
			std::vector<unsigned> hidden;
			std::vector<std::size_t> operands;
			for (const SheetRow *row : body) {
				const std::optional<Value> amount = cell_money(row->at(column), rows, currency);
				if (!amount) continue;
				sum = sum + amount->amount;
				counted.push_back(row->number);
				if (row->hidden) hidden.push_back(row->number);
				if (row->at(column)->symbol) operands.push_back(*row->at(column)->symbol);
			}
			std::string line;
			bool checked = false;
			try {
				const std::string total = out.money(semantic::make_money(sum, currency),
					"total|" + std::to_string(sheet.passage) + "|" + column + "|" + row_ranges(counted));
				line = "sum of " + column + " rows " + row_ranges(counted) + " (" + currency + "): " + total;
				for (const SheetRow *row : totals) {
					const Cell *own = row->at(column);
					const std::optional<Value> shown = cell_money(own, rows, currency);
					if (!shown) continue;
					checked = true;
					if (own->symbol) operands.push_back(*own->symbol);
					if (shown->amount == sum) line += "; equals " + own->ref + " " + cell_word(own, rows);
					else
						line += "; differs from " + own->ref + " " + cell_word(own, rows) + " by "
							+ out.money(semantic::make_money((shown->amount - sum).abs(), currency),
								"totaldiff|" + std::to_string(sheet.passage) + "|" + column);
				}
				if (!hidden.empty()) line += "; includes hidden row" + std::string(hidden.size() > 1 ? "s " : " ") + row_ranges(hidden);
			} catch (const semantic::AlgebraError &) {
				continue;
			}
			out.emit(Kind::ColumnTotal, line, operands, 0.5).sheet_total = checked;
		}
		// Due - paid - offset, row by row, where the headers say which is which -
		// and only then: a Debit column makes whose payment a Credit is a
		// question of whose ledger it is.
		if (roles.count(Role::Credit) != 0 && roles.count(Role::Debit) == 0 && roles.count(Role::Offset) == 0) {
			roles[Role::Offset] = roles[Role::Credit];
			roles.erase(Role::Credit);
		}
		if (unplaced || roles.count(Role::Unclear) != 0 || roles.count(Role::Debit) != 0 || roles.count(Role::Credit) != 0) continue;
		if (roles.count(Role::Due) == 0 || roles.count(Role::Paid) == 0) continue;
		const std::string due = roles[Role::Due];
		const std::string paid = roles[Role::Paid];
		const std::string offset = roles.count(Role::Offset) != 0 ? roles[Role::Offset] : std::string();
		const std::string currency = column_currency(sheet, due, rows);
		std::vector<unsigned> zero;
		Rational running;
		std::size_t differing = 0;
		for (const SheetRow *row : body) {
			const std::optional<Value> owed = cell_money(row->at(due), rows, currency);
			const std::optional<Value> received = cell_money(row->at(paid), rows, currency);
			if (!owed || !received) continue;
			Rational balance = owed->amount - received->amount;
			if (!offset.empty()) {
				const std::optional<Value> credited = cell_money(row->at(offset), rows, currency);
				if (credited) balance = balance - credited->amount;
			}
			running = running + balance;
			if (balance.is_zero()) {
				zero.push_back(row->number);
				continue;
			}
			if (++differing > 12) continue;
			std::vector<std::size_t> operands;
			for (const std::string &column : {due, paid, offset})
				if (!column.empty() && row->at(column) && row->at(column)->symbol) operands.push_back(*row->at(column)->symbol);
			try {
				const std::string amount = out.money(semantic::make_money(balance.abs(), currency),
					"balance|" + std::to_string(sheet.passage) + "|" + std::to_string(row->number));
				out.emit(Kind::RowBalance, "row " + std::to_string(row->number) + ": " + due + " - " + paid
						+ (offset.empty() ? std::string() : " - " + offset) + " is " + amount
						+ (balance.sign() > 0 ? " (less paid than due)" : " (more paid than due)")
						+ (row->hidden ? "; the row is hidden" : ""),
					operands, 1.0);
			} catch (const semantic::AlgebraError &) {
			}
		}
		std::vector<std::size_t> columnFacts;
		for (const SheetRow *row : body)
			for (const std::string &column : {due, paid, offset})
				if (!column.empty() && row->at(column) && row->at(column)->symbol) columnFacts.push_back(*row->at(column)->symbol);
		if (!zero.empty())
			out.emit(Kind::RowBalance, "rows " + row_ranges(zero) + ": " + due + " - " + paid
					+ (offset.empty() ? std::string() : " - " + offset) + " is zero", columnFacts, 0.5);
		if (differing > 0) {
			try {
				const std::string total = out.money(semantic::make_money(running.abs(), currency),
					"runningbalance|" + std::to_string(sheet.passage));
				out.emit(Kind::RowBalance, "all rows together: " + due + " - " + paid
						+ (offset.empty() ? std::string() : " - " + offset) + " is " + total
						+ (running.sign() > 0 ? " (less paid than due)" : running.sign() < 0 ? " (more paid than due)" : ""),
					columnFacts, 1.0);
			} catch (const semantic::AlgebraError &) {
			}
		}
	}
}

// ---------------------------------------------------------------------------
// (b) Stepped amounts

// What an amount is named by - "Base Rent", «арендная плата», "License Fee" -
// as an item and the words (or stems, '*') it is written in, the most specific
// first. Matched as words stand: "rent" never in "current" or "Parent", "fee"
// never in "coffee", «цена» never in «процент» or «лицензионный».
struct Item {
	const char *name;
	std::vector<std::string> words;
};

const std::vector<Item> &items() {
	static const std::vector<Item> all{
		{"base rent", {"base rent", "basic rent", "minimum rent", "fixed rent", "minimum annual rent"}},
		{"license fee", {"license fee", "licence fee", "royalty", "royalties", "\xD1\x80\xD0\xBE\xD1\x8F\xD0\xBB\xD1\x82\xD0\xB8",  // роялти
			"\xD0\xBB\xD0\xB8\xD1\x86\xD0\xB5\xD0\xBD\xD0\xB7\xD0\xB8\xD0\xBE\xD0\xBD\xD0\xBD*"}},                                // лицензионн*
		{"salary", {"salary", "salaries", "wage", "wages", "base pay",
			"\xD0\xBE\xD0\xBA\xD0\xBB\xD0\xB0\xD0\xB4*",                                   // оклад*
			"\xD0\xB7\xD0\xB0\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x82\xD0\xBD*",           // заработн*
			"\xD0\xB7\xD0\xB0\xD1\x80\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82*"}},                 // зарплат*
		{"rent", {"rent", "rental", "\xD0\xB0\xD1\x80\xD0\xB5\xD0\xBD\xD0\xB4\xD0\xBD*"}},  // арендн*
		{"fee", {"fee", "fees", "retainer", "\xD0\xB2\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD0\xB3\xD1\x80\xD0\xB0\xD0\xB6\xD0\xB4*"}},  // вознагражд*
		{"premium", {"premium", "premiums", "\xD0\xBF\xD1\x80\xD0\xB5\xD0\xBC\xD0\xB8*"}},  // преми*
		{"price", {"price", "prices", "\xD1\x86\xD0\xB5\xD0\xBD\xD0\xB0", "\xD1\x86\xD0\xB5\xD0\xBD\xD1\x8B",  // цена цены
			"\xD1\x86\xD0\xB5\xD0\xBD\xD0\xB5", "\xD1\x86\xD0\xB5\xD0\xBD\xD1\x83", "\xD1\x86\xD0\xB5\xD0\xBD\xD0\xBE\xD0\xB9",  // цене цену ценой
			"\xD1\x81\xD1\x82\xD0\xBE\xD0\xB8\xD0\xBC\xD0\xBE\xD1\x81\xD1\x82*"}},         // стоимост*
		{"installment", {"installment", "installments", "instalment", "instalments",
			"\xD0\xB2\xD0\xB7\xD0\xBD\xD0\xBE\xD1\x81*"}},                                 // взнос*
		{"interest", {"interest"}},
		{"charge", {"charge", "charges"}},
		{"payment", {"payment", "payments", "\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB6*",  // платеж*
			"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD1\x91\xD0\xB6*"}},                         // платёж*
		{"plata", {"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB0", "\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD1\x8B",  // плата платы
			"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5", "\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD1\x83",  // плате плату
			"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xBE\xD0\xB9"}},                          // платой
	};
	return all;
}

bool mentions(const std::string &folded, const Item &item) {
	return std::any_of(item.words.begin(), item.words.end(), [&](const std::string &word) { return has_word(folded, word); });
}

// Where the item is first written in `folded`, npos when it is not.
std::size_t item_position(const std::string &folded, const Item &item) {
	std::size_t first = std::string::npos;
	for (const std::string &word : item.words) first = std::min(first, word_position(folded, word));
	return first;
}

// The item a sentence names its amount by, the most specific it writes.
const Item *item_of(const std::string &folded) {
	for (const Item &item : items())
		if (mentions(folded, item)) return &item;
	return nullptr;
}

// Ordinal words for the days of a month, English and Russian (the Russian as a
// stem any case may end): "fifth", «пятого», «двадцать пятое».
std::optional<unsigned> ordinal_word(const std::string &folded) {
	static const std::vector<std::pair<std::string, unsigned>> english{{"first", 1}, {"second", 2}, {"third", 3},
		{"fourth", 4}, {"fifth", 5}, {"sixth", 6}, {"seventh", 7}, {"eighth", 8}, {"ninth", 9}, {"tenth", 10},
		{"eleventh", 11}, {"twelfth", 12}, {"thirteenth", 13}, {"fourteenth", 14}, {"fifteenth", 15}, {"sixteenth", 16},
		{"seventeenth", 17}, {"eighteenth", 18}, {"nineteenth", 19}, {"twentieth", 20}, {"thirtieth", 30}};
	static const std::vector<std::pair<std::string, unsigned>> russian{
		{"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB2", 1}, {"\xD0\xB2\xD1\x82\xD0\xBE\xD1\x80", 2}, {"\xD1\x82\xD1\x80\xD0\xB5\xD1\x82", 3},
		{"\xD1\x87\xD0\xB5\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80\xD1\x82", 4}, {"\xD1\x87\xD0\xB5\xD1\x82\xD0\xB2\xD1\x91\xD1\x80\xD1\x82", 4},
		{"\xD0\xBF\xD1\x8F\xD1\x82", 5}, {"\xD1\x88\xD0\xB5\xD1\x81\xD1\x82", 6}, {"\xD1\x81\xD0\xB5\xD0\xB4\xD1\x8C\xD0\xBC", 7},
		{"\xD0\xB2\xD0\xBE\xD1\x81\xD1\x8C\xD0\xBC", 8}, {"\xD0\xB4\xD0\xB5\xD0\xB2\xD1\x8F\xD1\x82", 9},
		{"\xD0\xB4\xD0\xB5\xD1\x81\xD1\x8F\xD1\x82", 10}, {"\xD0\xBE\xD0\xB4\xD0\xB8\xD0\xBD\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 11},
		{"\xD0\xB4\xD0\xB2\xD0\xB5\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 12}, {"\xD1\x82\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 13},
		{"\xD1\x87\xD0\xB5\xD1\x82\xD1\x8B\xD1\x80\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 14}, {"\xD0\xBF\xD1\x8F\xD1\x82\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 15},
		{"\xD1\x88\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 16}, {"\xD1\x81\xD0\xB5\xD0\xBC\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 17},
		{"\xD0\xB2\xD0\xBE\xD1\x81\xD0\xB5\xD0\xBC\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 18}, {"\xD0\xB4\xD0\xB5\xD0\xB2\xD1\x8F\xD1\x82\xD0\xBD\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 19},
		{"\xD0\xB4\xD0\xB2\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 20}, {"\xD1\x82\xD1\x80\xD0\xB8\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82", 30}};
	// "twenty-fifth", «двадцать пятого»: a tens word and a unit.
	const auto one = [&](const std::string &word) -> std::optional<unsigned> {
		for (const auto &[form, day] : english)
			if (word == form) return day;
		// A Russian stem takes an ending of at most four letters: «пятого», «третье».
		for (const auto &[stem, day] : russian)
			if (word.size() > stem.size() && word.compare(0, stem.size(), stem) == 0 && word.size() - stem.size() <= 8) {
				// «пятнадцатого» begins with «пят» too: the longest stem that fits wins below.
				bool longer = false;
				for (const auto &[other, otherDay] : russian)
					if (other.size() > stem.size() && word.compare(0, other.size(), other) == 0) longer = true;
				if (!longer) return day;
			}
		return std::nullopt;
	};
	std::vector<std::string> words;
	std::string word;
	for (const char character : folded + " ") {
		if (character == ' ' || character == '-') {
			if (!word.empty()) words.push_back(word);
			word.clear();
		} else {
			word.push_back(character);
		}
	}
	if (words.empty() || words.size() > 2) return std::nullopt;
	if (words.size() == 1) return one(words.front());
	const std::optional<unsigned> tens = one(words.front());
	const std::optional<unsigned> unit = one(words.back());
	if (words.front() == "twenty") return unit && *unit < 10 ? std::optional<unsigned>(20 + *unit) : std::nullopt;
	if (words.front() == "thirty") return unit && *unit == 1 ? std::optional<unsigned>(31) : std::nullopt;
	// «двадцать пятого», «тридцать первого»: the tens written as a number word.
	if (words.front() == "\xD0\xB4\xD0\xB2\xD0\xB0\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82\xD1\x8C")
		return unit && *unit < 10 ? std::optional<unsigned>(20 + *unit) : std::nullopt;
	if (words.front() == "\xD1\x82\xD1\x80\xD0\xB8\xD0\xB4\xD1\x86\xD0\xB0\xD1\x82\xD1\x8C")
		return unit && *unit == 1 ? std::optional<unsigned>(31) : std::nullopt;
	(void)tens;
	return std::nullopt;
}

// "on the first day of each month", "the fifth (5th) day of each month", "on
// the 1st of each month", «не позднее 5-го числа», «до 10 (десятого) числа
// каждого месяца», «пятого числа»: the day of the month an amount is due on,
// when a sentence gives one. A day that moves - "the last (business) day of the
// month", «последнего рабочего дня месяца» - is no one day, and gives none.
std::optional<unsigned> due_day_of_month(const std::string &folded) {
	static const std::regex english(
		"(?:\\b(\\d{1,2})(?:st|nd|rd|th)?|\\b([a-z]+(?:-[a-z]+)?))(?: \\((?:\\d{1,2})(?:st|nd|rd|th)?\\))?"
		"(?: day)? of (?:each|every|the|any) (?:calendar )?month");
	static const std::regex russian(
		"(?:(\\d{1,2})(?:-?(?:\\xD0\\xB3\\xD0\\xBE|\\xD0\\xB5|\\xD0\\xBE\\xD0\\xB5|\\xD0\\xBE\\xD0\\xB3\\xD0\\xBE))?"
		"|((?:[\\xD0\\xD1][\\x80-\\xBF])+(?: (?:[\\xD0\\xD1][\\x80-\\xBF])+)?))"
		" (?:\\([^)]{1,30}\\) )?\\xD1\\x87\\xD0\\xB8\\xD1\\x81\\xD0\\xBB");  // числ
	const auto valid = [](unsigned day) { return day >= 1 && day <= 31 ? std::optional<unsigned>(day) : std::nullopt; };
	for (std::sregex_iterator match(folded.begin(), folded.end(), english), end; match != end; ++match) {
		if ((*match)[1].matched) return valid(static_cast<unsigned>(std::stoul((*match)[1].str())));
		if (const std::optional<unsigned> day = ordinal_word((*match)[2].str())) return valid(*day);
	}
	for (std::sregex_iterator match(folded.begin(), folded.end(), russian), end; match != end; ++match) {
		if ((*match)[1].matched) return valid(static_cast<unsigned>(std::stoul((*match)[1].str())));
		// «до пятого числа», «не позднее двадцать пятого числа»: the ordinal is the
		// last word or two before «числа».
		const std::string words = (*match)[2].str();
		if (const std::optional<unsigned> day = ordinal_word(words)) return valid(*day);
		const std::size_t space = words.find(' ');
		if (space != std::string::npos)
			if (const std::optional<unsigned> day = ordinal_word(words.substr(space + 1))) return valid(*day);
	}
	return std::nullopt;
}

// Which way a sentence moves an amount: up ("increases", "escalates", «индексируется»,
// «увеличивается»), down ("adjusted downward", "reduced", «снижается»), both, or neither.
enum class Direction { None, Up, Down, Both };

Direction direction_of(const std::string &folded) {
	const bool up = any_whole_word(folded, {"increase*", "escalat*", "rise", "rises", "rising", "raise", "raised", "raises",
		"bump*", "uplift*", "step up", "steps up", "stepped up", "go up", "goes up", "grow", "grows",
		"\xD1\x83\xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x87*",                // увелич*
		"\xD0\xB8\xD0\xBD\xD0\xB4\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB8\xD1\x80*",  // индексир*
		"\xD0\xB8\xD0\xBD\xD0\xB4\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB0\xD1\x86*",  // индексац*
		"\xD0\xBF\xD0\xBE\xD0\xB2\xD1\x8B\xD1\x88*",                         // повыш*
		"\xD0\xB2\xD0\xBE\xD0\xB7\xD1\x80\xD0\xB0\xD1\x81\xD1\x82*"});        // возраст*
	const bool down = any_whole_word(folded, {"decrease*", "reduc*", "lower", "lowered", "lowers", "downward*",
		"decline*", "abate*", "abatement", "cut", "cuts",
		"\xD1\x81\xD0\xBD\xD0\xB8\xD0\xB6*",                                 // сниж*
		"\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8C\xD1\x88*",                 // уменьш*
		"\xD0\xBF\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xB6*"});                       // пониж*
	return up && down ? Direction::Both : up ? Direction::Up : down ? Direction::Down : Direction::None;
}

// Words that make a step anything but one fixed rate: bounds ("not less than 2%
// nor more than 5%", «не более»), an index it follows ("CPI", "cost of living",
// «индекс потребительских цен», «инфляция»), a choice ("the greater of").
bool step_is_conditional(const std::string &folded) {
	return any_whole_word(folded, {"not less than", "not more than", "no less than", "no more than", "not to exceed",
		"at least", "at most", "minimum", "maximum", "floor", "ceiling", "cap", "capped", "up to", "greater of", "lesser of",
		"higher of", "lower of", "whichever", "between", "cpi", "consumer price*", "price index*", "cost of living",
		"cost-of-living", "inflation*", "index-linked", "indexed to",
		"\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",                     // не менее
		"\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5",                     // не более
		"\xD0\xBD\xD0\xB5 \xD0\xB2\xD1\x8B\xD1\x88\xD0\xB5",                             // не выше
		"\xD0\xBD\xD0\xB5 \xD0\xBD\xD0\xB8\xD0\xB6\xD0\xB5",                             // не ниже
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB2\xD1\x8B\xD1\x88*",            // не превыш*
		"\xD0\xBC\xD0\xB0\xD0\xBA\xD1\x81\xD0\xB8\xD0\xBC\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD*",  // максимальн*
		"\xD0\xBC\xD0\xB8\xD0\xBD\xD0\xB8\xD0\xBC\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD*",     // минимальн*
		"\xD0\xB2 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB0\xD1\x85",     // в пределах
		"\xD0\xBF\xD0\xBE\xD1\x82\xD1\x80\xD0\xB5\xD0\xB1\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD0\xBA*",  // потребительск*
		"\xD0\xB8\xD0\xBD\xD1\x84\xD0\xBB\xD1\x8F\xD1\x86*",                             // инфляц*
		"\xD0\xB8\xD0\xBF\xD1\x86"});                                                    // ипц
}

// Whether a step lasts only a while: "reduced by 50% for three (3) months",
// "for a period of six months", "until the Rent Commencement Date", "during
// the first Lease Year", an abatement, «в течение трех месяцев», «на период»,
// «до окончания». Read to hold a comparison back, never to make one: a step
// that ends leaves the ledger's later rows no measure of it.
bool step_is_temporary(const std::string &folded) {
	static const std::regex forPeriod(
		"\\bfor (?:a period of |the (?:first|next|initial|following|last) )?"
		"(?:(?:a|an|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|fifteen|eighteen|twenty|twenty-four|thirty|"
		"forty-five|sixty|ninety|several|some) )?(?:\\(\\d{1,3}\\) )?(?:\\d{1,3} )?(?:full |consecutive )?(?:calendar )?"
		"(?:day|week|month|year|lease year|period|billing period|quarter)s?\\b");
	if (std::regex_search(folded, forPeriod)) return true;
	return any_whole_word(folded, {"until", "till", "through", "thru", "during", "abate*", "abatement", "free rent",
		"rent holiday", "holiday", "waive*", "temporar*", "one-time", "one time", "for the first", "for the next",
		"\xD0\xB2 \xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",           // в течение
		"\xD0\xBD\xD0\xB0 \xD0\xBF\xD0\xB5\xD1\x80\xD0\xB8\xD0\xBE\xD0\xB4",           // на период
		"\xD0\xBD\xD0\xB0 \xD1\x81\xD1\x80\xD0\xBE\xD0\xBA",                           // на срок
		"\xD0\xB4\xD0\xBE \xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x87\xD0\xB0\xD0\xBD\xD0\xB8*",  // до окончани*
		"\xD0\xB4\xD0\xBE \xD0\xB4\xD0\xB0\xD1\x82\xD1\x8B",                           // до даты
		"\xD0\xB2\xD1\x80\xD0\xB5\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xBD*",                   // временн*
		"\xD0\xBA\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xBA\xD1\x83\xD0\xBB*"});                 // каникул*
}

struct Step {
	std::size_t base = 0;              // the amount's row
	std::optional<std::size_t> rate;   // the rate's row, or
	std::optional<Rational> literal;   // a rate the text writes itself ("3%")
	std::string literal_text;
	bool down = false;                 // the step lowers the amount
	bool temporary = false;            // the step lasts only a while (step_is_temporary)
	std::optional<std::size_t> day;    // the step's day of the year (or a date's)
	// A day of the year the sentence writes itself, not protected ("July 1").
	std::optional<temporal::DayOfYear> literal_day;
	std::string literal_day_text;
	const Item *item = nullptr;
	std::size_t passage = 0;
};

// Every rate a sentence writes in figures itself: "3%", "(3%)", "3 percent",
// "2.5 per cent", «5 (пять) процентов», «0,1 %». Each as its fraction and as written.
std::vector<std::pair<Rational, std::string>> literal_rates(const std::string &sentence) {
	static const std::regex percent(
		"(\\d{1,3}(?:[.,]\\d{1,4})?)\\s?(?:\\([^)0-9]{1,40}\\)\\s?)?"
		"(?:%|percent\\b|per cent\\b|pct\\b|\\xD0\\xBF\\xD1\\x80\\xD0\\xBE\\xD1\\x86\\xD0\\xB5\\xD0\\xBD\\xD1\\x82)");  // процент
	const std::string folded = fold(sentence);
	std::vector<std::pair<Rational, std::string>> rates;
	for (std::sregex_iterator match(folded.begin(), folded.end(), percent), end; match != end; ++match) {
		// Figures that belong to a symbol ("[[E61]]") or to a longer number are none.
		const std::size_t at = static_cast<std::size_t>(match->position(1));
		if (at > 0 && (std::isalnum(static_cast<unsigned char>(folded[at - 1])) != 0 || folded[at - 1] == '[')) continue;
		std::string figures = (*match)[1].str();
		std::replace(figures.begin(), figures.end(), ',', '.');
		const std::optional<Rational> value = Rational::parse_decimal(figures);
		if (!value) continue;
		rates.emplace_back(*value / Rational(100), figures + "%");
	}
	return rates;
}

void step_relations(Builder &out, const std::vector<Sentence> &sentences, const std::vector<Sheet> &sheets) {
	const Rows &rows = out.rows;
	std::vector<Step> steps;
	for (const Sentence &sentence : sentences) {
		// One fixed rate, one way: anything else - a band, an index, a choice, a
		// step both ways - is for the typed operations the model asks for.
		const Direction direction = direction_of(sentence.folded);
		if (direction != Direction::Up && direction != Direction::Down) continue;
		if (step_is_conditional(sentence.folded)) continue;
		Step step;
		step.passage = sentence.passage;
		step.down = direction == Direction::Down;
		step.temporary = step_is_temporary(sentence.folded);
		std::vector<std::size_t> moneyHere;
		std::vector<std::size_t> ratesHere;
		for (const Marker &marker : sentence.markers) {
			if (rows.is(marker.row, VKind::Percentage)) {
				if (std::find(ratesHere.begin(), ratesHere.end(), marker.row) == ratesHere.end()) ratesHere.push_back(marker.row);
			} else if (rows.is(marker.row, VKind::Money)) {
				if (std::find(moneyHere.begin(), moneyHere.end(), marker.row) == moneyHere.end()) moneyHere.push_back(marker.row);
			} else if ((rows.is(marker.row, VKind::DayOfYear) || rows.is_day(marker.row)) && !step.day) {
				step.day = marker.row;
			}
		}
		// Exactly one rate, by its value: "three percent ([[E61]])" and "3% (three
		// percent)" are one; "2%" and "5%" in one sentence are a band.
		std::set<std::string> values;
		for (const std::size_t row : ratesHere) values.insert(semantic::value_key(*rows.value(row)));
		const std::vector<std::pair<Rational, std::string>> literals = literal_rates(sentence.text);
		for (const auto &literal : literals) values.insert(semantic::value_key(semantic::make_percentage(literal.first)));
		if (values.size() != 1) continue;
		if (!ratesHere.empty()) {
			step.rate = ratesHere.front();
		} else {
			step.literal = literals.front().first;
			step.literal_text = literals.front().second;
		}
		if (moneyHere.size() > 1) continue;  // which amount steps, and which bounds it, is not Singularity's to guess
		step.item = item_of(sentence.folded);
		if (!moneyHere.empty()) {
			step.base = moneyHere.front();
		} else {
			// The amount the same item is given in, in the same passage.
			if (step.item == nullptr) continue;
			std::optional<std::size_t> found;
			for (const Sentence &other : sentences) {
				if (other.passage != sentence.passage || &other == &sentence || !mentions(other.folded, *step.item)) continue;
				// Where the amount is set, not where it is merely mentioned ("withheld
				// $61,300 of Base Rent"): "shall pay monthly Base Rent of", «составляет».
				if (!contains_any(other.folded, {"monthly", "per month", "annual", "per annum", "per year", "shall pay",
						"shall be", "is set at", "\xD0\xB5\xD0\xB6\xD0\xB5\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x87",
						"\xD0\xB2 \xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86", "\xD1\x81\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82",
						"\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80"}))
					continue;
				// The amount written nearest after the item's name.
				const std::size_t named = item_position(other.folded, *step.item);
				std::optional<std::size_t> after;
				std::optional<std::size_t> any;
				for (const Marker &marker : other.markers) {
					if (!rows.is(marker.row, VKind::Money)) continue;
					if (!any) any = marker.row;
					if (!after && marker.start >= named) after = marker.row;
				}
				found = after ? after : any;
				if (found) break;
			}
			if (!found) continue;
			step.base = *found;
		}
		if (!step.day) {
			const std::vector<temporal::FoundDayOfYear> days = temporal::find_days_of_year(sentence.text);
			if (days.size() == 1) {
				step.literal_day = days.front().value;
				step.literal_day_text = sentence.text.substr(days.front().offset, days.front().length);
			}
		}
		if (!step.day && !step.literal_day && !any_whole_word(sentence.folded, {"each year", "annually", "every year",
				"per year", "yearly", "each lease year", "each contract year", "each calendar year", "each anniversary",
				"every anniversary", "\xD0\xB5\xD0\xB6\xD0\xB5\xD0\xB3\xD0\xBE\xD0\xB4*"}))  // ежегод*
			continue;
		steps.push_back(step);
		if (steps.size() >= 3) break;
	}
	for (const Step &step : steps) {
		const Value *base = rows.value(step.base);
		if (base == nullptr || base->kind != VKind::Money) continue;
		const Rational rate = step.rate ? rows.value(*step.rate)->amount : *step.literal;
		const Rational factor = step.down ? Rational(1) - rate : Rational(1) + rate;
		const std::string rateWritten = step.rate ? rows.symbol(*step.rate) : step.literal_text;
		const std::string moved = step.down ? " reduced by " : " increased by ";
		std::string stepped;
		try {
			stepped = out.money(semantic::make_money(base->amount * factor, base->currency),
				std::string(step.down ? "stepdown|" : "step|") + rows.symbol(step.base) + "|" + rateWritten);
		} catch (const semantic::AlgebraError &) {
			continue;
		}
		std::vector<std::size_t> operands{step.base};
		if (step.rate) operands.push_back(*step.rate);
		if (step.day) operands.push_back(*step.day);
		out.emit(Kind::Escalation, stepped + " = " + rows.symbol(step.base) + moved + rateWritten + " (one step, to the cent)"
				+ (step.day ? "; the step falls on " + rows.symbol(*step.day)
					: step.literal_day ? "; the step falls on " + step.literal_day_text : std::string()),
			operands, 1.5);
		// Whether the step falls on the day the amount is due.
		std::optional<unsigned> stepMonthDay;
		std::optional<unsigned> stepMonth;
		const std::string stepWritten = step.day ? rows.symbol(*step.day) : step.literal_day_text;
		if (step.literal_day) {
			stepMonthDay = step.literal_day->day;
			stepMonth = step.literal_day->month;
		} else if (step.day) {
			const Value *day = rows.value(*step.day);
			if (day->kind == VKind::DayOfYear) {
				stepMonthDay = day->day_of_year.day;
				stepMonth = day->day_of_year.month;
			} else {
				const business_calendar::CivilDate civil = business_calendar::civil_from_days(day->date.first_day);
				stepMonthDay = civil.day;
				stepMonth = civil.month;
			}
		}
		if (stepMonthDay) {
			for (const Sentence &sentence : sentences) {
				if (sentence.passage != step.passage) continue;
				const std::optional<unsigned> dueDay = due_day_of_month(sentence.folded);
				if (!dueDay) continue;
				if (step.item != nullptr && !mentions(sentence.folded, *step.item)) continue;
				out.emit(Kind::StepOnDueDate, *stepMonthDay == *dueDay
						? stepWritten + " is a due date of " + rows.symbol(step.base)
							+ " (the day of the month it is paid on): the step applies from that month's whole payment"
						: stepWritten + " is not the day of the month " + rows.symbol(step.base)
							+ " is paid on: the step falls within a month",
					{step.day.value_or(step.base), step.base}, 1.5);
				break;
			}
		}
		// The ledger's own figures for that item from the latest step on, against
		// one step over what the ledger bills in the year before it. How many
		// steps lie between the amount's own date and the ledger's is known only
		// that way: a year-3 row is never measured against a year-2 amount. With
		// no single amount billed before the step, nothing is compared.
		// Only a lasting step up is measured against the ledger: a reduction, an
		// abatement or a step "for three (3) months" ends, and the full rows after
		// it are no shortfall or overcharge (reviewer, 26 Sep 2026).
		if (!stepMonthDay || step.item == nullptr || step.down || step.temporary) continue;
		for (const Sheet &sheet : sheets) {
			std::string dateColumn;
			std::string amountColumn;
			for (const auto &[column, header] : sheet.headers) {
				std::size_t days = 0;
				for (const SheetRow &row : sheet.rows)
					if (const Cell *cell = row.at(column); cell && cell->symbol && rows.is_day_or_month(*cell->symbol)) ++days;
				if (days >= 2 && dateColumn.empty()) dateColumn = column;
				const Role role = column_role(header);
				if (mentions(header, *step.item) && amountColumn.empty()
						&& (role == Role::None || role == Role::Due))
					amountColumn = column;
			}
			if (dateColumn.empty() || amountColumn.empty()) continue;
			std::optional<long long> latestStep;
			for (const SheetRow &row : sheet.rows) {
				const Cell *cell = row.at(dateColumn);
				if (!cell || !cell->symbol || !rows.is_day_or_month(*cell->symbol)) continue;
				const long long day = rows.value(*cell->symbol)->date.first_day;
				const business_calendar::CivilDate civil = business_calendar::civil_from_days(day);
				if (civil.day == *stepMonthDay && civil.month == *stepMonth && (!latestStep || day > *latestStep)) latestStep = day;
			}
			if (!latestStep) continue;
			// What the ledger bills in the twelve months before the step: one amount, or no comparison.
			std::set<std::string> priorAmounts;
			std::optional<Value> prior;
			std::vector<unsigned> priorRows;
			std::vector<std::size_t> priorOperands;
			for (const SheetRow &row : sheet.rows) {
				const Cell *when = row.at(dateColumn);
				if (!when || !when->symbol || !rows.is_day_or_month(*when->symbol)) continue;
				const long long day = rows.value(*when->symbol)->date.first_day;
				if (day >= *latestStep || day < *latestStep - 366) continue;
				const std::optional<Value> amount = cell_money(row.at(amountColumn), rows, base->currency);
				if (!amount || amount->currency != base->currency) continue;
				priorAmounts.insert(semantic::value_key(*amount));
				prior = amount;
				priorRows.push_back(row.number);
				if (row.at(amountColumn)->symbol) priorOperands.push_back(*row.at(amountColumn)->symbol);
			}
			// Only where the ledger bills the base amount itself in the twelve months
			// before the step: then the step is one step over it whether the
			// escalation compounds or runs on the initial amount ("3% of the initial
			// Base Rent"). One step over a later year's figure assumed compounding, and
			// called a correct simple-escalation ledger under-billed.
			if (priorAmounts.size() != 1 || !prior || prior->amount != base->amount) continue;
			const std::string expected = stepped;
			const std::string basis = "; " + stepped + " is one step over " + rows.symbol(step.base)
				+ ", which the ledger bills in the 12 months before the step (rows " + row_ranges(priorRows) + ")";
			Rational stepAmount;
			try {
				stepAmount = (base->amount * factor).rounded(semantic::minor_unit_places(base->currency));
			} catch (const semantic::AlgebraError &) {
				continue;
			}
			// Rows billed the same amount are compared once, each cell named: under
			// fact identity each row's amount is a fact - a symbol - of its own.
			struct Group {
				std::vector<unsigned> numbers;
				std::vector<std::string> cells;
				std::string comparison;
				std::vector<std::size_t> operands;
			};
			std::map<std::string, Group> byAmount;
			std::vector<std::string> order;
			for (const SheetRow &row : sheet.rows) {
				const Cell *when = row.at(dateColumn);
				if (!when || !when->symbol || !rows.is_day_or_month(*when->symbol)) continue;
				const long long day = rows.value(*when->symbol)->date.first_day;
				if (day < *latestStep || day >= *latestStep + 366) continue;
				const Cell *amountCell = row.at(amountColumn);
				const std::optional<Value> amount = cell_money(amountCell, rows, base->currency);
				if (!amount || amount->currency != base->currency) continue;
				const std::string amountKey = semantic::value_key(*amount);
				auto found = byAmount.find(amountKey);
				if (found == byAmount.end()) {
					Group group;
					try {
						if (amount->amount == stepAmount) group.comparison = " equals " + expected;
						else
							group.comparison = std::string(amount->amount < stepAmount ? " less than " : " more than ") + expected
								+ " by " + out.money(semantic::make_money((stepAmount - amount->amount).abs(), base->currency),
									"stepdiff|" + rows.symbol(step.base) + "|" + amountColumn + "|" + std::to_string(row.number));
					} catch (const semantic::AlgebraError &) {
						continue;
					}
					group.operands.push_back(step.base);
					group.operands.insert(group.operands.end(), priorOperands.begin(), priorOperands.end());
					found = byAmount.emplace(amountKey, std::move(group)).first;
					order.push_back(amountKey);
				}
				Group &group = found->second;
				group.numbers.push_back(row.number);
				group.cells.push_back(amountCell->ref + " " + cell_word(amountCell, rows));
				group.operands.push_back(*when->symbol);
				if (amountCell->symbol) group.operands.push_back(*amountCell->symbol);
			}
			for (const std::string &key : order) {
				const Group &group = byAmount.at(key);
				std::string cells;
				for (const std::string &cell : group.cells) cells += (cells.empty() ? "" : ", ") + cell;
				const bool many = group.numbers.size() > 1;
				const std::string verb = group.comparison.rfind(" equals", 0) == 0 ? (many ? " each" : "") + group.comparison
					: std::string(many ? " each is" : " is") + group.comparison;
				out.emit(Kind::EscalatedVsLedger, "ledger rows " + row_ranges(group.numbers) + " (dated on or after the latest "
						+ stepWritten + " in the ledger): " + cells + verb + basis, group.operands, 2.0);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// (c) Dates

struct DurationRule {
	long long count = 0;
	bool business = false;
	bool before = false;      // "N days before X"
	std::size_t end = 0;      // where the duration's words end in the sentence
	std::string written;      // the duration as the sentence writes it
};

// "ten (10) business days", "5 business days", «10 (десяти) календарных дней».
std::vector<DurationRule> durations_in(const std::string &sentence) {
	std::vector<DurationRule> found;
	static const std::regex english(
		"(?:([a-z-]+) )?\\((\\d{1,4})\\) ?(calendar |business |working |banking )?days?\\b"
		"|\\b(\\d{1,4}) (calendar |business |working |banking )?days?\\b",
		std::regex::icase);
	static const std::regex russian(
		"(\\d{1,4}) ?(?:\\([^)]{1,40}\\) ?)?"
		"((?:\\xD0\\xBA\\xD0\\xB0\\xD0\\xBB\\xD0\\xB5\\xD0\\xBD\\xD0\\xB4\\xD0\\xB0\\xD1\\x80\\xD0\\xBD|\\xD1\\x80\\xD0\\xB0\\xD0\\xB1\\xD0\\xBE\\xD1\\x87)[^ ]* )?"
		"(\\xD0\\xB4\\xD0\\xBD\\xD0\\xB5\\xD0\\xB9|\\xD0\\xB4\\xD0\\xBD\\xD1\\x8F|\\xD0\\xB4\\xD0\\xB5\\xD0\\xBD\\xD1\\x8C)");
	const std::string folded = fold(sentence);
	for (std::sregex_iterator match(folded.begin(), folded.end(), english), end; match != end; ++match) {
		DurationRule rule;
		const std::string digits = (*match)[2].matched ? (*match)[2].str() : (*match)[4].str();
		const std::string qualifier = (*match)[3].matched ? (*match)[3].str() : (*match)[5].str();
		rule.count = std::stoll(digits);
		rule.business = qualifier.rfind("business", 0) == 0 || qualifier.rfind("working", 0) == 0 || qualifier.rfind("banking", 0) == 0;
		rule.end = static_cast<std::size_t>(match->position() + match->length());
		rule.written = sentence.substr(static_cast<std::size_t>(match->position()), static_cast<std::size_t>(match->length()));
		found.push_back(rule);
	}
	for (std::sregex_iterator match(folded.begin(), folded.end(), russian), end; match != end; ++match) {
		DurationRule rule;
		rule.count = std::stoll((*match)[1].str());
		rule.business = (*match)[2].matched && (*match)[2].str().rfind("\xD1\x80\xD0\xB0\xD0\xB1", 0) == 0;
		rule.end = static_cast<std::size_t>(match->position() + match->length());
		rule.written = sentence.substr(static_cast<std::size_t>(match->position()), static_cast<std::size_t>(match->length()));
		found.push_back(rule);
	}
	for (DurationRule &rule : found) {
		const std::string after = folded.substr(rule.end, 24);
		rule.before = after.rfind(" before", 0) == 0 || after.rfind(" prior to", 0) == 0
			|| after.rfind(" \xD0\xB4\xD0\xBE ", 0) == 0;
	}
	return found;
}

struct DateMention {
	std::size_t row = 0;
	double score = 0.0;
	std::size_t first = 0;
	// Written in running text or asked about, not only in a table's cells.
	bool outside_cells = false;
};

std::string order_words(long long left, long long right) {
	return left < right ? " is before " : left > right ? " is after " : " is the same day as ";
}

// Why no business day is counted, in closed words.
std::string unknown_rule_words(const business_calendar::GoverningRules &rules) {
	if (rules.unknown_reason == "rule_conflict")
		return "business days not counted: the material defines Business Day in two ways that disagree";
	if (rules.unknown_reason == "definition_not_found")
		return "business days not counted: the definition of Business Day the agreement refers to is not in the material";
	return "business days not counted: the rule that governs them is not known";
}

void date_relations(Builder &out, const std::vector<Sentence> &sentences, const std::vector<Sheet> &sheets) {
	const Rows &rows = out.rows;
	const auto dayOf = [&](std::size_t row) { return rows.value(row)->date.first_day; };
	// Every day a passage writes, and how much it matters to the question.
	std::map<std::size_t, DateMention> mentions;
	std::size_t order = 0;
	for (const Sentence &sentence : sentences) {
		const bool shares = [&]() {
			std::size_t shared = 0;
			for (const std::string &stem : content_stems(sentence.folded))
				if (out.questionStems.count(stem) != 0) ++shared;
			return shared >= 2;
		}();
		for (const Marker &marker : sentence.markers) {
			if (!rows.is_day(marker.row)) continue;
			DateMention &mention = mentions[marker.row];
			if (mention.score == 0.0 && mention.first == 0) mention.first = ++order;
			mention.row = marker.row;
			mention.outside_cells = mention.outside_cells || !sentence.cells;
			mention.score += (sentence.rule ? 2.0 : 0.0) + (shares ? 1.0 : 0.0) + (sentence.cells ? 0.1 : 0.3);
		}
	}
	for (const std::size_t row : out.questionRows)
		if (rows.is_day(row)) {
			DateMention &mention = mentions[row];
			mention.row = row;
			mention.outside_cells = true;
			mention.score += 3.0;
			if (mention.first == 0) mention.first = ++order;
		}
	if (mentions.empty()) return;
	std::vector<DateMention> key;
	for (const auto &[row, mention] : mentions) key.push_back(mention);
	std::sort(key.begin(), key.end(), [](const DateMention &a, const DateMention &b) {
		return a.score != b.score ? a.score > b.score : a.first < b.first;
	});
	if (key.size() > 10) key.resize(10);

	// Thresholds a pair is compared with: what the question asks about, and the
	// business-day periods the rules set - rare, and what a count in business
	// days is for. A calendar-day period of some other rule is compared only
	// with the dates of its own sentence.
	std::vector<DurationRule> thresholds;
	std::set<std::pair<long long, bool>> seenThreshold;
	const auto keepThresholds = [&](const std::string &text, bool businessOnly) {
		for (const DurationRule &rule : durations_in(text))
			if (rule.count > 0 && (!businessOnly || rule.business) && seenThreshold.insert({rule.count, rule.business}).second)
				thresholds.push_back(rule);
	};
	keepThresholds(out.input.question, false);
	for (const Sentence &sentence : sentences)
		if (sentence.rule && !sentence.cells) keepThresholds(sentence.text, true);
	std::map<std::size_t, std::vector<DurationRule>> ownThresholds;
	for (const Sentence &sentence : sentences) {
		if (sentence.cells) continue;
		const std::vector<DurationRule> own = durations_in(sentence.text);
		if (own.empty()) continue;
		for (const Marker &marker : sentence.markers)
			if (rows.is_day(marker.row)) ownThresholds[marker.row].insert(ownThresholds[marker.row].end(), own.begin(), own.end());
	}

	// The pairs: dates one line writes together, the documents' own dates in
	// order, and the key dates in order.
	std::vector<std::pair<std::size_t, std::size_t>> pairs;
	std::set<std::pair<std::size_t, std::size_t>> seen;
	// Why a pair is worth a line: its dates stand in one sentence, one document's
	// own chronology, the documents' own dates in turn, or the question.
	std::map<std::pair<std::size_t, std::size_t>, double> reason;
	// A ledger's dates one after another are its rows, not events: a pair needs
	// one date the text or the question writes outside a table.
	const auto outside = [&](std::size_t row) {
		const auto found = mentions.find(row);
		return found != mentions.end() && found->second.outside_cells;
	};
	const auto pair = [&](std::size_t a, std::size_t b, double weight = 0.0) {
		if (a == b || dayOf(a) == dayOf(b) || (!outside(a) && !outside(b))) return;
		if (dayOf(a) > dayOf(b)) std::swap(a, b);
		if (seen.insert({a, b}).second) pairs.emplace_back(a, b);
		reason[{a, b}] += weight;
	};
	for (const Sentence &sentence : sentences) {
		if (sentence.cells) continue;
		std::vector<std::size_t> days;
		for (const Marker &marker : sentence.markers)
			if (rows.is_day(marker.row) && std::find(days.begin(), days.end(), marker.row) == days.end()) days.push_back(marker.row);
		for (std::size_t index = 0; index + 1 < days.size() && index < 2; ++index) pair(days[index], days[index + 1], 2.0);
	}
	// One document's own chronology: an email thread's notice, reply and repair.
	std::map<std::size_t, std::vector<std::size_t>> byPassage;
	for (const Sentence &sentence : sentences) {
		if (sentence.cells) continue;
		std::vector<std::size_t> &days = byPassage[sentence.passage];
		for (const Marker &marker : sentence.markers)
			if (rows.is_day(marker.row) && std::find(days.begin(), days.end(), marker.row) == days.end()) days.push_back(marker.row);
	}
	for (auto &[passage, days] : byPassage) {
		if (days.size() < 2 || days.size() > 8) continue;
		std::sort(days.begin(), days.end(), [&](std::size_t a, std::size_t b) { return dayOf(a) < dayOf(b); });
		for (std::size_t index = 0; index + 1 < days.size(); ++index) pair(days[index], days[index + 1], 3.0);
		if (days.size() > 2) pair(days.front(), days.back(), 3.0);
	}
	// A document's own date: its "Date:" line, what it is "made as of" or
	// "dated", or a date standing alone on one of its first lines.
	std::map<std::size_t, std::size_t> documentDates;
	for (const Sentence &sentence : sentences) {
		if (sentence.cells || documentDates.count(sentence.passage) != 0) continue;
		const std::string trimmed = sentence.folded.substr(0, 40);
		for (const Marker &marker : sentence.markers) {
			if (!rows.is_day(marker.row)) continue;
			const std::string before = fold(sentence.text.substr(marker.start >= 16 ? marker.start - 16 : 0,
				marker.start >= 16 ? 16 : marker.start));
			const bool alone = sentence.line <= 3 && sentence.markers.size() == 1
				&& sentence.text.find_first_not_of(" ", marker.end) == std::string::npos && marker.start <= 1;
			if (trimmed.rfind("date:", 0) == 0 || alone || contains_any(before, {"made as of ", "made on ", "dated ",
					"\xD0\xBE\xD1\x82 "})) {
				documentDates[sentence.passage] = marker.row;
				break;
			}
		}
	}
	std::vector<std::size_t> documents;
	for (const auto &[passage, row] : documentDates)
		if (std::find(documents.begin(), documents.end(), row) == documents.end()) documents.push_back(row);
	std::sort(documents.begin(), documents.end(), [&](std::size_t a, std::size_t b) { return dayOf(a) < dayOf(b); });
	for (std::size_t index = 0; index + 1 < documents.size(); ++index) pair(documents[index], documents[index + 1], 1.0);
	std::vector<std::size_t> chronological;
	for (const DateMention &mention : key) chronological.push_back(mention.row);
	std::sort(chronological.begin(), chronological.end(), [&](std::size_t a, std::size_t b) { return dayOf(a) < dayOf(b); });
	for (std::size_t index = 0; index + 1 < chronological.size(); ++index) pair(chronological[index], chronological[index + 1]);
	// And one further: a notice, the reply, the repair - the rule is often about
	// the first and the last.
	for (std::size_t index = 0; index + 2 < chronological.size() && index < 6; ++index)
		pair(chronological[index], chronological[index + 2]);
	for (const std::size_t row : out.questionRows)
		if (rows.is_day(row))
			for (const DateMention &mention : key) pair(row, mention.row, 2.0);
	// A date written everywhere (a lease's own date) is not what every pair is
	// about: its weight is capped, and why the pair exists counts as much.
	const auto scoreOf = [&](std::size_t row) {
		const auto found = mentions.find(row);
		return found == mentions.end() ? 0.0 : std::min(found->second.score, 2.5);
	};
	std::stable_sort(pairs.begin(), pairs.end(), [&](const auto &a, const auto &b) {
		return scoreOf(a.first) + scoreOf(a.second) + reason[a] > scoreOf(b.first) + scoreOf(b.second) + reason[b];
	});
	if (pairs.size() > out.input.limits.date_pairs) pairs.resize(out.input.limits.date_pairs);
	// The rule business days between two dates are counted under: the
	// material's own (a definition of Business Day), else a rule the lawyer or
	// the law gives, else weekdays with the holidays not known.
	const business_calendar::GoverningRules &between = out.rules(std::nullopt);
	for (const auto &[a, b] : pairs) {
		const long long from = dayOf(a);
		const long long to = dayOf(b);
		const long long days = to - from;
		const std::string pairKey = rows.symbol(a) + "|" + rows.symbol(b);
		std::string line = out.duration(days, semantic::DurationUnit::Day, "days|" + pairKey) + " calendar days";
		// What the business days are: exactly, at most (whatever holidays the rule
		// does not name - a holiday only ever takes a day away), or not counted.
		std::optional<long long> exactBusiness;
		std::optional<long long> atMost;
		std::optional<long long> atLeast;
		if (between.business_day) {
			const business_calendar::CountBound bound = business_calendar::count_bound(*between.business_day, from, to);
			exactBusiness = bound.exact;
			if (!bound.exact) {
				atMost = bound.at_most;
				if (bound.at_least > 0) atLeast = bound.at_least;
			}
			const std::string counted = exactBusiness
				? out.duration(*exactBusiness, semantic::DurationUnit::BusinessDay, "bdays|" + pairKey + "|"
					+ business_calendar::rule_name(*between.business_day))
				: out.duration(*atMost, semantic::DurationUnit::BusinessDay, "bdays_at_most|" + pairKey + "|"
					+ business_calendar::rule_name(*between.business_day));
			line += std::string(" and ") + (exactBusiness ? "" : "at most ") + counted + " business days ("
				+ business_calendar::explain(*between.business_day, bound.assumptions) + ")";
		}
		line += " from " + rows.symbol(a) + " to " + rows.symbol(b);
		if (!between.business_day) line += "; " + unknown_rule_words(between);
		std::size_t compared = 0;
		std::vector<DurationRule> compareWith = thresholds;
		std::set<std::pair<long long, bool>> taken(seenThreshold);
		for (const std::size_t end : {a, b})
			for (const DurationRule &rule : ownThresholds[end])
				if (rule.count > 0 && taken.insert({rule.count, rule.business}).second) compareWith.push_back(rule);
		for (const DurationRule &threshold : compareWith) {
			if (compared >= out.input.limits.thresholds_per_pair) break;
			std::string said;
			if (!threshold.business) {
				said = (days < threshold.count ? "fewer than " : days > threshold.count ? "more than " : "exactly ")
					+ std::to_string(threshold.count) + " days";
			} else if (exactBusiness) {
				said = (*exactBusiness < threshold.count ? "fewer than " : *exactBusiness > threshold.count ? "more than "
					: "exactly ") + std::to_string(threshold.count) + " business days";
			} else if (atMost && *atMost < threshold.count) {
				// Fewer whatever the holidays are: they can only take days away.
				said = "fewer than " + std::to_string(threshold.count) + " business days whatever the holidays";
			} else if (atLeast && *atLeast >= threshold.count) {
				// At least that many whatever the uncertain days turn out to be.
				said = (*atLeast > threshold.count ? "more than " : "at least ") + std::to_string(threshold.count)
					+ " business days whatever the holidays";
			} else {
				said = "whether it is " + std::to_string(threshold.count) + " business days or more is not determined";
			}
			line += std::string(compared == 0 ? "; " : ", ") + said;
			++compared;
		}
		out.emit(Kind::DaysBetween, line, {a, b}, 1.0, between.business_day);
	}

	// Deadlines a rule sets from a date: "within ten (10) days of this
	// notice", «не позднее 10 календарных дней после окончания квартала».
	std::vector<long long> allDays;
	std::map<long long, std::size_t> rowOfDay;
	for (const Sentence &sentence : sentences)
		for (const Marker &marker : sentence.markers)
			if (rows.is_day(marker.row) && rowOfDay.emplace(dayOf(marker.row), marker.row).second) allDays.push_back(dayOf(marker.row));
	for (const std::size_t row : out.questionRows)
		if (rows.is_day(row) && rowOfDay.emplace(dayOf(row), row).second) allDays.push_back(dayOf(row));
	std::sort(allDays.begin(), allDays.end());
	struct Deadline {
		long long day;
		std::string symbol;
		std::string how;
		std::size_t anchor;
	};
	std::vector<Deadline> deadlines;
	std::set<std::string> seenDeadline;
	std::size_t deadlineLines = 0;
	for (const Sentence &sentence : sentences) {
		if (!sentence.rule || sentence.cells || deadlineLines >= 8) continue;
		for (const DurationRule &rule : durations_in(sentence.text)) {
			if (rule.count <= 0 || rule.count > 3660) continue;
			const std::string rest = sentence.folded.substr(rule.end, 120);
			std::string preposition;
			for (const char *word : {" after ", " of ", " from ", " following ", " before ", " prior to ",
					" \xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5 ", " \xD1\x81 ", " \xD1\x81\xD0\xBE ", " \xD0\xBE\xD1\x82 ",
					" \xD0\xB4\xD0\xBE "})
				if (rest.rfind(word, 0) == 0) { preposition = word; break; }
			if (preposition.empty()) continue;
			// The anchor: a day the rest of the sentence writes; the end of a period
			// it names; this document's own date.
			std::optional<std::size_t> anchor;
			bool periodEnd = false;
			for (const Marker &marker : sentence.markers) {
				if (marker.start < rule.end || marker.start > rule.end + 120) continue;
				const Value *value = rows.value(marker.row);
				if (value == nullptr || value->kind != VKind::Date) continue;
				anchor = marker.row;
				periodEnd = value->date.precision != temporal::Precision::Day;
				break;
			}
			const bool endWords = contains_any(rest, {"end of", "\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x87\xD0\xB0\xD0\xBD",
				"\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x86", "\xD0\xB8\xD1\x81\xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD"});
			if (!anchor && endWords) {
				// The end of a period a workbook labels: "Конец квартала", "Quarter end".
				std::string period;
				for (const char *noun : {"quarter", "month", "year", "\xD0\xBA\xD0\xB2\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0\xD0\xBB",
						"\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86", "\xD0\xB3\xD0\xBE\xD0\xB4"})
					if (rest.find(noun) != std::string::npos) { period = noun; break; }
				std::vector<std::size_t> labelled;
				for (const Sheet &sheet : sheets)
					for (const SheetRow &row : sheet.rows) {
						const std::string label = fold(row.label());
						if (period.empty() || label.find(period) == std::string::npos
								|| !contains_any(label, {"end", "\xD0\xBA\xD0\xBE\xD0\xBD\xD0\xB5\xD1\x86",
									"\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x87", "last", "\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4"}))
							continue;
						for (const Cell &cell : row.cells)
							if (cell.symbol && rows.is_day(*cell.symbol)
									&& std::find(labelled.begin(), labelled.end(), *cell.symbol) == labelled.end())
								labelled.push_back(*cell.symbol);
					}
				if (labelled.size() == 1) anchor = labelled.front();
			}
			if (!anchor && contains_any(rest, {"this notice", "this letter", "this demand", "\xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD1\x89"})) {
				for (const Sentence &other : sentences)
					if (other.passage == sentence.passage && other.folded.rfind("date:", 0) == 0)
						for (const Marker &marker : other.markers)
							if (rows.is_day(marker.row)) { anchor = marker.row; break; }
			}
			if (!anchor) continue;
			const Value *anchorValue = rows.value(*anchor);
			const bool before = rule.before;
			const long long from = periodEnd ? (before ? anchorValue->date.first_day : anchorValue->date.last_day)
				: anchorValue->date.first_day;
			// The rule of the sentence's own document governs its business days.
			const business_calendar::GoverningRules &governing = out.rules(sentence.passage);
			const std::optional<business_calendar::BusinessDayRule> &dayRule = governing.business_day;
			const std::string how = std::to_string(rule.count) + (rule.business ? " business days " : " calendar days ")
				+ (before ? "before " : "after ") + (periodEnd ? (before ? "the start of " : "the end of ") : "") + rows.symbol(*anchor)
				+ (rule.business && dayRule ? " (" + business_calendar::describe(*dayRule) + ")" : "");
			if (!seenDeadline.insert(how).second) continue;
			++deadlineLines;
			if (rule.business && !dayRule) {
				out.emit(Kind::Deadline, how + ", by the rule '" + rule.written + "': the day is not worked out; "
					+ unknown_rule_words(governing), {*anchor}, 1.5);
				continue;
			}
			// A business-day deadline under a rule whose closures are not all known
			// is only bounded: counted forward, never before the day the uncertain
			// days counted as business days give, and - where the certain days reach
			// it - never after the day they alone give. A calendar-day period ends
			// on its day, moved by the operative document's roll; a day that is not
			// a business day with no roll known is a bound, never exact.
			// A roll clause the contract limits to payments reaches this period only
			// when the rule's own sentence is about paying.
			const bool payment = contains_any(sentence.folded, {"pay", "\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82",  // оплат
				"\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82\xD0\xB5\xD0\xB6", "\xD1\x83\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82",  // платеж уплат
				"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB"});  // перечисл
			business_calendar::DayBound bound;
			if (rule.business) bound = business_calendar::deadline_bound(*dayRule, from, before ? -rule.count : rule.count);
			else bound = business_calendar::calendar_period_end(dayRule ? &*dayRule : nullptr, from + (before ? -rule.count : rule.count),
				before, payment);
			if (!bound.exact && !bound.not_before && !bound.not_after) continue;
			// The side a bound certainly holds: the earliest counted forward, the
			// latest counted back.
			const bool forward = bound.exact || (!before && bound.not_before) || (before && !bound.not_after);
			const long long day = bound.exact ? *bound.exact : forward ? *bound.not_before : *bound.not_after;
			const std::string symbol = out.date(day, std::string(bound.exact ? "deadline|" : forward
				? "deadline_not_before|" : "deadline_not_after|") + how);
			std::string line;
			if (bound.exact) {
				line = symbol + " = " + how;
			} else {
				line = how + (forward ? " falls on or after " : " falls on or before ") + symbol;
				const std::optional<long long> other = forward ? bound.not_after : bound.not_before;
				if (other)
					line += (forward ? " and on or before " : " and on or after ") + out.date(*other, std::string(forward
						? "deadline_not_after|" : "deadline_not_before|") + how);
			}
			if (!bound.assumptions.empty() || (!bound.exact && rule.business)) {
				std::string said;
				for (const std::string &one : bound.assumptions) said += (said.empty() ? "" : "; ") + one;
				if (said.empty()) said = "the holidays that would move it are not known";
				line += " (" + said + ")";
			}
			line += ", by the rule '" + rule.written + "'";
			// Where the deadline stands among the request's days - only what the
			// bound decides.
			const auto upper = std::upper_bound(allDays.begin(), allDays.end(), day);
			const auto same = rowOfDay.find(day);
			if (bound.exact) {
				deadlines.push_back({day, symbol, how, *anchor});
				if (same != rowOfDay.end()) line += "; it is the same day as " + rows.symbol(same->second);
				else {
					if (upper != allDays.begin()) line += "; it is after " + rows.symbol(rowOfDay.at(*(upper - 1)));
					if (upper != allDays.end())
						line += std::string(upper != allDays.begin() ? " and" : ";") + " before " + rows.symbol(rowOfDay.at(*upper));
				}
			} else if (forward) {
				const auto below = std::lower_bound(allDays.begin(), allDays.end(), day);
				if (below != allDays.begin()) line += "; it is after " + rows.symbol(rowOfDay.at(*(below - 1)));
			} else if (upper != allDays.end()) {
				line += "; it is before " + rows.symbol(rowOfDay.at(*upper));
			}
			out.emit(Kind::Deadline, line, {*anchor}, 1.5, dayRule);
			if (!bound.exact || !dayRule || !rule.business) continue;
			// Whether a business-day deadline's day is itself a business day - a
			// calendar-day period's own day is settled above, by its roll.
			const business_calendar::DayStatus dayStatus = business_calendar::status(*dayRule, day);
			if (dayStatus != business_calendar::DayStatus::Weekend && dayStatus != business_calendar::DayStatus::Holiday) continue;
			const business_calendar::DayBound next = business_calendar::next_business_day_bound(*dayRule, day);
			if (!next.exact && !next.not_before) continue;
			const long long nextDay = next.exact ? *next.exact : *next.not_before;
			out.emit(Kind::NextBusinessDay, symbol + " falls on " + (dayStatus == business_calendar::DayStatus::Weekend
					? "a weekend" : "a holiday") + " (" + business_calendar::explain(*dayRule, next.assumptions) + "); the next business day is "
					+ (next.exact ? std::string() : "on or after ") + out.date(nextDay, std::string(next.exact ? "nextbusiness|"
						: "nextbusiness_not_before|") + how),
				{*anchor}, 1.5, dayRule);
		}
	}
	// A workbook cell that states a deadline, against the deadline a rule sets.
	for (const Sheet &sheet : sheets)
		for (const SheetRow &row : sheet.rows) {
			const std::string label = fold(row.label());
			if (!contains_any(label, {"deadline", "cut-off", "cutoff", "due date", "\xD0\xBA\xD1\x80\xD0\xB0\xD0\xB9\xD0\xBD",
					"\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA"}))
				continue;
			for (const Cell &cell : row.cells) {
				if (!cell.symbol || !rows.is_day(*cell.symbol)) continue;
				for (const Deadline &deadline : deadlines) {
					if (deadline.anchor == *cell.symbol) continue;
					out.emit(Kind::RuleVsCell, deadline.symbol + " (" + deadline.how + ")" + order_words(deadline.day, dayOf(*cell.symbol))
							+ rows.symbol(*cell.symbol) + " (cell " + cell.ref + ", " + utf8_prefix(row.label(), 48) + ")",
						{*cell.symbol, deadline.anchor}, 2.0);
				}
			}
		}
}

// ---------------------------------------------------------------------------
// (d) Sums the question asks for

void named_sums(Builder &out, const std::vector<Marker> &questionMarkers, const std::vector<Marker> &answerMarkers) {
	const std::string question = fold(out.input.question);
	if (!contains_any(question, {"total", "in sum", "aggregate", "altogether", "combined", "\xD0\xB8\xD1\x82\xD0\xBE\xD0\xB3",
			"\xD0\xB2\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE", "\xD0\xBE\xD0\xB1\xD1\x89\xD0\xB0\xD1\x8F \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC",
			"\xD0\xBE\xD0\xB1\xD1\x89\xD1\x83\xD1\x8E \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC", "\xD0\xB2 \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC\xD0\xB5",
			"\xD1\x81\xD1\x83\xD0\xBC\xD0\xBC\xD0\xB0\xD1\x80"}))
		return;
	const Rows &rows = out.rows;
	for (const std::vector<Marker> *markers : {&questionMarkers, &answerMarkers}) {
		std::vector<std::size_t> amounts;
		for (const Marker &marker : *markers)
			if (rows.is(marker.row, VKind::Money) && std::find(amounts.begin(), amounts.end(), marker.row) == amounts.end())
				amounts.push_back(marker.row);
		if (amounts.size() < 2 || amounts.size() > 8) continue;
		std::set<std::string> currencies;
		for (const std::size_t row : amounts) currencies.insert(rows.value(row)->currency);
		if (currencies.size() != 1 || currencies.begin()->empty()) {
			out.emit(Kind::NamedSum, "the amounts are in different currencies and are not added up", amounts, 1.0);
			return;
		}
		Rational sum;
		std::string terms;
		for (const std::size_t row : amounts) {
			sum = sum + rows.value(row)->amount;
			terms += (terms.empty() ? "" : " + ") + rows.symbol(row);
		}
		try {
			out.emit(Kind::NamedSum, out.money(semantic::make_money(sum, *currencies.begin()), "sum|" + terms) + " = " + terms
					+ " (" + *currencies.begin() + ")", amounts, 2.0);
		} catch (const semantic::AlgebraError &) {
		}
		return;
	}
}

// ---------------------------------------------------------------------------
// (e) Whose contact details

const char *contact_word(const RequestMappingEntry &entry) {
	switch (entry.type.value_or(EntityType::Address)) {
		case EntityType::Email: return "email";
		case EntityType::Phone: return "phone";
		case EntityType::BankAccount: return "account";
		default: return "address";
	}
}

// A line of contact details alone - a letterhead's address, a signature
// block's phone - not a sentence that mentions one.
bool short_contact_line(const Sentence &sentence) {
	std::size_t words = 0;
	std::size_t at = 0;
	for (const Marker &marker : sentence.markers) {
		for (std::size_t index = at; index < marker.start; ++index)
			if (std::isalpha(static_cast<unsigned char>(sentence.text[index])) || static_cast<unsigned char>(sentence.text[index]) >= 0x80) ++words;
		at = marker.end;
	}
	for (std::size_t index = at; index < sentence.text.size(); ++index)
		if (std::isalpha(static_cast<unsigned char>(sentence.text[index])) || static_cast<unsigned char>(sentence.text[index]) >= 0x80) ++words;
	return words <= 24;
}

void contact_owners(Builder &out, const std::vector<Sentence> &sentences) {
	const Rows &rows = out.rows;
	// contact row -> its owner, as "anchor" words; "" once two places disagree.
	std::map<std::size_t, std::string> owner;
	std::map<std::size_t, std::vector<std::size_t>> ownerRow;
	std::map<std::size_t, std::string> context;
	const auto link = [&](std::size_t contact, const std::string &anchor, const std::vector<std::size_t> &anchorRow,
			const std::string &where) {
		const auto found = owner.find(contact);
		if (found == owner.end()) {
			owner[contact] = anchor;
			ownerRow[contact] = anchorRow;
			context[contact] = where;
		} else if (found->second != anchor) {
			found->second.clear();
		}
	};
	std::optional<std::pair<std::string, std::vector<std::size_t>>> previous;
	std::size_t previousPassage = static_cast<std::size_t>(-1);
	std::size_t previousLine = 0;
	for (const Sentence &sentence : sentences) {
		if (sentence.cells) {
			previous.reset();
			continue;
		}
		std::vector<std::size_t> people;
		std::vector<std::size_t> organisations;
		std::vector<std::size_t> contacts;
		for (const Marker &marker : sentence.markers) {
			const auto &type = rows[marker.row].type;
			if (rows.is_contact(marker.row)) contacts.push_back(marker.row);
			else if (type && (*type == EntityType::Person || *type == EntityType::Judge)) {
				if (std::find(people.begin(), people.end(), marker.row) == people.end()) people.push_back(marker.row);
			} else if (rows.is_party(marker.row)) {
				if (std::find(organisations.begin(), organisations.end(), marker.row) == organisations.end())
					organisations.push_back(marker.row);
			}
		}
		const std::string where = contains_any(sentence.folded, {"notice", "\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4"})
			? "notice details" : sentence.folded.rfind("from:", 0) == 0 || sentence.folded.rfind("to:", 0) == 0
			? "message header" : "the same line";
		std::optional<std::pair<std::string, std::vector<std::size_t>>> anchor;
		if (people.size() + organisations.size() == 1) {
			const std::size_t row = people.empty() ? organisations.front() : people.front();
			anchor = std::make_pair(rows.symbol(row), std::vector<std::size_t>{row});
		} else if (organisations.size() == 1 && people.size() == 1) {
			anchor = std::make_pair(rows.symbol(organisations.front()) + " (attention " + rows.symbol(people.front()) + ")",
				std::vector<std::size_t>{organisations.front(), people.front()});
		}
		if (anchor) {
			for (const std::size_t contact : contacts) link(contact, anchor->first, anchor->second, where);
			previous = anchor;
		} else if (people.empty() && organisations.empty() && previous && sentence.passage == previousPassage
				&& sentence.line <= previousLine + 2 && short_contact_line(sentence)) {
			// A letterhead's or a signature block's next lines belong to the name above them.
			for (const std::size_t contact : contacts) link(contact, previous->first, previous->second, "the block under its name");
		} else {
			previous.reset();
		}
		previousPassage = sentence.passage;
		previousLine = sentence.line;
	}
	std::map<std::string, std::vector<std::size_t>> byOwner;
	for (const auto &[contact, anchor] : owner)
		if (!anchor.empty()) byOwner[anchor].push_back(contact);
	for (const auto &[anchor, contacts] : byOwner) {
		std::string line = "contact details of " + anchor + ":";
		std::vector<std::size_t> operands = ownerRow.at(contacts.front());
		bool first = true;
		for (const std::size_t contact : contacts) {
			line += std::string(first ? " " : ", ") + rows.symbol(contact) + " (" + contact_word(rows[contact]) + ")";
			operands.push_back(contact);
			first = false;
		}
		line += " - from " + context.at(contacts.front());
		out.emit(Kind::ContactOwner, line, operands, 0.5);
	}
}

// ---------------------------------------------------------------------------
// (f) Equal values: local knowledge, sent only when the task needs it

// Two facts of one value are two symbols (owner correction, 26 Sep 2026):
// that they are equal is a relation Singularity knows here. A figure the lawyer
// wrote in the request is related to the facts of the material it equals - to
// those whose sentence shares a word with the request when any does, else to
// every one of the same unit. Facts of the material equal to one another are
// said only pairwise between facts the question names by their roles, and only
// for a question that compares or checks a sheet (plan()).
void value_equalities(Builder &out, const std::vector<Sentence> &sentences) {
	const Rows &rows = out.rows;
	const auto quantity = [&](std::size_t row) {
		const Value *value = rows.value(row);
		return value != nullptr && value->kind != VKind::Boolean;
	};
	std::map<std::string, std::vector<std::size_t>> byValue;
	std::map<std::size_t, std::set<std::string>> stemsOf;
	for (const Sentence &sentence : sentences)
		for (const Marker &marker : sentence.markers) {
			if (!quantity(marker.row) || out.questionRows.count(marker.row) != 0) continue;
			std::vector<std::size_t> &same = byValue[(*rows.mapping)[marker.row].value_key];
			if (std::find(same.begin(), same.end(), marker.row) == same.end()) same.push_back(marker.row);
			const std::set<std::string> stems = content_stems(sentence.folded);
			stemsOf[marker.row].insert(stems.begin(), stems.end());
		}
	for (const std::size_t asked : out.questionRows) {
		if (!quantity(asked)) continue;
		const auto equal = byValue.find((*rows.mapping)[asked].value_key);
		if (equal == byValue.end() || equal->second.empty()) continue;
		std::vector<std::size_t> sharing;
		for (const std::size_t row : equal->second) {
			const std::set<std::string> &stems = stemsOf[row];
			if (std::any_of(stems.begin(), stems.end(), [&](const std::string &stem) { return out.questionStems.count(stem) != 0; }))
				sharing.push_back(row);
		}
		const std::vector<std::size_t> &related = sharing.empty() ? equal->second : sharing;
		for (std::size_t index = 0; index < related.size() && index < 4; ++index) {
			Relation &relation = out.emit(Kind::ValueEquality, rows.symbol(asked) + " (written in the request) equals "
				+ rows.symbol(related[index]), {asked, related[index]}, 2.5);
			relation.lawyer_value = true;
		}
	}
	// Facts of the material equal to one another: only the facts the question
	// itself asks about - each named by its role in words the question uses
	// ("Is the liability cap equal to the purchase price?") - and only pairwise.
	// Equal values the question does not name stay here: a price, a cap and an
	// insurance limit of $5m say nothing to "Is the escrow above the threshold?".
	const auto asked_about = [&](std::size_t row) {
		const std::string &role = (*rows.mapping)[row].provenance.role;
		if (role.empty()) return false;
		const std::set<std::string> stems = content_stems(fold(role));
		return std::any_of(stems.begin(), stems.end(), [&](const std::string &stem) { return out.questionStems.count(stem) != 0; });
	};
	// Equal facts as a chain - A and B, B and C: every other pair follows, and
	// no fact is told equal three times over (live EN1 c22: four equal figures
	// sent as six pairs spent their budget before the model asked anything).
	for (const auto &[key, same] : byValue) {
		std::vector<std::size_t> named;
		for (const std::size_t row : same)
			if (asked_about(row)) named.push_back(row);
		for (std::size_t first = 0; first + 1 < named.size() && first + 1 < 4; ++first)
			out.emit(Kind::ValueEquality, rows.symbol(named[first]) + " and " + rows.symbol(named[first + 1]) + " are equal in value",
				{named[first], named[first + 1]}, 0.5);
	}
}

// ---------------------------------------------------------------------------
// Intents

bool any_word(const std::string &folded, std::initializer_list<const char *> words) {
	return contains_any(folded, words);
}

} // namespace

const char *to_string(Kind kind) {
	switch (kind) {
		case Kind::ColumnTotal: return "column_total";
		case Kind::RowBalance: return "row_balance";
		case Kind::HiddenRow: return "hidden_row";
		case Kind::Escalation: return "escalation";
		case Kind::StepOnDueDate: return "step_on_due_date";
		case Kind::EscalatedVsLedger: return "escalated_vs_ledger";
		case Kind::DaysBetween: return "days_between";
		case Kind::Deadline: return "deadline";
		case Kind::NextBusinessDay: return "next_business_day";
		case Kind::DateOrder: return "date_order";
		case Kind::RuleVsCell: return "rule_vs_cell";
		case Kind::NamedSum: return "named_sum";
		case Kind::ValueEquality: return "value_equality";
		case Kind::ContactOwner: return "contact_owner";
	}
	return "relation";
}

const char *to_string(Intent intent) {
	switch (intent) {
		case Intent::Total: return "total";
		case Intent::Shortfall: return "shortfall";
		case Intent::Compare: return "compare";
		case Intent::Deadline: return "deadline";
		case Intent::BusinessDays: return "business_days";
		case Intent::DaysBetween: return "days_between";
		case Intent::Escalation: return "escalation";
		case Intent::Contact: return "contact";
		case Intent::Chronology: return "chronology";
		case Intent::SheetCheck: return "sheet_check";
	}
	return "total";
}

std::vector<Intent> intents_of(std::string_view question) {
	const std::string folded = fold(question);
	std::vector<Intent> intents;
	const auto add = [&](Intent intent, bool present) {
		if (present && std::find(intents.begin(), intents.end(), intent) == intents.end()) intents.push_back(intent);
	};
	add(Intent::Total, any_word(folded, {"total", "in sum", "sum of", "aggregate", "altogether", "combined", "in all",
		"\xD0\xB8\xD1\x82\xD0\xBE\xD0\xB3",                                             // итог
		"\xD0\xB2\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE",                                     // всего
		"\xD0\xBE\xD0\xB1\xD1\x89\xD0\xB0\xD1\x8F \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC",     // общая сумм
		"\xD0\xBE\xD0\xB1\xD1\x89\xD1\x83\xD1\x8E \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC",     // общую сумм
		"\xD0\xB2 \xD1\x81\xD1\x83\xD0\xBC\xD0\xBC\xD0\xB5",                            // в сумме
		"\xD1\x81\xD1\x83\xD0\xBC\xD0\xBC\xD0\xB0\xD1\x80"}));                           // суммар
	add(Intent::Shortfall, any_word(folded, {"short", "underpa", "underbill", "owe", "outstanding", "balance", "arrear",
		"unpaid", "deficien", "overpa", "overdue", "how much",
		"\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBF\xD0\xBB\xD0\xB0\xD1\x82",             // недоплат
		"\xD0\xB7\xD0\xB0\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB5\xD0\xBD",             // задолжен
		"\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB3",                                             // долг
		"\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD0\xBE\xD0\xBA",                     // остаток
		"\xD1\x81\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xB4\xD0\xBE",                             // сальдо
		"\xD1\x81\xD0\xBA\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xBA\xD0\xBE"}));                   // сколько
	add(Intent::Compare, any_word(folded, {"compare", "more than", "less than", "exceed", "greater", "higher", "lower",
		"below", "above", "at least", "at most", "enough", "meet", "threshold", "equal", "match", "same as", "within the cap",
		"\xD0\xB1\xD0\xBE\xD0\xBB\xD1\x8C\xD1\x88\xD0\xB5",                             // больше
		"\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8C\xD1\x88\xD0\xB5",                             // меньше
		"\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB2\xD1\x8B\xD1\x88",                             // превыш
		"\xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xB5",                    // не менее
		"\xD0\xBD\xD0\xB5 \xD0\xB1\xD0\xBE\xD0\xBB\xD0\xB5\xD0\xB5",                    // не более
		"\xD0\xB4\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD0\xBE\xD1\x87",             // достаточ
		"\xD1\x81\xD0\xBE\xD0\xBE\xD1\x82\xD0\xB2\xD0\xB5\xD1\x82\xD1\x81\xD1\x82\xD0\xB2", // соответств
		"\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBD"}));                                          // равн
	const bool penalty = any_word(folded, {"penalt", "interest", "late fee", "default rate", "per day", "daily",
		"\xD0\xBD\xD0\xB5\xD1\x83\xD1\x81\xD1\x82\xD0\xBE\xD0\xB9\xD0\xBA",             // неустойк
		"\xD0\xBF\xD0\xB5\xD0\xBD\xD0\xB8",                                             // пени
		"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82"});                    // процент
	add(Intent::Deadline, penalty || any_word(folded, {"deadline", "due", "timely", "on time", "late", "by when",
		"no later", "within", "expire", "cure", "notice period", "cut-off", "cutoff",
		"\xD1\x81\xD1\x80\xD0\xBE\xD0\xBA",                                             // срок
		"\xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB5",    // не позднее
		"\xD0\xB2 \xD1\x82\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",            // в течение
		"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD1\x80\xD0\xBE\xD1\x87",                     // просроч
		"\xD0\xB2\xD0\xBE\xD0\xB2\xD1\x80\xD0\xB5\xD0\xBC\xD1\x8F",                     // вовремя
		"\xD1\x81\xD0\xB2\xD0\xBE\xD0\xB5\xD0\xB2\xD1\x80\xD0\xB5\xD0\xBC\xD0\xB5\xD0\xBD", // своевремен
		"\xD0\xB8\xD1\x81\xD1\x82\xD0\xB5\xD0\xBA"}));                                  // истек
	add(Intent::BusinessDays, any_word(folded, {"business day", "working day", "banking day",
		"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD1\x85 \xD0\xB4\xD0\xBD",    // рабочих дн
		"\xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x87\xD0\xB8\xD0\xB5 \xD0\xB4\xD0\xBD",    // рабочие дн
		"\xD0\xB1\xD0\xB0\xD0\xBD\xD0\xBA\xD0\xBE\xD0\xB2\xD1\x81\xD0\xBA\xD0\xB8\xD1\x85 \xD0\xB4\xD0\xBD"})); // банковских дн
	add(Intent::DaysBetween, penalty || any_word(folded, {" days", "how long", "how many", "period", "between",
		"before", "after", "since", "until",
		"\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9",                                             // дней
		"\xD0\xB4\xD0\xBD\xD1\x8F",                                                     // дня
		"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB8\xD0\xBE\xD0\xB4",                             // период
		"\xD0\xBC\xD0\xB5\xD0\xB6\xD0\xB4\xD1\x83",                                     // между
		"\xD0\xB4\xD0\xBE ",                                                            // до
		"\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5"}));                                  // после
	add(Intent::Escalation, any_word(folded, {"increase", "escalat", "step-up", "step up", "adjust", "indexa", "raise",
		"old rate", "new rate",
		"\xD1\x83\xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x87",                             // увелич
		"\xD0\xB8\xD0\xBD\xD0\xB4\xD0\xB5\xD0\xBA\xD1\x81",                             // индекс
		"\xD0\xBF\xD0\xBE\xD0\xB2\xD1\x8B\xD1\x88"}));                                  // повыш
	add(Intent::Contact, any_word(folded, {"address", "email", "e-mail", "phone", "contact", "notice to", "send ",
		"be sent", "sent to", "deliver", "serve ", "letter to", "write to", "where must", "where should", "where do",
		"\xD0\xB0\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81",                                     // адрес
		"\xD0\xBF\xD0\xBE\xD1\x87\xD1\x82",                                             // почт
		"\xD1\x82\xD0\xB5\xD0\xBB\xD0\xB5\xD1\x84\xD0\xBE\xD0\xBD",                     // телефон
		"\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82\xD0\xB0\xD0\xBA\xD1\x82",                     // контакт
		"\xD0\xBD\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2"}));                          // направ
	add(Intent::Chronology, any_word(folded, {"chronolog", "timeline", "sequence", "order of events", "when ", "what date",
		"which date",
		"\xD1\x85\xD1\x80\xD0\xBE\xD0\xBD\xD0\xBE\xD0\xBB\xD0\xBE\xD0\xB3",             // хронолог
		"\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD", // последовательн
		"\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xB4\xD0\xB0"}));                                  // когда
	add(Intent::SheetCheck, any_word(folded, {"spreadsheet", "workbook", "ledger", "sheet", "schedule", "table", "column",
		"excel", "reconcil", "check the calc", "verify",
		"\xD1\x82\xD0\xB0\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x86",                             // таблиц
		"\xD1\x80\xD0\xB5\xD0\xB5\xD1\x81\xD1\x82\xD1\x80",                             // реестр
		"\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBE\xD1\x81\xD1\x82",             // ведомост
		"\xD1\x80\xD0\xB0\xD1\x81\xD1\x87\xD1\x91\xD1\x82", "\xD1\x80\xD0\xB0\xD1\x81\xD1\x87\xD0\xB5\xD1\x82", // расчёт расчет
		"\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80"}));                          // провер
	return intents;
}

namespace {

// The sentences of the protected passages, split as the derivations read them.
std::vector<Sentence> sentences_of(const Input &input, const Rows &rows) {
	std::vector<Sentence> sentences;
	static const std::regex cellLine("^(Row [0-9]{1,7}( \\(hidden\\))?: )?\\$?[A-Z]{1,3}\\$?[0-9]{1,7} = ");
	for (std::size_t passage = 0; passage < input.passages.size(); ++passage) {
		const std::string &text = input.passages[passage];
		std::size_t from = 0;
		std::size_t line = 0;
		while (from < text.size()) {
			std::size_t end = text.find('\n', from);
			if (end == std::string::npos) end = text.size();
			const std::string lineText = text.substr(from, end - from);
			from = end + 1;
			++line;
			if (lineText.find_first_not_of(" \t\r") == std::string::npos) continue;
			const bool cells = std::regex_search(lineText, cellLine);
			for (std::string &part : cells ? std::vector<std::string>{lineText} : split_sentences(lineText)) {
				Sentence sentence;
				sentence.passage = passage;
				sentence.line = line;
				sentence.folded = fold(part);
				sentence.markers = markers_in(part, rows);
				sentence.cells = cells;
				sentence.rule = !cells && reads_as_rule(sentence.folded);
				sentence.text = std::move(part);
				sentences.push_back(std::move(sentence));
			}
		}
	}
	return sentences;
}

Rows rows_of(const Input &input) {
	Rows rows{input.mapping, {}};
	for (std::size_t row = 0; row < input.mapping->size(); ++row) rows.by_placeholder.emplace((*input.mapping)[row].placeholder, row);
	return rows;
}

// The party each fact belongs to: the one party of its sentence or row; ""
// once two places give two.
std::map<std::size_t, std::string> parties_in(const std::vector<Sentence> &sentences, const Rows &rows) {
	std::map<std::size_t, std::string> party_of;
	for (const Sentence &sentence : sentences) {
		std::vector<std::size_t> parties;
		for (const Marker &marker : sentence.markers)
			if (rows.is_party(marker.row) && std::find(parties.begin(), parties.end(), marker.row) == parties.end())
				parties.push_back(marker.row);
		if (parties.size() != 1) continue;
		const std::string party = rows.symbol(parties.front());
		for (const Marker &marker : sentence.markers) {
			if (rows.is_party(marker.row) || rows.value(marker.row) == nullptr) continue;
			const auto found = party_of.find(marker.row);
			if (found == party_of.end()) party_of.emplace(marker.row, party);
			else if (found->second != party) found->second.clear();
		}
	}
	return party_of;
}

// The question as intents and stems read it: the raw question when there is
// one (local only), else the protected one.
const std::string &question_text(const Input &input) {
	return input.raw_question.empty() ? input.question : input.raw_question;
}

} // namespace

Candidates derive_candidates(const Input &input) {
	Candidates candidates;
	if (input.mapping == nullptr || input.mapping->empty()) return candidates;
	Builder out{input, rows_of(input), {}, {}, {}, {}, {}, {}, {}};
	out.questionStems = content_stems(fold(question_text(input)));
	const std::vector<Marker> questionMarkers = markers_in(input.question, out.rows);
	for (const Marker &marker : questionMarkers) out.questionRows.insert(marker.row);
	if (!input.passages.empty())
		for (const Marker &marker : markers_in(input.passages.front(), out.rows)) out.firstPassageRows.insert(marker.row);

	const std::vector<Sentence> sentences = sentences_of(input, out.rows);
	const std::vector<Sheet> sheets = sheets_in(input.passages, out.rows, input.limits);
	table_relations(out, sheets);
	step_relations(out, sentences, sheets);
	date_relations(out, sentences, sheets);
	named_sums(out, questionMarkers, markers_in(input.last_answer, out.rows));
	value_equalities(out, sentences);
	contact_owners(out, sentences);

	candidates.party_of = parties_in(sentences, out.rows);
	candidates.relations = std::move(out.relations);
	candidates.derived = std::move(out.derived);
	return candidates;
}

std::map<std::size_t, std::string> parties_of(const Input &input) {
	if (input.mapping == nullptr || input.mapping->empty()) return {};
	const Rows rows = rows_of(input);
	return parties_in(sentences_of(input, rows), rows);
}

TaskFocus focus_of(const Input &input) {
	TaskFocus focus;
	if (input.mapping == nullptr) return focus;
	const Rows rows = rows_of(input);
	for (const Marker &marker : markers_in(input.question, rows)) focus.question_rows.insert(marker.row);
	focus.intents = intents_of(question_text(input));
	const std::set<std::string> questionStems = content_stems(fold(question_text(input)));
	for (std::size_t passage = 0; passage < input.passages.size(); ++passage) {
		std::size_t shared = 0;
		for (const std::string &stem : content_stems(fold(input.passages[passage])))
			if (questionStems.count(stem) != 0) ++shared;
		if (shared >= 2) focus.focus_passages.push_back(passage);
	}
	if (focus.focus_passages.empty())
		for (std::size_t passage = 0; passage < input.passages.size() && passage < 3; ++passage) focus.focus_passages.push_back(passage);
	for (const std::size_t passage : focus.focus_passages)
		for (const Marker &marker : markers_in(input.passages[passage], rows)) focus.focus_rows.insert(marker.row);
	return focus;
}

namespace {

// How sensitive a category is taken to be, most first: the record names the
// most sensitive of a relation's operands.
int category_weight(const std::string &category) {
	static const std::vector<std::string> order{"BANK_ACCOUNT", "TAX_ID", "PERSON", "JUDGE", "EMAIL", "PHONE", "ADDRESS",
		"MONEY_AMOUNT", "PERCENTAGE", "NUMBER", "CASE_NUMBER", "CONTRACT_IDENTIFIER", "ORGANISATION", "LEGAL_ENTITY", "COURT",
		"LOCATION", "DATE"};
	const auto found = std::find(order.begin(), order.end(), category);
	return found == order.end() ? static_cast<int>(order.size()) : static_cast<int>(found - order.begin());
}

bool is_date_kind(Kind kind) {
	return kind == Kind::DaysBetween || kind == Kind::Deadline || kind == Kind::NextBusinessDay || kind == Kind::RuleVsCell
		|| kind == Kind::StepOnDueDate || kind == Kind::DateOrder;
}

// A relation's line with its derived values' symbols, and those symbols.
std::string rendered(const Relation &relation, const Candidates &candidates, const IssueDerived &issue,
		std::map<std::size_t, std::string> &issued, std::vector<std::string> &results,
		std::vector<std::pair<std::string, std::string>> &derivations) {
	std::string line;
	std::size_t position = 0;
	while (position < relation.text.size()) {
		const std::size_t open = relation.text.find(kDerivedMark, position);
		if (open == std::string::npos) { line += relation.text.substr(position); break; }
		const std::size_t close = relation.text.find(kDerivedMark, open + 1);
		const std::size_t index = std::stoul(relation.text.substr(open + 1, close - open - 1));
		auto found = issued.find(index);
		if (found == issued.end()) found = issued.emplace(index, issue ? issue(candidates.derived.at(index)) : std::string()).first;
		if (found->second.empty()) return {};
		line += relation.text.substr(position, open - position) + found->second;
		if (std::find(results.begin(), results.end(), found->second) == results.end()) {
			results.push_back(found->second);
			derivations.emplace_back(found->second, candidates.derived.at(index).derivation);
		}
		position = close + 1;
	}
	return line;
}

} // namespace

Plan plan(const Input &input, const Candidates &candidates, const TaskFocus &focus, const PlanContext &context,
		const IssueDerived &issue) {
	Plan result;
	result.candidates = candidates.relations.size();
	if (input.mapping == nullptr) return result;
	const std::vector<RequestMappingEntry> &mapping = *input.mapping;
	const std::set<Intent> asked(focus.intents.begin(), focus.intents.end());
	// Why a candidate may leave: the intent it serves, and that it relates what
	// the task targets. Otherwise it stays on this computer, unrecorded.
	struct Chosen {
		std::size_t index;
		std::string justification;
		std::string subproblem;
	};
	std::vector<Chosen> chosen;
	// Structural signals (owner, 26 Sep 2026): what a model reading raw figures
	// sees at a glance - a hidden row inside a total, a ledger still billing
	// the pre-step amount after an escalation - is offered whatever words the
	// question uses, as relations over symbols, charged like any other.
	constexpr std::size_t kStructuralSignals = 6;
	std::size_t structuralSignals = 0;
	for (std::size_t index = 0; index < candidates.relations.size(); ++index) {
		const Relation &relation = candidates.relations[index];
		const bool namesOperand = std::any_of(relation.operand_rows.begin(), relation.operand_rows.end(),
			[&](std::size_t row) { return focus.question_rows.count(row) != 0; });
		if (relation.lawyer_value) {
			chosen.push_back({index, "lawyer_value_in_request", "lawyer_value"});
			continue;
		}
		std::optional<Intent> served;
		for (const Intent intent : focus.intents)
			if (relation.serves.count(intent) != 0) { served = intent; break; }
		const bool targeted = namesOperand || relation.operand_rows.empty() || std::any_of(relation.operand_rows.begin(),
			relation.operand_rows.end(), [&](std::size_t row) { return focus.focus_rows.count(row) != 0; });
		// A structural signal the question's own route would not reach - no
		// intent of it, or none of its operands in the passages the question
		// is about (live E1, 26 Sep 2026: "how much ... owe" served, the ledger
		// not in focus, the signal lost) - is still offered.
		if ((relation.kind == Kind::HiddenRow || relation.kind == Kind::EscalatedVsLedger) && (!served || !targeted)
				&& structuralSignals < kStructuralSignals) {
			++structuralSignals;
			chosen.push_back({index, "structural_signal", relation.kind == Kind::HiddenRow ? "hidden_row" : "escalated_vs_ledger"});
			continue;
		}
		if (!served) continue;
		// Equal values of the material are said only for a comparison or a sheet check.
		if (relation.kind == Kind::ValueEquality && asked.count(Intent::Compare) == 0 && asked.count(Intent::SheetCheck) == 0)
			continue;
		if (!targeted) continue;
		std::string justification = namesOperand ? "question_names_operand" : "question_intent:" + std::string(to_string(*served));
		if (relation.sheet_total && asked.count(Intent::SheetCheck) != 0) justification = "sheet_total_check";
		chosen.push_back({index, justification, to_string(*served)});
	}
	result.skipped_irrelevant = candidates.relations.size() - chosen.size();
	std::stable_sort(chosen.begin(), chosen.end(), [&](const Chosen &a, const Chosen &b) {
		return candidates.relations[a.index].rank > candidates.relations[b.index].rank;
	});
	std::map<std::size_t, std::string> issued;
	std::size_t bytes = 0;
	std::size_t sent = 0;
	for (const Chosen &choice : chosen) {
		const Relation &relation = candidates.relations[choice.index];
		OutboundRelation record;
		record.kind = to_string(relation.kind);
		record.requested_by = "planner";
		record.justification = choice.justification;
		record.subproblem = choice.subproblem;
		RelationFacts facts;
		facts.kind = record.kind;
		std::string heaviest;
		for (const std::size_t row : relation.operand_rows) {
			const RequestMappingEntry &entry = mapping[row];
			record.operands.push_back(entry.placeholder);
			const std::string category = context.category ? context.category(entry) : std::string("PROTECTED_VALUE");
			facts.operand_categories.push_back(category);
			const auto party = candidates.party_of.find(row);
			facts.operand_parties.push_back(party == candidates.party_of.end() ? std::string() : party->second);
			if (heaviest.empty() || category_weight(category) < category_weight(heaviest)) heaviest = category;
			if (entry.derived) continue;
			RelationSource source;
			source.marker = entry.placeholder;
			source.origin = context.origin ? context.origin(entry) : std::string("source");
			source.passage_sha256 = entry.provenance.passage_sha256;
			source.offset = entry.provenance.offset;
			source.length = entry.provenance.length;
			source.cell = entry.provenance.cell;
			record.sources.push_back(std::move(source));
		}
		record.value_sensitivity = heaviest;
		record.relation_sensitivity = context.classify && context.classify(facts) == RelationSensitivity::Sensitive
			? "sensitive" : "ordinary";
		if (relation.rule) {
			record.business_day_rule = business_calendar::rule_name(*relation.rule);
			record.business_day_rule_source.origin = "source";
			record.business_day_rule_source.passage_sha256 = relation.rule->defining_passage_sha256;
			record.business_day_rule_source.offset = relation.rule->defining_offset;
			record.business_day_rule_source.length = relation.rule->defining_length;
		}
		if (context.permit && !context.permit(facts)) {
			// Held back by the relation policy: recorded, never rendered - no
			// derived symbol is issued for it and nothing of it is sent.
			record.decision = "withheld";
			record.id = "W" + std::to_string(result.withheld.size() + 1);
			result.withheld.push_back(std::move(record));
			continue;
		}
		// The safety cap: a justified relation it drops is counted, and the model
		// can still ask for it as an operation.
		if (sent >= input.limits.relations || bytes + relation.text.size() + 8 > input.limits.bytes) {
			++result.dropped_by_cap;
			continue;
		}
		std::vector<std::string> results;
		std::vector<std::pair<std::string, std::string>> derivations;
		std::string line = rendered(relation, candidates, issue, issued, results, derivations);
		if (line.empty()) continue;
		bytes += line.size() + 8;
		++sent;
		record.id = "R" + std::to_string(sent);
		record.sent = line;
		record.results = results;
		for (const auto &[marker, how] : derivations)
			record.derivation += (record.derivation.empty() ? "" : "; ") + marker + " := " + how;
		record.decision = "sent";
		for (const std::size_t row : relation.operand_rows) {
			const Value *value = mapping[row].value ? &*mapping[row].value : nullptr;
			if (is_date_kind(relation.kind) && value != nullptr && value->kind == VKind::Date)
				result.timeline_markers.insert(mapping[row].placeholder);
		}
		result.lines.push_back(std::move(line));
		result.records.push_back(std::move(record));
	}
	// The question's own dates when it asks about time; every date of the
	// passages it targets when it asks for a chronology.
	const bool aboutTime = asked.count(Intent::Chronology) != 0 || asked.count(Intent::Deadline) != 0
		|| asked.count(Intent::DaysBetween) != 0 || asked.count(Intent::BusinessDays) != 0;
	const auto isDate = [&](std::size_t row) {
		return mapping[row].value && mapping[row].value->kind == VKind::Date;
	};
	if (aboutTime)
		for (const std::size_t row : focus.question_rows)
			if (isDate(row)) result.timeline_markers.insert(mapping[row].placeholder);
	if (asked.count(Intent::Chronology) != 0)
		for (const std::size_t row : focus.focus_rows)
			if (isDate(row)) result.timeline_markers.insert(mapping[row].placeholder);
	return result;
}

std::optional<TableTotal> table_total(const std::vector<std::string> &raw_passages, const std::string &table,
		const std::string &column, bool include_hidden) {
	static const std::vector<RequestMappingEntry> none;
	Rows rows{&none, {}};
	Limits limits;
	limits.tables = 64;
	const std::string wanted = fold(table);
	// The passages that are that table: its "Sheet" line names it, or its handle does.
	std::vector<std::string> chosen;
	for (const std::string &passage : raw_passages) {
		const std::string head = fold(passage.substr(0, std::min<std::size_t>(passage.size(), 400)));
		if (wanted.empty() || head.find(wanted) != std::string::npos) chosen.push_back(passage);
	}
	std::string letters;
	for (const char character : column)
		if (std::isalpha(static_cast<unsigned char>(character))) letters += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	for (const Sheet &sheet : sheets_in(chosen, rows, limits)) {
		const std::size_t bodyStart = sheet.header ? *sheet.header + 1 : 0;
		std::optional<Value> total;
		TableTotal found;
		std::string currency;
		for (std::size_t index = bodyStart; index < sheet.rows.size(); ++index) {
			const SheetRow &row = sheet.rows[index];
			if (total_row(row)) continue;
			const Cell *cell = row.at(letters);
			if (cell == nullptr || !cell->literal) continue;
			if (cell->literal->kind != VKind::Money && cell->literal->kind != VKind::Number) continue;
			if (row.hidden) {
				found.hidden_cells.push_back(cell->ref);
				if (!include_hidden) continue;
			}
			const std::string cellCurrency = cell->literal->kind == VKind::Money ? cell->literal->currency : std::string();
			if (!cellCurrency.empty()) {
				if (!currency.empty() && currency != cellCurrency) return std::nullopt;
				currency = cellCurrency;
			}
			found.counted_cells.push_back(cell->ref);
			const Rational amount = cell->literal->amount;
			total = total ? semantic::make_number(total->amount + amount) : semantic::make_number(amount);
		}
		if (!total || found.counted_cells.empty()) continue;
		found.total = currency.empty() ? *total : semantic::make_money(total->amount, currency);
		return found;
	}
	return std::nullopt;
}

} // namespace singularity::protected_api::papinian::relations
