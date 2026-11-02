#pragma once

#include "Context/ContextRetriever/ContextRetriever.hpp"
#include "Context/Types/ProjectContext.hpp"
#include "Matter/Types/Matter.hpp"

#include <string>
#include <vector>

namespace singularity::context {

// One strictly validated, source-backed value returned by the Context model.
// It is not canonical state: apply_context_candidates() must still pass it
// through ContextCuration's authority and suggestion rules.
struct ContextCandidate {
	ContextSection section = ContextSection::Other;
	std::string label;
	std::string value;
	ResolvedSourceReference source;
	// Populated only after parsing by the Project orchestrator from the local
	// Entity Registry. It is never accepted from model output.
	std::optional<std::string> entity_id;
};

struct PreparedContextBuild {
	std::string prompt;
	std::vector<RetrievedContextChunk> supplied_chunks;
};

struct ContextBuildApplication {
	long long candidate_count = 0;
	long long applied_count = 0;
	long long suggestion_count = 0;
	std::vector<std::string> item_ids;
	std::vector<std::string> suggestion_ids;
};

// The automatic Context contract's own limits, so no caller has to guess them:
// the instruction every build carries, and the most candidates one build may
// return. A capacity decision measures these instead of assuming a number.
inline constexpr std::size_t kMaximumContextCandidates = 16;
// This build's own output reservation - the same kind of declared per-call
// ceiling the professional path uses for its own contract, and bounded by the
// executing model's REAL resolved window by the caller (never a window figure
// assumed here). Sixteen concise, source-backed candidates can exceed a generic
// small budget before the JSON object closes.
inline constexpr std::size_t kContextBuildMaxOutputTokens = 2048;
[[nodiscard]] const std::string &context_build_instruction();

// Uses the existing opaque SourceMarkerRegistry format. Document names and
// filesystem paths never enter the model prompt; only real locally extracted
// chunks and request-scoped source markers do.
[[nodiscard]] PreparedContextBuild prepare_context_build(
		const std::vector<RetrievedContextChunk> &chunks);

// Accepts only the documented JSON shape and only source markers issued for
// this exact invocation. A fabricated/missing source rejects the whole model
// response before canonical state can be touched.
[[nodiscard]] std::vector<ContextCandidate> parse_context_candidates(
		const std::string &response_text, const std::vector<RetrievedContextChunk> &supplied_chunks);

// Applies every candidate to a caller-owned Matter through ContextCuration.
// Callers apply to a working copy and persist only after the whole operation
// succeeds, providing transactional failure safety around model output.
[[nodiscard]] ContextBuildApplication apply_context_candidates(
		matters::Matter &matter, const std::vector<ContextCandidate> &candidates, const std::string &timestamp);

} // namespace singularity::context
