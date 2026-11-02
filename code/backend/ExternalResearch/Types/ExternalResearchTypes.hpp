#pragma once

#include "Provider/Types/UsageMetadata.hpp"

#include <atomic>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

namespace singularity::research {

enum class ResearchMode { Off, Auto, Required, Contradictory };

// A domain preflight tells the existing research loop whether the rejected
// final needs more source retrieval or only one tool-free structured repair.
// This prevents syntax/provenance mistakes from spending research capacity.
enum class FinalTextIssueKind { CorrectionOnly, ResearchCoverage, OperationWork };

struct FinalTextIssue {
	FinalTextIssueKind kind = FinalTextIssueKind::CorrectionOnly;
	std::string message;
	// Stable domain-owned class for a tool-free correction. The research loop
	// permits one correction for each distinct class in the current evidence
	// state, while the existing total model-turn budget remains authoritative.
	std::string correction_key = "domain_validation";
};

struct OperationExecutionProgress {
	std::size_t model_turns_used = 0;
	// Kept separately even though every current model turn maps to one
	// provider request. This is an operation audit invariant, not an
	// assumption the reporting layer has to reconstruct later.
	std::size_t provider_requests_used = 0;
	std::size_t successful_tool_calls_used = 0;
	std::size_t search_calls_used = 0;
	std::size_t open_calls_used = 0;
	std::size_t item_deltas_proposed = 0;
	std::size_t item_deltas_accepted = 0;
	std::size_t item_deltas_rejected = 0;
	std::size_t incomplete_response_repairs = 0;
	std::size_t protected_response_repairs = 0;
	// A professional document operation is assembled from retained item
	// deltas. It must never ask the model to reproduce the whole accepted
	// action graph merely because a later item failed.
	std::size_t full_action_regenerations = 0;
	std::size_t document_mutations_committed = 0;
};

inline const char *to_string(ResearchMode mode) noexcept {
	switch (mode) {
		case ResearchMode::Off: return "off";
		case ResearchMode::Auto: return "auto";
		case ResearchMode::Required: return "required";
		case ResearchMode::Contradictory: return "contradictory";
	}
	return "auto";
}

enum class ExternalSourceClass {
	OfficialLegislation,
	CourtDecision,
	Government,
	SecondaryLegalAnalysis,
	GeneralWeb,
};

// How much source content Singularity actually obtained for this evidence item.
// SearchResult is discovery metadata/snippet only. RetrievedContent means the
// canonical research adapter fetched/read the source through web.open. Legacy
// persisted evidence defaults to SearchResult so an old snippet can never be
// silently upgraded into substantive support after reload.
enum class ExternalEvidenceContentKind { SearchResult, RetrievedContent };

inline const char *to_string(ExternalEvidenceContentKind kind) noexcept {
	switch (kind) {
		case ExternalEvidenceContentKind::SearchResult: return "search_result";
		case ExternalEvidenceContentKind::RetrievedContent: return "retrieved_content";
	}
	return "search_result";
}

inline std::optional<ExternalEvidenceContentKind> external_evidence_content_kind_from_string(
		const std::string &value) noexcept {
	if (value == "search_result") return ExternalEvidenceContentKind::SearchResult;
	if (value == "retrieved_content") return ExternalEvidenceContentKind::RetrievedContent;
	return std::nullopt;
}

inline const char *to_string(ExternalSourceClass sourceClass) noexcept {
	switch (sourceClass) {
		case ExternalSourceClass::OfficialLegislation: return "official_legislation";
		case ExternalSourceClass::CourtDecision: return "court_decision";
		case ExternalSourceClass::Government: return "government";
		case ExternalSourceClass::SecondaryLegalAnalysis: return "secondary_legal_analysis";
		case ExternalSourceClass::GeneralWeb: return "general_web";
	}
	return "general_web";
}

inline std::optional<ExternalSourceClass> external_source_class_from_string(
		const std::string &value) noexcept {
	if (value == "official_legislation") return ExternalSourceClass::OfficialLegislation;
	if (value == "court_decision") return ExternalSourceClass::CourtDecision;
	if (value == "government") return ExternalSourceClass::Government;
	if (value == "secondary_legal_analysis") return ExternalSourceClass::SecondaryLegalAnalysis;
	if (value == "general_web") return ExternalSourceClass::GeneralWeb;
	return std::nullopt;
}

struct ExternalEvidenceReference {
	std::string evidence_id;
	std::string url;
	std::optional<std::string> title;
	std::optional<std::string> domain;
	std::string retrieved_at;
	ExternalSourceClass source_class = ExternalSourceClass::GeneralWeb;
	std::optional<std::string> jurisdiction;
	std::optional<std::string> publication_date;
	std::optional<std::string> decision_date;
	std::optional<std::string> case_number;
	std::optional<std::string> court;
	// Adapter-reported search snippet, attributed answer span, or fetched
	// content. The hash covers this excerpt, not an independently read page.
	std::string excerpt;
	std::string content_hash;
	std::optional<std::string> provider_source_id;
	// Native adapter provenance; absent in records created before Phase 24.1D.
	std::optional<std::string> research_provider_id;
	std::optional<std::string> research_engine;
	ExternalEvidenceContentKind content_kind = ExternalEvidenceContentKind::SearchResult;

	[[nodiscard]] std::string marker() const {
		return "[EXTERNAL:" + evidence_id + "]";
	}
};

struct ResearchPolicy {
	std::optional<std::string> jurisdiction;
	bool freshness_required = false;
	std::vector<std::string> source_preferences;
	std::vector<std::string> allowed_domains;
	std::vector<std::string> blocked_domains;
	std::size_t maximum_tool_calls = 4;
	std::atomic_bool *cancellation = nullptr;
};

struct ResearchRequest {
	std::string query;
	ResearchPolicy policy;
};

struct ResearchResponse {
	std::string provider_id;
	std::string summary;
	std::vector<ExternalEvidenceReference> evidence;
	// The hosted model the research call ran on and the usage its response
	// reported (unset fields: not reported).
	std::string model_id;
	providers::UsageMetadata usage;
};

// One opened source. `provider_id`, `model_id` and `usage` are set only when
// opening called a hosted service (a fetch); resolving a cached search result
// locally calls nothing and bills nothing.
struct ResearchOpenResult {
	ExternalEvidenceReference evidence;
	std::string provider_id;
	std::string model_id;
	std::optional<providers::UsageMetadata> usage;
};

// A hosted research call that the service answered (and billed) but that
// produced nothing usable: the failure keeps what the service reported.
class BilledResearchFailure final : public std::runtime_error {
public:
	BilledResearchFailure(const std::string &message, std::string providerId, std::string modelId,
			providers::UsageMetadata reportedUsage)
		: std::runtime_error(message), provider_id(std::move(providerId)), model_id(std::move(modelId)),
		  usage(std::move(reportedUsage)) {}
	std::string provider_id;
	std::string model_id;
	providers::UsageMetadata usage;
};

} // namespace singularity::research
