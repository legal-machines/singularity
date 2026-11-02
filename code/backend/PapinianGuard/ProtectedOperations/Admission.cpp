#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <vector>

// Operand admission (DESIGN4 §8.1 R1-R11), the grounding index (§8.2) and
// query admission (§8.7). Admission sees the AST, the declared types, the
// operands' provenance and the sent text - never a hidden value - so every
// verdict it gives is the same in every world (§8.4 L2). Where the design
// allows a laundered form that this build cannot prove equivalent (a
// threshold moved inside a derived value), admission refuses rather than
// guesses: the protected fact's value can only ever be narrowed less.
namespace singularity::protected_api::papinian::operations {

namespace {

using semantic::DurationUnit;
using semantic::Rational;
using semantic::Value;

std::optional<long long> minutes_in(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Minute: return 1;
		case DurationUnit::Hour: return 60;
		case DurationUnit::Day: return 1440;
		case DurationUnit::Week: return 10080;
		default: return std::nullopt;
	}
}

std::optional<long long> months_in(DurationUnit unit) {
	switch (unit) {
		case DurationUnit::Month: return 1;
		case DurationUnit::Quarter: return 3;
		case DurationUnit::Year: return 12;
		default: return std::nullopt;
	}
}

// The same quantity, however written: "2 years" is "24 months", 0,2 % in a
// clause is 0.002 in a cell, a bare "19" in a ledger cell grounds 19.
bool same_figure(const Value &constant, const Value &grounded) {
	if (semantic::value_key(constant) == semantic::value_key(grounded)) return true;
	if (constant.kind == semantic::Kind::Duration && grounded.kind == semantic::Kind::Duration) {
		// A period counted back ("-24 months" of "the 24 months preceding") is
		// the period the material writes.
		const Rational amount = constant.amount.abs();
		if (constant.unit == DurationUnit::BusinessDay || grounded.unit == DurationUnit::BusinessDay)
			return constant.unit == grounded.unit && amount == grounded.amount;
		if (minutes_in(constant.unit) && minutes_in(grounded.unit))
			return amount * Rational(*minutes_in(constant.unit)) == grounded.amount * Rational(*minutes_in(grounded.unit));
		if (months_in(constant.unit) && months_in(grounded.unit))
			return amount * Rational(*months_in(constant.unit)) == grounded.amount * Rational(*months_in(grounded.unit));
		return false;
	}
	// A count counted back ("-24" months of "the 24 months preceding") is the
	// count the material writes: a sign says the direction, not a figure.
	const bool plain = constant.kind == semantic::Kind::Number || constant.kind == semantic::Kind::Count;
	if (plain && (grounded.kind == semantic::Kind::Number || grounded.kind == semantic::Kind::Count
			|| grounded.kind == semantic::Kind::Duration))
		return constant.amount.abs() == grounded.amount.abs();
	// A proportion and the plain figure the material writes for it ("a share of
	// 0.5"): one quantity.
	if (constant.kind == semantic::Kind::Percentage
			&& (grounded.kind == semantic::Kind::Percentage || grounded.kind == semantic::Kind::Number))
		return constant.amount == grounded.amount;
	return false;
}

// R4: the numbers every calendar and scale uses.
bool safe_constant(const Value &value) {
	// 2-11 since 26 Sep 2026: a small count scales an amount by the periods it
	// covers ("three months" read from "rows 16-18"; live E1 was refused
	// multiply(shortfall, 3) and lost the answer). The product is a symbol, and
	// any relation on it is charged like every other.
	static const std::set<long long> safe{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 24, 60, 100, 360, 365, 366, 1440};
	if (!value.amount.is_integer()) return false;
	// Counted back or forward alike: "-12" months is a year, as "12" is.
	const semantic::Int128 number = value.amount.abs().numerator();
	if (number < 0 || number > 1440) return false;
	const long long whole = static_cast<long long>(number);
	switch (value.kind) {
		case semantic::Kind::Number:
		case semantic::Kind::Count:
		case semantic::Kind::Duration: return safe.count(whole) != 0;
		case semantic::Kind::Percentage: return value.amount == Rational(0) || value.amount == Rational(1);
		default: return false;
	}
}

bool zero_constant(const Value &value) {
	return value.amount.is_zero() && value.kind != semantic::Kind::Date && value.kind != semantic::Kind::DayOfYear
		&& value.kind != semantic::Kind::Boolean;
}

struct GroundedFigure {
	Value value;
	compute::ConstantUse use;
};

std::vector<GroundedFigure> grounded_figures(const std::string &payload, const std::vector<compute::PayloadSegment> &segments) {
	std::vector<GroundedFigure> found;
	for (const compute::PayloadSegment &segment : segments) {
		compute::Grounding grounding = compute::Grounding::Material;
		switch (segment.kind) {
			case compute::PayloadSegment::Kind::Source: grounding = compute::Grounding::Material; break;
			case compute::PayloadSegment::Kind::Request: grounding = compute::Grounding::Request; break;
			case compute::PayloadSegment::Kind::LawyerAnswer: grounding = compute::Grounding::Lawyer; break;
			default: continue;
		}
		if (segment.start >= payload.size()) continue;
		const std::string text = payload.substr(segment.start, std::min(segment.length, payload.size() - segment.start));
		for (const semantic::RecognisedFigure &figure : semantic::FigureRecogniser::find(text, semantic::language_of(text))) {
			if (figure.structural || !figure.value) continue;
			// An amount or a date is never a constant (R2): it is its symbol.
			if (figure.value->kind == semantic::Kind::Money || figure.value->kind == semantic::Kind::Date
					|| figure.value->kind == semantic::Kind::DayOfYear)
				continue;
			GroundedFigure grounded;
			grounded.value = *figure.value;
			// A rate is for the period its own clause gives it ("0.05% of the
			// overdue amount per day", «0,2 % ... за каждый день»).
			if (grounded.value.kind == semantic::Kind::Percentage && grounded.value.basis == semantic::RateBasis::None)
				grounded.value.basis = semantic::rate_basis_of_clause(text, figure.start, figure.start + figure.length);
			grounded.use.value_key = semantic::value_key(*figure.value);
			grounded.use.grounding = grounding;
			grounded.use.passage_sha256 = segment.passage_sha256;
			grounded.use.offset = segment.start + figure.start;
			grounded.use.length = figure.length;
			found.push_back(std::move(grounded));
		}
	}
	return found;
}

// The index for one payload, kept while the same payload is admitted op after op.
const std::vector<GroundedFigure> &index_for(const compute::OpContext &context) {
	thread_local std::size_t lastHash = 0;
	thread_local std::size_t lastSize = static_cast<std::size_t>(-1);
	thread_local std::vector<GroundedFigure> lastIndex;
	std::size_t hash = std::hash<std::string>{}(context.sent);
	for (const compute::PayloadSegment &segment : context.segments)
		hash = hash * 1000003u ^ (segment.start * 31u + segment.length * 7u + static_cast<std::size_t>(segment.kind));
	if (hash != lastHash || context.sent.size() != lastSize) {
		lastIndex = grounded_figures(context.sent, context.segments);
		lastHash = hash;
		lastSize = context.sent.size();
	}
	return lastIndex;
}

std::optional<compute::ConstantUse> grounded_use(const Value &constant, const compute::OpContext &context) {
	for (const GroundedFigure &figure : index_for(context))
		if (same_figure(constant, figure.value)) {
			compute::ConstantUse use = figure.use;
			use.value_key = semantic::value_key(constant);
			return use;
		}
	return std::nullopt;
}

bool dimensionless_kind(semantic::Kind kind) {
	return kind == semantic::Kind::Percentage || kind == semantic::Kind::Number || kind == semantic::Kind::Count
		|| kind == semantic::Kind::Duration;
}

// Whether a hidden root is a dimensionless fact (a rate, a count, a period, a
// plain number). A root whose kind this build cannot read is taken as one: the
// stricter rule then applies.
bool dimensionless_root(const std::string &root, const compute::OpContext &context) {
	if (context.table == nullptr) return true;
	const auto row = context.table->fact_by_key.find(root);
	if (row != context.table->fact_by_key.end() && row->second < context.table->mapping.size()) {
		const RequestMappingEntry &entry = context.table->mapping[row->second];
		return !entry.value || dimensionless_kind(entry.value->kind);
	}
	for (const tables::ProtectedTable &table : context.table->tables)
		for (const tables::TableRow &tableRow : table.rows)
			for (const tables::TableCell &cell : tableRow.cells)
				if (cell.fact_key == root) return !cell.value || dimensionless_kind(cell.value->kind);
	return true;
}

// A root's public grouping key (DESIGN4 §8.3): (sheet, column) for a cell, the
// role and kind otherwise - never a value.
std::string group_key_of(const std::string &root, const compute::OpContext &context) {
	if (context.table == nullptr) return "fact:" + root;
	const auto row = context.table->fact_by_key.find(root);
	if (row == context.table->fact_by_key.end() || row->second >= context.table->mapping.size()) return "fact:" + root;
	const RequestMappingEntry &entry = context.table->mapping[row->second];
	const FactProvenance &provenance = entry.provenance;
	if (!provenance.cell.empty()) {
		std::string column = provenance.cell;
		while (!column.empty() && column.back() >= '0' && column.back() <= '9') column.pop_back();
		return "cell:" + provenance.passage_sha256.substr(0, 16) + "|" + column;
	}
	if (!provenance.role.empty()) {
		const std::string kind = entry.value ? semantic::value_key(*entry.value).substr(0, semantic::value_key(*entry.value).find(':')) : "ENTITY";
		return "role:" + provenance.role + "|" + kind;
	}
	return "fact:" + root;
}

bool same_operand_op(const std::string &op) {
	for (const char *name : {"add", "subtract", "multiply", "divide", "ratio", "percentage_change", "sum", "average", "min",
			"max", "compare", "equal", "not_equal", "greater_than", "less_than", "greater_or_equal", "less_or_equal",
			"between", "duration_between", "percentage_of"})
		if (op == name) return true;
	return false;
}

bool structural_operand(const compute::Operand &operand) {
	return operand.table != nullptr || operand.rows || operand.groups || operand.schedule != nullptr
		|| operand.calendar != nullptr || operand.type.kind == compute::TypeKind::Calendar
		|| operand.type.kind == compute::TypeKind::Entity || operand.type.kind == compute::TypeKind::Table
		|| operand.type.kind == compute::TypeKind::Column || operand.type.kind == compute::TypeKind::RowSet
		|| operand.type.kind == compute::TypeKind::GroupSet || operand.type.kind == compute::TypeKind::Schedule;
}

Admission refuse(compute::Code code, std::size_t argument = 0) {
	Admission verdict;
	verdict.code = code;
	verdict.argument = argument;
	return verdict;
}

} // namespace

