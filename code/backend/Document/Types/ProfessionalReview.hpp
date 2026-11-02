#pragma once

#include <optional>
#include <string>
#include <vector>

namespace singularity::documents {

// Shared professional-review vocabulary. Originally declared only inside
// ConnectedDocumentAction.hpp; extracted here so a second, simpler generic
// mutation engine (ProfessionalArtifactAction) can reuse the exact same
// verdict/issue contract - including the JSON shape ChatConversation.cpp's
// professional_review_from_task() already projects to the frontend - rather
// than inventing a competing one. ConnectedDocumentAction.hpp now includes
// this header instead of defining these itself; no behavior change there.
//
// A structurally-valid, completion-gated draft can still be professionally
// incomplete (a real design-partner acceptance run proved this concretely:
// exact field placement, document safety and the completion contract all
// passed, yet the drafted content under-covered available evidence and
// omitted material mitigants). The Professional Review is a second,
// independent gate: one fresh, bounded evaluator turn using the same model,
// run once a draft is otherwise complete and before any commit. THE MODEL
// decides what "professionally complete" means for this exact task; Singularity
// only owns routing the verdict and bounding how many correction cycles that
// may cost. These categories are the SHAPE of a professional defect (what
// kind of gap this is), never its legal content.
enum class ReviewIssueCategory {
	EvidenceCompleteness, Specificity, MaterialMitigants, OutputCompleteness, DraftQuality, ClaimSupport
};

enum class ProfessionalReviewVerdict { Pass, NeedsMoreEvidence, NeedsOutputReplan, NeedsRedraft };

struct ProfessionalReviewIssue {
	ReviewIssueCategory category = ReviewIssueCategory::EvidenceCompleteness;
	std::optional<std::string> evidence_objective_id;
	std::optional<std::string> table_handle;
	std::optional<std::string> field_handle;
	// Canonical source document id (never a filename) the reviewer itself
	// judges may resolve THIS issue - the model's own pointer to where to
	// look next, never Singularity inferring "the unused document must be it".
	// Optional like every other reference above: a genuine draft_quality or
	// specificity defect implicates no particular source at all. Read by
	// execute_task_attempt()'s own NeedsMoreEvidence branch to build a
	// targeted evidence-completion turn (only the named source(s), plus the
	// already-accepted draft) instead of a full redraft - see
	// professional_artifact_targeted_evidence_completion_instruction().
	std::optional<std::string> document_id;
	std::string message;
	std::string suggested_next_action;
};

// THE MODEL OWNS WHAT A SOURCE ACTUALLY CONTRIBUTES. A source being selected,
// readable and available to the model never implies it must materially
// appear in the deliverable - see this file's own header comment on why a
// structurally-valid draft can still be professionally incomplete, and the
// mirror-image mistake this type exists to prevent: a reviewer conflating
// "every selected source was cited" with "the deliverable is complete".
// UsedAsEvidence/ReviewedNoMaterialEvidence/MethodologyOrInstruction/
// IrrelevantToObjective are the only distinctions THIS whole-document-text
// review actually needs to make (it never does ConnectedDocumentAction's own
// progressive per-table document.inspect, so a source is always already
// fully visible to the model - "unavailable"/"skipped" are that engine's own
// concepts, not this one's). ReviewedNoMaterialEvidence is a genuinely valid,
// complete disposition, never itself a defect.
enum class SourceDisposition {
	UsedAsEvidence, ReviewedNoMaterialEvidence, MethodologyOrInstruction, IrrelevantToObjective
};

struct ProfessionalReviewSourceDisposition {
	// Canonical source document id (never a filename - see resolve_
	// professional_artifact_candidates()'s own comment on identity).
	std::string document_id;
	SourceDisposition disposition = SourceDisposition::UsedAsEvidence;
	// Model-owned free text: why this disposition, e.g. "contains the closing
	// date cited in paragraph 2" or "an internal methodology checklist, no
	// case facts". Never parsed or matched by Singularity.
	std::string reason;
};

struct ProfessionalReviewResult {
	ProfessionalReviewVerdict verdict = ProfessionalReviewVerdict::Pass;
	std::vector<ProfessionalReviewIssue> issues;
	// One entry per real Project source document this request's context
	// actually included - see professional_artifact_review_instruction()'s
	// own comment. Empty for a review call that predates this field (kept
	// optional/empty rather than required so ConnectedDocumentAction's own,
	// separate Professional Completion Challenge - which already has its own
	// richer per-table EvidenceObjective coverage tracking, never duplicated
	// here - is free to leave it unset without meaning anything by that).
	std::vector<ProfessionalReviewSourceDisposition> source_dispositions;
};

[[nodiscard]] inline const char *to_string(ReviewIssueCategory category) noexcept {
	switch (category) {
		case ReviewIssueCategory::EvidenceCompleteness: return "evidence_completeness";
		case ReviewIssueCategory::Specificity: return "specificity";
		case ReviewIssueCategory::MaterialMitigants: return "material_mitigants";
		case ReviewIssueCategory::OutputCompleteness: return "output_completeness";
		case ReviewIssueCategory::DraftQuality: return "draft_quality";
		case ReviewIssueCategory::ClaimSupport: return "claim_support";
	}
	return "draft_quality";
}

// Named distinctly from ConnectedDocumentAction.cpp's own private, non-
// noexcept review_issue_category_from_string() (an anonymous-namespace
// helper in the same singularity::documents namespace) purely to avoid a
// declaration collision in that translation unit - not a second, divergent
// mapping: both switch over the exact same six ReviewIssueCategory values.
[[nodiscard]] inline std::optional<ReviewIssueCategory> professional_review_issue_category_from_string(
		const std::string &value) noexcept {
	if (value == "evidence_completeness") return ReviewIssueCategory::EvidenceCompleteness;
	if (value == "specificity") return ReviewIssueCategory::Specificity;
	if (value == "material_mitigants") return ReviewIssueCategory::MaterialMitigants;
	if (value == "output_completeness") return ReviewIssueCategory::OutputCompleteness;
	if (value == "draft_quality") return ReviewIssueCategory::DraftQuality;
	if (value == "claim_support") return ReviewIssueCategory::ClaimSupport;
	return std::nullopt;
}

[[nodiscard]] inline const char *to_string(ProfessionalReviewVerdict verdict) noexcept {
	switch (verdict) {
		case ProfessionalReviewVerdict::Pass: return "pass";
		case ProfessionalReviewVerdict::NeedsMoreEvidence: return "needs_more_evidence";
		case ProfessionalReviewVerdict::NeedsOutputReplan: return "needs_output_replan";
		case ProfessionalReviewVerdict::NeedsRedraft: return "needs_redraft";
	}
	return "needs_redraft";
}

[[nodiscard]] inline std::optional<ProfessionalReviewVerdict> professional_review_verdict_from_string(
		const std::string &value) noexcept {
	if (value == "pass") return ProfessionalReviewVerdict::Pass;
	if (value == "needs_more_evidence") return ProfessionalReviewVerdict::NeedsMoreEvidence;
	if (value == "needs_output_replan") return ProfessionalReviewVerdict::NeedsOutputReplan;
	if (value == "needs_redraft") return ProfessionalReviewVerdict::NeedsRedraft;
	return std::nullopt;
}

[[nodiscard]] inline const char *to_string(SourceDisposition disposition) noexcept {
	switch (disposition) {
		case SourceDisposition::UsedAsEvidence: return "used_as_evidence";
		case SourceDisposition::ReviewedNoMaterialEvidence: return "reviewed_no_material_evidence";
		case SourceDisposition::MethodologyOrInstruction: return "methodology_or_instruction";
		case SourceDisposition::IrrelevantToObjective: return "irrelevant_to_objective";
	}
	return "reviewed_no_material_evidence";
}

[[nodiscard]] inline std::optional<SourceDisposition> source_disposition_from_string(
		const std::string &value) noexcept {
	if (value == "used_as_evidence") return SourceDisposition::UsedAsEvidence;
	if (value == "reviewed_no_material_evidence") return SourceDisposition::ReviewedNoMaterialEvidence;
	if (value == "methodology_or_instruction") return SourceDisposition::MethodologyOrInstruction;
	if (value == "irrelevant_to_objective") return SourceDisposition::IrrelevantToObjective;
	return std::nullopt;
}

} // namespace singularity::documents
