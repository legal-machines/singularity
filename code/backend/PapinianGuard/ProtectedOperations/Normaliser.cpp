#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Canonical forms over hidden root facts (DESIGN4 §8.1): which hidden facts an
// operation reaches, whether its value is already known from the payload, and
// the key an observation is charged and cached under. Keys are built from fact
// keys and constants' value keys only - never from a hidden value - so the key
// of a question is the same in every world (§8.4 L2).
//
// The key is canonical over operand order and over the ways one question can be
// asked: `greater_than(x, y)`, `less_than(y, x)`, `less_or_equal(x, y)` are one
// order observation; `compare`, `sign` and a difference's returned sign are one
// three-way observation; equality has its own. A question already answered is
// answered again free (the ledger), so a model gains nothing by rephrasing.
namespace singularity::protected_api::papinian::operations {

namespace {

bool among(const std::string &op, std::initializer_list<const char *> names) {
	for (const char *name : names)
		if (op == name) return true;
	return false;
}

// A derived operand's own derivation, when it is one the relations below read
// through: "ratio(fk:a,fk:b)" or "apply_rate(fk:y,fk:r;...)" -> {op, args, mode}.
struct Derivation {
	std::string op;
	std::vector<std::string> args;
	std::string mode;
};

std::optional<Derivation> derivation_of(const compute::Operand &operand) {
	if (operand.derived == nullptr) return std::nullopt;
	const std::string &ast = operand.derived->ast;
	const std::size_t open = ast.find('(');
	if (open == std::string::npos || ast.empty() || ast.back() != ')') return std::nullopt;
	Derivation parsed;
	parsed.op = ast.substr(0, open);
	const std::string inner = ast.substr(open + 1, ast.size() - open - 2);
	const std::size_t semicolon = inner.find(';');
	const std::string args = inner.substr(0, semicolon);
	for (std::size_t at = 0; at <= args.size();) {
		const std::size_t comma = std::min(args.find(',', at), args.size());
		parsed.args.push_back(args.substr(at, comma - at));
		at = comma + 1;
	}
	// Only operands that are facts or constants, never a nested set.
	for (const std::string &arg : parsed.args)
		if (arg.rfind("fk:", 0) != 0 && arg.rfind("c:", 0) != 0) return std::nullopt;
	if (semicolon != std::string::npos) {
		std::vector<std::string> options;
		const std::string rest = inner.substr(semicolon + 1);
		for (std::size_t at = 0; at <= rest.size();) {
			const std::size_t comma = std::min(rest.find(',', at), rest.size());
			options.push_back(rest.substr(at, comma - at));
			at = comma + 1;
		}
		if (options.size() > 5) parsed.mode = options[5];
	}
	return parsed;
}

} // namespace

// An operand as a key names it: its fact, its constant, its table or its words.
std::string operand_key(const compute::Operand &operand) {
	if (!operand.fact_key.empty()) return "fk:" + operand.fact_key;
	if (operand.rows) return "rows:" + operand.rows->canonical;
	if (operand.groups) return "groups:" + operand.groups->canonical;
	if (operand.table != nullptr) return "table:" + operand.table->handle + (operand.arg.column.empty() ? "" : "." + operand.arg.column);
	if (operand.calendar != nullptr) return "cal:" + operand.calendar->handle;
	if (operand.value && operand.roots.empty()) return "c:" + semantic::value_key(*operand.value);
	return "w:" + operand.arg.written;
}

// The fact keys among `roots` that the model may know without the lawyer's
// material: facts that first appeared in a page the model itself asked for.
bool external_root(const std::string &root, const compute::OpContext &context) {
	if (context.table == nullptr) return false;
	const auto row = context.table->fact_by_key.find(root);
	if (row == context.table->fact_by_key.end() || row->second >= context.table->mapping.size()) return false;
	return context.table->mapping[row->second].provenance.origin == FactProvenance::Origin::External;
}

std::optional<CanonicalForm> normalise(const compute::OpNode &node, const std::vector<compute::Operand> &operands,
		const compute::OpContext &context, compute::Code &error) {
	error = compute::Code::None;
	unsigned depth = 0;
	for (const compute::Operand &operand : operands) depth = std::max(depth, operand.depth);
	if (depth + 1 > compute::kMaxDerivationDepth) {
		error = compute::Code::DepthExceeded;
		return std::nullopt;
	}
	CanonicalForm form;
	for (const compute::Operand &operand : operands)
		for (const std::string &root : operand.roots)
			if (!external_root(root, context)) form.hidden_roots.insert(root);
	form.known = form.hidden_roots.empty();
	form.monomials = operands.size();

	std::vector<std::string> keys;
	for (const compute::Operand &operand : operands) keys.push_back(operand_key(operand));
	const std::string &op = node.op;
	// One relation, one key however it is asked (owner, 27 Sep 2026: a relation
	// already given is free): x against a share of y at r is x/y against r, and
	// a ratio compared is the ratio itself, whichever symbol names it.
	const bool relation = among(op, {"greater_than", "less_than", "greater_or_equal", "less_or_equal", "compare", "equal",
		"not_equal"});
	if (relation && operands.size() == 2) {
		for (std::size_t index = 0; index < 2; ++index) {
			const std::optional<Derivation> derived = derivation_of(operands[index]);
			if (derived && derived->op == "ratio" && derived->args.size() == 2)
				keys[index] = "ratio(" + derived->args[0] + "," + derived->args[1] + ")";
		}
		for (std::size_t index = 0; index < 2; ++index) {
			const std::optional<Derivation> derived = derivation_of(operands[index]);
			if (!derived || derived->op != "apply_rate" || derived->args.size() != 2
					|| (!derived->mode.empty() && derived->mode != "share"))
				continue;
			const std::string other = keys[1 - index];
			keys[index] = derived->args[1];
			keys[1 - index] = "ratio(" + other + "," + derived->args[0] + ")";
			break;
		}
	}
	std::string head;
	if (among(op, {"greater_than", "less_than", "greater_or_equal", "less_or_equal", "before", "after"})) {
		head = "ord";
		std::sort(keys.begin(), keys.end());
	} else if (among(op, {"compare", "subtract"})) {
		head = "cmp";
		std::sort(keys.begin(), keys.end());
	} else if (op == "sign") {
		head = "cmp";
		keys.push_back("c:zero");
		std::sort(keys.begin(), keys.end());
	} else if (among(op, {"equal", "not_equal", "same_time"})) {
		head = "eq";
		std::sort(keys.begin(), keys.end());
	} else if (among(op, {"add", "multiply", "sum", "average", "min", "max", "and", "or"})) {
		head = op;
		std::sort(keys.begin(), keys.end());
	} else {
		head = op;
	}
	std::string key = head + "(";
	for (std::size_t index = 0; index < keys.size(); ++index) key += (index == 0 ? "" : ",") + keys[index];
	key += ")";
	if (node.options) {
		// Options with nothing set are no options: one question, one key.
		const compute::OpOptions &options = *node.options;
		const std::string set = options.unit.value_or("") + ";" + options.calendar.value_or("") + ";"
			+ options.anchor.value_or("") + ";" + options.mode.value_or("");
		if (set != ";;;") key += "[" + set + "]";
	}
	form.key = key;

	// Affine in its one hidden root: the root itself, as an operand, against
	// constants only (DESIGN4 §8.1 R6 case (c)).
	if (form.hidden_roots.size() == 1) {
		const std::string &root = *form.hidden_roots.begin();
		bool direct = false;
		bool others_constant = true;
		for (const compute::Operand &operand : operands) {
			if (operand.derived == nullptr && operand.fact_key == root) direct = true;
			else if (!operand.roots.empty()) others_constant = false;
		}
		form.affine = direct && others_constant;
	}
	return form;
}

} // namespace singularity::protected_api::papinian::operations
