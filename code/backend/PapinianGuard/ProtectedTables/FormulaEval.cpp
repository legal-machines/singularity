#include "PapinianGuard/ProtectedTables/FormulaEval.hpp"

#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

// Singularity's own evaluator of a workbook formula (DESIGN4 §6.6): the formula is
// read whole first - so whether it can be evaluated depends on its text alone
// (not_evaluable) - and only then worked out over the table's cells in exact
// numbers, where a value can make it fail (evaluation_failed: #DIV/0!, text
// where a figure is needed, an error cell).
namespace singularity::protected_api::papinian::tables {
namespace {

using semantic::ExactRational;

struct Node {
	enum class Kind { Number, Ref, Range, Negate, Binary, Function };
	Kind kind = Kind::Number;
	ExactRational number;
	// Ref and Range: 1-based columns and rows.
	std::size_t column = 0;
	std::size_t row = 0;
	std::size_t last_column = 0;
	std::size_t last_row = 0;
	char op = '+';
	std::string function;
	// SUBTOTAL's code, when the formula writes it as a whole number.
	int code = 0;
	std::vector<std::unique_ptr<Node>> children;
};

std::size_t column_number(std::string_view letters) {
	std::size_t number = 0;
	for (const char letter : letters) number = number * 26 + static_cast<std::size_t>(letter - 'A' + 1);
	return number;
}

std::string column_letters(std::size_t number) {
	std::string letters;
	while (number > 0) {
		const std::size_t rest = (number - 1) % 26;
		letters.insert(letters.begin(), static_cast<char>('A' + rest));
		number = (number - 1) / 26;
	}
	return letters;
}

// Recursive descent over the closed grammar; any other shape is not
// evaluable.
class Parser {
public:
	Parser(std::string_view text, std::string sheet) : text_(text), sheet_(std::move(sheet)) {}

	std::unique_ptr<Node> parse() {
		skip();
		if (at_ < text_.size() && text_[at_] == '=') ++at_;
		std::unique_ptr<Node> node = expression();
		skip();
		if (!node || at_ != text_.size()) return nullptr;
		return node;
	}

private:
	std::string_view text_;
	std::string sheet_;
	std::size_t at_ = 0;
	int depth_ = 0;

	void skip() {
		while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t')) ++at_;
	}

	bool take(char character) {
		skip();
		if (at_ < text_.size() && text_[at_] == character) {
			++at_;
			return true;
		}
		return false;
	}

	std::unique_ptr<Node> expression() {
		if (++depth_ > 64) return nullptr;
		std::unique_ptr<Node> left = term();
		while (left) {
			skip();
			if (at_ >= text_.size() || (text_[at_] != '+' && text_[at_] != '-')) break;
			const char op = text_[at_++];
			std::unique_ptr<Node> right = term();
			if (!right) return nullptr;
			auto node = std::make_unique<Node>();
			node->kind = Node::Kind::Binary;
			node->op = op;
			node->children.push_back(std::move(left));
			node->children.push_back(std::move(right));
			left = std::move(node);
		}
		--depth_;
		return left;
	}

	std::unique_ptr<Node> term() {
		std::unique_ptr<Node> left = unary();
		while (left) {
			skip();
			if (at_ >= text_.size() || (text_[at_] != '*' && text_[at_] != '/')) break;
			const char op = text_[at_++];
			std::unique_ptr<Node> right = unary();
			if (!right) return nullptr;
			auto node = std::make_unique<Node>();
			node->kind = Node::Kind::Binary;
			node->op = op;
			node->children.push_back(std::move(left));
			node->children.push_back(std::move(right));
			left = std::move(node);
		}
		return left;
	}

	std::unique_ptr<Node> unary() {
		skip();
		if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+')) {
			const bool negate = text_[at_] == '-';
			++at_;
			std::unique_ptr<Node> inner = unary();
			if (!inner || !negate) return inner;
			auto node = std::make_unique<Node>();
			node->kind = Node::Kind::Negate;
			node->children.push_back(std::move(inner));
			return node;
		}
		return primary(false);
	}

