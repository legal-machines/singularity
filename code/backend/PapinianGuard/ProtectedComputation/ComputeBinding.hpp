#pragma once

#include "PapinianGuard/ProtectedComputation/ComputeBudget.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeTypes.hpp"
#include "PapinianGuard/ProtectedMarkers/ProtectedMarkers.hpp"
#include "PapinianGuard/ProtectedOperations/ComputeExecutor.hpp"
#include "PapinianGuard/ProtectedOperations/ProtectedOperations.hpp"
#include "PapinianGuard/ProtectedOperations/TemporalOps.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The binding of the protected computation protocol for one protected call
// (DESIGN4 §1.1, §13.2 wave 2): ResearchToolLoop's `computeExecutor` - parse a
// round's ops, execute them over the guard's table (operations::execute_ops
// through compute_context(), lane X), write the lines and the results block
// (ComputeProtocol, lane K). Owned by lane K; header-inline so builds without
// PapinianGuardService.cpp still link (the link rule of ComputeExecutor.hpp).
namespace singularity::protected_api::papinian::compute {

// What an operation of one round runs with, over `table`: X's wiring of the
// guard (compute_context), and what the loop's prompt says - the payload as it
// is sent (the frozen mapping of the prompt's data: what the model reads, and
// all a constant may be grounded in), what each part of it is, the governing
// business-day rule and the raw passages, and Singularity's own words.
inline OpContext round_context(const PapinianGuard &guard, OperationProtectedSymbolTable &table, const Policy &policy,
		const std::string &given) {
	// An operation turn's prompt is a JSON envelope: read as its sections.
	const std::string prompt = data_plane_text(given);
	OpContext context = operations::compute_context(guard, table, policy);
	// Nothing sent, nothing open: a prompt that cannot be mapped grounds no
	// constant at all.
	try {
		context.sent = guard.apply_frozen_mapping(prompt, table);
	} catch (const std::exception &) {
		context.sent.clear();
	}
	context.segments = payload_segments(context.sent);
	context.rules = operations::governing_rules_of(prompt);
	auto passages = std::make_shared<std::vector<business_calendar::RulePassage>>(operations::rule_passages(prompt));
	auto matter = std::make_shared<std::optional<business_calendar::BusinessDayRule>>(operations::lawyer_rule_in(prompt));
	context.rules_for = [passages, matter](const std::vector<std::string> &writings) {
		business_calendar::GoverningInput input;
		input.passages = *passages;
		input.matter_rule = *matter;
		input.operand_writings = writings;
		return business_calendar::governing_rule(input);
	};
	for (const business_calendar::RulePassage &passage : *passages) context.raw_passages.push_back(passage.text);
	context.language = semantic::language_of(operations::prompt_section(prompt, "CURRENT_REQUEST"));
	// Which party each fact belongs to, only when a relation needs it: read
	// once, from what is sent (exactly as the cycle-3 executor reads it).
	if (!context.party_of) {
		auto sent = std::make_shared<std::string>(context.sent);
		auto parties = std::make_shared<std::optional<std::map<std::string, std::string>>>();
		context.party_of = [parties, sent, &table](const RequestMappingEntry &entry) -> std::string {
			if (!*parties) {
				std::map<std::string, std::string> byMarker;
				const std::vector<RequestMappingEntry> rows = table.mapping;
				relations::Input input;
				input.mapping = &rows;
				if (!sent->empty()) input.passages = {*sent};
				for (const auto &[row, party] : relations::parties_of(input))
					if (!party.empty() && row < rows.size()) byMarker[rows[row].placeholder] = party;
				*parties = std::move(byMarker);
			}
			const auto found = (*parties)->find(entry.placeholder);
			return found == (*parties)->end() ? std::string() : found->second;
		};
	}
	context.render = [](singularity::context::control_text::Template id,
			const std::vector<singularity::context::control_text::Slot> &slots) {
		return singularity::context::control_text::render(id, slots);
	};
	context.task = table.operation_id;
	return context;
}

// The evidence a prompt's tables are described by (DESIGN4 §6.2, D14): its
// SOURCE_MATERIAL, each passage under its [SOURCE:DOC_n:CHUNK_m] marker - the
// sheets and columns it reads are the ones the tables block describes.
inline std::vector<std::string> table_evidence(const std::string &prompt) {
	const std::string sources = operations::prompt_section(data_plane_text(prompt), "SOURCE_MATERIAL");
	if (sources.empty()) return {};
	return {sources};
}

// The business-day calendars and band schedules of an operation's material
// (DESIGN4 §6.6, §7.3), each built once: calendars from the rule passages the
// prompt holds (text alone), schedules once the registry holds the facts their
// thresholds and band values are - after the first call's protection. `prompt`
// is the local prompt (its sections, or the JSON envelope that carries them).
inline void prepare_calendars_and_schedules(OperationProtectedSymbolTable &table, const std::string &prompt) {
	if (!table.calendars.empty() && !table.schedules.empty()) return;
	const std::string text = data_plane_text(prompt);
	const std::vector<business_calendar::RulePassage> passages = operations::rule_passages(text);
	if (table.calendars.empty()) {
		const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm local{};
#if defined(_WIN32)
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif
		try {
			table.calendars = calendar_handles(passages, operations::lawyer_rule_in(text), local.tm_year + 1900);
		} catch (const std::exception &) {
			table.calendars.clear();
		}
	}
	if (table.schedules.empty() && !table.mapping.empty()) {
		// Band schedules are the documents' own words, never a Project record's
		// structure (a JSON record once read as "≥ 9, < 26 → 8").
		std::vector<business_calendar::RulePassage> documents;
		for (const business_calendar::RulePassage &passage : passages)
			if (!passage.matter_record) documents.push_back(passage);
		try {
			table.schedules = tables::schedules_in(documents, table);
		} catch (const std::exception &) {
			table.schedules.clear();
		}
	}
}

// The research loop's compute executor over `table` (which outlives it: the
// operation's own table, shared by every call of the attempt). An empty
// function would mean the protocol is not offered; this one always serves.
inline std::function<ComputeRoundOutput(const ComputeRoundInput &)> compute_executor(
		const PapinianGuard &guard, OperationProtectedSymbolTable &table, const Policy &policy,
		const ComputeBudgetLimits &limits = {}) {
	return [&guard, &table, policy, limits](const ComputeRoundInput &input) {
		if (input.completeness_check) {
			ComputeRoundOutput verdict;
			if (const std::optional<std::string> repair = completeness_repair(input.final_text, table, input.lawyer_question_offered,
					input.operations_open))
				verdict.local_block = *repair;
			return verdict;
		}
		OpContext context = round_context(guard, table, policy, input.prompt);
		ComputeRoundOutput output = serve_round(input, table, std::move(context), limits,
			[](const std::vector<OpNode> &ops, OpContext &runWith) { return operations::execute_ops(ops, runWith); });
		output.disclosure = disclosure_details(table.observations);
		return output;
	};
}

// `text` as a durable pause keeps it (DESIGN4 §1.10): each symbol this table
// issued written as its local anchor, ⟪writing|fact:key⟫, which a resumed run -
// numbering its symbols afresh - protects as the same fact again. A symbol
// with no fact key is written as its writing (protection masks it again); one
// the table never issued (a handle) is left as it stands. Local only.
inline std::string localized_for_pause(const std::string &text, const OperationProtectedSymbolTable &table) {
	namespace marks = fact_anchor_marks;
	std::string kept;
	std::size_t cursor = 0;
	for (const markers::Parsed &parsed : markers::find_all(text)) {
		if (parsed.start < cursor || !parsed.canonical) continue;
		const std::string placeholder = "[[" + parsed.base + "]]";
		const auto row = std::find_if(table.mapping.begin(), table.mapping.end(),
			[&](const RequestMappingEntry &entry) { return entry.placeholder == placeholder; });
		if (row == table.mapping.end() || row->original.empty()) continue;
		kept.append(text, cursor, parsed.start - cursor);
		if (marks::is_key(row->fact_key))
			kept += std::string(marks::kOpen) + row->original + std::string(marks::kKey) + row->fact_key
				+ std::string(marks::kClose);
		else
			kept += row->original;
		cursor = parsed.start + parsed.length;
	}
	kept.append(text, cursor, std::string::npos);
	return kept;
}

// Where a resumed run's snapshot waits until its material is protected again,
// and the labels it could not rebuild wait for the next results block
// (DESIGN4 §1.10). Value-free, in memory, on the operation's own table.
inline constexpr const char *kPendingSnapshotKey = "compute:pending_snapshot";
inline constexpr const char *kLostLabelsKey = "compute:lost_labels";

// A resumed run's operations rebuilt (DESIGN4 §1.10): once the first protected
// call of the resumed run has read and protected its material - before the
// mapping is frozen, so the derived facts the loop's anchors name are rows of
// this table for that very call and its answer contract - the snapshot's op
// log is re-executed over it (restore_snapshot, lane X), the task's counts
// come back as they were (never refilled by a pause), and the names that could
// not be rebuilt are kept for the next results block. Nothing pending,
// nothing done. Never throws.
inline void restore_pending(const PapinianGuard &guard, OperationProtectedSymbolTable &table, const Policy &policy,
		const std::string &prompt) {
	const auto pending = table.model_authored.find(kPendingSnapshotKey);
	if (pending == table.model_authored.end()) return;
	const std::optional<ComputeSnapshot> snapshot = snapshot_from_json(pending->second);
	table.model_authored.erase(pending);
	if (!snapshot) return;
	table.compute_rounds_used = std::max(table.compute_rounds_used, snapshot->compute_rounds_used);
	table.ops_used = std::max(table.ops_used, snapshot->ops_used);
	table.repair_rounds_used = std::max(table.repair_rounds_used, snapshot->repair_rounds_used);
	std::vector<std::string> lost;
	try {
		OpContext context = round_context(guard, table, policy, prompt);
		lost = restore_snapshot(table, *snapshot, context);
	} catch (const std::exception &) {
		// Nothing could be rebuilt: every name is lost, and said so.
		lost.clear();
		for (const auto &named : snapshot->labels) lost.push_back(named.first);
	}
	std::string kept;
	for (const std::string &label : lost) kept += (kept.empty() ? "" : "\n") + label;
	if (!kept.empty()) table.model_authored[kLostLabelsKey] = kept;
}

// The names a resumed run could not rebuild, said once (in the next results
// block).
inline std::vector<std::string> take_lost_labels(OperationProtectedSymbolTable &table) {
	std::vector<std::string> labels;
	const auto lost = table.model_authored.find(kLostLabelsKey);
	if (lost == table.model_authored.end()) return labels;
	std::size_t at = 0;
	const std::string &text = lost->second;
	while (at <= text.size()) {
		const std::size_t end = std::min(text.find('\n', at), text.size());
		if (end > at) labels.push_back(text.substr(at, end - at));
		at = end + 1;
	}
	table.model_authored.erase(lost);
	return labels;
}

// Everything the research loop runs the protocol with, over one operation's
// `table` (DESIGN4 §1.1, §1.10, §8.7, D13): the round executor; the value-free
// snapshot a durable pause keeps and its restoring on a resumed run (applied
// when the resumed run's first round arrives, once its material has been
// protected again: its names are then rebuilt from what the material now
// reads); the loop's text localized for a pause; query admission; and the last
// reply as the provider wrote it. `data_plane` gives the part of the loop's
// prompt that is not Singularity's instruction (routing's
// without_instruction_sections): the executor and admission read only it.
struct LoopBinding {
	std::function<ComputeRoundOutput(const ComputeRoundInput &)> executor;
	std::function<nlohmann::json()> snapshot;
	std::function<std::vector<std::string>(const nlohmann::json &)> resume;
	std::function<std::string(const std::string &)> localize;
	std::function<Code(const std::string &, bool, const std::string &)> admission;
	std::function<std::optional<nlohmann::json>()> reply;
	// The model's own bytes as a verbatim span the next call sends unchanged
	// (DESIGN4 §8.7).
	std::function<std::string(const std::string &)> verbatim;
};

// QUERY ADMISSION (DESIGN4 §8.7) over one operation's `table`: a search the
// model writes is judged as written, against the material as the provider was
// sent it. Every call that goes out protected is held to it, with or without
// the protocol's budgets - a model whose window is too small for the protocol
// writes searches all the same. `data_plane` as for loop_binding.
inline std::function<Code(const std::string &, bool, const std::string &)> query_admission(const PapinianGuard &guard,
		OperationProtectedSymbolTable &table, std::function<std::string(const std::string &)> data_plane = {}) {
	const auto plane = [data_plane](const std::string &prompt) {
		const std::string text = data_plane_text(prompt);
		return data_plane ? data_plane(text) : text;
	};
	return [&guard, &table, plane](const std::string &query, bool web, const std::string &prompt) {
		std::string sent;
		try {
			sent = guard.apply_frozen_mapping(plane(prompt), table);
		} catch (const std::exception &) {
			sent.clear();
		}
		return operations::admit_query(query, sent, payload_segments(sent), web);
	};
}

// The last reply as the provider wrote it (DESIGN4 D13), kept on `table` by
// each protected call: what admission judges a search by, and what an echo of
// the model's own request is written from - never what restoration made of it.
inline std::function<std::optional<nlohmann::json>()> provider_reply(OperationProtectedSymbolTable &table) {
	return [&table]() -> std::optional<nlohmann::json> {
		const auto written = table.model_authored.find("reply");
		if (written == table.model_authored.end()) return std::nullopt;
		nlohmann::json value = nlohmann::json::parse(written->second, nullptr, false);
		if (!value.is_object()) return std::nullopt;
		return value;
	};
}

inline LoopBinding loop_binding(const PapinianGuard &guard, OperationProtectedSymbolTable &table, const Policy &policy,
		const ComputeBudgetLimits &limits, std::function<std::string(const std::string &)> data_plane = {}) {
	LoopBinding binding;
	const auto plane = [data_plane](const std::string &prompt) {
		const std::string text = data_plane_text(prompt);
		return data_plane ? data_plane(text) : text;
	};
	binding.executor = [&guard, &table, policy, limits, plane](const ComputeRoundInput &given) {
		// The completeness gate: nothing is executed, only read.
		if (given.completeness_check) {
			ComputeRoundOutput verdict;
			if (const std::optional<std::string> repair = completeness_repair(given.final_text, table, given.lawyer_question_offered,
					given.operations_open))
				verdict.local_block = *repair;
			return verdict;
		}
		ComputeRoundInput input = given;
		input.prompt = plane(given.prompt);
		// A snapshot no protected call has rebuilt yet (a resumed run whose
		// first call was not protected) is rebuilt here, before the round.
		restore_pending(guard, table, policy, input.prompt);
		const std::vector<std::string> lost = take_lost_labels(table);
		OpContext context = round_context(guard, table, policy, input.prompt);
		ComputeRoundOutput output = serve_round(input, table, std::move(context), limits,
			[](const std::vector<OpNode> &ops, OpContext &runWith) { return operations::execute_ops(ops, runWith); });
		const std::string lostText = labels_lost_text(lost);
		if (!lostText.empty()) output.local_block = lostText + output.local_block;
		output.disclosure = disclosure_details(table.observations);
		return output;
	};
	binding.snapshot = [&table]() {
		return nlohmann::json::parse(snapshot_json(snapshot(table)), nullptr, false);
	};
	// Kept until the resumed run's first protected call has protected its
	// material again (restore_pending, called by that call's protection and,
	// failing that, before the first round); the names it cannot rebuild are
	// said in the next results block.
	binding.resume = [&table](const nlohmann::json &state) {
		if (snapshot_from_json(state.dump())) table.model_authored[kPendingSnapshotKey] = state.dump();
		return std::vector<std::string>();
	};
	// A pause keeps the model's words as plain text: a resumed run protects
	// them again (its table is new, and knows no span).
	binding.localize = [&table](const std::string &text) {
		return localized_for_pause(neutralize_verbatim(text, nullptr), table);
	};
	binding.verbatim = [&table](const std::string &bytes) { return register_verbatim(table, bytes); };
	binding.admission = query_admission(guard, table, data_plane);
	binding.reply = provider_reply(table);
	return binding;
}

} // namespace singularity::protected_api::papinian::compute
