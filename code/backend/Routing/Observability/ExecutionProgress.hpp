#pragma once

// The canonical live progress projection: consumes the existing fine-grained
// OperationEvent stream (Routing/Observability/OperationEvent.hpp - never
// duplicated, only subscribed to, exactly like Cli/TerminalTrace/
// TerminalTrace.hpp already does) and produces one small, typed,
// queryable snapshot in the calm product vocabulary
// (ExecutionStagePresentation.hpp). This is the "one canonical progress
// projection" between production execution and any presentation surface
// (console today, Native Bridge/UI later) - it owns no execution behavior
// and mutates no domain state.
//
// Restart truthfulness does NOT depend on this class: MatterTask's own
// durable operation_stage/operation_execution_state (already restart-tested
// by ChatExecutionReloadAcceptance.cpp) is authoritative on its own via
// execution_stage_from_operation_stage() with no refinement argument. This
// class only adds a same-process, in-flight refinement on top of that while
// an operation is actually live in this process.

#include "Routing/Observability/ExecutionStagePresentation.hpp"
#include "Routing/Observability/OperationEvent.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace singularity::routing::observability {

// Same-process liveness/staleness classification for a stage that has been
// active for a while. Never a fake product failure - see StallClassifier's
// own comment. The real, already-configured hard ceiling this reuses is
// Platform/HttpClient.hpp's own long-request default (600s) - the same
// number that already governs a real provider call's actual hard timeout,
// never a second, invented one.
enum class StallClassification { Normal, LongRunning, UnusuallyLong, Timeout };

inline StallClassification classify_stall(std::chrono::milliseconds stage_elapsed) {
	constexpr auto kLongRunning = std::chrono::seconds(15);
	constexpr auto kUnusuallyLong = std::chrono::seconds(60);
	constexpr auto kHardTimeout = std::chrono::seconds(600); // Platform/HttpClient.hpp's own long-request default.
	if (stage_elapsed >= kHardTimeout) return StallClassification::Timeout;
	if (stage_elapsed >= kUnusuallyLong) return StallClassification::UnusuallyLong;
	if (stage_elapsed >= kLongRunning) return StallClassification::LongRunning;
	return StallClassification::Normal;
}

inline const char *to_string(StallClassification value) noexcept {
	switch (value) {
		case StallClassification::Normal: return "normal";
		case StallClassification::LongRunning: return "long_running";
		case StallClassification::UnusuallyLong: return "unusually_long";
		case StallClassification::Timeout: return "timeout";
	}
	return "normal";
}

// Semantic reason a model/provider call actually happened - Section 12's
// "call taxonomy". Attached as an OperationEvent detail
// (`{"purpose", to_string(...)}`) at each real ModelTurnStart emission site,
// never inferred after the fact from call count alone.
enum class CallPurpose {
	SubstantiveGeneration,
	ResponseRepair,
	Research,
	EvidenceExpansion,
	ProfessionalReview,
	Correction,
	ContextAutomation,
	DirectModelExecution,
	// The continuation after a round of protected operations Singularity worked
	// out at the model's request (DESIGN4 §1.7): its own cost and time bucket.
	LocalComputation,
};

inline const char *to_string(CallPurpose value) noexcept {
	switch (value) {
		case CallPurpose::SubstantiveGeneration: return "substantive_generation";
		case CallPurpose::ResponseRepair: return "response_repair";
		case CallPurpose::Research: return "research";
		case CallPurpose::EvidenceExpansion: return "evidence_expansion";
		case CallPurpose::ProfessionalReview: return "professional_review";
		case CallPurpose::Correction: return "correction";
		case CallPurpose::ContextAutomation: return "context_automation";
		case CallPurpose::DirectModelExecution: return "direct_model_execution";
		case CallPurpose::LocalComputation: return "local_computation";
	}
	return "substantive_generation";
}

