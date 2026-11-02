#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"


#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The observation ledger (DESIGN4 §8.3) and the pause snapshot (§1.10).
//
// Every charge is decided from the AST before evaluation and paid before the
// outcome exists, so a refusal for the budget never depends on a value. A
// root repeated in a charge (list_rows' rank weight, a band lookup) is charged
// once per repetition. A question whose canonical key was answered before - in
// this task or any earlier task of the matter - is answered again free: the
// model learns nothing new by asking it again, however it is phrased. A zero
// test of a divisor is its own allowance, one per canonical divisor for the
// life of the matter, and never part of a root's budget. What the relations
// told of facts' order is kept as edges, and a relation they imply is free
// however it is asked (a = b and b > c tell a > c). A preload - the relations
// planner, the legend's timeline - is charged like any disclosure, from the
// same counts, but only up to kPreload*: what it leaves is the lawyer's task's
// (owner, 28 Sep 2026).
namespace singularity::protected_api::papinian::compute {

namespace {

bool zero_only(const ObservationCharge &charge) {
	return charge.roots.empty() && charge.calendar_roots.empty() && charge.group_keys.empty()
		&& !charge.zero_divisors.empty();
}

std::map<std::string, std::size_t> repetitions(const std::vector<std::string> &keys, std::size_t weight) {
	std::map<std::string, std::size_t> counted;
	for (const std::string &key : keys) counted[key] += std::max<std::size_t>(weight, 1);
	return counted;
}

std::size_t count_in(const std::map<std::string, std::size_t> &counts, const std::string &key) {
	const auto found = counts.find(key);
	return found == counts.end() ? 0 : found->second;
}

// The limits a charge of `priority` is held to.
struct Limits {
	std::size_t root = kObservationsPerRoot;
	std::size_t calendar = kCalendarObservationsPerDateRoot;
	std::size_t group = kObservationsPerGroupKey;
	std::size_t task = kObservationsPerTask;
};

Limits limits_of(DisclosurePriority priority, std::size_t keep) {
	if (priority == DisclosurePriority::OnDemand) return {};
	const std::size_t kept = std::min(kObservationsPerRoot, std::max(keep, kObservationsPerRoot - kPreloadObservationsPerRoot));
	return {kObservationsPerRoot - kept, kPreloadCalendarObservationsPerDateRoot, kPreloadObservationsPerGroupKey,
		kPreloadObservationsPerTask};
}

// An edge "lt|a|b" or "le|a|b" read back.
struct Edge {
	std::string from;
	std::string to;
	bool strict = false;
};

std::optional<Edge> edge_of(const std::string &written) {
	const std::size_t first = written.find('|');
	const std::size_t second = first == std::string::npos ? std::string::npos : written.find('|', first + 1);
	if (second == std::string::npos) return std::nullopt;
	const std::string kind = written.substr(0, first);
	if (kind != "lt" && kind != "le") return std::nullopt;
	Edge edge{written.substr(first + 1, second - first - 1), written.substr(second + 1), kind == "lt"};
	if (edge.from.empty() || edge.to.empty()) return std::nullopt;
	return edge;
}

// Whether x <= y follows from the edges, and whether strictly.
std::pair<bool, bool> reaches(const std::string &x, const std::string &y, const std::vector<Edge> &edges) {
	std::set<std::pair<std::string, bool>> seen{{x, false}};
	std::vector<std::pair<std::string, bool>> frontier{{x, false}};
	bool reached = false;
	bool strict = false;
	while (!frontier.empty()) {
		const auto [at, strictSoFar] = frontier.back();
		frontier.pop_back();
		for (const Edge &edge : edges) {
			if (edge.from != at) continue;
			const std::pair<std::string, bool> next{edge.to, strictSoFar || edge.strict};
			if (next.first == y) {
				reached = true;
				strict = strict || next.second;
			}
			if (seen.insert(next).second) frontier.push_back(next);
		}
	}
	return {reached, strict};
}

} // namespace

std::vector<std::string> order_edges(const OpRecord &record) {
	std::vector<std::string> edges;
	if (record.status != "ok" || record.operand_fact_keys.size() != 2) return edges;
	const std::string &x = record.operand_fact_keys[0];
	const std::string &y = record.operand_fact_keys[1];
	if (x.empty() || y.empty() || x == y) return edges;
	const std::string op = record.request_canonical.substr(0, record.request_canonical.find('('));
	std::string word = record.relation;
	if (word.empty()) {
		std::istringstream words(record.result_line);
		std::string id, status;
		words >> id >> status >> word;
	}
	const std::string signs = record.sign + " " + record.result_line;
	const auto lt = [&](const std::string &a, const std::string &b) { edges.push_back("lt|" + a + "|" + b); };
	const auto le = [&](const std::string &a, const std::string &b) { edges.push_back("le|" + a + "|" + b); };
	if (op == "compare") {
		if (word == "LESS") lt(x, y);
		else if (word == "GREATER") lt(y, x);
		else if (word == "EQUAL") { le(x, y); le(y, x); }
		return edges;
	}
	if (op == "subtract") {
		if (signs.find("POSITIVE") != std::string::npos) lt(y, x);
		else if (signs.find("NEGATIVE") != std::string::npos) lt(x, y);
		else if (signs.find("ZERO") != std::string::npos) { le(x, y); le(y, x); }
		return edges;
	}
	if (word != "TRUE" && word != "FALSE") return edges;
	const bool holds = word == "TRUE";
	if (op == "greater_than" || op == "after") holds ? lt(y, x) : le(x, y);
	else if (op == "less_than" || op == "before") holds ? lt(x, y) : le(y, x);
	else if (op == "greater_or_equal") holds ? le(y, x) : lt(x, y);
	else if (op == "less_or_equal") holds ? le(x, y) : lt(y, x);
	else if ((op == "equal" || op == "same_time") && holds) { le(x, y); le(y, x); }
	return edges;
}

bool ObservationLedger::implied(const std::string &a, const std::string &b, const std::string &relation) const {
	if (a.empty() || b.empty()) return false;
	// One fact, however many places write it: how it stands to itself tells
	// nothing (a figure quoted in two documents from one source, an earlier
	// answer's figure anchored to its fact).
	if (a == b) return true;
	if (matter.order_edges.empty()) return false;
	std::vector<Edge> edges;
	for (const std::string &written : matter.order_edges)
		if (const std::optional<Edge> edge = edge_of(written)) edges.push_back(*edge);
	const auto [aLeB, aLtB] = reaches(a, b, edges);
	const auto [bLeA, bLtA] = reaches(b, a, edges);
	const bool known = aLtB || bLtA || (aLeB && bLeA);
	if (relation == "greater_than" || relation == "after") return bLtA || aLeB;
	if (relation == "less_than" || relation == "before") return aLtB || bLeA;
	if (relation == "greater_or_equal") return bLeA || aLtB;
	if (relation == "less_or_equal") return aLeB || bLtA;
	return known;  // compare, subtract's sign, equal, not_equal, same_time
}

std::vector<std::pair<std::string, std::string>> disclosure_details(const ObservationLedger &ledger) {
	const DisclosureDiagnostics &seen = ledger.diagnostics;
	std::string refusals;
	for (const std::string &refusal : seen.refusals) refusals += (refusals.empty() ? "" : "; ") + refusal;
	std::string atLimit;
	for (const std::string &root : seen.roots_at_limit) atLimit += (atLimit.empty() ? "" : " ") + root;
	return {{"disclosurePreloadProposed", std::to_string(seen.preload_proposed)},
		{"disclosurePreloadSent", std::to_string(seen.preload_sent)},
		{"disclosurePreloadFree", std::to_string(seen.preload_free)},
		{"disclosurePreloadSkippedIrrelevant", std::to_string(seen.preload_skipped_irrelevant)},
		{"disclosurePreloadSkippedReserve", std::to_string(seen.preload_skipped_reserve)},
		{"disclosureOnDemandCharged", std::to_string(seen.on_demand_charged)},
		{"disclosureOnDemandFree", std::to_string(seen.on_demand_free)},
		{"disclosureOnDemandRefused", std::to_string(seen.on_demand_refused)},
		{"disclosureTaskObservations", std::to_string(ledger.task_observations)},
		{"disclosureRootsAtLimit", atLimit},
		{"disclosureRefusals", refusals}};
}

void ObservationLedger::learn(const std::vector<std::string> &edges) {
	for (const std::string &edge : edges)
		if (edge_of(edge)) matter.order_edges.insert(edge);
}

bool ObservationLedger::answered(const std::string &key) const {
	if (key.empty()) return false;
	if (matter.answered.count(key) != 0) return true;
	// A pair's order or equality follows from its three-way comparison (or a
	// difference's sign) already given: implied, so free (owner, 27 Sep 2026).
	for (const char *implied : {"ord(", "eq("}) {
		const std::string head(implied);
		if (key.rfind(head, 0) == 0 && matter.answered.count("cmp(" + key.substr(head.size())) != 0) return true;
	}
	return false;
}

Code ObservationLedger::admit(const ObservationCharge &charge, DisclosurePriority priority) const {
	if (answered(charge.key) || zero_only(charge)) return Code::None;
	return refusal(charge, priority).empty() ? Code::None : Code::DisclosureBudget;
}

std::string ObservationLedger::refusal(const ObservationCharge &charge, DisclosurePriority priority) const {
	if (answered(charge.key) || zero_only(charge)) return {};
	const Limits limit = limits_of(priority, preload_keep);
	const std::size_t weight = std::max<std::size_t>(charge.weight, 1);
	const auto of = [](std::size_t count, std::size_t bound) { return std::to_string(count) + "/" + std::to_string(bound); };
	if (task_observations + weight > limit.task) return "task " + of(task_observations, limit.task);
	// One operation may ask several at once (list_rows' rank weight): said so.
	const auto asks = [](std::size_t times) { return times > 1 ? ", asks " + std::to_string(times) : std::string(); };
	for (const auto &[root, times] : repetitions(charge.roots, weight))
		if (count_in(matter.by_root, root) + times > limit.root)
			return "root " + root + " " + of(count_in(matter.by_root, root), limit.root) + " (preload "
				+ std::to_string(count_in(matter.preload_by_root, root)) + asks(times) + ")";
	for (const auto &[root, times] : repetitions(charge.calendar_roots, weight))
		if (count_in(matter.by_calendar_root, root) + times > limit.calendar)
			return "calendar " + root + " " + of(count_in(matter.by_calendar_root, root), limit.calendar);
	std::set<std::string> groups(charge.group_keys.begin(), charge.group_keys.end());
	for (const std::string &group : groups)
		if (count_in(matter.by_group_key, group) + weight > limit.group)
			return "group " + group + " " + of(count_in(matter.by_group_key, group), limit.group);
	return {};
}

void ObservationLedger::charge(const ObservationCharge &charge, DisclosurePriority priority) {
	if (answered(charge.key)) return;
	for (const std::string &divisor : charge.zero_divisors) matter.zero_divisors.insert(divisor);
	if (!charge.key.empty()) matter.answered.insert(charge.key);
	if (zero_only(charge)) return;
	const std::size_t weight = std::max<std::size_t>(charge.weight, 1);
	task_observations += weight;
	for (const auto &[root, times] : repetitions(charge.roots, weight)) {
		matter.by_root[root] += times;
		task_by_root[root] += times;
		if (priority == DisclosurePriority::Preload) matter.preload_by_root[root] += times;
		if (matter.by_root[root] >= kObservationsPerRoot) diagnostics.roots_at_limit.insert(root);
	}
	for (const auto &[root, times] : repetitions(charge.calendar_roots, weight)) matter.by_calendar_root[root] += times;
	std::set<std::string> groups(charge.group_keys.begin(), charge.group_keys.end());
	for (const std::string &group : groups) {
		matter.by_group_key[group] += weight;
		task_by_group_key[group] += weight;
	}
}

// ---------------------------------------------------------------------------
// Pause and resume (DESIGN4 §1.10): symbols, fact keys, codes and counts.

ComputeSnapshot snapshot(const OperationProtectedSymbolTable &table) {
	ComputeSnapshot taken;
	for (const OpRecord &record : table.op_log) {
		SnapshotOp op;
		op.op_id = record.op_id;
		op.request_canonical = record.request_canonical;
		op.operand_fact_keys = record.operand_fact_keys;
		op.status = record.status;
		op.code = record.code;
		op.declared_label = record.declared_label;
		for (const std::string &symbol : record.results) {
			const auto derived = table.derived_facts.find(symbol);
			op.result_fact_keys.push_back(derived == table.derived_facts.end() ? std::string() : derived->second.fact_key);
		}
		taken.op_log.push_back(std::move(op));
	}
	for (const auto &[label, symbol] : table.compute_labels) {
		const auto derived = table.derived_facts.find(symbol);
		if (derived != table.derived_facts.end()) taken.labels[label] = derived->second.fact_key;
	}
	taken.task_observations = table.observations.task_observations;
	taken.task_by_root = table.observations.task_by_root;
	taken.task_by_group_key = table.observations.task_by_group_key;
	taken.answered = table.observations.matter.answered;
	taken.compute_rounds_used = table.compute_rounds_used;
	taken.ops_used = table.ops_used;
	taken.repair_rounds_used = table.repair_rounds_used;
	taken.provider_chain = table.provider_chain;
	return taken;
}

namespace {

std::string trimmed_copy(std::string text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
	return text;
}

std::vector<std::string> split_top(const std::string &text, char separator) {
	std::vector<std::string> pieces;
	std::string piece;
	int depth = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const char character = text[index];
		if (character == '[') ++depth;
		if (character == ']') --depth;
		if (character == separator && depth == 0) {
			pieces.push_back(piece);
			piece.clear();
			continue;
		}
		piece.push_back(character);
	}
	pieces.push_back(piece);
	return pieces;
}

bool digits_only(const std::string &text) {
	return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
}

// The op a log entry asked for, read back from its canonical request
// (ComputeProtocol's request_canonical): op(args;where:atoms;options). A
// decimal comma inside a constant ("0,2 %") is rejoined so the arguments
// come back as many as the entry's operands.
std::optional<OpNode> node_of(const SnapshotOp &entry, std::size_t index) {
	const std::string &text = entry.request_canonical;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || text.empty() || text.back() != ')') return std::nullopt;
	OpNode node;
	node.id = "q" + std::to_string(index + 1);
	node.op = text.substr(0, open);
	node.why = "resumed";
	if (!entry.declared_label.empty()) node.bind = entry.declared_label;
	const std::vector<std::string> sections = split_top(text.substr(open + 1, text.size() - open - 2), ';');
	std::vector<std::string> args = split_top(sections.front(), ',');
	if (args.size() == 1 && trimmed_copy(args.front()).empty()) args.clear();
	const std::size_t wanted = entry.operand_fact_keys.size();
	for (std::size_t at = 0; wanted > 0 && args.size() > wanted && at + 1 < args.size();) {
		const std::string &next = args[at + 1];
		if (digits_only(trimmed_copy(args[at])) && !next.empty() && next.front() >= '0' && next.front() <= '9') {
			args[at] += "," + next;
			args.erase(args.begin() + static_cast<std::ptrdiff_t>(at) + 1);
			continue;
		}
		++at;
	}
	for (std::string &arg : args) node.args.push_back(trimmed_copy(arg));
	for (std::size_t section = 1; section < sections.size(); ++section) {
		const std::string &part = sections[section];
		if (part.rfind("where:", 0) == 0) {
			std::vector<WhereAtom> atoms;
			for (const std::string &written : split_top(part.substr(6), '&')) {
				WhereAtom atom;
				const std::string clean = trimmed_copy(written);
				const std::size_t first = clean.find(' ');
				if (first == std::string::npos) return std::nullopt;
				atom.column = clean.substr(0, first);
				const std::size_t second = clean.find(' ', first + 1);
				atom.test = clean.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
				if (second != std::string::npos) {
					const std::string rest = trimmed_copy(clean.substr(second + 1));
					const std::size_t closed = rest.find("]] ");
					const std::size_t gap = rest.find(' ');
					if (atom.test == "between" && closed != std::string::npos) {
						atom.value = rest.substr(0, closed + 2);
						atom.value2 = trimmed_copy(rest.substr(closed + 3));
					} else if (atom.test == "between" && gap != std::string::npos) {
						atom.value = rest.substr(0, gap);
						atom.value2 = trimmed_copy(rest.substr(gap + 1));
					} else {
						atom.value = rest;
					}
				}
				atoms.push_back(std::move(atom));
			}
			node.where = atoms;
			continue;
		}
		OpOptions options;
		for (const std::string &pair : split_top(part, ',')) {
			const std::size_t equals = pair.find('=');
			if (equals == std::string::npos) continue;
			const std::string name = trimmed_copy(pair.substr(0, equals));
			const std::string value = trimmed_copy(pair.substr(equals + 1));
			if (name == "unit") options.unit = value;
			else if (name == "calendar") options.calendar = value;
			else if (name == "places" && digits_only(value)) options.places = std::stoi(value);
			else if (name == "rounding") options.rounding = value;
			else if (name == "fn") options.fn = value;
			else if (name == "mode") options.mode = value;
			else if (name == "reveal") options.reveal = value == "true";
			else if (name == "anchor") options.anchor = value;
		}
		node.options = options;
	}
	return node;
}

