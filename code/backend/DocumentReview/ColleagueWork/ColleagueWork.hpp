#pragma once

#include "Provider/Types/TaskError.hpp"
#include "Provider/Types/UsageMetadata.hpp"
#include "Provider/Types/PromptCache.hpp"
#include "Provider/Types/ReasoningEffort.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace singularity::document_review {

// What every piece of a colleague's work on documents shares (D-115): the model
// it runs on, a document's words as it reads them, and one call of the model -
// protected, within this computer's budget, and recorded where the lawyer can
// see it.

// The model the colleague `agentId` thinks with: the one its page chose
// (D-185), or else the one the Matter's AI Setup gives its reasoning; for a
// colleague that works only on this computer, a model on it. Throws
// ReviewRefused, in the lawyer's words, when there is none.
routing::ModelTarget modelFor(const std::string& matterId, bool isLocalOnly, const std::string& agentId);

// The computer this colleague runs on, as every member reading its thread
// knows it: "Dana Whitfield's computer". "This computer" read on Marcus's
// screen as his own (the walkthrough of 2026-10-29). "this computer" only
// where no member's name is kept here.
std::string computerWords();

// Who chooses a colleague's model, on its page (D-184, D-185): the member at
// the computer that runs it, by name - its administrator - or "the firm's
// administrator" where no name is kept here. Capitalised to begin a sentence.
std::string modelChooser(bool isSentenceStart);

// Why a call could not be made, in words a lawyer reads, naming `model`;
// `message` is the error's own, read where it says more than its code (an
// account out of credit).
std::string reasonFor(providers::TaskErrorCode code, const std::string& model, const std::string& message = std::string());

// What became of a call at the protection boundary, for its routing record:
// the mode it ran under, "sent", "local", "blocked" or "failed", and for a
// request protection refused, Papinian Guard's own words for why - by kind and
// place, never a value. A record that said "off" for a request protection
// refused would hide that it was protected. Left empty, the log settles it
// (routing::settle_protection_decision).
struct CallDecision {
	std::string mode;
	std::string decision;
	std::string reason;
};

CallDecision decisionOf(const routing::ModelTarget& model, const routing::InferenceRequest& request,
	const std::optional<providers::TaskErrorCode>& error, const std::string& message);

// A provider's own words for why it failed a request, value-free (D-172): what
// it quotes ("..." or `...`) becomes "...", and every key-like or long token is
// "[redacted]", so the log says why a request failed without what it held.
// Five rejections in ten minutes could not be told apart (2026-10-31).
std::string providerWords(const std::string& message);

// The operation Usage and "What left this device" count, with the call logged.
// A call that failed after the model answered was still paid for: `spent` is
// what it cost, kept with the operation and in the log like any other call's.
void recordCall(const std::string& matterId, const std::string& taskId, const routing::ModelTarget& model,
	const routing::InferenceResult* result, const std::optional<providers::TaskErrorCode>& error, long long latency,
	const providers::UsageMetadata* spent = nullptr, const std::optional<std::string>& spentFinish = std::nullopt,
	const std::string& presetId = "document-review", std::size_t documentCount = 1,
	const CallDecision& decided = CallDecision());

// A document's words as a colleague reads them: a workbook's cells by
// reference, with what Excel shows and each formula; a deck's paragraphs
// slide by slide; any other document as the Matter's extractor reads it, a
// scan through text recognition. At most `mostCharacters`, and said where the
// reading stops.
struct DocumentWords {
	std::string text;
	bool isComplete = true;
	std::string problem;
};
DocumentWords wordsOfDocument(const std::filesystem::path& path, std::size_t mostCharacters);