struct ExecutionProgressSnapshot {
	std::string stage_key;
	std::string stage_text;
	std::chrono::milliseconds elapsed{0};
	std::chrono::milliseconds stage_elapsed{0};
	std::size_t sequence = 0;
	StallClassification stall = StallClassification::Normal;
	// Time since the execution last emitted any real event - the honest
	// "last heartbeat" of a long model call.
	std::chrono::milliseconds since_last_event{0};
	// Documents the request drew passages from, and how many still reached the
	// model after the capacity budget; unset until context is assembled.
	std::optional<int> candidate_document_count;
	std::optional<int> supplied_document_count;
};

struct ExecutionProgressTransition {
	std::string stage_key;
	std::string stage_text;
	std::chrono::milliseconds elapsed{0};
	std::size_t sequence = 0;
};

struct ExecutionProgressMetrics {
	std::optional<std::chrono::milliseconds> time_to_first_event;
	std::chrono::milliseconds maximum_silent_interval{0};
	std::size_t event_count = 0;
};

// Subscribes to the live OperationEvent stream for exactly one operation
// (constructed the same way TerminalTrace is - from inside the unit of work
// that runs on the real worker thread) and maintains the smallest live state
// needed to answer "what stage, since when, how many events" without
// re-deriving it from raw log text. Thread-safety matches OperationEvent's
// own contract: one instance per in-flight operation, on that operation's
// own thread.
class ExecutionProgressTracker {
public:
	explicit ExecutionProgressTracker(bool subscribe = true, bool forward_to_previous = false,
			bool wants_forensic_payload = true)
		: started_(std::chrono::steady_clock::now()), stageStarted_(started_), lastEvent_(started_) {
		transitions_.push_back(ExecutionProgressTransition{
			presentation_.presentation_key, presentation_.text, std::chrono::milliseconds(0), 0});
		if (subscribe) {
			sink_ = std::make_unique<ScopedEventSink>(
				[this](const OperationEvent &event) { observe(event); }, forward_to_previous, wants_forensic_payload);
		}
	}

	ExecutionProgressTracker(const ExecutionProgressTracker &) = delete;
	ExecutionProgressTracker &operator=(const ExecutionProgressTracker &) = delete;

