#pragma once

#include <optional>
#include <string>
#include <vector>

namespace singularity::document_review {

// A document review an AI colleague carries out in a thread: a lawyer names
// the colleague and asks for it, the colleague reviews the Matter's document
// against the firm's Playbook through the Document Intelligence & Preparation
// Layer, and hands in a revised draft for the lawyer to accept or return
// (the owner's overnight mandate, 2026-10-24). One review may have several
// versions: each request to revise makes the next, and every earlier one is
// kept as it was. Kept in the Matter's own data (`.lm`, "documentReviews"),
// so it reads the same after a restart and on every computer that opens the
// Matter.

// How one run of a version went, for the record a lawyer can open: what was
// prepared, which model answered, what it cost and how long it took. Ids,
// counts and states - never a document's words.
struct ReviewExecution {
	// The colleague's run and the work it was given (Agent/), and the
	// operation Usage and "What left this device" count (MatterTask).
	std::string runId;
	std::string assignmentId;
	std::string operationId;
	std::string providerId;
	std::string modelId;
	// The provider that served the calls, as the gateway named it (D-116).
	std::string servedBy;
	// "protected", "unprotected" or "local".
	std::string destination;
	bool isProtectionApplied = false;
	long long inputTokens = 0;
	long long outputTokens = 0;
	std::optional<long long> costMicros;
	std::string costCurrency;
	// "provider_reported" or "estimated".
	std::string costSource;
	long long latencyMilliseconds = 0;
	long long preparationMilliseconds = 0;
	std::string finishReason;
	std::string startedAt;
	std::string finishedAt;
	// What the preparation was (ContextPreparation's summary) and each stage
	// with when it began, as JSON text.
	std::string preparation;
	std::string stages;
	int attempt = 1;
};

struct ReviewVersion {
	int number = 0;
	// The lawyer's message this version answers, and its words.
	std::string requestMessageId;
	std::string instruction;
	std::string createdAt;
	// "queued", "preparing", "running", "producing", "ready_for_review",
	// "waiting_for_information", "failed", "accepted" or "returned".
	std::string state = "queued";
	// Why it failed, or what it waits for, in words a lawyer reads.
	std::string stateMessage;
	// The revised draft: a new document of the Matter, never the source.
	std::string resultDocumentId;
	std::string resultRelativePath;
	std::string resultFileName;
	std::string resultSha256;
	std::string workProductId;
	// The colleague's message in the thread that hands it in.
	std::string answerMessageId;
	// The validated result (DocumentIntelligence/ReviewResult), as JSON text.
	std::string result;
	ReviewExecution execution;
};

// The lawyer's decision on a version: who, when, which version, and what.
struct ReviewDecision {
	std::string id;
	int version = 0;
	// "accepted" or "returned".
	std::string decision;
	std::string actorId;
	std::string actorName;
	std::string decidedAt;
	std::string comment;
	// The lawyer's message in the thread that says it.
	std::string messageId;
	// What was accepted: the draft's SHA-256 when it was, and whether that is
	// the draft as the colleague wrote it or as someone changed it since in
	// Word, Excel or PowerPoint (D-119).
	std::string sha256;
	bool isAsWritten = true;
	// Changes of the version the lawyer left out, by their ids: the next
	// version is this one without them.
	std::vector<std::string> leftOut;
};

struct DocumentReview {
	std::string id;
	std::string chatId;
	std::string agentId;
	std::string agentName;
	// The source, as it was when the review began. Never changed by it.
	std::string sourceDocumentId;
	std::string sourceRelativePath;
	std::string sourceFileName;
	std::string sourceSha256;
	int sourceVersion = 0;
	std::string playbookId;
	std::string playbookName;
	int playbookVersion = 0;
	std::string requestedByActorId;
	std::string requestedByName;
	std::string createdAt;
	std::string updatedAt;
	// What the structure of the source showed, as JSON text: counts, the
	// exhibits it lacks, the references it cannot resolve, words in it
	// addressed to a model. Never its words.
	std::string structure;
	std::vector<ReviewVersion> versions;
	std::vector<ReviewDecision> decisions;
};

// One assignment a lawyer gave a colleague in a thread, as the colleague
// carried it out (D-115, D-119): what it understood, each step with the
// document it worked on and the review version that keeps that step's own
// record - what it read, proposed, wrote, checked and left (the version's
// result, "record") - the email it drafted, and what the whole took. Kept in
// the Matter (`.lm`, "assignmentRecords"), so it reads the same on every
// computer that opens the Matter. A step's record is never copied here: the
// review version is its one place.
struct AssignmentStepRecord {
	int number = 0;
	std::string documentId;
	std::string sourceName;
	// "word", "workbook", "deck", "pdf" or "text".
	std::string kind;
	// What the plan meant it to do.
	std::string intended;
	std::vector<std::string> basisNames;
	std::string reviewId;
	int version = 0;
	// "waiting" until it begins, "running" while it is worked on; then how it
	// ended when handed in: "completed", "unchanged", "failed", "cancelled" or
	// "interrupted".
	std::string status;
	// What the result is for the lawyer (D-120): "completed", "partial" (done,
	// with changes not written or values that do not reconcile), "blocked"
	// (it waits for an answer), "unchanged" or "failed". Empty until handed in.
	std::string outcome;
	// What the colleague calls the document when it speaks of it: "the NDA".
	std::string label;
	// What was read back from the saved draft, in a lawyer's words: "Revised:
	// 2 cells changed. 1 value still differs from the document it comes from."
	std::string statusWords;
	// What is left for the lawyer.
	std::vector<std::string> remaining;
	int calls = 0;
	// Calls asked again because an answer could not be used: each said in
	// `recoveries`, in words.
	int retries = 0;
	std::vector<std::string> recoveries;
	// A document the colleague wrote (D-141), with no review behind it: its
	// SHA-256 as written, and the lawyers' decisions on it (D-143), accepted as
	// it is or returned with what should change, as a revised draft's are.
	std::string writtenSha256;
	std::vector<ReviewDecision> decisions;
};

// A file an email carries, as it was when attached: which version of which
// review it is, so the email is ready only while that version is the one the
// lawyer accepted.
struct AssignmentAttachment {
	std::string name;
	std::string documentId;
	std::string reviewId;
	int version = 0;
	std::string sha256;
};

struct AssignmentEmailRecord {
	bool isWanted = false;
	// "drafted", "not_drafted" or "waiting" (for drafts to attach).
	std::string state;
	std::string reason;
	std::string documentId;
	std::string name;
	std::string subject;
	std::string to;
	std::string cc;
	std::vector<AssignmentAttachment> attachments;
	// What its body states that no draft or document of the Matter bears out.
	std::vector<std::string> unverified;
	int calls = 0;
	int retries = 0;
	std::vector<std::string> recoveries;
};

// What only the lawyer can decide in an assignment (D-120): the question, why it
// matters, where, the options with the colleague's recommendation first where
// it has one, and whether the next step waits on it - until it is answered.
struct AssignmentDecision {
	// "dec-<n>", one assignment's own.
	std::string id;
	// The step it belongs to, from 1; 0 for the assignment's own (its email).
	int stepNumber = 0;
	std::string reviewId;
	int version = 0;
	std::string documentId;
	std::string documentName;
	// "Section 2.1", "Contracts!L4", "Slide 3"; empty for the whole document.
	std::string place;
	std::string question;
	std::string why;
	std::vector<std::string> options;
	// The first option is the colleague's recommendation; why says what turns
	// on the decision.
	bool isRecommended = false;
	// Why the colleague recommends its first option, in a sentence: shown once,
	// with that option, never repeated elsewhere (D-174). Empty when it gave
	// none, or recommends nothing.
	std::string reasoning;
	// Accepting the draft, or sending the email, waits on it.
	bool isBlocking = false;
	// The colleague's message the decision stands under.
	std::string messageId;
	// "open", "answered", or "withdrawn" when a later version of the draft that
	// raised it replaced that version without raising it again (kept for the
	// record, never put to the member).
	std::string state = "open";
	std::string answer;
	std::string answeredByActorId;
	std::string answeredByName;
	std::string answeredAt;
};

// One call of the colleague's tools in an assignment (D-121): written before it
// runs and its result after, so a run resumed after a stop or a restart knows
// what was done and does nothing twice. The execution's own record, apart from
// the thread and never shown as the conversation.
struct AssignmentCall {
	int turn = 0;
	std::string tool;
	// Its arguments, and the result the model was given, as JSON.
	std::string arguments;
	// "running" until it ends; then "done", "refused" (not permitted, not
	// valid) or "interrupted" (Singularity stopped while it ran).
	std::string state = "running";
	std::string result;
	std::string startedAt;
	std::string finishedAt;
};

struct AssignmentRecord {
	std::string id;
	std::string chatId;
	std::string requestMessageId;
	// The colleague's reply that took the work up, which the assignment's card
	// stands under (D-120).
	std::string ackMessageId;
	// The colleague's last word on it, which its decisions stand under.
	std::string summaryMessageId;
	std::string agentId;
	std::string agentName;
	std::string givenByActorId;
	std::string givenByName;
	// The lawyer's words, and what the colleague understood them to ask.
	std::string instruction;
	std::string understanding;
	// What the plan named that could not be used, and why.
	std::vector<std::string> setAside;
	std::string providerId;
	std::string modelId;
	std::string startedAt;
	std::string finishedAt;
	// "working", "completed", "failed", "cancelled" or "interrupted".
	std::string state = "working";
	std::vector<AssignmentStepRecord> steps;
	AssignmentEmailRecord email;
	std::vector<AssignmentDecision> decisions;
	// The colleague's tool calls, and the handles it knows the Matter's
	// documents by ("DOC_3"), kept so a resumed run reads them as before.
	std::vector<AssignmentCall> toolCalls;
	std::vector<std::pair<std::string, std::string>> handles;
	int turns = 0;
	// The whole of it: every call of the model, the plan's included.
	int calls = 0;
	int retries = 0;
	std::vector<std::string> recoveries;
	long long inputTokens = 0;
	long long outputTokens = 0;
	std::optional<long long> costMicros;
	long long milliseconds = 0;
};

} // namespace singularity::document_review