// One call of the model for a colleague's work.
struct ColleagueCall {
	std::string matterId;
	// The conversation or thread the work is for: a design partner's computer
	// keeps the call in that chat's work log (Node/WorkLog, D-257).
	std::string chatId;
	std::string agentId;
	std::string givenByActorId;
	std::string givenByName;
	// What the Matter's record says the call was, never the documents' words:
	// "Assignment plan", "Revision of Amsterdam DD Tracker.xlsx".
	std::string label;
	// The routing log's name for the kind of work.
	std::string presetId;
	std::size_t documentCount = 1;
	std::string reviewId;
	std::string prompt;
	// The prompt in parts, when the call gives it so: `prompt` is their joined
	// text. Each part is protected on its own, and a provider's cache can end
	// where one ends (`promptCache`).
	std::vector<std::string> promptParts;
	// Singularity's own instruction beside the parts, read before them: never
	// scored or rewritten as data (routing::InferenceRequest::prompt_instruction).
	std::string promptInstruction;
	std::shared_ptr<const providers::PromptCache> promptCache;
	// The pictures members attached that the call carries as they are, as image
	// inputs (D-256); none for a call of words only.
	std::shared_ptr<const std::vector<providers::InputPicture>> pictures;
	// A short think for a conversational answer; the model's own otherwise.
	providers::ReasoningEffort reasoningEffort = providers::ReasoningEffort::Default;
	// A colleague's talk: its protection legend speaks a conversation's words.
	bool isConversation = false;
	// The protection table the calls of one talk share, so a name or a value
	// keeps its symbol from one of its calls to the next: the colleague reads
	// one conversation, and the provider's cache of it holds. A table of the
	// call's own when unset. Kept in memory for the talk alone (AG-SEC-3).
	std::shared_ptr<protected_api::papinian::OperationProtectedSymbolTable> symbols;
	std::string schemaName;
	nlohmann::json schema;
	// Most the answer may be, its thinking included; the model's own limit when
	// unset.
	std::optional<std::uint64_t> maximumOutputTokens;
	bool isLocalOnly = false;
	bool isCorrection = false;
	const std::atomic<bool>* cancelled = nullptr;
	// The model for this call when it is not the colleague's own: what gathers
	// its memory (D-206). A colleague kept to this computer takes only one on it.
	std::optional<routing::ModelTarget> model;
	// The routing log's word for the call's part: "reasoning", or "context" for
	// its memory.
	std::string operationRole = "reasoning";
};

struct ColleagueCallResult {
	bool isAnswered = false;
	bool isCancelled = false;
	bool isInterrupted = false;
	// The model cannot be asked now for a cause that clears by itself (D-169):
	// the Mac is locked, so its saved credential cannot be read (interrupted at
	// once); the network is down, or the provider asks to slow down (asked again
	// a few times first). The work is interrupted, never failed: it waits and
	// carries on when the model can be asked, and nothing is said meanwhile.
	bool isUnavailable = false;
	std::string text;
	// Why there is no answer, in the lawyer's words, and the error it was.
	std::string failure;
	std::optional<providers::TaskErrorCode> errorCode;
	routing::ModelTarget model;
	// The provider that served the call, as the gateway named it.
	std::string servedBy;
	// "protected", "unprotected" or "local".
	std::string destination;
	bool isProtectionApplied = false;
	long long inputTokens = 0;
	long long outputTokens = 0;
	// Of the output, the model's thinking, where the provider says.
	long long reasoningTokens = 0;
	// Of the input, what the provider read from its prompt cache, and wrote to it.
	long long cachedInputTokens = 0;
	long long cacheWriteTokens = 0;
	std::optional<long long> costMicros;
	// "provider_reported" or "estimated".
	std::string costSource;
	long long latencyMilliseconds = 0;
	std::string finishReason;
	// The operation of the Matter that records the call.
	std::string taskId;
};

// The call, protected as every call of the colleague is (Papinian Guard on
// everything that leaves this computer), within what is left of this
// computer's budget for colleagues' work by external models - refused before
// anything is sent when the call could cost more - recorded as an operation of
// the Matter and in its routing log, and stopped by a Stop on the colleague or
// Singularity closing. Never throws.
ColleagueCallResult callColleagueModel(const ColleagueCall& call);

// Whether a call that brought no answer is worth asking again as it was
// (D-119): the provider busy or out of reach for a moment, or its answer cut
// off or unreadable - never a Stop, Singularity closing, a refusal, a key,
// the budget, or protection that held.
bool isWorthAskingAgain(const ColleagueCallResult& result);

// A pause before asking again, longer each time, that a Stop or Singularity
// closing ends at once.
void pauseBeforeAskingAgain(int attempt, const std::atomic<bool>* cancelled);
// The pauses, shortened for a test that runs a provider's trouble through.
void setPausesBeforeAskingAgainForTesting(long long first, long long later);

} // namespace singularity::document_review