	// A cell reference at the cursor ("$B$4", "Ledger!B4", "'Rent ledger'!B4"),
	// only of this table's own sheet.
	bool reference(std::size_t &column, std::size_t &row, bool &otherSheet) {
		const std::size_t start = at_;
		otherSheet = false;
		if (at_ < text_.size() && text_[at_] == '\'') {
			const std::size_t close = text_.find('\'', at_ + 1);
			if (close == std::string_view::npos || close + 1 >= text_.size() || text_[close + 1] != '!') return false;
			otherSheet = std::string(text_.substr(at_ + 1, close - at_ - 1)) != sheet_;
			at_ = close + 2;
		} else {
			std::size_t cursor = at_;
			while (cursor < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[cursor])) || text_[cursor] == '_'
					|| static_cast<unsigned char>(text_[cursor]) >= 0x80))
				++cursor;
			if (cursor < text_.size() && text_[cursor] == '!' && cursor > at_) {
				otherSheet = std::string(text_.substr(at_, cursor - at_)) != sheet_;
				at_ = cursor + 1;
			}
		}
		std::size_t cursor = at_;
		if (cursor < text_.size() && text_[cursor] == '$') ++cursor;
		const std::size_t letters = cursor;
		while (cursor < text_.size() && text_[cursor] >= 'A' && text_[cursor] <= 'Z' && cursor - letters < 4) ++cursor;
		if (cursor == letters || cursor - letters > 3) { at_ = start; return false; }
		const std::string_view columnText = text_.substr(letters, cursor - letters);
		if (cursor < text_.size() && text_[cursor] == '$') ++cursor;
		const std::size_t digits = cursor;
		while (cursor < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor])) && cursor - digits < 8) ++cursor;
		if (cursor == digits || (cursor < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[cursor])) || text_[cursor] == '('))) {
			at_ = start;
			return false;
		}
		column = column_number(columnText);
		row = static_cast<std::size_t>(std::strtoul(std::string(text_.substr(digits, cursor - digits)).c_str(), nullptr, 10));
		at_ = cursor;
		return true;
	}

	std::unique_ptr<Node> primary(bool rangeAllowed) {
		skip();
		if (at_ >= text_.size()) return nullptr;
		if (text_[at_] == '(') {
			++at_;
			std::unique_ptr<Node> inner = expression();
			if (!inner || !take(')')) return nullptr;
			return inner;
		}
		if (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '.') {
			const std::size_t start = at_;
			while (at_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '.')) ++at_;
			const std::optional<semantic::Rational> value = semantic::Rational::parse_decimal(text_.substr(start, at_ - start));
			if (!value) return nullptr;
			auto node = std::make_unique<Node>();
			node->kind = Node::Kind::Number;
			node->number = ExactRational::from(*value);
			const std::string_view written = text_.substr(start, at_ - start);
			node->code = written.find('.') == std::string_view::npos && written.size() <= 4
				? std::atoi(std::string(written).c_str()) : -1;
			return node;
		}
		// A function: letters and dots, then "(".
		{
			std::size_t cursor = at_;
			while (cursor < text_.size() && (std::isalpha(static_cast<unsigned char>(text_[cursor])) || text_[cursor] == '.')) ++cursor;
			std::size_t open = cursor;
			while (open < text_.size() && text_[open] == ' ') ++open;
			if (cursor > at_ && open < text_.size() && text_[open] == '(') {
				std::string name(text_.substr(at_, cursor - at_));
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				static const std::set<std::string> known{"SUM", "MIN", "MAX", "AVERAGE", "COUNT", "SUBTOTAL"};
				if (known.count(name) == 0) return nullptr;
				at_ = open + 1;
				auto node = std::make_unique<Node>();
				node->kind = Node::Kind::Function;
				node->function = name;
				skip();
				if (!take(')')) {
					while (true) {
						std::unique_ptr<Node> argument = argument_node();
						if (!argument) return nullptr;
						node->children.push_back(std::move(argument));
						if (take(',')) continue;
						if (take(')')) break;
						return nullptr;
					}
				}
				if (name == "SUBTOTAL") {
					// The function code, written as a whole number: structure, not a figure.
					if (node->children.size() < 2 || node->children.front()->kind != Node::Kind::Number
							|| node->children.front()->code < 0)
						return nullptr;
					node->code = node->children.front()->code;
					const int base = node->code > 100 ? node->code - 100 : node->code;
					if (!(node->code >= 1 && node->code <= 11) && !(node->code >= 101 && node->code <= 111)) return nullptr;
					// COUNTA, PRODUCT, STDEV, VAR: outside the closed set.
					if (base != 1 && base != 2 && base != 4 && base != 5 && base != 9) return nullptr;
					for (std::size_t index = 1; index < node->children.size(); ++index)
						if (node->children[index]->kind != Node::Kind::Range && node->children[index]->kind != Node::Kind::Ref)
							return nullptr;
				}
				return node;
			}
		}
		std::size_t column = 0;
		std::size_t row = 0;
		bool otherSheet = false;
		if (!reference(column, row, otherSheet) || otherSheet) return nullptr;
		skip();
		if (at_ < text_.size() && text_[at_] == ':') {
			if (!rangeAllowed) return nullptr;
			++at_;
			skip();
			std::size_t lastColumn = 0;
			std::size_t lastRow = 0;
			if (!reference(lastColumn, lastRow, otherSheet) || otherSheet) return nullptr;
			auto node = std::make_unique<Node>();
			node->kind = Node::Kind::Range;
			node->column = std::min(column, lastColumn);
			node->last_column = std::max(column, lastColumn);
			node->row = std::min(row, lastRow);
			node->last_row = std::max(row, lastRow);
			if ((node->last_row - node->row + 1) * (node->last_column - node->column + 1) > 200000) return nullptr;
			return node;
		}
		auto node = std::make_unique<Node>();
		node->kind = Node::Kind::Ref;
		node->column = column;
		node->row = row;
		return node;
	}

	// A function's argument: a range, or any expression. Whether it is a range
	// is seen from a reference followed by ":" - never by parsing the argument
	// twice, which would double the work at every level of nesting.
	std::unique_ptr<Node> argument_node() {
		skip();
		const std::size_t start = at_;
		std::size_t column = 0;
		std::size_t row = 0;
		bool otherSheet = false;
		const bool isReference = reference(column, row, otherSheet);
		skip();
		const bool range = isReference && at_ < text_.size() && text_[at_] == ':';
		at_ = start;
		if (range) {
			std::unique_ptr<Node> node = primary(true);
			skip();
			if (node && node->kind == Node::Kind::Range && at_ < text_.size() && (text_[at_] == ',' || text_[at_] == ')'))
				return node;
			return nullptr;
		}
		return expression();
	}
};