// The symbol a fact key has in this table now: a derived fact's, or its row's.
std::string symbol_of_fact(const OperationProtectedSymbolTable &table, const std::string &factKey) {
	for (const auto &[symbol, fact] : table.derived_facts)
		if (fact.fact_key == factKey) return symbol;
	const auto row = table.fact_by_key.find(factKey);
	if (row != table.fact_by_key.end() && row->second < table.mapping.size()) return table.mapping[row->second].placeholder;
	return std::string();
}

} // namespace

std::vector<std::string> restore_snapshot(OperationProtectedSymbolTable &table, const ComputeSnapshot &snapshot,
		OpContext &context) {
	// Counts come back as they were: a pause never refills a budget. Every
	// question the task had answered stays answered, so rebuilding it is free.
	ObservationLedger &ledger = table.observations;
	ledger.task_observations = std::max(ledger.task_observations, snapshot.task_observations);
	for (const auto &[root, count] : snapshot.task_by_root) ledger.task_by_root[root] = std::max(ledger.task_by_root[root], count);
	for (const auto &[group, count] : snapshot.task_by_group_key)
		ledger.task_by_group_key[group] = std::max(ledger.task_by_group_key[group], count);
	ledger.matter.answered.insert(snapshot.answered.begin(), snapshot.answered.end());
	if (table.provider_chain.empty()) table.provider_chain = snapshot.provider_chain;

	std::set<std::string> rebuilt;
	const std::size_t logBefore = table.op_log.size();
	for (std::size_t index = 0; index < snapshot.op_log.size(); ++index) {
		const SnapshotOp &entry = snapshot.op_log[index];
		if (entry.status != "ok" && entry.status != "bounded") continue;
		std::optional<OpNode> node = node_of(entry, index);
		if (!node) continue;
		// The operands the entry named, by fact key, as this table now names them.
		bool resolvable = true;
		for (std::size_t argument = 0; argument < node->args.size(); ++argument) {
			std::string &written = node->args[argument];
			if (written.rfind("[[", 0) != 0 || written.rfind("[[Table", 0) == 0 || written.rfind("[[Calendar", 0) == 0
					|| written.rfind("[[Schedule", 0) == 0)
				continue;
			const std::string factKey = argument < entry.operand_fact_keys.size() ? entry.operand_fact_keys[argument] : std::string();
			const std::string symbol = factKey.empty() ? std::string() : symbol_of_fact(table, factKey);
			if (symbol.empty()) {
				resolvable = false;
				break;
			}
			written = symbol;
			table.sent_symbols.insert(symbol);
		}
		if (!resolvable) continue;
		if (node->bind && table.compute_labels.count(*node->bind) != 0) continue;
		std::vector<OpAnswer> answers;
		try {
			answers = operations::execute_ops({*node}, context);
		} catch (const std::exception &) {
			answers.clear();
		}
		if (answers.empty() || !node->bind) continue;
		const OpAnswer &answer = answers.front();
		if (answer.status != OpStatus::Ok || answer.results.empty()) continue;
		const auto expected = snapshot.labels.find(*node->bind);
		const auto derived = table.derived_facts.find(answer.results.front());
		if (expected != snapshot.labels.end() && derived != table.derived_facts.end()
				&& derived->second.fact_key != expected->second) {
			// The material changed under the name: it is not the fact it was.
			table.compute_labels.erase(*node->bind);
			continue;
		}
		table.compute_labels[*node->bind] = answer.results.front();
		rebuilt.insert(*node->bind);
	}
	// The earlier calls' records were disclosed with those calls; the replay
	// asked nothing of the model and is not listed again.
	if (table.op_log.size() > logBefore) table.op_log.resize(logBefore);

	std::vector<std::string> lost;
	for (const auto &named : snapshot.labels)
		if (rebuilt.count(named.first) == 0 && table.compute_labels.count(named.first) == 0) lost.push_back(named.first);
	return lost;
}

} // namespace singularity::protected_api::papinian::compute