std::optional<semantic::Value> material_figure(const semantic::Value &constant, const compute::OpContext &context) {
	std::optional<semantic::Value> found;
	for (const GroundedFigure &figure : index_for(context)) {
		if (!same_figure(constant, figure.value)) continue;
		if (figure.value.basis != semantic::RateBasis::None) return figure.value;
		if (!found) found = figure.value;
	}
	return found;
}

std::vector<compute::ConstantUse> ground_constants(const std::string &payload, const std::vector<compute::PayloadSegment> &segments) {
	std::vector<compute::ConstantUse> uses;
	for (GroundedFigure &figure : grounded_figures(payload, segments)) uses.push_back(std::move(figure.use));
	return uses;
}

Admission admit(const compute::OpNode &node, const std::vector<compute::Operand> &operands, const compute::OpContext &context) {
	Admission verdict;
	const compute::OpEntry *entry = compute::find_op(node.op);
	if (entry == nullptr) return refuse(compute::Code::UnknownOp);
	const compute::OpOptions options = node.options.value_or(compute::OpOptions{});

	// ---- constants (R2, R3, R4, R10) and whether anything hidden is reached (R5)
	std::set<std::string> constants;
	bool reachesFact = false;
	for (std::size_t index = 0; index < operands.size(); ++index) {
		const compute::Operand &operand = operands[index];
		if (operand.arg.kind == compute::ArgKind::Constant) {
			// A word the op names is no value: nothing to ground.
			if (!operand.value && operand.type.kind == compute::TypeKind::Word) continue;
			if (!operand.value) return refuse(compute::Code::TypeMismatch, index + 1);
			const Value &value = *operand.value;
			if (zero_constant(value)) continue;
			if (value.kind == semantic::Kind::Money || value.kind == semantic::Kind::Date || value.kind == semantic::Kind::DayOfYear)
				return refuse(compute::Code::UnsafeLiteral, index + 1);
			if (safe_constant(value)) {
				compute::ConstantUse use;
				use.value_key = semantic::value_key(value);
				use.grounding = compute::Grounding::Safe;
				verdict.constants.push_back(use);
				constants.insert(use.value_key);
				continue;
			}
			const std::optional<compute::ConstantUse> grounded = grounded_use(value, context);
			if (!grounded) return refuse(compute::Code::ConstantNotInMaterial, index + 1);
			verdict.constants.push_back(*grounded);
			constants.insert(grounded->value_key);
			continue;
		}
		if (structural_operand(operand)) {
			reachesFact = true;
			continue;
		}
		if (operand.derived != nullptr && operand.roots.empty()) {
			// R10: a value computable from the payload is a constant.
			if (operand.value && !zero_constant(*operand.value)) {
				if (!safe_constant(*operand.value) && !grounded_use(*operand.value, context))
					return refuse(compute::Code::ConstantNotInMaterial, index + 1);
				constants.insert(semantic::value_key(*operand.value));
			}
			continue;
		}
		if (!operand.roots.empty() || operand.row != nullptr) reachesFact = true;
	}
	if (!operands.empty() && !reachesFact) return refuse(compute::Code::ConstantOnly);

	// ---- R9: the same quantity twice, in any op
	if (same_operand_op(node.op))
		for (std::size_t first = 0; first < operands.size(); ++first)
			for (std::size_t second = first + 1; second < operands.size(); ++second) {
				const std::string &a = operands[first].fact_key;
				const std::string &b = operands[second].fact_key;
				if (!a.empty() && a == b) return refuse(compute::Code::SameOperand, second + 1);
			}

	// ---- R7: at most two distinct constants in the whole derivation tree of
	// anything observed. A value worked out and kept opaque says nothing, so it
	// is built whatever its constants (live R2, 27 Sep 2026: three penalties,
	// each a rate times its own day count, could never be added up); the tree's
	// constants travel with it, and the first observation of it is refused here.
	std::set<std::string> tree = constants;
	for (const compute::Operand &operand : operands)
		if (operand.derived != nullptr)
			for (const compute::ConstantUse &use : operand.derived->constants) tree.insert(use.value_key);
	const bool manyConstants = tree.size() > compute::kMaxConstantsPerTree;

	// ---- depth and the canonical form
	compute::Code formError = compute::Code::None;
	const std::optional<CanonicalForm> form = normalise(node, operands, context, formError);
	if (!form) return refuse(formError == compute::Code::None ? compute::Code::DepthExceeded : formError);

	// ---- observations (R6, R8, R11) and their pre-charge (§8.3)
	std::optional<compute::ObservationCharge> charge;
	if (entry->observation != compute::ObservationClass::Never) {
		if (entry->pre_charge != nullptr) charge = entry->pre_charge(operands, options, context);
		else if (entry->observation == compute::ObservationClass::Always) {
			compute::ObservationCharge all;
			std::set<std::string> roots;
			for (const compute::Operand &operand : operands) roots.insert(operand.roots.begin(), operand.roots.end());
			all.roots.assign(roots.begin(), roots.end());
			charge = all;
		}
	}
	if (charge) {
		const bool zeroOnly = charge->roots.empty() && charge->calendar_roots.empty() && charge->group_keys.empty()
			&& !charge->zero_divisors.empty();
		if (!zeroOnly) {
			if (manyConstants) return refuse(compute::Code::TooManyConstants);
			for (std::size_t index = 0; index < operands.size(); ++index)
				if (operands[index].derived != nullptr && operands[index].derived->scale_breaking)
					return refuse(compute::Code::RoundedObservation, index + 1);
			bool external = false;
			bool lawyer = false;
			for (const compute::Operand &operand : operands)
				for (const std::string &root : operand.roots) (external_root(root, context) ? external : lawyer) = true;
			if (external && lawyer) return refuse(compute::Code::ExternalOperand);
			std::vector<std::string> hidden;
			for (const std::string &root : charge->roots)
				if (!external_root(root, context)) hidden.push_back(root);
			if (hidden.empty() && charge->calendar_roots.empty() && charge->group_keys.empty())
				return refuse(compute::Code::ConstantOnly);
			// R6 (c): one hidden dimensionless fact meets constants only as
			// itself against one grounded threshold, never through a value
			// built from it.
			// A table family's question is keyed by the set, partition or band it
			// is about; every other by the normalised form, so a rephrased
			// question is the same key.
			const bool tabular = std::any_of(operands.begin(), operands.end(), [](const compute::Operand &operand) {
				return operand.table != nullptr || operand.rows || operand.groups || operand.schedule != nullptr;
			});
			std::string key = tabular && !charge->key.empty() ? charge->key : form->key;
			// A root repeated in the charge is charged once per repetition
			// (list_rows' rank weight); the rules below read the distinct ones.
			const std::set<std::string> distinct(hidden.begin(), hidden.end());
			if (distinct.size() == 1 && !tree.empty() && dimensionless_root(hidden.front(), context)) {
				if (!form->affine || tree.size() > 1) return refuse(compute::Code::ConstantDepth);
				key = "thr(" + hidden.front() + "," + *tree.begin() + ")";
			}
			charge->key = key;
			charge->roots = hidden;
			// A table family names its columns' grouping keys itself; any other
			// charge is keyed here, one key per distinct root.
			std::set<std::string> groups(charge->group_keys.begin(), charge->group_keys.end());
			if (groups.empty())
				for (const std::string &root : distinct) groups.insert(group_key_of(root, context));
			charge->group_keys.assign(groups.begin(), groups.end());
		}
	}
	verdict.charge = charge;
	return verdict;
}

