#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "PapinianGuard/ProtectedMarkers/ProtectedMarkers.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"
#include "Routing/ProtectedInference/TextualAnswer.hpp"
#include "Routing/Types/ModelVisibleTurn.hpp"

// What one protected call carried of the protected computation protocol, read
// from exactly what it sent (DESIGN4 §11.2): the role lines and table blocks
// of its legend, and each operation result it returned to the model - with
// the operation as the model asked for it. Symbols, closed words, codes and
// where facts are stated; never a value (PrivacyMetadata.hpp). And the string
// fields of a model's answer the answer contract reads, and puts back.
//
// Header-only: read by the provider boundary (ProtectedInference.cpp) and its
// tests.
namespace singularity::routing::compute_disclosure {

namespace papinian = protected_api::papinian;

// ---------------------------------------------------------------------------
// The text a call sent, as the model reads it: every string of its data
// plane - the prompt, the passages - and every string of its legend, one after
// another. What a line "was sent" means: it stands in here.

inline void strings_of(const nlohmann::json &value, std::string &out) {
	if (value.is_string()) {
		out += value.get_ref<const std::string &>();
		out += '\n';
		return;
	}
	if (value.is_object())
		for (auto entry = value.begin(); entry != value.end(); ++entry) strings_of(entry.value(), out);
	if (value.is_array())
		for (const nlohmann::json &entry : value) strings_of(entry, out);
}

inline std::string sent_text(const ModelVisibleTurn &turn) {
	std::string text;
	strings_of(turn.data.value, text);
	if (!turn.data.protection_metadata.is_null()) strings_of(turn.data.protection_metadata, text);
	return text;
}

// The symbols a call gave the model as facts: every symbol of what it sent,
// but for the examples Singularity's own instructions write ("[[Amount3]]",
// "[[Person1.first]]" in the legend's instruction and in the prompt's
// instruction sections), which name no fact of this request.
inline std::set<std::string> symbols_sent(const ModelVisibleTurn &turn) {
	std::string text;
	strings_of(turn.data.value, text);
	for (const char *section : {"SYSTEM", "TASK", "CAPABILITY", "SOURCE_COVERAGE", "TEMPORAL_CONTEXT", "REQUEST_COVERAGE"}) {
		const std::string opening = std::string("<<<LM_SECTION:") + section + ">>>\n";
		const std::string closing = std::string("\n<<<LM_END_SECTION:") + section + ">>>";
		for (std::size_t at = text.find(opening); at != std::string::npos; at = text.find(opening, at + 1)) {
			const std::size_t end = text.find(closing, at + opening.size());
			if (end == std::string::npos) break;
			text.erase(at + opening.size(), end - at - opening.size());
		}
	}
	if (turn.data.protection_metadata.is_object())
		for (auto entry = turn.data.protection_metadata.begin(); entry != turn.data.protection_metadata.end(); ++entry)
			if (entry.key() != "instruction") strings_of(entry.value(), text);
	std::set<std::string> symbols;
	for (const papinian::markers::Parsed &parsed : papinian::markers::find_all(text))
		if (text.compare(parsed.start, 2, "[[") == 0) symbols.insert("[[" + parsed.base + "]]");
	return symbols;
}

// The text the answer contract reads (AnswerContract check()'s sent text): all
// a call sent, laid out so that what grounds a figure the answer writes openly
// (DESIGN4 §8.2) is told apart by PromptAssembler's own sections - Singularity's
// own words and legend first, where they ground nothing; a typed operation
// turn's material in a section of its own; then each prompt with its sections
// and what the loop appended to it, whose segments (ComputeProtocol
// payload_segments) say which part is the lawyer's material and which is the
// model's own words sent back.
inline bool singularity_own_key(const std::string &key) {
	static const std::set<std::string> own{"runtimeInstruction", "turnInstruction", "localComputation", "instruction",
		"message", "validationIssue", "outstandingIssue", "issue", "reason", "dispositionReason", "note", "draft", "drafts",
		"query", "question", "lawyerQuestion", "review", "verdict", "protocol"};
	return own.count(key) > 0;
}

inline void contract_parts(const nlohmann::json &value, bool own, std::string &lmOwn, std::string &material,
		std::string &sectioned) {
	if (value.is_string()) {
		const std::string &text = value.get_ref<const std::string &>();
		if (text.find("<<<LM_SECTION:") != std::string::npos) sectioned += text + "\n";
		else if (own) lmOwn += text + "\n";
		else material += "<<<LM_SECTION:PROJECT_CONTEXT>>>\n" + text + "\n<<<LM_END_SECTION:PROJECT_CONTEXT>>>\n";
		return;
	}
	if (value.is_object())
		for (auto entry = value.begin(); entry != value.end(); ++entry)
			contract_parts(entry.value(), own || singularity_own_key(entry.key()), lmOwn, material, sectioned);
	if (value.is_array())
		for (const nlohmann::json &entry : value) contract_parts(entry, own, lmOwn, material, sectioned);
}

inline std::string contract_text(const ModelVisibleTurn &turn) {
	std::string lmOwn;
	std::string material;
	std::string sectioned;
	contract_parts(turn.data.value, false, lmOwn, material, sectioned);
	if (!turn.data.protection_metadata.is_null()) strings_of(turn.data.protection_metadata, lmOwn);
	return lmOwn + material + sectioned;
}

// ---------------------------------------------------------------------------
// Where a fact is stated, as a record names it (RelationSource).

inline papinian::RelationSource source_of(const papinian::RequestMappingEntry &row) {
	papinian::RelationSource source;
	source.marker = row.placeholder;
	source.origin = row.derived ? std::string("derived") : std::string(papinian::to_string(row.provenance.origin));
	source.passage_sha256 = row.provenance.passage_sha256;
	source.offset = row.provenance.offset;
	source.length = row.provenance.length;
	source.cell = row.provenance.cell;
	return source;
}

inline const papinian::RequestMappingEntry *row_named(const papinian::OperationProtectedSymbolTable &table,
		const std::string &symbol) {
	for (const papinian::RequestMappingEntry &row : table.mapping)
		if (row.placeholder == symbol) return &row;
	return nullptr;
}

inline std::string category_of(const papinian::RequestMappingEntry &row) {
	return row.type ? std::string(papinian::to_string(*row.type))
		: std::string(row.source == papinian::DetectionSource::CustomTerm ? "CUSTOM_TERM" : "PROTECTED_VALUE");
}

// ---------------------------------------------------------------------------
// Role lines (DESIGN4 §5.4): "[[Amount3]] MONEY(USD/month) PRICE "Base Rent"
// · DOC_1 §4.1". A line that names nothing but a symbol's type is not a role.

struct RoleLine {
	std::string symbol;
	std::string type;
	std::string role_class;
	std::string words;
	std::string location;
};

inline RoleLine read_role_line(const std::string &line) {
	RoleLine read;
	std::size_t at = line.find(' ');
	read.symbol = line.substr(0, at);
	if (at == std::string::npos) return read;
	++at;
	// The type, with its bracketed tail.
	std::size_t end = at;
	std::size_t depth = 0;
	while (end < line.size() && (depth > 0 || line[end] != ' ')) {
		if (line[end] == '(') ++depth;
		if (line[end] == ')' && depth > 0) --depth;
		++end;
	}
	read.type = line.substr(at, end - at);
	at = end < line.size() ? end + 1 : end;
	// A closed role class in capitals.
	end = at;
	while (end < line.size() && ((line[end] >= 'A' && line[end] <= 'Z') || line[end] == '_')) ++end;
	if (end > at && (end == line.size() || line[end] == ' ')) {
		read.role_class = line.substr(at, end - at);
		at = end < line.size() ? end + 1 : end;
	}
	const std::size_t dot = line.find(" \xC2\xB7 ", at);
	read.words = line.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
	if (dot != std::string::npos) read.location = line.substr(dot + 4);
	while (!read.words.empty() && read.words.back() == ' ') read.words.pop_back();
	return read;
}

// The role lines of a call's legend that its sent text carries, each with the
// relation sensitivity of joining its fact to a party it names.
inline std::vector<papinian::OutboundRole> roles_sent(const nlohmann::json &legend, const std::string &sent,
		const papinian::OperationProtectedSymbolTable &table, const protected_api::RelationPolicy &policy) {
	std::vector<papinian::OutboundRole> roles;
	if (!legend.is_object()) return roles;
	std::set<std::string> seen;
	for (const char *key : {"symbols", "roles"}) {
		const auto list = legend.find(key);
		if (list == legend.end() || !list->is_array()) continue;
		for (const nlohmann::json &entry : *list) {
			if (!entry.is_string()) continue;
			const std::string &line = entry.get_ref<const std::string &>();
			if (sent.find(line) == std::string::npos || !seen.insert(line).second) continue;
			const RoleLine read = read_role_line(line);
			if (read.role_class.empty() && read.words.empty()) continue;
			papinian::OutboundRole role;
			role.symbol = read.symbol;
			role.line = line;
			role.source = read.location;
			role.role_class = read.role_class;
			// The fact's own category, and the party symbols its words name.
			protected_api::RelationFacts facts;
			facts.kind = "role";
			const papinian::RequestMappingEntry *row = row_named(table, read.symbol);
			if (row != nullptr) {
				role.value_sensitivity = category_of(*row);
				facts.operand_categories.push_back(role.value_sensitivity);
				facts.operand_parties.push_back(std::string());
			}
			for (const papinian::markers::Parsed &parsed : papinian::markers::find_all(read.words)) {
				const std::string party = "[[" + parsed.base + "]]";
				const papinian::RequestMappingEntry *named = row_named(table, party);
				if (named == nullptr || named->value) continue;
				facts.operand_categories.push_back(category_of(*named));
				facts.operand_parties.push_back(party);
			}
			role.relation_sensitivity = protected_api::to_string(protected_api::classify_relation(facts, policy));
			roles.push_back(std::move(role));
		}
	}
	return roles;
}

// ---------------------------------------------------------------------------
// Table blocks (DESIGN4 §6.4): each block of the legend's tables the sent
// text carries, by its handle, and the describe_tables that asked for it.

inline std::vector<papinian::OutboundTable> tables_sent(const nlohmann::json &legend, const std::string &sent,
		const papinian::OperationProtectedSymbolTable &table) {
	std::vector<papinian::OutboundTable> blocks;
	if (!legend.is_object()) return blocks;
	const auto list = legend.find("tables");
	if (list == legend.end() || !list->is_array()) return blocks;
	for (const nlohmann::json &entry : *list) {
		std::string block;
		if (entry.is_string()) block = entry.get<std::string>();
		else if (entry.is_object() && entry.contains("block") && entry.at("block").is_string()) block = entry.at("block").get<std::string>();
		if (block.empty() || sent.find(block) == std::string::npos) continue;
		papinian::OutboundTable disclosed;
		disclosed.block = block;
		for (const papinian::markers::Parsed &parsed : papinian::markers::find_all(block)) {
			const std::string handle = "[[" + parsed.base + "]]";
			if (handle.rfind("[[Table", 0) != 0) continue;
			disclosed.handle = handle;
			break;
		}
		for (const papinian::compute::OpRecord &record : table.op_log) {
			if (record.request_canonical.rfind("describe_tables", 0) != 0 || record.status != "ok") continue;
			if (disclosed.handle.empty() || record.result_line.find(disclosed.handle) == std::string::npos) continue;
			disclosed.requested_by_op = record.op_id;
			disclosed.justification = record.justification;
		}
		blocks.push_back(std::move(disclosed));
	}
	return blocks;
}

// ---------------------------------------------------------------------------
// Operations (DESIGN4 §2.3, §11.1).

// Where the facts an operation reached are stated: every root of every value
// it worked out, and every fact of the material it read directly.
inline std::vector<papinian::RelationSource> operation_sources(const papinian::compute::OpRecord &record,
		const papinian::OperationProtectedSymbolTable &table) {
	std::vector<papinian::RelationSource> sources;
	std::set<std::pair<std::string, std::size_t>> seen;
	const auto add = [&](const papinian::RelationSource &source) {
		const std::string place = source.passage_sha256.empty() ? source.marker : source.passage_sha256 + "|" + source.cell;
		if (!seen.insert({place, source.offset}).second) return;
		sources.push_back(source);
	};
	const auto add_symbol = [&](const std::string &symbol) {
		const auto derived = table.derived_facts.find(symbol);
		if (derived != table.derived_facts.end()) {
			for (const papinian::RelationSource &source : derived->second.root_sources) add(source);
			return;
		}
		const papinian::RequestMappingEntry *row = row_named(table, symbol);
		if (row != nullptr && !row->derived) add(source_of(*row));
	};
	for (const std::string &symbol : record.results) add_symbol(symbol);
	for (const std::string &symbol : record.operand_symbols) add_symbol(symbol);
	for (const std::string &key : record.operand_fact_keys) {
		const auto found = table.fact_by_key.find(key);
		if (found == table.fact_by_key.end() || found->second >= table.mapping.size()) continue;
		const papinian::RequestMappingEntry &row = table.mapping[found->second];
		if (!row.derived) add(source_of(row));
	}
	return sources;
}

inline papinian::OutboundOperation operation_of(const papinian::compute::OpRecord &record,
		const papinian::OperationProtectedSymbolTable &table) {
	papinian::OutboundOperation operation;
	operation.op_id = record.op_id;
	operation.requested = record.request_canonical;
	operation.status = record.status;
	operation.code = record.code;
	operation.returned = record.result_line;
	operation.results = record.results;
	operation.observation = record.observation;
	operation.sources = operation_sources(record, table);
	operation.justification = record.justification;
	for (const std::string &symbol : record.results) {
		const auto derived = table.derived_facts.find(symbol);
		if (derived == table.derived_facts.end()) continue;
		if (operation.value_sensitivity.empty()) operation.value_sensitivity = derived->second.value_sensitivity;
		if (derived->second.relation_sensitivity == "sensitive") operation.relation_sensitivity = "sensitive";
	}
	if (operation.value_sensitivity.empty())
		for (const std::string &symbol : record.operand_symbols) {
			const papinian::RequestMappingEntry *row = row_named(table, symbol);
			if (row == nullptr) continue;
			operation.value_sensitivity = category_of(*row);
			break;
		}
	return operation;
}

// An operation as a record keeps it: what the model asked and why, through
// `symbolic` - the provider boundary's own masking, which writes any protected
// value the model's literal or its why happens to write as that value's symbol
// (PrivacyMetadata: never a value Papinian Guard kept) - and the line it was
// told exactly as sent.
using Symbolic = std::function<std::string(const std::string &)>;

inline papinian::OutboundOperation disclosed_operation(const papinian::compute::OpRecord &record,
		const papinian::OperationProtectedSymbolTable &table, const Symbolic &symbolic) {
	papinian::OutboundOperation operation = operation_of(record, table);
	if (symbolic) {
		operation.requested = symbolic(operation.requested);
		operation.justification = symbolic(operation.justification);
	}
	return operation;
}

// Every operation the table holds, as the call whose response asked it lists
// it (outbound::note_operations_requested): refused and erroring ones too, and
// ones no later call returned.
// `listed`: the ones already listed, left out.
inline std::vector<papinian::OutboundOperation> operations_held(const papinian::OperationProtectedSymbolTable &table,
		const Symbolic &symbolic, const std::set<std::string> &listed = {}) {
	std::vector<papinian::OutboundOperation> operations;
	for (const papinian::compute::OpRecord &record : table.op_log)
		if (!record.op_id.empty() && listed.count(record.op_id) == 0)
			operations.push_back(disclosed_operation(record, table, symbolic));
	return operations;
}

// Each operation whose result line this call's sent text carries (DESIGN4
// §11.2: every result line present in the body, on every call that carries
// it - a later call of the attempt that sends the results block again lists
// them again). The first call to carry a line is marked on the op log
// (`payload`).
inline std::vector<papinian::OutboundOperation> returned_in(papinian::OperationProtectedSymbolTable &table,
		const std::string &sent, const std::string &payload, const Symbolic &symbolic = {}) {
	std::vector<papinian::OutboundOperation> returned;
	for (papinian::compute::OpRecord &record : table.op_log) {
		if (record.result_line.empty() || sent.find(record.result_line) == std::string::npos) continue;
		if (record.received_by_call.empty()) record.received_by_call = payload.empty() ? std::string("sent") : payload;
		returned.push_back(disclosed_operation(record, table, symbolic));
	}
	return returned;
}

// ---------------------------------------------------------------------------
// The string fields of a model's answer, as the answer contract reads them.

inline std::string pointer_part(std::string value) {
	std::size_t position = 0;
	while ((position = value.find('~', position)) != std::string::npos) {
		value.replace(position, 1, "~0");
		position += 2;
	}
	position = 0;
	while ((position = value.find('/', position)) != std::string::npos) {
		value.replace(position, 1, "~1");
		position += 2;
	}
	return value;
}

// Each string field of `value` with its JSON pointer - as restoration names
// them, a textual answer's embedded one included - except what the model asks
// Singularity to work out (localFacts: the symbol plane).
inline void each_field(const nlohmann::json &value, const std::string &pointer,
		const std::function<void(const std::string &, const std::string &)> &visit) {
	const std::size_t facts = pointer.find("/localFacts");
	if (facts != std::string::npos && (facts + 11 == pointer.size() || pointer[facts + 11] == '/')) return;
	// Nor a textual-protocol call of Singularity's compute tool: its arguments are
	// operations over symbols, never the lawyer's work (DESIGN4 §1.2).
	if (value.is_object() && value.contains("tool") && value.at("tool").is_string()
			&& papinian::compute::is_compute_tool(value.at("tool").get<std::string>()))
		return;
	if (value.is_string()) {
		const std::string &text = value.get_ref<const std::string &>();
		if (const std::optional<nlohmann::json> embedded = textual_answer::embedded_answer(text)) {
			each_field(*embedded, pointer, visit);
			return;
		}
		visit(pointer, text);
		return;
	}
	if (value.is_object())
		for (auto entry = value.begin(); entry != value.end(); ++entry)
			each_field(entry.value(), pointer + "/" + pointer_part(entry.key()), visit);
	if (value.is_array()) {
		std::size_t index = 0;
		for (const nlohmann::json &entry : value) each_field(entry, pointer + "/" + std::to_string(index++), visit);
	}
}

// `value` with each field `replaced` names (by pointer) put in its place, an
// embedded answer written back as the string it came in.
inline nlohmann::json with_fields(const nlohmann::json &value, const std::string &pointer,
		const std::map<std::string, std::string> &replaced) {
	if (value.is_string()) {
		const auto found = replaced.find(pointer);
		if (found != replaced.end()) return found->second;
		const std::string &text = value.get_ref<const std::string &>();
		if (const std::optional<nlohmann::json> embedded = textual_answer::embedded_answer(text)) {
			const nlohmann::json rebuilt = with_fields(*embedded, pointer, replaced);
			return rebuilt == *embedded ? value : nlohmann::json(rebuilt.dump());
		}
		return value;
	}
	if (value.is_object()) {
		nlohmann::json result = nlohmann::json::object();
		for (auto entry = value.begin(); entry != value.end(); ++entry)
			result[entry.key()] = with_fields(entry.value(), pointer + "/" + pointer_part(entry.key()), replaced);
		return result;
	}
	if (value.is_array()) {
		nlohmann::json result = nlohmann::json::array();
		std::size_t index = 0;
		for (const nlohmann::json &entry : value) result.push_back(with_fields(entry, pointer + "/" + std::to_string(index++), replaced));
		return result;
	}
	return value;
}

// Where a field stands for the lawyer: a document's content (an artifact, a
// cell, a proposed change) keeps its sentences and shows a gap; anything else
// is the chat answer.
inline bool artifact_field(const std::string &pointer) {
	for (const char *part : {"/artifactAction", "/workProduct", "/cells/", "/operations/", "/content", "/replacementText",
			"/newValue", "/table"})
		if (pointer.find(part) != std::string::npos) return true;
	return false;
}

} // namespace singularity::routing::compute_disclosure
