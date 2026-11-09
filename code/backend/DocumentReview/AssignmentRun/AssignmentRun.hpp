#pragma once

#include "DocumentReview/DocumentReviewRun/DocumentReviewRun.hpp"
#include "DocumentReview/Types/DocumentReview.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace singularity::matters {
struct Matter;
}

namespace singularity::document_review {

// A colleague's work on what a lawyer gives it in a thread, for every
// colleague, with or without a Playbook. The colleague understands the request
// first - from the lawyer's words, the thread and the Matter's documents - and
// then answers in words, asks the one question that blocks the work, or does
// the work: one document or several, in the order the work needs, each in a
// copy beside its original that the lawyer accepts or returns, and, when the
// lawyer asks, a draft email with those copies attached. Every copy is read
// back from the saved file before anything is said about it, and the
// colleague's messages say only what was read back. Nothing is ever sent and no
// original is ever written.
struct ColleagueWorkOutcome {
	// "answered": the colleague answered in its talk (ColleagueTalk), its words
	// posted in the conversation. "answer": a request answered in words, by the
	// colleague's ordinary answer in the thread - nothing was done here.
	// Otherwise "completed", "waiting", "failed", "cancelled" or "interrupted",
	// as a review run's.
	std::string status = "failed";
	std::string reason;
	std::string operationId;
	std::optional<double> cost;
	std::string currency;
	std::string excerpt;
	std::optional<std::string> workProductId;
	// A failure the colleague said in the conversation already, in its own
	// words ("I can't answer right now: ..."): the run's report does not say
	// it again.
	bool isFailureSaid = false;
};

ColleagueWorkOutcome runColleagueWork(const ReviewRunInput& input);

// Whether the pictures members attached show a writing: each of its
// distinctive words stands among `pictureWords` - the pictures' words as this
// computer read them, a letter misread at most - and its legal form (Inc.,
// LLC, Ltd.) aside, as a logo shows a company's name without it. What a
// member's own picture shows, the member asks about: a search may name it,
// though the Matter knows it as one of its identities.
bool isShownInPicture(const std::string& writing, const std::string& pictureWords);

// Any other symbol stays, and the search is refused as before; what is given
// back still meets the research tools' last check (searchProtectedValues).
std::string withFoundValues(const std::string& written, const std::string& found,
	const std::shared_ptr<protected_api::papinian::OperationProtectedSymbolTable>& symbols);

// The values of this work's table a search may not carry: each one the
// client's material holds (`held`; all of them where that cannot be read),
// and the Matter's identities, but for a form a member's picture shows and a
// register or law site Singularity's own words name as a source to search
// (USPTO, TSDR, Justia, Google Patents, CourtListener) - whatever the web
// writes. The firm's own terms stay protected besides, in webContext.
std::vector<std::string> searchProtectedValues(const protected_api::papinian::OperationProtectedSymbolTable& symbols,
	const std::optional<std::set<std::string>>& held, const std::string& pictureWords,
	const std::set<std::string>& ownOnly = {});

// The names, lower-cased, of the Matter's identities known only from its
// colleagues' own documents - a new document or a revised draft each still as
// its colleague saved it, never an email draft - which no lawyer confirmed and
// no member's words write (`membersWords`): the colleagues' own writing from
// the web and the picture, never the client's material. They stay masked to
// the AI provider; they never keep the colleague's own search back.
std::set<std::string> namesOnlyInColleaguesOwn(const matters::Matter& matter, const std::string& membersWords);

// The version waiting for the lawyer, made again from its original without the
// changes they leave out: a workbook's cells, a deck's passages, a text's
// passages or a PDF's marks written once more as the version wrote them - no
// model is asked - read back, and handed in as the next version, the lawyer's
// own word for it said in the thread. Only while the original reads exactly as
// the version read it. A Word draft's changes are tracked changes, rejected in
// Word itself: never here, so approving the colleague's work stays apart from
// accepting Word's changes.
DocumentReview leaveOutChanges(const std::string& projectId, const std::string& reviewId, int version,
	const std::vector<std::string>& changeIds);

// ---- A document's part of the work, as the colleague's loop asks for it ----

struct AssignmentStep {
	std::string documentId;
	std::string instruction;
	std::vector<std::string> basisDocumentIds;
	// The Playbook that covers the document; empty when none does.
	std::string playbookId;
	// "change", or for a PDF "annotate" or "redact".
	std::string action = "change";
	// What the colleague calls the document when it speaks of it: "the NDA".
	std::string label;
};

struct AssignmentEmail {
	bool isWanted = false;
	std::string to;
	std::string cc;
	std::string subject;
	std::string instruction;
	// Documents of the Matter attached as they are, beside the drafts this
	// work makes: an accepted draft, a signed copy.
	std::vector<std::string> attachDocumentIds;
};

} // namespace singularity::document_review