// Evaluation: a value, or a failure a value caused.
struct Result {
	bool failed = false;
	ExactRational value;
};

class Evaluator {
public:
	Evaluator(const ProtectedTable &table, std::vector<std::string> &inputs) : table_(table), inputs_(inputs) {}

	Result run(const Node &node) {
		switch (node.kind) {
			case Node::Kind::Number: return {false, node.number};
			case Node::Kind::Ref: return scalar(node.column, node.row);
			case Node::Kind::Range: return {true, {}};
			case Node::Kind::Negate: {
				Result inner = run(*node.children.front());
				if (inner.failed) return inner;
				return {false, -inner.value};
			}
			case Node::Kind::Binary: {
				const Result left = run(*node.children[0]);
				if (left.failed) return left;
				const Result right = run(*node.children[1]);
				if (right.failed) return right;
				switch (node.op) {
					case '+': return {false, left.value + right.value};
					case '-': return {false, left.value - right.value};
					case '*': return {false, left.value * right.value};
					case '/':
						if (right.value.is_zero()) return {true, {}};
						return {false, left.value / right.value};
					default: return {true, {}};
				}
			}
			case Node::Kind::Function: return function(node);
		}
		return {true, {}};
	}

private:
	const ProtectedTable &table_;
	std::vector<std::string> &inputs_;
	std::map<std::string, std::optional<ExactRational>> memo_;
	std::set<std::string> visiting_;
	int depth_ = 0;

	enum class Read { Figure, Blank, Text, Error };

	struct CellReading {
		Read read = Read::Blank;
		ExactRational value;
		bool hidden = false;
		bool subtotal = false;
		// Text or a logical value as the file stores it (a "$50" typed as text
		// included): a range function passes it over, as Excel's SUM does.
		bool textual = false;
	};

