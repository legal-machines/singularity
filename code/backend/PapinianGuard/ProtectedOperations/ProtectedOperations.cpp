#include "PapinianGuard/ProtectedOperations/ProtectedOperations.hpp"

#include "PapinianGuard/ProtectedRelations/ProtectedRelations.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace singularity::protected_api::papinian::operations {
namespace {

namespace control = singularity::context::control_text;
namespace temporal = singularity::context::temporal;
using semantic::Language;
using semantic::Value;
using VKind = semantic::Kind;

const std::string kOpen = "\xE2\x9F\xA6";   // ⟦
const std::string kClose = "\xE2\x9F\xA7";  // ⟧

std::string trimmed(std::string_view text) {
	std::size_t first = 0;
	std::size_t last = text.size();
	while (first < last && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
	while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
	return std::string(text.substr(first, last - first));
}

std::string lower(std::string text) {
	for (char &character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return text;
}

// "⟦[[Amount3]]⟧" and "[[Amount3]]" alike: the symbol, or "" when the operand
// is not wholly one symbol.
std::string marker_of(std::string text) {
	text = trimmed(text);
	if (text.size() > kOpen.size() + kClose.size() && text.compare(0, kOpen.size(), kOpen) == 0
			&& text.compare(text.size() - kClose.size(), kClose.size(), kClose) == 0)
		text = trimmed(text.substr(kOpen.size(), text.size() - kOpen.size() - kClose.size()));
	if (text.size() < 5 || text.compare(0, 2, "[[") != 0 || text.compare(text.size() - 2, 2, "]]") != 0) return {};
	if (text.find("[[", 2) != std::string::npos || text.find("]]") != text.size() - 2) return {};
	return text;
}

bool is_date(const Value &value) { return value.kind == VKind::Date || value.kind == VKind::DayOfYear; }

bool is_day(const Value &value) { return value.kind == VKind::Date && value.date.precision == temporal::Precision::Day; }

// One operand as the operation reads it.
struct Operand {
	std::string written;
	std::string marker;
	std::optional<RequestMappingEntry> row;
	std::optional<Value> value;
	// The fact it is, for a derivation: its fact key, else its symbol, else its
	// literal value.
	std::string key;
	std::string failure;
	// A figure the model wrote rather than a symbol. It is an operand only
	// where the material itself writes it openly (Executor::resolve), so an
	// answer line that writes it back says nothing the sent text did not.
	bool literal = false;
	// A fact that may be written but not related any further: one worked out
	// by an expression, which figures of the model's own may shape.
	bool written_only = false;
	// A ratio of two quantities of one kind (a pure number): compared, never
	// calculated with further - sums of such ratios would build any constant,
	// and a constant compared with a protected fact would tell its value.
	bool dimensionless = false;
	// A date worked out from a period of the model's own choosing: it may be
	// compared, but a count of days between it and another is only written -
	// or sums of such counts would build any period.
	bool period_literal = false;
};

// A line in two planes: what the model reads (symbols) and what this computer
// keeps in the loop's prompt (each symbol as its local anchor).
struct Line {
	struct Piece {
		std::string text;
		bool symbol = false;
	};
	std::vector<Piece> pieces;
	Line &say(std::string text) {
		pieces.push_back({std::move(text), false});
		return *this;
	}
	Line &symbol(std::string marker) {
		pieces.push_back({std::move(marker), true});
		return *this;
	}
	Line &operand(const Operand &operand) {
		if (!operand.marker.empty()) return symbol(operand.marker);
		return say(operand.written);
	}
	[[nodiscard]] std::string model() const {
		std::string text;
		for (const Piece &piece : pieces) text += piece.text;
		return text;
	}
	[[nodiscard]] std::string local(const Context &context) const {
		std::string text;
		for (const Piece &piece : pieces) {
			if (!piece.symbol) {
				text += piece.text;
				continue;
			}
			const RequestMappingEntry *row = context.row_of ? context.row_of(piece.text) : nullptr;
			text += row != nullptr ? local_writing(*row) : piece.text;
		}
		return text;
	}
};

// What a reason code tells the model, in closed words over no value.
std::string reason_words(const std::string &reason) {
	if (reason == "rule_unknown") return "the rule that governs business days is not known here";
	if (reason == "rule_conflict") return "the material defines Business Day in two ways that disagree";
	if (reason == "definition_not_found") return "the definition the agreement incorporates is not in the material read";
	if (reason == "precision_insufficient") return "a date names a longer period, so the answer depends on its day";
	if (reason == "currency_mismatch") return "the amounts are in different currencies";
	if (reason == "operand_unknown") return "an operand is not a symbol Singularity issued, or not a value it can read";
	if (reason == "not_a_quantity") return "an operand is not a quantity or a date this operation works on";
	if (reason == "withheld_by_policy") return "not worked out under the lawyer's privacy policy";
	if (reason == "division_by_zero") return "the divisor is zero";
	if (reason == "no_exact_writing") return "the result has no exact value; ask for it rounded (op round with the places)";
	if (reason == "per_period") return "the result is owed for each period of the rate; ask for it over the period (a third operand)";
	if (reason == "rate_basis_unknown")
		return "what period the rate is for (per day, per month or per annum) is not written with it; give the rate with its "
			"period as the material writes it, or ask the lawyer";
	if (reason == "unknown_op") return "not an operation Singularity knows";
	if (reason == "own_figure")
		return "an operand is a figure of your own, or a fact worked out from one: operations relate the symbols Singularity gave, "
			"and take a period, a rate or a count only as the material writes it";
	if (reason == "ratio_operand") return "a ratio of two quantities of one kind is compared, never calculated with further";
	if (reason == "too_many") return "more questions than one answer may ask; ask it in your next answer if you still need it";
	if (reason == "arity") return "the operation was given the wrong number of operands";
	if (reason == "same_operand") return "an operand is given twice; name each fact once";
	return "Singularity could not work it out exactly";
}

// A value's type as a derived fact carries it.
EntityType type_of(const Value &value) {
	switch (value.kind) {
		case VKind::Money: return EntityType::MoneyAmount;
		case VKind::Percentage: return EntityType::Percentage;
		case VKind::Date:
		case VKind::DayOfYear: return EntityType::Date;
		default: return EntityType::Number;
	}
}

// How a derived value is described beside its symbol, in closed words.
// `rounding`: 0 none, 1 the op rounds ("rounded"), 2 the op may round -
// said for every value of the op alike.
std::string kind_words(const Value &value, int rounding) {
	std::string words;
	switch (value.kind) {
		case VKind::Money: words = "money" + (value.currency.empty() ? std::string() : " in " + value.currency); break;
		case VKind::Percentage: words = "a rate"; break;
		case VKind::Date: words = "a date"; break;
		case VKind::DayOfYear: words = "a day of the year"; break;
		case VKind::Duration:
			switch (value.unit) {
				case semantic::DurationUnit::Day: words = "a period in days"; break;
				case semantic::DurationUnit::BusinessDay: words = "a period in business days"; break;
				case semantic::DurationUnit::Week: words = "a period in weeks"; break;
				case semantic::DurationUnit::Month: words = "a period in months"; break;
				case semantic::DurationUnit::Year: words = "a period in years"; break;
			}
			break;
		case VKind::Number: words = "a number"; break;
		case VKind::Boolean: words = "true or false"; break;
	}
	if (rounding == 1) words += ", rounded";
	else if (rounding == 2)
		words += value.kind == VKind::Money ? ", rounded half up to the cent where needed" : ", rounded to six decimal places where needed";
	return words;
}

std::string unit_word(const std::string &unit, bool &known, std::optional<semantic::DurationUnit> &parsed) {
	std::string words = lower(trimmed(unit));
	known = true;
	parsed.reset();
	for (char &character : words)
		if (character == '_') character = ' ';
	if (words.empty()) return "";
	if (words.rfind("calendar ", 0) == 0) words = words.substr(9);
	if (words == "day" || words == "days") parsed = semantic::DurationUnit::Day;
	else if (words == "business day" || words == "business days" || words == "working day" || words == "working days"
			|| words == "banking days" || words == "banking day")
		parsed = semantic::DurationUnit::BusinessDay;
	else if (words == "week" || words == "weeks") parsed = semantic::DurationUnit::Week;
	else if (words == "month" || words == "months") parsed = semantic::DurationUnit::Month;
	else if (words == "year" || words == "years") parsed = semantic::DurationUnit::Year;
	else known = false;
	return words;
}

bool digit(char character) { return character >= '0' && character <= '9'; }

// The number written at text[at]: all its digits ("1,275.00" -> "127500"), the
// digits of its whole part ("1275"), and where it ends. Groups of three after
// a comma, a dot or a space are thousands; a dot, or a comma before one or two
// final digits, begins the fraction.
struct Written {
	std::string whole;
	std::string integer;
	std::size_t end = 0;
};

Written number_at(const std::string &text, std::size_t at) {
	Written number;
	bool fraction = false;
	std::size_t end = at;
	while (end < text.size()) {
		if (digit(text[end])) {
			number.whole += text[end];
			if (!fraction) number.integer += text[end];
			++end;
			continue;
		}
		const char mark = text[end];
		if ((mark != ',' && mark != '.' && mark != ' ') || end + 1 >= text.size() || !digit(text[end + 1])) break;
		std::size_t digits = 0;
		while (end + 1 + digits < text.size() && digit(text[end + 1 + digits])) ++digits;
		if (mark == ' ' && digits != 3) break;
		if (!fraction && (mark == '.' ? digits != 3 || number.integer.size() > 3 : mark == ',' && digits <= 2)) fraction = true;
		++end;
	}
	number.end = end;
	return number;
}

// `text` with every symbol and every statement blanked out (same length, so
// offsets stay): what the model wrote in figures of its own.
std::string outside_symbols(const std::string &text) {
	std::string blank = text;
	const auto wipe = [&](std::size_t from, std::size_t to) {
		for (std::size_t index = from; index < to && index < blank.size(); ++index) blank[index] = ' ';
	};
	for (std::size_t open = text.find("[["); open != std::string::npos; open = text.find("[[", open + 2)) {
		const std::size_t close = text.find("]]", open + 2);
		if (close == std::string::npos || close - open > 64) continue;
		wipe(open, close + 2);
	}
	for (std::size_t open = text.find("{{"); open != std::string::npos; open = text.find("{{", open + 2)) {
		const std::size_t close = text.find("}}", open + 2);
		if (close == std::string::npos) break;
		wipe(open, close + 2);
	}
	// A source handle ([SOURCE:DOC_2:CHUNK_1]) is Singularity's own.
	for (std::size_t open = text.find("[SOURCE:"); open != std::string::npos; open = text.find("[SOURCE:", open + 1)) {
		const std::size_t close = text.find(']', open);
		if (close == std::string::npos) break;
		wipe(open, close + 1);
	}
	return blank;
}


// The number a figure writes, as a canonical decimal ("1,275.00" -> "1275",
// "0,2" -> "0.2", "10" -> "10"): the same figure however it is grouped.
std::string canonical(const Written &number) {
	std::string integer = number.integer;
	while (integer.size() > 1 && integer.front() == '0') integer.erase(0, 1);
	if (integer.empty()) integer = "0";
	std::string fraction = number.whole.size() > number.integer.size() ? number.whole.substr(number.integer.size()) : std::string();
	while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
	return fraction.empty() ? integer : integer + "." + fraction;
}

// Every figure `text` writes outside its symbols and statements, canonical.
std::vector<std::string> numbers_in(const std::string &text) {
	std::vector<std::string> numbers;
	const std::string plain = outside_symbols(text);
	for (std::size_t index = 0; index < plain.size();) {
		if (!digit(plain[index]) || (index > 0 && digit(plain[index - 1]))) {
			++index;
			continue;
		}
		const Written number = number_at(plain, index);
		numbers.push_back(canonical(number));
		index = std::max(number.end, index + 1);
	}
	return numbers;
}

class Executor {
public:
	explicit Executor(Context &context) : context_(context) {}

	// `over_cap`: past the questions one answer may ask - refused alike,
	// whatever it asks.
	OperationAnswer run(const OperationRequest &request, std::size_t index, bool over_cap = false) {
		answer_ = OperationAnswer{};
		answer_.index = index;
		request_ = &request;
		operands_.clear();
		rules_.reset();
		expression_kind_.clear();
		record_ = OutboundRelation{};
		record_.id = "O" + std::to_string(index);
		record_.requested_by = "model";
		record_.justification = "model_operation";
		record_.subproblem = "operation:" + std::to_string(index);
		op_ = lower(trimmed(request.op));
		for (char &character : op_)
			if (character == ' ' || character == '-') character = '_';
		// Names a model may use for the same operation.
		if (op_ == "duration_between" || op_ == "days_between") op_ = "between";
		if (op_ == "add_period" || op_ == "add") op_ = "deadline";
		if (op_ == "multiply") op_ = "product";
		if (op_ == "subtract") op_ = "difference";
		if (op_ == "add_up" || op_ == "total") op_ = "sum";
		if (op_ == "divide") op_ = "ratio";
		if (op_.empty() || op_.size() > 40 || op_.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos)
			return refuse("unknown_op", "expression");
		record_.kind = op_;
		if (over_cap) return refuse("too_many");
		if (op_ == "table_total") return table_total();
		if (op_ == "expression") return expression();
		for (const std::string &written : request.operands) {
			if (trimmed(written).empty()) continue;
			operands_.push_back(resolve(written, slot(operands_.size())));
		}
		// Every refusal below is decided by what the model wrote and what the
		// sent text shows - never by a protected value - and says the same
		// whatever the values are: no answer tells a figure of the model's own
		// apart from a protected one.
		for (const Operand &operand : operands_)
			if (operand.failure == "own_figure" || operand.written_only) return refuse("own_figure");
		for (const Operand &operand : operands_) {
			if (!operand.failure.empty()) return unknown(operand.failure);
		}
		if (!literals_fit()) return refuse("own_figure");
		if (op_ != "compare" && op_ != "equals")
			for (const Operand &operand : operands_)
				if (operand.dimensionless) return unknown("ratio_operand");
		if (!permitted()) return withheld();
		if (op_ == "compare" || op_ == "equals") return compare();
		if (op_ == "before" || op_ == "after" || op_ == "same_day" || op_ == "within" || op_ == "overlaps"
				|| op_ == "same_day_of_year")
			return date_relation();
		if (op_ == "difference" && operands_.size() == 2 && operands_[0].value && operands_[1].value
				&& is_day(*operands_[0].value) && is_day(*operands_[1].value)) {
			std::swap(operands_[0], operands_[1]);
			op_ = "between";
			record_.kind = op_;
		}
		if (op_ == "between") return between();
		if (op_ == "deadline") return deadline();
		if (op_ == "next_business_day") return next_business_day();
		if (op_ == "day_class") return day_class();
		if (op_ == "next_occurrence") return next_occurrence();
		if (op_ == "difference" || op_ == "sum" || op_ == "product" || op_ == "ratio" || op_ == "rate" || op_ == "round")
			return arithmetic();
		return refuse("unknown_op");
	}

private:
	Context &context_;
	OperationAnswer answer_;
	OutboundRelation record_;
	std::string op_;
	const OperationRequest *request_ = nullptr;
	std::vector<Operand> operands_;
	std::optional<business_calendar::GoverningRules> rules_;
	std::optional<std::set<std::string>> open_;
	std::string expression_kind_;

	// Where a figure of the model's own may stand: nowhere but a period, a rate
	// or a count the material states (the period of a deadline, the rate and
	// period of a rate, the other side of a comparison with a period, count or
	// rate the material states), and a rounding's places.
	enum class Slot { Symbol, Parameter, Places };
	Slot slot(std::size_t index) const {
		if (op_ == "round" && index == 1) return Slot::Places;
		if (op_ == "rate" && index >= 1) return Slot::Parameter;
		if (op_ == "deadline" && index == 1) return Slot::Parameter;
		if (op_ == "compare" || op_ == "equals") return Slot::Parameter;
		return Slot::Symbol;
	}

	// The figures the sent text writes openly (outside its symbols), canonical.
	const std::set<std::string> &open_figures() {
		if (!open_) {
			const std::vector<std::string> numbers = numbers_in(context_.sent);
			open_.emplace(numbers.begin(), numbers.end());
		}
		return *open_;
	}

	// A comparison with a figure of the model's own relates it only to a
	// period, count or rate the material states, of the same kind - never to
	// an amount, a date or a value Singularity worked out.
	bool literals_fit() const {
		std::size_t literals = 0;
		for (const Operand &operand : operands_) literals += operand.literal ? 1 : 0;
		if (literals == 0 || (op_ != "compare" && op_ != "equals")) return true;
		if (literals != 1 || operands_.size() != 2) return false;
		const Operand &fact = operands_[0].literal ? operands_[1] : operands_[0];
		const Operand &figure = operands_[0].literal ? operands_[0] : operands_[1];
		if (!fact.row || fact.row->derived || !fact.value || !figure.value) return false;
		const VKind kind = fact.value->kind;
		return (kind == VKind::Duration || kind == VKind::Number || kind == VKind::Percentage) && figure.value->kind == kind;
	}

	// ---- operands -----------------------------------------------------------------------------

	Operand resolve(const std::string &written, Slot slot = Slot::Symbol) {
		Operand operand;
		operand.written = trimmed(written);
		operand.marker = marker_of(operand.written);
		if (!operand.marker.empty()) {
			const RequestMappingEntry *row = context_.row_of ? context_.row_of(operand.marker) : nullptr;
			if (row == nullptr) {
				operand.failure = "operand_unknown";
				return operand;
			}
			operand.row = *row;
			operand.marker = row->placeholder;
			if (!row->value) {
				operand.failure = "not_a_quantity";
				return operand;
			}
			operand.value = row->value;
			operand.key = !row->fact_key.empty() ? row->fact_key : "sym:" + row->placeholder;
			if (row->derived) {
				operand.written_only = row->provenance.derivation.find("|written_only") != std::string::npos;
				operand.dimensionless = row->provenance.derivation.find("|dimensionless") != std::string::npos;
				operand.period_literal = row->provenance.derivation.find("|period_literal") != std::string::npos;
			}
			return operand;
		}
		// The model's own figure. Whether it may stand here is told by where it
		// stands and what the sent text writes openly - never by the protected
		// values, so no answer tells whether a figure is one of them.
		operand.literal = true;
		const std::string text = [&] {
			std::string inner = operand.written;
			if (inner.size() > kOpen.size() + kClose.size() && inner.compare(0, kOpen.size(), kOpen) == 0
					&& inner.compare(inner.size() - kClose.size(), kClose.size(), kClose) == 0)
				inner = trimmed(inner.substr(kOpen.size(), inner.size() - kOpen.size() - kClose.size()));
			return inner;
		}();
		operand.written = text;
		if (text.find("[[") != std::string::npos) {
			operand.failure = "operand_unknown";
			return operand;
		}
		if (slot == Slot::Symbol) {
			operand.failure = "own_figure";
			return operand;
		}
		std::optional<Value> parsed = semantic::parse_value(text);
		if (!parsed) {
			// "10" with the unit given apart.
			const auto count = semantic::parse_value(text + " days");
			if (count && count->kind == VKind::Duration) parsed = semantic::make_number(count->amount);
		}
		if (!parsed) {
			operand.failure = "operand_unknown";
			return operand;
		}
		operand.value = parsed;
		operand.key = "lit:" + semantic::value_key(*parsed);
		if (slot == Slot::Places) {
			const bool places = parsed->kind == VKind::Number && parsed->amount.is_integer() && parsed->amount >= semantic::Rational(0)
				&& parsed->amount <= semantic::Rational(9);
			if (!places) operand.failure = "own_figure";
			return operand;
		}
		// A period, a rate or a count - as the sent text writes it, figure for
		// figure: an amount or a date is only ever a symbol.
		const VKind kind = parsed->kind;
		if (kind != VKind::Duration && kind != VKind::Number && kind != VKind::Percentage) {
			operand.failure = "own_figure";
			return operand;
		}
		const std::vector<std::string> figures = numbers_in(text);
		if (figures.empty()) {
			operand.failure = "own_figure";
			return operand;
		}
		for (const std::string &figure : figures)
			if (open_figures().count(figure) == 0) {
				operand.failure = "own_figure";
				return operand;
			}
		// One the guard protects elsewhere is a symbol there, and is one here.
		if (context_.table != nullptr)
			for (const RequestMappingEntry &row : context_.table->mapping)
				if (!row.derived && row.value && row.value->kind == kind && semantic::value_key(*row.value) == semantic::value_key(*parsed)) {
					operand.failure = "own_figure";
					return operand;
				}
		// Local repair: a rate given without its period takes the one the sent
		// text writes it with ("0.2%" for «…»), when every place it
		// writes that rate names the same period; else it keeps none.
		if (kind == VKind::Percentage && parsed->basis == semantic::RateBasis::None) {
			operand.value->basis = written_basis(*parsed);
		}
		return operand;
	}

	// The period the sent text writes a rate of this value with, where every
	// writing of it names one and the same; None otherwise.
	semantic::RateBasis written_basis(const Value &rate) const {
		const std::string &sent = context_.sent;
		const std::string wanted = semantic::value_key(rate);
		std::optional<semantic::RateBasis> found;
		std::size_t at = 0;
		while (at < sent.size()) {
			if (sent[at] < '0' || sent[at] > '9' || (at > 0 && ((sent[at - 1] >= '0' && sent[at - 1] <= '9') || sent[at - 1] == '.'
					|| sent[at - 1] == ','))) {
				++at;
				continue;
			}
			std::size_t end = at;
			while (end < sent.size() && ((sent[end] >= '0' && sent[end] <= '9')
					|| ((sent[end] == '.' || sent[end] == ',') && end + 1 < sent.size() && sent[end + 1] >= '0' && sent[end + 1] <= '9')))
				++end;
			std::size_t sign = end;
			if (sign < sent.size() && sent[sign] == ' ') ++sign;
			if (sent.compare(sign, 2, "\xC2\xA0") == 0) sign += 2;
			if (sign < sent.size() && sent[sign] == '%') {
				const std::optional<Value> written = semantic::parse_value(sent.substr(at, sign + 1 - at));
				if (written && written->kind == VKind::Percentage && semantic::value_key(*written) == wanted) {
					const semantic::RateBasis basis = semantic::rate_basis_of_clause(sent, at, sign + 1);
					if (found && *found != basis) return semantic::RateBasis::None;
					found = basis;
				}
			}
			at = end;
		}
		return found.value_or(semantic::RateBasis::None);
	}

	// What a relation tells, whatever its op is called: an order or an equality,
	// a difference, a sum, or a scale between quantities - how the relation
	// policy classifies it (two parties' figures side by side).
	std::string revealed_kind() const {
		if (op_ == "compare" || op_ == "equals" || op_ == "before" || op_ == "after" || op_ == "same_day" || op_ == "within"
				|| op_ == "overlaps" || op_ == "same_day_of_year")
			return "compare";
		if (op_ == "difference" || op_ == "between") return "difference";
		if (op_ == "sum" || op_ == "table_total") return "sum";
		if (op_ == "ratio" || op_ == "product" || op_ == "rate") return "ratio";
		if (op_ == "expression" && !expression_kind_.empty()) return expression_kind_;
		return op_;
	}

	RelationFacts facts() const {
		RelationFacts facts;
		facts.kind = revealed_kind();
		for (const Operand &operand : operands_) {
			if (!operand.row) continue;
			facts.operand_categories.push_back(context_.category ? context_.category(*operand.row) : std::string("PROTECTED_VALUE"));
			facts.operand_parties.push_back(context_.party_of ? context_.party_of(*operand.row) : std::string());
		}
		return facts;
	}

	bool permitted() {
		const RelationFacts relation = facts();
		record_.relation_sensitivity = context_.classify && context_.classify(relation) == RelationSensitivity::Sensitive
			? "sensitive" : "ordinary";
		std::string heaviest;
		for (const std::string &category : relation.operand_categories)
			if (heaviest.empty() || (category == "MONEY_AMOUNT" && heaviest != "MONEY_AMOUNT")) heaviest = category;
		record_.value_sensitivity = heaviest;
		for (const Operand &operand : operands_) {
			if (!operand.row) continue;
			record_.operands.push_back(operand.marker);
			if (operand.row->derived) continue;
			RelationSource source;
			source.marker = operand.marker;
			source.origin = context_.origin ? context_.origin(*operand.row) : std::string("source");
			source.passage_sha256 = operand.row->provenance.passage_sha256;
			source.offset = operand.row->provenance.offset;
			source.length = operand.row->provenance.length;
			source.cell = operand.row->provenance.cell;
			record_.sources.push_back(std::move(source));
		}
		return !context_.permit || context_.permit(relation);
	}

	// The canonical derivation of a result: the op over its operands' facts, so
	// the same operation on the same facts is the same derived fact.
	std::string derivation(const std::string &extra = {}) const {
		std::string key = op_ + "(";
		for (std::size_t index = 0; index < operands_.size(); ++index) key += (index == 0 ? "" : ",") + operands_[index].key;
		key += ")";
		if (!extra.empty()) key += "|" + extra;
		// What a figure of the model's own shaped: a date moved by its period is
		// marked so, and a count of days reaching one is only written.
		bool fromPeriod = false;
		for (const Operand &operand : operands_)
			fromPeriod = fromPeriod || operand.period_literal || (operand.literal && op_ == "deadline");
		if (fromPeriod) key += op_ == "between" ? "|written_only" : "|period_literal";
		return key;
	}

	// The operands joined by `sign`, each as written (a figure of the model's
	// own stands only where the sent text writes it).
	std::string over_markers(const std::string &sign) const {
		std::string text;
		for (const Operand &operand : operands_) text += (text.empty() ? "" : " " + sign + " ") + operand.written;
		return text;
	}

	// ---- answers ------------------------------------------------------------------------------

	std::string render(control::Template id, const std::vector<control::Slot> &slots) const {
		return context_.render ? context_.render(id, slots) : std::string();
	}

	control::Slot count() const { return {control::Slot::Kind::Count, std::to_string(answer_.index)}; }
	control::Slot word(const std::string &value) const { return {control::Slot::Kind::Word, value}; }

	OperationAnswer finish(Status status, const std::string &reason, control::Template head, const Line &body) {
		answer_.status = status;
		answer_.reason = reason;
		std::vector<control::Slot> slots{count(), word(op_)};
		if (head == control::Template::OperationUnknown || head == control::Template::OperationRefused) slots.push_back(word(reason));
		const std::string opening = render(head, slots);
		answer_.line = opening + body.model();
		answer_.local_line = opening + body.local(context_);
		record_.decision = reason == "withheld_by_policy" ? "withheld" : "sent";
		record_.sent = record_.decision == "sent" ? answer_.line : std::string();
		record_.results = answer_.results;
		answer_.record = record_;
		return answer_;
	}

	OperationAnswer refuse(const std::string &reason, const std::string &op = {}) {
		if (!op.empty()) op_ = op;
		record_.kind = op_;
		return finish(Status::Refused, reason, control::Template::OperationRefused, Line().say(reason_words(reason)));
	}

	OperationAnswer unknown(const std::string &reason, Line body = {}) {
		if (body.pieces.empty()) body.say(reason_words(reason));
		return finish(Status::Unknown, reason, control::Template::OperationUnknown, body);
	}

	OperationAnswer withheld() {
		// Nothing of the relation is said, not even that it would hold.
		return finish(Status::Unknown, "withheld_by_policy", control::Template::OperationUnknown,
			Line().say(reason_words("withheld_by_policy")));
	}

	OperationAnswer exact(const Line &body) { return finish(Status::Exact, "", control::Template::OperationResult, body); }

	OperationAnswer bounded(const Line &body) { return finish(Status::Bounded, "", control::Template::OperationBounded, body); }

	// A derived fact's symbol, or "" when none could be issued.
	std::string issue(const Value &value, const std::string &how, const std::string &shown) {
		if (!context_.issue) return {};
		const std::string marker = context_.issue(value, type_of(value), how);
		if (marker.empty()) return {};
		answer_.results.push_back(marker);
		record_.derivation += (record_.derivation.empty() ? "" : "; ") + marker + " := " + shown;
		return marker;
	}

	bool needs(std::size_t fewest, std::size_t most) const { return operands_.size() >= fewest && operands_.size() <= most; }

	// ---- relations ----------------------------------------------------------------------------

	OperationAnswer compare() {
		if (!needs(2, 2)) return unknown("arity");
		Value a = *operands_[0].value;
		Value b = *operands_[1].value;
		const Operand &left = operands_[0];
		const Operand &right = operands_[1];
		if (is_date(a) != is_date(b)) return unknown("not_a_quantity");
		if (is_date(a)) {
			const auto truth = [&](semantic::Relation relation) { return relation.truth; };
			semantic::Truth equal = semantic::Truth::Undetermined;
			semantic::Truth earlier = semantic::Truth::Undetermined;
			semantic::Truth later = semantic::Truth::Undetermined;
			try {
				equal = truth(semantic::same_day(a, b));
				earlier = truth(semantic::before(a, b));
				later = truth(semantic::after(a, b));
			} catch (const semantic::AlgebraError &) {
				return unknown("not_a_quantity");
			}
			if (op_ == "equals") {
				if (equal == semantic::Truth::True) return exact(Line().operand(left).say(" is the same day as ").operand(right).say(" (EQUAL)"));
				if (earlier == semantic::Truth::True || later == semantic::Truth::True)
					return exact(Line().operand(left).say(" is not the same day as ").operand(right).say(" (NOT_EQUAL)"));
				return unknown("precision_insufficient");
			}
			if (equal == semantic::Truth::True) return exact(Line().operand(left).say(" is the same day as ").operand(right).say(" (EQUAL)"));
			if (earlier == semantic::Truth::True) return exact(Line().operand(left).say(" is before ").operand(right).say(" (LESS)"));
			if (later == semantic::Truth::True) return exact(Line().operand(left).say(" is after ").operand(right).say(" (GREATER)"));
			return unknown("precision_insufficient");
		}
		// Local repair: an amount whose currency is not written takes the other's.
		if (a.kind == VKind::Money && b.kind == VKind::Money) {
			if (a.currency.empty()) a.currency = b.currency;
			if (b.currency.empty()) b.currency = a.currency;
		}
		const auto holds = [&](semantic::Comparator comparator) {
			return semantic::compare(a, comparator, b).truth;
		};
		try {
			const semantic::Truth equal = holds(semantic::Comparator::Equal);
			if (op_ == "equals") {
				if (equal == semantic::Truth::True) return exact(Line().operand(left).say(" is equal to ").operand(right).say(" (EQUAL)"));
				if (equal == semantic::Truth::False)
					return exact(Line().operand(left).say(" is not equal to ").operand(right).say(" (NOT_EQUAL)"));
				return unknown("precision_insufficient");
			}
			if (equal == semantic::Truth::True) return exact(Line().operand(left).say(" is equal to ").operand(right).say(" (EQUAL)"));
			if (holds(semantic::Comparator::Greater) == semantic::Truth::True)
				return exact(Line().operand(left).say(" is greater than ").operand(right).say(" (GREATER)"));
			if (holds(semantic::Comparator::Less) == semantic::Truth::True)
				return exact(Line().operand(left).say(" is less than ").operand(right).say(" (LESS)"));
		} catch (const semantic::AlgebraError &error) {
			return unknown(error.kind() == "currency" ? "currency_mismatch" : "not_a_quantity");
		}
		return unknown("precision_insufficient");
	}

	OperationAnswer date_relation() {
		const bool period = op_ == "within";
		if (!(period ? needs(2, 3) : needs(2, 2))) return unknown("arity");
		for (const Operand &operand : operands_)
			if (!is_date(*operand.value)) return unknown("not_a_quantity");
		semantic::Relation relation;
		std::string holds;
		std::string fails;
		try {
			const Value &a = *operands_[0].value;
			const Value &b = *operands_[1].value;
			if (op_ == "before") { relation = semantic::before(a, b); holds = " is before "; fails = " is not before "; }
			else if (op_ == "after") { relation = semantic::after(a, b); holds = " is after "; fails = " is not after "; }
			else if (op_ == "same_day") { relation = semantic::same_day(a, b); holds = " is the same day as "; fails = " is not the same day as "; }
			else if (op_ == "overlaps") { relation = semantic::overlaps(a, b); holds = " overlaps "; fails = " does not overlap "; }
			else if (op_ == "same_day_of_year") {
				relation = semantic::same_day_of_year(a, b);
				holds = " falls on the same day of the year as ";
				fails = " does not fall on the same day of the year as ";
			} else {
				relation = operands_.size() == 2 ? semantic::within(a, b) : semantic::within(a, b, *operands_[2].value);
				holds = " is within ";
				fails = " is not within ";
			}
		} catch (const semantic::AlgebraError &) {
			return unknown("not_a_quantity");
		}
		if (relation.truth == semantic::Truth::Undetermined) return unknown("precision_insufficient");
		Line line;
		line.operand(operands_[0]).say(relation.truth == semantic::Truth::True ? holds : fails).operand(operands_[1]);
		if (operands_.size() == 3) line.say(" to ").operand(operands_[2]);
		return exact(line);
	}

	// ---- arithmetic ---------------------------------------------------------------------------

	std::optional<std::string> writing(const Value &value) const {
		return context_.exact_writing ? context_.exact_writing(value) : std::nullopt;
	}

	// An expression over the operands' exact writings.
	std::optional<std::string> expression_of(const std::string &sign) const {
		std::string text;
		for (const Operand &operand : operands_) {
			const auto written = writing(*operand.value);
			if (!written) return std::nullopt;
			text += (text.empty() ? "" : " " + sign + " ") + kOpen + *written + kClose;
		}
		return text;
	}

	// Whether this op's result may be rounded: said alike for every value, so
	// how the words run never tells whether a value needed it.
	bool rounding_op() const {
		return op_ == "product" || op_ == "ratio" || op_ == "rate" || op_ == "round" || op_ == "expression";
	}

	// `shown` is the derivation as the record keeps it; `said` as the line says
	// it (the same, unless it is an expression, which is not written back).
	OperationAnswer worked(const std::string &expression, const std::string &shown, const std::string &extra = {},
			const std::optional<std::string> &said = std::nullopt) {
		if (!context_.work_out) return unknown("not_worked_out");
		singularity::context::computed_statements::WorkedValue result = context_.work_out(expression);
		// A quotient or a product with no exact decimal writing (1 / 3) is
		// rounded to six places - never refused for it, which would tell that
		// the values do not divide evenly.
		if (!result.value && result.failure == "no_exact_writing" && rounding_op())
			result = context_.work_out("round(" + expression + ", 6)");
		if (!result.value) return unknown(result.failure == "undetermined" ? "precision_insufficient" : result.failure);
		if (result.value->kind == VKind::Boolean)
			return exact(Line().say(said.value_or(shown)).say(result.value->truth ? " holds (TRUE)" : " does not hold (FALSE)"));
		const std::string marker = issue(*result.value, derivation(extra), shown);
		const std::string &text = said ? *said : shown;
		if (marker.empty()) return unknown("not_worked_out");
		Line line;
		line.symbol(marker).say(" = ");
		// The derivation over the operands, as the model wrote them.
		for (std::size_t index = 0; index < text.size();) {
			const std::size_t open = text.find("[[", index);
			if (open == std::string::npos) { line.say(text.substr(index)); break; }
			const std::size_t close = text.find("]]", open);
			if (close == std::string::npos) { line.say(text.substr(index)); break; }
			if (open > index) line.say(text.substr(index, open - index));
			line.symbol(text.substr(open, close + 2 - open));
			index = close + 2;
		}
		line.say(" (" + kind_words(*result.value, rounding_op() ? (op_ == "round" ? 1 : 2) : 0) + ")");
		return exact(line);
	}

	OperationAnswer arithmetic() {
		const bool chain = op_ == "sum" || op_ == "product";
		// Each fact once: a fact summed with itself again and again would build
		// any multiple of it.
		if (chain)
			for (std::size_t first = 0; first < operands_.size(); ++first)
				for (std::size_t second = first + 1; second < operands_.size(); ++second)
					if (operands_[first].key == operands_[second].key) return unknown("same_operand");
		if (op_ == "round" ? !needs(1, 2) : op_ == "rate" ? !needs(2, 3) : chain ? !needs(2, 64) : !needs(2, 2))
			return op_ == "sum" && needs(1, 1) ? arithmetic_single() : unknown("arity");
		for (const Operand &operand : operands_)
			if (operand.value->kind == VKind::Boolean || operand.value->kind == VKind::DayOfYear) return unknown("not_a_quantity");
		// Local repair: an amount whose currency is not written takes the others'.
		std::string currency;
		for (const Operand &operand : operands_)
			if (operand.value->kind == VKind::Money && !operand.value->currency.empty()) {
				if (!currency.empty() && currency != operand.value->currency) return unknown("currency_mismatch");
				currency = operand.value->currency;
			}
		for (Operand &operand : operands_)
			if (operand.value->kind == VKind::Money && operand.value->currency.empty()) operand.value->currency = currency;
		if (op_ == "round") {
			int places = operands_[0].value->kind == VKind::Money ? semantic::minor_unit_places(operands_[0].value->currency) : -1;
			if (operands_.size() == 2) {
				const Value &count = *operands_[1].value;
				if (count.kind != VKind::Number || !count.amount.is_integer() || count.amount < semantic::Rational(-9)
						|| count.amount > semantic::Rational(9))
					return unknown("not_a_quantity");
				places = static_cast<int>(count.amount.numerator());
			}
			if (places < 0) return unknown("arity");
			const auto written = writing(*operands_[0].value);
			if (!written) return unknown("no_exact_writing");
			return worked("round(" + kOpen + *written + kClose + ", " + std::to_string(places) + ")",
				"round(" + operands_[0].written + ", " + std::to_string(places) + ")", std::to_string(places));
		}
		if (op_ == "rate" && operands_[0].value->kind != VKind::Money && operands_[0].value->kind != VKind::Number)
			return unknown("not_a_quantity");
		// A rate of no known period is never counted over a period: 5 % a year
		// for 45 days is not 5 % * 45. Its period is the material's own ("5% per
		// annum", «…»), or the answer is UNKNOWN.
		if (op_ == "rate" && operands_.size() == 3 && operands_[2].value->kind == VKind::Duration
				&& operands_[1].value->kind == VKind::Percentage && operands_[1].value->basis == semantic::RateBasis::None)
			return unknown("rate_basis_unknown");
		const std::string sign = op_ == "difference" ? "-" : op_ == "sum" ? "+" : op_ == "ratio" ? "/" : "*";
		const auto expression = expression_of(sign);
		if (!expression) return unknown("no_exact_writing");
		// A ratio of two quantities of one kind is a pure number: read and
		// compared, never calculated with further.
		const bool dimensionless = op_ == "ratio" && operands_[0].value->kind == operands_[1].value->kind;
		return worked(*expression, over_markers(sign), dimensionless ? "dimensionless" : std::string());
	}

	OperationAnswer arithmetic_single() {
		const auto written = writing(*operands_[0].value);
		if (!written) return unknown("no_exact_writing");
		return worked(kOpen + *written + kClose, operands_[0].written);
	}

	// ---- calendar -----------------------------------------------------------------------------

	// The rule as a derivation names it: its source, its days and where it is
	// defined - a count under another rule is another fact.
	static std::string rule_key(const business_calendar::BusinessDayRule &rule) {
		std::string weekend;
		for (const int day : rule.weekend) weekend += std::to_string(day);
		std::string calendars;
		for (const std::string &id : rule.calendars) calendars += id + ",";
		for (const std::string &id : rule.possible_calendars) calendars += "?" + id + ",";
		for (const business_calendar::DayRule &named : rule.named_days)
			calendars += "n" + std::to_string(named.month) + "." + std::to_string(named.day) + "." + std::to_string(named.easter_offset) + ",";
		return business_calendar::rule_name(rule) + "|" + calendars + (rule.holidays_known ? "k" : "u") + "|" + weekend + "|"
			+ rule.roll + "|" + rule.defining_passage_sha256 + "@" + std::to_string(rule.defining_offset);
	}

	// The rules that govern this operation: found for its own facts - the
	// documents that count in business days and write them - when the caller
	// can, else the request's.
	const business_calendar::GoverningRules &rules() {
		if (!rules_) {
			if (context_.rules_for) {
				std::vector<std::string> writings;
				for (const Operand &operand : operands_)
					if (operand.row && !operand.row->derived && !operand.row->original.empty()) writings.push_back(operand.row->original);
				rules_ = context_.rules_for(writings);
			} else {
				rules_ = context_.rules;
			}
		}
		return *rules_;
	}

	// The business-day rule that governs, or why none can be told.
	std::optional<business_calendar::BusinessDayRule> governing(std::string &reason) {
		const business_calendar::GoverningRules &found = rules();
		if (!found.business_day) {
			reason = found.unknown_reason.empty() ? "rule_unknown" : found.unknown_reason;
			return std::nullopt;
		}
		const business_calendar::BusinessDayRule &rule = *found.business_day;
		record_.business_day_rule = business_calendar::rule_name(rule);
		if (!rule.defining_passage_sha256.empty()) {
			record_.business_day_rule_source.origin = "source";
			record_.business_day_rule_source.passage_sha256 = rule.defining_passage_sha256;
			record_.business_day_rule_source.offset = rule.defining_offset;
			record_.business_day_rule_source.length = rule.defining_length;
		}
		return rule;
	}

	Value day_value(long long day, const Value &like) const {
		return semantic::make_date(temporal::TemporalValue{day, day, temporal::Precision::Day},
			like.date_style.empty() ? std::string("english") : like.date_style);
	}

	OperationAnswer between() {
		if (!needs(2, 2)) return unknown("arity");
		const Value &a = *operands_[0].value;
		const Value &b = *operands_[1].value;
		if (a.kind != VKind::Date || b.kind != VKind::Date) return unknown("not_a_quantity");
		bool known = true;
		std::optional<semantic::DurationUnit> unit;
		const std::string asked = unit_word(request_->unit, known, unit);
		if (!known) return unknown("not_a_quantity");
		const bool business = unit == semantic::DurationUnit::BusinessDay
			|| (lower(trimmed(request_->rule)) == "governing" && !unit);
		if (business) {
			if (!is_day(a) || !is_day(b)) return unknown("precision_insufficient");
			std::string reason;
			const auto rule = governing(reason);
			if (!rule) return unknown(reason);
			const business_calendar::CountBound bound = business_calendar::count_bound(*rule, a.date.first_day, b.date.first_day);
			const std::string words = " (" + business_calendar::explain(*rule, bound.assumptions) + ")";
			if (bound.exact) {
				const std::string marker = issue(semantic::make_duration(semantic::Rational(*bound.exact), semantic::DurationUnit::BusinessDay),
					derivation("bdays|" + rule_key(*rule)),
					"business_days_between(" + operands_[0].written + ", " + operands_[1].written + ", rule)");
				if (marker.empty()) return unknown("not_worked_out");
				return exact(Line().symbol(marker).say(" = the business days after ").operand(operands_[0])
					.say(" up to and including ").operand(operands_[1]).say(words));
			}
			const std::string marker = issue(semantic::make_duration(semantic::Rational(bound.at_most), semantic::DurationUnit::BusinessDay),
				derivation("bdays_at_most|" + rule_key(*rule)),
				"at_most_business_days_between(" + operands_[0].written + ", " + operands_[1].written + ", rule)");
			if (marker.empty()) return unknown("not_worked_out");
			return bounded(Line().say("at most ").symbol(marker).say(" after ").operand(operands_[0])
				.say(" up to and including ").operand(operands_[1]).say(" - the exact count depends on closures not known here" + words));
		}
		const auto from = writing(a);
		const auto to = writing(b);
		if (!from || !to) return unknown("no_exact_writing");
		const bool months = unit == semantic::DurationUnit::Month || unit == semantic::DurationUnit::Year;
		return worked(std::string(months ? "months_between(" : "days_between(") + kOpen + *from + kClose + ", " + kOpen + *to
				+ kClose + ")",
			std::string(months ? "months_between(" : "days_between(") + operands_[0].written + ", " + operands_[1].written + ")",
			asked);
	}

	// The duration operand of a deadline: a period, or a count with the unit given apart.
	std::optional<Value> period_of(const Operand &operand) {
		const Value &value = *operand.value;
		if (value.kind == VKind::Duration) return value;
		if (value.kind != VKind::Number || !value.amount.is_integer()) return std::nullopt;
		bool known = true;
		std::optional<semantic::DurationUnit> unit;
		unit_word(request_->unit, known, unit);
		if (!known || !unit) return std::nullopt;
		return semantic::make_duration(value.amount, *unit);
	}

	OperationAnswer deadline() {
		if (!needs(2, 2)) return unknown("arity");
		const Value &from = *operands_[0].value;
		if (from.kind != VKind::Date) return unknown("not_a_quantity");
		const std::optional<Value> period = period_of(operands_[1]);
		if (!period) return unknown("not_a_quantity");
		const std::string shown = "deadline(" + operands_[0].written + ", " + operands_[1].written + ")";
		if (period->unit == semantic::DurationUnit::BusinessDay) {
			if (!is_day(from)) return unknown("precision_insufficient");
			if (!period->amount.is_integer()) return unknown("not_a_quantity");
			std::string reason;
			const auto rule = governing(reason);
			if (!rule) return unknown(reason);
			const long long count = static_cast<long long>(period->amount.numerator());
			const business_calendar::DayBound bound = business_calendar::deadline_bound(*rule, from.date.first_day, count);
			const std::string words = " (" + business_calendar::explain(*rule, bound.assumptions) + ")";
			if (bound.exact) {
				const std::string marker = issue(day_value(*bound.exact, from), derivation("bdeadline|" + rule_key(*rule)), shown);
				if (marker.empty()) return unknown("not_worked_out");
				return exact(Line().symbol(marker).say(" = ").operand(operands_[1]).say(" after ").operand(operands_[0]).say(words));
			}
			if (bound.not_before || bound.not_after) {
				Line line;
				line.say("the day ").operand(operands_[1]).say(" after ").operand(operands_[0]);
				if (bound.not_before) {
					const std::string marker = issue(day_value(*bound.not_before, from), derivation("bdeadline_not_before|" + rule_key(*rule)), shown);
					if (marker.empty()) return unknown("not_worked_out");
					line.say(" is not before ").symbol(marker);
				}
				if (bound.not_after) {
					const std::string marker = issue(day_value(*bound.not_after, from), derivation("bdeadline_not_after|" + rule_key(*rule)), shown);
					if (marker.empty()) return unknown("not_worked_out");
					line.say(bound.not_before ? " and not after " : " is not after ").symbol(marker);
				}
				return bounded(line.say(" - the exact day depends on closures not known here" + words));
			}
			return unknown("rule_unknown");
		}
		// Calendar units: the date plus the period, then rolled when the rule that
		// governs rolls a period's end to the next business day.
		const auto date = writing(from);
		const auto length = writing(*period);
		if (!date || !length) return unknown("no_exact_writing");
		if (!context_.work_out) return unknown("not_worked_out");
		const singularity::context::computed_statements::WorkedValue sum = context_.work_out(kOpen + *date + kClose + " + " + kOpen + *length + kClose);
		if (!sum.value || sum.value->kind != VKind::Date)
			return unknown(sum.failure == "undetermined" ? "precision_insufficient" : sum.failure.empty() ? "not_a_quantity" : sum.failure);
		Value end = *sum.value;
		if (!is_day(end)) {
			const std::string marker = issue(end, derivation("calendar|none"), shown);
			if (marker.empty()) return unknown("not_worked_out");
			return exact(Line().symbol(marker).say(" = ").operand(operands_[1]).say(" after ").operand(operands_[0]));
		}
		// The period's last day under the governing rule's roll: the day itself
		// when it is a business day; moved by the operative document's clause or
		// its law; a bound when it is not a business day and no roll is known -
		// never the weekend day passed off as exact.
		std::optional<business_calendar::BusinessDayRule> rule;
		if (rules().business_day) {
			std::string reason;
			rule = governing(reason);
		}
		const business_calendar::DayBound bound = business_calendar::calendar_period_end(rule ? &*rule : nullptr, end.date.first_day);
		const std::string key = rule ? rule_key(*rule) : std::string("none");
		std::string words;
		if (rule && (!rule->roll.empty() || !bound.assumptions.empty())) {
			std::vector<std::string> said = bound.assumptions;
			// A clause scoped to payments moves no other period: its words are not said.
			const bool reaches = rule->roll_scope != "payments";
			if (reaches && (rule->roll == "next_business_day" || rule->roll == "modified_following"))
				said.insert(said.begin(), "a period ending on a day that is not a business day ends on the next one");
			else if (reaches && rule->roll == "preceding_business_day")
				said.insert(said.begin(), "a period ending on a day that is not a business day ends on the one before");
			words = " (" + business_calendar::explain(*rule, said) + ")";
		} else if (!bound.assumptions.empty()) {
			std::string said;
			for (const std::string &one : bound.assumptions) said += (said.empty() ? "" : "; ") + one;
			words = " (" + said + ")";
		}
		if (bound.exact) {
			end = day_value(*bound.exact, from);
			const std::string marker = issue(end, derivation("calendar|" + key), shown);
			if (marker.empty()) return unknown("not_worked_out");
			return exact(Line().symbol(marker).say(" = ").operand(operands_[1]).say(" after ").operand(operands_[0]).say(words));
		}
		if (bound.not_before || bound.not_after) {
			Line line;
			line.say("the period ").operand(operands_[1]).say(" after ").operand(operands_[0]);
			if (bound.not_before) {
				const std::string marker = issue(day_value(*bound.not_before, from), derivation("deadline_not_before|" + key), shown);
				if (marker.empty()) return unknown("not_worked_out");
				line.say(" ends on or after ").symbol(marker);
			}
			if (bound.not_after) {
				const std::string marker = issue(day_value(*bound.not_after, from), derivation("deadline_not_after|" + key), shown);
				if (marker.empty()) return unknown("not_worked_out");
				line.say(bound.not_before ? " and on or before " : " ends on or before ").symbol(marker);
			}
			return bounded(line.say(words));
		}
		return unknown("rule_unknown");
	}

	OperationAnswer next_business_day() {
		if (!needs(1, 1)) return unknown("arity");
		const Value &day = *operands_[0].value;
		if (!is_day(day)) return unknown(day.kind == VKind::Date ? "precision_insufficient" : "not_a_quantity");
		std::string reason;
		const auto rule = governing(reason);
		if (!rule) return unknown(reason);
		const business_calendar::DayBound bound = business_calendar::next_business_day_bound(*rule, day.date.first_day);
		const std::string words = " (" + business_calendar::explain(*rule, bound.assumptions) + ")";
		const std::string shown = "next_business_day(" + operands_[0].written + ")";
		if (bound.exact) {
			const std::string marker = issue(day_value(*bound.exact, day), derivation("next_bday|" + rule_key(*rule)), shown);
			if (marker.empty()) return unknown("not_worked_out");
			return exact(Line().symbol(marker).say(" = the first business day on or after ").operand(operands_[0]).say(words));
		}
		if (bound.not_before) {
			const std::string marker = issue(day_value(*bound.not_before, day),
				derivation("next_bday_not_before|" + rule_key(*rule)), shown);
			if (marker.empty()) return unknown("not_worked_out");
			Line line;
			line.say("the first business day on or after ").operand(operands_[0]).say(" is not before ").symbol(marker);
			if (bound.not_after) {
				const std::string latest = issue(day_value(*bound.not_after, day), derivation("next_bday_not_after|" + rule_key(*rule)), shown);
				if (latest.empty()) return unknown("not_worked_out");
				line.say(" and not after ").symbol(latest);
			}
			return bounded(line.say(" - it depends on closures not known here" + words));
		}
		return unknown("rule_unknown");
	}

	OperationAnswer day_class() {
		if (!needs(1, 1)) return unknown("arity");
		const Value &day = *operands_[0].value;
		if (!is_day(day)) return unknown(day.kind == VKind::Date ? "precision_insufficient" : "not_a_quantity");
		std::string reason;
		const auto rule = governing(reason);
		if (!rule) return unknown(reason);
		const long long at = day.date.first_day;
		const business_calendar::CountBound around = business_calendar::count_bound(*rule, at - 1, at);
		const std::string words = " (" + business_calendar::explain(*rule, around.assumptions) + ")";
		switch (business_calendar::status(*rule, at)) {
			case business_calendar::DayStatus::Weekend: return exact(Line().operand(operands_[0]).say(" is a weekend day" + words));
			case business_calendar::DayStatus::Holiday: return exact(Line().operand(operands_[0]).say(" is a holiday" + words));
			case business_calendar::DayStatus::Business: return exact(Line().operand(operands_[0]).say(" is a business day" + words));
			case business_calendar::DayStatus::Possible:
				return bounded(Line().operand(operands_[0]).say(" is a day the definition's words may or may not close" + words));
			case business_calendar::DayStatus::Open: break;
		}
		return bounded(Line().operand(operands_[0]).say(" is a weekday; whether it is a holiday is not known here" + words));
	}

	OperationAnswer next_occurrence() {
		if (!needs(2, 2)) return unknown("arity");
		try {
			const Value next = semantic::next_occurrence(*operands_[0].value, *operands_[1].value);
			const std::string marker = issue(next, derivation(), "next_occurrence(" + operands_[0].written + ", " + operands_[1].written + ")");
			if (marker.empty()) return unknown("not_worked_out");
			return exact(Line().symbol(marker).say(" = the first ").operand(operands_[0]).say(" on or after ").operand(operands_[1]));
		} catch (const semantic::AlgebraError &error) {
			return unknown(error.kind() == "undetermined" ? "precision_insufficient" : "not_a_quantity");
		}
	}

	// ---- tables and expressions ---------------------------------------------------------------

	OperationAnswer table_total() {
		const std::vector<std::string> &given = request_->operands;
		if (given.size() < 2 || given.size() > 3) return unknown("arity");
		const std::string table = trimmed(given[0]);
		std::string column = trimmed(given[1]);
		const std::string hiddenWord = given.size() == 3 ? lower(trimmed(given[2])) : std::string("true");
		const bool includeHidden = hiddenWord != "false" && hiddenWord != "no" && hiddenWord != "exclude_hidden";
		if (column.empty() || column.size() > 3 || table.size() > 80 || table.find("[[") != std::string::npos)
			return unknown("operand_unknown");
		for (const char character : column)
			if (!std::isalpha(static_cast<unsigned char>(character))) return unknown("operand_unknown");
		for (char &character : column) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
		const auto total = relations::table_total(context_.raw_passages, table, column, includeHidden);
		if (!total) return unknown("operand_unknown");
		record_.kind = "table_total";
		operands_.clear();
		if (!permitted()) return withheld();
		const std::string extra = table + "|" + column + "|" + (includeHidden ? "hidden" : "shown");
		operands_.clear();
		const std::string marker = issue(total->total, derivation(extra), "table_total(" + table + ", " + column + ")");
		if (marker.empty()) return unknown("not_worked_out");
		std::string hidden;
		for (const std::string &cell : total->hidden_cells) hidden += (hidden.empty() ? "" : ", ") + cell;
		Line line;
		line.symbol(marker).say(" = the total of column " + column + " of " + table
			+ (hidden.empty() ? std::string(" (no hidden rows)")
				: includeHidden ? " (hidden rows included: " + hidden + ")" : " (hidden rows left out: " + hidden + ")"));
		return exact(line);
	}

	OperationAnswer expression() {
		std::string text = trimmed(request_->expression);
		if (text.empty() && request_->operands.size() == 1) text = trimmed(request_->operands.front());
		if (text.rfind("{{=", 0) == 0 && text.size() >= 5 && text.compare(text.size() - 2, 2, "}}") == 0)
			text = trimmed(text.substr(3, text.size() - 5));
		// "… as money": how a statement is written, never part of the value.
		{
			static const std::regex format("\\s+as\\s+[^\\[\\]()]+$");
			text = std::regex_replace(text, format, "");
		}
		if (text.empty() || text.find("{{") != std::string::npos) return unknown("operand_unknown");
		// What the expression asks: an order or an equality is a comparison;
		// otherwise its arithmetic - as the relation policy classifies it.
		const bool comparison = text.find_first_of("<>=") != std::string::npos || text.find("\xE2\x89") != std::string::npos
			|| text.find("if(") != std::string::npos || text.find("if (") != std::string::npos;
		expression_kind_ = comparison ? "compare" : text.find('-') != std::string::npos ? "difference"
			: text.find('+') != std::string::npos ? "sum" : text.find_first_of("*/") != std::string::npos ? "ratio" : "expression";
		// Figures of the model's own: only ones the sent text writes openly, and
		// never in a comparison - its answer would be a word about a protected
		// value against a figure the model chose.
		const std::vector<std::string> figures = numbers_in(text);
		if (!figures.empty() && comparison) return refuse("own_figure");
		for (const std::string &figure : figures)
			if (open_figures().count(figure) == 0) return refuse("own_figure");
		// Every symbol becomes its value's exact writing.
		std::string worked;
		std::string canonical;
		for (std::size_t index = 0; index < text.size();) {
			const std::size_t open = text.find("[[", index);
			if (open == std::string::npos) {
				worked += text.substr(index);
				canonical += text.substr(index);
				break;
			}
			const std::size_t close = text.find("]]", open);
			if (close == std::string::npos) return unknown("operand_unknown");
			worked += text.substr(index, open - index);
			canonical += text.substr(index, open - index);
			Operand operand = resolve(text.substr(open, close + 2 - open));
			if (operand.written_only) return refuse("own_figure");
			if (!operand.failure.empty()) return unknown(operand.failure);
			if (operand.dimensionless) return unknown("ratio_operand");
			const auto written = writing(*operand.value);
			if (!written) return unknown("no_exact_writing");
			worked += kOpen + *written + kClose;
			canonical += "{" + operand.key + "}";
			operands_.push_back(std::move(operand));
			index = close + 2;
		}
		if (!permitted()) return withheld();
		return worked_expression(worked, text, canonical);
	}

	OperationAnswer worked_expression(const std::string &worked, const std::string &shown, const std::string &canonical) {
		const std::string saved = op_;
		op_ = "expression";
		operands_.clear();
		// The derivation is the expression over the operands' facts.
		Operand whole;
		whole.key = canonical;
		operands_.push_back(whole);
		// The model knows the expression it asked for; it is not written back.
		// Its result is only written: an expression is the fallback for a figure
		// no operation gives, never a step to relate further.
		OperationAnswer answer = this->worked(worked, shown, "written_only", std::string("your expression"));
		op_ = saved;
		return answer;
	}
};

} // namespace

const char *to_string(Status status) {
	switch (status) {
		case Status::Exact: return "exact";
		case Status::Bounded: return "bounded";
		case Status::Unknown: return "unknown";
		case Status::Refused: return "refused";
	}
	return "unknown";
}

std::string local_writing(const RequestMappingEntry &row) {
	namespace marks = fact_anchor_marks;
	std::string writing = row.original;
	if (writing.empty() && row.value) {
		try {
			writing = semantic::display(*row.value);
		} catch (const semantic::AlgebraError &) {
		}
	}
	if (!row.fact_key.empty() && marks::is_key(row.fact_key))
		return std::string(marks::kOpen) + writing + std::string(marks::kKey) + row.fact_key + std::string(marks::kClose);
	return kOpen + writing + kClose;
}

std::vector<OperationAnswer> execute(const std::vector<OperationRequest> &requests, Context &context) {
	std::vector<OperationAnswer> answers;
	Executor executor(context);
	for (std::size_t index = 0; index < requests.size(); ++index) {
		// Past the questions one answer may ask, each is refused alike - and
		// recorded like any other.
		OperationAnswer answer = executor.run(requests[index], index + 1, index >= kMaximumQuestions);
		// The record waits for the call that carries its line (noteOutboundCall).
		if (context.table != nullptr) context.table->pending_relations.push_back(answer.record);
		answers.push_back(std::move(answer));
	}
	return answers;
}

std::string block(const std::vector<OperationAnswer> &answers, bool local) {
	std::string text;
	for (const OperationAnswer &answer : answers) text += (local ? answer.local_line : answer.line) + "\n";
	return text;
}

// ---------------------------------------------------------------------------
// The answer check.

namespace {

// The prompt's own standalone number tokens, digits only ("1,275.00" -> "127500"
// and "1275"), outside its symbols.
std::set<std::string> prompt_numbers(const std::string &prompt) {
	std::set<std::string> numbers;
	const std::string text = outside_symbols(prompt);
	for (std::size_t index = 0; index < text.size();) {
		if (!digit(text[index]) || (index > 0 && (std::isalnum(static_cast<unsigned char>(text[index - 1])) != 0 || text[index - 1] == '_'))) {
			++index;
			continue;
		}
		const Written number = number_at(text, index);
		numbers.insert(number.whole);
		numbers.insert(number.integer);
		index = number.end;
	}
	return numbers;
}

bool reference_before(const std::string &text, std::size_t at) {
	const std::size_t from = at > 24 ? at - 24 : 0;
	const std::string before = lower(text.substr(from, at - from));
	for (const char *cue : {"section", "clause", "article", "art.", "schedule", "exhibit", "annex", "appendix", "paragraph",
			 "no.", "\xC2\xA7", "\xE2\x84\x96", "\xD0\xBF.", "\xD1\x81\xD1\x82.", "\xD0\xBF\xD1\x83\xD0\xBD\xD0\xBA\xD1\x82",
			 "\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD1\x8C", "\xD1\x80\xD0\xB0\xD0\xB7\xD0\xB4\xD0\xB5\xD0\xBB", "row ", "rows ",
			 "cell", "page", "line", "item", "step", "chunk", "doc_"})
		if (before.find(cue) != std::string::npos && before.size() - before.rfind(cue) <= std::string(cue).size() + 3) return true;
	return false;
}

// Whether the words right after a figure make it money, a rate, a date or a period.
bool cue_after(const std::string &text, std::size_t at) {
	const std::string after = lower(text.substr(at, std::min<std::size_t>(24, text.size() - at)));
	std::size_t start = 0;
	while (start < after.size() && (after[start] == ' ' || static_cast<unsigned char>(after[start]) == 0xC2)) ++start;
	const std::string rest = after.substr(start);
	for (const char *cue : {"%", "percent", "per cent", "usd", "eur", "gbp", "rub", "dollar", "euro", "\xE2\x82\xBD",
			 "\xE2\x82\xAC", "\xD1\x80\xD1\x83\xD0\xB1", "\xD0\xBF\xD1\x80\xD0\xBE\xD1\x86\xD0\xB5\xD0\xBD\xD1\x82", "day", "week",
			 "month", "year", "business day", "calendar day", "\xD0\xB4\xD0\xB5\xD0\xBD", "\xD0\xB4\xD0\xBD", "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86",
			 "\xD0\xBB\xD0\xB5\xD1\x82", "\xD0\xB3\xD0\xBE\xD0\xB4", "million", "billion", "thousand", "\xD0\xBC\xD0\xBB\xD0\xBD",
			 "\xD1\x82\xD1\x8B\xD1\x81"})
		if (rest.rfind(cue, 0) == 0) return true;
	return false;
}

bool cue_before(const std::string &text, std::size_t at) {
	std::size_t index = at;
	while (index > 0 && text[index - 1] == ' ') --index;
	if (index == 0) return false;
	const char previous = text[index - 1];
	if (previous == '$') return true;
	for (const char *sign : {"\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5", "\xE2\x82\xBD"}) {
		const std::size_t length = std::string(sign).size();
		if (index >= length && text.compare(index - length, length, sign) == 0) return true;
	}
	const std::size_t from = index > 4 ? index - 4 : 0;
	const std::string before = text.substr(from, index - from);
	return before.find("USD") != std::string::npos || before.find("EUR") != std::string::npos || before.find("RUB") != std::string::npos;
}

bool month_near(const std::string &text, std::size_t at, std::size_t end) {
	const std::size_t from = at > 12 ? at - 12 : 0;
	const std::string window = lower(text.substr(from, std::min(text.size(), end + 12) - from));
	for (const char *month : {"january", "february", "march", "april", "may ", "june", "july", "august", "september", "october",
			 "november", "december", "jan.", "feb.", "mar.", "apr.", "aug.", "sep.", "sept.", "oct.", "nov.", "dec.",
			 "\xD1\x8F\xD0\xBD\xD0\xB2\xD0\xB0\xD1\x80", "\xD1\x84\xD0\xB5\xD0\xB2\xD1\x80\xD0\xB0\xD0\xBB", "\xD0\xBC\xD0\xB0\xD1\x80\xD1\x82",
			 "\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB5\xD0\xBB", "\xD0\xBC\xD0\xB0\xD1\x8F", "\xD0\xB8\xD1\x8E\xD0\xBD", "\xD0\xB8\xD1\x8E\xD0\xBB",
			 "\xD0\xB0\xD0\xB2\xD0\xB3\xD1\x83\xD1\x81\xD1\x82", "\xD1\x81\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8F\xD0\xB1\xD1\x80",
			 "\xD0\xBE\xD0\xBA\xD1\x82\xD1\x8F\xD0\xB1\xD1\x80", "\xD0\xBD\xD0\xBE\xD1\x8F\xD0\xB1\xD1\x80", "\xD0\xB4\xD0\xB5\xD0\xBA\xD0\xB0\xD0\xB1\xD1\x80"})
		if (window.find(month) != std::string::npos) return true;
	return false;
}

// The value a symbol stands for, read from the rows.
const RequestMappingEntry *row_named(const std::vector<RequestMappingEntry> &mapping, const std::string &marker) {
	for (const RequestMappingEntry &entry : mapping)
		if (entry.placeholder == marker) return &entry;
	return nullptr;
}

// a + b - c ... over Money in one currency, Numbers or Durations in one unit;
// nothing when the terms do not add up that simply.
std::optional<Value> added(const std::vector<std::pair<char, const Value *>> &terms) {
	if (terms.empty()) return std::nullopt;
	Value total = *terms.front().second;
	if (total.kind != VKind::Money && total.kind != VKind::Number && total.kind != VKind::Duration) return std::nullopt;
	if (total.kind == VKind::Money && total.basis != semantic::RateBasis::None) return std::nullopt;
	for (std::size_t index = 1; index < terms.size(); ++index) {
		const Value &term = *terms[index].second;
		if (term.kind != total.kind || term.basis != total.basis) return std::nullopt;
		if (term.kind == VKind::Money && !term.currency.empty() && !total.currency.empty() && term.currency != total.currency)
			return std::nullopt;
		if (term.kind == VKind::Duration && term.unit != total.unit) return std::nullopt;
		try {
			total.amount = terms[index].first == '+' ? total.amount + term.amount : total.amount - term.amount;
		} catch (const semantic::AlgebraError &) {
			return std::nullopt;
		}
	}
	return total;
}

bool same_amount(const Value &a, const Value &b) {
	if (a.kind != b.kind) return false;
	try {
		if (a.kind == VKind::Money) {
			const int places = semantic::minor_unit_places(a.currency.empty() ? b.currency : a.currency);
			return a.amount.rounded(places) == b.amount.rounded(places);
		}
		return a.amount == b.amount && (a.kind != VKind::Duration || a.unit == b.unit);
	} catch (const semantic::AlgebraError &) {
		return true;  // never flagged on what cannot be told
	}
}

} // namespace

FigureCheck check_figures(const std::string &provider_text, const std::string &sent_prompt,
		const std::vector<RequestMappingEntry> &mapping) {
	FigureCheck check;
	const std::set<std::string> known = prompt_numbers(sent_prompt);
	const std::string text = outside_symbols(provider_text);
	// Figures of the model's own.
	for (std::size_t index = 0; index < text.size();) {
		if (!digit(text[index]) || (index > 0 && (std::isalpha(static_cast<unsigned char>(text[index - 1])) != 0
				|| text[index - 1] == '_' || digit(text[index - 1])))) {
			++index;
			continue;
		}
		// A date written in figures ("01.08.2026", "08/01/2026", "2026-08-01") is
		// one figure, a date: it is traced like any other.
		static const std::regex numericDate("^(?:[0-9]{1,2}[./][0-9]{1,2}[./][0-9]{2,4}|[0-9]{4}-[0-9]{2}-[0-9]{2})(?![0-9])");
		std::smatch dated;
		const std::string ahead = text.substr(index, std::min<std::size_t>(12, text.size() - index));
		if (std::regex_search(ahead, dated, numericDate)) {
			const std::size_t start = index;
			index += static_cast<std::size_t>(dated.length(0));
			if (sent_prompt.find(dated.str(0)) == std::string::npos) check.untraced.push_back(start);
			continue;
		}
		const Written number = number_at(text, index);
		const std::size_t end = number.end;
		const std::string &whole = number.whole;
		const std::string &integer = number.integer;
		const std::size_t start = index;
		index = end;
		// A letter or an underscore right after it: part of a word or a handle.
		if (end < text.size() && (std::isalpha(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_')) continue;
		const bool cued = cue_before(text, start) || cue_after(text, end) || month_near(text, start, end)
			|| (end < text.size() && (text[end] == '/' || (text[end] == '.' && end + 1 < text.size() && digit(text[end + 1]))));
		const bool year = whole.size() == 4 && (whole.rfind("19", 0) == 0 || whole.rfind("20", 0) == 0) && !cue_before(text, start)
			&& !cue_after(text, end);
		if (!cued && (integer.size() <= 3 || year)) continue;
		if (reference_before(text, start)) continue;
		if (known.count(whole) != 0 || known.count(integer) != 0) continue;
		check.untraced.push_back(start);
	}
	// An amount written in words ("forty-three thousand dollars", «…
	// …") is a figure too.
	{
		const std::string folded = lower(text);
		for (const char *currency : {"dollar", "\xD1\x80\xD1\x83\xD0\xB1\xD0\xBB", "euro"}) {
			const std::string word(currency);
			for (std::size_t at = folded.find(word); at != std::string::npos; at = folded.find(word, at + word.size())) {
				// The words right before it, as long a run as reads as a number.
				std::size_t from = at;
				while (from > 0 && (std::isalpha(static_cast<unsigned char>(text[from - 1])) != 0 || text[from - 1] == ' '
						|| text[from - 1] == '-' || static_cast<unsigned char>(text[from - 1]) >= 0x80) && at - from < 120)
					--from;
				std::optional<std::size_t> start;
				for (std::size_t cut = from; cut < at; ++cut) {
					if (cut != from && text[cut - 1] != ' ') continue;
					const std::string words = trimmed(text.substr(cut, at - cut));
					if (words.empty()) break;
					const std::optional<semantic::Rational> number = semantic::parse_number_words(words);
					if (number && *number >= semantic::Rational(10)) {
						start = cut;
						if (sent_prompt.find(words) != std::string::npos) start.reset();
						break;
					}
				}
				if (start) check.untraced.push_back(*start);
			}
		}
	}
	// Arithmetic and comparisons the model states over symbols. What it states
	// is checked against the values only where Singularity already told it that
	// relation - an operation's answer or a relation line of the sent prompt -
	// since checking any other would tell it the relation. One nothing told it
	// is untraced whatever the values are: it takes it from an operation.
	static const std::regex symbol("\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\]");
	static const std::regex equation(
		"(\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\])\\s*=\\s*(\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\](?:\\s*[-+]\\s*\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\])+)");
	static const std::regex reversed(
		"(\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\](?:\\s*[-+]\\s*\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\])+)\\s*=\\s*(\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\])");
	static const std::regex ordered("(\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\])\\s*(>=|<=|>|<| is greater than | is less than | exceeds "
		"| is more than | is before | is after | precedes | is earlier than | is later than )\\s*(\\[\\[[A-Za-z]{1,12}[0-9]{1,5}\\]\\])");
	// The sent prompt's lines: each a place Singularity may have told a relation.
	std::vector<std::string> lines;
	for (std::size_t from = 0; from < sent_prompt.size();) {
		const std::size_t end = sent_prompt.find('\n', from);
		lines.push_back(sent_prompt.substr(from, end == std::string::npos ? std::string::npos : end - from));
		if (end == std::string::npos) break;
		from = end + 1;
	}
	const auto told = [&](const std::vector<std::string> &symbols, std::initializer_list<const char *> words) {
		for (const std::string &line : lines) {
			if (!std::all_of(symbols.begin(), symbols.end(), [&](const std::string &marker) { return line.find(marker) != std::string::npos; }))
				continue;
			for (const char *word : words)
				if (line.find(word) != std::string::npos) return true;
		}
		return false;
	};
	const auto symbols_of = [&](const std::string &chain) {
		std::vector<std::string> found;
		for (std::sregex_iterator it(chain.begin(), chain.end(), symbol), done; it != done; ++it) found.push_back(it->str());
		return found;
	};
	const auto terms_of = [&](const std::string &chain) -> std::optional<std::vector<std::pair<char, const Value *>>> {
		std::vector<std::pair<char, const Value *>> terms;
		char sign = '+';
		std::size_t cursor = 0;
		for (std::sregex_iterator it(chain.begin(), chain.end(), symbol), done; it != done; ++it) {
			const std::string between = chain.substr(cursor, static_cast<std::size_t>(it->position()) - cursor);
			if (between.find('-') != std::string::npos || between.find("\xE2\x88\x92") != std::string::npos) sign = '-';
			else if (between.find('+') != std::string::npos) sign = '+';
			const RequestMappingEntry *row = row_named(mapping, it->str());
			if (row == nullptr || !row->value) return std::nullopt;
			terms.emplace_back(sign, &*row->value);
			cursor = static_cast<std::size_t>(it->position() + it->length());
		}
		return terms;
	};
	// The derivation an operation over these symbols was keyed by (Executor::
	// derivation): "difference(a,b)", "sum(a,b,...)".
	const auto derivation_of = [&](const std::string &chain) -> std::string {
		std::vector<std::string> keys;
		std::string signs;
		std::size_t cursor = 0;
		for (std::sregex_iterator it(chain.begin(), chain.end(), symbol), done; it != done; ++it) {
			const std::string between = chain.substr(cursor, static_cast<std::size_t>(it->position()) - cursor);
			if (!keys.empty()) signs += between.find('-') != std::string::npos ? '-' : '+';
			const RequestMappingEntry *row = row_named(mapping, it->str());
			if (row == nullptr) return {};
			keys.push_back(!row->fact_key.empty() ? row->fact_key : "sym:" + row->placeholder);
			cursor = static_cast<std::size_t>(it->position() + it->length());
		}
		if (keys.size() < 2) return {};
		std::string joined;
		for (const std::string &key : keys) joined += (joined.empty() ? "" : ",") + key;
		if (signs == "-") return "difference(" + joined + ")";
		if (signs.find('-') == std::string::npos) return "sum(" + joined + ")";
		return {};
	};
	const auto checkSum = [&](const std::string &result, const std::string &chain, std::size_t at) {
		// The operation Singularity worked out for exactly these facts has its own
		// symbol: stating another - even of an equal value, another fact - is a
		// wrong answer. This reads derivations only, never values.
		const std::string derivation = derivation_of(chain);
		if (!derivation.empty())
			for (const RequestMappingEntry &entry : mapping)
				if (entry.derived && (entry.provenance.derivation == derivation
						|| entry.provenance.derivation.rfind(derivation + "|", 0) == 0)) {
					if (entry.placeholder != result) check.wrong.push_back(at);
					return;
				}
		std::vector<std::string> involved = symbols_of(chain);
		involved.push_back(result);
		if (!told(involved, {"=", " sum ", "total", "difference", "equals"})) {
			check.untraced.push_back(at);
			return;
		}
		const RequestMappingEntry *row = row_named(mapping, result);
		const auto terms = terms_of(chain);
		if (row == nullptr || !row->value || !terms) return;
		const std::optional<Value> total = added(*terms);
		if (total && !same_amount(*total, *row->value)) check.wrong.push_back(at);
	};
	for (std::sregex_iterator it(provider_text.begin(), provider_text.end(), equation), done; it != done; ++it)
		checkSum((*it)[1].str(), (*it)[2].str(), static_cast<std::size_t>(it->position()));
	for (std::sregex_iterator it(provider_text.begin(), provider_text.end(), reversed), done; it != done; ++it)
		checkSum((*it)[2].str(), (*it)[1].str(), static_cast<std::size_t>(it->position()));
	for (std::sregex_iterator it(provider_text.begin(), provider_text.end(), ordered), done; it != done; ++it) {
		const std::string a = (*it)[1].str();
		const std::string b = (*it)[3].str();
		const std::size_t at = static_cast<std::size_t>(it->position());
		if (!told({a, b}, {"greater than", "less than", "equal", "GREATER", "LESS", "EQUAL", " before ", " after ", "same day",
				" > ", " < ", "exceeds", "more than", "earlier", "later"})) {
			check.untraced.push_back(at);
			continue;
		}
		const RequestMappingEntry *left = row_named(mapping, a);
		const RequestMappingEntry *right = row_named(mapping, b);
		if (left == nullptr || right == nullptr || !left->value || !right->value) continue;
		const std::string relation = trimmed((*it)[2].str());
		try {
			if (is_date(*left->value) || is_date(*right->value)) {
				const bool later = relation == ">" || relation == ">=" || relation == "is after" || relation == "is later than"
					|| relation == "is greater than" || relation == "exceeds" || relation == "is more than";
				const semantic::Relation holds = later ? semantic::after(*left->value, *right->value) : semantic::before(*left->value, *right->value);
				if (holds.truth == semantic::Truth::False && relation != ">=" && relation != "<=") check.wrong.push_back(at);
				continue;
			}
			const semantic::Comparator comparator = relation == ">=" ? semantic::Comparator::GreaterOrEqual
				: relation == "<=" ? semantic::Comparator::LessOrEqual
				: relation == "<" || relation == "is less than" ? semantic::Comparator::Less : semantic::Comparator::Greater;
			if (semantic::compare(*left->value, comparator, *right->value).truth == semantic::Truth::False) check.wrong.push_back(at);
		} catch (const semantic::AlgebraError &) {
		}
	}
	return check;
}

} // namespace singularity::protected_api::papinian::operations

// ---------------------------------------------------------------------------
// The protected computation protocol's executor (DESIGN4 §3.1,
// ComputeExecutor.hpp): each op resolved, typed, admitted, charged, evaluated
// and its result issued as a derived fact - or answered with its closed code.
// The order is the privacy argument: everything before evaluation reads only
// the request, the declared types, the sent text and the ledger's counts, so a
// refusal is the same in every world; an admitted observation is paid for
// before its outcome exists.

#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"
#include "PapinianGuard/RestorationGrammar/SpelledFigures.hpp"

namespace singularity::protected_api::papinian::operations {

// The executor's own names, apart from the cycle-3 executor's above.
namespace compute_executor {

using compute::Code;
using compute::OpAnswer;
using compute::OpStatus;
using compute::Operand;
using compute::TypeKind;

OpAnswer failure(OpStatus status, Code code, std::size_t argument = 0, std::string expected = {}, std::string hint = {}) {
	OpAnswer answer;
	answer.status = status;
	answer.code = code;
	answer.error.code = code;
	answer.error.argument = argument;
	answer.error.expected = std::move(expected);
	answer.error.hint = std::move(hint);
	return answer;
}

// The admission rules' codes are refusals; everything else a request gets
// wrong is an error.
OpStatus status_of_refusal(Code code) {
	switch (code) {
		case Code::UnsafeLiteral:
		case Code::ConstantNotInMaterial:
		case Code::ConstantOnly:
		case Code::ConstantDepth:
		case Code::TooManyConstants:
		case Code::SameOperand:
		case Code::RoundedObservation:
		case Code::ExternalOperand:
		case Code::DepthExceeded:
		case Code::DisclosureBudget:
		case Code::WithheldByPolicy:
		case Code::QueryLiteral: return OpStatus::Refused;
		default: return OpStatus::Error;
	}
}

std::string strip_brackets(const std::string &symbol) {
	if (symbol.size() > 4 && symbol.compare(0, 2, "[[") == 0 && symbol.compare(symbol.size() - 2, 2, "]]") == 0)
		return symbol.substr(2, symbol.size() - 4);
	return symbol;
}

// A symbol as the operand it is: a value this operation worked out, or a
// registry row the model was sent. Anything else is `unknown_symbol` (R1).
bool resolve_symbol(const std::string &symbol, Operand &operand, compute::OpContext &context, bool fromLabel) {
	OperationProtectedSymbolTable &table = *context.table;
	const auto derived = table.derived_facts.find(symbol);
	if (derived != table.derived_facts.end()) {
		const compute::DerivedFact &fact = derived->second;
		operand.derived = &fact;
		if (fact.certainty.kind == compute::Certainty::Kind::Exact) operand.value = fact.value;
		const std::optional<compute::ArgType> declared = tables::parse_type(fact.type);
		operand.type = declared ? *declared : type_of_value(fact.value);
		operand.fact_key = fact.fact_key;
		operand.roots = fact.roots;
		operand.depth = fact.depth;
		return true;
	}
	if (!fromLabel && table.sent_symbols.count(symbol) == 0) return false;
	const RequestMappingEntry *row = context.row_of ? context.row_of(symbol) : nullptr;
	if (row == nullptr) return false;
	operand.row = row;
	operand.type = arg_type_of(*row);
	if (!operand.arg.part.empty()) {
		operand.type.kind = TypeKind::Entity;
	} else {
		operand.value = row->value;
	}
	operand.fact_key = row->fact_key.empty() ? "sym:" + strip_brackets(symbol) : row->fact_key;
	operand.roots = {operand.fact_key};
	return true;
}

std::optional<Operand> resolve(const std::string &written, std::size_t index, compute::OpContext &context, OpAnswer &failed) {
	compute::TypeError error;
	const std::optional<compute::Arg> arg = compute::parse_arg(written, error);
	if (!arg) {
		failed = failure(OpStatus::Error, error.code, index + 1, error.expected, error.hint);
		return std::nullopt;
	}
	Operand operand;
	operand.arg = *arg;
	compute::TypeError tableError;
	tableError.code = Code::None;
	if (tables::resolve_arg(*context.table, *arg, operand, tableError)) {
		if (tableError.code != Code::None) {
			failed = failure(OpStatus::Error, tableError.code, index + 1, tableError.expected, tableError.hint);
			return std::nullopt;
		}
		return operand;
	}
	switch (arg->kind) {
		case compute::ArgKind::Symbol:
			if (!resolve_symbol(arg->symbol, operand, context, false)) {
				failed = failure(OpStatus::Error, Code::UnknownSymbol, index + 1);
				return std::nullopt;
			}
			return operand;
		case compute::ArgKind::Ref: {
			const auto named = context.table->compute_labels.find(arg->label);
			// A bound's name - at least one figure, at most another - is no value to
			// work with further; "no such name" sent a model hunting for a typo (a
			// fake-provider run on R2).
			if (named == context.table->compute_labels.end() && context.table->bounded_labels.count(arg->label) != 0) {
				failed = failure(OpStatus::Error, Code::BoundedOperand, index + 1, {}, "that name is a bound - at least "
					"one figure, at most another - not one figure: state the bound as it is, or narrow the rows first");
				return std::nullopt;
			}
			if (named == context.table->compute_labels.end() || !resolve_symbol(named->second, operand, context, true)) {
				failed = failure(OpStatus::Error, Code::UnknownLabel, index + 1);
				return std::nullopt;
			}
			return operand;
		}
		case compute::ArgKind::Calendar: {
			operand.type.kind = TypeKind::Calendar;
			const std::string handle = strip_brackets(arg->symbol);
			for (const compute::CalendarHandle &calendar : context.table->calendars)
				if (calendar.handle == handle) operand.calendar = &calendar;
			return operand;
		}
		case compute::ArgKind::Constant: {
			// A figure, or a word the op names (derive_column's "subtract"): a
			// word is no value, and only an op whose typer takes one, from its
			// own closed vocabulary, accepts it.
			std::optional<semantic::Value> value = semantic::parse_value(arg->constant);
			// A period counted back in the material's own words ("-twenty-four (24)
			// months", live EN1 26 Sep 2026): the words' period, signed.
			if (!value) {
				std::string rest = arg->constant;
				while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
				bool negative = false;
				bool positive = false;
				if (rest.rfind("-", 0) == 0) { negative = true; rest.erase(0, 1); }
				else if (rest.rfind("\xE2\x88\x92", 0) == 0) { negative = true; rest.erase(0, 3); }
				else if (rest.rfind("+", 0) == 0) { positive = true; rest.erase(0, 1); }
				if (positive) {
					std::optional<semantic::Value> plain = semantic::parse_value(rest);
					if (plain && (plain->kind == semantic::Kind::Duration || plain->kind == semantic::Kind::Number
							|| plain->kind == semantic::Kind::Count))
						value = plain;
				}
				if (negative) {
					std::optional<semantic::Value> unsigned_value = semantic::parse_value(rest);
					if (unsigned_value && (unsigned_value->kind == semantic::Kind::Duration
							|| unsigned_value->kind == semantic::Kind::Number || unsigned_value->kind == semantic::Kind::Count)) {
						unsigned_value->amount = semantic::Rational(0) - unsigned_value->amount;
						value = unsigned_value;
					}
				}
			}
			if (!value) {
				operand.type.kind = TypeKind::Word;
				return operand;
			}
			operand.value = value;
			operand.type = type_of_value(*value);
			return operand;
		}
		default:
			failed = failure(OpStatus::Error, Code::UnknownSymbol, index + 1);
			return std::nullopt;
	}
}

// Whether what was told already settles this relation of two facts (owner,
// 27 Sep 2026: an implied relation is free): the ledger keeps the order every
// answered relation, planner line and timeline line told, for the matter's
// life - a = b and b > c make a > c known, so asking it discloses nothing new.
bool implied(const compute::OpNode &node, const std::vector<Operand> &operands, const compute::OpContext &context) {
	static const std::set<std::string> relations{"compare", "subtract", "greater_than", "less_than", "greater_or_equal",
		"less_or_equal", "equal", "not_equal", "before", "after", "same_time"};
	if (relations.count(node.op) == 0 || operands.size() != 2 || context.ledger == nullptr) return false;
	return context.ledger->implied(operands[0].fact_key, operands[1].fact_key, node.op);
}

// "0" is a zero of the quantity it is compared with, added to or bounded by:
// a zero amount in that currency, a zero rate, no days. A plain figure set
// against a proportion is that proportion ("0.5" of a ratio is 50 %).
void type_zeros(const compute::OpNode &node, std::vector<Operand> &operands) {
	static const std::set<std::string> ops{"greater_than", "less_than", "greater_or_equal", "less_or_equal", "equal",
		"not_equal", "compare", "add", "subtract", "min", "max", "between", "sum", "average"};
	if (ops.count(node.op) == 0) return;
	const Operand *model = nullptr;
	for (const Operand &operand : operands) {
		const TypeKind kind = operand.type.kind;
		if (operand.arg.kind != compute::ArgKind::Constant
				&& (kind == TypeKind::Money || kind == TypeKind::Percentage || kind == TypeKind::Rate
					|| kind == TypeKind::Duration || kind == TypeKind::Count)) {
			model = &operand;
			break;
		}
	}
	if (model == nullptr) return;
	const compute::ArgType type = model->type;
	for (Operand &operand : operands) {
		if (operand.arg.kind != compute::ArgKind::Constant || !operand.value || operand.value->kind != semantic::Kind::Number)
			continue;
		const bool proportion = type.kind == TypeKind::Percentage || type.kind == TypeKind::Rate;
		// A bare number beside days or a count is in their unit - "45" added to
		// [[Duration1]] is 45 days (live RU1, 27 Sep 2026: refused as NUMBER);
		// beside an amount only zero is, a bare number never meets money (R6).
		const bool measured = type.kind == TypeKind::Duration || type.kind == TypeKind::Count;
		if (!operand.value->amount.is_zero() && !proportion && !measured) continue;
		semantic::Value zero = *operand.value;
		switch (type.kind) {
			case TypeKind::Money:
				zero.kind = semantic::Kind::Money;
				zero.currency = type.currency == "?" ? std::string() : type.currency;
				zero.basis = type.basis;
				break;
			case TypeKind::Percentage:
			case TypeKind::Rate:
				zero.kind = semantic::Kind::Percentage;
				zero.basis = type.basis;
				break;
			case TypeKind::Duration:
				zero.kind = semantic::Kind::Duration;
				zero.unit = type.unit;
				break;
			default: zero.kind = semantic::Kind::Count; break;
		}
		operand.value = zero;
		operand.type = type;
	}
}

// Whether the model's declared type is the one the operation returns - read
// by kind: "MONEY" accepts "MONEY(USD)".
bool expected_fits(const std::string &expected, const compute::ArgType &type) {
	const auto head = [](std::string text) {
		text = text.substr(0, text.find('('));
		for (char &character : text) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
		while (!text.empty() && text.back() == ' ') text.pop_back();
		return text;
	};
	const std::string wanted = head(expected);
	if (wanted.empty()) return true;
	const std::string got = head(compute::written(type));
	return wanted == got || (wanted == "RATE" && got == "PERCENTAGE") || (wanted == "PERCENTAGE" && got == "RATE")
		|| (wanted == "NUMBER" && got == "COUNT");
}

// The op over its operands' keys (fact keys, constants' value keys, tables),
// its filter and its options: what a derived fact is named by.
std::string ast_of(const compute::OpNode &node, const std::vector<Operand> &operands) {
	std::string text = node.op + "(";
	for (std::size_t index = 0; index < operands.size(); ++index) text += (index == 0 ? "" : ",") + operand_key(operands[index]);
	if (node.where && !node.where->empty()) {
		text += ";where:";
		for (std::size_t index = 0; index < node.where->size(); ++index) {
			const compute::WhereAtom &atom = (*node.where)[index];
			text += (index == 0 ? "" : "&") + atom.column + " " + atom.test;
			if (atom.value) text += " " + *atom.value;
			if (atom.value2) text += " " + *atom.value2;
		}
	}
	if (node.options) {
		const compute::OpOptions &options = *node.options;
		text += ";" + options.unit.value_or("") + "," + options.calendar.value_or("") + ","
			+ (options.places ? std::to_string(*options.places) : std::string()) + "," + options.rounding.value_or("") + ","
			+ options.fn.value_or("") + "," + options.mode.value_or("") + ","
			// Whether a sign is shown is how the result is said, not what it is.
			+ "," + options.anchor.value_or("");
	}
	return text + ")";
}

struct Issuing {
	compute::DerivedFact base;
	compute::OpContext *context = nullptr;

	std::string issue(const std::string &suffix, const semantic::Value &value, const std::string &type,
			compute::Certainty certainty = {}) const {
		compute::DerivedFact fact = base;
		fact.ast += suffix;
		fact.value = value;
		// An amount Singularity worked out is written to its currency's minor
		// unit: "$223,000.00" (DESIGN4 §1.9).
		if (fact.value.kind == semantic::Kind::Money)
			fact.value.source_decimals = std::max(fact.value.source_decimals, semantic::minor_unit_places(fact.value.currency));
		fact.stated_openly = fact.value.kind != semantic::Kind::Money && fact.value.kind != semantic::Kind::Date
			&& fact.value.kind != semantic::Kind::DayOfYear && material_figure(fact.value, *context).has_value();
		fact.type = type;
		fact.certainty = certainty;
		return context->issue(fact);
	}
};

// The labels a row set restores to: its rows' own labels, each ended by the
// restoration grammar's separator.
std::string row_set_writing(const compute::OpContext &context, const tables::RowSet &rows) {
	const tables::ProtectedTable *table = tables::find_table(*context.table, rows.table);
	if (table == nullptr) return std::string();
	std::string written;
	for (const auto &[number, label] : tables::row_labels(*table, rows.certain, rows.certain.size())) {
		static_cast<void>(number);
		if (!label.empty()) written += label + restoration_grammar::kRowLabelSeparator;
	}
	return written;
}

std::string group_set_writing(const compute::OpContext &context, const tables::GroupSet &groups) {
	std::string written;
	for (const std::string &key : groups.key_words) {
		std::string words = key;
		if (key.rfind("[[", 0) == 0 && context.row_of)
			if (const RequestMappingEntry *row = context.row_of(key)) words = row->original;
		if (!words.empty()) written += words + restoration_grammar::kRowLabelSeparator;
	}
	return written;
}

void write_original(compute::OpContext &context, const std::string &symbol, const std::string &writing) {
	if (writing.empty()) return;
	for (RequestMappingEntry &row : context.table->mapping)
		if (row.placeholder == symbol) {
			row.original = writing;
			return;
		}
}

// A word as a table's column, the way a model naturally writes one against the
// table its op is about: its letter ("J"), or the header the tables block shows
// for exactly one described column ("Royalty due"). Nothing when it is neither.
std::string column_letter(const tables::ProtectedTable &table, const std::string &written) {
	const auto folded = [](std::string text) {
		std::string out;
		bool space = false;
		for (const char character : text) {
			if (character == ' ' || character == '\t' || character == '_') {
				space = !out.empty();
				continue;
			}
			if (space) out.push_back(' ');
			space = false;
			out.push_back(character);
		}
		// Case in every script (utf8proc): "…" is "…".
		return singularity::platform::folded_text(out);
	};
	const auto described = [&](const std::string &letter) {
		return std::find(table.described_columns.begin(), table.described_columns.end(), letter) != table.described_columns.end();
	};
	std::string word = written;
	while (!word.empty() && word.front() == ' ') word.erase(word.begin());
	while (!word.empty() && word.back() == ' ') word.pop_back();
	if (word.size() >= 1 && word.size() <= 3
			&& std::all_of(word.begin(), word.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
		return tables::column_of(table, word) != nullptr && described(word) ? word : std::string();
	const std::string wanted = folded(word);
	if (wanted.empty()) return {};
	std::string found;
	for (const tables::TableColumn &column : table.columns) {
		if (!described(column.letter) || folded(column.header) != wanted) continue;
		if (!found.empty()) return {};
		found = column.letter;
	}
	return found;
}

// Whether a bare word is the name an earlier op of this task bound.
bool bound_name(const compute::OpContext &context, const std::string &word) {
	if (context.table == nullptr || !compute::valid_label(word)) return false;
	if (context.table->compute_labels.count(word) != 0) return true;
	for (const tables::ProtectedTable &table : context.table->tables)
		if (table.row_sets.count("@" + word) != 0 || table.group_sets.count("@" + word) != 0
				|| tables::derived_column(table, "@" + word) != nullptr)
			return true;
	return false;
}

compute::OpAnswer execute_one(const compute::OpNode &given, compute::OpContext &context, compute::OpRecord &record) {
	// The op as it is worked out: the model's own, with a bare earlier name
	// read as that name and a column written by its header read as its letter.
	compute::OpNode node = given;
	// A filter's test values written again as arguments ("filter([[Table2]],
	// [[Date65]], 24 months) where within [[Date65]] 24 months", live EN1 26 Sep
	// 2026): the same values, said twice - read once, in the test.
	if (node.op == "filter" && node.args.size() > 1 && node.where && !node.where->empty()) {
		const auto trimmed = [](std::string text) {
			while (!text.empty() && text.front() == ' ') text.erase(text.begin());
			while (!text.empty() && text.back() == ' ') text.pop_back();
			return text;
		};
		bool repeated = true;
		for (std::size_t index = 1; index < node.args.size() && repeated; ++index) {
			const std::string written = trimmed(node.args[index]);
			bool found = false;
			for (const compute::WhereAtom &atom : *node.where)
				found = found || (atom.value && trimmed(*atom.value) == written) || (atom.value2 && trimmed(*atom.value2) == written)
					|| trimmed(atom.column) == written;
			repeated = found;
		}
		if (repeated) node.args.resize(1);
	}
	for (std::string &argument : node.args)
		if (!argument.empty() && argument.front() != '@' && argument.find("[[") == std::string::npos && bound_name(context, argument))
			argument = "@" + argument;
	struct NodeScope {
		compute::OpContext &context;
		const compute::OpNode *previous;
		~NodeScope() { context.node = previous; }
	} scope{context, context.node};
	context.node = &node;
	compute::OpOptions options = node.options.value_or(compute::OpOptions{});
	const compute::OpEntry *entry = compute::find_op(node.op);
	if (entry == nullptr || entry->typer == nullptr || entry->evaluator == nullptr) return failure(OpStatus::Error, Code::UnknownOp);
	// Only a filter reads conditions: on any other op they would be ignored
	// and the result read as if they held (live EN2, 27 Sep 2026: a column
	// summed "where B is General rep" summed every claim). Said, never guessed.
	if (node.where && !node.where->empty() && node.op != "filter")
		return failure(OpStatus::Error, Code::BadPredicate, 0, {},
			"only filter takes where: filter(" + (node.args.empty() ? std::string("table") : node.args.front())
				+ "; where=[...]) with a bind, then " + node.op + "(@that_name, ...)");

	// ---- operands (R1)
	std::vector<Operand> operands;
	for (std::size_t index = 0; index < node.args.size(); ++index) {
		OpAnswer failed;
		std::optional<Operand> operand = resolve(node.args[index], index, context, failed);
		if (!operand) return failed;
		operands.push_back(std::move(*operand));
	}
	const tables::ProtectedTable *about = nullptr;
	for (const Operand &operand : operands)
		if (operand.table != nullptr) {
			about = operand.table;
			break;
		}
	if (about != nullptr) {
		for (Operand &operand : operands) {
			if (operand.arg.kind != compute::ArgKind::Constant || operand.value || operand.type.kind != TypeKind::Word) continue;
			const std::string letter = column_letter(*about, operand.arg.constant);
			if (letter.empty()) continue;
			compute::Arg column = operand.arg;
			column.kind = compute::ArgKind::Column;
			column.symbol = about->handle;
			column.column = letter;
			Operand resolved;
			compute::TypeError columnError;
			columnError.code = Code::None;
			if (tables::resolve_arg(*context.table, column, resolved, columnError) && columnError.code == Code::None)
				operand = resolved;
		}
		if (node.where)
			for (compute::WhereAtom &atom : *node.where) {
				if (atom.column.empty() || atom.column.front() == '@') continue;
				const std::string letter = column_letter(*about, atom.column);
				if (!letter.empty()) atom.column = letter;
			}
	}
	type_zeros(node, operands);
	// A rate written as the material writes it is the material's rate: for the
	// period its own clause gives it ("0.05% ... per day").
	for (Operand &operand : operands) {
		if (operand.arg.kind != compute::ArgKind::Constant || !operand.value
				|| operand.value->kind != semantic::Kind::Percentage || operand.value->basis != semantic::RateBasis::None)
			continue;
		const std::optional<semantic::Value> stated = material_figure(*operand.value, context);
		if (!stated || stated->kind != semantic::Kind::Percentage || stated->basis == semantic::RateBasis::None) continue;
		operand.value->basis = stated->basis;
		operand.type = type_of_value(*operand.value);
	}
	for (const Operand &operand : operands) {
		record.operand_fact_keys.push_back(operand.fact_key);
		record.operand_types.push_back(compute::written(operand.type));
		// The symbol it is, a name as the symbol it stands for: what the
		// disclosure traces the operation's sources through.
		if (operand.derived != nullptr) record.operand_symbols.push_back(operand.derived->placeholder);
		else if (operand.row != nullptr) record.operand_symbols.push_back(operand.row->placeholder);
		else if (operand.arg.kind == compute::ArgKind::Symbol) record.operand_symbols.push_back(operand.arg.symbol);
	}

	// ---- types (class S)
	std::vector<compute::ArgType> types;
	for (const Operand &operand : operands) types.push_back(operand.type);
	compute::TypeError typeError;
	const std::optional<compute::ResultType> typed = entry->typer(types, options, typeError);
	if (!typed) {
		OpAnswer answer = failure(OpStatus::Error, typeError.code, typeError.argument, typeError.expected, typeError.hint);
		answer.error = typeError;
		return answer;
	}
	// A type the model expected otherwise is said beside the result, never a
	// reason to drop it (live EN1, 27 Sep 2026: aggregate expected as TABLE,
	// refused three times, the breakdown lost): the same words, no lost round.
	std::string expectNote;
	if (node.expect && !typed->relation && !expected_fits(*node.expect, typed->type))
		expectNote = "the result is " + compute::written(typed->type) + ", not " + *node.expect;

	// An amount or a rate whose period is not stated is read for the period the
	// other operands of its kind state (the typing joined them so).
	for (const semantic::Kind kind : {semantic::Kind::Money, semantic::Kind::Percentage}) {
		std::set<semantic::RateBasis> stated;
		for (const Operand &operand : operands)
			if (operand.value && operand.value->kind == kind && operand.value->basis != semantic::RateBasis::None)
				stated.insert(operand.value->basis);
		if (stated.size() != 1) continue;
		for (Operand &operand : operands)
			if (operand.value && operand.value->kind == kind && operand.value->basis == semantic::RateBasis::None)
				operand.value->basis = *stated.begin();
	}

	// ---- admission (R2-R11) and the observation's charge, before evaluation
	Admission verdict = admit(node, operands, context);
	if (verdict.code != Code::None) return failure(status_of_refusal(verdict.code), verdict.code, verdict.argument);
	// A relation what was told already implies is answered, not charged.
	if (verdict.charge && context.ledger != nullptr && !verdict.charge->key.empty() && implied(node, operands, context))
		context.ledger->matter.answered.insert(verdict.charge->key);
	// A difference is always given; its sign only while the facts' relations
	// allow (owner, 27 Sep 2026: creating an opaque derived symbol is free -
	// live E1 was refused "billed - paid - offsets" for a sign it never asked).
	if (verdict.charge && context.ledger != nullptr && node.op == "subtract" && (!options.reveal || *options.reveal)
			&& context.ledger->admit(*verdict.charge) != Code::None) {
		options.reveal = false;
		node.options = options;
		verdict = admit(node, operands, context);
		if (verdict.code != Code::None) return failure(status_of_refusal(verdict.code), verdict.code, verdict.argument);
	}
	if (verdict.charge) {
		if (context.ledger == nullptr) return failure(OpStatus::Refused, Code::DisclosureBudget);
		const Code budget = context.ledger->admit(*verdict.charge);
		if (budget != Code::None) {
			// Why, in fact keys and counts, for the task's diagnostics.
			++context.ledger->diagnostics.on_demand_refused;
			context.ledger->diagnostics.refusals.push_back(node.id + " " + context.ledger->refusal(*verdict.charge));
			return failure(OpStatus::Refused, budget);
		}
		const bool free = context.ledger->answered(verdict.charge->key);
		++(free ? context.ledger->diagnostics.on_demand_free : context.ledger->diagnostics.on_demand_charged);
		context.ledger->charge(*verdict.charge);
		record.observation = true;
		record.observation_key = verdict.charge->key;
		if (!free) {
			const std::set<std::string> roots(verdict.charge->roots.begin(), verdict.charge->roots.end());
			record.charged_roots.assign(roots.begin(), roots.end());
			record.charged_group_keys = verdict.charge->group_keys;
		}
	}

	// ---- evaluation, over the real values, on this computer
	OpAnswer answer;
	try {
		answer = entry->evaluator(operands, options, context);
	} catch (const semantic::AlgebraError &) {
		answer = failure(OpStatus::Error, Code::NotEvaluable);
	} catch (const std::exception &) {
		answer = failure(OpStatus::Error, Code::EvaluationFailed);
	}
	if (answer.result.type.kind == TypeKind::Number && !answer.result.relation && typed->type.kind != TypeKind::Number)
		answer.result = *typed;
	for (const Operand &operand : operands) {
		// A row set whose rows were already disclosed (list_rows answered for it)
		// adds none of its own roots to what is worked out over it: what decided
		// its rows is known, and an implied relation is free (owner, 27 Sep 2026).
		// The listing itself is that disclosure, and keeps them.
		if (node.op != "list_rows" && operand.rows && context.ledger != nullptr && !operand.rows->canonical.empty()
				&& context.ledger->answered("list_rows(" + operand.rows->canonical + ")"))
			continue;
		answer.roots.insert(operand.roots.begin(), operand.roots.end());
	}
	if (answer.status != OpStatus::Ok && answer.status != OpStatus::Bounded) return answer;

	// ---- the result as a derived fact
	Issuing issuing;
	issuing.context = &context;
	compute::DerivedFact &base = issuing.base;
	base.op_id = "c" + std::to_string(context.call) + ".r" + std::to_string(context.round) + "." + node.id;
	base.op = node.op;
	base.ast = ast_of(node, operands);
	base.operand_symbols = record.operand_symbols;
	for (const Operand &operand : operands)
		if (!operand.fact_key.empty()) base.operand_fact_keys.push_back(operand.fact_key);
	// The constants of the whole tree travel with the value (R7 counts them).
	base.constants = verdict.constants;
	for (const Operand &operand : operands)
		if (operand.derived != nullptr)
			for (const compute::ConstantUse &use : operand.derived->constants)
				if (std::none_of(base.constants.begin(), base.constants.end(),
						[&](const compute::ConstantUse &known) { return known.value_key == use.value_key; }))
					base.constants.push_back(use);
	base.label = node.bind.value_or(std::string());
	base.roots = answer.roots;
	base.root_sources = answer.root_sources;

	base.task = context.task;
	base.subproblem = node.why;
	std::set<std::string> constants;
	for (const compute::ConstantUse &use : verdict.constants) constants.insert(use.value_key);
	for (const Operand &operand : operands) {
		base.depth = std::max(base.depth, operand.depth + 1);
		if (operand.derived != nullptr) {
			for (const compute::ConstantUse &use : operand.derived->constants) constants.insert(use.value_key);
			base.scale_breaking = base.scale_breaking || operand.derived->scale_breaking;
			base.root_sources.insert(base.root_sources.end(), operand.derived->root_sources.begin(),
				operand.derived->root_sources.end());
		}
	}
	// Where every root is stated, one source each: a registry fact's place, or
	// - for a workbook cell no prompt carried - the cell it stands in.
	{
		std::vector<RelationSource> located;
		std::set<std::string> unlocated;
		for (const std::string &root : base.roots) {
			const auto row = context.table->fact_by_key.find(root);
			if (row == context.table->fact_by_key.end() || row->second >= context.table->mapping.size()) {
				unlocated.insert(root);
				continue;
			}
			const RequestMappingEntry &entry = context.table->mapping[row->second];
			RelationSource source;
			source.marker = entry.placeholder;
			source.origin = context.origin ? context.origin(entry) : std::string();
			source.passage_sha256 = entry.provenance.passage_sha256;
			source.offset = entry.provenance.offset;
			source.length = entry.provenance.length;
			source.cell = entry.provenance.cell;
			located.push_back(source);
		}
		for (const tables::ProtectedTable &table : context.table->tables) {
			if (unlocated.empty()) break;
			for (const tables::TableRow &row : table.rows)
				for (const tables::TableCell &cell : row.cells) {
					if (!cell.value && cell.text.empty()) continue;
					const auto found = unlocated.find(tables::cell_root(table, cell));
					if (found == unlocated.end()) continue;
					located.push_back(cell.source);
					unlocated.erase(found);
				}
		}
		// The evaluator's own sources first (a table total's counted cells), then
		// one per root not already among them.
		std::set<std::pair<std::string, std::size_t>> seen;
		for (const RelationSource &source : base.root_sources)
			seen.insert({source.passage_sha256 + "|" + source.cell, source.offset});
		for (const RelationSource &source : located)
			if (seen.insert({source.passage_sha256 + "|" + source.cell, source.offset}).second) base.root_sources.push_back(source);
	}
	if (base.depth == 0) base.depth = 1;
	base.constant_count = static_cast<unsigned>(constants.size());
	base.scale_breaking = base.scale_breaking || node.op == "round";
	if (!context.issue) return failure(OpStatus::Error, Code::NotEvaluable);

	if (answer.status == OpStatus::Bounded) {
		if (!answer.lo && !answer.hi) {
			OpAnswer unsure = failure(OpStatus::Unknown, Code::PrecisionInsufficient);
			unsure.words = answer.words;
			unsure.roots = answer.roots;
			unsure.result = answer.result;
			return unsure;
		}
		if (node.bind) context.table->bounded_labels.insert(*node.bind);
		// One end known ("no earlier than"): that end as its symbol.
		if (!answer.lo || !answer.hi) {
			const semantic::Value &end = answer.lo ? *answer.lo : *answer.hi;
			answer.results = {issuing.issue(answer.lo ? "#lo" : "#hi", end, compute::written(type_of_value(end)))};
			return answer;
		}
		answer.results = {issuing.issue("#lo", *answer.lo, compute::written(type_of_value(*answer.lo))),
			issuing.issue("#hi", *answer.hi, compute::written(type_of_value(*answer.hi)))};
		return answer;
	}
	// A formula worked out again and named (live E1, 26 Sep 2026: "recalc(E19)
	// -> ledger_balance", then "equal(@ledger_balance, 0)"): the value is the
	// result, and whether the saved value holds is said beside it.
	if (node.op == "recalc" && node.bind && answer.value && !answer.relation.empty()) {
		answer.sign = answer.relation == "TRUE" ? "the saved value holds" : "the saved value no longer holds";
		answer.relation.clear();
	}
	// A TRUE or FALSE the model named is a flag later ops may use by that name
	// (live EN1, 26 Sep 2026: "greater_than(...) -> threshold_met", then
	// "if_then_else(@threshold_met, ...)"); it is said as the relation it is.
	if (node.bind && (answer.relation == "TRUE" || answer.relation == "FALSE")
			&& context.table->compute_labels.count(*node.bind) == 0)
		context.table->compute_labels[*node.bind] =
			issuing.issue(std::string(), semantic::make_boolean(answer.relation == "TRUE"), "BOOLEAN");
	if (!answer.relation.empty() || answer.result.type.kind == TypeKind::Word) return answer;
	// A derived column is kept by its table under the model's name (@label):
	// no value of its own to name.
	if (answer.result.type.kind == TypeKind::Column) return answer;
	if (!answer.results.empty()) {
		// The evaluator's own symbol (a cell's fact): nothing new is issued.
	} else if (answer.rows && answer.result.type.kind == TypeKind::RowSet) {
		// A set of rows is stated where its rows are: each row's label cell.
		if (const tables::ProtectedTable *owner = tables::find_table(*context.table, answer.rows->table))
			for (const std::size_t number : answer.rows->certain)
				if (const tables::TableRow *row = tables::row_of(*owner, number))
					for (const tables::TableCell &cell : row->cells)
						if (cell.value || !cell.text.empty()) {
							issuing.base.roots.insert(tables::cell_root(*owner, cell));
							issuing.base.root_sources.push_back(cell.source);
							break;
						}
		const std::string symbol = issuing.issue(std::string(), semantic::Value{}, compute::written(answer.result.type));
		write_original(context, symbol, row_set_writing(context, *answer.rows));
		tables::remember(*context.table, symbol, node.bind.value_or(std::string()), answer);
		answer.results = {symbol};
	} else if (answer.groups && answer.result.type.kind == TypeKind::GroupSet) {
		const std::string symbol = issuing.issue(std::string(), semantic::Value{}, "GROUP_SET(" + answer.result.type.key + ")");
		write_original(context, symbol, group_set_writing(context, *answer.groups));
		tables::remember(*context.table, symbol, node.bind.value_or(std::string()), answer);
		answer.results = {symbol};
	} else if (!answer.pairs.empty()) {
		std::string words;
		for (std::size_t index = 0; index < answer.pairs.size(); ++index) {
			const auto &[key, value] = answer.pairs[index];
			const std::string symbol = issuing.issue("#" + std::to_string(index + 1), value, compute::written(type_of_value(value)));
			answer.results.push_back(symbol);
			words += (index == 0 ? "" : "\n  ") + key + " \xE2\x86\x92 " + symbol;
		}
		answer.words = words;
	} else if (answer.value) {
		answer.results = {issuing.issue(std::string(), *answer.value, compute::written(answer.result.type))};
	} else if (answer.groups || answer.rows) {
		answer.words = answer.words.empty() ? std::string("none") : answer.words;
	} else {
		return failure(OpStatus::Error, Code::NotEvaluable);
	}
	// Cells of different columns added together are said to be so (live EN1,
	// 26 Sep 2026: a "Paid" cell summed with three shortfall cells, silently):
	// the model sees what it combined and can correct it.
	if (node.op == "add" || node.op == "sum") {
		std::set<std::string> columns;
		for (const Operand &operand : operands) {
			if (operand.arg.kind == compute::ArgKind::CellRef && !operand.arg.column.empty()) {
				columns.insert(operand.arg.column);
				continue;
			}
			if (operand.row == nullptr) continue;
			const auto first = context.table->mapping.data();
			if (operand.row < first || operand.row >= first + context.table->mapping.size()) continue;
			const std::size_t index = static_cast<std::size_t>(operand.row - first);
			for (const auto &[cell, fact] : context.table->fact_by_cell) {
				if (fact != index) continue;
				const std::size_t bang = cell.rfind('!');
				std::string letters;
				for (std::size_t at = bang == std::string::npos ? 0 : bang + 1; at < cell.size() && cell[at] >= 'A' && cell[at] <= 'Z'; ++at)
					letters.push_back(cell[at]);
				if (!letters.empty()) columns.insert(letters);
				break;
			}
		}
		if (columns.size() > 1) {
			std::string listed;
			std::size_t at = 0;
			for (const std::string &column : columns)
				listed += (at++ == 0 ? "" : at == columns.size() ? " and " : ", ") + column;
			answer.sign = (answer.sign.empty() ? std::string() : answer.sign + " \xC2\xB7 ") + "cells from columns " + listed;
		}
	}
	if (!expectNote.empty() && (answer.status == OpStatus::Ok || answer.status == OpStatus::Bounded))
		answer.sign = (answer.sign.empty() ? std::string() : answer.sign + " \xC2\xB7 ") + expectNote;
	// A name the op bound stands for its symbol from here on, in this round too.
	if (node.bind && !answer.results.empty() && answer.relation.empty() && context.table->compute_labels.count(*node.bind) == 0)
		context.table->compute_labels[*node.bind] = answer.results.front();
	return answer;
}

} // namespace compute_executor

std::vector<compute::OpAnswer> execute_ops(const std::vector<compute::OpNode> &ops, compute::OpContext &context) {
	compute::ensure_families_registered();
	if (context.table != nullptr) tables::relink_tables(*context.table);
	std::vector<compute::OpAnswer> answers;
	answers.reserve(ops.size());
	// Names this batch's failed ops would have bound: an op that uses one is
	// answered dependency_failed - the model fixes the op it depends on.
	std::set<std::string> failedBinds;
	for (const compute::OpNode &node : ops) {
		compute::OpRecord record;
		record.op_id = "c" + std::to_string(context.call) + ".r" + std::to_string(context.round) + "." + node.id;
		record.call = context.call;
		record.round = context.round;
		record.model_turn = context.model_turn;
		record.carrier = compute::to_string(context.carrier);
		record.request_canonical = compute::request_canonical(node);
		record.expected_type = node.expect.value_or(std::string());
		record.declared_label = node.bind.value_or(std::string());
		record.justification = node.why;
		compute::OpAnswer answer;
		std::size_t dependency = 0;
		const auto failed_name = [&](const std::string &written) {
			const std::size_t start = written.find_first_not_of(" \t");
			if (start == std::string::npos) return false;
			std::string label = written.substr(written[start] == '@' ? start + 1 : start);
			while (!label.empty() && (label.back() == ' ' || label.back() == '\t')) label.pop_back();
			return failedBinds.count(label) != 0;
		};
		for (std::size_t argument = 0; argument < node.args.size() && dependency == 0; ++argument)
			if (failed_name(node.args[argument])) dependency = argument + 1;
		// A filter's test on a name that failed ("on_or_after @lookback_start").
		if (dependency == 0 && node.where)
			for (std::size_t atom = 0; atom < node.where->size() && dependency == 0; ++atom) {
				const compute::WhereAtom &where = (*node.where)[atom];
				if ((where.value && failed_name(*where.value)) || (where.value2 && failed_name(*where.value2))
						|| (!where.column.empty() && where.column.front() == '@' && failed_name(where.column)))
					dependency = atom + 1;
			}
		if (dependency != 0) {
			answer = compute_executor::failure(compute::OpStatus::Error, compute::Code::DependencyFailed, dependency);
		} else if (context.table == nullptr) {
			answer = compute_executor::failure(compute::OpStatus::NotServed, compute::Code::RoundBudget);
		} else {
			const compute::OpNode *previous = context.node;
			context.node = &node;
			try {
				answer = compute_executor::execute_one(node, context, record);
			} catch (const std::exception &) {
				answer = compute_executor::failure(compute::OpStatus::Error, compute::Code::EvaluationFailed);
			}
			context.node = previous;
		}
		record.status = compute::to_string(answer.status);
		record.code = answer.status == compute::OpStatus::Ok || answer.status == compute::OpStatus::Bounded ? std::string()
			: compute::to_string(answer.code != compute::Code::None ? answer.code : answer.certainty.reason);
		record.results = answer.results;
		record.relation = answer.relation;
		record.sign = answer.sign;
		if (node.bind && answer.status != compute::OpStatus::Ok && answer.status != compute::OpStatus::Bounded)
			failedBinds.insert(*node.bind);
		// The order it told is known from now on, for the matter's life.
		if (context.ledger != nullptr) context.ledger->learn(compute::order_edges(record));
		// A formula worked out again whose saved value holds told that the two
		// are one value: comparing them after it asks nothing new (live E1 c26:
		// three recalcs of the ledger's totals, each compared with its saved
		// total after, spent the base rent's budget twice over).
		if (context.ledger != nullptr && context.table != nullptr && node.op == "recalc"
				&& answer.sign == "the saved value holds" && !answer.results.empty() && !record.operand_fact_keys.empty()) {
			const auto worked = context.table->derived_facts.find(answer.results.front());
			const std::string &saved = record.operand_fact_keys.back();
			if (worked != context.table->derived_facts.end() && !worked->second.fact_key.empty() && !saved.empty()
					&& worked->second.fact_key != saved)
				context.ledger->learn({"le|" + worked->second.fact_key + "|" + saved, "le|" + saved + "|" + worked->second.fact_key});
		}
		if (context.table != nullptr) context.table->op_log.push_back(std::move(record));
		answers.push_back(std::move(answer));
	}
	return answers;
}

} // namespace singularity::protected_api::papinian::operations
