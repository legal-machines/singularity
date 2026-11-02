#pragma once

#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::protected_api::papinian {

// The real local materials one prepared request actually drew from - never a
// frontend guess. Phase 11 "Reveal original locally" only ever answers
// "where did this exact value come from", so origin detection is a byte-for-
// byte search against this same request's own inputs, not string matching
// against unrelated Project content.
struct OriginSources {
	struct Document {
		std::string display_name;
		std::string content;
	};

	// The user's own typed current request, before context assembly.
	std::string raw_request_text;
	std::vector<Document> documents;
};

// One entry from a request's ProtectionResult, redacted down to what "What
// leaves this device?" may show before an explicit "Reveal original locally"
// click - PRIVACY_CONTRACT.md/phase-11's ProtectionOccurrence. Never carries
// entity.value/RequestMappingEntry::original.
struct ProtectedOccurrenceSummary {
	// Stable for identical request content regardless of request_namespace
	// (which is regenerated per protect() call) - a category plus its
	// first-appearance index, e.g. "PERSON_1". Opaque to the frontend; only
	// ever echoed back to reveal_occurrence().
	std::string occurrence_id;
	// The same taxonomy label PrivacyMetadata::category_counts already uses
	// (e.g. "PERSON", "MONEY_AMOUNT", "CUSTOM_TERM").
	std::string category;
	// Human presentation of the same identity, e.g. "PERSON 1" - never the
	// technical "[[LM_<namespace>_PERSON_1]]" placeholder.
	std::string display_placeholder;
	// "request" (the user's own typed text), "document" (a named Project
	// source) or "context" (assembled material not tied to one document, e.g.
	// Matter instructions/history).
	std::string origin_kind;
	// Set only when origin_kind == "document".
	std::optional<std::string> origin_object_id;
};

struct ProtectedPreviewSummary {
	// `result.protected_text` with every technical placeholder swapped for its
	// display_placeholder - the "Protected preview" presentation layer.
	// Distinct from the literal outbound text ("Show exact protected text"),
	// which callers already have as ProtectionResult::protected_text /
	// PreviewResult::outbound_text.
	std::string human_readable_text;
	std::vector<ProtectedOccurrenceSummary> occurrences;
};

// Redacts a real ProtectionResult down to safe, no-original-value summary
// data. Returns an empty summary (human_readable_text == the input text
// unchanged) when protection was not active, exactly like protect() itself.
[[nodiscard]] ProtectedPreviewSummary summarize_for_preview(
		const ProtectionResult &result, const OriginSources &sources);

// The one place "Reveal original locally" is allowed to see a real value -
// an on-demand, single-occurrence lookup, never a stored/returned map.
// Recomputed fresh from the same ProtectionResult a caller already produced
// for this one request/response; nothing here is retained afterward.
struct RevealedOccurrence {
	std::string occurrence_id;
	std::string category;
	std::string display_placeholder;
	std::string origin_kind;
	std::optional<std::string> origin_object_id;
	// A short window around the real match inside whichever source text
	// actually contains it - never the full source, never the raw value alone
	// outside this local inspection context.
	std::string excerpt_before;
	std::string excerpt_match;
	std::string excerpt_after;
};

// std::nullopt when `occurrence_id` does not match any mapping entry (for
// example, the request text changed since the preview that produced it) -
// callers must fail with a plain-language message, never guess.
[[nodiscard]] std::optional<RevealedOccurrence> reveal_occurrence(
		const ProtectionResult &result, const OriginSources &sources, const std::string &occurrence_id);

} // namespace singularity::protected_api::papinian