	static std::optional<ExactRational> figure_of(const semantic::Value &value) {
		switch (value.kind) {
			case semantic::Kind::Money:
			case semantic::Kind::Number:
			case semantic::Kind::Percentage:
			case semantic::Kind::Duration:
			case semantic::Kind::Count:
				return ExactRational::from(value.amount);
			case semantic::Kind::Date:
				// Excel's serial day.
				return ExactRational(semantic::BigInt(value.date.first_day + 25569));
			case semantic::Kind::Boolean: return ExactRational(semantic::BigInt(value.truth ? 1 : 0));
			default: return std::nullopt;
		}
	}

	CellReading read(std::size_t column, std::size_t row) {
		const std::string ref = column_letters(column) + std::to_string(row);
		if (std::find(inputs_.begin(), inputs_.end(), ref) == inputs_.end()) inputs_.push_back(ref);
		CellReading reading;
		const TableRow *found = row_of(table_, row);
		if (found == nullptr) return reading;
		reading.hidden = found->hidden;
		const TableCell *cell = cell_of(table_, row, column_letters(column));
		if (cell == nullptr) return reading;
		std::string upper = cell->formula;
		std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		reading.subtotal = upper.find("SUBTOTAL(") != std::string::npos;
		if (!cell->formula.empty()) {
			// A formula it reads is worked out again, not taken as saved; one Singularity
			// cannot work out keeps the value it was saved with.
			if (const std::optional<ExactRational> again = recompute(*cell)) {
				reading.read = Read::Figure;
				reading.value = *again;
				return reading;
			}
		}
		if (cell->kind == CellKind::Error) {
			reading.read = Read::Error;
			return reading;
		}
		reading.textual = cell->kind == CellKind::Text || cell->kind == CellKind::Boolean;
		if (cell->value) {
			if (const std::optional<ExactRational> figure = figure_of(*cell->value)) {
				reading.read = Read::Figure;
				reading.value = *figure;
				return reading;
			}
		}
		const bool blank = cell->kind == CellKind::Empty && cell->text.empty();
		reading.read = blank ? Read::Blank : Read::Text;
		return reading;
	}

	std::optional<ExactRational> recompute(const TableCell &cell) {
		if (const auto known = memo_.find(cell.ref); known != memo_.end()) return known->second;
		if (visiting_.count(cell.ref) != 0 || depth_ > 256) return std::nullopt;
		Parser parser(cell.formula, table_.sheet);
		const std::unique_ptr<Node> node = parser.parse();
		if (!node) return memo_[cell.ref] = std::nullopt;
		visiting_.insert(cell.ref);
		++depth_;
		const Result result = run(*node);
		--depth_;
		visiting_.erase(cell.ref);
		return memo_[cell.ref] = result.failed ? std::nullopt : std::optional<ExactRational>(result.value);
	}

	Result scalar(std::size_t column, std::size_t row) {
		const CellReading reading = read(column, row);
		switch (reading.read) {
			case Read::Figure: return {false, reading.value};
			case Read::Blank: return {false, ExactRational()};
			case Read::Text:
			case Read::Error: return {true, {}};
		}
		return {true, {}};
	}

	// The figures of a function's arguments: a range's figure cells (text and
	// blanks passed over), an expression's value.
	bool gather(const Node &function, std::size_t from, bool skipHidden, bool skipSubtotals,
			std::vector<ExactRational> &figures) {
		for (std::size_t index = from; index < function.children.size(); ++index) {
			const Node &argument = *function.children[index];
			if (argument.kind == Node::Kind::Range || argument.kind == Node::Kind::Ref) {
				const std::size_t lastRow = argument.kind == Node::Kind::Range ? argument.last_row : argument.row;
				const std::size_t lastColumn = argument.kind == Node::Kind::Range ? argument.last_column : argument.column;
				for (std::size_t row = argument.row; row <= lastRow; ++row)
					for (std::size_t column = argument.column; column <= lastColumn; ++column) {
						const CellReading reading = read(column, row);
						if (reading.read == Read::Error) return false;
						if (skipHidden && reading.hidden) continue;
						if (skipSubtotals && reading.subtotal) continue;
						if (reading.read == Read::Figure && !reading.textual) figures.push_back(reading.value);
					}
				continue;
			}
			const Result value = run(argument);
			if (value.failed) return false;
			figures.push_back(value.value);
		}
		return true;
	}

