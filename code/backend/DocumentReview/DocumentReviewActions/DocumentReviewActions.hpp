#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "DocumentReview/Types/DocumentReview.hpp"

namespace singularity::document_review {

// A refusal a lawyer reads word for word: what cannot be done, and why.
class ReviewRefused final : public std::runtime_error {
public:
	explicit ReviewRefused(const std::string& message)
		: std::runtime_error(message)
	{
	}
};

// The reviews of one Matter, of one of its threads when `chatId` is given,
// the newest first.
std::vector<DocumentReview> listReviews(const std::string& projectId, const std::string& chatId = std::string());
DocumentReview getReview(const std::string& projectId, const std::string& reviewId);

// The review a colleague is working on, or has handed in, in a thread and not
// yet had accepted: where a new request to it there is a revision.
std::optional<DocumentReview> openReviewInThread(const std::string& projectId, const std::string& chatId,
	const std::string& agentId);

// A new review, kept in the Matter.
DocumentReview addReview(const std::string& projectId, DocumentReview review);

// One change to one review, saved as one operation of the Matter and said to
// whoever listens (matter.changed). The review is read again under the lock,
// so a decision and a run never write over each other.
DocumentReview changeReview(const std::string& projectId, const std::string& reviewId,
	const std::function<void(DocumentReview&)>& change);

// The assignments colleagues carried out in a Matter, of one thread when
// `chatId` is given, the newest first; a new one kept; one changed under the
// lock, as a review is (D-119).
std::vector<AssignmentRecord> listAssignments(const std::string& projectId, const std::string& chatId = std::string());
AssignmentRecord addAssignment(const std::string& projectId, AssignmentRecord record);
AssignmentRecord changeAssignment(const std::string& projectId, const std::string& recordId,
	const std::function<void(AssignmentRecord&)>& change);

// The lawyer's answer to one of an assignment's decisions (D-120): recorded
// with who gave it and when, once; and the words the lawyer says to the
// colleague in the thread - "Section 2.1 of the NDA: keep the carve-out." - which
// the colleague takes up as any message of theirs.
struct DecisionAnswer {
	AssignmentRecord record;
	std::string words;
	// Whether the words name the colleague, who takes the work up again with
	// them: only while the work waits on the lawyer and no work in line in its
	// thread will take it up. Work running now reads the answer from its record
	// before its next step; named as well, the colleague would line the same
	// work up a second time (2026-10-31: two answers given one after the other).
	bool namesColleague = true;
};
DecisionAnswer answerDecision(const std::string& projectId, const std::string& recordId, const std::string& decisionId,
	const std::string& answer);

// An assignment as a lawyer reads it now (D-119): where each step's review
// stands - waiting for acceptance, accepted (by whom, when, as written or as
// changed since), returned, replaced by a later version - and whether its
// email is ready to send: only while every draft it carries is the version
// that was accepted, byte for byte as it was attached.
struct AssignmentStepState {
	// The latest version's state: "ready_for_review", "accepted", "returned",
	// "superseded", "failed", "unchanged", "running" ...; "" with no review.
	std::string state;
	int latestVersion = 0;
	std::string decidedBy;
	std::string decidedAt;
	bool isAsWritten = true;
	std::string words;
};
struct AssignmentEmailReadiness {
	bool isReady = false;
	std::vector<std::string> waitingFor;
	std::vector<std::string> replaced;
	std::vector<std::string> changedSince;
	std::string words;
};
struct AssignmentView {
	AssignmentRecord record;
	std::vector<AssignmentStepState> steps;
	AssignmentEmailReadiness email;
};
std::vector<AssignmentView> viewAssignments(const std::string& projectId, const std::string& chatId = std::string());

// Marks a turn of the thread as one of a review's (MatterTask::
// document_review_id), with the work products it hands in.
void linkTurn(const std::string& projectId, const std::string& taskId, const std::string& reviewId,
	const std::vector<std::string>& workProductIds = std::vector<std::string>());

// The lawyer's decision on the version waiting for it. Accepting records who
// accepted which version, and when; it neither closes the thread nor sends
// anything anywhere. Returning records the lawyer's comments and gives them
// to the colleague as its next request, so it revises the draft. Either is
// said in the thread as the lawyer's own message. Only the person at this
// computer decides here, and only the latest version waiting for review.
struct DecisionCommand {
	std::string projectId;
	std::string reviewId;
	int version = 0;
	// "accept" or "return".
	std::string decision;
	std::string comment;
};

DocumentReview decide(const DecisionCommand& command);

// The lawyer's decision on a document a colleague wrote (D-141), as on a
// revised draft (D-143): accepting records who accepted it, when, and the file
// as it is now - as written, or as someone changed it since in Word; returning
// records what should change and gives it to the colleague as its next
// request, so it revises its document. Either is said in the thread as the
// lawyer's own message. A document is decided once: a returned one is
// followed by the colleague's revision, which is decided as any revision is.
struct WrittenDecisionCommand {
	std::string projectId;
	std::string assignmentId;
	// The step that wrote it, from 1.
	int step = 0;
	// "accept" or "return".
	std::string decision;
	std::string comment;
};

AssignmentRecord decideWritten(const WrittenDecisionCommand& command);

// A later pass at a draft that ended with no draft of its own - it failed,
// stopped, or changed nothing - never takes the earlier one away (2026-10-29:
// Unicorn's second pass at the IC deck stopped when Papinian Guard's model went
// away, and version 1, a good draft, could no longer be accepted; 2026-10-30: a
// pass that changed nothing left the closing saying nothing was done). The pass is taken out of the review, and the version it
// would have replaced waits for the lawyer again; what the pass cost stays in
// the assignment's record. A first pass that ends so stays, failed, for "Try
// again". Gives the number of the version that waits again, or 0.
int giveBackEarlierVersion(DocumentReview& review);

// The name of a document a colleague wrote that no lawyer has accepted yet,
// among a Matter's assignments - "Project Frost - APA Issues List" - so an
// email that carries it waits for it (D-143); "" for a document accepted, or
// one no colleague wrote.
std::string writtenAwaitingAcceptance(const std::vector<AssignmentRecord>& records, const std::string& documentId);

// "Try again" on a version that failed or waits for information: the
// colleague is given the same request again, in the same thread.
DocumentReview retry(const std::string& projectId, const std::string& reviewId);

// What this computer has spent on work by external models, kept beside its
// records (DocumentIntelligence/review-budget.json), and the limit it chose on
// that, if any: a hard stop before a request is sent. There is none unless one
// is chosen there, as "chosenLimitUsd" - the owner removed the limits on
// 2026-10-26 (D-132). The "limitUsd" earlier versions wrote is not read: it
// was their default, $2 and at most $4, written back with every call.
struct ReviewBudget {
	std::optional<double> limitUsd;
	long long spentMicros = 0;
	int calls = 0;
	int estimatedCalls = 0;
	// The prices a request is bounded by before it is sent, per million
	// tokens: above what a reviewing model costs, so the bound is never low.
	double ceilingInputPerMillionUsd = 5.0;
	double ceilingOutputPerMillionUsd = 20.0;
};

ReviewBudget loadReviewBudget();
void recordReviewSpend(long long micros, bool isEstimated);
// The most one request of `promptCharacters` with at most `maximumOutputTokens`
// can cost, at the budget's ceiling prices, in micro-dollars.
long long reviewCallCeilingMicros(std::size_t promptCharacters, std::uint64_t maximumOutputTokens,
	const ReviewBudget& budget);
// The most one request can cost, in micro-dollars: at the model's own prices,
// as its provider reports them, where it does; at the budget's ceiling prices
// where it does not. With no limit on what the model writes, there is no bound.
std::optional<long long> reviewCostBoundMicros(std::size_t promptCharacters, std::optional<std::uint64_t> outputTokens,
	std::optional<double> inputPricePerToken, std::optional<double> outputPricePerToken, const ReviewBudget& budget);

// The least a review, and a step of a colleague's work, may be allowed to
// write: below it an answer is cut short, so the request is not sent at all.
inline constexpr std::uint64_t kLeastReviewOutputTokens = 16000;
inline constexpr std::uint64_t kLeastStepOutputTokens = 4000;

// The most a request may ask the model to write so that, at worst, it costs
// what is left of the budget and no more (D-118): `wanted` where that fits,
// else what the remainder pays for, at the same prices the bound uses. Zero
// when not even `least` tokens fit (or `wanted`, where it is smaller): the
// request is then not sent. A chosen limit stays a hard stop either way; with
// none chosen, `wanted` (D-132).
std::uint64_t affordableOutputTokens(std::size_t promptCharacters, std::uint64_t wanted,
	std::optional<double> inputPricePerToken, std::optional<double> outputPricePerToken, const ReviewBudget& budget,
	std::uint64_t least);

} // namespace singularity::document_review