	ExecutionProgressSnapshot snapshot() const {
		std::lock_guard<std::mutex> lock(mutex_);
		const auto now = std::chrono::steady_clock::now();
		ExecutionProgressSnapshot result;
		result.stage_key = presentation_.presentation_key;
		result.stage_text = presentation_.text;
		result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_);
		result.stage_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stageStarted_);
		result.sequence = sequence_;
		result.stall = classify_stall(result.stage_elapsed);
		result.since_last_event = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEvent_);
		result.candidate_document_count = candidateDocuments_;
		result.supplied_document_count = suppliedDocuments_;
		return result;
	}

	ExecutionProgressMetrics metrics() const {
		std::lock_guard<std::mutex> lock(mutex_);
		ExecutionProgressMetrics result = metrics_;
		if (metrics_.event_count != 0 && !terminal_) {
			const auto currentSilence = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - lastEvent_);
			if (currentSilence > result.maximum_silent_interval)
				result.maximum_silent_interval = currentSilence;
		}
		return result;
	}

	std::vector<ExecutionProgressTransition> transitions() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return transitions_;
	}

	// Public so a single explicitly-composed observer can feed trace,
	// progress and measurement from one event stream. Normal callers simply
	// use the subscribing constructor above.
	void observe(const OperationEvent &event) {
		std::lock_guard<std::mutex> lock(mutex_);
		++sequence_;
		const auto eventTime = event.timestamp < started_ ? started_ : event.timestamp;
		if (metrics_.event_count == 0) {
			metrics_.time_to_first_event = std::chrono::duration_cast<std::chrono::milliseconds>(eventTime - started_);
		} else {
			const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(eventTime - lastEvent_);
			if (silence > metrics_.maximum_silent_interval) metrics_.maximum_silent_interval = silence;
		}
		lastEvent_ = eventTime;
		++metrics_.event_count;
		if (event.kind == OperationEventKind::OperationAccepted
				|| event.kind == OperationEventKind::OperationFailed) terminal_ = true;
		if (event.kind == OperationEventKind::ProjectContextBuildEnd) {
			if (const auto candidates = detail_integer(event, "candidateDocumentCount")) candidateDocuments_ = *candidates;
			if (const auto supplied = detail_integer(event, "suppliedDocumentCount")) suppliedDocuments_ = *supplied;
		}
		if (event.kind == OperationEventKind::ToolExecutionEnd
				&& detail(event, "tool").value_or(std::string()).rfind("project.", 0) == 0)
			inspectingProject_ = false;
		// Choosing which documents to read is a small model call of its own, with
		// its own protection: it belongs to reading the documents, and showing
		// its protection and model steps made the list run "protecting, working,
		// protecting, working" before the request itself had begun.
		const bool automationTurn = detail(event, "purpose").value_or(std::string()) == "context_automation";
		if (event.kind == OperationEventKind::ModelTurnStart && automationTurn) {
			choosingDocuments_ = true;
			return;
		}
		if (event.kind == OperationEventKind::ModelTurnEnd && automationTurn) {
			choosingDocuments_ = false;
			return;
		}
		if (choosingDocuments_ && (event.kind == OperationEventKind::PapinianProtectStart
				|| event.kind == OperationEventKind::PapinianProtectEnd || event.kind == OperationEventKind::ProviderSendStart
				|| event.kind == OperationEventKind::PapinianRestoreStart
				|| event.kind == OperationEventKind::PapinianRestoreEnd)) return;
		const std::optional<ExecutionStage> refinement = refine(event);
		if (!refinement.has_value()) return;
		auto next = presentation_for(*refinement);
		const std::string tool = detail(event, "tool").value_or(std::string());
		if (event.kind == OperationEventKind::ToolExecutionStart && tool.rfind("project.", 0) == 0) {
			next = project_inspection_text(tool);
			inspectingProject_ = true;
		} else if (inspectingProject_ && (event.kind == OperationEventKind::SourceReadStart
				|| event.kind == OperationEventKind::SourceReadProgress
				|| event.kind == OperationEventKind::SourceReadEnd)) {
			// The reading a Project search or read does is that step itself, said
			// already: its per-document progress and its end do not replace it.
			return;
		} else if (event.kind == OperationEventKind::SourceReadStart) {
			const int total = detail_integer(event, "documentCount").value_or(0);
			if (total > 0) next = reading_sources_text(total);
		} else if (event.kind == OperationEventKind::SourceReadProgress) {
			next = reading_sources_progress_text(detail_integer(event, "completed").value_or(0),
				detail_integer(event, "total").value_or(0));
		}
		if (next.presentation_key == presentation_.presentation_key && next.text == presentation_.text) return;
		stageStarted_ = eventTime;
		presentation_ = next;
		transitions_.push_back(ExecutionProgressTransition{presentation_.presentation_key,
			presentation_.text, std::chrono::duration_cast<std::chrono::milliseconds>(eventTime - started_), sequence_});
	}

