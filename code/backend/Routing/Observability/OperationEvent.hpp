#pragma once

// The smallest project-consistent observer/event-sink abstraction needed to
// expose real Singularity execution to a terminal client (Singularity CLI) and to
// Phase 24.1D acceptance/debug execution, without scattering std::cout
// throughout domain code and without a second execution pipeline.
//
// Production code (ResearchToolLoop.hpp, ProtectedInference.cpp, ...) calls
// emit() at real decision points. Default behavior is a single pointer
// comparison against nullptr - effectively no-op until a consumer
// registers a sink via ScopedEventSink, and thread-local so two concurrent
// operations never cross-talk even though the real dispatch path
// (NativeBridge::Impl::handle_request -> BackgroundExecutor::submit(),
// see Bridge/NativeBridge/NativeBridge.cpp) reuses a small pool of OS
// threads across many operations rather than giving each one a dedicated
// thread. This is NOT a single mutable process-global sink - isolation is
// proven under genuine concurrent thread-pool reuse (200 operations across
// 4 real BackgroundExecutor worker threads, zero cross-operation event
// leakage) by OperationEvent.test.cpp. The one rule that proof depends on,
// and that any future consumer must follow: construct ScopedEventSink from
// INSIDE the unit of work that actually executes on the worker thread (as
// SingularityCli.cpp's Session does around its own chat.send call), never on the
// thread that merely submits/dispatches the work - the submitting thread
// and the executing thread are not the same thread under BackgroundExecutor,
// so a sink registered on the wrong one would silently observe nothing,
// never another operation's events. The GUI does not currently subscribe;
// nothing about this type is GUI-specific, but nothing requires the GUI to
// change either.
//
// `details` carries privacy-safe structured facts (counts, byte sizes,
// ids, categories) - always safe to print. `forensic_payload`, when set, is
// the exact real content a genuinely forensic-level event has on hand at
// the moment it is emitted (an outbound provider body already serialized
// and already past the FinalPayloadValidator/Papinian boundary, a raw
// provider response, entity detection detail, ...) - production code only
// ever populates it with content it legitimately already possesses at that
// point, never something it goes out of its way to gather. This type makes
// no policy decision about whether forensic_payload should actually be
// shown - that is the SINK's own responsibility (Singularity CLI's /trace
// level), matching "the CLI configures the operation and subscribes to
// events; it must not own the operation." Production code must never
// attach a credential, API key, or Authorization header value here.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace singularity::routing::observability {

// Whether forensic payloads - the prompt before protection, the protection's
// original-to-symbol mapping, the exact provider body, a raw response - can
// reach any sink at all in this build. The shipping application is compiled
// with SINGULARITY_NO_FORENSIC_OBSERVABILITY (deploy/backend/CMakeLists.txt,
// checked by deploy/scripts/check_forensic_gate.py): there has_sink() is
// always false, so no call site ever builds such a copy, and emit() drops one
// a call site attached anyway. Only developer tooling (the CLI's /trace
// forensic and its capture, the acceptance runners) and tests are built
// without it.
#if defined(SINGULARITY_NO_FORENSIC_OBSERVABILITY)
inline constexpr bool kForensicObservability = false;
#else
inline constexpr bool kForensicObservability = true;
#endif

enum class OperationEventKind {
	OperationStart,
	SourceReadStart,
	SourceReadProgress,
	SourceReadEnd,
	ProjectContextBuildStart,
	ProjectContextBuildEnd,
	PapinianProtectStart,
	PapinianProgress,
	PapinianModelInference,
	PapinianReconcile,
	PapinianOffsetResolution,
	PapinianMappingBuild,
	PapinianPayloadReady,
	PapinianRestoreProgress,
	ProviderResultParsed,
	PapinianProtectEnd,
	PapinianRestoreStart,
	PapinianRestoreEnd,
	ModelTurnStart,
	ModelTurnEnd,
	ModelResultKind,
	ToolProposal,
	ToolNormalized,
	ResearchQueryCanonicalized,
	ToolValidated,
	ToolExecutionStart,
	ToolExecutionEnd,
	ResearchStart,
	ResearchEnd,
	EvidenceAppended,
	ContextBuildStart,
	ContextCompaction,
	ContextBuildEnd,
	FinalizationWarningIssued,
	ProviderSendStart,
	ProviderWaitHeartbeat,
	ProviderResponseReceived,
	FinalGenerationStart,
	FinalGenerationEnd,
	DocumentActionProposed,
	DocumentPatchValidated,
	ApprovalRequested,
	// The model put a question to the lawyer and the operation waits for the
	// answer (Tool/LawyerQuestion). Its details are counts, never the question.
	QuestionAsked,
	DocumentMutationStart,
	DocumentVerificationStart,
	DocumentVerificationEnd,
	DocumentMutationEnd,
	PersistenceStart,
	PersistenceComplete,
	FreshProcessReloadStart,
	FreshProcessReloadComplete,
	OperationAccepted,
	OperationFailed,
};

