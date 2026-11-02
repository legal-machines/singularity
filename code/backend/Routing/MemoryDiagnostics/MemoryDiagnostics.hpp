#pragma once

// Temporary Phase 24.1D offline production-path diagnostic. The only target
// that defines SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC is the EXCLUDE_FROM_ALL
// replay executable; normal product and test builds compile to no-ops.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace singularity::routing::memory_diagnostics {

enum class PromptRepresentationKind {
	RawLoopPrompt,
	ModelVisibleTurnSerialized,
	ProviderEnvelope,
};

inline const char *to_string(PromptRepresentationKind kind) {
	switch (kind) {
		case PromptRepresentationKind::RawLoopPrompt: return "RawLoopPrompt";
		case PromptRepresentationKind::ModelVisibleTurnSerialized: return "ModelVisibleTurnSerialized";
		case PromptRepresentationKind::ProviderEnvelope: return "ProviderEnvelope";
	}
	return "Unknown";
}

class PromptGrowthBaselines final {
public:
	void observe(PromptRepresentationKind kind, std::size_t bytes) {
		Slot &slot = slot_for(kind);
		if (slot.observed && bytes > slot.bytes * 2)
			throw std::runtime_error("PRODMEMDIAG assertion: next_prompt_bytes > previous_prompt_bytes * 2");
		slot.observed = true;
		slot.bytes = bytes;
	}

	std::size_t previous(PromptRepresentationKind kind) {
		const Slot &slot = slot_for(kind);
		return slot.observed ? slot.bytes : 0;
	}

private:
	struct Slot {
		bool observed = false;
		std::size_t bytes = 0;
	};

	Slot &slot_for(PromptRepresentationKind kind) {
		switch (kind) {
			case PromptRepresentationKind::RawLoopPrompt: return raw_loop_prompt_;
			case PromptRepresentationKind::ModelVisibleTurnSerialized: return model_visible_turn_serialized_;
			case PromptRepresentationKind::ProviderEnvelope: return provider_envelope_;
		}
		throw std::logic_error("Unknown prompt representation kind.");
	}

	const Slot &slot_for(PromptRepresentationKind kind) const {
		return const_cast<PromptGrowthBaselines *>(this)->slot_for(kind);
	}

	Slot raw_loop_prompt_;
	Slot model_visible_turn_serialized_;
	Slot provider_envelope_;
};

// A live-run progress stream - deliberately separate from checkpoint()/
// assert_prompt_growth() below: those are a controlled-fixture diagnostic
// (compiled only into the offline replay target, with throwing assertions
// tuned to that fixture's own predictable growth) never meant to run
// against a real live acceptance's genuinely variable data. progress()
// carries no assertion, ever - purely observational - and compiles into
// EVERY build, but only actually prints when explicitly opted into at
// runtime (SINGULARITY_VERBOSE_PROGRESS=1), so an ordinary product/test run's
// stdout is never touched by this. Exists because a real live Phase 24.1D
// acceptance run was, from the outside, indistinguishable between
// "waiting on the network" and "grinding through local Papinian NER" -
// this makes that distinction directly observable while the operator
// watches their own terminal, not something to infer from process
// CPU/RSS after the fact.
inline bool verbose_progress_enabled() {
	static const bool enabled = [] {
		const char *value = std::getenv("SINGULARITY_VERBOSE_PROGRESS");
		return value != nullptr && *value != '\0' && std::string(value) != "0";
	}();
	return enabled;
}

inline std::chrono::steady_clock::time_point progress_start_time() {
	static const auto start = std::chrono::steady_clock::now();
	return start;
}

inline void progress(const std::string &line) {
	if (verbose_progress_enabled() == false) return;
	const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - progress_start_time()).count();
	std::fprintf(stderr, "PROGRESS [+%lld.%03llds] %s\n",
		static_cast<long long>(elapsedMs / 1000), static_cast<long long>(elapsedMs % 1000), line.c_str());
	std::fflush(stderr);
}

} // namespace singularity::routing::memory_diagnostics

#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
#include "LocalModel/Models/PapinianNER/NativeRuntime/gliner2_ffi.h"

#include <mach/mach.h>
#include <mach/task.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace singularity::routing::memory_diagnostics {

inline std::string target_document_payload;
inline std::string protected_target_document_payload;
inline std::size_t project_context_bytes = 0;
inline std::size_t conversation_history_bytes = 0;
inline PromptGrowthBaselines prompt_growth_baselines;

inline std::uint64_t phys_footprint_bytes() {
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	return task_info(mach_task_self(), TASK_VM_INFO,
		reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS ? info.phys_footprint : 0;
}

inline std::size_t occurrences(const std::string &haystack, const std::string &needle) {
	if (needle.empty()) return 0;
	std::size_t count = 0;
	for (std::size_t offset = 0; (offset = haystack.find(needle, offset)) != std::string::npos;
			offset += needle.size()) ++count;
	return count;
}

inline void checkpoint(const std::string &boundary, const std::string &details = {}) {
	std::printf("PRODMEMDIAG boundary=%s phys_footprint=%llu engine_instances=%zu semantic_chunks_total=%zu%s%s\n",
		boundary.c_str(), static_cast<unsigned long long>(phys_footprint_bytes()),
		lm_gliner2_engine_instances_alive(), lm_gliner2_semantic_chunks_executed_total(),
		details.empty() ? "" : " ", details.c_str());
	std::fflush(stdout);
	if (lm_gliner2_engine_instances_alive() > 1)
		throw std::runtime_error("PRODMEMDIAG assertion: Papinian runtime instance count > 1 for one operation");
}

inline void register_document_payloads(std::string original, std::string protected_payload) {
	target_document_payload = std::move(original);
	protected_target_document_payload = std::move(protected_payload);
}

inline void assert_prompt_growth(const std::string &prompt, const std::string &boundary,
		std::size_t estimated_tokens, PromptRepresentationKind representation) {
	const std::size_t original_count = occurrences(prompt, target_document_payload);
	const std::size_t protected_count = occurrences(prompt, protected_target_document_payload);
	const std::size_t expected_count = 1;
	checkpoint(boundary,
		"loop_prompt_bytes=" + std::to_string(prompt.size())
		+ " representation=" + to_string(representation)
		+ " estimated_tokens=" + std::to_string(estimated_tokens)
		+ " project_context_bytes=" + std::to_string(project_context_bytes)
		+ " conversation_history_bytes=" + std::to_string(conversation_history_bytes)
		+ " original_document_occurrences=" + std::to_string(original_count)
		+ " protected_document_occurrences=" + std::to_string(protected_count));
	prompt_growth_baselines.observe(representation, prompt.size());
	if (protected_count > expected_count || original_count > expected_count)
		throw std::runtime_error("PRODMEMDIAG assertion: document occurrence count > expected");
}

inline const char *bounded_runtime_id() { return lm_gliner2_bounded_runtime_id(); }
inline std::size_t maximum_encoder_positions() { return lm_gliner2_max_encoder_positions(); }

} // namespace singularity::routing::memory_diagnostics

#else

namespace singularity::routing::memory_diagnostics {
inline void checkpoint(const std::string &, const std::string & = {}) {}
inline void register_document_payloads(std::string, std::string) {}
inline void assert_prompt_growth(const std::string &, const std::string &, std::size_t,
		PromptRepresentationKind) {}
} // namespace singularity::routing::memory_diagnostics

#endif