private:
	static std::optional<std::string> detail(const OperationEvent &event, const std::string &key) {
		for (const auto &entry : event.details) if (entry.first == key) return entry.second;
		return std::nullopt;
	}
	static std::optional<int> detail_integer(const OperationEvent &event, const std::string &key) {
		const auto value = detail(event, key);
		if (!value) return std::nullopt;
		try { return std::stoi(*value); } catch (const std::exception &) { return std::nullopt; }
	}

	// The live, same-process-only refinement from a fine internal event to a
	// coarse product stage - see ExecutionStagePresentation.hpp's own comment
	// on execution_stage_from_operation_stage()'s `refinement` parameter.
	// Deliberately conservative: only the well-understood, unambiguous
	// mappings are included; anything else leaves the current stage alone
	// rather than guessing.
	static std::optional<ExecutionStage> refine(const OperationEvent &event) {
		switch (event.kind) {
			case OperationEventKind::OperationStart: return ExecutionStage::Preparing;
			case OperationEventKind::SourceReadStart: return ExecutionStage::ReadingSources;
			case OperationEventKind::SourceReadProgress: return ExecutionStage::ReadingSources;
			case OperationEventKind::SourceReadEnd:
			case OperationEventKind::ProjectContextBuildStart: return ExecutionStage::PreparingContext;
			case OperationEventKind::PapinianProtectStart: return ExecutionStage::ProtectingInformation;
			case OperationEventKind::ProjectContextBuildEnd: return ExecutionStage::PreparingContext;
			case OperationEventKind::ContextBuildStart: return ExecutionStage::PreparingContext;
			case OperationEventKind::ResearchStart:
				return detail(event, "mode") == std::optional<std::string>("required")
					? std::optional<ExecutionStage>(ExecutionStage::Researching) : std::nullopt;
			case OperationEventKind::ToolExecutionStart: {
				const std::string tool = detail(event, "tool").value_or(std::string());
				if (tool.rfind("web.", 0) == 0) return ExecutionStage::Researching;
				if (tool == "document.inspect") return ExecutionStage::ReadingSources;
				if (tool.rfind("project.", 0) == 0) return ExecutionStage::ReadingSources;
				if (tool == "document.patch.apply" || tool == "artifact.action.apply")
					return ExecutionStage::ApplyingChanges;
				return std::nullopt;
			}
			case OperationEventKind::EvidenceAppended: return ExecutionStage::CheckingEvidence;
			case OperationEventKind::PapinianProtectEnd: return ExecutionStage::Reasoning;
			case OperationEventKind::ProviderSendStart: return ExecutionStage::Reasoning;
			case OperationEventKind::PapinianRestoreStart: return ExecutionStage::RestoringInformation;
			// Restoration ending checks nothing by itself: "Checking sources" is
			// entered only where sources are really checked (EvidenceAppended, an
			// evidence-expansion turn), never forecast from protection.
			case OperationEventKind::PapinianRestoreEnd: return std::nullopt;
			case OperationEventKind::ModelTurnStart: {
				const std::string purpose = detail(event, "purpose").value_or("substantive_generation");
				if (purpose == "professional_review" || purpose == "correction"
						|| purpose == "response_repair") return ExecutionStage::Reviewing;
				if (purpose == "research") return ExecutionStage::Researching;
				if (purpose == "evidence_expansion") return ExecutionStage::CheckingEvidence;
				return ExecutionStage::Reasoning;
			}
			case OperationEventKind::FinalGenerationStart: return ExecutionStage::Drafting;
			case OperationEventKind::DocumentActionProposed: return ExecutionStage::Drafting;
			case OperationEventKind::DocumentPatchValidated: return ExecutionStage::PreparingChanges;
			case OperationEventKind::ApprovalRequested: return ExecutionStage::WaitingForApproval;
			case OperationEventKind::QuestionAsked: return ExecutionStage::WaitingForAnswer;
			case OperationEventKind::DocumentMutationStart: return ExecutionStage::ApplyingChanges;
			case OperationEventKind::DocumentVerificationStart: return ExecutionStage::Verifying;
			case OperationEventKind::PersistenceStart: return ExecutionStage::Saving;
			case OperationEventKind::OperationAccepted: return ExecutionStage::Completed;
			case OperationEventKind::OperationFailed: return ExecutionStage::Failed;
			default: return std::nullopt;
		}
	}

	mutable std::mutex mutex_;
	std::chrono::steady_clock::time_point started_;
	std::chrono::steady_clock::time_point stageStarted_;
	std::chrono::steady_clock::time_point lastEvent_;
	ExecutionStagePresentation presentation_ = presentation_for(ExecutionStage::Preparing);
	std::size_t sequence_ = 0;
	ExecutionProgressMetrics metrics_;
	bool terminal_ = false;
	// A Project search, read or list is running for the model.
	bool inspectingProject_ = false;
	bool choosingDocuments_ = false;
	std::optional<int> candidateDocuments_;
	std::optional<int> suppliedDocuments_;
	std::vector<ExecutionProgressTransition> transitions_;
	std::unique_ptr<ScopedEventSink> sink_;
};

} // namespace singularity::routing::observability
