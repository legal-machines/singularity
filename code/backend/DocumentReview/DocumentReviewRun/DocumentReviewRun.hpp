#pragma once

#include "DocumentReview/Types/DocumentReview.hpp"
#include "Provider/Types/ReasoningEffort.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::protected_api::papinian {
struct OperationProtectedSymbolTable;
}

namespace singularity::document_review {

class FactsReading;

// One piece of review work a lawyer gave a Playbook colleague in a thread, as
// the agent's run carries it out (Agent/AgentProduction).
struct ReviewRunInput {
	std::string matterId;
	std::string chatId;
	std::string agentId;
	std::string agentName;
	// Who the colleague is and how it answers: its brief.
	std::string agentBrief;
	std::string playbookId;
	// The lawyer's words, and the message they are.
	std::string instruction;
	std::string requestMessageId;
	std::string givenByActorId;
	std::string givenByName;
	std::string runId;
	std::string assignmentId;
	bool isLocalOnly = false;
	const std::atomic<bool>* cancelled = nullptr;
	// One part of a larger assignment, as the colleague's plan gives it: the
	// document this part works on, the documents it rests on, the lawyer's
	// whole assignment in their words (`instruction` is then this part of
	// it) and what the parts before it changed, read back from their drafts.
	// All empty for a review asked for on its own.
	std::string sourceDocumentId;
	std::vector<std::string> supportingDocumentIds;
	std::string assignment;
	std::string earlierInAssignment;
	// The colleague said it is on the whole assignment already: this part says
	// nothing as it begins.
	bool isQuiet = false;
	// A part of an assignment hands in through the assignment's card and its
	// closing message: its own message is kept with the version, never posted,
	// and written by Singularity from what happened, no model asked.
	bool isHandedInByAssignment = false;
	// No Playbook of the firm covers this document: the revision does what the
	// request asks and nothing besides, with the request as its one position.
	bool isRequestOnly = false;
	// The assignment's readings of the Matter's documents, and the drafts it
	// made so far, which stand for their originals in every check of a value
	// against its source. A review asked for on its own reads its own.
	std::shared_ptr<FactsReading> facts;
	std::vector<std::string> assignmentDraftIds;
	// The chat is the member's personal chat with the colleague, not a thread
	// of the Matter.
	bool isOwnChat = false;
	// The colleague took the work on in its talk (ColleagueTalk): the loop does
	// it, never answers in words instead, and says nothing more as it begins -
	// `ackMessageId` is what the colleague said as it took it on.
	bool isTakenOn = false;
	std::string ackMessageId;
	// Its words as it took the work on: its closing message carries on from
	// them.
	std::string ackText;
	// The colleague's own run (its schedule, a change, another's report):
	// `instruction` is its standing work and why it runs now; nobody wrote to it.
	bool isOwnRun = false;
	// Who the colleague is in the firm, as its talk carries it, and where the
	// member who wrote to it stands: their office and their standing to it
	// ("who ranks below you"); empty where the firm does not say.
	std::string agentIdentity;
	std::string askerOffice;
	std::string askerStanding;
	// Told once, when the colleague goes to work - looks something up, or takes
	// work on - after answering in words was all it did so far: its run is at
	// work from then on.
	std::function<void()> workBegins;
	// Told the moment its answer in words is posted: it is neither typing nor at
	// work any more while its run ends.
	std::function<void()> answerGiven;
	// The protection table every model call of one assignment shares - its
	// turns, its revisions, its email - so a name or a value keeps its symbol
	// from one call to the next and the provider reads what the calls before
	// kept. Made where the assignment's work begins; unset, each call has its
	// own. A call of its own renumbered the names, and two turns in a row read
	// nothing of each other from the cache. Memory only, for the assignment
	// alone.
	std::shared_ptr<protected_api::papinian::OperationProtectedSymbolTable> symbols;
	// What the colleague's talk read with its tools before taking the work on -
	// documents, passages, threads, earlier messages - as they read: the
	// client's material its words may carry into the work, which a web search
	// of the work may not name. Memory only.
	std::shared_ptr<std::string> talkMaterial;
	// How long a revision's model thinks: none for a revision the assignment's
	// turn already chose - two of three such answers, thought at length, came
	// back empty and were asked again. The model's own otherwise.
	providers::ReasoningEffort revisionEffort = providers::ReasoningEffort::Default;
};

// What the colleague did in a thread, as Singularity recorded it from the
// saved drafts: each assignment's steps, its decisions and its email. Empty
// when it did nothing there.
std::string colleagueThreadRecord(const std::string& matterId, const std::string& chatId, const std::string& agentId);

struct ReviewRunOutput {
	// "completed", "waiting", "failed", "cancelled" or "interrupted".
	std::string status = "failed";
	std::string reason;
	std::string reviewId;
	int version = 0;
	std::optional<std::string> workProductId;
	std::string operationId;
	std::optional<double> cost;
	std::string currency;
	// The opening of what the colleague said in the thread.
	std::string excerpt;
	// What the version holds, as Singularity read it back from the saved
	// draft: the source, the draft, and each change written, as a lawyer says
	// it ("Section 4.2 (Payment terms): “fifteen (15)” becomes “sixty (60)”").
	std::string sourceName;
	std::string resultDocumentId;
	std::string resultFileName;
	std::vector<std::string> writtenChanges;
	// Where each written change stands ("Section 5.2"), in the same order.
	std::vector<std::string> writtenPlaces;
	// The comments read back in the draft's margin: "Section 5.2: “…”".
	std::vector<std::string> writtenComments;
	std::vector<std::string> openPoints;
	// The same calls as the lawyer takes them: the question, why it
	// matters, where, and the options with the recommendation first.
	std::vector<AssignmentDecision> decisions;
	// What the version cost in calls and tokens, and each call asked again
	// because its answer could not be used, in words.
	int calls = 0;
	long long inputTokens = 0;
	long long outputTokens = 0;
	std::vector<std::string> recoveries;
	// A value a written change gives that no document, the Playbook or the
	// lawyer's words give: "Section 5.2: forty-five (45) days".
	std::vector<std::string> unsourced;
};

// Whether work given to a Playbook colleague in a thread is a document review
// it carries out (rather than a question it answers in words): a request to
// review, revise, mark up or redline, or any request in a thread where it has
// a review not yet accepted - a revision.
bool isReviewRequest(const std::string& matterId, const std::string& chatId, const std::string& agentId,
	const std::string& instruction);

// The review, from the lawyer's request to the draft handed in:
// 1. which review and which version: a new review, the next version of one
//    handed in or returned, or the same version again after a failure;
// 2. the source: the thread's document, the one the request names, or the
//    Matter's one contract - and when that is not clear, the colleague asks
//    which, and the version waits for information;
// 3. preparation (DocumentIntelligence): the source taken in and its
//    structure read, the Playbook's positions, the request prepared;
// 4. the model the Matter's AI Setup gives its reasoning, through Routing's
//    protected execution and Papinian Guard, within this computer's review
//    budget, recorded as an operation of the Matter;
// 5. the answer checked against the source; a revised draft written as Word
//    tracked changes into a new document of the Matter; the work kept as a
//    Work Product;
// 6. the colleague's message in the thread, handing the draft in.
// Each stage is saved as it begins, so the version's state is always the
// truth; every expected failure ends the version as failed, with its reason,
// and a message in the thread - never an exception, and never a spinner.
ReviewRunOutput runDocumentReview(const ReviewRunInput& input);

// Makes the review the work a Playbook colleague does when a lawyer asks it
// for one in a thread (agents::install_thread_document_work): the colleague's
// run hands the work over, and is told how it ended. The application calls it
// once, when it starts; the Agent domain does not depend on this one.
void installForColleagues();

} // namespace singularity::document_review