	Result function(const Node &node) {
		std::string name = node.function;
		std::size_t from = 0;
		bool skipHidden = false;
		bool skipSubtotals = false;
		if (name == "SUBTOTAL") {
			from = 1;
			skipHidden = node.code > 100;
			skipSubtotals = true;
			switch (node.code > 100 ? node.code - 100 : node.code) {
				case 1: name = "AVERAGE"; break;
				case 2: name = "COUNT"; break;
				case 4: name = "MAX"; break;
				case 5: name = "MIN"; break;
				case 9: name = "SUM"; break;
				default: return {true, {}};
			}
		}
		std::vector<ExactRational> figures;
		if (!gather(node, from, skipHidden, skipSubtotals, figures)) return {true, {}};
		if (name == "COUNT") return {false, ExactRational(semantic::BigInt(static_cast<long long>(figures.size())))};
		if (name == "SUM" || name == "AVERAGE") {
			ExactRational total;
			for (const ExactRational &figure : figures) total = total + figure;
			if (name == "SUM") return {false, total};
			if (figures.empty()) return {true, {}};
			return {false, total / ExactRational(semantic::BigInt(static_cast<long long>(figures.size())))};
		}
		if (figures.empty()) return {false, ExactRational()};
		ExactRational best = figures.front();
		for (const ExactRational &figure : figures)
			if (name == "MIN" ? figure < best : figure > best) best = figure;
		return {false, best};
	}
};

void collect_refs(const Node &node, const ProtectedTable &table, std::vector<std::string> &into,
		std::vector<const TableCell *> &formulas) {
	if (node.kind == Node::Kind::Ref || node.kind == Node::Kind::Range) {
		const std::size_t lastRow = node.kind == Node::Kind::Range ? node.last_row : node.row;
		const std::size_t lastColumn = node.kind == Node::Kind::Range ? node.last_column : node.column;
		for (const TableRow &row : table.rows) {
			if (row.number < node.row || row.number > lastRow) continue;
			for (const TableCell &cell : row.cells) {
				if (cell.column < node.column || cell.column > lastColumn) continue;
				if (std::find(into.begin(), into.end(), cell.ref) != into.end()) continue;
				into.push_back(cell.ref);
				if (!cell.formula.empty()) formulas.push_back(&cell);
			}
		}
		return;
	}
	for (const std::unique_ptr<Node> &child : node.children) collect_refs(*child, table, into, formulas);
}

} // namespace

std::vector<std::string> formula_inputs(const ProtectedTable &table, const TableCell &cell) {
	std::vector<std::string> inputs;
	std::vector<const TableCell *> pending{&cell};
	std::set<std::string> read;
	while (!pending.empty() && read.size() < 4096) {
		const TableCell *next = pending.back();
		pending.pop_back();
		if (!read.insert(next->ref).second) continue;
		Parser parser(next->formula, table.sheet);
		const std::unique_ptr<Node> node = parser.parse();
		if (!node) {
			if (next == &cell) return {};
			continue;
		}
		collect_refs(*node, table, inputs, pending);
	}
	inputs.erase(std::remove(inputs.begin(), inputs.end(), cell.ref), inputs.end());
	return inputs;
}

FormulaOutcome recalc(const ProtectedTable &table, const TableCell &cell) {
	FormulaOutcome outcome;
	if (cell.formula.empty()) return outcome;
	Parser parser(cell.formula, table.sheet);
	const std::unique_ptr<Node> node = parser.parse();
	if (!node || node->kind == Node::Kind::Range) return outcome;
	std::vector<std::string> read;
	Evaluator evaluator(table, read);
	const Result result = evaluator.run(*node);
	outcome.inputs = formula_inputs(table, cell);
	if (result.failed) {
		outcome.kind = FormulaOutcome::Kind::EvaluationFailed;
		return outcome;
	}
	outcome.kind = FormulaOutcome::Kind::Value;
	outcome.value = result.value;
	return outcome;
}

} // namespace singularity::protected_api::papinian::tables
