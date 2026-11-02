#pragma once

#include "ExternalResearch/ResearchTools/ResearchTools.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Bounds what ResearchToolLoop.hpp actually sends back to a reasoning model
// after a tool call, against that model's real context window - see
// ResearchToolLoop.hpp's own untrusted-tool-result append, which previously
// had no budget accounting at all and could (and did, against the
// production "openai:gpt-4" reasoning assignment - 8192 tokens, the real
// documented figure for that exact model) overflow the model's context on a
// genuinely evidence-heavy research turn.
namespace singularity::research {

// A conservative, provider-agnostic token estimate - the same
// ~2-UTF8-code-points-per-token estimate. Document/DocumentChunker.cpp's own
// estimated_token_count() uses ~4 chars/token, a reasonable ratio for plain
// English prose - but this budget exists specifically to bound tool-result
// JSON (URLs, quoted keys, escaped punctuation), and real measurement against
// the actual provider proved 4 chars/token badly optimistic for that content:
// a live turn this loop let through as under-budget was rejected by OpenAI at
// 13403 real tokens, back-computing to a real ~2.1-2.24 chars/token ratio for
// that exact payload (BPE tokenizers split URLs and dense punctuation into
// many short tokens). 2 chars/token is deliberately conservative rather than
// a tight fit to that one data point - this heuristic must never again be the
// reason a continuation the loop believed fit still overflows the model.
// Two code points per token holds only for scripts whose characters take one
// or two UTF-8 bytes (Latin, Cyrillic, Greek, Hebrew, Arabic, ...). Scripts
// encoded in three or four bytes (CJK, Kana, Hangul, Devanagari, Thai, emoji,
// ...) are split by real tokenizers into about one token per character or
// more, so each such code point counts as a whole token: a Chinese or Hindi
// document must never look half its real size to a capacity decision.
inline std::size_t estimate_tokens(std::string_view text) noexcept {
	std::size_t halfTokens = 0;
	for (unsigned char byte : text) {
		if ((byte & 0xc0U) == 0x80U) continue;
		halfTokens += byte >= 0xe0U ? 2 : 1;
	}
	return std::max<std::size_t>(1, (halfTokens + 1) / 2);
}

// estimate_tokens() of `text` as a JSON string value - the form a provider-
// facing turn carries prompt text in (ModelVisibleTurn::serialize()), where
// every quote, backslash and control character is escaped. Budgeting raw
// prompt text with this keeps it consistent with the continuation gate, which
// measures the serialized turn; tab/newline-dense extracts (spreadsheets,
// CSV) grow materially once escaped.
inline std::size_t estimate_serialized_tokens(std::string_view text) noexcept {
	std::size_t halfTokens = 0;
	for (unsigned char byte : text) {
		if ((byte & 0xc0U) == 0x80U) continue;
		halfTokens += byte >= 0xe0U ? 2 : 1;
		if (byte == '"' || byte == '\\' || byte == '\n' || byte == '\r' || byte == '\t' || byte == '\b' || byte == '\f')
			++halfTokens;
		else if (byte < 0x20U)
			halfTokens += 5;
	}
	return std::max<std::size_t>(1, (halfTokens + 1) / 2);
}

// The real, documented context window (in tokens) of well-known reasoning
// models a Node role assignment can point at - published per-model facts,
// never a blanket number applied to every model. Ordered most-specific
// prefix first since several real model-id families nest ("gpt-4o" and
// "gpt-4.1" are themselves prefixed by "gpt-4"). A model this catalog does
// not recognize returns std::nullopt; callers must not guess a number for
// it, only apply the budget where capacity is actually known.
// The local provider has NO catalog entry: Singularity is the sole authority for
// local executable context (local inference's context-capability contract - the
// planned, feasibility-checked figure before a load and the actually granted
// figure while loaded, both read live through LocalInferenceService). A local model's
// capacity reaches this file only as `discoveredContextWindowTokens`; when
// Singularity cannot yet say (no cached calibration, not inspected), it stays
// unknown here too. The previous provider-level constant (4096) mirrored an
// Singularity implementation detail that no longer holds - Singularity plans per model
// and per machine up to 131072 - and was a duplicated authority.
inline std::optional<std::size_t> known_model_context_window_tokens(
		const std::string &providerId, const std::string &modelId) noexcept {
	if (providerId != "openai") return std::nullopt;
	static const std::vector<std::pair<std::string, std::size_t>> catalog{
		{"gpt-4.1", 1047576},
		{"gpt-4o", 128000},
		{"gpt-4-turbo", 128000},
		{"gpt-4-32k", 32768},
		{"gpt-4", 8192},
		{"gpt-3.5-turbo-instruct", 4096},
		{"gpt-3.5-turbo-16k", 16384},
		{"gpt-3.5-turbo", 16385},
		{"o4-mini", 200000},
		{"o3", 200000},
		{"o1", 200000},
	};
	for (const auto &entry : catalog)
		if (modelId.rfind(entry.first, 0) == 0) return entry.second;
	return std::nullopt;
}

// Provider transports the model; model capability belongs to the resolved
// model, not to the string value of providerId. `discoveredContextWindowTokens`
// is the real, provider-reported capability already captured on the
// selected model's ModelDescriptor at connection/discovery time (see
// ProviderRegistry.cpp's discovered_context_window_tokens(), threaded
// through InferenceExecutor.cpp's resolve_generation_inputs()) - an
// OpenAI-compatible gateway (OpenRouter, Groq, a self-hosted endpoint, ...)
// that reports a real `context_length` for the exact model in use always
// wins here, regardless of which provider transported it. The hand-written
// catalog above is only a fallback for a provider/model that reports no
// such thing - direct OpenAI's real /models response has none today, and
// "local" has none via local inference yet either. Callers (ResearchToolLoop)
// pass whatever they were given straight through and hold no provider-name
// knowledge of their own; this is the one place that combines "real
// discovered capability" with "known fallback catalog" into one resolved
// answer, still std::nullopt (never guessed) when neither source knows.
inline std::optional<std::size_t> resolve_model_context_window_tokens(
		std::optional<std::size_t> discoveredContextWindowTokens,
		const std::string &providerId, const std::string &modelId) noexcept {
	if (discoveredContextWindowTokens.has_value() && *discoveredContextWindowTokens > 0)
		return discoveredContextWindowTokens;
	return known_model_context_window_tokens(providerId, modelId);
}

// The one rule for a local model's executable context, from local inference's own
// two figures: the context actually granted to the current load is the final
// authority; before a load, the feasibility-checked planned figure Singularity
// computed for this machine; with neither, genuinely unknown - never a
// constant and never the package's advertised maximum.
inline std::optional<std::size_t> resolve_local_context_window_tokens(
		std::optional<std::uint64_t> grantedContextTokens,
		std::optional<std::uint64_t> plannedContextTokens) noexcept {
	if (grantedContextTokens && *grantedContextTokens > 0) return static_cast<std::size_t>(*grantedContextTokens);
	if (plannedContextTokens && *plannedContextTokens > 0) return static_cast<std::size_t>(*plannedContextTokens);
	return std::nullopt;
}

// The one rule for the output ceiling actually dispatched to a local model
// once its granted context is known: never more than half the granted
// window - the same half-window rule the request was budgeted with before
// the load, now applied to the real grant - and never more than the caller
// asked for. A caller's per-turn override is bounded the same way: it was
// derived from the pre-load (planned) figure and is exactly the stale
// estimate this rule exists to correct. With no known grant the caller's
// figure stands - never a constant.
inline std::uint64_t bounded_local_output_tokens(std::uint64_t requestedOutputTokens,
		std::optional<std::size_t> grantedContextWindowTokens) noexcept {
	if (!grantedContextWindowTokens || *grantedContextWindowTokens == 0) return requestedOutputTokens;
	return std::min<std::uint64_t>(requestedOutputTokens, static_cast<std::uint64_t>(*grantedContextWindowTokens) / 2);
}

// The real, documented MAXIMUM COMPLETION allowance a cloud provider accepts
// for a well-known model - a distinct fact from its context window (several
// of these providers cap completion tokens far below total context, and
// require the caller to send SOME explicit numeric ceiling on every request
// regardless of how the semantic contract itself is bounded). Used only when
// a TurnContract-driven call wants "the largest safe physical allowance", so
// a real semantic completion is never cut short by a small, Singularity-guessed
// number the way a local generation used to be - see ModelVisibleTurn::
// request_semantic_completion. Never a blanket number applied to every
// model, and never something this codebase invents: every figure here is
// the provider's own published per-model ceiling, current as of this
// session. A model this catalog does not recognize returns std::nullopt;
// callers fall back to whatever ceiling they already had rather than
// guessing one.
inline std::optional<std::size_t> known_model_maximum_completion_tokens(
		const std::string &providerId, const std::string &modelId) noexcept {
	if (providerId == "openai") {
		static const std::vector<std::pair<std::string, std::size_t>> catalog{
			{"gpt-4.1", 32768},
			{"gpt-4o", 16384},
			{"gpt-4-turbo", 4096},
			{"gpt-4-32k", 4096},
			{"gpt-4", 4096},
			{"gpt-3.5-turbo-instruct", 4096},
			{"gpt-3.5-turbo-16k", 4096},
			{"gpt-3.5-turbo", 4096},
			{"o4-mini", 100000},
			{"o3", 100000},
			{"o1", 100000},
		};
		for (const auto &entry : catalog)
			if (modelId.rfind(entry.first, 0) == 0) return entry.second;
		return std::nullopt;
	}
	if (providerId == "anthropic") {
		static const std::vector<std::pair<std::string, std::size_t>> catalog{
			{"claude-3-5-haiku", 8192},
			{"claude-3-5-sonnet", 8192},
			{"claude-3-opus", 4096},
			{"claude-3-haiku", 4096},
		};
		for (const auto &entry : catalog)
			if (modelId.rfind(entry.first, 0) == 0) return entry.second;
		return std::nullopt;
	}
	// OpenRouter (and any other OpenAI-compatible gateway) transports several
	// vendors under one providerId, each with its own vendor/model-id prefix
	// on modelId - a real, live external-API acceptance run against
	// "openai/gpt-5" this session proved WHY this entry matters concretely,
	// not hypothetically: GPT-5 is a reasoning model whose completion_tokens
	// bill invisible reasoning tokens BEFORE any visible content, so the
	// EvidencePlanning-phase fallback estimate above (phase_aware_reserved_
	// output_tokens()'s ~256-token bounded-inspection-call reservation - a
	// perfectly real ceiling for a non-reasoning model's own small tool call)
	// left GPT-5 zero room to ever emit its tool call: two consecutive
	// attempts both returned finish_reason="length" with 256 completion
	// tokens entirely consumed by reasoning and textBytes=0. Each ceiling
	// below is the vendor's own documented max output/completion allowance
	// for that exact model as of this session, not a guess - matching this
	// function's own contract (used only when ModelVisibleTurn::
	// request_semantic_completion is set, i.e. never changes what counts as
	// turn completion, only how much physical room a real semantic
	// completion is given to actually happen in).
	if (providerId == "openai_compatible") {
		static const std::vector<std::pair<std::string, std::size_t>> catalog{
			{"openai/gpt-5", 128000},
			{"openai/gpt-4.1", 32768},
			{"openai/gpt-4o", 16384},
			{"openai/o4-mini", 100000},
			{"openai/o3", 100000},
			{"openai/o1", 100000},
			{"anthropic/claude-sonnet-4.5", 64000},
			{"anthropic/claude-opus-4", 32000},
			{"google/gemini-2.5-flash", 65536},
			{"google/gemini-2.5-pro", 65536},
		};
		for (const auto &entry : catalog)
			if (modelId.rfind(entry.first, 0) == 0) return entry.second;
		return std::nullopt;
	}
	return std::nullopt;
}

// What run_research_tool_loop() must fit its next continuation turn inside.
struct ContinuationBudget {
	std::size_t capacity_tokens = 0;
	std::size_t reserved_output_tokens = 0;
};

struct CompactedToolResult {
	// The block to append after the existing loop prompt (already framed with
	// its own leading/trailing protocol text) - never larger than what
	// fits_within's caller established was actually available.
	std::string appended_text;
	// True when one or more evidence entries had their excerpt shortened, or
	// were dropped, to make the turn fit. Canonical evidence is never
	// affected by this - see ResearchToolLoop.hpp, which stores the full,
	// uncompacted ExternalEvidenceReference set on the InferenceResult (and
	// from there, the canonical MatterTask) regardless of what a compacted
	// prompt copy above actually contains.
	bool evidence_compacted = false;
};

// The common continuation boundary for any provider-facing input, including
// typed operation turns. `providerFacingInput` must already include every
// model-visible control/data plane and tool-schema representation the caller
// will send. This does not compact or reinterpret content; it is the shared
// predictive gate used before a provider/runtime dispatch.
inline bool fits_continuation_budget(const std::string &providerFacingInput,
		const ContinuationBudget &budget) noexcept;

namespace detail {

// A small, fixed safety margin for provider-side framing this heuristic
// cannot see (message-role wrapping, system-prompt tokens the request layer
// adds after this loop runs) - not a guess at total capacity, only a
// cushion against under-counting the literal text already assembled here.
inline std::size_t framing_margin_tokens(std::size_t capacityTokens) noexcept {
	return std::max<std::size_t>(128, capacityTokens / 20);
}

// The largest prefix of `text`, at most `maxBytes` long, that ends on a
// UTF-8 character boundary - never splits a multi-byte sequence (Cyrillic,
// any non-ASCII script) in half. A live Phase 24.1D acceptance run proved
// this is not theoretical: a raw `excerpt.resize(excerptChars)` byte cut
// through a real Russian-language search excerpt produced a dangling UTF-8
// continuation byte, which nlohmann::json::dump() then rejected
// (type_error.316) the next time that excerpt was serialized into the
// outbound provider request - after the loop had already accepted it as
// "fits the budget". A continuation byte has the bit pattern 10xxxxxx
// (`(byte & 0xC0) == 0x80`); walking back over a run of them lands on the
// start of the character currently being cut, so that whole (necessarily
// incomplete) character is dropped rather than left half-written.
inline std::size_t utf8_safe_truncation_length(const std::string &text, std::size_t maxBytes) noexcept {
	if (text.size() <= maxBytes) return text.size();
	std::size_t end = maxBytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
	return end;
}

inline nlohmann::json shortened_evidence(const nlohmann::json &evidence, std::size_t excerptChars) {
	nlohmann::json copy = evidence;
	// Unknown metadata remains unknown when omitted from the disposable
	// model context. Canonical persisted evidence retains its complete shape.
	if (copy.is_object()) {
		for (auto entry = copy.begin(); entry != copy.end();) {
			if (entry.value().is_null()) entry = copy.erase(entry);
			else ++entry;
		}
	}
	if (copy.contains("excerpt") && copy.at("excerpt").is_string()) {
		std::string excerpt = copy.at("excerpt").get<std::string>();
		if (excerpt.size() > excerptChars) {
			excerpt.resize(utf8_safe_truncation_length(excerpt, excerptChars));
			excerpt += "\xE2\x80\xA6"; // UTF-8 ellipsis, marks a visibly truncated excerpt.
		}
		copy["excerpt"] = excerpt;
	}
	return copy;
}

inline nlohmann::json essential_evidence(const nlohmann::json &evidence, std::size_t excerptChars) {
	// The smallest useful disposable model-context form. Canonical evidence
	// remains complete in InferenceResult/MatterTask; this copy retains what
	// a small model needs to reason and cite: opaque identity, integrity,
	// source URL/class, adapter provenance, retrieval time, and some content.
	// Drop duplicated display metadata (providerSourceId/domain/title and
	// optional legal metadata) only after ordinary excerpt compaction could
	// not fit even one source.
	nlohmann::json result = nlohmann::json::object();
	for (const char *key : {"evidenceId", "contentHash", "contentKind", "url", "sourceClass",
			"researchProviderId", "researchEngine", "retrievedAt", "excerpt"}) {
		if (evidence.contains(key) && !evidence.at(key).is_null()) result[key] = evidence.at(key);
	}
	if (result.contains("excerpt") && result.at("excerpt").is_string()) {
		std::string excerpt = result.at("excerpt").get<std::string>();
		if (excerpt.size() > excerptChars) {
			excerpt.resize(utf8_safe_truncation_length(excerpt, excerptChars));
			excerpt += "\xE2\x80\xA6";
		}
		result["excerpt"] = std::move(excerpt);
	}
	return result;
}

} // namespace detail

// Cutting a UTF-8 string safely is a general primitive, not a research
// concept: any module that bounds real document or provider text needs it,
// and the alternative is a fourth hand-rolled copy of the continuation-byte
// walk. Re-exported from detail rather than moved, so every existing
// research::detail:: caller keeps compiling unchanged.
using detail::utf8_safe_truncation_length;

inline bool fits_continuation_budget(const std::string &providerFacingInput,
		const ContinuationBudget &budget) noexcept {
	return estimate_tokens(providerFacingInput) + budget.reserved_output_tokens
		+ detail::framing_margin_tokens(budget.capacity_tokens) <= budget.capacity_tokens;
}

// FINAL PHYSICAL ADMISSION of a request's first provider-facing turn: with the
// executing model's context capacity known, that turn plus the output ceiling
// it is dispatched with must fit, by the same measure every continuation gate
// applies. Known capacity is authoritative; unknown capacity is never guessed
// and admits. routing::execute() refuses a request that fails this before
// anything is sent (context_capacity_exceeded).
inline bool first_turn_fits_known_capacity(const std::string &providerFacingTurn,
		std::optional<std::size_t> capacityTokens, std::size_t dispatchedOutputTokens) noexcept {
	return !capacityTokens || fits_continuation_budget(providerFacingTurn,
		ContinuationBudget{*capacityTokens, dispatchedOutputTokens});
}

inline constexpr const char *kContextCapacityExceededBeforeDispatch =
	"This request does not fit the selected model's context window together with the response it needs, so "
	"nothing was sent. Select fewer or smaller documents, shorten the request or choose a model with a larger "
	"context window.";

// The admissible input-token budget for one piece of canonical research
// input (e.g. a single web.search query) submitted on behalf of `modelId` -
// the real capacity boundary that replaces the removed, unjustified
// 1000-byte web.search query limit (see validate_search()'s own comment).
// Reuses the exact reserved-output/framing-margin policy
// fit_tool_result_to_continuation() already applies to the continuation
// prompt, so a query and a continuation are bounded by one consistent
// policy, not two unrelated heuristics - and reuses
// known_model_context_window_tokens() rather than any model-specific
// literal, so this stays provider/model independent by construction: it has
// no branch that mentions any specific model. Returns std::nullopt when
// that model's real context window is not known, so a caller can fail
// truthfully rather than silently assume unlimited capacity - see
// known_model_context_window_tokens()'s own comment on why "unknown" must
// never be guessed at.
// Same policy, applied to an already-resolved context window rather than
// looking one up itself - the form resolve_model_context_window_tokens()'s
// callers (ResearchToolLoop, which holds no provider-name knowledge of its
// own) actually need, since they already resolved discovered-vs-catalog
// capacity before this point.
// SINGULARITY'S OWN POLICY, in one place - deliberately NOT a property of any
// model, provider or runtime, and never to be restated call-site by call-site:
// a call declares the output ceiling its own contract needs, and against a
// RESOLVED window that reservation is additionally bounded so the window always
// keeps room for the prompt itself. With capacity unknown the declared ceiling
// stands, because an unknown window is never guessed at (see
// known_model_context_window_tokens()). Every call that reserves output -
// the professional response, its review, an operation turn, automatic Context -
// gets its reservation from here, so the policy can be changed once and cannot
// silently diverge between them.
// The output ceiling a professional response declares. A data-room-wide table
// or chronology, with its structured Work Product beside the prose, runs past
// eight thousand tokens - sooner on a reasoning model, whose own reasoning is
// billed against the same ceiling - and a response cut at the ceiling is
// discarded and generated again: twice the wait and twice the bill for one
// answer (live: a 59-row chronology took 215 s where 105 s would have done).
// The ceiling therefore grows with the window - an eighth of it - never below
// the historical 8192, so a small local window keeps exactly the room for its
// prompt it always had, and never above 32768. A ceiling is only ever paid for
// by the tokens actually generated.
inline std::size_t professional_response_output_ceiling(std::optional<std::size_t> contextWindow) noexcept {
	constexpr std::size_t kSmallest = 8192;
	constexpr std::size_t kLargest = 32768;
	if (contextWindow.has_value() == false) return kSmallest;
	return std::min(kLargest, std::max(kSmallest, *contextWindow / 8));
}

inline std::size_t reserved_output_tokens_for_window(
		std::optional<std::size_t> contextWindow, std::size_t declaredOutputCeiling) noexcept {
	if (contextWindow.has_value() == false) return declaredOutputCeiling;
	return std::min<std::size_t>(declaredOutputCeiling, *contextWindow / 2);
}

inline std::optional<std::size_t> admissible_research_input_tokens_for_window(
		std::optional<std::size_t> contextWindow, std::size_t reservedOutputTokens) noexcept {
	if (contextWindow.has_value() == false) return std::nullopt;
	const std::size_t overhead = reservedOutputTokens + detail::framing_margin_tokens(*contextWindow);
	if (overhead >= *contextWindow) return 0;
	return *contextWindow - overhead;
}

inline std::optional<std::size_t> admissible_research_input_tokens(
		const std::string &providerId, const std::string &modelId, std::size_t reservedOutputTokens) noexcept {
	return admissible_research_input_tokens_for_window(
		known_model_context_window_tokens(providerId, modelId), reservedOutputTokens);
}

// Fits `toolResultValue` (the raw ToolResult::value a web.search/web.open
// call returned - {"evidence": [...]} or {"evidence": {...}}, alongside
// whatever else that tool reported) into the room actually left in the
// model's context window after `promptSoFar` and the reserved output budget
// - compacting per-evidence excerpts, then dropping the least-recent
// evidence entries, before ever silently exceeding capacity. Throws
// std::runtime_error (never returns a block guaranteed to overflow) when
// even one minimally-compacted evidence entry cannot fit - the "fail
// truthfully before provider execution" side of the invariant, for the
// (rare, evidence-saturated) case bounded compaction genuinely cannot
// resolve.
inline CompactedToolResult fit_tool_result_to_continuation(const nlohmann::json &toolResultValue,
		const std::string &promptSoFar, const std::string &wrapperPrefix, const std::string &wrapperSuffix,
		const ContinuationBudget &budget,
		const std::function<std::string(const std::string &)> &compileProviderFacingInput =
			[](const std::string &input) { return input; }) {
	auto fits = [&](const std::string &candidate) {
		return fits_continuation_budget(compileProviderFacingInput(candidate), budget);
	};
	if (!fits(promptSoFar))
		throw std::runtime_error(
			"Web research stopped: no context budget remains in this model's context window for further research evidence.");

	nlohmann::json compactedResult = toolResultValue;
	bool compacted = false;
	if (toolResultValue.contains("evidence")) {
		const nlohmann::json &raw = toolResultValue.at("evidence");
		nlohmann::json framing = toolResultValue;
		framing["evidence"] = raw.is_array() ? nlohmann::json::array() : nlohmann::json::object();
		std::vector<nlohmann::json> entries;
		if (raw.is_array()) for (const auto &entry : raw) entries.push_back(entry);
		else if (raw.is_object()) entries.push_back(raw);

		// Shrink excerpts before ever dropping an entry outright - preserves
		// source identity/class/citation id/provenance for every source that
		// fits, at the cost of excerpt depth, rather than silently losing
		// whole sources first.
		// Smaller excerpt levels strictly reduce per-entry cost, so trying
		// them in decreasing order and keeping the last (smallest) attempt's
		// result maximizes how many real sources make it in - stopping early
		// only once a level already fits every entry, since shrinking
		// further would only lose excerpt depth for no additional sources.
		struct CompactionLevel { std::size_t excerpt_chars; bool essential_metadata; };
		static const CompactionLevel kCompactionLevels[] = {
			{700, false}, {400, false}, {200, false}, {80, false}, {80, true}, {40, true}};
		std::vector<nlohmann::json> fitted;
		for (const CompactionLevel level : kCompactionLevels) {
			fitted.clear();
			for (const nlohmann::json &entry : entries) {
				const nlohmann::json candidate = level.essential_metadata
					? detail::essential_evidence(entry, level.excerpt_chars)
					: detail::shortened_evidence(entry, level.excerpt_chars);
				auto trialEntries = fitted;
				trialEntries.push_back(candidate);
				nlohmann::json trial = framing;
				if (raw.is_array()) trial["evidence"] = trialEntries;
				else trial["evidence"] = trialEntries.front();
				if (fits(promptSoFar + wrapperPrefix + trial.dump() + wrapperSuffix))
					fitted.push_back(candidate);
			}
			// A level other than the first (700, the real production
			// bounded_excerpt() cap - see ExternalResearchAdapter.cpp) means
			// every entry's excerpt had to be shortened to fit, even when
			// none was dropped outright - still a real compaction.
			if (fitted.size() == entries.size()) {
				if (level.excerpt_chars != kCompactionLevels[0].excerpt_chars || level.essential_metadata)
					compacted = true;
				break;
			}
		}
		if (fitted.empty() && entries.empty() == false)
			throw std::runtime_error(
				"Current information was requested, but the model's context window has no room left for the research evidence found.");
		if (fitted.size() != entries.size()) compacted = true;
		if (raw.is_array()) {
			nlohmann::json array = nlohmann::json::array();
			for (auto &entry : fitted) array.push_back(std::move(entry));
			compactedResult["evidence"] = std::move(array);
		} else if (fitted.empty() == false) {
			compactedResult["evidence"] = fitted.front();
		}
	}

	std::string block = wrapperPrefix + compactedResult.dump() + wrapperSuffix;
	if (!fits(promptSoFar + block))
		throw std::runtime_error("Web research stopped: the complete tool result exceeds the remaining context budget.");
	return CompactedToolResult{std::move(block), compacted || compactedResult != toolResultValue};
}

} // namespace singularity::research