inline const char *to_string(OperationEventKind kind) noexcept {
	switch (kind) {
		case OperationEventKind::OperationStart: return "OPERATION_START";
		case OperationEventKind::SourceReadStart: return "SOURCE_READ_START";
		case OperationEventKind::SourceReadProgress: return "SOURCE_READ_PROGRESS";
		case OperationEventKind::SourceReadEnd: return "SOURCE_READ_END";
		case OperationEventKind::ProjectContextBuildStart: return "PROJECT_CONTEXT_BUILD_START";
		case OperationEventKind::ProjectContextBuildEnd: return "PROJECT_CONTEXT_BUILD_END";
		case OperationEventKind::PapinianProtectStart: return "PAPIAN_PROTECT_START";
		case OperationEventKind::PapinianProgress: return "PAPIAN_NER_PROGRESS";
		case OperationEventKind::PapinianModelInference: return "PAPIAN_MODEL_INFERENCE";
		case OperationEventKind::PapinianReconcile: return "PAPIAN_RECONCILE";
		case OperationEventKind::PapinianOffsetResolution: return "PAPIAN_OFFSET_RESOLUTION";
		case OperationEventKind::PapinianMappingBuild: return "PAPIAN_MAPPING_BUILD";
		case OperationEventKind::PapinianPayloadReady: return "PAPIAN_PROTECTED_PAYLOAD_READY";
		case OperationEventKind::PapinianRestoreProgress: return "PAPIAN_RESTORE_PROGRESS";
		case OperationEventKind::ProviderResultParsed: return "PARSED_PROVIDER_RESULT";
		case OperationEventKind::PapinianProtectEnd: return "PAPIAN_PROTECT_END";
		case OperationEventKind::PapinianRestoreStart: return "PAPIAN_RESTORE_START";
		case OperationEventKind::PapinianRestoreEnd: return "PAPIAN_RESTORE_END";
		case OperationEventKind::ModelTurnStart: return "MODEL_TURN_START";
		case OperationEventKind::ModelTurnEnd: return "MODEL_TURN_END";
		case OperationEventKind::ModelResultKind: return "MODEL_RESULT_KIND";
		case OperationEventKind::ToolProposal: return "TOOL_PROPOSAL";
		case OperationEventKind::ToolNormalized: return "TOOL_NORMALIZED";
		case OperationEventKind::ResearchQueryCanonicalized: return "RESEARCH_QUERY_CANONICALIZED";
		case OperationEventKind::ToolValidated: return "TOOL_VALIDATED";
		case OperationEventKind::ToolExecutionStart: return "TOOL_EXECUTION_START";
		case OperationEventKind::ToolExecutionEnd: return "TOOL_EXECUTION_END";
		case OperationEventKind::ResearchStart: return "RESEARCH_START";
		case OperationEventKind::ResearchEnd: return "RESEARCH_END";
		case OperationEventKind::EvidenceAppended: return "EVIDENCE_APPENDED";
		case OperationEventKind::ContextBuildStart: return "CONTEXT_BUILD_START";
		case OperationEventKind::ContextCompaction: return "CONTEXT_COMPACTION";
		case OperationEventKind::ContextBuildEnd: return "CONTEXT_BUILD_END";
		case OperationEventKind::FinalizationWarningIssued: return "FINALIZATION_WARNING_ISSUED";
		case OperationEventKind::ProviderSendStart: return "PROVIDER_SEND_START";
		case OperationEventKind::ProviderWaitHeartbeat: return "PROVIDER_WAIT";
		case OperationEventKind::ProviderResponseReceived: return "RAW_PROVIDER_RESPONSE";
		case OperationEventKind::FinalGenerationStart: return "FINAL_GENERATION_START";
		case OperationEventKind::FinalGenerationEnd: return "FINAL_GENERATION_END";
		case OperationEventKind::DocumentActionProposed: return "DOCUMENT_ACTION_PROPOSED";
		case OperationEventKind::DocumentPatchValidated: return "DOCUMENT_PATCH_VALIDATED";
		case OperationEventKind::ApprovalRequested: return "APPROVAL_REQUESTED";
		case OperationEventKind::QuestionAsked: return "QUESTION_ASKED";
		case OperationEventKind::DocumentMutationStart: return "DOCUMENT_MUTATION_START";
		case OperationEventKind::DocumentVerificationStart: return "DOCUMENT_VERIFICATION_START";
		case OperationEventKind::DocumentVerificationEnd: return "DOCUMENT_VERIFICATION_END";
		case OperationEventKind::DocumentMutationEnd: return "DOCUMENT_MUTATION_END";
		case OperationEventKind::PersistenceStart: return "PERSISTENCE_START";
		case OperationEventKind::PersistenceComplete: return "PERSISTENCE_COMPLETE";
		case OperationEventKind::FreshProcessReloadStart: return "FRESH_PROCESS_RELOAD_START";
		case OperationEventKind::FreshProcessReloadComplete: return "FRESH_PROCESS_RELOAD_COMPLETE";
		case OperationEventKind::OperationAccepted: return "OPERATION_ACCEPTED";
		case OperationEventKind::OperationFailed: return "OPERATION_FAILED";
	}
	return "UNKNOWN";
}