bool writes_a_symbol(const std::string &text) {
	const auto isLetter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
	const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
	const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
	for (std::size_t open = text.find('['); open != std::string::npos; open = text.find('[', open + 1)) {
		std::size_t at = open;
		while (at < text.size() && text[at] == '[') ++at;
		while (at < text.size() && isSpace(text[at])) ++at;
		const std::size_t letters = at;
		while (at < text.size() && isLetter(text[at])) ++at;
		if (at == letters) continue;
		while (at < text.size() && isSpace(text[at])) ++at;
		// Opened like a symbol, it counts as one closed or not: restoration
		// fills in the closed writings and refuses the rest, and a search is
		// sent neither.
		if (at < text.size() && isDigit(text[at])) return true;
	}
	return false;
}

compute::Code admit_query(const std::string &query, const std::string &payload,
		const std::vector<compute::PayloadSegment> &segments, bool web) {
	// A web query never carries a symbol, in any writing: nothing protected
	// reaches a search engine. "[Org1]" stands in the material only as part of
	// "[[Org1]]", and restoration fills it in all the same (PRIV-1).
	if (web && writes_a_symbol(query)) return compute::Code::QueryLiteral;
	std::string material;
	for (const compute::PayloadSegment &segment : segments) {
		if (segment.kind != compute::PayloadSegment::Kind::Source && segment.kind != compute::PayloadSegment::Kind::Request
				&& segment.kind != compute::PayloadSegment::Kind::LawyerAnswer)
			continue;
		if (segment.start >= payload.size()) continue;
		material += payload.substr(segment.start, std::min(segment.length, payload.size() - segment.start));
		material += '\n';
	}
	// A figure in a query is a guess at a value unless the material states it.
	std::string outside;
	for (std::size_t at = 0; at < query.size();) {
		const std::size_t open = query.find("[[", at);
		if (open == std::string::npos) {
			outside += query.substr(at);
			break;
		}
		outside += query.substr(at, open - at) + " ";
		const std::size_t close = query.find("]]", open);
		at = close == std::string::npos ? query.size() : close + 2;
	}
	for (const semantic::RecognisedFigure &figure : semantic::FigureRecogniser::find(outside, semantic::language_of(outside))) {
		if (figure.structural) continue;
		const std::string written = outside.substr(figure.start, figure.length);
		if (material.find(written) == std::string::npos) return compute::Code::QueryLiteral;
	}
	// A word with a digit, or a capitalised word inside the query, names
	// something: it must already stand in the lawyer's material.
	std::size_t index = 0;
	bool first = true;
	while (index < outside.size()) {
		while (index < outside.size() && (outside[index] == ' ' || outside[index] == '\t' || outside[index] == '\n')) ++index;
		std::size_t end = index;
		while (end < outside.size() && outside[end] != ' ' && outside[end] != '\t' && outside[end] != '\n') ++end;
		if (end > index) {
			std::string word = outside.substr(index, end - index);
			while (!word.empty() && std::string(",.;:!?\"'()").find(word.back()) != std::string::npos) word.pop_back();
			const bool digit = std::any_of(word.begin(), word.end(), [](char c) { return c >= '0' && c <= '9'; });
			const bool capital = !first && !word.empty() && word[0] >= 'A' && word[0] <= 'Z';
			if (!word.empty() && (digit || capital) && material.find(word) == std::string::npos) return compute::Code::QueryLiteral;
			first = false;
		}
		index = end;
	}
	return compute::Code::None;
}

} // namespace singularity::protected_api::papinian::operations
