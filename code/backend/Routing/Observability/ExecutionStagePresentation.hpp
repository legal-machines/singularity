#pragma once

// The single canonical mapping from Singularity's real, durable, already-tested
// execution state (MatterTask::operation_stage/operation_execution_state -
// ChatExecution.cpp/MatterRequestExecution.cpp's own persist_operation_
// transition()/direct field assignments, restart-proof by construction since
// they are the SAME fields ChatExecutionReloadAcceptance.cpp already proves
// survive a fresh process) to the small, calm, truthful, USER-FACING product
// vocabulary this file defines. One concept, one source of truth: this file
// invents no new durable state and duplicates no existing one - it only
// translates.
//
// Never used to decide behavior. Presentation only.

#include <optional>
#include <string>
#include <utility>

namespace singularity::routing::observability {

// The small, stable, user-meaningful vocabulary. Deliberately coarser than
// MatterTask::operation_stage's own internal values and far coarser than
// OperationEventKind (Routing/Observability/OperationEvent.hpp) - that finer
// stream stays exactly what it already is: an internal/diagnostic signal,
// never renamed or repurposed into the product vocabulary here.
enum class ExecutionStage {
	Preparing,
	ReadingSources,
	ProtectingInformation,
	RestoringInformation,
	PreparingContext,
	Reasoning,
	Researching,
	Drafting,
	CheckingEvidence,
	Reviewing,
	PreparingChanges,
	WaitingForApproval,
	WaitingForAnswer,
	ApplyingChanges,
	Verifying,
	Saving,
	Completed,
	Failed,
	Cancelled,
};

struct ExecutionStagePresentation {
	// Stable, localization-ready key - never the English text itself, so a
	// future locale table can key off this without parsing prose.
	std::string presentation_key;
	// Polished default English product copy. Calm, truthful, no engineering
	// jargon (Section 6's own explicit bad examples - "Running PromptAssembler",
	// "Schema repair #1" - must never appear here).
	std::string text;
};

// `text` for stages whose copy depends on a truthful count (ReadingSources,
// CheckingEvidence) is deliberately generic here - callers with a real count
// use reading_sources_text()/checking_evidence_text() below instead, so a
// caller that only has the stage (no count yet available) still gets honest,
// non-numeric copy rather than a fabricated or stale number.
inline ExecutionStagePresentation presentation_for(ExecutionStage stage) {
	switch (stage) {
		case ExecutionStage::Preparing: return {"preparing", "Preparing your request"};
		case ExecutionStage::ReadingSources: return {"reading_sources", "Reading your sources"};
		case ExecutionStage::ProtectingInformation: return {"protecting_information", "Protecting sensitive information"};
		case ExecutionStage::RestoringInformation: return {"restoring_information", "Restoring protected details"};
		case ExecutionStage::PreparingContext: return {"preparing_context", "Preparing the relevant context"};
		case ExecutionStage::Reasoning: return {"reasoning", "Working through the request"};
		case ExecutionStage::Researching: return {"researching", "Researching supporting information"};
		case ExecutionStage::Drafting: return {"drafting", "Drafting the result"};
		case ExecutionStage::CheckingEvidence: return {"checking_evidence", "Checking supporting evidence"};
		case ExecutionStage::Reviewing: return {"reviewing", "Reviewing the result"};
		case ExecutionStage::PreparingChanges: return {"preparing_changes", "Preparing your changes"};
		case ExecutionStage::WaitingForApproval: return {"waiting_for_approval", "Waiting for your approval"};
		case ExecutionStage::WaitingForAnswer: return {"waiting_for_answer", "Waiting for your answer"};
		case ExecutionStage::ApplyingChanges: return {"applying_changes", "Applying approved changes"};
		case ExecutionStage::Verifying: return {"verifying", "Verifying the document"};
		case ExecutionStage::Saving: return {"saving", "Saving the result"};
		case ExecutionStage::Completed: return {"completed", "Done"};
		case ExecutionStage::Failed: return {"failed", "This request could not be completed"};
		case ExecutionStage::Cancelled: return {"cancelled", "Cancelled"};
	}
	return {"unknown", "Working"};
}

inline ExecutionStagePresentation reading_sources_text(int count) {
	if (count <= 0) return presentation_for(ExecutionStage::ReadingSources);
	return {"reading_sources", "Reading " + std::to_string(count) + (count == 1 ? " source" : " sources")};
}

inline ExecutionStagePresentation reading_sources_progress_text(int completed, int total) {
	if (total <= 0) return presentation_for(ExecutionStage::ReadingSources);
	return {"reading_sources", "Reading sources \xC2\xB7 " + std::to_string(completed) + " of "
		+ std::to_string(total)};
}

// Singularity reading more of the Project for the model in the middle of its work:
// its own steps, never the initial reading of the sources come round again.
// What it searched for is Project content: it is on the operation's own record,
// never in this presentation.
inline ExecutionStagePresentation project_inspection_text(const std::string &tool) {
	if (tool == "project.search") return {"searching_project", "Searching the Matter"};
	if (tool == "project.list") return {"reading_project", "Looking through the Matter's documents"};
	return {"reading_project", "Reading more of the Matter"};
}

inline ExecutionStagePresentation checking_evidence_text(int completed, int total) {
	if (total <= 0) return presentation_for(ExecutionStage::CheckingEvidence);
	return {"checking_evidence", "Checking evidence \xC2\xB7 " + std::to_string(completed) + " of "
		+ std::to_string(total) + (total == 1 ? " objective" : " objectives")};
}

inline ExecutionStagePresentation reviewing_text(int pass, int max_passes) {
	if (max_passes <= 0) return presentation_for(ExecutionStage::Reviewing);
	return {"reviewing", "Reviewing the result \xC2\xB7 pass " + std::to_string(pass) + " of max "
		+ std::to_string(max_passes)};
}

// The one real translation Singularity's durable operation_stage values
// (ChatExecution.cpp's persist_operation_transition()/MatterRequestExecution.
// cpp's direct task.operation_stage assignments - both already restart-tested)
// need. `refinement`, when supplied, is a live, in-process-only hint from the
// current OperationEvent stream (Routing/Observability/OperationEvent.hpp) -
// e.g. a Papinian protect/restore window, an active research loop, a
// Professional-Review-tagged model turn - used only to present a more
// specific truthful stage while the durable value is still the coarse
// "model"; it is NEVER required for restart truthfulness (a fresh process
// with no live event stream simply omits it and still gets an honest,
// non-jargon stage from the durable value alone - proving Section 20's own
// requirement: "Working through the request", never "Thinking" or "Starting
// over", for a task reloaded mid-flight).
inline ExecutionStage execution_stage_from_operation_stage(
		const std::string &operation_stage, const std::string &operation_execution_state,
		std::optional<ExecutionStage> refinement = std::nullopt) {
	if (operation_execution_state == "failed" || operation_execution_state == "conflict") return ExecutionStage::Failed;
	if (operation_execution_state == "stopped") return ExecutionStage::Cancelled;
	if (operation_execution_state == "completed") return ExecutionStage::Completed;
	if (operation_execution_state == "waiting_for_approval" || operation_stage == "approval")
		return ExecutionStage::WaitingForApproval;
	if (operation_execution_state == "waiting_for_answer" || operation_stage == "question")
		return ExecutionStage::WaitingForAnswer;
	if (operation_stage == "preparing") return ExecutionStage::Preparing;
	if (operation_stage == "commit_prepared") return ExecutionStage::ApplyingChanges;
	if (operation_stage == "committed") return ExecutionStage::Verifying;
	// operation_stage == "model" (the coarse "generation is happening" state)
	// or anything else not yet given a more specific durable value: the live
	// refinement, when one exists for THIS process, may narrow it further.
	if (refinement.has_value()) return *refinement;
	return ExecutionStage::Reasoning;
}

} // namespace singularity::routing::observability