struct OperationEvent {
	OperationEventKind kind;
	// A short, safe, human-readable summary - always fine to print in safe
	// trace. Never raw Project/provider content.
	std::string message;
	// Privacy-safe structured facts (counts, byte sizes, ids, categories,
	// provider/model names, canonical tool ids) - always safe to print.
	std::vector<std::pair<std::string, std::string>> details;
	// Set only by a call site that already legitimately holds genuinely
	// forensic-level real content at this exact point (an already-serialized
	// outbound provider body, a raw provider response, real entity/mapping
	// detail, a canonicalized research query). Never a credential, API key,
	// or Authorization header value. Whether this is ever shown is entirely
	// the sink's own decision (Singularity CLI's /trace forensic), never decided
	// here.
	std::optional<std::string> forensic_payload;
	std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
};

using OperationEventSink = std::function<void(const OperationEvent &)>;

class ScopedEventSink;

inline ScopedEventSink *&current_sink_slot() {
	static thread_local ScopedEventSink *sink = nullptr;
	return sink;
}

// RAII registration - thread-local, so it only ever affects the thread that
// constructs it (matching how a real operation, including its own
// tool-approval wait, already runs on one thread in this codebase). Never
// affects any other concurrent operation, never affects the GUI unless the
// GUI itself constructs one.
class ScopedEventSink {
public:
	// `wants_forensic_payload` false marks a presentation-only observer (live
	// progress): its presence alone never makes production code gather
	// forensic plaintext copies - see has_sink().
	explicit ScopedEventSink(OperationEventSink sink, bool forward_to_previous = false,
			bool wants_forensic_payload = true)
		: sink_(std::move(sink)), forwardToPrevious_(forward_to_previous),
		  wantsForensicPayload_(wants_forensic_payload) {
		previous_ = current_sink_slot();
		current_sink_slot() = this;
	}
	~ScopedEventSink() { current_sink_slot() = previous_; }
	ScopedEventSink(const ScopedEventSink &) = delete;
	ScopedEventSink &operator=(const ScopedEventSink &) = delete;

private:
	OperationEventSink sink_;
	ScopedEventSink *previous_ = nullptr;
	bool forwardToPrevious_ = false;
	bool wantsForensicPayload_ = true;

	friend void emit(OperationEvent event);
	friend bool has_sink() noexcept;
};

inline void emit(OperationEvent event) {
	ScopedEventSink *sink = current_sink_slot();
	if (sink == nullptr) return;
	if constexpr (!kForensicObservability) event.forensic_payload.reset();
	// Nested observers are exclusive by default, preserving the original
	// operation-local contract. A diagnostic/presentation observer may opt in
	// to forwarding so any bounded stack of read-only projections (for example
	// TerminalTrace, call taxonomy and ExecutionProgressTracker) can consume
	// the same canonical event without any projection creating a second
	// execution pipeline. Walk the registered chain explicitly: invoking a
	// previous sink's callback directly must not silently stop forwarding at
	// the second observer.
	while (sink != nullptr) {
		if (sink->sink_) sink->sink_(event);
		if (!sink->forwardToPrevious_) break;
		sink = sink->previous_;
	}
}

// True when an observer that receives this thread's events wants forensic
// detail. Every production call site uses this to decide whether to build
// forensic payloads (plaintext copies), so a presentation-only observer
// such as live Chat progress must not turn it on.
inline bool has_sink() noexcept {
	if constexpr (!kForensicObservability) return false;
	ScopedEventSink *sink = current_sink_slot();
	while (sink != nullptr) {
		if (sink->wantsForensicPayload_) return true;
		if (!sink->forwardToPrevious_) return false;
		sink = sink->previous_;
	}
	return false;
}

} // namespace singularity::routing::observability
