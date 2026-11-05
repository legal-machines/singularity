#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/Trace/Trace.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Routing/MatterRequestExecution/MatterRequestExecution.hpp"
#include "WorkProduct/WorkProductDocument/WorkProductDocument.hpp"

#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/ChatContinuity/ChatContinuity.hpp"
#include "Chat/ConversationClearing/ConversationClearing.hpp"
#include "Chat/OperationOrigin/OperationOrigin.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/ProjectEvidence/ProjectEvidence.hpp"
#include "Context/PromptAssembler/PromptAssembler.hpp"
#include "Context/QuoteVerification/QuoteVerification.hpp"
#include "Context/ContextCuration/ContextCuration.hpp"
#include "Context/ContextRegistry/ContextRegistry.hpp"
#include "Context/ControlText/ControlText.hpp"
#include "Context/SourceLabels/SourceLabels.hpp"
#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "Context/SourcePicker/SourcePicker.hpp"
#include "Context/TemporalContext/TemporalContext.hpp"
#include "Document/DocumentCache/DocumentCache.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/ConnectedDocumentAction/ConnectedDocumentAction.hpp"
#include "Document/DirectDocumentWork/DirectDocumentWork.hpp"
#include "Document/ArtifactCapabilityRegistry/ArtifactCapabilityRegistry.hpp"
#include "Document/DocumentJournal/DocumentJournal.hpp"
#include "Document/DocumentPatchTool/DocumentPatchTool.hpp"
#include "Document/ProfessionalArtifactAction/ProfessionalArtifactAction.hpp"
#include "ExternalResearch/ResearchIntent/ResearchIntent.hpp"
#include "Consilium/ConsiliumActions/ConsiliumActions.hpp"
#include "ExternalResearch/ResearchToolLoop/ResearchToolLoop.hpp"
#include "ExternalResearch/ResearchTools/ResearchTools.hpp"
#include "Matter/MatterActions/MatterActions.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBudget.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeProtocol.hpp"
#include "Routing/ProtectedTableSource/ProtectedTableSource.hpp"
#include "PapinianGuard/PreviewSession/PreviewSession.hpp"
#include "PapinianGuard/ProtectedPreview/ProtectedPreview.hpp"
#include "PapinianGuard/SemanticAlgebra/SemanticAlgebra.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/LegalTaskPresets/LegalTaskPresets.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Routing/MemoryDiagnostics/MemoryDiagnostics.hpp"
#include "Routing/Observability/OperationEvent.hpp"
#include "Tool/LawyerQuestion/LawyerQuestion.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"
#include "WorkProduct/ChronologyParser/ChronologyParser.hpp"
#include "WorkProduct/ComparisonParser/ComparisonParser.hpp"
#include "WorkProduct/FindingParser/FindingParser.hpp"
#include "WorkProduct/ObligationParser/ObligationParser.hpp"
#include "WorkProduct/WorkflowDefinitions/WorkflowDefinitions.hpp"
#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <regex>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace singularity::routing::matter_request_execution {
namespace {

std::mutex &source_picker_mutex();
SourcePicker &source_picker();
std::mutex &approval_filed_hook_mutex();
ApprovalFiledHook &approval_filed_hook();

// Below this context window a model keeps its room for the conversation itself:
// it is given neither the guidance on the Project's memory nor the data room's
// journal.
constexpr std::size_t kJournalMinimumWindowTokens = 32000;
// How many times one answer may ask to read more of the Project before it is
// given (Context/ProjectEvidence) - each one continuation turn of the same
// operation, like a research request. What each returns is bounded by the room
// the model's own context still has, never by a fixed size.
constexpr std::size_t kProjectEvidenceRequestsPerAnswer = 4;
// How many times one operation may stop to put a question to the lawyer and
// continue with the answer (Tool/LawyerQuestion) - across every call it makes,
// its review's redraft included. A lawyer asked more than twice in one request
// is being asked what the request should have settled.
constexpr std::size_t kMaximumLawyerQuestions = 2;
// How many rounds of questions about protected values one call may have Singularity
// answer (localFacts) where the protected computation protocol does not
// run. Under protection the protocol's own budgets hold instead
// (PapinianGuard/ProtectedComputation/ComputeBudget.hpp: 4 rounds and 2
// repair rounds a call, 10 rounds and 160 operations a task).
constexpr std::size_t kMaximumLocalFactRounds = 3;

using Json = nlohmann::json;

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// The exact change a pending approval covers, kept in the operation's durable
// checkpoint: a decision is recorded against it and a resumed commit is
// refused unless it is still that change - the same accepted proposal (its
// digest) against the same target document version. Source labels inside the
// change are rendered from current Project metadata at commit, so the
// rendered tool intent is recorded for audit, not compared.
Json approval_binding_json(const tools::ToolApprovalRequest &approval, const std::string &acceptedProposal) {
	Json binding{{"approvalId", approval.approval_id}, {"toolId", approval.tool_id},
		{"intentDigest", approval.intent_digest}, {"proposalDigest", platform::sha256_hex_digest(acceptedProposal)},
		{"targetDocumentId", approval.target_document_id ? Json(*approval.target_document_id) : Json(nullptr)},
		{"targetVersion", approval.target_version ? Json(*approval.target_version) : Json(nullptr)},
		{"targetFingerprint", approval.target_fingerprint ? Json(*approval.target_fingerprint) : Json(nullptr)},
		// What the change was for, in the proposal's own words: kept with the
		// decision, so a later turn knows what the lawyer declined or approved.
		{"summary", approval.summary},
		{"decision", "pending"}};
	// The name a new document is shown under, kept with the decision: the
	// approved document is written under exactly that name.
	const Json change = approval.change_detail_json.empty() ? Json::object()
		: Json::parse(approval.change_detail_json, nullptr, false);
	if (change.is_object() && change.contains("newDocument") && change.at("newDocument").is_string())
		binding["newDocument"] = change.at("newDocument");
	return binding;
}

// A resumed, approved change whose bound target document changed meanwhile.
class ApprovedTargetChanged final : public std::runtime_error {
public:
	ApprovedTargetChanged()
		: std::runtime_error("The document changed after this change was proposed for approval, so nothing was changed. "
			"Send the request again to prepare a change against its current version.") {}
};

// A resumed change a document it was PREPARED FROM changed under: the proposal
// no longer rests on the evidence it was made from.
class ProposalEvidenceChanged final : public std::runtime_error {
public:
	explicit ProposalEvidenceChanged(const std::string &message) : std::runtime_error(message) {}
};

// A change Singularity could not carry out even after the model's one chance to
// correct it. The request itself did not fail: the lawyer receives the model's
// answer and one calm sentence saying nothing was written. The tool's own
// reason, written for the model, stays in the operation's record
// (artifact.refused) and never becomes the lawyer's answer.
class ArtifactNotPlaced final : public std::runtime_error {
public:
	ArtifactNotPlaced(std::string answer, std::string note)
		: std::runtime_error(note), answer(std::move(answer)), note(std::move(note)) {}
	std::string answer;
	std::string note;
};

// That one sentence: what could not be done, to which document, and that
// nothing was written.
std::string change_not_placed_note(const matters::Matter &matter, const std::optional<std::string> &documentId,
		bool createsDocument, bool russian) {
	if (createsDocument)
		return russian ? std::string("\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD1\x81\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD0\xBD\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xB9 \xD0\xB4\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82 \xD0\xB2 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xBC \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB5, \xD0\xBF\xD0\xBE\xD1\x8D\xD1\x82\xD0\xBE\xD0\xBC\xD1\x83 \xD0\xBD\xD0\xB8\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBE.")
			: std::string("Singularity could not create the new document exactly as proposed, so nothing was written.");
	// Said whole, because the answer after it was written for a change the lawyer
	// would see: Golden C (25 Sep 2026) ended "No change will be made unless the
	// proposed edit is approved" with no approval anywhere.
	const std::string after = russian ? std::string(" \xD0\xB2 \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xBC \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB5, \xD0\xBF\xD0\xBE\xD1\x8D\xD1\x82\xD0\xBE\xD0\xBC\xD1\x83 \xD0\xBD\xD0\xB8\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBE \xD0\xB8 \xD0\xBD\xD0\xB8\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBD\xD0\xB5 \xD0\xB6\xD0\xB4\xD1\x91\xD1\x82 \xD0\xB2\xD0\xB0\xD1\x88\xD0\xB5\xD0\xB3\xD0\xBE \xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x81\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8F. \xD0\x92\xD1\x8B \xD0\xBC\xD0\xBE\xD0\xB6\xD0\xB5\xD1\x82\xD0\xB5 \xD1\x81\xD0\xB0\xD0\xBC\xD0\xB8 \xD0\xB2\xD0\xBD\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8 \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5, \xD0\xBE\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB5 \xD0\xBD\xD0\xB8\xD0\xB6\xD0\xB5, \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xBF\xD0\xBE\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD0\xB8\xD1\x82\xD1\x8C \xD0\xB5\xD1\x89\xD1\x91 \xD1\x80\xD0\xB0\xD0\xB7.")
		: std::string(", so nothing was written and nothing is waiting for your approval. You can make the "
			"change described below yourself, or ask again.");
	std::string name;
	if (documentId)
		for (const matters::MatterDocument &document : matter.documents)
			if (document.id == *documentId && !document.name.empty()) name = document.name;
	if (russian)
		return std::string("\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB2\xD0\xBD\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8 \xD1\x8D\xD1\x82\xD0\xBE \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5") + (name.empty() ? std::string() : std::string(" \xD0\xB2 ") + name)
			+ std::string("") + after;
	return name.empty() ? "Singularity could not place this change exactly as proposed" + after
		: "Singularity could not place this change in " + name + " exactly as proposed" + after;
}

// A sentence of the model's answer that speaks of an approval still to come
// for the change ("No change will be made unless the proposed edit is
// approved"). Said beside a change that was not placed, it would tell the
// lawyer to wait for a card that is not there (Golden C, 25 Sep 2026). Read
// narrowly: an approval and the lawyer or the proposed change in the same
// sentence, so a clause the answer quotes ("unless approved in writing by
// Customer") stays.
bool speaks_of_pending_approval(const std::string &sentence) {
	std::string lower;
	lower.reserve(sentence.size());
	for (char character : sentence) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	const auto has = [&](const char *words) { return lower.find(words) != std::string::npos; };
	if (has("approv"))
		for (const char *words : {"you", "no change", "nothing will", "will not be made", "won't be made",
				"proposed edit", "proposed change", "proposed redline", "pending approval", "awaiting approval",
				"for approval", "please"})
			if (has(words)) return true;
	bool russianApproval = false;
	for (const char *root : {"\xD0\xBE\xD0\xB4\xD0\xBE\xD0\xB1\xD1\x80", "\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x81\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD", "\xD1\x83\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB4", "\xD0\xBF\xD0\xBE\xD0\xB4\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB4"})
		if (has(root)) russianApproval = true;
	return russianApproval && (has("\xD0\xB2\xD0\xB0\xD1\x88") || has("\xD0\x92\xD0\xB0\xD1\x88"));
}

// Where the model's answer speaks of an approval still to come: each such
// sentence, with the spaces after it, as [start, end) in the answer.
std::vector<std::pair<std::size_t, std::size_t>> pending_approval_sentences(const std::string &answer) {
	std::vector<std::pair<std::size_t, std::size_t>> talk;
	std::size_t start = 0;
	const auto close = [&](std::size_t end) {
		if (end > start && speaks_of_pending_approval(answer.substr(start, end - start))) talk.emplace_back(start, end);
		start = end;
	};
	for (std::size_t at = 0; at < answer.size(); ++at) {
		const char character = answer[at];
		const bool ends = character == '\n' || ((character == '.' || character == '!' || character == '?')
			&& (at + 1 == answer.size() || answer[at + 1] == ' ' || answer[at + 1] == '\n'));
		if (!ends) continue;
		while (at + 1 < answer.size() && answer[at + 1] == ' ') ++at;
		close(at + 1);
	}
	close(answer.size());
	return talk;
}

// The model's answer without its sentences about an approval still to come:
// what the lawyer reads under the note that nothing was placed.
std::string without_pending_approval_talk(const std::string &answer) {
	std::string kept;
	std::size_t cursor = 0;
	for (const auto &[start, end] : pending_approval_sentences(answer)) {
		kept.append(answer, cursor, start - cursor);
		cursor = end;
	}
	kept.append(answer, cursor, std::string::npos);
	while (!kept.empty() && std::isspace(static_cast<unsigned char>(kept.back())) != 0) kept.pop_back();
	return kept;
}

// One step of a proposed Word change, as the lawyer would point to it: the
// table entry it changes, the row it adds or removes, or the passage it acts on.
std::string unplaced_step_words(const Json &step, bool russian) {
	const auto text = [&](const Json &value, const char *key) {
		return value.is_object() && value.contains(key) && value.at(key).is_string()
			? value.at(key).get<std::string>() : std::string();
	};
	const auto quoted = [russian](std::string words) {
		constexpr std::size_t kLongest = 80;
		if (words.size() > kLongest) {
			std::size_t cut = kLongest;
			while (cut > 0 && (static_cast<unsigned char>(words[cut]) & 0xC0) == 0x80) --cut;
			words = words.substr(0, cut) + "...";
		}
		return russian ? "\u00ab" + words + "\u00bb" : "\u201c" + words + "\u201d";
	};
	const std::string kind = text(step, "kind");
	const Json locator = step.is_object() && step.contains("locator") ? step.at("locator") : Json();
	const std::string row = text(locator, "rowKey");
	const std::string was = text(step, "expectedOldValue");
	const bool cell = kind == "replaceCellText" || (kind == "setFormatting" && !row.empty());
	if (russian) {
		if (cell)
			return (row.empty() ? std::string("\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB0 \xD1\x82\xD0\xB0\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x86\xD1\x8B") : std::string("\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB0 ") + quoted(row) + std::string(" \xD1\x82\xD0\xB0\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x86\xD1\x8B"))
				+ (was.empty() ? std::string() : std::string(", \xD1\x81\xD0\xB5\xD0\xB9\xD1\x87\xD0\xB0\xD1\x81 ") + quoted(was));
		if (kind == "insertTableRow") return "\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F \xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB0 \xD1\x82\xD0\xB0\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x86\xD1\x8B";
		if (kind == "deleteTableRow") return std::string("\xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 \xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB8 \xD1\x82\xD0\xB0\xD0\xB1\xD0\xBB\xD0\xB8\xD1\x86\xD1\x8B ") + quoted(row);
		if (was.empty()) return "\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82, \xD0\xB4\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD0\xBC\xD1\x8B\xD0\xB9 \xD0\xB2 \xD0\xBA\xD0\xBE\xD0\xBD\xD1\x86\xD0\xB5";
		return std::string("\xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 \xD1\x84\xD1\x80\xD0\xB0\xD0\xB3\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB0 ") + quoted(was);
	}
	if (cell)
		return "the " + (row.empty() ? std::string("table") : quoted(row)) + " entry of the table"
			+ (was.empty() ? std::string() : ", now " + quoted(was));
	if (kind == "insertTableRow") return "the new row of the table";
	if (kind == "deleteTableRow") return "the removal of the table row " + quoted(row);
	if (was.empty()) return "the text added at the end";
	return "the change to " + quoted(was);
}

// The answer a change was proposed with goes before the lawyer with the change:
// they decide with the model's reasons in view, and a change they decline keeps
// them. Its prose is split around the citations this operation recorded,
// exactly as a finished answer is (ChatConversation); a marker it cannot
// resolve never reaches the words as a handle.
void attach_proposal_answer(tools::ToolApprovalRequest &approval, const std::string &answer,
		const std::vector<context::ResolvedSourceReference> &references) {
	if (answer.empty()) return;
	Json detail = approval.change_detail_json.empty() ? Json::object()
		: Json::parse(approval.change_detail_json, nullptr, false);
	if (!detail.is_object()) return;
	// The lawyer reads the arithmetic the model asked for already worked out
	// here (Context/ComputedStatements); the stored answer keeps it as asked.
	const context::ProjectedResponse projected =
		context::project_response_references(context::computed_statements::compute(answer), references);
	Json segments = Json::array();
	for (const context::ProjectedResponseSegment &segment : projected.segments)
		segments.push_back(Json{{"text", segment.citation_number > 0 ? std::string() : segment.text},
			{"citationNumber", segment.citation_number}});
	detail["answer"] = std::move(segments);
	approval.change_detail_json = detail.dump();
}

// What a review asked for, in words a person reads: a source handle the
// reviewer used is named as the source it is.
std::string review_issues_for_people(const documents::ProfessionalReviewResult &review,
		const std::vector<context::ResolvedSourceReference> &references,
		const std::vector<research::ExternalEvidenceReference> &external, const matters::Matter &matter) {
	std::string note;
	for (const documents::ProfessionalReviewIssue &issue : review.issues) {
		if (issue.message.empty()) continue;
		const std::string plain = documents::plain_words_for_lawyer(
			context::source_text_for_people(issue.message, references, external, matter));
		if (plain.empty()) continue;
		if (!note.empty()) note += " ";
		note += plain;
		if (note.size() >= 600) break;
	}
	return note;
}

// A draft Singularity's own review did not clear waits for the lawyer with the
// review's concern beside it - whether Singularity's review passed this exact
// result is theirs to see.
void attach_review_note(tools::ToolApprovalRequest &approval, const documents::ProfessionalReviewResult &review,
		const std::vector<context::ResolvedSourceReference> &references,
		const std::vector<research::ExternalEvidenceReference> &external, const matters::Matter &matter) {
	const std::string note = review_issues_for_people(review, references, external, matter);
	if (note.empty()) return;
	Json detail = approval.change_detail_json.empty() ? Json::object()
		: Json::parse(approval.change_detail_json, nullptr, false);
	if (!detail.is_object()) return;
	detail["reviewNote"] = note;
	approval.change_detail_json = detail.dump();
}

// Refuses an approved change whose target is no longer the document version the
// approval named.
void require_approved_target_unchanged(const Json &draft, const std::optional<std::string> &documentId,
		const std::optional<int> &version, const std::optional<std::string> &fingerprint) {
	if (!draft.contains("approval") || !draft.at("approval").is_object()) return;
	const Json &binding = draft.at("approval");
	const auto text = [&](const char *key) -> std::optional<std::string> {
		if (!binding.contains(key) || !binding.at(key).is_string()) return std::nullopt;
		return binding.at(key).get<std::string>();
	};
	const std::optional<int> boundVersion = binding.contains("targetVersion") && binding.at("targetVersion").is_number_integer()
		? std::optional<int>(binding.at("targetVersion").get<int>()) : std::nullopt;
	if (text("targetDocumentId") != documentId || boundVersion != version || text("targetFingerprint") != fingerprint)
		throw ApprovedTargetChanged();
	const std::optional<std::string> proposalDigest = text("proposalDigest");
	if (proposalDigest && (!draft.contains("rawProviderResult") || !draft.at("rawProviderResult").is_string()
			|| platform::sha256_hex_digest(draft.at("rawProviderResult").get<std::string>()) != *proposalDigest))
		throw std::runtime_error("The approved change no longer matches the proposal that was approved, so nothing was "
			"changed.");
}

// The person's durable decision on this operation's one real change, read from
// the operation's OWN checkpoint - never from ToolBroker's in-process pending
// map, which a quit or a crash destroys while the Project on disk survives
// untouched. record_approval_response() (ChatExecution.cpp) is what writes it.
std::string recorded_operation_decision(const matters::MatterTask &task) {
	if (!task.document_operation_checkpoint) return std::string();
	const std::string decision = task.document_operation_checkpoint->draft.value("decision", std::string());
	return decision == "pending" ? std::string() : decision;
}

// Whether this computer prepared the write-ahead record a checkpoint carries:
// only such a write is finished after an interruption. One written into the
// shared .lm - or left by a write another computer began - is never carried out
// here.
void require_mutation_prepared_here(const std::string &matterId, const matters::MatterTask &task) {
	const Json &record = task.document_operation_checkpoint->mutation_commit;
	if (task.agent_operation_id
			&& tools::prepared_mutation_here(matterId, *task.agent_operation_id, platform::sha256_hex_digest(record.dump())))
		return;
	throw providers::TaskError(providers::TaskErrorCode::TargetChanged,
		"This change was being written by Singularity on another computer, or its record was changed in the "
		"Matter's shared folder, so it was not finished here and nothing was written. Send the request again to "
		"prepare it here.");
}

// Every checkpoint this computer saves is noted as kept here - and a
// write-ahead record in it as a write this computer prepared - before the save
// (Tool/OperationAuthority). Called by every writer of a checkpoint.
void note_checkpoint_saved(const std::string &matterId, const matters::MatterTask &task) {
	if (!task.agent_operation_id || !task.document_operation_checkpoint) return;
	const Json &record = task.document_operation_checkpoint->mutation_commit;
	if (record.is_object() && record.value("state", std::string()) == "prepared")
		tools::note_prepared_mutation(matterId, *task.agent_operation_id, platform::sha256_hex_digest(record.dump()));
	tools::note_checkpoint_kept(matterId, *task.agent_operation_id,
		operation_checkpoint_digest(*task.document_operation_checkpoint));
}

// The person's decision on this operation's one real change, as THIS computer
// recorded it. The checkpoint lives in the Project's .lm, which anyone with a
// shared folder can write: an "approved" there writes nothing unless the person
// at this computer approved exactly the change it holds - its approval, action,
// target, version and content (Tool/OperationAuthority). A rejection only ever
// stops a change, so it stands as recorded.
std::string trusted_operation_decision(const std::string &matterId, const matters::MatterTask &task) {
	const std::string decision = recorded_operation_decision(task);
	if (decision != "approved") return decision;
	if (!task.agent_operation_id) return std::string();
	return tools::approved_here(matterId, *task.agent_operation_id,
			tools::approval_binding_of_checkpoint(task.document_operation_checkpoint->draft))
		? decision : std::string();
}

// This computer generated the step a checkpoint keeps and, when the change is
// put to the lawyer, prepared exactly this approval (Tool/OperationAuthority):
// only such a step resumes under the Project's own mode, and only such an
// approval can be given here. Noted before the checkpoint is saved.
void note_checkpoint_here(const std::string &matterId, const std::string &operationId, const Json &draft) {
	if (draft.contains("rawProviderResult") && draft.at("rawProviderResult").is_string())
		tools::note_generated_here(matterId, operationId,
			platform::sha256_hex_digest(draft.at("rawProviderResult").get<std::string>()));
	if (draft.contains("approval") && draft.at("approval").is_object())
		tools::note_prepared_approval(matterId, operationId, tools::approval_binding_of_checkpoint(draft));
}

// What an attempt of this operation may do without asking: the Project's own
// mode, the same for a person and an AI colleague (D-158), never more than this
// computer recorded for the operation when it began. An operation this computer did not begin, or a
// resumed step whose content it did not generate - the Project's shared .lm can
// be written by anyone with the folder - runs ask-first: every change it would
// make is put to the lawyer here (Tool/OperationAuthority).
tools::ProjectActionPermissionState authorized_permission(const matters::Matter &matter,
		const std::string &operationId) {
	tools::ProjectActionPermissionState permission = tools::project_action_permission(matter.id);
	// Work kept on this computer reaches no network, whatever else it may do.
	if (operation_works_only_here(matter, operationId)) permission.external_network_permitted = false;
	const std::optional<tools::OperationAuthority> authority = tools::operation_authority(matter.id, operationId);
	if (!authority)
		return tools::within_ceiling(permission, tools::ProjectActionMode::AskForApproval,
			"This request did not begin on this computer, so every change it would make waits for your approval here.");
	permission = tools::within_ceiling(permission, authority->ceiling,
		"This request carries on one that asked first, so every change waits for your approval.");
	for (const matters::MatterTask &task : matter.tasks) {
		if (task.agent_operation_id != operationId || !task.document_operation_checkpoint) continue;
		if (!tools::checkpoint_kept_here(matter.id, operationId,
				operation_checkpoint_digest(*task.document_operation_checkpoint)))
			permission = tools::within_ceiling(permission, tools::ProjectActionMode::AskForApproval,
				"This step was kept in the Matter's shared folder, so its change waits for your approval here.");
	}
	return permission;
}

// A resumed operation rebuilds its checkpoint draft from the plan. These are the
// facts that belong to the OPERATION rather than to the plan, so rebuilding must
// never drop them: the decision a person already gave and the approval it
// answered, the generation that decision was given on, what this operation
// already served or could not, and the documents it picked to read.
void carry_forward_durable_operation_facts(const Json &prior, Json &next) {
	for (const char *key : {"approval", "decision", "rawProviderResult",
			"fulfilledResearchRequests", "researchLimitation", "suppliedEvidence", "pickedDocumentIds", "requestedBy"})
		if (prior.contains(key)) next[key] = prior.at(key);
}

// The live version and fingerprint of the document an approval was bound to, as
// the plan resolved the Project this attempt. A bound document that is no longer
// a candidate resolves to nothing, so the approved-target check refuses rather
// than writing an approved change into a document nobody approved.
void require_approved_advanced_workflow_target_unchanged(
		const documents::ConnectedDocumentActionPlan &plan, const Json &draft) {
	if (!draft.contains("approval") || !draft.at("approval").is_object()) return;
	const Json &binding = draft.at("approval");
	if (!binding.contains("targetDocumentId") || !binding.at("targetDocumentId").is_string())
		throw ApprovedTargetChanged();
	const std::string boundId = binding.at("targetDocumentId").get<std::string>();
	std::optional<int> version;
	std::optional<std::string> fingerprint;
	if (boundId == plan.target_document_id) {
		version = plan.target_version;
		fingerprint = plan.target_fingerprint;
	} else {
		for (const documents::DocumentActionCandidate &candidate : plan.other_candidate_documents) {
			if (candidate.document_id != boundId) continue;
			version = candidate.version;
			fingerprint = candidate.fingerprint;
			break;
		}
	}
	require_approved_target_unchanged(draft, boundId, version, fingerprint);
}

// Adds the tool calls a later round made to those already recorded, once each.
void append_tool_call_evidence(std::vector<tools::ToolCallEvidence> &recorded,
		const std::vector<tools::ToolCallEvidence> &added) {
	for (const tools::ToolCallEvidence &evidence : added)
		if (evidence.id.empty() || std::none_of(recorded.begin(), recorded.end(),
				[&](const tools::ToolCallEvidence &known) { return known.id == evidence.id; }))
			recorded.push_back(evidence);
}

// The Professional Review gate (ProfessionalArtifactAction.hpp) is one or
// more additional real generation calls beyond the operation's own primary
// result - accumulated here into the SAME task.usage the lawyer/billing view
// already reads, rather than silently under-reporting real token/cost spend
// for a review or correction cycle. Additive fields sum when both sides have
// a value; a field only one side populated is kept rather than discarded.
providers::UsageMetadata merge_usage_metadata(
		const providers::UsageMetadata &accumulated, const providers::UsageMetadata &addition) {
	return providers::accumulate_usage(accumulated, addition);
}

// AdvancedWorkflow's real choreography - ConnectedDocumentAction's own
// existing, unchanged multi-turn tool machinery (methodology reveal,
// evidence agenda, table inspection/compaction, output planning, drafting,
// professional review), reused verbatim. Called ONLY from
// execute_task_attempt(), and only once the model's OWN single unified call
// has already declared actionType "advanced_workflow" against a real
// structurally-resolved candidate - never pre-wired into the first request
// the way it used to be, which is what made ConnectedDocumentAction a
// second, keyword/pre-generation-selected Chat engine. This function is the
// "port" of that engine's capability, not a rewrite of it: every handler
// below still calls straight into ConnectedDocumentAction.cpp's own,
// completely unmodified functions.
// Singularity validates the model's typed research request itself: only the
// three fields of web.search's own schema, each of the schema's own type -
// the strict wire schema is not always provider-enforced, and a policy-only
// key (allowed/blocked domains, source preferences) must never be able to
// carry a model-authored value to the provider. A request that fails this
// shape is no runnable request. This normalized form is the ONE identity of
// a request everywhere: what the loop escalates and records as fulfilled,
// and what an accepted result's own request is compared against - never the
// raw model object, whose dropped or mistyped extra keys say nothing about
// which search actually ran.
std::optional<nlohmann::json> normalize_research_request(const nlohmann::json &raw) {
	if (!raw.is_object() || !raw.contains("query") || !raw.at("query").is_string()
			|| raw.at("query").get_ref<const std::string &>().empty())
		return std::nullopt;
	nlohmann::json request{{"query", raw.at("query")}};
	if (raw.contains("jurisdiction") && raw.at("jurisdiction").is_string()) request["jurisdiction"] = raw.at("jurisdiction");
	if (raw.contains("freshnessRequired") && raw.at("freshnessRequired").is_boolean())
		request["freshnessRequired"] = raw.at("freshnessRequired");
	return request;
}

std::optional<nlohmann::json> normalize_research_request_json(const std::string &requestJson) {
	return normalize_research_request(nlohmann::json::parse(requestJson, nullptr, false));
}

std::optional<nlohmann::json> professional_research_request_from_final_text(const std::string &text) {
	const auto proposal = documents::parse_professional_response_proposal(text);
	if (!proposal || !proposal->external_research_request_json) return std::nullopt;
	return normalize_research_request_json(*proposal->external_research_request_json);
}

void wire_connected_document_action_request(routing::InferenceRequest &request,
		const documents::ConnectedDocumentActionPlan &plan, const nlohmann::json &operationRequestData,
		const std::string &operationId, std::optional<std::uint64_t> availableContextWindowTokens) {
	request.accepts_research_final_text = [](const std::string &text) {
		if (text.empty()) return false;
		const auto envelope = nlohmann::json::parse(text, nullptr, false);
		return envelope.is_discarded() || !envelope.is_object()
			|| (envelope.value("type", std::string()) != "tool_call"
				&& envelope.value("type", std::string()) != "tool_calls"
				&& envelope.value("type", std::string()) != "final");
	};
	request.research_final_text_issue = [plan](
			const std::string &,
			const std::vector<research::ExternalEvidenceReference> &evidence) {
		return documents::connected_document_draft_completion_issue(plan, evidence);
	};
	request.research_final_text_instruction =
		"Return one short plain-language completion sentence after every backend draft item is accepted.";
	request.operation_model_tool = documents::connected_document_draft_model_tool_definition();
	request.operation_canonical_tool_id = "document.draft.upsert";
	request.operation_research_request_model_tool =
		documents::connected_document_research_request_model_tool_definition();
	request.operation_research_request_canonical_tool_id = "document.draft.request_research";
	request.operation_tool_handler = [plan](const nlohmann::json &arguments,
			const std::vector<research::ExternalEvidenceReference> &evidence) {
		const auto result = documents::upsert_connected_document_draft_item(plan, arguments, evidence);
		return std::pair<bool, std::string>{result.accepted, result.message};
	};
	request.operation_research_request_handler = [plan](
			const nlohmann::json &arguments,
			const std::vector<research::ExternalEvidenceReference> &evidence,
			const research::OperationExecutionProgress &progress) {
		const auto result = documents::request_connected_document_draft_research(
			plan, arguments, evidence, progress);
		return std::pair<bool, std::string>{result.accepted, result.message};
	};
	request.operation_research_completed_handler = [plan](
			const std::vector<research::ExternalEvidenceReference> &evidence,
			const research::OperationExecutionProgress &progress) {
		documents::complete_connected_document_draft_research(plan, evidence, progress);
	};
	if (plan.compact_context_mode || plan.requires_evidence_agenda) {
		request.operation_inspect_model_tool =
			documents::connected_document_inspect_model_tool_definition();
		request.operation_inspect_canonical_tool_id = "document.inspect";
		request.operation_inspect_handler = [plan](
				const nlohmann::json &arguments,
				const std::vector<research::ExternalEvidenceReference> &,
				const research::OperationExecutionProgress &) {
			const auto result = documents::request_connected_document_table_inspection(plan, arguments);
			return std::pair<bool, std::string>{result.accepted, result.message};
		};
		request.operation_map_next_model_tool =
			documents::connected_document_map_next_model_tool_definition();
		request.operation_map_next_canonical_tool_id = "document.map.next";
		request.operation_map_next_handler = [plan](
				const nlohmann::json &arguments,
				const std::vector<research::ExternalEvidenceReference> &,
				const research::OperationExecutionProgress &) {
			const auto result = documents::request_connected_document_table_map_next_page(plan, arguments);
			return std::pair<bool, std::string>{result.accepted, result.message};
		};
	}
	if (plan.compact_context_mode) {
		request.operation_context_compaction_handler = [plan]() {
			return documents::compact_connected_document_inspection_window(plan);
		};
	}
	if (plan.requires_evidence_agenda) {
		request.operation_evidence_agenda_model_tool =
			documents::connected_document_evidence_agenda_model_tool_definition();
		request.operation_evidence_agenda_canonical_tool_id = "document.evidence.define";
		request.operation_evidence_agenda_handler = [plan](
				const nlohmann::json &arguments,
				const std::vector<research::ExternalEvidenceReference> &,
				const research::OperationExecutionProgress &) {
			const auto result = documents::define_connected_document_evidence_agenda(plan, arguments);
			return std::pair<bool, std::string>{result.accepted, result.message};
		};
	}
	request.operation_output_agenda_model_tool =
		documents::connected_document_output_agenda_model_tool_definition();
	request.operation_output_agenda_canonical_tool_id = "document.outputs.define";
	request.operation_output_agenda_handler = [plan](
			const nlohmann::json &arguments,
			const std::vector<research::ExternalEvidenceReference> &,
			const research::OperationExecutionProgress &) {
		const auto result = documents::define_connected_document_output_agenda(plan, arguments);
		return std::pair<bool, std::string>{result.accepted, result.message};
	};
	if (!plan.methodology_handles.empty()) {
		request.operation_methodology_reveal_model_tool =
			documents::connected_document_methodology_reveal_model_tool_definition();
		request.operation_methodology_reveal_canonical_tool_id = "document.methodology.reveal";
		request.operation_methodology_reveal_handler = [plan](
				const nlohmann::json &arguments,
				const std::vector<research::ExternalEvidenceReference> &,
				const research::OperationExecutionProgress &) {
			const auto result = documents::reveal_connected_document_methodology(plan, arguments);
			return std::pair<bool, std::string>{result.accepted, result.message};
		};
	}
	request.operation_professional_review_model_tool =
		documents::connected_document_professional_review_model_tool_definition();
	request.operation_professional_review_canonical_tool_id = "document.review.submit";
	request.operation_professional_review_handler = [plan](
			const nlohmann::json &arguments,
			const std::vector<research::ExternalEvidenceReference> &,
			const research::OperationExecutionProgress &) {
		const auto result = documents::submit_connected_document_professional_review(plan, arguments);
		return std::pair<bool, std::string>{result.accepted, result.message};
	};
	request.operation_state_compiler = [plan, operationRequestData, operationId](
			const std::vector<research::ExternalEvidenceReference> &evidence,
			const std::optional<research::FinalTextIssue> &issue) {
		auto projection = documents::compile_connected_document_operation_state(plan, evidence, issue);
		projection.operation_id = operationId;
		if (projection.frontier == research::OperationWorkFrontier::PlanningOutputs) {
			nlohmann::json condensed = nlohmann::json::object();
			if (operationRequestData.contains("currentRequest"))
				condensed["currentRequest"] = operationRequestData.at("currentRequest");
			projection.data_plane.value["request"] = std::move(condensed);
		} else {
			projection.data_plane.value["request"] = operationRequestData;
		}
		return projection;
	};
	request.operation_completion_text = "The requested Project document was updated after local validation.";
	request.maximum_output_tokens = availableContextWindowTokens
		? std::min<std::uint64_t>(2048, std::max<std::uint64_t>(512, *availableContextWindowTokens / 3))
		: std::uint64_t{2048};
}

Json privacy_metadata_json(const protected_api::papinian::PrivacyMetadata &metadata) {
  return Json{{"mode", metadata.mode},
              {"papinianGuardVersion", metadata.papinian_guard_version},
              {"papinianNerVersion", metadata.papinian_ner_version},
              {"protectedCount", metadata.protected_count},
              {"categoryCounts", metadata.category_counts},
			  {"responseIntegrity", metadata.response_integrity},
			  {"finalPayloadValidated", metadata.final_payload_validated},
			  {"finalPayloadSha256", metadata.final_payload_sha256},
			  {"protectedProjectEntityIds", metadata.protected_project_entity_ids},
			  {"responseIssueCode", metadata.response_issue_code},
			  {"responseMarkerCandidate", metadata.response_marker_candidate},
			  {"responseCanonicalMarker", metadata.response_canonical_marker},
			  {"responseMarkerType", metadata.response_marker_type},
			  {"responseJsonPointer", metadata.response_json_pointer},
			  {"responseNormalizationOutcome", metadata.response_normalization_outcome},
			  {"responseRepairClass", metadata.response_repair_class},
			  {"responseRepairAttempts", metadata.response_repair_attempts}};

}

void apply_safe_response_diagnostics(protected_api::papinian::PrivacyMetadata &metadata,
		const std::optional<providers::SafeResponseDiagnostics> &diagnostics) {
	if (!diagnostics) return;
	metadata.response_issue_code = diagnostics->issue_code;
	metadata.response_marker_candidate = diagnostics->marker_candidate;
	metadata.response_canonical_marker = diagnostics->canonical_marker;
	metadata.response_marker_type = diagnostics->marker_type;
	metadata.response_json_pointer = diagnostics->json_pointer;
	metadata.response_normalization_outcome = diagnostics->normalization_outcome;
	metadata.response_repair_class = diagnostics->repair_class;
	metadata.response_repair_attempts = diagnostics->repair_attempts;
}

auto find_matter(std::vector<matters::Matter> &list, const std::string &id) {
  return std::find_if(list.begin(), list.end(),
                      [&](const matters::Matter &matter) { return matter.id == id; });
}

void upsert_task_record(matters::Matter &matter, matters::MatterTask task) {
	const auto existing = std::find_if(matter.tasks.begin(), matter.tasks.end(),
		[&](const matters::MatterTask &entry) { return entry.id == task.id; });
	if (existing == matter.tasks.end()) matter.tasks.push_back(std::move(task));
	else *existing = std::move(task);
}

void append_operation_event(matters::MatterTask &task, const std::string &type,
		const std::string &message, const std::optional<std::string> &subject = std::nullopt,
		const std::optional<std::string> &outcome = std::nullopt) {
	matters::AgentOperationEvent event;
	event.id = matters::generate_event_id();
	event.sequence = task.operation_events.empty() ? 1 : task.operation_events.back().sequence + 1;
	event.type = type;
	event.created_at = workspaces::current_timestamp();
	event.operation_revision = task.operation_revision;
	event.message = message;
	event.subject_id = subject;
	event.outcome = outcome;
	task.operation_events.push_back(std::move(event));
}

// The lawyer's own words in this request: what they asked, and every answer
// they typed to a question the operation put to them. What the approval floor
// reads - never the model's account of them.
std::vector<std::string> lawyer_words(const matters::MatterTask &task) {
	std::vector<std::string> words{task.prompt};
	for (const matters::LawyerQuestionRecord &question : task.lawyer_questions) {
		if (!question.answers.is_object()) continue;
		for (auto answer = question.answers.begin(); answer != question.answers.end(); ++answer)
			if (answer.value().is_object() && answer.value().contains("text") && answer.value().at("text").is_string())
				words.push_back(answer.value().at("text").get<std::string>());
	}
	return words;
}

bool lawyer_asks_to_see_changes_first(const std::vector<std::string> &words) {
	return std::any_of(words.begin(), words.end(), [](const std::string &said) {
		return tools::asks_to_see_changes_first(said);
	});
}

/* - - - - - - */

// A confirmation, read word by word: the text's words as a person reads them -
// case, width and accents folded by utf8proc in every script - each as its
// code points. An apostrophe joins ("let's" is "lets").
std::vector<std::u32string> folded_words(const std::string &text) {
	const std::string value = platform::folded_text(text, true);
	std::vector<std::u32string> words;
	std::u32string word;
	const auto *bytes = reinterpret_cast<const utf8proc_uint8_t *>(value.data());
	const utf8proc_ssize_t size = static_cast<utf8proc_ssize_t>(value.size());
	utf8proc_ssize_t at = 0;
	while (at < size) {
		utf8proc_int32_t point = 0;
		const utf8proc_ssize_t length = utf8proc_iterate(bytes + at, size - at, &point);
		at += length > 0 ? length : 1;
		if (length <= 0) continue;
		const utf8proc_category_t category = utf8proc_category(point);
		const bool isWordCharacter = (category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_ME)
			|| (category >= UTF8PROC_CATEGORY_ND && category <= UTF8PROC_CATEGORY_NO);
		if (isWordCharacter) {
			word.push_back(static_cast<char32_t>(point));
			continue;
		}
		if (point == '\'' || point == 0x2019 || point == 0x2018 || point == 0x02BC) continue;
		if (!word.empty()) words.push_back(word);
		word.clear();
	}
	if (!word.empty()) words.push_back(word);
	return words;
}

std::set<std::u32string> folded_word_set(std::initializer_list<const char *> written) {
	std::set<std::u32string> words;
	for (const char *text : written)
		for (std::u32string &word : folded_words(text)) words.insert(std::move(word));
	return words;
}

// The words that say "do what you showed me", in the languages Singularity's
// lawyers write. A bounded list: a confirmation in words it does not hold is
// drafted anew, exactly as before, and costs time - never a change.
const std::set<std::u32string> &confirming_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"go", "proceed", "create", "apply", "do", "make", "write", "add", "save", "approve", "approved", "confirm",
		"confirmed", "accept", "accepted", "yes", "yeah", "yep", "ok", "okay", "sure", "fine", "agreed", "agree",
		"finalize", "finalise", "execute", "generate", "produce", "lgtm",
		"давай", "давайте", "создай", "создайте", "создавай", "создавайте", "создать", "примени", "примените",
		"применяй", "применяйте", "применить", "сделай", "сделайте", "делай", "делайте", "внеси", "внесите", "вноси",
		"вносите", "запиши", "запишите", "сохрани", "сохраните", "добавь", "добавьте", "да", "ок", "окей", "хорошо",
		"согласен", "согласна", "согласны", "одобряю", "подтверждаю", "действуй", "действуйте", "вперёд", "годится",
		"подходит", "оформи", "оформите"});
	return words;
}

// Words that ask for nothing by themselves: "please", "the", "it", "now".
const std::set<std::u32string> &request_free_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"ahead", "please", "pls", "now", "then", "and", "so", "just", "the", "a", "an", "it", "this", "that", "these",
		"those", "them", "one", "thanks", "thank", "you", "can", "could", "would", "will", "let", "lets", "us", "me",
		"right", "away", "as", "is", "proposed", "shown", "drafted", "described", "above", "same", "your", "draft",
		"document", "doc", "file", "change", "changes", "edit", "edits", "version", "word", "docx", "new", "separate",
		"into", "to", "in", "for", "of", "we", "i", "discussed", "planned", "suggested", "looks", "good", "great",
		"и", "пожалуйста", "тогда", "теперь", "так", "ну", "уже", "сейчас", "это", "этот", "эту", "эти", "его", "её",
		"их", "всё", "документ", "документа", "файл", "файла", "изменение", "изменения", "изменений", "правку",
		"правки", "проект", "черновик", "вариант", "версию", "как", "предложено", "предложил", "предложили",
		"показано", "показал", "показали", "вы", "ты", "мне", "нам", "в", "во", "же", "отлично", "можно", "к", "ко",
		"отдельный", "отдельном", "отдельным", "отдельную", "отдельной", "отдельное", "отдельно", "новый", "новом",
		"новым", "новую", "новой", "новое", "ворд", "ворде", "файле", "документе"});
	return words;
}

// Words that send a change to a document of its own: "in a separate file",
// "в отдельном документе" - and, with a document word, "a new document".
const std::set<std::u32string> &separate_document_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"separate", "separately", "standalone", "copy", "отдельный", "отдельном", "отдельным", "отдельную",
		"отдельной", "отдельное", "отдельно", "копию", "копия", "копии"});
	return words;
}
const std::set<std::u32string> &new_thing_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"new", "another", "fresh", "новый", "новом", "новым", "новую", "новой", "новое", "другой", "другом",
		"другим", "другую"});
	return words;
}
const std::set<std::u32string> &document_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"document", "doc", "docx", "file", "word", "документ", "документе", "документом", "документа", "файл",
		"файле", "файлом", "файла", "ворд", "ворде"});
	return words;
}
// A place inside something that already exists: "write it into the MSA",
// "внеси в договор".
const std::set<std::u32string> &inside_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"into", "in", "inside", "within", "onto", "в", "во"});
	return words;
}
// What relates one thing to another: "the amendment to the MSA", "поправка к
// договору" - or, with nothing named before it, where a change goes ("Add it to
// the MSA.").
const std::set<std::u32string> &relating_words() {
	static const std::set<std::u32string> words = folded_word_set({"to", "к", "ко"});
	return words;
}
// Words a file name carries that name no document.
const std::set<std::u32string> &file_name_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"docx", "doc", "pdf", "xlsx", "xls", "txt", "md", "rtf", "odt", "csv", "pptx"});
	return words;
}

// Words that turn a confirmation into a new request: "Create it, but for $20
// million.", "Только без пункта 3.".
const std::set<std::u32string> &new_request_words() {
	static const std::set<std::u32string> words = folded_word_set({
		"but", "except", "however", "instead", "only", "without", "not", "dont", "no", "rather", "also", "unless",
		"replace", "remove", "delete", "modify", "revise", "fix", "correct",
		"но", "однако", "кроме", "вместо", "только", "без", "не", "нет", "также", "еще", "замени", "убери",
		"удали", "измени", "исправь", "поправь"});
	return words;
}

bool has_digit(const std::u32string &word) {
	return std::any_of(word.begin(), word.end(), [](char32_t point) {
		const utf8proc_category_t category = utf8proc_category(static_cast<utf8proc_int32_t>(point));
		return category >= UTF8PROC_CATEGORY_ND && category <= UTF8PROC_CATEGORY_NO;
	});
}

// The same word, or the same word in another form - "amendment" and
// "amendments", "поправка" and "поправку": a long shared beginning. A figure is
// the same figure only when it is written the same.
bool is_proposal_word(const std::u32string &word, const std::vector<std::u32string> &proposal) {
	for (const std::u32string &known : proposal) {
		if (known == word) return true;
		if (has_digit(word) || has_digit(known)) continue;
		const std::size_t shorter = std::min(word.size(), known.size());
		if (shorter < 5) continue;
		std::size_t shared = 0;
		while (shared < shorter && word[shared] == known[shared]) ++shared;
		if (shared >= 5 && shared + 3 >= shorter) return true;
	}
	return false;
}

// For as long as one attempt of an operation runs: when the lawyer asked in
// this request to see its changes first, every change it makes to the Project
// - through whichever tool - waits for them, whatever the Project's action
// mode. Released when the attempt ends, however it ends.
class ChangesHeldForLawyer {
public:
	ChangesHeldForLawyer(const std::string &projectId, const std::string &operationId)
		: projectId_(projectId)
		, operationId_(operationId) {
	}
	~ChangesHeldForLawyer() {
		tools::production_tool_broker().release_changes(projectId_, operationId_);
	}
	ChangesHeldForLawyer(const ChangesHeldForLawyer &) = delete;
	ChangesHeldForLawyer &operator=(const ChangesHeldForLawyer &) = delete;

	void hold() {
		tools::production_tool_broker().hold_changes(projectId_, operationId_);
	}

private:
	std::string projectId_;
	std::string operationId_;
};

// A task's own execution thread is not the only writer of operation_revision:
// ChatExecution.cpp's operation_status() also independently persists a
// "waiting_for_approval" transition (via persist_operation_transition(), a
// SEPARATE load/modify/save cycle) the first time its own live polling
// notices a pending approval - real, live-reproduced evidence this session
// (item13's own real approval flow) proved that observer can win this
// revision race and persist a higher operation_revision than the execution
// thread's own final result, even though the execution thread already
// finished successfully by the time this merge runs. A revision number
// alone therefore never reliably orders these two independent writers.
[[nodiscard]] bool is_terminal_operation_execution_state(const std::string &state) {
	return state == "completed" || state == "failed" || state == "stopped"
		|| state == "conflict" || state == "interrupted";
}

void preserve_newer_lifecycle(const matters::MatterTask &current, matters::MatterTask &replacement) {
	std::vector<matters::AgentOperationEvent> merged = current.operation_events;
	for (const matters::AgentOperationEvent &event : replacement.operation_events) {
		const auto known = std::find_if(merged.begin(), merged.end(),
			[&](const matters::AgentOperationEvent &entry) { return entry.id == event.id; });
		if (known == merged.end()) merged.push_back(event);
	}
	std::stable_sort(merged.begin(), merged.end(), [](const auto &left, const auto &right) {
		return left.created_at < right.created_at;
	});
	for (std::size_t index = 0; index < merged.size(); ++index) merged[index].sequence = index + 1;
	replacement.operation_events = std::move(merged);
	// Work kept on this computer stays so: no save lifts the mark.
	replacement.local_only = replacement.local_only || current.local_only;
	// An operation's signed origin is written once, when it began: no later save
	// replaces or drops it.
	if (current.origin) replacement.origin = current.origin;
	// A person's approval decision is recorded once, on this operation's own
	// checkpoint, and is never erased by a writer that does not carry it - this
	// execution thread saving its own copy, or another device saving a snapshot
	// taken before the decision arrived. Two different decisions never merge:
	// the recorded one stands.
	const auto recorded_decision = [](const matters::MatterTask &entry) {
		if (!entry.document_operation_checkpoint) return std::string();
		const nlohmann::json &draft = entry.document_operation_checkpoint->draft;
		if (!draft.contains("approval") || !draft.at("approval").is_object()) return std::string();
		const std::string decision = draft.at("approval").value("decision", std::string());
		return decision == "pending" ? std::string() : decision;
	};
	const std::string decided = recorded_decision(current);
	// The decision answers one approval. A writer carrying a different one has
	// prepared the change again since - its own, newer approval stands, and the
	// old decision never lands on it (an "approved" written into the shared .lm
	// once took over a change prepared afresh here, and its card could no longer
	// be answered).
	const auto approval_id_of = [](const matters::MatterTask &entry) {
		if (!entry.document_operation_checkpoint) return std::string();
		const nlohmann::json &draft = entry.document_operation_checkpoint->draft;
		if (!draft.contains("approval") || !draft.at("approval").is_object()) return std::string();
		return draft.at("approval").value("approvalId", std::string());
	};
	const std::string replacementApproval = approval_id_of(replacement);
	const bool sameApproval = replacementApproval.empty() || replacementApproval == approval_id_of(current);
	if (!decided.empty() && sameApproval && recorded_decision(replacement) != decided) {
		if (!replacement.document_operation_checkpoint) {
			replacement.document_operation_checkpoint = current.document_operation_checkpoint;
		} else if (current.document_operation_checkpoint
				&& current.document_operation_checkpoint->draft.contains("approval")) {
			replacement.document_operation_checkpoint->draft["approval"] =
				current.document_operation_checkpoint->draft.at("approval");
			replacement.document_operation_checkpoint->draft["decision"] = decided;
		}
		// The prompt a decision answered is gone for good: a writer that still
		// carries it must not put the question back in front of the person.
		if (!current.operation_approval) replacement.operation_approval.reset();
		// A rejection is an outcome, not only a decision: the change was not
		// written, and the turn reads as declined. A writer that saved its own
		// copy before the rejection arrived must not turn that into an answer.
		if (decided == "rejected" && current.status == matters::TaskStatus::Error) {
			replacement.status = current.status;
			replacement.error = current.error;
			replacement.error_code = current.error_code;
			replacement.rejected_approval = current.rejected_approval;
		}
	}
	// Real product bug (caught live, this session's own item13 regression
	// surfaced by the ChatExecution.cpp false-completion fix): once this
	// execution's own in-memory result already reached a terminal outcome,
	// that is authoritative regardless of which writer's revision counter is
	// numerically higher - an observer's stale mid-flight "waiting_for_
	// approval" snapshot must never regress a real completion back to
	// pending. This is the mirror of the false-success bug this whole task
	// fixed (pending wrongly reported as done); this is done wrongly
	// reported as still pending - both are the same "completion must mean
	// something durable" invariant, just approached from either side.
	if (is_terminal_operation_execution_state(replacement.operation_execution_state)) return;
	if (current.operation_revision > replacement.operation_revision) {
		replacement.operation_revision = current.operation_revision;
		replacement.operation_execution_state = current.operation_execution_state;
		replacement.operation_stage = current.operation_stage;
		replacement.operation_updated_at = current.operation_updated_at;
		replacement.operation_activity = current.operation_activity;
		replacement.operation_cancel_requested = current.operation_cancel_requested;
		replacement.operation_resume_allowed = current.operation_resume_allowed;
		replacement.operation_approval = current.operation_approval;
	}
}

void persist_task_checkpoint(const std::filesystem::path &workspacePath,
		const std::string &matterId, const matters::MatterTask &task) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	auto project = matters::load_matter(workspacePath, matterId);
	if (!project)
		throw std::runtime_error("The Project no longer exists while its operation is running.");
	matters::MatterTask checkpoint = task;
	const auto current = std::find_if(project->tasks.begin(), project->tasks.end(),
		[&](const matters::MatterTask &entry) { return entry.id == task.id; });
	if (current != project->tasks.end()) preserve_newer_lifecycle(*current, checkpoint);
	note_checkpoint_saved(matterId, checkpoint);
	upsert_task_record(*project, std::move(checkpoint));
	project->updated_at = workspaces::current_timestamp();
	matters::save_matter(workspacePath, *project);
}

// This thread's copy of the operation counts only its own saves. The window's
// own saves - an approval shown and then granted earlier in this run, a
// question shown - move the saved revision past it, and a save behind them
// loses to them: an answered question would read "waiting for your answer"
// until the run ends. Called under the persistence lock, so nothing is saved
// between this and the save that follows. A Stop saved since stays ahead and
// stands.
void catch_up_to_saved_revision(const std::filesystem::path &workspacePath,
		const std::string &matterId, matters::MatterTask &task) {
	const auto project = matters::load_matter(workspacePath, matterId);
	if (!project) return;
	const auto saved = std::find_if(project->tasks.begin(), project->tasks.end(),
		[&](const matters::MatterTask &entry) { return entry.id == task.id; });
	if (saved == project->tasks.end() || saved->operation_cancel_requested) return;
	if (saved->operation_revision > task.operation_revision) task.operation_revision = saved->operation_revision;
}

Json occurrence_summary_json(const protected_api::papinian::ProtectedOccurrenceSummary &occurrence) {
  return Json{
      {"occurrenceId", occurrence.occurrence_id},
      {"category", occurrence.category},
      {"displayPlaceholder", occurrence.display_placeholder},
      {"originKind", occurrence.origin_kind},
      {"originObjectId", occurrence.origin_object_id ? Json(*occurrence.origin_object_id) : Json(nullptr)},
  };
}

struct TaskAttemptSpec {
  std::string prompt;
  std::optional<std::string> chat_id;
  std::optional<std::string> matter_instructions;
  // Same defaults as context::PromptAssemblyInput, since an omitted preset/
  // profile in matter.runTask's params falls through to them unchanged.
  std::string legal_task_preset_id = "general";
  int legal_task_preset_version = 1;
  std::string prompt_profile_id = std::string(context::kDefaultSystemPromptProfileId);
  int prompt_profile_version = context::kDefaultSystemPromptProfileVersion;
  std::vector<std::string> selected_document_ids;
	std::optional<std::string> target_document_id;
  std::optional<routing::ModelTarget> model_target;
	professional::ProfessionalExecutionIntent professional_intent;
	std::string operation_id;
  // 1 for a fresh task; > 1 and a matching root_task_id for a retried one -
  // see matters::next_attempt_number().
  int attempt = 1;
  std::optional<std::string> root_task_id;
  // nullopt for a fresh task (use the current live Protected API policy's
  // mode, exactly as before this field existed). Set by retry_matter_task()
  // to the original task's own protected_api_mode, so a policy change made
  // after the original attempt cannot silently change what Retry/Run with
  // another model actually do - Master Spec Sec.66's "reuse ... protection
  // mode".
  std::optional<protected_api::Mode> protection_mode_override;
	// Set only when resuming a checkpointed operation whose request could not
	// be assembled within the selected model's known capacity: the request is
	// assembled as if capacity were unknown, because the resume may need no
	// model call at all (an approved proposal commits from its checkpoint).
	// Any turn it does dispatch is still refused by routing::execute()'s own
	// first-turn admission.
	bool assemble_without_known_capacity = false;
	// Where the effective model for this request actually came from, and which
	// role produced its primary result - the AI Setup resolver's own answer,
	// recorded on the task instead of being assumed (SET-1). Unset means the
	// caller chose this exact model for this one request.
	std::optional<std::string> ai_setup_source;
	std::optional<std::string> operation_role;
	// Set only for a rerun (Retry, Run with another model, Edit and continue):
	// the Project object the original turn actually realized. A rerun replaces
	// its own work - see the retryOfTaskId branch in task_attempt_spec_from_
	// params() and resolve_rerun_bound_record_plan() (SLR-3).
	std::optional<std::string> realized_record_document_id;
	std::optional<std::string> realized_record_table_id;
	std::optional<std::size_t> realized_record_banner_row_index;
	std::optional<std::string> realized_artifact_document_id;
	// The request comes from someone who can answer a question while it runs
	// (the app's Chat composer): only then may the model stop to ask the lawyer.
	// A console or a test sends none, and nothing would ever answer.
	bool can_ask_lawyer = false;
	// Nobody watches the request as it runs (an AI colleague's own work): a wait
	// on the lawyer keeps its place and returns at once (Beta 1 #14).
	bool unattended = false;
	// The agent whose thread the request is in, and whether the agent began
	// it itself (MatterTask::agent_id, agent_initiated).
	std::string agent_id;
	bool agent_initiated = false;
	// The agent's name, for a turn it began itself (MatterTask::author_name).
	std::string agent_name;
	// Who asked when it is not the person at this computer: a colleague who
	// named an agent in a thread; and the message of theirs it answers.
	std::string author_actor_id;
	std::string author_name;
	std::string answers_message_id;
	// Set only when resuming an operation whose first attempt already picked its
	// documents (Context/SourcePicker): the pick recorded on its checkpoint. The
	// resumed attempt reads those documents again instead of asking the model to
	// choose them a second time - approving a change never pays for another pick.
	std::optional<std::vector<std::string>> recorded_pick;
};

// Runs one real task attempt against a matter: assembles context from the
// matter's own readable documents (see retrieve_document_context()), sends
// it through the exact same routing::execute() pipeline models.execute
// uses - workspace/model resolution, Protected API transform/restore, the
// provider call itself - and appends a safe-field-only Routing Log entry.
// Never throws for an inference-side failure (bad key, unreachable
// provider, unknown model): that becomes an error MatterTask instead, so a
// failed attempt is still real history, not a lost request. Does not
// persist the returned task or the matter - callers append it and call
// matters::save_matters() themselves, since matter.runTask and
// matter.retryTask differ in what else (document refresh, a not-found
// lookup) happens inside the same locked transaction first.
// Everything a task attempt must resolve BEFORE it is either sent for real
// or previewed (Phase 08 WP8-B, "What leaves this device?") - Matter
// context retrieval, prompt assembly/budget, and the effective Protected
// API policy for this one attempt. Deliberately the single place this
// happens: execute_task_attempt() (real send) and preview_outbound_
// request() (preview) both call this and nothing else builds a
// routing::InferenceRequest from a TaskAttemptSpec, so the two paths cannot
// silently diverge in what they ask Papinian Guard/the provider layer to
// process.
struct PreparedTaskRequest {
  routing::InferenceRequest request;
  protected_api::Policy effective_policy;
	// The documents picked for a request that named none (Context/SourcePicker)
	// and what the pick cost - counted to the task like any call of its own.
	std::vector<std::string> picked_document_ids;
	providers::UsageMetadata pick_usage;
  context::SourceMarkerRegistry supplied_markers;
  std::unordered_set<std::string> distinct_context_document_ids;
  std::size_t chunk_count = 0;
  std::vector<context::ContextItemSnapshot> context_snapshot;
	// AdvancedWorkflow's own structural candidate - see prepare_task_request()'s
	// own comment. Only ever activated post-generation, inside
	// execute_task_attempt(), when the model itself declares actionType
	// "advanced_workflow" against this exact candidate.
	std::optional<documents::ConnectedDocumentActionPlan> connected_document_action;
	// The same model-visible request payload wire_connected_document_action_
	// request() needs to build the operation_state_compiler closure - carried
	// through so it never has to be recomputed (and risk diverging) at the
	// later point where AdvancedWorkflow is actually activated.
	nlohmann::json operation_request_data;
	std::optional<documents::DirectDocumentWorkTaskPlan> direct_document_work;
	// Structural candidates only (resolved before generation) - the actual
	// ProfessionalArtifactActionPlan is built AFTER generation, from what the
	// model itself declared, by build_professional_artifact_plan() in
	// execute_task_attempt() below. See ProfessionalArtifactAction.hpp's own
	// header comment.
	documents::ProfessionalArtifactCandidates professional_artifact_candidates;
	professional::ProfessionalExecutionIntent professional_intent;
	// Product-state answers come from already-resolved backend state. They do
	// not enter provider/local inference and therefore cannot consume tokens or
	// accidentally inherit document work.
	std::optional<std::string> local_response;
	// True only when PromptAssembler's real request budget actually excluded a
	// whole lower-priority item - the honest source for MatterTask::context_
	// budget_excluded_material, never a frontend byte estimate.
	bool context_budget_excluded_material = false;
	// What the request carried of Project Context and earlier turns (its
	// REQUEST_COVERAGE statement), recorded as MatterTask::request_coverage.
	context::PromptCoverage request_coverage;
	// Phase 27's source-problem Error - the honest source for MatterTask::
	// unavailable_source_documents, populated only for Relevant-mode
	// documents this exact attempt genuinely could not read (matters::
	// retrieve_document_context()'s own out-parameter).
	std::vector<matters::UnavailableSourceDocument> unavailable_source_documents;
	// What a revising continuation needs to deliver evidence the same way the
	// first call did: the assembly input (budget included), every retrieved
	// passage in delivery order (the marker numbering's own basis) and the
	// passages the first call actually carried.
	context::PromptAssemblyInput prompt_input;
	std::vector<context::RetrievedContextChunk> document_context;
	std::unordered_set<std::string> supplied_chunk_ids;
	// Set when the AdvancedWorkflow operation's request data could not be held
	// by its share of the executing model's known capacity: the request is
	// refused with this message only if the model declares advanced_workflow.
	std::optional<std::string> operation_request_capacity_refusal;
	// Set when the request asked for (or allowed) web research that cannot run
	// on this Node: why, as stated to the model and in the result.
	std::optional<std::string> research_unavailable_reason;
	// The operation's read-only Project inspection (Context/ProjectEvidence),
	// when this request offers it: what the model asked to read and was given,
	// numbered after the request's own passages. Null when not offered.
	std::shared_ptr<context::project_evidence::Session> project_evidence;
	// The capability text without the inspection protocol - what a later round
	// of the same operation (a review's redraft) is assembled from, since only
	// the first call can ask to read more.
	std::optional<std::string> capability_without_inspection;
	// The passages this operation's inspection gave the model, in the order they
	// were numbered - read in this attempt, or restored from its checkpoint.
	// Part of the operation's provenance: what its proposal was prepared from.
	std::vector<context::RetrievedContextChunk> project_evidence_passages;
	// The order Singularity worked out between the documents this request carried,
	// as the model was given it (TEMPORAL_CONTEXT), each relation with the
	// dates it rests on - kept on the operation as its provenance.
	std::vector<context::temporal_context::TemporalRelation> temporal_relations;
	// The model reads this request's values as protected symbols and was told
	// to write its figures as statements for Singularity to work out
	// (Context/ComputedStatements): its review is told the same, and what it
	// writes is checked before it is taken.
	bool computed_statements_offered = false;
	// The protected computation protocol runs for this request (DESIGN4 §1):
	// its operations are typed ops, answered over the operation's own table.
	bool compute_protocol_offered = false;
	// The request's own checks of the answer's text (its statements, its
	// chronology), before the check every professional response has of what
	// it says it changed (with_claimed_change_check).
	bool answer_text_checked = false;
};

// The documents a request's passages come from, as Singularity reads them, under
// the handles the model knows them by - dated once, for the order worked out
// between them on this computer (Context/TemporalContext).
std::vector<context::temporal_context::DatedDocument> dated_request_documents(
		const std::filesystem::path &workspace_path, const matters::Matter &matter,
		const std::vector<context::RetrievedContextChunk> &document_context, const context::SourceMarkerRegistry &markers) {
	std::vector<context::temporal_context::SuppliedDocument> supplied;
	std::unordered_set<std::string> seen;
	const documents::LocalDocumentExtractor extractor;
	const documents::DocumentCache cache(workspace_path, extractor);
	for (const context::RetrievedContextChunk &source : document_context) {
		if (!seen.insert(source.document_id).second) continue;
		const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == source.document_id; });
		if (document == matter.documents.end()) continue;
		std::string text;
		try {
			if (const auto extraction = cache.load(document->fingerprint, document->extractor_version)) text = extraction->text;
		} catch (const std::exception &) {
		}
		supplied.push_back({markers.document_token_for(source.chunk.chunk_id), document->id, document->relative_path,
			std::move(text)});
	}
	return context::temporal_context::date_documents(supplied);
}

// A Stop (or any invalidation) stops the operation before its next model
// call: later rounds - AdvancedWorkflow, review, corrections - never start
// after the person asked to stop. The usage already billed stays on the task.
void require_operation_current(const routing::InferenceRequest &request) {
	if (request.tool_execution_context && request.tool_execution_context->operation_is_current
			&& !request.tool_execution_context->operation_is_current())
		throw std::runtime_error("The operation was stopped before its next model call.");
}

matters::RequestCoverage recorded_coverage(const context::PromptCoverage &coverage) {
	matters::RequestCoverage recorded{coverage.context_items, coverage.omitted_context_item_ids,
		coverage.conversation_turns, coverage.turns_in_full, coverage.turns_as_records, coverage.omitted_turns};
	for (const context::PromptSourceCoverage &source : coverage.sources)
		recorded.sources.push_back(matters::SourceCoverage{source.document_id, source.supplied, source.retrieved,
			source.explicitly_selected});
	recorded.relevance_selection = coverage.relevance_selection;
	recorded.relevance_candidate_documents = coverage.relevance_candidate_documents;
	return recorded;
}

// The documents whose passages this request's prompt actually carries (after
// any accepted continuation) - what preview and the Routing Log report as
// sent, never every passage retrieval considered.
std::unordered_set<std::string> supplied_document_ids(const PreparedTaskRequest &prepared) {
	std::unordered_set<std::string> ids;
	for (const context::ResolvedSourceReference &reference : prepared.supplied_markers.references())
		ids.insert(reference.document_id);
	return ids;
}

// The Project tool calls a model's validated answer asks for, when it asks to
// read more of the Project before it answers (ResearchToolLoop.hpp's
// finalTextProjectEvidenceCalls).
nlohmann::json project_evidence_calls_from_final_text(const std::string &text) {
	const auto proposal = documents::parse_professional_response_proposal(text);
	if (!proposal || !proposal->project_evidence_request_json) return nlohmann::json::array();
	return context::project_evidence::tool_calls_for(
		nlohmann::json::parse(*proposal->project_evidence_request_json, nullptr, false));
}

// The question a model's validated answer puts to the lawyer, when it asks
// anything (ResearchToolLoop.hpp's finalTextLawyerQuestion). ToolBroker's own
// check of lawyer.ask decides whether it can be put as written.
std::optional<nlohmann::json> lawyer_question_from_final_text(const std::string &text) {
	const auto proposal = documents::parse_professional_response_proposal(text);
	if (!proposal || !proposal->lawyer_question_json) return std::nullopt;
	nlohmann::json question = nlohmann::json::parse(*proposal->lawyer_question_json, nullptr, false);
	if (!tools::lawyer_question::asks_anything(question)) return std::nullopt;
	// What the lawyer reads on the question card is about their matter, never
	// about Singularity's protection of it.
	const auto plain = [](nlohmann::json &holder, const char *key) {
		if (holder.is_object() && holder.contains(key) && holder.at(key).is_string())
			holder[key] = documents::plain_words_for_lawyer(holder.at(key).get<std::string>());
	};
	plain(question, "title");
	for (nlohmann::json &asked : question["questions"]) {
		plain(asked, "prompt");
		plain(asked, "note");
		if (asked.is_object() && asked.contains("options") && asked.at("options").is_array())
			for (nlohmann::json &option : asked["options"]) {
				plain(option, "label");
				plain(option, "note");
			}
	}
	return question;
}

// Every statement a final answer writes that Singularity cannot work out, in
// every text of it - the answer, the proposed document, its cells and edits -
// except the questions it asks (localFacts), which are answered, not written.
std::vector<context::computed_statements::StatementFailure> statement_failures_in(const std::string &text) {
	std::vector<context::computed_statements::StatementFailure> failures;
	const auto check = [&](const std::string &written) {
		if (!context::computed_statements::has_statements(written)) return;
		for (context::computed_statements::StatementFailure &failure :
				context::computed_statements::compute_checked(written).failures)
			failures.push_back(std::move(failure));
	};
	const std::function<void(const Json &)> walk = [&](const Json &value) {
		if (value.is_string()) { check(value.get<std::string>()); return; }
		if (value.is_array()) { for (const Json &entry : value) walk(entry); return; }
		if (!value.is_object()) return;
		for (auto entry = value.begin(); entry != value.end(); ++entry)
			if (entry.key() != "localFacts") walk(entry.value());
	};
	const Json value = Json::parse(text, nullptr, false);
	if (value.is_object()) walk(value);
	else check(text);
	return failures;
}

// The check a final answer passes before it is taken: each statement Singularity
// could not work out, as the model wrote it, and why - one line each, for the
// model to write again - then the whole response as it was written, field by
// field, so the model corrects those statements in the answer it gave rather
// than writing another. Its operands are the protected values inside ⟦ ⟧, so
// the correction reaches the model with the symbols it wrote.
std::optional<research::FinalTextIssue> computed_statement_issue(const std::string &text,
		const std::vector<research::ExternalEvidenceReference> &) {
	const std::vector<context::computed_statements::StatementFailure> failures = statement_failures_in(text);
	if (failures.empty()) return std::nullopt;
	std::string message;
	std::set<std::string> named;
	for (const context::computed_statements::StatementFailure &failure : failures)
		if (named.insert(failure.statement).second) message += "- " + failure.statement + " - " + failure.reason + "\n";
	message += "Your previous response, field by field as you wrote it:\n"
		+ documents::readable_professional_response(text);
	return research::FinalTextIssue{research::FinalTextIssueKind::CorrectionOnly, message, "computed_statement"};
}

// ---- A chronology without its dates --------------------------------------------------------------
// Protection sends a Project's dates as symbols, and a model that reads a
// symbol as a secret sets events out without them: a live Golden run (25 Sep
// 2026, Papinian on) answered "what changed from our previous position" with a
// chronology - earlier draft, signed agreement, Board-approved renewal position
// - and not one date, though each of those documents carries its own and each
// symbol would have come back real. A chronology a lawyer reads is its dates.
// So an answer whose chronology dates none of its events, where the documents
// it rests on are dated, goes back to the same model once, with the response
// it gave: never a second time and never a note to the lawyer - after that one
// correction the answer is taken as it is.

// One chronology an answer sets out: what it is called, the text of each of
// its rows, and every text of it that may cite a passage.
struct AnswerChronology {
	std::string name;
	std::vector<std::string> rows;
	std::string citing;
};

// Whether a caption calls what it heads a chronology, in the languages Singularity
// is used in. The model's own words about its own table, never the request's.
bool names_chronology(const std::string &caption) {
	static const std::vector<std::string> kWords{"chronolog", "cronolog", "kronolog", "timeline", "time line",
		"хронолог", "年表", "时间线", "時間線", "時系列", "연표", "타임라인", "التسلسل الزمني"};
	const std::string folded = platform::folded_text(caption);
	for (const std::string &word : kWords)
		if (folded.find(word) != std::string::npos) return true;
	return false;
}

// A line of an answer as its reader sees it: no markdown emphasis or heading
// marks, no spaces around it, no colon closing it.
std::string plain_caption(const std::string &line) {
	std::string plain;
	for (const char character : line)
		if (character != '*' && character != '#' && character != '|') plain += character;
	const std::size_t begin = plain.find_first_not_of(" \t\r");
	if (begin == std::string::npos) return std::string();
	std::size_t end = plain.find_last_not_of(" \t\r:");
	if (end == std::string::npos || end < begin) end = begin;
	return plain.substr(begin, end - begin + 1);
}

// Whether a line of an answer heads a section: a markdown heading, or a line
// that is bold and nothing else.
bool is_section_heading(const std::string &line) {
	if (!line.empty() && line[0] == '#') return true;
	return line.size() > 4 && line.rfind("**", 0) == 0 && line.compare(line.size() - 2, 2, "**") == 0
		&& line.find("**", 2) == line.size() - 2;
}

// Whether a line of an answer introduces what follows it: a short line closed
// by a colon ("The events in order:"), never a sentence of the answer's prose.
bool is_lead_line(const std::string &line) {
	if (line.empty() || line.back() != ':') return false;
	std::size_t words = 1;
	for (const char character : line)
		if (character == ' ') ++words;
	return words <= 12;
}

// Every markdown table `text` writes under a caption naming a chronology: the
// section heading it stands in, or the short line introducing it. A column
// header ("Timeline" of an obligation) or a sentence of prose that mentions a
// timeline makes no table a chronology.
void add_markdown_chronologies(const std::string &text, std::vector<AnswerChronology> &found) {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t end = text.find('\n', start);
		std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		const std::size_t begin = line.find_first_not_of(" \t\r");
		const std::size_t last = line.find_last_not_of(" \t\r");
		lines.push_back(begin == std::string::npos ? std::string() : line.substr(begin, last - begin + 1));
		if (end == std::string::npos) break;
		start = end + 1;
	}
	std::string section;
	std::string lead;
	for (std::size_t index = 0; index < lines.size();) {
		if (lines[index].empty() || lines[index][0] != '|') {
			if (is_section_heading(lines[index])) {
				section = lines[index];
				lead.clear();
			} else if (is_lead_line(lines[index])) {
				lead = lines[index];
			}
			++index;
			continue;
		}
		AnswerChronology table;
		const std::string header = lines[index];
		const std::string heading = names_chronology(lead) || section.empty() ? lead : section;
		table.name = plain_caption(heading.empty() ? header : heading);
		for (++index; index < lines.size() && !lines[index].empty() && lines[index][0] == '|'; ++index) {
			// The line between the header and the rows is no row.
			if (lines[index].find_first_not_of("|-: \t") == std::string::npos) continue;
			table.rows.push_back(lines[index]);
			table.citing += lines[index] + "\n";
		}
		if (names_chronology(section + " " + lead)) found.push_back(std::move(table));
		lead.clear();
	}
}

// Every chronology a final answer sets out: a table or list of its reusable
// document, or of its written answer, captioned as one, and its typed events,
// which are a chronology by what they are.
std::vector<AnswerChronology> chronologies_in(const std::string &text) {
	std::vector<AnswerChronology> found;
	const Json value = Json::parse(text, nullptr, false);
	if (!value.is_object()) {
		add_markdown_chronologies(text, found);
		return found;
	}
	const auto textOf = [](const Json &holder, const char *key) {
		return holder.is_object() && holder.contains(key) && holder.at(key).is_string()
			? holder.at(key).get<std::string>() : std::string();
	};
	add_markdown_chronologies(textOf(value, "response"), found);
	if (value.contains("workProduct") && value.at("workProduct").is_object()) {
		const Json &document = value.at("workProduct");
		const std::string title = textOf(document, "title");
		if (document.contains("blocks") && document.at("blocks").is_array())
			for (const Json &block : document.at("blocks")) {
				const std::string type = textOf(block, "type");
				if (type != "table" && type != "list") continue;
				AnswerChronology chronology;
				const std::string label = textOf(block, "label");
				// A block is captioned by its own label; the document's title captions
				// only a block that has none - "Parties" of a chronology memo is no
				// chronology, nor is a column headed "Timeline".
				chronology.name = label.empty() ? title : label;
				const std::string caption = chronology.name;
				const char *entries = type == "table" ? "rows" : "items";
				if (block.contains(entries) && block.at(entries).is_array())
					for (const Json &entry : block.at(entries)) {
						std::string row;
						if (entry.is_string()) row = entry.get<std::string>();
						if (entry.is_array())
							for (const Json &cell : entry)
								if (cell.is_string()) row += (row.empty() ? "" : " | ") + cell.get<std::string>();
						chronology.rows.push_back(row);
						chronology.citing += row + "\n";
					}
				if (block.contains("sources")) chronology.citing += block.at("sources").dump();
				if (names_chronology(caption)) found.push_back(std::move(chronology));
			}
	}
	if (value.contains("structuredResults") && value.at("structuredResults").is_object()
			&& value.at("structuredResults").contains("events") && value.at("structuredResults").at("events").is_array()) {
		AnswerChronology events;
		events.name = "structuredResults.events";
		for (const Json &event : value.at("structuredResults").at("events")) {
			const std::string row = textOf(event, "date") + " " + textOf(event, "description");
			events.rows.push_back(row);
			events.citing += row + "\n" + (event.is_object() && event.contains("sources") ? event.at("sources").dump() : "");
		}
		found.push_back(std::move(events));
	}
	return found;
}

// The document handles a text cites ("DOC_2" of "[SOURCE:DOC_2:CHUNK_1]").
std::set<std::string> cited_document_handles(const std::string &text) {
	std::set<std::string> handles;
	const std::string opening = "SOURCE:";
	for (std::size_t at = text.find(opening); at != std::string::npos; at = text.find(opening, at + opening.size())) {
		const std::size_t from = at + opening.size();
		const std::size_t end = text.find_first_of(":]", from);
		if (end == std::string::npos) break;
		handles.insert(text.substr(from, end - from));
	}
	return handles;
}

// The check itself: the first chronology of the answer with two rows or more
// and no date in any of them, where at least two of the documents it cites -
// every supplied document, when it cites none - are dated by their own date
// (Context/TemporalContext, the dates protection sent as symbols).
std::optional<research::FinalTextIssue> undated_chronology_issue(const std::string &text,
		const std::vector<context::temporal_context::DatedDocument> &supplied) {
	for (const AnswerChronology &chronology : chronologies_in(text)) {
		if (chronology.rows.size() < 2) continue;
		bool isDated = false;
		for (const std::string &row : chronology.rows)
			if (!context::temporal::find(row).empty() || !context::temporal::find_days_of_year(row).empty()) {
				isDated = true;
				break;
			}
		if (isDated == true) continue;
		const std::set<std::string> cited = cited_document_handles(chronology.citing);
		std::size_t datedCount = 0;
		for (const context::temporal_context::DatedDocument &document : supplied)
			if (document.dating.date && document.dating.conflicting == false
					&& (cited.empty() || cited.count(document.document.handle) != 0))
				++datedCount;
		if (datedCount < 2) continue;
		const std::string message = "Your chronology" + (chronology.name.empty() ? std::string()
				: " \"" + chronology.name + "\"")
			+ " gives none of its events a date, though the documents it rests on are dated. Return your complete "
			"response again with each event's date written where the lawyer reads that event - in the chronology and "
			"wherever the answer sets the same events out - by the date's symbol, which Singularity puts back as the real "
			"date before the lawyer reads it. An event the documents do not date stays without one: never write a date "
			"they do not give. Keep everything else as you had it.\nYour previous response, field by field as you "
			"wrote it:\n" + documents::readable_professional_response(text);
		return research::FinalTextIssue{research::FinalTextIssueKind::CorrectionOnly, message, "undated_chronology"};
	}
	return std::nullopt;
}

// ---- A change claimed and not made -----------------------------------------------------------------
// An answer that says it changed a document but carries no change: the lawyer
// would read that the document was changed when nothing was - or read a
// colleague saying it "would" change it, when they asked it to. Read from the
// response's own fields: no action on a document and no record, and an answer
// that says, in the first person, that it made (or would make) a change. A
// sentence about a change made before ("already", "earlier", "approved") or one
// it did not make ("not") says nothing of this answer.
bool claims_a_change_not_made(const documents::ProfessionalResponseProposal &proposal) {
	if (proposal.artifact_action || proposal.structured_record_json) return false;
	// English by word; Russian by its own verbs, written as UTF-8 bytes.
	static const std::regex claim(
		std::string("\\bI(?:'ve| have)?\\s+(?:just\\s+)?(?:made|changed|revised|amended|updated|edited|replaced|inserted|added|"
			"deleted|removed|rewrote|rewritten|corrected)\\b|\\bI\\s+(?:would|will|can|could)\\s+(?:change|revise|amend|update|"
			"edit|replace|make)\\b|") + "(?:^|\\s)(?:\xd0\xaf|\xd1\x8f)\\s+(?:\xd1\x83\xd0\xb6\xd0\xb5\\s+)?(?:\xd0\xb2\xd0\xbd\xd0\xb5\xd1\x81|\xd0\xb2\xd0\xbd\xd1\x91\xd1\x81|\xd0\xb8\xd0\xb7\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xbb|\xd0\xb8\xd1\x81\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xb8\xd0\xbb|\xd0\xbe\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd0\xbb|\xd0\xb7\xd0\xb0\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xbb|\xd0\xb4\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xb8\xd0\xbb|\xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb8\xd0\xbb|\xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd0\xbb)",
		std::regex::ECMAScript | std::regex::icase);
	std::string sentence;
	const auto said = [&]() {
		// A typographic apostrophe ("I\u2019ve") reads as the plain one.
		for (std::size_t at = sentence.find("\xe2\x80\x99"); at != std::string::npos; at = sentence.find("\xe2\x80\x99", at))
			sentence.replace(at, 3, "'");
		std::string lower = sentence;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		const bool aboutBefore = lower.find("already") != std::string::npos || lower.find("earlier") != std::string::npos
			|| lower.find("previous") != std::string::npos || lower.find("approved") != std::string::npos
			|| lower.find(" not ") != std::string::npos || lower.find("n't") != std::string::npos
			|| lower.find("\xd1\x83\xd0\xb6\xd0\xb5") != std::string::npos;
		const bool claimed = !aboutBefore && std::regex_search(sentence, claim);
		sentence.clear();
		return claimed;
	};
	for (const char character : proposal.response) {
		sentence += character;
		if ((character == '.' || character == '!' || character == '?' || character == '\n') && said()) return true;
	}
	return said();
}

std::optional<research::FinalTextIssue> claimed_change_issue(const std::string &text) {
	const std::optional<documents::ProfessionalResponseProposal> proposal =
		documents::parse_professional_response_proposal(text);
	if (!proposal || !claims_a_change_not_made(*proposal)) return std::nullopt;
	const std::string message = "Your answer says you changed a document - or that you would - but it carries no change, "
		"so nothing would be written while the lawyer reads that it was. Return your complete response again with the "
		"change itself as its action: actionType for the kind of change (replace_content for new words in place of "
		"words the document has) and artifactAction naming the document, with matchText copied exactly from the "
		"document as it reads now and replacementText the words that replace it - or, if you are not changing any "
		"document, say plainly that you have not changed it. Keep everything else as you had it.\nYour previous "
		"response, field by field as you wrote it:\n" + documents::readable_professional_response(text);
	return research::FinalTextIssue{research::FinalTextIssueKind::CorrectionOnly, message, "claimed_change"};
}

// Every professional response is also held, once, to what it says it did
// (claimed_change_issue), after the checks the request already runs.
std::function<std::optional<research::FinalTextIssue>(const std::string &,
		const std::vector<research::ExternalEvidenceReference> &)>
with_claimed_change_check(std::function<std::optional<research::FinalTextIssue>(const std::string &,
		const std::vector<research::ExternalEvidenceReference> &)> inner) {
	return [inner = std::move(inner), isClaimChecked = std::make_shared<bool>(false)](const std::string &text,
			const std::vector<research::ExternalEvidenceReference> &evidence) -> std::optional<research::FinalTextIssue> {
		if (inner)
			if (std::optional<research::FinalTextIssue> issue = inner(text, evidence)) return issue;
		if (*isClaimChecked == true) return std::nullopt;
		std::optional<research::FinalTextIssue> issue = claimed_change_issue(text);
		if (issue) *isClaimChecked = true;
		return issue;
	};
}

// ---- Figures left out ----------------------------------------------------------------------------
// After its corrections an answer may still ask for a figure Singularity cannot
// work out. The lawyer then reads the answer without that figure, and one
// calm sentence after it says which figure and why - never a guess, and never
// the wording of the calculation the model wrote, which is the model's and
// not the lawyer's. The answer is kept so: what could not be worked out is
// taken out of it, so that nothing the model left open ("{{= …" never
// closed, an {{if}} with no {{end}}) reaches the sentence written after it,
// and a later request reads what the lawyer read.

const std::string kOperandOpen = "\xE2\x9F\xA6";
const std::string kOperandClose = "\xE2\x9F\xA7";

// The keyword of the tag a "{{" at `at` opens, read as ComputedStatements
// reads an {{if}} block: "=" for a value, "if", "end" ("{{/if}}" and
// "{{endif}}" too), "tag" for any other statement tag, "" for none.
std::string opened_tag(std::string_view text, std::size_t at) {
	std::size_t cursor = at + 2;
	while (cursor < text.size() && text[cursor] == ' ') ++cursor;
	const auto word = [&text](std::size_t from, std::string_view expected) {
		if (from + expected.size() > text.size()) return false;
		for (std::size_t index = 0; index < expected.size(); ++index)
			if (std::tolower(static_cast<unsigned char>(text[from + index])) != expected[index]) return false;
		const std::size_t after = from + expected.size();
		return after == text.size() || (std::isalnum(static_cast<unsigned char>(text[after])) == 0 && text[after] != '_');
	};
	if (cursor < text.size() && text[cursor] == '=') return "=";
	if (cursor < text.size() && text[cursor] == '/') {
		std::size_t next = cursor + 1;
		while (next < text.size() && text[next] == ' ') ++next;
		return word(next, "if") ? "end" : "";
	}
	if (word(cursor, "endif") || word(cursor, "end")) return "end";
	if (word(cursor, "if")) return "if";
	for (const char *other : {"elseif", "elif", "else", "case", "gender"})
		if (word(cursor, other)) return "tag";
	return "";
}

// The "}}" closing a tag whose head begins at `from`: the one that balances
// every "{{" opened inside it. What stands inside ⟦ ⟧ is a value, never a brace.
std::size_t head_close(std::string_view text, std::size_t from) {
	int depth = 0;
	for (std::size_t index = from; index + 1 < text.size();) {
		if (text.compare(index, kOperandOpen.size(), kOperandOpen) == 0) {
			const std::size_t close = text.find(kOperandClose, index + kOperandOpen.size());
			if (close == std::string_view::npos) return std::string_view::npos;
			index = close + kOperandClose.size();
			continue;
		}
		if (text[index] == '{' && text[index + 1] == '{') {
			++depth;
			index += 2;
			continue;
		}
		if (text[index] == '}' && text[index + 1] == '}') {
			if (depth == 0) return index;
			--depth;
			index += 2;
			continue;
		}
		++index;
	}
	return std::string_view::npos;
}

// Just past the {{end}} of the {{if}} whose words begin at `from`, or npos
// when the model never closed it. A tag's own head is passed over whole.
std::size_t block_end(std::string_view text, std::size_t from) {
	std::size_t depth = 0;
	std::size_t cursor = from;
	while (cursor < text.size()) {
		const std::size_t next = text.find("{{", cursor);
		if (next == std::string_view::npos) break;
		const std::string tag = opened_tag(text, next);
		const std::size_t close = tag.empty() ? std::string_view::npos : head_close(text, next + 2);
		if (close == std::string_view::npos) {
			cursor = next + 1;
			continue;
		}
		if (tag == "if") {
			++depth;
		} else if (tag == "end") {
			if (depth == 0) return close + 2;
			--depth;
		}
		cursor = close + 2;
	}
	return std::string_view::npos;
}

struct FigureLeftOut {
	std::size_t at = 0;  // where it stood, in the answer as it is kept
	std::string reason;
};

struct SettledAnswer {
	std::string text;
	std::vector<FigureLeftOut> left_out;
	// Nothing in `text` is left for Singularity to fail on.
	bool settled = true;
	// The answer's fact anchors, carried through each piece taken out: one
	// inside a piece goes with it, one after it moves back by its length.
	std::vector<protected_api::papinian::FactAnchor> anchors;
};

// `answer` with every statement Singularity cannot work out taken out: a value
// and a tag that opens nothing go whole, an {{if}} whose condition cannot be
// decided goes with the words it chooses between, and an {{if}} the model
// never closed but that can be decided is closed where the answer ends, as
// ComputedStatements reads it. What works out stays as the model wrote it.
SettledAnswer settle_unworked_statements(const std::string &answer,
		const std::vector<protected_api::papinian::FactAnchor> &anchors = {}) {
	SettledAnswer settled{answer, {}, true, anchors};
	std::string &text = settled.text;
	// Takes [from, to) out; the words on either side keep one space, none
	// stands before a stop and no line begins with one. Where it was, in the
	// text that remains.
	const auto takeOut = [&settled, &text](std::size_t from, std::size_t to) {
		if (from > 0 && text[from - 1] == ' '
				&& (to >= text.size() || text[to] == ' ' || std::string_view(".,;:!?)").find(text[to]) != std::string_view::npos))
			--from;
		else if ((from == 0 || text[from - 1] == '\n') && to < text.size() && text[to] == ' ')
			++to;
		to = std::min(to, text.size());
		text.erase(from, to - from);
		for (FigureLeftOut &figure : settled.left_out)
			if (figure.at > from) figure.at = figure.at >= to ? figure.at - (to - from) : from;
		std::vector<protected_api::papinian::FactAnchor> carried;
		for (protected_api::papinian::FactAnchor &anchor : settled.anchors) {
			if (anchor.start + anchor.length <= from) carried.push_back(std::move(anchor));
			else if (anchor.start >= to) {
				anchor.start -= to - from;
				carried.push_back(std::move(anchor));
			}
		}
		settled.anchors = std::move(carried);
		return from;
	};
	for (int pass = 0; pass < 3; ++pass) {
		const std::vector<context::computed_statements::StatementFailure> failures =
			context::computed_statements::compute_checked(text).failures;
		if (failures.empty()) return settled;
		std::size_t cursor = 0;
		for (std::size_t index = 0; index < failures.size(); ++index) {
			const context::computed_statements::StatementFailure &failure = failures[index];
			if (failure.statement.empty()) continue;
			const std::size_t at = text.find(failure.statement, cursor);
			if (at == std::string::npos) continue;
			const std::string tag = opened_tag(text, at);
			if (tag != "if") {
				cursor = takeOut(at, at + failure.statement.size());
				if (tag == "=") settled.left_out.push_back({cursor, failure.reason});
				continue;
			}
			// An {{if}} is reported by its head, once for each thing wrong with it:
			// that it was never closed, then why its condition cannot be decided.
			std::string reason = failure.reason;
			while (index + 1 < failures.size() && failures[index + 1].statement == failure.statement)
				reason = failures[++index].reason;
			const std::size_t words = at + failure.statement.size();
			const std::size_t end = block_end(text, words);
			const bool undecided = !context::computed_statements::compute_checked(failure.statement + "{{end}}").failures.empty();
			if (undecided) {
				cursor = takeOut(at, end == std::string_view::npos ? text.size() : end);
				settled.left_out.push_back({cursor, reason});
			} else {
				if (end == std::string_view::npos) text += "{{end}}";
				cursor = words;
			}
		}
	}
	settled.settled = context::computed_statements::compute_checked(text).failures.empty();
	return settled;
}

// Why a figure could not be worked out, as the lawyer reads it: the reason
// itself when it speaks only of the lawyer's own values - "⟦$100.00⟧ and
// ⟦€5.00⟧ are amounts in different currencies", "⟦July 2026⟧ is a month that
// contains ⟦July 25, 2026⟧" - up to the advice it gives the model; else that
// the calculation written for it could not be done. The values stay inside
// ⟦ ⟧, so a later request that reads the answer reads them protected.
std::string plain_reason(const std::string &reason, bool russian) {
	const std::string calculation = russian ? "\xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB9 \xD0\xB4\xD0\xBB\xD1\x8F \xD0\xBD\xD0\xB5\xD1\x91 \xD1\x80\xD0\xB0\xD1\x81\xD1\x87\xD1\x91\xD1\x82 \xD0\xBD\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB2\xD1\x8B\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C"
		: "the calculation written for it could not be done";
	std::string said;
	bool names = false;
	for (std::size_t at = 0; at < reason.size();) {
		if (reason.compare(at, kOperandOpen.size(), kOperandOpen) == 0) {
			const std::size_t close = reason.find(kOperandClose, at + kOperandOpen.size());
			if (close == std::string::npos) return calculation;
			const std::string value = reason.substr(at + kOperandOpen.size(), close - at - kOperandOpen.size());
			if (value.empty() || value.front() == ' ' || value.back() == ' ' || value.find("{{") != std::string::npos
					|| value.find("}}") != std::string::npos)
				return calculation;
			names = true;
			said.append(reason, at, close + kOperandClose.size() - at);
			at = close + kOperandClose.size();
			continue;
		}
		if (reason.compare(at, kOperandClose.size(), kOperandClose) == 0) return calculation;
		if (reason.compare(at, 2, "\xC2\xAB") == 0 || reason.compare(at, 2, "\xC2\xBB") == 0) return calculation;
		const char byte = reason[at];
		if (byte == ':') break;
		if (std::string_view("{}\"*/=<>+[]`").find(byte) != std::string_view::npos) return calculation;
		said.push_back(byte);
		++at;
	}
	while (!said.empty() && std::string_view(" .,;-").find(said.back()) != std::string_view::npos) said.pop_back();
	return names && !said.empty() ? said : calculation;
}

// Markup and stops that are not words of their own.
std::string trimmed_words(std::string words) {
	const std::string_view edge = " \t\r*_#`>|-:;,.!?(\"";
	while (!words.empty() && edge.find(words.back()) != std::string_view::npos) words.pop_back();
	std::size_t start = 0;
	while (start < words.size() && edge.find(words[start]) != std::string_view::npos) ++start;
	return words.substr(start);
}

bool has_word(const std::string &text) {
	return std::any_of(text.begin(), text.end(), [](char byte) {
		return std::isalnum(static_cast<unsigned char>(byte)) != 0 || static_cast<unsigned char>(byte) >= 0x80; });
}

// The words right before a figure left out, on its line and after any figure
// left out before it - "The total is" - by which the lawyer finds it; else
// the words right after it, up to the next (`after` is then false). Only the
// answer's own words: never a statement or an operand.
std::string words_beside(const std::string &text, std::size_t at, std::size_t floor, std::size_t ceiling, bool &after) {
	const auto split = [](const std::string &words) {
		std::vector<std::string> split;
		std::size_t cursor = 0;
		while (cursor < words.size()) {
			const std::size_t start = words.find_first_not_of(" \t", cursor);
			if (start == std::string::npos) break;
			const std::size_t end = std::min(words.find_first_of(" \t", start), words.size());
			split.push_back(words.substr(start, end - start));
			cursor = end;
		}
		return split;
	};
	const auto joined = [](std::vector<std::string>::const_iterator from, std::vector<std::string>::const_iterator to) {
		std::string words;
		for (auto word = from; word != to; ++word) words += (words.empty() ? "" : " ") + *word;
		return words;
	};
	const std::size_t lineBreak = at == 0 ? std::string::npos : text.rfind('\n', at - 1);
	const std::size_t lineStart = std::max(lineBreak == std::string::npos ? 0 : lineBreak + 1, std::min(floor, at));
	std::string before = text.substr(lineStart, at - lineStart);
	std::size_t cut = 0;
	for (const std::string &mark : {std::string("{{"), std::string("}}"), kOperandOpen, kOperandClose}) {
		const std::size_t found = before.rfind(mark);
		if (found != std::string::npos) cut = std::max(cut, found + mark.size());
	}
	before = before.substr(cut);
	for (const char *stop : {". ", "! ", "? ", "; "}) {
		const std::size_t found = before.rfind(stop);
		if (found != std::string::npos && has_word(before.substr(found + 2))) before = before.substr(found + 2);
	}
	const std::vector<std::string> wordsBefore = split(before);
	std::string words = trimmed_words(joined(wordsBefore.size() > 5 ? wordsBefore.end() - 5 : wordsBefore.begin(),
		wordsBefore.end()));
	after = true;
	if (has_word(words)) return words;
	const std::size_t lineEnd = std::min({text.find('\n', at), text.size(), std::max(ceiling, at)});
	std::string following = text.substr(at, lineEnd - at);
	std::size_t stop = following.size();
	for (const std::string &mark : {std::string("{{"), std::string("}}"), kOperandOpen, kOperandClose, std::string(". "),
			std::string("! "), std::string("? "), std::string("; ")})
		stop = std::min(stop, following.find(mark));
	const std::vector<std::string> wordsAfter = split(following.substr(0, stop));
	words = trimmed_words(joined(wordsAfter.begin(), wordsAfter.size() > 5 ? wordsAfter.begin() + 5 : wordsAfter.end()));
	after = false;
	return has_word(words) ? words : std::string();
}

// The one sentence the lawyer reads after an answer with figures left out:
// which - by the words beside each - and why, in the answer's language.
std::string figures_left_out_sentence(const SettledAnswer &settled, bool russian) {
	std::vector<std::pair<std::string, std::string>> figures;  // where, why
	std::set<std::string> said;
	for (const FigureLeftOut &figure : settled.left_out) {
		std::size_t floor = 0, ceiling = settled.text.size();
		for (const FigureLeftOut &other : settled.left_out) {
			if (other.at < figure.at) floor = std::max(floor, other.at);
			if (other.at > figure.at) ceiling = std::min(ceiling, other.at);
		}
		bool after = true;
		const std::string words = words_beside(settled.text, figure.at, floor, ceiling, after);
		std::string where;
		if (!words.empty())
			where = russian ? std::string(after ? "\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5 \xC2\xAB" : "\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB4 \xC2\xAB") + words + "\xC2\xBB"
				: std::string(after ? "the one after \xE2\x80\x9C" : "the one before \xE2\x80\x9C") + words + "\xE2\x80\x9D";
		std::string why = plain_reason(figure.reason, russian);
		if (said.insert(where + "\n" + why).second && figures.size() < 5) figures.emplace_back(std::move(where), std::move(why));
	}
	if (figures.empty()) return {};
	if (settled.left_out.size() == 1) {
		const auto &[where, why] = figures.front();
		return std::string(russian ? "\xD0\x9E\xD0\xB4\xD0\xBD\xD0\xB0 \xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB8\xD0\xBD\xD0\xB0 \xD0\xB7\xD0\xB4\xD0\xB5\xD1\x81\xD1\x8C \xD0\xBD\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xB0" : "One figure is not given here")
			+ (where.empty() ? std::string() : ", " + where) + ": " + why + ".";
	}
	std::string sentence = russian ? "\xD0\x9D\xD0\xB5\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB5 \xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB8\xD0\xBD\xD1\x8B \xD0\xB7\xD0\xB4\xD0\xB5\xD1\x81\xD1\x8C \xD0\xBD\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8B: " : "Some figures are not given here: ";
	for (std::size_t index = 0; index < figures.size(); ++index)
		sentence += (index == 0 ? "" : "; ") + (figures[index].first.empty() ? figures[index].second
			: figures[index].first + " - " + figures[index].second);
	return sentence + ".";
}

// Whether an answer is written in Russian: Cyrillic letters outnumber Latin
// ones among its own words - a statement ("{{= … }}"), an operand ("⟦…⟧")
// and a symbol ("[[E5]]") are not words of the answer. With no words of its
// own, the letters it has.
bool written_in_russian(std::string_view answer) {
	std::size_t cyrillic = 0, latin = 0, allCyrillic = 0, allLatin = 0;
	std::size_t depth = 0;
	bool operand = false, symbol = false;
	for (std::size_t at = 0; at < answer.size(); ++at) {
		const std::string_view rest = answer.substr(at);
		if (rest.rfind("{{", 0) == 0) { ++depth; ++at; continue; }
		if (depth > 0 && rest.rfind("}}", 0) == 0) { --depth; ++at; continue; }
		if (rest.rfind("\xE2\x9F\xA6", 0) == 0) { operand = true; at += 2; continue; }
		if (rest.rfind("\xE2\x9F\xA7", 0) == 0) { operand = false; at += 2; continue; }
		if (rest.rfind("[[", 0) == 0) { symbol = true; ++at; continue; }
		if (symbol && rest.rfind("]]", 0) == 0) { symbol = false; ++at; continue; }
		const auto byte = static_cast<unsigned char>(answer[at]);
		const bool isCyrillic = (byte == 0xD0 || byte == 0xD1) && at + 1 < answer.size();
		const bool isLatin = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
		const bool word = depth == 0 && !operand && !symbol;
		if (isCyrillic) {
			++allCyrillic;
			if (word) ++cyrillic;
			++at;
		} else if (isLatin) {
			++allLatin;
			if (word) ++latin;
		}
	}
	return cyrillic + latin == 0 ? allCyrillic > allLatin : cyrillic > latin;
}

// Where the top-level string member `key` of an object's JSON text stands: the
// bytes between its quotes, as the text writes them (escaped). A member of the
// same name nested deeper, or a string that merely contains the name, is not it.
std::optional<std::pair<std::size_t, std::size_t>> top_level_string_member(const std::string &json,
		std::string_view key) {
	int depth = 0;
	char previous = 0;  // the last character outside a string
	for (std::size_t at = 0; at < json.size(); ++at) {
		const char character = json[at];
		if (character == '"') {
			std::size_t end = at + 1;
			while (end < json.size() && json[end] != '"') end += json[end] == '\\' ? 2 : 1;
			if (end >= json.size()) return std::nullopt;
			const bool isKey = depth == 1 && (previous == '{' || previous == ',');
			if (isKey && std::string_view(json).substr(at + 1, end - at - 1) == key) {
				std::size_t value = end + 1;
				while (value < json.size() && std::isspace(static_cast<unsigned char>(json[value]))) ++value;
				if (value >= json.size() || json[value] != ':') return std::nullopt;
				++value;
				while (value < json.size() && std::isspace(static_cast<unsigned char>(json[value]))) ++value;
				if (value >= json.size() || json[value] != '"') return std::nullopt;
				std::size_t close = value + 1;
				while (close < json.size() && json[close] != '"') close += json[close] == '\\' ? 2 : 1;
				if (close >= json.size()) return std::nullopt;
				return std::make_pair(value + 1, close);
			}
			previous = '"';
			at = end;
			continue;
		}
		if (std::isspace(static_cast<unsigned char>(character))) continue;
		if (character == '{' || character == '[') ++depth;
		else if (character == '}' || character == ']') --depth;
		previous = character;
	}
	return std::nullopt;
}

// How many bytes the JSON string text escaped[from, to) stands for once read.
std::size_t unescaped_length(std::string_view escaped) {
	std::size_t length = 0;
	for (std::size_t at = 0; at < escaped.size();) {
		if (escaped[at] != '\\') { ++length; ++at; continue; }
		if (at + 1 < escaped.size() && escaped[at + 1] == 'u' && at + 6 <= escaped.size()) {
			const unsigned code = static_cast<unsigned>(std::stoul(std::string(escaped.substr(at + 2, 4)), nullptr, 16));
			if (code >= 0xD800 && code <= 0xDBFF) { length += 4; at += 12; continue; }  // a surrogate pair
			length += code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
			at += 6;
			continue;
		}
		++length;
		at += 2;
	}
	return length;
}

// The answer a model's restored result gives the lawyer, with the fact anchors
// restoration produced in that answer's own coordinates (DESIGN3 9.3.4). A
// structured result (professional_response) is its whole JSON text: only
// the anchors inside its "response" member are the answer's, moved by where
// that member stands - never by a writing's rank among every field, where a
// chat title or a draft before the response writing the same figure for
// another fact would shift it. A plain text is its own answer. An anchor
// whose writing is not wholly one amount, rate, period or date is not one
// restoration made - its offsets belong to another text - and is dropped:
// that figure is then sent as a fact of its own, still masked.
struct AnchoredAnswer {
	std::string text;
	std::vector<protected_api::papinian::FactAnchor> anchors;
};

AnchoredAnswer anchored_answer_of(const std::string &restored,
		const std::vector<protected_api::papinian::FactAnchor> &anchors) {
	AnchoredAnswer answer;
	const nlohmann::json parsed = nlohmann::json::parse(restored, nullptr, false);
	std::size_t from = 0;
	std::size_t to = restored.size();
	const bool structured = !parsed.is_discarded() && (parsed.is_object() || parsed.is_array() || parsed.is_string());
	if (structured) {
		const auto member = parsed.is_object() && parsed.contains("response") && parsed.at("response").is_string()
			? top_level_string_member(restored, "response") : std::nullopt;
		if (!member) return answer;
		answer.text = parsed.at("response").get<std::string>();
		from = member->first;
		to = member->second;
	} else {
		answer.text = restored;
	}
	for (const protected_api::papinian::FactAnchor &anchor : anchors) {
		if (anchor.length == 0 || anchor.start < from || anchor.start + anchor.length > to) continue;
		const std::string_view writing(restored.data() + anchor.start, anchor.length);
		if (writing.find('\\') != std::string_view::npos) continue;
		const std::size_t start = structured
			? unescaped_length(std::string_view(restored).substr(from, anchor.start - from)) : anchor.start;
		if (start + anchor.length > answer.text.size() || answer.text.compare(start, anchor.length, writing) != 0) continue;
		const std::optional<protected_api::papinian::semantic::Value> value = protected_api::papinian::semantic::parse_value(writing);
		if (!value || value->kind == protected_api::papinian::semantic::Kind::Boolean) continue;
		answer.anchors.push_back(protected_api::papinian::FactAnchor{start, anchor.length, anchor.key});
	}
	return answer;
}

// `anchors` over `from`, moved onto `to`, the stored answer Singularity made of
// it: where `to` holds `from` whole (after a note of its own, say), by that
// place; otherwise by where the edit keeps each very occurrence
// (carry_fact_anchors) - never by a writing's rank, which a figure taken out
// or a sentence of Singularity's own would pass to another fact's writing.
std::vector<protected_api::papinian::FactAnchor> anchors_onto(const std::string &from,
		const std::vector<protected_api::papinian::FactAnchor> &anchors, const std::string &to) {
	return protected_api::papinian::carry_fact_anchors(from, anchors, to);
}

// The answer as the lawyer reads it and as it is kept, once its corrections
// are spent: every figure it asks for that Singularity still cannot work out is
// left out, and one calm sentence after it says which and why. An answer
// whose statements all work out is returned as it was written. Its fact
// anchors go with it by position: one inside a figure left out goes with the
// figure, every other one stays on its own occurrence - the sentence's own
// writing of an operand is Singularity's and carries none.
AnchoredAnswer figures_left_out_of(const AnchoredAnswer &answer) {
	if (!context::computed_statements::has_statements(answer.text)) return answer;
	const SettledAnswer settled = settle_unworked_statements(answer.text, answer.anchors);
	if (settled.left_out.empty()) return {settled.text, settled.anchors};
	const std::string sentence = figures_left_out_sentence(settled, written_in_russian(answer.text));
	std::string text = settled.text;
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.pop_back();
	if (text.empty()) return {sentence, {}};
	std::vector<protected_api::papinian::FactAnchor> anchors;
	// What Singularity could not settle stays after the sentence, where it can
	// reach nothing written after it.
	const std::size_t shift = settled.settled ? 0 : sentence.size() + 2;
	for (const protected_api::papinian::FactAnchor &anchor : settled.anchors)
		if (anchor.start + anchor.length <= text.size())
			anchors.push_back(protected_api::papinian::FactAnchor{anchor.start + shift, anchor.length, anchor.key});
	return {settled.settled ? text + "\n\n" + sentence : sentence + "\n\n" + text, std::move(anchors)};
}

std::string figures_left_out_of(const std::string &answer) {
	return figures_left_out_of(AnchoredAnswer{answer, {}}).text;
}

// without_pending_approval_talk, its fact anchors carried by position: one in
// a sentence taken out goes with it, every other one moves back by what was
// taken out before it.
AnchoredAnswer without_pending_approval_talk(const AnchoredAnswer &answer) {
	AnchoredAnswer kept;
	std::size_t cursor = 0, removed = 0;
	const auto keep = [&](std::size_t from, std::size_t to) {
		kept.text.append(answer.text, from, to - from);
		for (const protected_api::papinian::FactAnchor &anchor : answer.anchors)
			if (anchor.start >= from && anchor.start + anchor.length <= to)
				kept.anchors.push_back(protected_api::papinian::FactAnchor{anchor.start - removed, anchor.length, anchor.key});
	};
	for (const auto &[start, end] : pending_approval_sentences(answer.text)) {
		keep(cursor, start);
		removed += end - start;
		cursor = end;
	}
	keep(cursor, answer.text.size());
	while (!kept.text.empty() && std::isspace(static_cast<unsigned char>(kept.text.back())) != 0) kept.text.pop_back();
	kept.anchors.erase(std::remove_if(kept.anchors.begin(), kept.anchors.end(),
		[&](const protected_api::papinian::FactAnchor &anchor) { return anchor.start + anchor.length > kept.text.size(); }),
		kept.anchors.end());
	return kept;
}

// The one sentence the lawyer reads first when a change is not written
// because a figure it needs still cannot be worked out: that nothing was
// written, and why, in plain words.
std::string change_figures_note(const std::vector<context::computed_statements::StatementFailure> &unworked,
		bool russian) {
	std::string reasons;
	std::set<std::string> said;
	for (const context::computed_statements::StatementFailure &failure : unworked) {
		const std::string reason = plain_reason(failure.reason, russian);
		if (said.insert(reason).second) reasons += (reasons.empty() ? "" : "; ") + reason;
	}
	if (russian)
		return std::string(unworked.size() == 1
				? "\xD0\x9D\xD0\xB8\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBE: \xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB8\xD0\xBD\xD1\x83, \xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD1\x83\xD1\x8E \xD0\xB4\xD0\xBB\xD1\x8F \xD1\x8D\xD1\x82\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F, \xD0\xBD\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB2\xD1\x8B\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C"
				: "\xD0\x9D\xD0\xB8\xD1\x87\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBE: \xD0\xB2\xD0\xB5\xD0\xBB\xD0\xB8\xD1\x87\xD0\xB8\xD0\xBD\xD1\x8B, \xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD1\x8B\xD0\xB5 \xD0\xB4\xD0\xBB\xD1\x8F \xD1\x8D\xD1\x82\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F, \xD0\xBD\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB2\xD1\x8B\xD1\x87\xD0\xB8\xD1\x81\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C")
			+ (reasons.empty() ? std::string(".") : " - " + reasons + ".");
	return std::string(unworked.size() == 1
			? "Nothing was written: a figure this change needs could not be worked out"
			: "Nothing was written: figures this change needs could not be worked out")
		+ (reasons.empty() ? std::string(".") : " - " + reasons + ".");
}

// How many more questions this operation may still put to the lawyer.
std::size_t remaining_lawyer_questions(const matters::MatterTask &task) {
	return task.lawyer_questions.size() >= kMaximumLawyerQuestions
		? 0 : kMaximumLawyerQuestions - task.lawyer_questions.size();
}

// Every answer the lawyer has given this operation, as the model reads it - for
// a later call of the same operation (a review's redraft, AdvancedWorkflow),
// which is assembled from the request again and would otherwise lose them.
std::string answered_lawyer_questions_text(const matters::MatterTask &task) {
	std::string text;
	for (const matters::LawyerQuestionRecord &record : task.lawyer_questions) {
		if (record.state != "answered" && record.state != "left_to_model") continue;
		text += tools::lawyer_question::answer_block(nlohmann::json{{"questions", record.questions}}, record.answers,
			remaining_lawyer_questions(task));
	}
	return text;
}

// The facts Singularity worked out for this task so far, for a later call of it -
// its professional review, a redraft, a targeted completion - which is
// assembled from the request again and would otherwise read a derived symbol
// ("over the cap by [[E7]]") with nothing to say what it was worked out from:
// a reviewer could not check it, and a redraft would ask again or guess.
// `answers` are the rounds' "<n>. <question> → <answer>" lines, numbered
// again here as one list; their values stay inside ⟦ ⟧, which protection
// sends as the task's own symbols. Empty when none was worked out.
std::string local_facts_for_later_call(const std::string &answers) {
	std::string numbered;
	std::size_t count = 0;
	std::size_t from = 0;
	while (from < answers.size()) {
		std::size_t end = answers.find('\n', from);
		if (end == std::string::npos) end = answers.size();
		std::string line = answers.substr(from, end - from);
		from = end + 1;
		std::size_t digits = 0;
		while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
		if (digits > 0 && line.compare(digits, 2, ". ") == 0) line.erase(0, digits + 2);
		if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
		numbered += std::to_string(++count) + ". " + line + "\n";
	}
	if (numbered.empty()) return numbered;
	return "\n\nLOCAL FACTS already worked out for this task on the lawyer's computer from the protected values (a "
		"symbol given as an answer here stands for the value worked out; the values themselves stay on the lawyer's "
		"computer):\n" + numbered;
}

// The lawyer's answers as the professional review reads them: part of the
// objective the draft follows - what the lawyer decided is the instruction,
// never a claim the draft must support from the documents. Without them the
// review judges the draft against the bare request and flags what the lawyer
// chose as unsupported.
std::string lawyer_answers_for_review(const matters::MatterTask &task) {
	std::string text;
	for (const matters::LawyerQuestionRecord &record : task.lawyer_questions) {
		if (record.state != "answered" && record.state != "left_to_model") continue;
		for (const tools::lawyer_question::AnsweredQuestion &entry : tools::lawyer_question::answered_questions(
				nlohmann::json{{"questions", record.questions}}, record.answers)) {
			text += "\n- " + entry.prompt + " ";
			std::string said;
			for (const std::string &label : entry.chosen) said += (said.empty() ? "" : "; ") + label;
			if (!entry.text.empty()) said += (said.empty() ? "" : "; ") + entry.text;
			// A question skipped among others answered is left to Singularity just
			// as "Decide these yourself" leaves them all.
			text += record.state == "left_to_model" || said.empty() ? "The lawyer left this to Singularity to decide."
				: "The lawyer answered: " + said;
		}
	}
	return text.empty() ? text
		: "\n\nWhat the lawyer answered during this task, part of the objective itself:" + text;
}

// AdvancedWorkflow compiles every turn from the request again, so the lawyer's
// answers travel inside the request itself, after its own words: data the model
// reads, never an instruction of Singularity's.
nlohmann::json operation_request_data_with_answers(nlohmann::json data, const matters::MatterTask &task) {
	const std::string answered = answered_lawyer_questions_text(task);
	if (answered.empty() || !data.contains("currentRequest") || !data.at("currentRequest").is_string()) return data;
	data["currentRequest"] = data.at("currentRequest").get<std::string>() + answered;
	return data;
}

// What the model is told it can ask for. The count is of the documents this
// request may know of - with a choice of documents, those only.
std::string project_inspection_protocol(const context::project_evidence::Session &session, bool scoped) {
	const std::size_t documents = session.inventory_size();
	const std::size_t unreadable = documents - session.readable_count();
	std::string text = "Singularity can list, search and read ";
	text += scoped ? "the " + std::to_string(documents) + (documents == 1 ? " document" : " documents")
			+ " chosen for this request"
		: "this Project - its " + std::to_string(documents) + (documents == 1 ? " document" : " documents");
	if (unreadable > 0) text += " (" + std::to_string(unreadable) + " unreadable)";
	text += " - for you during this task. If the request depends on a fact, figure, date, approval, clause or "
		"document the supplied material lacks, set projectEvidenceRequest instead of guessing, and never write that it "
		"is missing or was not supplied while a document you have not read could hold it: "
		"search = words to find; read = up to " + std::to_string(context::project_evidence::kMaximumDocumentsPerRead)
		+ " documents, read from their beginning, by handle (F1, F2 ...) from a listing or search, or by file name; "
		"list = a folder (\"\" for all). Set several at once as needed. While asking, response is one sentence on "
		"what you seek and actionType \"none\"; the results arrive in this task as SOURCE passages you cite like the "
		"others, and you then complete the request. At most " + std::to_string(kProjectEvidenceRequestsPerAnswer)
		+ " requests. When the supplied material is enough, projectEvidenceRequest is null. Order documents by the "
		"dates they state: a draft dated before a signed version is the earlier draft it replaced, and each reflects "
		"only what was decided before it.";
	return text;
}

// One inspection step as the lawyer reads it in the operation's record.
std::string project_inspection_message(const nlohmann::json &step) {
	const std::string kind = step.value("kind", std::string());
	const std::string subject = step.value("subject", std::string());
	const std::string outcome = step.value("outcome", std::string());
	const std::size_t passages = step.value("passages", std::size_t{0});
	const std::size_t leftOut = step.value("passagesLeftOut", std::size_t{0});
	std::string documents;
	if (step.contains("documents") && step.at("documents").is_array())
		for (const nlohmann::json &name : step.at("documents"))
			if (name.is_string()) documents += (documents.empty() ? "" : ", ") + name.get<std::string>();
	const std::string separator = " \xC2\xB7 ";
	const auto counted = [](std::size_t count) {
		return std::to_string(count) + (count == 1 ? " passage" : " passages");
	};
	std::string message;
	if (kind == "search") {
		message = "Searched the Project for \xE2\x80\x9C" + subject + "\xE2\x80\x9D";
		if (passages > 0) message += separator + counted(passages) + " from " + documents;
		else if (outcome == "already_given") message += separator + "nothing new";
		else if (outcome == "no_room") message += separator + "what it found did not fit";
		else message += separator + "nothing found";
	} else if (kind == "read") {
		if (passages > 0) message = "Read " + documents + separator + counted(passages);
		else message = "Did not read " + (subject.empty() ? std::string("the document asked for") : subject) + separator
			+ (outcome == "already_given" ? "already read" : outcome == "not_readable" ? "it cannot be read"
				: outcome == "no_room" ? "it did not fit" : "not a document of this Project");
	} else {
		message = subject.empty() ? std::string("Listed the Project's documents")
			: "Listed the documents in \xE2\x80\x9C" + subject + "\xE2\x80\x9D";
	}
	if (passages > 0 && leftOut > 0) message += separator + std::to_string(leftOut) + " more did not fit";
	return message;
}

// What the request recorded as carried, extended by the passages inspection
// gave: the evidence a lawyer sees counted is the evidence that reached the model.
void cover_project_evidence(matters::MatterTask &task, const std::vector<context::RetrievedContextChunk> &passages) {
	for (const context::RetrievedContextChunk &passage : passages) {
		auto covered = std::find_if(task.request_coverage.sources.begin(), task.request_coverage.sources.end(),
			[&](const matters::SourceCoverage &entry) { return entry.document_id == passage.document_id; });
		if (covered == task.request_coverage.sources.end()) {
			task.request_coverage.sources.push_back(matters::SourceCoverage{passage.document_id, 0, 0, false});
			covered = std::prev(task.request_coverage.sources.end());
		}
		++covered->supplied;
		++covered->retrieved;
	}
}

// After the call that could inspect the Project: what it read joins the one
// evidence basis this operation's markers are numbered from, its passages are
// supplied (so a citation of one validates and a quotation of one is checked
// against it), a document it found that the request may change can be named
// as the target, and the rest of the operation is assembled without the
// protocol, since only that call could ask.
void absorb_project_evidence(PreparedTaskRequest &prepared, matters::MatterTask &task, const matters::Matter &matter) {
	if (!prepared.project_evidence) return;
	if (prepared.capability_without_inspection)
		prepared.prompt_input.capability_instructions = prepared.capability_without_inspection->empty()
			? std::nullopt : prepared.capability_without_inspection;
	const std::vector<context::RetrievedContextChunk> given = prepared.project_evidence->given();
	if (given.empty()) return;
	prepared.project_evidence_passages = given;
	prepared.document_context = prepared.project_evidence->basis();
	for (const context::RetrievedContextChunk &passage : given) {
		prepared.supplied_chunk_ids.insert(passage.chunk.chunk_id);
		prepared.distinct_context_document_ids.insert(passage.document_id);
	}
	const context::SourceMarkerRegistry markers(prepared.document_context);
	prepared.supplied_markers = markers.restricted_to(prepared.supplied_chunk_ids);
	for (const std::string &documentId : prepared.project_evidence->newly_changeable_document_ids()) {
		const auto passage = std::find_if(given.begin(), given.end(),
			[&](const context::RetrievedContextChunk &entry) { return entry.document_id == documentId; });
		const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &entry) { return entry.id == documentId; });
		if (passage == given.end() || document == matter.documents.end()) continue;
		prepared.professional_artifact_candidates.nameable_targets.push_back(
			{markers.document_token_for(passage->chunk.chunk_id), documentId, document->file_type});
	}
	cover_project_evidence(task, given);
}

// A passage's words, whatever the bytes of the file around them: runs of white
// space read as one space, and the ends are trimmed. Two readings of the same
// passage - a document saved again unchanged, a file re-zipped by Word - hash
// alike; one changed word does not.
std::string supplied_passage_text_hash(const std::string &text) {
	std::string normalized;
	normalized.reserve(text.size());
	bool isSpace = false;
	for (const char character : text) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f'
				|| character == '\v') {
			isSpace = true;
			continue;
		}
		if (isSpace && !normalized.empty()) normalized += ' ';
		isSpace = false;
		normalized += character;
	}
	return platform::sha256_hex_digest(normalized);
}

// The exact Project evidence the generation that produced this proposal was
// given - every supplied passage, the request's own and those inspection read,
// by the chunk identity its marker was issued for, in the order the markers
// were numbered. A chunk's identity carries its document's fingerprint, so it
// names that passage as it read then and nothing else. The proposal rests on
// this; a resume re-reads it before anything is written.
void record_supplied_evidence(nlohmann::json &draft, const PreparedTaskRequest &prepared) {
	std::unordered_set<std::string> inspected;
	for (const context::RetrievedContextChunk &passage : prepared.project_evidence_passages)
		inspected.insert(passage.chunk.chunk_id);
	nlohmann::json passages = nlohmann::json::array();
	std::unordered_set<std::string> recorded;
	for (const context::RetrievedContextChunk &passage : prepared.document_context) {
		if (prepared.supplied_chunk_ids.count(passage.chunk.chunk_id) == 0) continue;
		if (!recorded.insert(passage.chunk.chunk_id).second) continue;
		passages.push_back({{"documentId", passage.document_id}, {"chunkId", passage.chunk.chunk_id},
			{"textHash", supplied_passage_text_hash(passage.chunk.text)},
			{"inspected", inspected.count(passage.chunk.chunk_id) != 0}});
	}
	if (!passages.empty()) draft["suppliedEvidence"] = std::move(passages);
	// The documents the request's pick chose (none, when it chose none), so a
	// resume reads the same ones without asking the model to choose again.
	draft["pickedDocumentIds"] = prepared.picked_document_ids;
}

// A resumed operation re-reads, against each document's current bytes, every
// passage the generation that produced its proposal was given (the snapshot
// record_supplied_evidence() kept). All still there, exactly: they are the
// operation's evidence basis again, in their recorded order - the request's own
// and the ones inspection read - so quotations are checked against what the
// model was shown. Any one gone - its document changed, moved away or became
// unreadable - and the proposal no longer rests on the evidence it was made
// from: the operation stops with nothing written, and the request is sent again
// to be prepared from the documents as they are. The approved target's own
// passages are the target check's (require_approved_target_unchanged()), which
// says so in its own words. Evidence can only be stale BEFORE the operation's
// own write begins: once its write-ahead record exists (`writeBegun`), the
// document may already carry this very change, and recovering that write is the
// write-ahead record's work - the passages still readable are restored, and
// nothing is refused for having changed. Restoring twice restores once.
void restore_supplied_evidence(PreparedTaskRequest &prepared, const nlohmann::json &draft,
		const std::filesystem::path &workspacePath, const matters::Matter &matter, bool writeBegun) {
	if (!draft.contains("suppliedEvidence") || !draft.at("suppliedEvidence").is_array()) return;
	if (!prepared.project_evidence_passages.empty()) return;
	std::optional<std::string> targetId;
	if (draft.contains("approval") && draft.at("approval").is_object()
			&& draft.at("approval").contains("targetDocumentId") && draft.at("approval").at("targetDocumentId").is_string())
		targetId = draft.at("approval").at("targetDocumentId").get<std::string>();
	const auto changed = [&](const std::string &documentId) {
		const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &entry) { return entry.id == documentId; });
		const std::string name = document == matter.documents.end() ? std::string("A document")
			: (document->relative_path.empty() ? document->name : document->relative_path);
		return ProposalEvidenceChanged(name + ", which this change was prepared from, has changed since the change "
			"was proposed, so nothing was written. Send the request again to prepare the change from the current "
			"documents.");
	};
	std::unordered_map<std::string, std::optional<std::vector<context::RetrievedContextChunk>>> read;
	std::unordered_set<std::string> known;
	for (const context::RetrievedContextChunk &passage : prepared.document_context) known.insert(passage.chunk.chunk_id);
	std::vector<context::RetrievedContextChunk> inspected;
	bool restoredAny = false;
	for (const nlohmann::json &entry : draft.at("suppliedEvidence")) {
		const std::string documentId = entry.value("documentId", std::string());
		const std::string chunkId = entry.value("chunkId", std::string());
		if (documentId.empty() || chunkId.empty()) continue;
		const bool isTarget = targetId && *targetId == documentId;
		if (read.count(documentId) == 0) {
			try {
				read[documentId] = matters::retrieve_document_context(workspacePath, matter, std::string(), {documentId},
					nullptr, matters::SelectedSourceDelivery::EveryPassage);
			} catch (const std::exception &) {
				read[documentId] = std::nullopt;
			}
		}
		const auto &passages = read.at(documentId);
		auto found = passages ? std::find_if(passages->begin(), passages->end(),
			[&](const context::RetrievedContextChunk &passage) { return passage.chunk.chunk_id == chunkId; })
			: std::vector<context::RetrievedContextChunk>::const_iterator();
		// A passage's identity carries its document's fingerprint, so any save of
		// the file - even one that changed nothing - gives it a new one. The
		// proposal rests on the passage's words: while they are still in the
		// document, word for word, the evidence has not changed (D19).
		const std::string textHash = entry.value("textHash", std::string());
		if (passages && found == passages->end() && !textHash.empty())
			found = std::find_if(passages->begin(), passages->end(), [&](const context::RetrievedContextChunk &passage) {
				return supplied_passage_text_hash(passage.chunk.text) == textHash;
			});
		if (!passages || found == passages->end()) {
			if (isTarget || writeBegun) continue;
			throw changed(documentId);
		}
		context::RetrievedContextChunk passage = *found;
		passage.explicitly_selected = false;
		const std::string currentChunkId = passage.chunk.chunk_id;
		if (known.insert(currentChunkId).second) prepared.document_context.push_back(passage);
		prepared.supplied_chunk_ids.insert(currentChunkId);
		prepared.distinct_context_document_ids.insert(documentId);
		restoredAny = true;
		if (entry.value("inspected", false)) inspected.push_back(std::move(passage));
	}
	if (!restoredAny) return;
	prepared.project_evidence_passages = std::move(inspected);
	prepared.supplied_markers = context::SourceMarkerRegistry(prepared.document_context)
		.restricted_to(prepared.supplied_chunk_ids);
}

// Controlled-experiment-only seam, never a product policy mechanism: lets a
// deliberate acceptance run force the compact/progressive-inspection
// context-delivery path even for a model whose real capacity would
// otherwise fit the full Project structure, so the SAME model/document/
// prompt can be compared under both delivery strategies without changing
// any targeting logic. It is process-local, read only when explicitly set,
// never persisted and never referenced by normal product operation. Absent
// in every real user's packaged app and every ordinary CLI run.
std::optional<std::size_t> forced_context_window_tokens_for_experiment() {
#ifdef NDEBUG
	// RUN-6: compiled out of a release build entirely. What a lawyer's request
	// is allowed to deliver is decided by the executing model's real capacity;
	// an environment variable must not be able to change that in the product
	// they actually run, even by accident. The seam exists for a deliberate
	// side-by-side acceptance run, and only a build made for that can honour it.
	return std::nullopt;
#else
	const char *value = std::getenv("SINGULARITY_FORCE_COMPACT_CONTEXT_TOKENS");
	if (value == nullptr || *value == '\0') return std::nullopt;
	try {
		return static_cast<std::size_t>(std::stoul(value));
	} catch (const std::exception &) {
		return std::nullopt;
	}
#endif
}

// The provider attempt that produced a proposal pending approval ran before
// the pause: its identity, usage, privacy record, tool evidence and research
// evidence are facts of the operation, persisted with the task and its
// checkpoint, and restored for the resumed attempt instead of being replaced
// by the empty result of a resume that makes no provider call.
void restore_generated_attempt(routing::InferenceResult &result, const matters::MatterTask &task) {
	result.provider_id = task.provider_id.value_or(std::string());
	result.model_id = task.model_id.value_or(std::string());
	result.execution_kind = task.execution_kind;
	result.runtime_id = task.runtime_id;
	result.runtime_version = task.runtime_version;
	result.runtime_source_commit = task.runtime_source_commit;
	result.runtime_platform = task.runtime_platform;
	result.runtime_backend = task.runtime_backend;
	result.runtime_model_id = task.runtime_model_id;
	result.protected_api_applied = task.protected_api_applied;
	result.privacy_metadata = task.privacy_metadata;
	result.usage = task.usage;
	result.tool_call_evidence = task.tool_call_evidence;
	if (task.document_operation_checkpoint) result.external_evidence = task.document_operation_checkpoint->external_evidence;
}

// The professional review's own output ceiling - also the size bound of the
// review issues a correction continuation adds on top of the first prompt.
constexpr std::size_t kProfessionalReviewMaxOutputTokens = 2048;
// What the review call itself may produce where its window has the room: a
// reasoning model thinks before it writes its verdict, and 2048 tokens can go
// on the thinking alone (a real GPT-5 review, 24 Sep 2026, returned nothing and
// the draft went to the lawyer unreviewed). Its issues stay bounded by the
// ceiling above wherever they return into a prompt.
constexpr std::size_t kProfessionalReviewCallMaxOutputTokens = 8192;
// The professional response's own declared output ceiling; what a resolved
// window actually leaves for it is Singularity's shared policy, never restated here
// (research::reserved_output_tokens_for_window).
constexpr std::size_t kProfessionalResponseMaxOutputTokens = 8192;

// The smallest output a professional request is still sent with when its
// prompt needs part of the output reservation (bounded policy: room for a
// complete short professional_response or review envelope). Below it the
// request is refused as exceeding the model's context capacity.
constexpr std::size_t kMinimumProfessionalOutputTokens = 1024;

// Reorders explicitly selected passages into each document's reading order
// (documents in selection order) and returns every passage's admission
// priority: its rank in `passages` as retrieval returned them. Relevant-mode
// passages keep retrieval order.
std::unordered_map<std::string, std::size_t> ordered_for_capacity_first_delivery(
		std::vector<context::RetrievedContextChunk> &passages, const std::vector<std::string> &selectedDocumentIds) {
	std::unordered_map<std::string, std::size_t> priority;
	for (std::size_t rank = 0; rank < passages.size(); ++rank)
		priority.emplace(passages[rank].chunk.chunk_id, rank);
	if (selectedDocumentIds.empty()) {
		// Retrieval ranks passages; the prompt reads them document by document -
		// the best-ranked document first, each in its own order - so a clause of
		// one draft never alternates with the same clause of another.
		std::unordered_map<std::string, std::size_t> documentRank;
		for (const context::RetrievedContextChunk &passage : passages)
			documentRank.emplace(passage.document_id, documentRank.size());
		std::stable_sort(passages.begin(), passages.end(),
			[&](const context::RetrievedContextChunk &left, const context::RetrievedContextChunk &right) {
				const std::size_t leftRank = documentRank.at(left.document_id);
				const std::size_t rightRank = documentRank.at(right.document_id);
				if (leftRank != rightRank) return leftRank < rightRank;
				return left.chunk.source_start < right.chunk.source_start;
			});
		return priority;
	}
	const auto selectionPosition = [&](const std::string &documentId) {
		return static_cast<std::size_t>(std::find(selectedDocumentIds.begin(), selectedDocumentIds.end(), documentId)
			- selectedDocumentIds.begin());
	};
	std::stable_sort(passages.begin(), passages.end(),
		[&](const context::RetrievedContextChunk &left, const context::RetrievedContextChunk &right) {
			const std::size_t leftPosition = selectionPosition(left.document_id);
			const std::size_t rightPosition = selectionPosition(right.document_id);
			if (leftPosition != rightPosition) return leftPosition < rightPosition;
			return left.chunk.source_start < right.chunk.source_start;
		});
	return priority;
}

// Capacity-first delivery budget. With the executing model's context
// capacity resolved (local inference's granted/planned figure for a local model, the
// provider-reported window for a remote one), the whole prompt is bounded in
// estimated tokens by capacity - reserved output - safety margin, measured
// with the same estimator the execution gate applies; optional material
// (sources, history, context items) additionally leaves continuation
// headroom for what a revising continuation adds on top of this prompt: the
// prior proposal or invalid output (bounded by the same output reservation)
// and the review's issues. Research evidence added by a continuation is
// compacted into whatever room remains (fit_tool_result_to_continuation).
// Text is measured in its serialized provider-facing form, as that gate
// measures it, and the conversational envelope the prompt travels in counts
// against the same ceiling (routing::execute()'s first-turn admission). With
// capacity unknown, the historical window (byte ceiling and relevance passage
// window) stays the bound.
void apply_capacity_first_budget(context::PromptAssemblyInput &input,
		std::optional<std::size_t> contextWindowTokens, std::size_t reservedOutputTokens) {
	input.fallback_maximum_passages = context::kRelevancePassageWindow;
	const auto ceiling = research::admissible_research_input_tokens_for_window(contextWindowTokens, reservedOutputTokens);
	if (!ceiling) return;
	const std::size_t envelopeTokens =
		research::estimate_tokens(routing::conversational_model_turn(std::string()).serialize());
	input.maximum_context_tokens = *ceiling > envelopeTokens ? *ceiling - envelopeTokens : 0;
	input.estimate_tokens = [](std::string_view text) { return research::estimate_serialized_tokens(text); };
	input.continuation_headroom_tokens = reservedOutputTokens + kProfessionalReviewMaxOutputTokens;
	input.reserved_output_tokens = reservedOutputTokens;
	input.minimum_output_tokens = std::min(reservedOutputTokens, kMinimumProfessionalOutputTokens);
}

PreparedTaskRequest prepare_task_request(const std::filesystem::path &workspace_path,
                                         const matters::Matter &matter,
                                         const TaskAttemptSpec &spec);

// Retry and edit reproduce the original attempt's protection mode but never
// protect less than the live policy now requires: a historical Off (or a
// narrower Custom) never silently sends raw text once protection is stronger.
std::optional<protected_api::Mode> retry_protection_mode(const matters::MatterTask &original) {
	const auto strength = [](protected_api::Mode mode) {
		return mode == protected_api::Mode::Standard ? 2 : mode == protected_api::Mode::Custom ? 1 : 0;
	};
	const protected_api::Mode historical = protected_api::mode_from_string(original.protected_api_mode);
	const protected_api::Mode live = platform::load_app_settings().local_node_setup.protected_api_policy.mode;
	if (strength(live) >= strength(historical)) return std::nullopt;
	return historical;
}

// A request the executing model's known capacity cannot hold is refused
// before anything is sent, under its own stable, non-retryable error code -
// except when it resumes a checkpointed operation, whose resume may commit
// from its checkpoint with zero model calls: that request is assembled as if
// capacity were unknown (TaskAttemptSpec::assemble_without_known_capacity).
PreparedTaskRequest prepare_task_request_within_capacity(const std::filesystem::path &workspace_path,
		const matters::Matter &matter, const TaskAttemptSpec &spec) {
	try {
		return prepare_task_request(workspace_path, matter, spec);
	} catch (const context::ContextCapacityExceeded &error) {
		const bool resumesCheckpoint = !spec.operation_id.empty()
			&& std::any_of(matter.tasks.begin(), matter.tasks.end(), [&](const matters::MatterTask &entry) {
				return entry.agent_operation_id == spec.operation_id && entry.document_operation_checkpoint.has_value()
					&& entry.operation_execution_state != "completed" && entry.operation_execution_state != "failed"
					&& entry.operation_execution_state != "stopped";
			});
		if (!resumesCheckpoint || spec.assemble_without_known_capacity)
			throw providers::TaskError(providers::TaskErrorCode::ContextCapacityExceeded, error.what());
		TaskAttemptSpec resumeSpec = spec;
		resumeSpec.assemble_without_known_capacity = true;
		return prepare_task_request(workspace_path, matter, resumeSpec);
	}
}

PreparedTaskRequest prepare_task_request(const std::filesystem::path &workspace_path,
                                         const matters::Matter &matter,
                                         const TaskAttemptSpec &spec) {
  namespace obs = routing::observability;
  platform::trace::Span prepareSpan("task", "prepare_task_request", {}, true);
  obs::emit({obs::OperationEventKind::ProjectContextBuildStart, "Assembling Project context", {},
      obs::has_sink() ? std::optional<std::string>(spec.prompt) : std::nullopt});
  context::PromptAssemblyInput prompt_input;
  prompt_input.current_request = spec.prompt;
  const auto settings = platform::load_app_settings();
  const auto effective_policy = protected_api::apply_mode_override(
      settings.local_node_setup.protected_api_policy, spec.protection_mode_override);
	// Capability-first context delivery (CORE PRODUCT LAW): which model will
	// actually run this request is already known here (spec.model_target, or
	// the current default reasoning assignment when unset) - resolving its
	// real capacity BEFORE building the document-action plan lets Singularity
	// decide once, up front, whether every structural row can be exposed
	// directly (today's unchanged behavior) or whether the model must
	// inspect collapsed tables progressively (see ConnectedDocumentActionPlan::
	// compact_context_mode). See estimate_model_context_window_tokens()'s own
	// comment for why this narrow read-only query exists separately from
	// execute()'s full resolution.
	const professional::ProfessionalExecutionIntent &intent = spec.professional_intent;
	const std::optional<std::size_t> executingContextWindowTokens = spec.assemble_without_known_capacity
			|| intent.capability == professional::ProfessionalCapability::ProductState
		? std::nullopt : routing::estimate_model_context_window_tokens(spec.model_target);
	// The experiment seam forces only the compaction decision; the executing
	// model's own capacity still bounds source delivery and the output
	// reservation (known capacity is authoritative).
	const auto forcedContextWindowTokens = forced_context_window_tokens_for_experiment();
	const auto availableContextWindowTokens = forcedContextWindowTokens
		? forcedContextWindowTokens
		: routing::estimate_model_context_window_tokens(spec.model_target);
	// A resolved context window bounds the output reservation too: a flat
	// 8192-token ceiling can be larger than a small window's whole capacity (a
	// local model's window is local inference's own granted/planned figure, never a
	// constant), which would leave ResearchToolLoop.hpp's capacity-fit checks
	// (e.g. its one bounded final-format-correction turn) no room for the
	// prompt itself. Capacity-first context delivery (this file's own CORE
	// PRODUCT LAW, see apply_capacity_first_budget()) reserves from the SAME
	// resolved executingContextWindowTokens, never a provider-agnostic guess.
	// The declared ceiling follows the window (ContextBudget.hpp) and never asks
	// a model for more than it documents it can produce.
	std::size_t declaredResponseCeiling = std::max(kProfessionalResponseMaxOutputTokens,
		research::professional_response_output_ceiling(executingContextWindowTokens));
	if (spec.model_target) {
		if (const auto documented = research::known_model_maximum_completion_tokens(
				spec.model_target->provider_id, spec.model_target->model_id))
			declaredResponseCeiling = std::min(declaredResponseCeiling,
				std::max(*documented, kProfessionalResponseMaxOutputTokens));
	}
	const auto professionalResponseMaxOutputTokens = research::reserved_output_tokens_for_window(
		executingContextWindowTokens, declaredResponseCeiling);
	// A compatible, genuinely empty FILL_TEMPLATE surface always wins (Beta
	// product decision - see DirectDocumentWork.hpp's own resolve_edit_
	// surface() comment): only once no such surface exists does the old
	// multi-turn document-action tool-choreography get a chance to run for
	// this attempt. Both read the same real Project documents; neither
	// invents one, and they are mutually exclusive for a single attempt.
	// Follow-up continuity's own real dependency-closed context (never the
	// unrestricted whole Project history): only THIS exact Chat's own prior
	// tasks, in their existing canonical order, exactly like the generic
	// relevant_history() boundary below already scopes conversational
	// history per-Chat. A chatless attempt (no Chat boundary to resolve
	// continuity within) simply never finds a prior object.
	std::vector<matters::MatterTask> directDocumentWorkChatTasks;
	if (spec.realized_record_document_id) {
		// SLR-3: this attempt is a rerun of a turn that already wrote a
		// structured record. That record IS this turn's work, so it is the only
		// continuity a rerun has: the rest of the Chat's history is not this
		// turn's predecessor, and a free template elsewhere in the Project is
		// not a second place to put the same piece of work.
		matters::MatterTask realized;
		realized.status = matters::TaskStatus::Success;
		realized.chat_id = spec.chat_id;
		realized.direct_document_work_document_id = spec.realized_record_document_id;
		realized.direct_document_work_table_id = spec.realized_record_table_id;
		realized.direct_document_work_banner_row_index = spec.realized_record_banner_row_index;
		directDocumentWorkChatTasks.push_back(std::move(realized));
	} else if (spec.chat_id) {
		for (const auto &priorTask : matter.tasks)
			if (priorTask.chat_id && *priorTask.chat_id == *spec.chat_id)
				directDocumentWorkChatTasks.push_back(priorTask);
	}
	// STRICT PRODUCT LAW (see ProfessionalExecutionIntent.hpp's own header
	// comment): WHICH kind of professional work this instruction calls for -
	// summarize, compare, research, create an artifact, update one, fill or
	// update a structured record, or just answer - is never decided here by
	// matching instruction text against a hand-written phrase dictionary.
	// Both structural candidates below are resolved UNCONDITIONALLY (never
	// gated on a guess about what the instruction means) from real Project
	// state alone: DirectDocumentWork's own resolver already only ever offers
	// a real candidate block/continuity target (structural facts, never
	// semantic ones - see its own header comment), and
	// resolve_professional_artifact_candidates() resolves ONLY the request's
	// separate explicit target identity as an update target and Project-folder
	// existence as create eligibility (never a selected source and never "the
	// only writable file happens to exist"). Both are folded into
	// ONE unified request/schema (professional_response_output_contract())
	// below; the MODEL's own returned actionType - chosen from exactly what
	// these two candidates make available - is what Singularity then validates
	// and executes. No second, mandatory router model call is ever added:
	// this is the SAME one substantive generation every Generative request
	// already pays for.
	auto directDocumentWork = intent.capability == professional::ProfessionalCapability::Generative
		? documents::resolve_direct_document_work_task_plan(spec.prompt, matter, spec.chat_id,
			directDocumentWorkChatTasks, spec.selected_document_ids, spec.target_document_id)
		: std::nullopt;
	// A rerun has exactly one place its work belongs: the record it already
	// wrote. Offering it a free template as well is what let Retry leave a
	// second copy of the same record in the Project (SLR-3).
	if (directDocumentWork && spec.realized_record_document_id) {
		directDocumentWork->new_work_document_id.reset();
		directDocumentWork->new_work_target.reset();
	}
	documents::ProfessionalArtifactCandidates artifactCandidates =
		documents::resolve_professional_artifact_candidates(
			spec.selected_document_ids, spec.target_document_id, matter,
			// How much of the bound document the resolved model can actually be
			// shown. Its own capacity decides; nothing here is a flat number.
			availableContextWindowTokens
				? std::optional<std::size_t>(static_cast<std::size_t>(*availableContextWindowTokens))
				: std::nullopt);
	// AdvancedWorkflow's own structural candidate (ConnectedDocumentAction's
	// existing, unchanged resolver - methodology/evidence-agenda/multi-table
	// candidates, never a keyword guess at whether this instruction wants
	// them). Resolved unconditionally alongside DirectDocumentWork's and the
	// generic artifact's own candidates - see this file's own comment above
	// on the unified professional_response_output_contract(): the model's
	// OWN actionType declaration (never this resolution itself) decides
	// whether AdvancedWorkflow is actually used. ConnectedDocumentAction is
	// never wired into `request` here any more (that used to make it a
	// second, pre-generation-selected Chat engine); its rich choreography is
	// only ever activated inside execute_task_attempt(), after the model has
	// already declared actionType "advanced_workflow" against this exact
	// candidate.
	const std::optional<documents::ConnectedDocumentActionPlan> connectedDocumentAction =
		intent.capability == professional::ProfessionalCapability::Generative
			? documents::prepare_connected_document_action(spec.prompt, matter, availableContextWindowTokens,
				spec.selected_document_ids, spec.target_document_id)
			: std::nullopt;
  prompt_input.matter_instructions = spec.matter_instructions;
  prompt_input.user_default_instructions =
      settings.user_default_legal_instructions;
  prompt_input.legal_task_preset_id = spec.legal_task_preset_id;
  prompt_input.legal_task_preset_version = spec.legal_task_preset_version;
  prompt_input.prompt_profile_id = spec.prompt_profile_id;
  prompt_input.prompt_profile_version = spec.prompt_profile_version;

	for (const context::ContextItem &item : context::list_items_for_ai_assembly(matter)) {
		// One JSON record per item: its kind, authority and source are
		// machine-owned and its label and value are escaped, so text inside a
		// value can never forge another record - least of all one declared as an
		// instruction - and an automatically derived value never reads as a fact
		// a person confirmed. An instruction carries its words only: its label is
		// the words it was created with and goes stale when the words are edited.
		// Locking a value is a person keeping it, so a locked value reads as confirmed.
		const char *authority = item.authority == context::ContextItemAuthority::UserAuthored ? "user_authored"
			: item.authority == context::ContextItemAuthority::AutomaticallyDerived && !item.is_locked
				? "automatically_derived" : "user_confirmed";
		nlohmann::ordered_json record{{"kind", context::to_string(item.kind)}, {"authority", authority},
			{"source", context::to_string(item.source ? item.source->kind : context::ContextSourceKind::User)}};
		if (item.source && item.source->kind == context::ContextSourceKind::Document)
			record["sourceStatus"] = context::to_string(context::context_source_status(matter, *item.source));
		if (item.kind != context::ContextItemKind::Instruction) record["label"] = item.label;
		record["value"] = item.value;
		std::string content = record.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
		prompt_input.project_context_items.push_back({item.id, std::move(content), item.is_pinned});
	}
	// The data room's journal - what Singularity noticed happening to the Project's
	// documents - goes with what the Project remembers, as records of their own
	// kind, after everything a person or the memory said: when room is short they
	// are the first to stay behind. What is new in the data room is then a
	// question like any other, answered from what the request was given.
	// A model with a small window keeps its room for the conversation itself and
	// is given no journal (the same bound its memory guidance has, below).
	if (!availableContextWindowTokens || *availableContextWindowTokens >= kJournalMinimumWindowTokens) {
		std::size_t journalIndex = 0;
		for (std::string &line : documents::journal::records_for_request(matter.document_events)) {
			prompt_input.project_context_items.push_back(
				{"data_room_change_" + std::to_string(journalIndex++), std::move(line), false});
		}
	}
	std::size_t projectContextBytes = 0;
	for (const auto &item : prompt_input.project_context_items) projectContextBytes += item.content.size();
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
	routing::memory_diagnostics::project_context_bytes = projectContextBytes;
#endif

  std::vector<matters::UnavailableSourceDocument> unavailable_source_documents;
	// Every Generative request retrieves through the SAME existing relevance-
	// scored retrieval path "Relevant" source policy already uses (never a
	// text-keyword gate): a bare greeting naturally retrieves nothing relevant
	// on its own merits, so no separate "is this a greeting" classification is
	// needed to keep it cheap - the retrieval system's own relevance judgment
	// already does that.
	const bool readsProjectDocuments = intent.capability == professional::ProfessionalCapability::Generative;
	// A request that names none of its documents does not have the whole data
	// room read for it. The documents it names are its pick; otherwise the
	// model picks them from a list (Context/SourcePicker), and only those are
	// read - one or two of them whole, more of them by relevance within them.
	// No picker, or a pick of nothing: relevance over the data room, as before.
	std::vector<std::string> sourceDocumentIds = spec.selected_document_ids;
	matters::SelectedSourceDelivery sourceDelivery = matters::SelectedSourceDelivery::EveryPassage;
	std::vector<std::string> pickedDocumentIds;
	// Whether the documents came from the lawyer's own words or from the
	// model's guess. The two are not the same kind of thing and are not read
	// the same way: see where the delivery is decided below.
	bool documentsWereNamed = false;
	// A room that fits the window is read whole (source_picker::fits_whole).
	bool readsWholeRoom = false;
	providers::UsageMetadata pickUsage;
	if (readsProjectDocuments && spec.selected_document_ids.empty() && spec.model_target
			&& intent.capability == professional::ProfessionalCapability::Generative) {
		const std::vector<context::source_picker::ListedDocument> listed = context::source_picker::list_documents(matter);
		// The document the message this request answers points at (its place,
		// D-107). Its name stands in the request because the place names it,
		// not because the lawyer's words do, and those words may rest on other
		// documents - "check this slide against the DD tracker": the model still
		// picks those, and the place's document is read with them (D-113).
		std::string placeDocumentId;
		if (!spec.answers_message_id.empty())
			for (const matters::MatterTask &message : matter.tasks)
				if (message.id == spec.answers_message_id) placeDocumentId = message.place.document_id;
		bool isNamedByPlaceAlone = false;
		if (listed.size() >= context::source_picker::kMinimumDocumentsToPick) {
			pickedDocumentIds = context::source_picker::documents_named_in(spec.prompt, listed);
			isNamedByPlaceAlone = !placeDocumentId.empty() && !pickedDocumentIds.empty()
				&& std::all_of(pickedDocumentIds.begin(), pickedDocumentIds.end(),
					[&](const std::string &documentId) { return documentId == placeDocumentId; });
			if (isNamedByPlaceAlone) pickedDocumentIds.clear();
			documentsWereNamed = !pickedDocumentIds.empty();
			if (pickedDocumentIds.empty()
					&& context::source_picker::fits_whole(matter, listed, availableContextWindowTokens)) {
				// Every document the matter holds fits the window with room to
				// spare: all of it is read, and nothing is left to a guess.
				for (const context::source_picker::ListedDocument &entry : listed)
					pickedDocumentIds.push_back(entry.document_id);
				readsWholeRoom = true;
			} else if (pickedDocumentIds.empty() && spec.recorded_pick) {
				// A resumed operation already picked its documents, and the call that
				// picked them was paid for: the same documents are read again - those
				// still in the Project - and the model is not asked a second time.
				for (const std::string &documentId : *spec.recorded_pick)
					if (std::any_of(listed.begin(), listed.end(), [&](const context::source_picker::ListedDocument &entry) {
							return entry.document_id == documentId; }))
						pickedDocumentIds.push_back(documentId);
			} else if (pickedDocumentIds.empty() && context::source_picker::worth_picking(matter, listed)) {
				SourcePicker picker;
				{
					const std::lock_guard<std::mutex> guard(source_picker_mutex());
					picker = source_picker();
				}
				if (picker) {
					std::string brief;
					for (const context::ContextItem &item : matter.context_items)
						if (item.label == context::kMemoryBriefLabel && item.authority == context::ContextItemAuthority::AutomaticallyDerived
								&& context::context_item_standing(matter, item).used_by_ai)
							brief = item.value;
					platform::trace::Span pickSpan("evidence", "source_pick", {{"documents", listed.size()}});
					try {
						SourcePickCall call;
						call.project_id = matter.id;
						call.operation_id = spec.operation_id;
						call.model = *spec.model_target;
						call.prompt = context::source_picker::pick_prompt(spec.prompt, listed, brief);
						call.contract = context::source_picker::pick_output_contract();
						// The pick carries the lawyer's request, the documents' names and the
						// memory brief: it is protected exactly as the run it belongs to.
						call.protected_api_policy = effective_policy;
						const SourcePickReply reply = picker(call);
						pickUsage = reply.usage;
						pickedDocumentIds = context::source_picker::picked_document_ids(reply.text, listed);
						pickSpan.add("picked", pickedDocumentIds.size());
					} catch (const std::exception &error) {
						pickSpan.add("outcome", "failed");
						pickSpan.add("reason", std::string(error.what()).substr(0, 160));
						pickedDocumentIds.clear();
					}
				}
			}
			// The place's document is read whatever else was picked, first.
			if (isNamedByPlaceAlone
					&& std::find(pickedDocumentIds.begin(), pickedDocumentIds.end(), placeDocumentId) == pickedDocumentIds.end())
				pickedDocumentIds.insert(pickedDocumentIds.begin(), placeDocumentId);
			if (!pickedDocumentIds.empty()) {
				sourceDocumentIds = pickedDocumentIds;
				// A document the lawyer NAMED is a document the lawyer chose, and
				// it is read whole - exactly as if they had ticked it in the
				// composer. Anything less is what produced the answer this rule
				// exists for: asked to prepare a clean draft of a document it had
				// been given passages of, the model correctly refused, because a
				// final instrument cannot be written from extracts. A model's own
				// pick is a guess, so a guess at more than two documents is still
				// read by relevance within them: a guess may not spend the whole
				// window.
				sourceDelivery = documentsWereNamed || readsWholeRoom || pickedDocumentIds.size() <= 2
					? matters::SelectedSourceDelivery::EveryPassage
					: matters::SelectedSourceDelivery::RelevanceWithin;
			}
		}
	}
  auto document_context = readsProjectDocuments
		? matters::retrieve_document_context(workspace_path, matter, spec.prompt, sourceDocumentIds,
			&unavailable_source_documents, sourceDelivery)
		: std::vector<context::RetrievedContextChunk>{};
	// Real "ordinary chat" detection (Beta UX correction), now resolved from
	// STRUCTURAL retrieval outcome rather than a text guess: the silent
	// General preset (never itself user-selectable) combined with no real
	// document engagement at all - no explicit selection, no relevant content
	// actually retrieved, no structured-record candidate, no update
	// candidate - means this message never asked for legal/document work at
	// all. Any real engagement on any of these axes means the user IS asking
	// Singularity to look at something, so this stays false for those.
	prompt_input.plain_chat = intent.capability == professional::ProfessionalCapability::ProductState
		|| (spec.selected_document_ids.empty() && document_context.empty()
			&& !directDocumentWork.has_value() && !artifactCandidates.existing_target_document_id.has_value());
	// Deterministic user control only (@web / an explicit instruction not to use
	// the internet); whether external evidence is actually needed is the
	// model's own typed judgment - see externalResearchRequest below.
	const research::ResearchMode resolvedResearchMode = research::resolve_research_mode(spec.prompt);
	// Web research that cannot run on this Node (its backend is not connected,
	// or Papinian cannot minimize a query) is never offered: the request runs
	// without it and says so, instead of failing mid-operation.
	std::optional<std::string> researchUnavailableReason;
	// Work kept on this computer researches nothing on the web - on every
	// attempt, whichever thread or computer carries it on.
	const bool worksOnlyHere = operation_works_only_here(matter, spec.operation_id);
	if (worksOnlyHere) {
		researchUnavailableReason = std::string(routing::kLocalOnlyWorkResearchUnavailable);
	} else if ((resolvedResearchMode == research::ResearchMode::Auto || resolvedResearchMode == research::ResearchMode::Required)
			&& spec.operation_id.empty() == false) {
		tools::ToolExecutionContext availabilityContext;
		availabilityContext.project_id = matter.id;
		availabilityContext.operation_id = spec.operation_id;
		availabilityContext.research_backend = settings.local_node_setup.research_backend;
		researchUnavailableReason = routing::research_unavailable_reason(availabilityContext);
	}
	const bool researchStepSuppressed = connectedDocumentAction && connectedDocumentAction->compact_context_mode;
	// The ONE research mode of this first call, for both the direct-document-
	// work and the generic branch below: a compact-context candidate's first
	// (WHAT-decision) call never researches - see the comment at the generic
	// branch - and the instruction the model receives says exactly that.
	const research::ResearchMode firstCallResearchMode =
		researchStepSuppressed || researchUnavailableReason ? research::ResearchMode::Off : resolvedResearchMode;
	// What typed professional results THIS objective produces, taken from the
	// Legal task the user chose (a structural fact Singularity already records on
	// every task), never from reading the request's wording. An unknown or
	// no-longer-registered preset is never silently treated as General - the
	// same rule find_legal_task_preset() already states - so it produces no
	// typed results rather than the wrong ones.
	const professional::StructuredResultKinds requiredStructuredResults =
		context::find_legal_task_preset(spec.legal_task_preset_id, spec.legal_task_preset_version)
			.value_or(context::LegalTaskPreset{}).structured_results;
	bool computedStatementsOffered = false;
	bool answerTextChecked = false;
	bool computeProtocolOffered = false;
	if (intent.capability == professional::ProfessionalCapability::Generative) {
		std::string instructions;
		// Singularity's own execution setting, so a question about which model or
		// provider runs this request is answered from fact, never guessed.
		if (spec.model_target)
			instructions += "This request runs on the model \"" + spec.model_target->model_id + "\" through the "
				"provider \"" + spec.model_target->provider_id + "\"; answer any question about the model or provider "
				"in use from this fact. ";
		instructions +=
			"Answer the user's actual request in the response field, citing each supported point by its exact "
			"[SOURCE:...] marker. Propose a side effect (actionType/artifactAction/structuredRecord) only when the "
			"request calls for one; otherwise actionType is \"none\".";
		// Project Context is what the Project already remembers (its memory brief
		// among it): read first, never cited as a source. The answer also names
		// the conversation. Kept to two sentences - every model reads this on
		// every request - and a model with a small window, which keeps its room
		// for the conversation itself, is not asked at all.
		constexpr std::size_t kMemoryGuidanceMinimumWindowTokens = kJournalMinimumWindowTokens;
		if (!availableContextWindowTokens || *availableContextWindowTokens >= kMemoryGuidanceMinimumWindowTokens) {
			instructions +=
				" Project Context, when supplied, is what is already remembered about this Project: consult it first, "
				"but it is not a source: what you state about a document still rests on, and cites, the supplied "
				"passages. A Project Context record of kind data_room_change is Singularity's own journal "
				"of what happened to the Project's documents, newest first - added, changed, moved, removed or "
				"returned, when, and whether Singularity made the change: answer what is new in the data room from it. "
				"chatTitle: a three-to-six-word name for this conversation's subject, written as a sentence is "
				"(a capital only for the first word and for names) with abbreviations as the lawyer wrote them, or null.";
			// A provider's model reads a protected request's amounts and dates as
			// placeholders. What it would compare or add up, it asks Singularity to
			// work out on the lawyer's computer (Context/ComputedStatements) -
			// nothing more leaves the device, and the lawyer reads the answer to it.
			// A name takes the form its place needs when it is restored
			// (PapinianGuard/RestorationGrammar): the model is never asked for it.
			// Under protection the model asks for typed operations (DESIGN4 §1.9, D6);
			// statements stay for a model on this computer, which reads the values.
			if (effective_policy.mode != protected_api::Mode::Off && spec.model_target
					&& spec.model_target->provider_id != "local") {
				instructions += std::string(" ") + protected_api::papinian::compute::compute_instruction();
				computedStatementsOffered = true;
				computeProtocolOffered = true;
			}
			// A workbook's total can count rows the lawyer does not see, and the
			// reading says so beside the formula; a live run still took such a
			// total at face value (a hidden, withdrawn claim, 24 Sep 2026).
			if (std::any_of(matter.documents.begin(), matter.documents.end(), [](const matters::MatterDocument &document) {
					return document.file_type == documents::DocumentFormat::xlsx; }))
				instructions += " A workbook figure marked as counting hidden rows includes rows the lawyer does not see: "
					"before relying on it, say which rows it counts and check what each hidden row is in the other "
					"documents.";
		}
		// Present only when this objective's own legal task actually produces
		// typed results, and then mandatory: the schema is the shape authority
		// and the provenance rule is enforced on the way in, so this says only
		// what neither can - that the typed results accompany the written
		// answer rather than replacing it. A request that produces no typed
		// results carries none of these bytes at all.
		if (professional::any_structured_result_required(requiredStructuredResults))
			instructions += " Also fill structuredResults from the supplied material; it accompanies your written "
				"answer and never replaces it.";
		if (directDocumentWork) {
			// A new record needs a genuinely free template: with none, only a
			// revision of this Chat's own prior record is offered, so a new record
			// is never written over it.
			const bool newRecordAvailable = directDocumentWork->new_work_target.has_value();
			if (newRecordAvailable && directDocumentWork->update_target.has_value())
				instructions += " A structured professional record candidate IS resolved for this Chat (actionType "
					"structured_record_fill_template for a new object, or structured_record_update_existing to revise "
					"this exact Chat's own prior object) - use it only if the request actually calls for that kind of "
					"structured record work.";
			else if (newRecordAvailable)
				instructions += " A structured professional record candidate IS resolved for this Chat (actionType "
					"structured_record_fill_template for a new object) - use it only if the request actually calls for "
					"that kind of structured record work.";
			else
				instructions += " A structured professional record candidate IS resolved for this Chat only to revise "
					"its own prior object (actionType structured_record_update_existing) - no new record can be added in "
					"this request's scope; use it only if the request actually asks to revise that record.";
		}
		// Every action of this protocol is named by the change it makes to a
		// document, and a model choosing between them is choosing between
		// changes - not between machinery. AdvancedWorkflow described by its
		// scale alone was chosen twice in live runs for work it cannot do: once
		// to SEE a data-room-wide table (150k tokens spent declining regions
		// nobody asked to change), once to add one entry to a register, where it
		// found no region to fill because a register has none until a row
		// exists. What each action can reach is part of the protocol, said once,
		// for all of them.
		if (connectedDocumentAction)
			instructions += " An AdvancedWorkflow candidate IS resolved (actionType \"advanced_workflow\"). Choose the "
				"action by the change asked for: create_content makes a new document; append_content adds what the "
				"document lacks (a further row, entry or paragraph); replace_content rewrites wording anchored by it; "
				"update_cells changes existing cells; advanced_workflow fills and revises regions that already exist, "
				"in one or more documents, in a governed pass with per-item coverage and an independent completeness "
				"review before anything is committed - it never adds a region. Work to READ (an answer, table, list, "
				"comparison or chronology, from any number of documents) changes no document: give it in response and "
				"workProduct with actionType \"none\".";
		// Checked against the lawyer's words by the approval floor, as a
		// document action's own quote is. Where the document action's own
		// instruction already says what requestedBy holds, it is not said twice:
		// every word here is required prompt on a small window.
		const bool requestedByExplained = artifactCandidates.model_instruction.find("requestedBy:") != std::string::npos;
		instructions += " " + artifactCandidates.model_instruction;
		if (directDocumentWork || connectedDocumentAction)
			instructions += requestedByExplained
				? " A structured record or advanced_workflow quotes them in its own requestedBy."
				: " For a structured record or advanced_workflow, requestedBy quotes the lawyer's words asking for it; "
					"else null.";
		// Only a turn that offers research tools cannot carry the response schema,
		// so only it is told the artifactAction shape in words.
		if (firstCallResearchMode == research::ResearchMode::Required && !artifactCandidates.artifact_shape_instruction.empty())
			instructions += " " + artifactCandidates.artifact_shape_instruction;
		if (researchUnavailableReason && resolvedResearchMode == research::ResearchMode::Required)
			instructions += " The user asked for external web research, but it cannot run on this Node (" + *researchUnavailableReason
				+ "): externalResearchRequest must be null. Say plainly in the response that current external authority "
				"could not be researched, and declare every conclusion that depends on it as an evidence gap.";
		else if (researchUnavailableReason)
			instructions += " External research cannot run on this Node for this request (" + *researchUnavailableReason
				+ "): externalResearchRequest must be null. If the objective depends on external or current authority the "
				"supplied material does not contain, say so plainly in the response and declare it as an evidence gap.";
		else if (researchStepSuppressed)
			instructions += " externalResearchRequest must be null: only AdvancedWorkflow, if you choose it, researches, "
				"in its own later steps. Otherwise, where the objective depends on external or current authority the "
				"supplied material does not contain, say so plainly and declare it as an evidence gap.";
		else if (resolvedResearchMode == research::ResearchMode::Auto)
			instructions += " Singularity can run external public research for this request. If the objective cannot be "
				"completed correctly without external or current public authority that the supplied material does not "
				"contain, set externalResearchRequest to ONE privacy-minimized, issue-level public query (never a "
				"confidential Project name, party, amount or fact) with its jurisdiction and freshnessRequired, and still "
				"give your best supported answer so far; Singularity decides whether that research may run and returns its "
				"evidence to you in this same task. When the supplied material is enough, externalResearchRequest must "
				"be null.";
		else if (resolvedResearchMode == research::ResearchMode::Off)
			instructions += " External research is turned off for this request: externalResearchRequest must be "
				"null. If the objective depends on external or current authority the supplied material does not contain, "
				"say so plainly in the response and declare it as an evidence gap.";
		else
			instructions += " Use the research tools directly for this request; externalResearchRequest must be null.";
		prompt_input.capability_instructions = instructions;
	}
  // Assigns every retrieved chunk its opaque provider-facing marker up
  // front, before PromptAssembler's budget can decide to drop some of them -
  // see the restriction below, which narrows this down to the "exact
  // request context snapshot" (Master Spec Sec.22) once that decision is
  // known.
	// Capacity-first delivery (see the budget below): each passage keeps its
	// retrieval rank as its admission priority - what is admitted first,
	// round-robin across documents, when the resolved capacity cannot hold the
	// whole selection - while SOURCE_MATERIAL and the marker numbering follow
	// each selected document's own reading order.
	// SLR-5: a follow-up that reviews, challenges or builds on an earlier answer
	// has to be able to SEE what that answer rested on. Retrieval only ever
	// looks at the current message, so "check the finding you made about the
	// termination clause" would arrive with the finding's own passage missing
	// and nothing to check it against. The passages this Chat's earlier answers
	// actually cited are re-admitted here as ordinary optional material: the
	// capacity law decides how much of it fits, and coverage states what was
	// carried. The source boundary is unchanged - a document outside THIS
	// request's own scope is never re-supplied because an earlier turn cited it
	// (see EVD-1/EXEC-1), which is exactly why this reads the scope that
	// retrieval itself just used.
	if (readsProjectDocuments && spec.chat_id) {
		std::unordered_set<std::string> present;
		for (const context::RetrievedContextChunk &source : document_context)
			present.insert(source.chunk.chunk_id);
		// The request's OWN source boundary, not whatever this message happened
		// to retrieve: an explicit selection is that boundary, and without one
		// the Project is. Reading it from the retrieval result instead would
		// mean the very case this exists for - a follow-up whose own words
		// retrieve nothing - still arrives with no evidence.
		const std::unordered_set<std::string> selectedScope(
			spec.selected_document_ids.begin(), spec.selected_document_ids.end());
		const auto inScope = [&](const std::string &documentId) {
			return selectedScope.empty() || selectedScope.count(documentId) != 0;
		};
		std::unordered_set<std::string> citedChunkIds;
		std::vector<std::string> citedDocumentIds;
		// Only the recent turns of this Chat. "The answer you just gave" is what a
		// follow-up reviews; re-reading every document a long Chat ever cited
		// would make each later request slower than the one before it, for
		// evidence nobody asked about. Older turns still reach the request as
		// their own records (CHAT-1/CHAT-2).
		//
		// Recent turns THAT RESTED ON EVIDENCE, not simply recent turns. A Chat
		// accumulates work that cites nothing - a Consilium panel's own member
		// records, a rename, an answer given from Memory alone - and counting
		// those against the window pushed out the one answer a follow-up is
		// actually about. A lawyer who runs a panel and then says "check that
		// again" means the finding, not the panel.
		constexpr std::size_t kRecentTurnsWithEvidence = 4;
		std::vector<const matters::MatterTask *> recentChatTurns;
		for (const matters::MatterTask &priorTask : matter.tasks) {
			if (!priorTask.chat_id || *priorTask.chat_id != *spec.chat_id) continue;
			if (priorTask.status != matters::TaskStatus::Success) continue;
			if (priorTask.source_references.empty()) continue;
			if (spec.root_task_id && matters::task_group_id(priorTask) == *spec.root_task_id) continue;
			// A resumed operation is not an earlier turn of itself: what it cited
			// comes back with its own checkpoint, never as a prior answer's evidence.
			if (!spec.operation_id.empty() && priorTask.agent_operation_id == std::optional<std::string>(spec.operation_id))
				continue;
			recentChatTurns.push_back(&priorTask);
		}
		if (recentChatTurns.size() > kRecentTurnsWithEvidence)
			recentChatTurns.erase(recentChatTurns.begin(),
				recentChatTurns.end() - static_cast<std::ptrdiff_t>(kRecentTurnsWithEvidence));
		for (const matters::MatterTask *priorTaskPointer : recentChatTurns) {
			const matters::MatterTask &priorTask = *priorTaskPointer;
			for (const context::ResolvedSourceReference &reference : priorTask.source_references) {
				if (!inScope(reference.document_id)) continue;
				if (present.count(reference.chunk_id) != 0) continue;
				if (!citedChunkIds.insert(reference.chunk_id).second) continue;
				if (std::find(citedDocumentIds.begin(), citedDocumentIds.end(), reference.document_id)
						== citedDocumentIds.end())
					citedDocumentIds.push_back(reference.document_id);
			}
		}
		// The documents this Chat's latest turns wrote - a new amendment, the
		// agreement it just changed. "Now make the penalty apply only to material
		// breaches" is about the document just created, and the lawyer should not
		// have to name it: it comes back whole, readable and nameable like any
		// supplied document. Nameable is all it is - which document the request
		// changes is still the model's to say, and whether that waits for the
		// lawyer is still the Project's permission.
		constexpr std::size_t kRecentWrittenDocuments = 2;
		std::vector<std::string> writtenDocumentIds;
		for (auto priorTask = matter.tasks.rbegin(); priorTask != matter.tasks.rend(); ++priorTask) {
			if (writtenDocumentIds.size() >= kRecentWrittenDocuments) break;
			if (!priorTask->chat_id || *priorTask->chat_id != *spec.chat_id) continue;
			if (priorTask->status != matters::TaskStatus::Success || !priorTask->artifact_document_id) continue;
			if (!spec.operation_id.empty() && priorTask->agent_operation_id == std::optional<std::string>(spec.operation_id))
				continue;
			const std::string &written = *priorTask->artifact_document_id;
			if (!inScope(written)) continue;
			if (std::find(writtenDocumentIds.begin(), writtenDocumentIds.end(), written) == writtenDocumentIds.end())
				writtenDocumentIds.push_back(written);
		}
		for (const std::string &documentId : writtenDocumentIds) {
			std::vector<context::RetrievedContextChunk> passages;
			try {
				passages = matters::retrieve_document_context(workspace_path, matter, spec.prompt, {documentId},
					nullptr, matters::SelectedSourceDelivery::EveryPassage);
			} catch (const std::exception &) {
				continue;
			}
			for (context::RetrievedContextChunk &passage : passages) {
				if (!present.insert(passage.chunk.chunk_id).second) continue;
				passage.explicitly_selected = false;
				document_context.push_back(std::move(passage));
			}
		}
		for (const std::string &documentId : citedDocumentIds) {
			std::vector<context::RetrievedContextChunk> passages;
			try {
				passages = matters::retrieve_document_context(workspace_path, matter, spec.prompt, {documentId},
					nullptr, matters::SelectedSourceDelivery::EveryPassage);
			} catch (const std::exception &) {
				continue;
			}
			for (context::RetrievedContextChunk &passage : passages) {
				if (citedChunkIds.count(passage.chunk.chunk_id) == 0) continue;
				if (!present.insert(passage.chunk.chunk_id).second) continue;
				// Optional material, never an explicit selection: it yields to
				// what this request itself selected, exactly like any other
				// retrieved passage.
				passage.explicitly_selected = false;
				document_context.push_back(std::move(passage));
			}
		}
	}
	const auto passage_priority = ordered_for_capacity_first_delivery(document_context, spec.selected_document_ids);
  const context::SourceMarkerRegistry retrieval_markers(document_context);
	// The order of the documents the prompt carries, as relations and never as
	// dates (Context/TemporalContext): each document dated once, and related to
	// exactly the documents whose passages the budget admits.
	const auto datedDocuments = std::make_shared<const std::vector<context::temporal_context::DatedDocument>>(
		dated_request_documents(workspace_path, matter, document_context, retrieval_markers));
	// A model with a small window keeps its room for the passages (the journal's
	// bound); it is a local one, whose documents' dates are not kept from it.
	if (!availableContextWindowTokens || *availableContextWindowTokens >= kJournalMinimumWindowTokens)
	prompt_input.temporal_context = [datedDocuments](const std::vector<std::string> &documentIds) {
		std::vector<context::temporal_context::DatedDocument> carried;
		for (const auto &dated : *datedDocuments)
			if (std::find(documentIds.begin(), documentIds.end(), dated.document.document_id) != documentIds.end())
				carried.push_back(dated);
		return context::temporal_context::derive(carried).text;
	};
  std::unordered_set<std::string> distinct_context_document_ids;
	// The first professional_response call never carries the AdvancedWorkflow
	// candidate's structural "targets" block (that block exists only in the
	// operation turn wired after the model declares advanced_workflow), so
	// every retrieved chunk - including one from a candidate document - is
	// this call's SOURCE_MATERIAL. Only the operation turn's own request data
	// below drops the candidate documents, which it already receives through
	// that targets block.
	std::unordered_set<std::string> operation_targets_chunk_ids;
	for (const auto &source : document_context) {
		if (connectedDocumentAction.has_value()) {
			const auto &others = connectedDocumentAction->other_candidate_documents;
			if (source.document_id == connectedDocumentAction->methodology_document_id
					|| source.document_id == connectedDocumentAction->target_document_id
					|| std::any_of(others.begin(), others.end(), [&](const auto &candidate) {
						return candidate.document_id == source.document_id; }))
				operation_targets_chunk_ids.insert(source.chunk.chunk_id);
		}
    prompt_input.source_chunks.push_back(
        {source.chunk.chunk_id, retrieval_markers.format_chunk(source),
         source.explicitly_selected, source.document_id, passage_priority.at(source.chunk.chunk_id),
         retrieval_markers.document_token_for(source.chunk.chunk_id), source.document_title});
    distinct_context_document_ids.insert(source.document_id);
  }
	// Which document each handle is - its folder and file name - stands above
	// the document's passages (SourceMaterialSpans' document_header()), where a
	// passage is read: a live run (24 Sep 2026) read "Previous Drafts/Northwind
	// MSA - Draft of February 10, 2026" as a renewal draft that followed the
	// signed agreement, and one (11 Oct 2026) reported the sellers' escrow as the
	// buyer's while the names stood in a Project Context record apart.
	// The existing documents this request may be asked to CHANGE, under the
	// same handles it is given to cite them by. Singularity still chooses no
	// target: it says which documents can be named, and the model names the
	// one the request asks to change (ProfessionalArtifactAction.hpp's
	// ArtifactTargetHandle). Offered from what retrieval supplied, before the
	// context budget narrows it - a document the budget drops is one the model
	// never reads, so it never names it.
	{
		std::vector<documents::ArtifactTargetHandle> nameable;
		std::unordered_set<std::string> offered;
		for (const auto &source : document_context) {
			if (!offered.insert(source.document_id).second) continue;
			const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
				[&](const matters::MatterDocument &candidate) { return candidate.id == source.document_id; });
			if (document == matter.documents.end()) continue;
			nameable.push_back({retrieval_markers.document_token_for(source.chunk.chunk_id),
				document->id, document->file_type});
		}
		const std::string offer = documents::offer_nameable_targets(artifactCandidates, std::move(nameable));
		if (!offer.empty())
			prompt_input.capability_instructions = prompt_input.capability_instructions.value_or(std::string()) + " " + offer;
	}
  // A retry/run-with-another-model attempt (spec.root_task_id set) must not
  // receive any other attempt of its own chain as conversational history -
  // see matters::exclude_chain().
  std::vector<matters::MatterTask> history_candidates =
      spec.root_task_id ? matters::exclude_chain(matter.tasks, *spec.root_task_id)
                        : matter.tasks;
  {
    // A request inside a Chat continues that Chat only. A request outside any
    // Chat has no Chat conversation: no Chat's turns (including a deleted
    // Chat's) and no automation record ever become its hidden history. The
    // request's own operation record is never one of its earlier turns.
    std::vector<matters::MatterTask> scoped;
    scoped.reserve(history_candidates.size());
    for (auto &candidate : history_candidates) {
      const bool sameConversation = spec.chat_id
        ? candidate.chat_id && *candidate.chat_id == *spec.chat_id
        : !candidate.chat_id && (candidate.operation_type.empty() || candidate.operation_type == "matter_task");
      const bool ownOperation = !spec.operation_id.empty() && candidate.agent_operation_id
        && *candidate.agent_operation_id == spec.operation_id;
      if (sameConversation && !ownOperation) scoped.push_back(std::move(candidate));
    }
    history_candidates = std::move(scoped);
  }
  // A conversation the member cleared here begins afresh: what was said up to
  // then goes into no request made from this computer
  // (Chat/ConversationClearing; the owner, 2026-11-04).
  if (spec.chat_id) {
    if (const auto cleared = chat::conversation_clearing::cleared_before(*spec.chat_id)) {
      history_candidates.erase(std::remove_if(history_candidates.begin(), history_candidates.end(),
          [&](const matters::MatterTask &candidate) { return !candidate.created_at.empty() && candidate.created_at <= *cleared; }),
          history_candidates.end());
    }
  }
  // Every earlier turn as a record, and each finished answer whole as capacity
  // allows (PromptAssembler admits and states the coverage).
  prompt_input.history_items = chat::conversation_history_items(matter, history_candidates);
	// A Chat the person compacted carries its earlier turns as ONE record built
	// from those turns' own canonical records, and everything sent afterwards as
	// itself. The compaction is the Chat's, not this request's: it survives a
	// restart and behaves the same on a frontier model and a local one, so the
	// conversation does not quietly mean something different depending on which
	// model is answering.
	if (spec.chat_id) {
		const auto compactedChat = std::find_if(matter.chats.begin(), matter.chats.end(),
			[&](const matters::MatterChat &entry) { return entry.id == *spec.chat_id; });
		// A compaction made before the conversation was cleared here is part of
		// what was cleared.
		const auto clearedAt = chat::conversation_clearing::cleared_before(*spec.chat_id);
		const bool isClearedCompaction = compactedChat != matter.chats.end() && compactedChat->compaction && clearedAt
			&& compactedChat->compaction->created_at <= *clearedAt;
		if (compactedChat != matter.chats.end() && compactedChat->compaction && !isClearedCompaction) {
			const matters::ChatCompaction &compaction = *compactedChat->compaction;
			const std::set<std::string> covered(compaction.covered_task_ids.begin(),
				compaction.covered_task_ids.end());
			std::vector<context::PromptHistoryItem> carried;
			context::PromptHistoryItem compacted;
			compacted.id = "chat_compaction";
			compacted.content = compaction.record;
			compacted.state_record = compaction.record;
			// The smallest form the conversation can be carried in, for a window
			// that cannot hold the one above. Every covered turn is still named in
			// it; a compaction written before this form existed falls back to the
			// one record it has, exactly as it behaved then.
			compacted.brief_record = compaction.brief_record.empty()
				? compaction.record : compaction.brief_record;
			carried.push_back(std::move(compacted));
			for (context::PromptHistoryItem &item : prompt_input.history_items)
				if (covered.count(item.id) == 0) carried.push_back(std::move(item));
			prompt_input.history_items = std::move(carried);
		}
	}
	std::size_t historyBytes = 0;
	for (const auto &item : prompt_input.history_items) historyBytes += item.content.size();
#if defined(SINGULARITY_CONNECTED_GOLDEN_MEMORY_DIAGNOSTIC)
	routing::memory_diagnostics::conversation_history_bytes = historyBytes;
#endif

  routing::InferenceRequest request;
	nlohmann::json operationRequestData;
	std::optional<std::string> operationCapacityRefusal;
	// Resolved candidates add only their own description (a structured-record
	// template, a bound artifact's preview); the request keeps its canonical
	// prompt and source selection (see DirectDocumentWorkTaskPlan::
	// candidate_context and ProfessionalArtifactCandidates::candidate_context).
	{
		std::string candidateContext = directDocumentWork ? directDocumentWork->candidate_context : std::string();
		if (!artifactCandidates.candidate_context.empty())
			candidateContext += (candidateContext.empty() ? "" : "\n\n") + artifactCandidates.candidate_context;
		if (!candidateContext.empty()) prompt_input.candidate_context = std::move(candidateContext);
	}
	// What relevance retrieval could have drawn from, so the request can say
	// truthfully how much of the Project it actually read.
	if (spec.selected_document_ids.empty()) {
		std::size_t readable = 0;
		for (const matters::MatterDocument &document : matter.documents)
			if (document.extraction_status == matters::DocumentExtractionState::Ready) ++readable;
		prompt_input.relevance_candidate_documents = readable;
	}
	// Project inspection (Context/ProjectEvidence): the model may ask, in its
	// answer, to list, search and read more of the Project, and the same
	// operation continues with what was read. Offered for generative work in an
	// operation (the reads run through ToolBroker) whenever the request's own
	// scope has a document to read and the answer is given in the final schema;
	// a request that must search the web first keeps its research tools instead.
	std::shared_ptr<context::project_evidence::Session> projectEvidence;
	std::optional<std::string> capabilityWithoutInspection;
	// What it returns is bounded by the room the executing model's own context
	// still has, so it is offered only where that capacity is known: an unknown
	// window is never guessed at (ContextBudget.hpp).
	if (intent.capability == professional::ProfessionalCapability::Generative && !spec.operation_id.empty()
			&& firstCallResearchMode != research::ResearchMode::Required && executingContextWindowTokens) {
		// What inspection reads from: the Project's documents, never its history.
		matters::Matter readable;
		readable.id = matter.id;
		readable.name = matter.name;
		readable.directory_path = matter.directory_path;
		readable.documents = matter.documents;
		std::unordered_set<std::string> nameable;
		for (const documents::ArtifactTargetHandle &target : artifactCandidates.nameable_targets)
			nameable.insert(target.document_id);
		// A document found by inspection may become the target only where one
		// supplied could have: nothing is bound.
		const bool foundMayBeTarget = !artifactCandidates.existing_target_document_id;
		auto session = std::make_shared<context::project_evidence::Session>(workspace_path, std::move(readable),
			spec.selected_document_ids, document_context,
			[](std::string_view text) { return research::estimate_serialized_tokens(text); },
			foundMayBeTarget
				? context::project_evidence::Session::Changeable([](const matters::MatterDocument &document) {
					return documents::artifact_format_capability(document.file_type).can_update(); })
				: context::project_evidence::Session::Changeable(),
			std::move(nameable));
		if (session->readable_count() > 0) {
			capabilityWithoutInspection = prompt_input.capability_instructions.value_or(std::string());
			prompt_input.capability_instructions = *capabilityWithoutInspection
				+ (capabilityWithoutInspection->empty() ? "" : " ")
				+ project_inspection_protocol(*session, !spec.selected_document_ids.empty());
			projectEvidence = std::move(session);
		}
	}
	// A question to the lawyer (Tool/LawyerQuestion): the model may stop, in
	// the middle of its work, to ask what only the lawyer can decide, and the
	// same operation continues with the answer. Offered only where someone can
	// answer - a Chat request from the app, in an operation whose question
	// ToolBroker can hold - and only to an answer given in the final schema; a
	// request that must search the web first keeps its research tools instead.
	std::optional<std::string> capabilityWithoutQuestion;
	std::optional<std::string> capabilityWithoutInspectionOrQuestion;
	if (spec.can_ask_lawyer && !spec.operation_id.empty() && spec.chat_id && !spec.chat_id->empty()
			&& intent.capability == professional::ProfessionalCapability::Generative
			&& firstCallResearchMode != research::ResearchMode::Required) {
		const auto withQuestion = [](const std::string &text) {
			return text + (text.empty() ? "" : " ") + tools::lawyer_question::protocol();
		};
		capabilityWithoutQuestion = prompt_input.capability_instructions.value_or(std::string());
		prompt_input.capability_instructions = withQuestion(*capabilityWithoutQuestion);
		// A later round of the operation (a review's redraft) can still ask, so
		// the capability it is assembled from carries the protocol too.
		if (capabilityWithoutInspection) {
			capabilityWithoutInspectionOrQuestion = capabilityWithoutInspection;
			capabilityWithoutInspection = withQuestion(*capabilityWithoutInspection);
		}
	}
	// A careful drafter's extra care (ProfessionalArtifactCandidates::
	// drafting_guidance) is offered on the same terms as the question, and is
	// the first to go: a model with no room for it still drafts, only without
	// the extra care it asks for. Offered to the first call alone.
	std::optional<std::string> capabilityWithoutGuidance;
	if (!artifactCandidates.drafting_guidance.empty()) {
		capabilityWithoutGuidance = prompt_input.capability_instructions.value_or(std::string());
		prompt_input.capability_instructions = *capabilityWithoutGuidance
			+ (capabilityWithoutGuidance->empty() ? "" : " ") + artifactCandidates.drafting_guidance;
	}
	apply_capacity_first_budget(prompt_input, executingContextWindowTokens, professionalResponseMaxOutputTokens);
	// The inspection protocol is required text like the rest of the capability
	// section, so whether it can be offered is decided by the same measurement
	// that decides everything else this request carries: assembled against the
	// executing model's own capacity. When it cannot fit beside what the request
	// itself requires, the request runs without it - exactly as it did before
	// inspection existed - and only a request that cannot fit even then fails.
	// The question protocol is offered on the same terms and is the first to go:
	// reading the Project serves the answer more than asking the lawyer does.
	const auto assembled = [&]() {
		try {
			return context::PromptAssembler::assemble_with_budget(prompt_input);
		} catch (const context::ContextCapacityExceeded &) {
			if (!capabilityWithoutGuidance && !capabilityWithoutQuestion && !projectEvidence) throw;
		}
		if (capabilityWithoutGuidance) {
			prompt_input.capability_instructions = capabilityWithoutGuidance->empty()
				? std::nullopt : capabilityWithoutGuidance;
			capabilityWithoutGuidance.reset();
			try {
				return context::PromptAssembler::assemble_with_budget(prompt_input);
			} catch (const context::ContextCapacityExceeded &) {
				if (!capabilityWithoutQuestion && !projectEvidence) throw;
			}
		}
		if (capabilityWithoutQuestion) {
			prompt_input.capability_instructions = capabilityWithoutQuestion->empty()
				? std::nullopt : capabilityWithoutQuestion;
			if (capabilityWithoutInspection) capabilityWithoutInspection = capabilityWithoutInspectionOrQuestion;
			capabilityWithoutQuestion.reset();
			try {
				return context::PromptAssembler::assemble_with_budget(prompt_input);
			} catch (const context::ContextCapacityExceeded &) {
				if (!projectEvidence) throw;
			}
		}
		prompt_input.capability_instructions = capabilityWithoutInspection->empty()
			? std::nullopt : capabilityWithoutInspection;
		projectEvidence.reset();
		capabilityWithoutInspection.reset();
		return context::PromptAssembler::assemble_with_budget(prompt_input);
	}();
	const bool lawyerQuestionOffered = capabilityWithoutQuestion.has_value();
	routing::memory_diagnostics::checkpoint("matter_request_prompt_assembled",
		"assembled_prompt_bytes=" + std::to_string(assembled.prompt.size())
		+ " project_context_bytes=" + std::to_string(projectContextBytes)
		+ " conversation_history_bytes=" + std::to_string(historyBytes)
		+ " source_chunks=" + std::to_string(document_context.size()));
	// How many documents the request drew passages from, and how many of them
	// still have a passage in the prompt after the capacity budget - the fact a
	// lawyer needs when evidence was left out.
	std::size_t candidateDocumentCount = 0;
	std::size_t suppliedDocumentCount = 0;
	{
		std::unordered_set<std::string> candidates;
		std::unordered_set<std::string> supplied;
		std::unordered_set<std::string> includedIds;
		for (const auto &item : assembled.budget.included) includedIds.insert(item.id);
		for (const auto &source : prompt_input.source_chunks) {
			candidates.insert(source.document_id);
			if (includedIds.count(source.id) != 0) supplied.insert(source.document_id);
		}
		candidateDocumentCount = candidates.size();
		suppliedDocumentCount = supplied.size();
	}
  obs::emit({obs::OperationEventKind::ProjectContextBuildEnd, "Project context assembled",
      {{"promptBytes", std::to_string(assembled.prompt.size())}, {"projectContextBytes", std::to_string(projectContextBytes)},
       {"candidateDocumentCount", std::to_string(candidateDocumentCount)},
       {"suppliedDocumentCount", std::to_string(suppliedDocumentCount)}},
      obs::has_sink() ? std::optional<std::string>(assembled.prompt) : std::nullopt});
  {
  request.prompt = assembled.prompt;
	// A candidate whose OWN structural resolution already says
	// compact_context_mode (too large for direct single-shot handling - see
	// ConnectedDocumentActionPlan::compact_context_mode's own comment) must
	// never also attempt Required research directly in this first, WHAT-
	// decision call: that call's job is only to learn whether AdvancedWorkflow
	// is even wanted, never to itself perform the compaction-dependent research
	// only the second, properly-wired AdvancedWorkflow call (wire_connected_
	// document_action_request()) actually knows how to bound. A candidate that
	// fits directly (compact_context_mode false) keeps ordinary research
	// composability (Section 21) - "find case law and answer" remains one call.
	request.research_mode = firstCallResearchMode;
	if (intent.capability == professional::ProfessionalCapability::Generative) {
		documents::ProfessionalResponseScope responseScope;
		responseScope.can_create = artifactCandidates.can_create;
		// A document bound by the lawyer, or one of the supplied documents the
		// model may name as its own target (nameable_targets): either way a
		// change to an existing document is something this request can do.
		responseScope.has_update_target = (artifactCandidates.existing_target_document_id.has_value()
			|| !artifactCandidates.nameable_targets.empty()
			|| (projectEvidence && projectEvidence->may_find_changeable()));
		responseScope.project_evidence_available = projectEvidence != nullptr;
		responseScope.lawyer_question_available = lawyerQuestionOffered;
		responseScope.advanced_workflow_available = connectedDocumentAction.has_value();
		responseScope.local_facts_available = computedStatementsOffered;
		responseScope.compute_protocol_available = computeProtocolOffered;
		request.research_final_output_contract =
			documents::professional_response_output_contract(requiredStructuredResults,
				directDocumentWork.has_value(), responseScope);
		// Native-tool-capable providers cannot receive tools and a strict final
		// response schema on the same research-loop turn. Validate the returned
		// final at the loop boundary anyway, so one malformed-but-JSON object is
		// repaired once with tools removed and the real professional_response
		// contract applied, instead of escaping the loop only to fail later in
		// execute_task_attempt(). Plain conversational text remains a valid
		// NoAction answer through the parser's intentional compatibility rule.
		request.accepts_research_final_text = [requireJsonFinal = firstCallResearchMode == research::ResearchMode::Required](
				const std::string &text) {
			if (requireJsonFinal && !nlohmann::json::parse(text, nullptr, false).is_object()) return false;
			return documents::parse_professional_response_proposal(text).has_value();
		};
		request.research_final_text_instruction =
			"Return exactly one complete response matching the supplied professional_response schema. "
			"Use the exact nested artifactAction and structuredRecord fields; do not move their fields "
			"to the outer object or invent an alternative artifact shape.";
		request.research_request_from_final_text = professional_research_request_from_final_text;
		if (projectEvidence) {
			request.project_evidence_calls_from_final_text = project_evidence_calls_from_final_text;
			request.maximum_project_evidence_requests = kProjectEvidenceRequestsPerAnswer;
		}
		// How many of this operation's questions are left is the task's to say
		// (execute_task_attempt()); a request prepared without one - a preview -
		// is offered the whole number.
		if (lawyerQuestionOffered) {
			request.lawyer_question_from_final_text = lawyer_question_from_final_text;
			request.maximum_lawyer_questions = kMaximumLawyerQuestions;
		}
		// What the model asks Singularity to work out about protected values, and
		// every statement it writes checked before its answer is taken: one it
		// cannot work out goes back to the same model, in the same operation.
		if (computeProtocolOffered) {
			// THE PROTECTED COMPUTATION PROTOCOL (DESIGN4 §1): the model's ops ride
			// on its answer (localFacts.ops) or beside an operation's tools
			// (lm_compute); the research loop serves them with these budgets, over
			// the tables of the documents the prompt names, read whole.
			request.compute_budget = protected_api::papinian::compute::ComputeBudgetLimits{};
			const auto tableSource = routing::protected_table_source::table_source(workspace_path, matter);
			// The prompt names each document by its handle ("DOC_3"); the source
			// reads Project document ids.
			std::map<std::string, std::string> documentOfHandle;
			for (const context::ResolvedSourceReference &reference : retrieval_markers.references())
				documentOfHandle.emplace(reference.document_token, reference.document_id);
			request.protected_table_source = [tableSource, documentOfHandle](const std::vector<std::string> &handles) {
				std::vector<std::string> ids;
				std::map<std::string, std::string> handleOfDocument;
				for (const std::string &handle : handles) {
					const auto found = documentOfHandle.find(handle);
					if (found == documentOfHandle.end()) continue;
					ids.push_back(found->second);
					handleOfDocument.emplace(found->second, handle);
				}
				// Each table under the handle the prompt names its document by, so
				// the evidence that reads its sheet describes it (the tables block).
				std::vector<protected_api::papinian::tables::TableInput> inputs = tableSource(ids);
				for (protected_api::papinian::tables::TableInput &input : inputs) {
					const auto handle = handleOfDocument.find(input.document_id);
					if (handle != handleOfDocument.end()) input.document_handle = handle->second;
				}
				return inputs;
			};
			// A chronology it sets out is still checked for its dates, once; a
			// statement is the answer contract's (statement_not_allowed).
			request.research_final_text_issue = [datedDocuments, isChronologyChecked = std::make_shared<bool>(false)](
					const std::string &text, const std::vector<research::ExternalEvidenceReference> &) {
				if (*isChronologyChecked == true) return std::optional<research::FinalTextIssue>();
				std::optional<research::FinalTextIssue> issue = undated_chronology_issue(text, *datedDocuments);
				if (issue) *isChronologyChecked = true;
				return issue;
			};
		} else if (computedStatementsOffered) {
			request.local_facts_from_final_text = documents::professional_response_local_facts;
			request.maximum_local_fact_rounds = kMaximumLocalFactRounds;
			// Once its statements are worked out, a chronology it sets out is
			// checked for its dates - once in the request: after that one
			// correction the answer is taken as it is (undated_chronology_issue).
			request.research_final_text_issue = [datedDocuments, isChronologyChecked = std::make_shared<bool>(false)](
					const std::string &text, const std::vector<research::ExternalEvidenceReference> &evidence) {
				std::optional<research::FinalTextIssue> issue = computed_statement_issue(text, evidence);
				if (issue || *isChronologyChecked == true) return issue;
				issue = undated_chronology_issue(text, *datedDocuments);
				if (issue) *isChronologyChecked = true;
				return issue;
			};
			// And before it answers, the facts its answer rests on: the first turn
			// asks for them alone (ResearchToolLoop's facts step), whatever the
			// request. Whether it has a protected amount or date at all is known
			// only once protection has read it - inside the protected call itself
			// (ProtectedInference), so asking first would cost a second protection
			// pass on every request to save a short turn on the few that have
			// none. A legal request over a Project almost always has dates.
			request.local_facts_step_contract = documents::local_facts_step_output_contract();
			request.local_facts_step_instruction = context::computed_statements::kFactsStepInstruction;
		}
		answerTextChecked = static_cast<bool>(request.research_final_text_issue);
		request.research_final_text_issue = with_claimed_change_check(std::move(request.research_final_text_issue));
		request.maximum_output_tokens = assembled.budget.output_tokens.value_or(professionalResponseMaxOutputTokens);
	}
	if (connectedDocumentAction.has_value()) {
		// A typed operation turn already carries its own complete, self-
		// sufficient protocol (TurnContract's control/data planes, exposed tool
		// schemas, forced tool_choice) and never returns the generic chat
		// [FINDING]/[OBLIGATION] freeform block syntax at all - that guidance
		// is real, necessary bytes for an ordinary chat response, but pure,
		// unconditional dead weight on every single operation turn (a real
		// design-partner acceptance run proved this concretely: this exact
		// suffix was a meaningful fraction of the fixed, per-turn "request"
		// tax that a real ~4096-token local model must pay before a single
		// operation turn can even be compiled). Trimmed only for this one
		// embedded copy, from its own well-known, stable start marker; the
		// canonical system prompt profile itself (SystemPromptProfiles.cpp,
		// shared by every non-operation caller) is never touched. Computed
		// here (never re-derived later) and carried on PreparedTaskRequest so
		// execute_task_attempt() only ever wires AdvancedWorkflow's real
		// choreography once it is actually the model's own declared choice -
		// see wire_connected_document_action_request()'s own comment.
		context::PromptAssemblyInput operation_input = prompt_input;
		// The first call's capability text (the professional_response schema
		// choices, its research wording) and candidate descriptions do not apply
		// to operation turns, which carry their own protocol.
		operation_input.capability_instructions.reset();
		operation_input.candidate_context.reset();
		operation_input.source_chunks.erase(std::remove_if(operation_input.source_chunks.begin(),
			operation_input.source_chunks.end(), [&](const context::PromptSourceMaterial &chunk) {
				return operation_targets_chunk_ids.count(chunk.id) != 0; }), operation_input.source_chunks.end());
		// The operation's turns carry this request data beside their own state,
		// tool schemas and output: with the executing model's capacity known it
		// gets half of that window - the share ConnectedDocumentAction's own
		// compaction decision leaves for everything but the document map - never
		// the whole window the first call was budgeted to.
		if (executingContextWindowTokens) {
			operation_input.maximum_context_tokens.reset();
			operation_input.estimate_tokens = nullptr;
			// The operation turn's own reservation, through the same shared policy
			// every other call uses - its declared ceiling is the whole window, so
			// what the policy leaves it is the share that policy owns.
			apply_capacity_first_budget(operation_input, executingContextWindowTokens,
				research::reserved_output_tokens_for_window(executingContextWindowTokens,
					*executingContextWindowTokens));
			operation_input.continuation_headroom_tokens = 0;
			operation_input.reserved_output_tokens = 0;
			operation_input.minimum_output_tokens = 0;
		}
		try {
			operationRequestData = context::PromptAssembler::assemble_with_budget(operation_input).model_visible_data;
		} catch (const context::ContextCapacityExceeded &error) {
			// Needed only if the model declares advanced_workflow: the request is
			// refused then, never merely because this projection exists.
			operationRequestData = nlohmann::json::object();
			operationCapacityRefusal = error.what();
		}
		if (operationRequestData.contains("system") && operationRequestData.at("system").contains("instruction")
				&& operationRequestData.at("system").at("instruction").is_string()) {
			std::string systemInstruction = operationRequestData.at("system").at("instruction").get<std::string>();
			const std::string findingMarker = "When the task calls for structured findings";
			const std::size_t cut = systemInstruction.find(findingMarker);
			if (cut != std::string::npos) {
				systemInstruction.resize(cut);
				while (!systemInstruction.empty()
						&& (systemInstruction.back() == '\n' || systemInstruction.back() == ' '))
					systemInstruction.pop_back();
				operationRequestData["system"]["instruction"] = std::move(systemInstruction);
			}
		}
	}
  }
  // Work kept on this computer runs on a local model and nothing else: never
  // the cloud model a Chat, a Project's AI Setup or a retry would choose.
  if (worksOnlyHere && (!spec.model_target || spec.model_target->provider_id != "local"))
    throw providers::TaskError(providers::TaskErrorCode::UnsupportedModel, kWorksOnlyHereModelRefused);
  request.model_target = spec.model_target;
  request.protected_api_policy_override = effective_policy;
	if (spec.operation_id.empty() == false) {
		tools::ToolExecutionContext toolContext;
		toolContext.project_id = matter.id;
		toolContext.chat_id = spec.chat_id;
		toolContext.operation_id = spec.operation_id;
		toolContext.permission = authorized_permission(matter, spec.operation_id);
		toolContext.project_sensitive_values = effective_policy.custom_terms;
		toolContext.research_backend = settings.local_node_setup.research_backend;
		toolContext.operation_is_current = [projectId = matter.id, operationId = spec.operation_id]() {
			return !tools::production_tool_broker().operation_invalidated(projectId, operationId);
		};
		// Work nobody watches (an AI colleague's) never holds its thread on the
		// lawyer: once its place is kept it returns at once and waits there, and
		// the decision resumes it (Beta 1 #14). A chat's wait goes on in this
		// process, where the lawyer is.
		toolContext.unattended = spec.unattended;
		if (spec.unattended) toolContext.lawyer_wait_limit = std::chrono::milliseconds(0);
		request.tool_execution_context = std::move(toolContext);
		// The model's own requests to read more of the Project are answered by
		// this operation's session and nothing else (project.read, a LocalRead).
		if (projectEvidence) {
			context::project_evidence::ensure_production_project_evidence_tools();
			request.tool_execution_context->project_evidence = [session = projectEvidence](const std::string &toolId,
					const std::string &arguments, std::size_t roomTokens) {
				return session->run(toolId, nlohmann::json::parse(arguments), roomTokens).dump();
			};
		}
		// A question the model puts to the lawyer waits in ToolBroker for the
		// answer, like an approval that always asks.
		if (lawyerQuestionOffered) tools::lawyer_question::ensure_production_lawyer_question_tool();
		// A change to a bound document is proposed in the response itself
		// (actionType "edit_document") and written only through ToolBroker, which
		// asks the person first whenever the Project's action mode says to. No
		// tool is ever offered that writes a document in the middle of a turn.
	}
	const auto entitySymbols = privacy::project_entity_symbols(matter);
	for (const privacy::ProjectEntity &entity : privacy::list_active_entities(matter)) {
		routing::ProjectEntityProtectionHint hint;
		hint.entity_id = entity.id;
		hint.category = entity.category;
		hint.local_values = privacy::protection_values(matter, entity);
		hint.curated_values = entity.locked_aliases;
		if (const auto symbol = entitySymbols.find(entity.id); symbol != entitySymbols.end()) hint.symbol = symbol->second;
		request.project_entity_hints.push_back(std::move(hint));
	}
	if (request.tool_execution_context.has_value()) {
		// What a research query may not carry, and how one party is written: the
		// values the Guard masks, judged as it judges them - a word an automatic
		// pass recorded by mistake ("license", "data room") is not cut from a query.
		for (const routing::ProjectEntityProtectionHint &hint : request.project_entity_hints) {
			std::vector<std::string> protectedValues;
			for (const std::string &value : hint.local_values)
				if (std::find(hint.curated_values.begin(), hint.curated_values.end(), value) != hint.curated_values.end()
						|| protected_api::papinian::recorded_value_names_identity(hint.category, value))
					protectedValues.push_back(value);
			for (const std::string &value : protectedValues)
				request.tool_execution_context->project_sensitive_values.push_back(value);
			// The same values, kept grouped by the identity they name, so a
			// change the model anchors on one writing of a party resolves
			// against the document's own writing of it.
			if (protectedValues.size() > 1)
				request.tool_execution_context->project_identity_writings.push_back(std::move(protectedValues));
		}
	}

  // A source reference is only ever valid against what this exact request
  // actually sent - not everything retrieved. PromptAssembler's budget can
  // drop a whole retrieved-source item; narrow the pre-budget marker
  // registry down to just the chunk IDs whose item actually survived, so a
  // budget-excluded chunk's marker can never validate even though it was
  // retrieved and assigned one above (Master Spec Sec.22).
  std::unordered_set<std::string> supplied_chunk_ids;
  for (const auto &item : assembled.budget.included) {
    if (item.category == "explicit-source" || item.category == "retrieved-source") {
      supplied_chunk_ids.insert(item.id);
    }
  }
	// What the request itself carried is never given again by inspection.
	if (projectEvidence) projectEvidence->mark_supplied(supplied_chunk_ids);
  std::set<std::string> included_context_item_ids;
  for (const context::PromptBudgetItem &item : assembled.budget.included) {
    if (item.category == "project-context" || item.category == "pinned-project-context") {
      included_context_item_ids.insert(item.id);
    }
  }

	// The relations the sent prompt carried, with what each rests on.
	std::vector<context::temporal_context::TemporalRelation> temporalRelations;
	{
		std::vector<context::temporal_context::DatedDocument> carried;
		for (const auto &dated : *datedDocuments) {
			const bool sent = std::any_of(document_context.begin(), document_context.end(), [&](const auto &source) {
				return source.document_id == dated.document.document_id && supplied_chunk_ids.count(source.chunk.chunk_id) > 0;
			});
			if (sent) carried.push_back(dated);
		}
		if (carried.size() >= 2) temporalRelations = context::temporal_context::derive(carried).relations;
	}

	std::optional<std::string> localResponse;
	if (intent.capability == professional::ProfessionalCapability::ProductState) {
		localResponse = spec.model_target
			? "This Chat is currently set to use " + spec.model_target->model_id
				+ " through " + spec.model_target->provider_id + "."
			: "No model is currently assigned to this Chat's primary result role.";
	}
	PreparedTaskRequest prepared{
      std::move(request),
      effective_policy,
      std::move(pickedDocumentIds),
      pickUsage,
      retrieval_markers.restricted_to(supplied_chunk_ids),
      std::move(distinct_context_document_ids),
      document_context.size(),
      context::snapshot_items_for_ai_assembly(matter, included_context_item_ids),
		  connectedDocumentAction,
		  std::move(operationRequestData),
		  directDocumentWork,
		  artifactCandidates,
		  intent,
		  std::move(localResponse),
		  !assembled.budget.excluded.empty(),
		  assembled.budget.coverage,
		  std::move(unavailable_source_documents),
		  prompt_input,
		  document_context,
		  supplied_chunk_ids,
		  std::move(operationCapacityRefusal),
		  std::move(researchUnavailableReason),
		  std::move(projectEvidence),
		  std::move(capabilityWithoutInspection),
		  {},
		  std::move(temporalRelations),
  };
	prepared.computed_statements_offered = computedStatementsOffered;
	prepared.compute_protocol_offered = computeProtocolOffered;
	prepared.answer_text_checked = answerTextChecked;
	return prepared;
}

// Phase 09's source-sensitive AI entry version guard. `expectedDocumentVersions`
// (an optional params array of {documentId, expectedVersion}) names each
// Document a caller's selection (e.g. Documents' Compare/Summarize/Risk
// Review/Extract obligations) was made against. Before context assembly ever
// reads a named Document's cached extraction, this re-runs the same real,
// targeted Phase 08 reconciliation Document/DocumentFileActions.hpp's
// destructive actions already revalidate through
// (documents::registry::reconcile_affected_paths() - the pure, in-place,
// non-persisting registry call every matter::detect_affected_paths() wraps;
// used directly here, never matter::detect_affected_paths() itself, so this
// guard has no persistence side effect and stays safe to reuse from
// preview_outbound_request(), which must never persist anything) for exactly
// that Document's last-known path, then requires it to still be available
// and at the exact version the caller observed - never silently substituting
// a document that changed underneath a stale selection. Mutates `matter` in
// place to the freshly reconciled state so task_attempt_spec_from_params()/
// prepare_task_request() downstream see the same truth this check just
// verified, never a separately-read snapshot; the caller's own existing
// save (run_task()'s final matters::save_matters(), never called at all by
// preview_outbound_request()) persists it exactly as it already persists
// refresh_matter_documents()'s changes - no new persistence path. A caller
// that supplies no expectedDocumentVersions (every request that is not a
// Documents-triggered selection today) is entirely unaffected - this reuses
// Phase 08's existing identity/version/hash truth, never a second
// provenance system, and a Managed document (no real Project-folder path to
// reconcile) is accepted as-is, matching every other Phase 09 action's
// Referenced-only scope.
void validate_expected_document_versions(const Json& params, matters::Matter& matter) {
	if (!params.contains("expectedDocumentVersions")) return;
	if (!params.at("expectedDocumentVersions").is_array()) {
		throw std::runtime_error("Expected document versions must be an array.");
	}
	for (const auto& entry : params.at("expectedDocumentVersions")) {
		if (!entry.is_object() || !entry.contains("documentId") || !entry.at("documentId").is_string() ||
				!entry.contains("expectedVersion") || !entry.at("expectedVersion").is_number_integer()) {
			throw std::runtime_error("Each expected document version needs a documentId and an integer expectedVersion.");
		}
		const auto document_id = entry.at("documentId").get<std::string>();
		const auto expected_version = entry.at("expectedVersion").get<int>();

		const auto found_document = std::find_if(matter.documents.begin(), matter.documents.end(),
				[&](const matters::MatterDocument& candidate) { return candidate.id == document_id; });
		if (found_document == matter.documents.end()) {
			throw std::runtime_error("This document changed since you selected it.");
		}
		// Managed (Phase 03 copy-in) documents have no real Project-folder path
		// and no filesystem-watcher staleness window to revalidate against.
		if (found_document->source_mode != matters::DocumentSourceMode::Referenced) continue;

		if (!matter.directory_path || matter.directory_path->empty() ||
				documents::registry::is_relative_path_safe(*matter.directory_path, found_document->relative_path) == false) {
			throw std::runtime_error("This document's location could not be safely resolved.");
		}
		const std::filesystem::path absolute_path =
				std::filesystem::path(*matter.directory_path) / found_document->relative_path;
		// Called for its in-place mutation of matter.documents; the outcome
		// itself is re-derived below via a fresh lookup by document_id.
		(void)documents::registry::reconcile_affected_paths(matter, {absolute_path.string()});

		const auto revalidated = std::find_if(matter.documents.begin(), matter.documents.end(),
				[&](const matters::MatterDocument& candidate) { return candidate.id == document_id; });
		if (revalidated == matter.documents.end() || revalidated->available == false ||
				revalidated->version != expected_version) {
			throw std::runtime_error("This document changed since you selected it.");
		}
	}
}

// "What leaves this device?" (Phase 11) needs to attribute each protected
// value to the real local material this exact request drew from - never a
// guess. Built from the same document_context retrieve_document_context()
// already gives prepare_task_request(), read again here rather than
// threaded through PreparedTaskRequest, so this stays a read-only, additive
// concern next to the unchanged real send/preview pipeline.
protected_api::papinian::OriginSources
origin_sources_for(const std::filesystem::path &workspace_path, const matters::Matter &matter,
                   const TaskAttemptSpec &spec) {
  protected_api::papinian::OriginSources sources;
  sources.raw_request_text = spec.prompt;
  for (const auto &source : matters::retrieve_document_context(workspace_path, matter, spec.prompt,
           spec.selected_document_ids, nullptr, matters::SelectedSourceDelivery::EveryPassage)) {
    sources.documents.push_back({source.document_name, source.chunk.text});
  }
  return sources;
}

// Node default AI Setup, expressed the way Routing/AiSetup/AiSetup.hpp's
// resolver expects - the same conversion Bridge/RoutingBridge/RoutingBridge.cpp
// uses for getEffectiveAiSetup, so "effective" means the identical thing in
// both places. role_assignments is the single canonical store for every
// Beta 1 role including Reasoning (see node_default_reasoning_target()'s own
// doc comment).
routing::AiSetupOverrideSet node_default_override_set(const platform::LocalNodeSetup &setup) {
  routing::AiSetupOverrideSet overrides;
  for (const auto &assignment : setup.role_assignments) {
    if (const auto role = routing::ai_setup_role_from_string(assignment.role)) {
      overrides.push_back(
          routing::RoleAssignment{*role, routing::ModelTarget{assignment.provider_id, assignment.model_id}});
    }
  }
  return overrides;
}

routing::AiSetupOverrideSet override_set_from(const std::vector<matters::AiSetupRoleOverride> &entries) {
  routing::AiSetupOverrideSet overrides;
  for (const auto &entry : entries) {
    if (const auto role = routing::ai_setup_role_from_string(entry.role)) {
      overrides.push_back(routing::RoleAssignment{*role, routing::ModelTarget{entry.provider_id, entry.model_id}});
    }
  }
  return overrides;
}

// The canonical Node -> Project -> Chat effective Reasoning resolution
// (Routing/AiSetup/AiSetup.hpp's resolve_role(), AI_SETUP_CONTRACT.md's
// "Inheritance") - the same authoritative resolution getEffectiveAiSetup
// shows the user, never Node-default-only. Preview and reveal both call this
// (see build_state_fingerprint()'s callers below), so a Project (or Chat, for
// a matter.previewOutboundRequest call naming one) AI Setup override is
// honestly reflected on both sides of the freshness check instead of only
// ever comparing against the Node default. Operation-level override (a
// one-time explicit providerId/modelId on this one request) is deliberately
// out of scope here: matter.previewOutboundRequest's own params never carry
// one today, so there is nothing for reveal to reproduce even if this
// function reported it.
routing::ModelTarget effective_reasoning_target(const platform::LocalNodeSetup &setup, const matters::Matter &matter,
                                                const std::optional<std::string> &chat_id) {
  const auto node_default = node_default_override_set(setup);
  const auto project_override = override_set_from(matter.ai_setup_overrides);
  routing::AiSetupOverrideSet chat_override;
  if (chat_id) {
    const auto chat = std::find_if(matter.chats.begin(), matter.chats.end(),
                                   [&](const matters::MatterChat &entry) { return entry.id == *chat_id; });
    if (chat != matter.chats.end()) chat_override = override_set_from(chat->ai_setup_overrides);
  }
  const routing::AiSetupOverrideSet no_operation_override;
  const auto resolved = routing::resolve_role(routing::AiSetupRole::Reasoning, node_default, project_override,
                                              chat_override, no_operation_override);
  return resolved.model.value_or(routing::ModelTarget{});
}

// The ephemeral preview session's binding to "is anything relevant still
// exactly as it was" (Phase 11 trust boundary, PapinianGuard/PreviewSession):
// the effective policy, the effective Reasoning provider/model (Node ->
// Project -> Chat, never Node-default-only - see effective_reasoning_target()
// above), and the version of every document the preview's context actually
// drew from. Computed identically at preview time (from the real, just-
// resolved state) and at reveal time (from a fresh reload) -
// PreviewSessionStore::look_up() only ever compares the two structurally,
// never inspecting what changed.
protected_api::papinian::PreviewSessionFingerprint
build_state_fingerprint(const protected_api::Policy &policy, const std::string &provider_id,
                        const std::string &model_id, const matters::Matter &matter,
                        const std::unordered_set<std::string> &document_ids) {
  protected_api::papinian::PreviewSessionFingerprint fingerprint;
  fingerprint.policy_mode = protected_api::to_string(policy.mode);
  for (const auto category : protected_api::effective_categories(policy)) {
    fingerprint.policy_categories.push_back(protected_api::to_string(category));
  }
  std::sort(fingerprint.policy_categories.begin(), fingerprint.policy_categories.end());
  fingerprint.policy_custom_terms = policy.custom_terms;
  std::sort(fingerprint.policy_custom_terms.begin(), fingerprint.policy_custom_terms.end());
  fingerprint.provider_id = provider_id;
  fingerprint.model_id = model_id;
  for (const auto &document_id : document_ids) {
    const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
                                       [&](const matters::MatterDocument &candidate) {
                                         return candidate.id == document_id;
                                       });
    // A document removed from the Matter entirely can never again match a
    // real version number, so it correctly makes the fingerprint stale
    // rather than being silently dropped from the comparison.
    fingerprint.document_versions[document_id] = document != matter.documents.end() ? document->version : -1;
  }
  return fingerprint;
}

// A change the lawyer was shown and that was not written - the last turn of the
// Chat, declined or stopped before it was approved - which the new instruction
// only confirms ("Go ahead and create the amendment."): the same proposal is
// put to them again for approval, with no model call, instead of being drafted
// anew. Only while nothing it rests on changed: the document it changes, and
// every passage it was prepared from (restore_supplied_evidence()), exactly as
// a resumed approval requires. Anything else - new words in the instruction, a
// changed file, a proposal still awaiting its decision - is drafted anew.
struct ReofferedProposal {
	matters::MatterTask task;
	Json draft;
};

std::optional<ReofferedProposal> proposal_to_offer_again(const std::filesystem::path &workspace_path,
		const matters::Matter &matter, const TaskAttemptSpec &spec) {
	if (spec.operation_id.empty() || !spec.chat_id || spec.root_task_id || spec.attempt != 1) return std::nullopt;
	const matters::MatterTask *previous = nullptr;
	for (const matters::MatterTask &task : matter.tasks) {
		// This request's own record, saved when it was sent: a resume of it is
		// never a confirmation of the turn before.
		if (task.agent_operation_id == spec.operation_id) {
			if (task.document_operation_checkpoint) return std::nullopt;
			continue;
		}
		if (task.chat_id != spec.chat_id) continue;
		if (previous == nullptr || task.created_at >= previous->created_at) previous = &task;
	}
	if (previous == nullptr || !previous->document_operation_checkpoint || previous->artifact_document_id)
		return std::nullopt;
	const documents::DocumentOperationCheckpoint &checkpoint = *previous->document_operation_checkpoint;
	const Json &draft = checkpoint.draft;
	if (draft.value("kind", std::string()) != "professional_artifact_action" || !draft.contains("rawProviderResult")
			|| !draft.at("rawProviderResult").is_string() || !draft.contains("approval") || !draft.at("approval").is_object()
			|| (checkpoint.mutation_commit.is_object() && !checkpoint.mutation_commit.empty()))
		return std::nullopt;
	// Declined, or stopped while it waited: the decision is the lawyer's, and a
	// new instruction is how they change it. One still waiting is answered on its
	// own card; one approved and not written failed for a reason of its own.
	const Json &binding = draft.at("approval");
	const std::string decision = binding.value("decision", std::string("pending"));
	if (decision != "rejected" && decision != "withdrawn") return std::nullopt;

	/* - - - - - - */

	std::string proposalWords = previous->prompt + "\n" + binding.value("summary", std::string()) + "\n"
		+ binding.value("newDocument", std::string());
	const Json raw = Json::parse(draft.at("rawProviderResult").get<std::string>(), nullptr, false);
	if (raw.is_object() && raw.contains("artifactAction") && raw.at("artifactAction").is_object())
		for (const char *field : {"title", "summary", "documentKind", "subject"}) {
			const Json &action = raw.at("artifactAction");
			if (action.contains(field) && action.at(field).is_string()) proposalWords += "\n" + action.at(field).get<std::string>();
		}
	// Where the change goes, read from the Project as it is now.
	ProposalDestination destination;
	const std::string targetDocumentId = binding.contains("targetDocumentId") && binding.at("targetDocumentId").is_string()
		? binding.at("targetDocumentId").get<std::string>() : std::string();
	destination.new_document_name = binding.value("newDocument", std::string());
	destination.creates_new_document = targetDocumentId.empty();
	if (raw.is_object() && raw.contains("artifactAction") && raw.at("artifactAction").is_object()) {
		const Json &action = raw.at("artifactAction");
		for (const char *field : {"title", "documentKind"})
			if (action.contains(field) && action.at(field).is_string())
				destination.new_document_name += "\n" + action.at(field).get<std::string>();
	}
	for (const matters::MatterDocument &document : matter.documents) {
		const std::string name = document.name + "\n" + std::filesystem::path(document.relative_path).stem().string();
		if (document.id == targetDocumentId) destination.target_document_name = name;
		else destination.other_document_names.push_back(name);
	}
	if (!only_confirms_proposal(spec.prompt, proposalWords, destination)) return std::nullopt;

	/* - - - - - - */

	// The document the change was shown against is still that document.
	const std::optional<std::string> targetId = binding.contains("targetDocumentId")
		&& binding.at("targetDocumentId").is_string()
		? std::optional<std::string>(binding.at("targetDocumentId").get<std::string>()) : std::nullopt;
	if (spec.target_document_id && spec.target_document_id != targetId) return std::nullopt;
	if (targetId) {
		const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &entry) { return entry.id == *targetId; });
		if (document == matter.documents.end() || document->available == false) return std::nullopt;
		if (binding.contains("targetFingerprint") && binding.at("targetFingerprint").is_string()
				&& binding.at("targetFingerprint").get<std::string>() != document->fingerprint)
			return std::nullopt;
		if (binding.contains("targetVersion") && binding.at("targetVersion").is_number_integer()
				&& binding.at("targetVersion").get<int>() != document->version)
			return std::nullopt;
	}
	// And every passage it was prepared from still reads as it did, word for
	// word - the test restore_supplied_evidence() applies, asked in advance.
	if (draft.contains("suppliedEvidence") && draft.at("suppliedEvidence").is_array()) {
		std::unordered_map<std::string, std::optional<std::vector<context::RetrievedContextChunk>>> read;
		for (const Json &entry : draft.at("suppliedEvidence")) {
			const std::string documentId = entry.value("documentId", std::string());
			const std::string chunkId = entry.value("chunkId", std::string());
			const std::string textHash = entry.value("textHash", std::string());
			if (documentId.empty() || chunkId.empty() || (targetId && *targetId == documentId)) continue;
			if (read.count(documentId) == 0) {
				try {
					read[documentId] = matters::retrieve_document_context(workspace_path, matter, std::string(),
						{documentId}, nullptr, matters::SelectedSourceDelivery::EveryPassage);
				} catch (const std::exception &) {
					read[documentId] = std::nullopt;
				}
			}
			const auto &passages = read.at(documentId);
			if (!passages) return std::nullopt;
			const bool isStill = std::any_of(passages->begin(), passages->end(),
				[&](const context::RetrievedContextChunk &passage) {
					return passage.chunk.chunk_id == chunkId
						|| (!textHash.empty() && supplied_passage_text_hash(passage.chunk.text) == textHash);
				});
			if (!isStill) return std::nullopt;
		}
	}
	return ReofferedProposal{*previous, draft};
}

matters::MatterTask execute_task_attempt(const std::filesystem::path &workspace_path,
                                         const std::string &matter_id,
                                         const matters::Matter &matter,
                                         const TaskAttemptSpec &spec) {
  // Phase 11 "on send" cleanup: a real request is about to leave for this
  // matter (regardless of whether it ultimately succeeds), so no outstanding
  // "What leaves this device?" preview for it is still describing a pending,
  // un-sent request - see PreviewSessionStore::invalidate_for_matter()'s own
  // doc comment. This is the one place every real send (matter.runTask,
  // retryTask, runConsilium, runWorkflow) passes through, so nothing else
  // needs its own explicit call for this.
  protected_api::papinian::production_preview_session_store().invalidate_for_matter(matter_id);

	// D-012 on every path into a model, not only a Chat's own (#229): a request
	// in a chat that holds work kept on this computer stays here, as the Chat
	// keeps its own requests (ChatExecution keep_on_this_computer). The Native
	// API's matter.runTask, matter.runConsilium and a synthesis given such a
	// chat reach this without passing the Chat.
	const routing::ScopedLocalOnlyWork keptByItsChat(spec.chat_id.has_value()
		&& chat::chat_holds_work_kept_here(matter, *spec.chat_id));
	// Work kept here never names a model outside this computer: refused before
	// anything is prepared or sent, as Retry and Run with another model refuse
	// it (keep_attempt_here). The model a request has no name for is the
	// executor's to keep local.
	if (routing::ScopedLocalOnlyWork::is_active() && spec.model_target && spec.model_target->provider_id != "local")
		throw providers::TaskError(providers::TaskErrorCode::UnsupportedModel, kWorksOnlyHereModelRefused);

	// An operation resumed after its first attempt picked its documents reads
	// the ones recorded on its checkpoint (record_supplied_evidence()): a resume
	// - an approval above all - never pays for a second pick. A checkpoint from
	// before the pick was recorded resumes as it always did.
	TaskAttemptSpec attemptSpec = spec;
	if (!spec.operation_id.empty())
		for (const matters::MatterTask &entry : matter.tasks) {
			if (entry.agent_operation_id != spec.operation_id || !entry.document_operation_checkpoint
					|| entry.operation_execution_state == "completed" || entry.operation_execution_state == "failed"
					|| entry.operation_execution_state == "stopped")
				continue;
			const Json &draft = entry.document_operation_checkpoint->draft;
			if (!draft.contains("pickedDocumentIds") || !draft.at("pickedDocumentIds").is_array()) continue;
			std::vector<std::string> picked;
			for (const Json &documentId : draft.at("pickedDocumentIds"))
				if (documentId.is_string()) picked.push_back(documentId.get<std::string>());
			attemptSpec.recorded_pick = std::move(picked);
			break;
		}
	// A run that waited on the lawyer when Singularity closed resumes from the place
	// it kept (research::ResearchPause), and reads the documents its pick chose
	// then: the passages the model already read keep their markers, and the pick
	// is not paid for twice.
	std::optional<Json> decidedPause;
	if (!spec.operation_id.empty() && research::research_pause_store().load) {
		decidedPause = research::research_pause_store().load(matter_id, spec.operation_id);
		if (decidedPause && !research::is_decided_pause(*decidedPause)) decidedPause.reset();
	}
	const Json pausedAttempt = decidedPause && decidedPause->contains("attempt") && decidedPause->at("attempt").is_object()
		? decidedPause->at("attempt") : Json::object();
	if (!attemptSpec.recorded_pick && pausedAttempt.contains("pickedDocumentIds")
			&& pausedAttempt.at("pickedDocumentIds").is_array()) {
		std::vector<std::string> picked;
		for (const Json &documentId : pausedAttempt.at("pickedDocumentIds"))
			if (documentId.is_string()) picked.push_back(documentId.get<std::string>());
		attemptSpec.recorded_pick = std::move(picked);
	}
	// "Go ahead and create it" after a change the lawyer was shown: that change
	// is put to them again. It is prepared exactly as the request that made it
	// was - its words, its selection and its pick - so the documents it names by
	// handle are the same documents, and nothing is picked again.
	const std::optional<ReofferedProposal> reoffer = proposal_to_offer_again(workspace_path, matter, spec);
	if (reoffer) {
		attemptSpec.prompt = reoffer->task.prompt;
		attemptSpec.selected_document_ids = reoffer->task.selected_document_ids;
		std::vector<std::string> picked;
		if (reoffer->draft.contains("pickedDocumentIds") && reoffer->draft.at("pickedDocumentIds").is_array())
			for (const Json &documentId : reoffer->draft.at("pickedDocumentIds"))
				if (documentId.is_string()) picked.push_back(documentId.get<std::string>());
		attemptSpec.recorded_pick = std::move(picked);
	}
	// The attempt's clock starts before its documents are chosen: the choice - a
	// model call, where a pick is worth one - is part of the lawyer's wait, and a
	// latency that left it out read 33 s for a 43 s answer (24 Sep 2026).
	const auto attempt_started_at = std::chrono::steady_clock::now();
	auto prepared = prepare_task_request_within_capacity(workspace_path, matter, attemptSpec);
	// The request's own passages, in the order its source markers were numbered
	// from, by document and words - never their text.
	Json requestPassages = Json::array();
	for (const context::RetrievedContextChunk &passage : prepared.document_context)
		requestPassages.push_back({{"documentId", passage.document_id},
			{"textHash", context::project_evidence::detail::passage_text_hash(passage.chunk.text)}});
	// Kept with any pause this attempt's loop makes (research::ResearchPause).
	research::set_research_attempt_facts(matter_id, spec.operation_id,
		Json{{"pickedDocumentIds", prepared.picked_document_ids}, {"requestPassages", requestPassages}});
	// A paused prompt carries the words of the passages it was prepared from,
	// under markers numbered from them. Prepared again after the restart, the
	// request must carry the same passages in the same order, or a marker the
	// model reads in the saved prompt names another passage now - so it is
	// taken up only when every one is still there word for word (a pause that
	// does not say which it was prepared from cannot be checked). The passages
	// the paused attempt's inspection found are read again and numbered where
	// they were, under the same rule. Otherwise the place cannot be taken up
	// faithfully: the pause keeps only its decision and its bill, and the
	// request runs from its start on the documents as they are now.
	if (decidedPause && decidedPause->contains("loop")) {
		bool isFaithful = pausedAttempt.contains("requestPassages") && pausedAttempt.at("requestPassages") == requestPassages;
		if (isFaithful == true && pausedAttempt.contains("projectEvidence")) {
			isFaithful = prepared.project_evidence
				&& prepared.project_evidence->restore(pausedAttempt.at("projectEvidence"));
			if (isFaithful == true)
				research::merge_research_attempt_facts(matter_id, spec.operation_id,
					Json{{"projectEvidence", prepared.project_evidence->kept()}});
		}
		if (isFaithful == false) {
			Json unusable = *decidedPause;
			unusable.erase("loop");
			unusable.erase("result");
			research::research_pause_store().save(matter_id, spec.operation_id, unusable);
		}
	}
	// One protection identity for the whole attempt: its draft, the review of
	// it, the corrections and every loop turn write one value with one symbol,
	// so what a later call reads back from an earlier one is masked as it was.
	prepared.request.operation_symbols = std::make_shared<protected_api::papinian::OperationProtectedSymbolTable>();
	// The matter's observation ledger (DESIGN4 §8.3, §4.5): at most four charged
	// observations of any hidden fact for the life of the matter, not of one
	// operation. Read into the operation's table here; what the operation
	// charged is merged back into the Matter when it ends, however it ends.
	{
		protected_api::papinian::compute::ObservationCounts &counts = prepared.request.operation_symbols->observations.matter;
		const matters::PersistedObservationLedger &kept = matter.observation_ledger;
		counts.by_root = kept.by_root;
		counts.by_calendar_root = kept.by_calendar_root;
		counts.by_group_key = kept.by_group_key;
		counts.answered.insert(kept.answered.begin(), kept.answered.end());
		counts.zero_divisors.insert(kept.zero_divisors.begin(), kept.zero_divisors.end());
		counts.preload_by_root = kept.preload_by_root;
		counts.order_edges.insert(kept.order_edges.begin(), kept.order_edges.end());
	}
	struct ObservationLedgerKeeper {
		std::filesystem::path workspace;
		std::string matter_id;
		std::shared_ptr<protected_api::papinian::OperationProtectedSymbolTable> table;
		~ObservationLedgerKeeper() {
			if (!table) return;
			try {
				const protected_api::papinian::compute::ObservationCounts &counts = table->observations.matter;
				if (counts.by_root.empty() && counts.by_calendar_root.empty() && counts.by_group_key.empty()
						&& counts.answered.empty() && counts.zero_divisors.empty() && counts.order_edges.empty())
					return;
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				auto found = matters::load_matter(workspace, matter_id);
				if (!found) return;
				matters::PersistedObservationLedger &ledger = found->observation_ledger;
				// Counts only grow: a charge another operation of the matter made
				// meanwhile is kept, never undone.
				const auto merge = [](std::map<std::string, std::size_t> &into, const std::map<std::string, std::size_t> &from) {
					for (const auto &[key, count] : from) into[key] = std::max(into[key], count);
				};
				merge(ledger.by_root, counts.by_root);
				merge(ledger.by_calendar_root, counts.by_calendar_root);
				merge(ledger.by_group_key, counts.by_group_key);
				std::set<std::string> answered(ledger.answered.begin(), ledger.answered.end());
				answered.insert(counts.answered.begin(), counts.answered.end());
				ledger.answered.assign(answered.begin(), answered.end());
				std::set<std::string> divisors(ledger.zero_divisors.begin(), ledger.zero_divisors.end());
				divisors.insert(counts.zero_divisors.begin(), counts.zero_divisors.end());
				ledger.zero_divisors.assign(divisors.begin(), divisors.end());
				merge(ledger.preload_by_root, counts.preload_by_root);
				std::set<std::string> edges(ledger.order_edges.begin(), ledger.order_edges.end());
				edges.insert(counts.order_edges.begin(), counts.order_edges.end());
				ledger.order_edges.assign(edges.begin(), edges.end());
				matters::save_matter(workspace, *found);
			} catch (...) {
				// The ledger is kept on the next operation's end; never a reason
				// for this one to fail.
			}
		}
	};
	const ObservationLedgerKeeper observationLedgerKeeper{workspace_path, matter_id,
		std::static_pointer_cast<protected_api::papinian::OperationProtectedSymbolTable>(prepared.request.operation_symbols)};
	// And one record of the facts Singularity works out in it: every call of the
	// attempt that answers some adds them, and its review, redraft and targeted
	// completion read them (local_facts_for_later_call). Process memory only.
	const auto operationLocalFacts = std::make_shared<std::string>();
	// Under the protocol every later call of the attempt reads the task's
	// derived facts in its symbol table, from the operation table itself
	// (DESIGN4 §1.6, §4.5): no prose copy of them is kept.
	if (!prepared.compute_protocol_offered)
		prepared.request.local_facts_answered = [operationLocalFacts](const std::string &answers) {
			*operationLocalFacts += answers;
		};

	matters::MatterTask task;
	const auto resumable = std::find_if(matter.tasks.begin(), matter.tasks.end(),
		[&](const matters::MatterTask &entry) {
			return !spec.operation_id.empty() && entry.agent_operation_id == spec.operation_id
				&& entry.operation_execution_state != "completed"
				&& entry.operation_execution_state != "failed"
				&& entry.operation_execution_state != "stopped";
		});
	const bool resumedFromCheckpoint = resumable != matter.tasks.end();
	if (resumedFromCheckpoint) task = *resumable;
	else {
		task.id = matters::generate_task_id();
		task.created_at = workspaces::current_timestamp();
	}
	task.operation_type = "matter_task";
	task.operation_role = spec.operation_role.value_or("reasoning");
	if (!spec.operation_id.empty()) task.agent_operation_id = spec.operation_id;
	task.operation_execution_state = "working";
	task.operation_stage = "model";
	task.operation_updated_at = workspaces::current_timestamp();
	if (spec.model_target) {
		task.requested_provider_id = spec.model_target->provider_id;
		task.requested_model_id = spec.model_target->model_id;
		task.ai_setup_source = spec.ai_setup_source.value_or("operationOverride");
	}
  task.chat_id = spec.chat_id;
  task.prompt = spec.prompt;
  // Who asked, and the agent's part in it, are facts of the operation from
  // its first attempt: a resume never rewrites them - a resumed agent run
  // stays the agent's, never the person's who pressed Resume.
  const bool attributed = task.agent_initiated || !task.agent_id.empty() || !task.author_actor_id.empty()
    || !task.author_name.empty();
  if (!attributed) {
    task.agent_id = spec.agent_id;
    task.agent_initiated = spec.agent_initiated;
    task.answers_message_id = spec.answers_message_id;
    // Who asked: the agent itself for a run it began, otherwise the person.
    if (spec.agent_initiated == true) {
      task.author_name = spec.agent_name;
    } else if (spec.author_actor_id.empty() == false) {
      task.author_actor_id = spec.author_actor_id;
      task.author_name = spec.author_name;
    } else {
      const platform::ActorIdentity identity = platform::resolve_actor_identity();
      task.author_actor_id = identity.actor_id;
      task.author_name = identity.display_name.value_or(std::string());
    }
  }
  task.legal_task_preset_id = spec.legal_task_preset_id;
  task.legal_task_preset_version = spec.legal_task_preset_version;
  task.prompt_profile_id = spec.prompt_profile_id;
  task.prompt_profile_version = spec.prompt_profile_version;
  task.selected_document_ids = spec.selected_document_ids;
	// An operation this attempt begins is signed with its origin, as one a Chat
	// begins is (Chat/OperationOrigin): what this computer recorded for it or,
	// where it recorded nothing, begun here by its person and asking first.
	if (!resumedFromCheckpoint && !task.origin) {
		const std::optional<tools::OperationAuthority> recorded = tools::operation_authority(matter.id, spec.operation_id);
		tools::OperationAuthority authority = recorded.value_or(tools::OperationAuthority{});
		if (!recorded) {
			authority.actor_id = platform::resolve_actor_identity().actor_id;
			authority.ceiling = tools::ProjectActionMode::AskForApproval;
			if (spec.agent_initiated || !spec.agent_id.empty()) {
				authority.origin = tools::OperationOrigin::Agent;
				authority.agent_id = spec.agent_id;
			}
		}
		authority.local_only = authority.local_only || routing::ScopedLocalOnlyWork::is_active()
			|| operation_works_only_here(matter, spec.operation_id);
		chat::sign_operation_origin(matter.id, task, authority);
	}
	// The request's explicit target is a fact of the request, recorded before
	// any pause so a resume, retry or edit binds the same target.
	if (spec.target_document_id) task.professional_target_document_id = spec.target_document_id;
	// Pre-generation: only structural facts Singularity actually resolved ahead of
	// time. Which action (if any) this operation performs, its real target
	// and destination are the MODEL's own post-generation declaration - see
	// where task.professional_write_intent/professional_target_document_id/
	// professional_destination_* are set for real further below, once the
	// unified response is parsed and validated.
	task.professional_capability = professional::to_string(prepared.professional_intent.capability);
	task.professional_operations.clear();
	task.professional_source_document_ids.clear();
	for (const professional::SourceReference &source : prepared.professional_intent.sources)
		task.professional_source_document_ids.push_back(source.document_id);
	task.professional_write_intent = false;
  task.attempt = spec.attempt;
  task.root_task_id = spec.root_task_id;
  // Captured unconditionally (including as an explicit null for "the Matter
  // had no instructions") so a later retry of *this* task always has a real
  // snapshot to reuse - see MatterTask::matter_instructions_snapshot_captured.
  task.matter_instructions_snapshot = spec.matter_instructions;
  task.matter_instructions_snapshot_captured = true;
  // Reflects what this attempt actually ran under (effective_policy.mode),
  // not necessarily the current live policy - matters for a retried
  // attempt, where they can differ.
  task.protected_api_mode = protected_api::to_string(prepared.effective_policy.mode);
  task.privacy_metadata.mode = task.protected_api_mode;
	task.context_snapshot = prepared.context_snapshot;
	task.temporal_relations = prepared.temporal_relations;
	task.context_budget_excluded_material = prepared.context_budget_excluded_material;
	task.request_coverage = recorded_coverage(prepared.request_coverage);
	task.unavailable_source_documents = prepared.unavailable_source_documents;
	task.research_mode = research::to_string(prepared.request.research_mode);
	// The lawyer's own "show me first" in this request - its instruction, or an
	// answer they typed to one of its questions - holds every change the
	// operation makes for their approval, whatever the Project's action mode and
	// whatever the model made of it. Held for this attempt only.
	std::optional<ChangesHeldForLawyer> changesHeld;
	if (prepared.request.tool_execution_context) {
		changesHeld.emplace(prepared.request.tool_execution_context->project_id,
			prepared.request.tool_execution_context->operation_id);
		if (lawyer_asks_to_see_changes_first(lawyer_words(task))) changesHeld->hold();
	}
	if (prepared.request.tool_execution_context) {
		prepared.request.tool_execution_context->approval_requested =
			[workspace_path, matter_id, &task, operationId = spec.operation_id,
					symbols = prepared.request.operation_symbols](const std::string &saved) {
				const Json value = Json::parse(saved);
				// Which protection the calls before this wait ran under, and the
				// random scope their markers were numbered in - never a value or
				// what a marker stood for - kept with any pause the loop makes.
				research::merge_research_attempt_facts(matter_id, operationId, Json{{"protection",
					Json{{"mode", task.protected_api_mode},
						{"symbolScope", symbols ? symbols->outbound_scope : std::string()}}}});
				// A question to the lawyer is not a change waiting for approval: it is
				// kept on the task as the lawyer is shown it, and the operation waits
				// for the answer. The loop keeps its place before it waits
				// (research::ResearchPause), so after a restart the same question is
				// answered and the run goes on from there.
				if (value.value("toolId", std::string()) == tools::lawyer_question::kToolId) {
					const Json question = value.contains("question") && value.at("question").is_object()
						? value.at("question") : Json::object();
					matters::LawyerQuestionRecord record;
					record.id = value.at("approvalId").get<std::string>();
					if (question.contains("title") && question.at("title").is_string())
						record.title = question.at("title").get<std::string>();
					if (question.contains("questions") && question.at("questions").is_array())
						record.questions = question.at("questions");
					record.asked_at = workspaces::current_timestamp();
					task.lawyer_questions.push_back(std::move(record));
					const persistence::PersistenceLock lock(persistence::persistence_mutex());
					catch_up_to_saved_revision(workspace_path, matter_id, task);
					// A question is not an approval: one granted earlier in this run is over.
					task.operation_approval.reset();
					++task.operation_revision;
					task.operation_execution_state = "waiting_for_answer";
					task.operation_stage = "question";
					task.operation_updated_at = task.lawyer_questions.back().asked_at;
					task.operation_activity = "Singularity asked you a question.";
					task.operation_resume_allowed = false;
					append_operation_event(task, "question.asked", "Singularity asked you a question.",
						task.lawyer_questions.back().id);
					task.operation_events.back().created_at = task.operation_updated_at;
					persist_task_checkpoint(workspace_path, matter_id, task);
					return;
				}
				const auto effect = tools::tool_effect_class_from_string(
					value.value("effect", std::string()));
				if (!effect) throw std::runtime_error("The tool approval request is invalid.");
				tools::ToolApprovalRequest approval;
				approval.approval_id = value.at("approvalId").get<std::string>();
				approval.operation_id = value.at("operationId").get<std::string>();
				approval.project_id = value.at("projectId").get<std::string>();
				approval.summary = value.at("summary").get<std::string>();
				approval.effect = *effect;
				approval.tool_id = value.value("toolId", std::string());
				approval.intent_digest = value.value("intentDigest", std::string());
				if (value.contains("targetDocumentId") && value.at("targetDocumentId").is_string())
					approval.target_document_id = value.at("targetDocumentId").get<std::string>();
				if (value.contains("targetVersion") && value.at("targetVersion").is_number_integer())
					approval.target_version = value.at("targetVersion").get<int>();
				if (value.contains("targetFingerprint") && value.at("targetFingerprint").is_string())
					approval.target_fingerprint = value.at("targetFingerprint").get<std::string>();
				approval.change_kind = value.value("changeKind", std::string());
				approval.removed_text = value.value("removedText", std::string());
				approval.added_text = value.value("addedText", std::string());
				if (value.contains("columns") && value.at("columns").is_array())
					for (const Json &column : value.at("columns")) if (column.is_string()) approval.columns.push_back(column.get<std::string>());
				if (value.contains("rows") && value.at("rows").is_array())
					for (const Json &row : value.at("rows")) {
						if (!row.is_array()) continue;
						std::vector<std::string> cells;
						for (const Json &cell : row) if (cell.is_string()) cells.push_back(cell.get<std::string>());
						approval.rows.push_back(std::move(cells));
					}
				if (value.contains("change") && value.at("change").is_object())
					approval.change_detail_json = value.at("change").dump();
				// Behind the window's own saves (a web search approved earlier in this
				// run), this save would lose to them: the change would read as never
				// asked for, and the run as completed with nothing to approve.
				{
					const persistence::PersistenceLock lock(persistence::persistence_mutex());
					catch_up_to_saved_revision(workspace_path, matter_id, task);
					++task.operation_revision;
					task.operation_execution_state = "waiting_for_approval";
					task.operation_stage = "approval";
					task.operation_updated_at = workspaces::current_timestamp();
					task.operation_activity = approval.summary;
					task.operation_approval = approval;
					task.operation_resume_allowed = true;
					append_operation_event(task, "approval.requested", "An action is waiting for approval.",
						approval.approval_id);
					routing::observability::emit({routing::observability::OperationEventKind::ApprovalRequested,
						"The proposed change is waiting for approval",
						{{"approvalId", approval.approval_id}}, std::nullopt});
					persist_task_checkpoint(workspace_path, matter_id, task);
				}
				ApprovalFiledHook hook;
				{
					const std::lock_guard<std::mutex> guard(approval_filed_hook_mutex());
					hook = approval_filed_hook();
				}
				if (hook) hook(matter_id, operationId, approval.approval_id);
			};
		// The lawyer's answer, kept beside the question it answers. Written here,
		// by the thread that runs the operation, like the question itself: a save
		// from anywhere else would be replaced by this thread's next one.
		prepared.request.tool_execution_context->lawyer_question_answered =
			[workspace_path, matter_id, &task, &changesHeld](const std::string &questionId, const std::string &answersJson) {
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				catch_up_to_saved_revision(workspace_path, matter_id, task);
				task.operation_approval.reset();
				const Json answers = Json::parse(answersJson, nullptr, false);
				const bool answered = answers.is_object();
				const std::string now = workspaces::current_timestamp();
				for (matters::LawyerQuestionRecord &record : task.lawyer_questions) {
					if (record.id != questionId) continue;
					record.answers = answered ? answers : Json(nullptr);
					record.state = answered ? "answered" : "left_to_model";
					record.answered_at = now;
				}
				++task.operation_revision;
				task.operation_execution_state = "working";
				task.operation_stage = "model";
				task.operation_updated_at = now;
				task.operation_activity = "Working on the request.";
				append_operation_event(task, answered ? "question.answered" : "question.left",
					answered ? "You answered; continuing the request." : "You left the decision to Singularity; continuing the request.",
					questionId, std::string(answered ? "answered" : "left_to_model"));
				persist_task_checkpoint(workspace_path, matter_id, task);
				// "Yes - but show me first" in an answer holds the changes still to
				// come exactly as it would in the request.
				if (changesHeld && lawyer_asks_to_see_changes_first(lawyer_words(task))) changesHeld->hold();
			};
		// How many questions are left is this operation's own count, whichever
		// attempt of it is running.
		if (prepared.request.lawyer_question_from_final_text)
			prepared.request.maximum_lawyer_questions = remaining_lawyer_questions(task);
	}
	// Every Project inspection the model asks for is a durable step of this
	// operation as it happens: what was searched, listed or read, and what of it
	// reached the model - the record a lawyer reads, live and after a restart.
	if (prepared.project_evidence && prepared.request.tool_execution_context) {
		prepared.request.tool_execution_context->project_evidence =
			[session = prepared.project_evidence, workspace_path, matter_id, &task, operationId = spec.operation_id](
					const std::string &toolId, const std::string &arguments, std::size_t roomTokens) {
				const Json step = session->run(toolId, Json::parse(arguments), roomTokens);
				// What was found is kept by passage identity for a pause this
				// attempt may make (research::ResearchPause), so a resumed attempt
				// numbers the same passages the same way.
				research::merge_research_attempt_facts(matter_id, operationId, Json{{"projectEvidence", session->kept()}});
				append_operation_event(task, "project." + step.value("kind", std::string("read")),
					project_inspection_message(step), std::nullopt, step.value("outcome", std::string()));
				task.operation_updated_at = workspaces::current_timestamp();
				persist_task_checkpoint(workspace_path, matter_id, task);
				return step.dump();
			};
	}
	bool commitAlreadyRecovered = false;
	// AdvancedWorkflow's own structural candidate is now resolved for EVERY
	// Generative request that has one (see prepare_task_request()'s own
	// comment) - never a signal by itself that this IS an AdvancedWorkflow
	// operation. This whole block must therefore never run just because the
	// candidate exists; it exists ONLY to resume an operation a PRIOR attempt
	// already confirmed (via its own durably-recorded checkpoint "kind") was
	// actually using it. A fresh attempt wires and checkpoints this only
	// after the model's own first response declares actionType
	// "advanced_workflow" - see that dispatch branch further below.
	const bool resumingAdvancedWorkflow = prepared.connected_document_action.has_value()
		&& resumedFromCheckpoint && task.document_operation_checkpoint
		&& task.document_operation_checkpoint->draft.value("kind", std::string()) == "connected_document_action";
	if (resumingAdvancedWorkflow) {
		wire_connected_document_action_request(prepared.request, *prepared.connected_document_action,
			operation_request_data_with_answers(prepared.operation_request_data, task), spec.operation_id, forced_context_window_tokens_for_experiment()
				? forced_context_window_tokens_for_experiment()
				: routing::estimate_model_context_window_tokens(spec.model_target));
		task.turn_contract_revision = prepared.connected_document_action->draft->revision;
		if (task.document_operation_checkpoint) {
			documents::restore_connected_document_draft_checkpoint(
				*prepared.connected_document_action, task.document_operation_checkpoint->draft);
			prepared.request.operation_initial_external_evidence =
				task.document_operation_checkpoint->external_evidence;
			prepared.request.operation_initial_progress = task.document_operation_checkpoint->progress;
			if (prepared.request.operation_context_compiler)
				prepared.request.prompt = prepared.request.operation_context_compiler(
					prepared.request.operation_initial_external_evidence, std::nullopt);
		}
		documents::DocumentOperationCheckpoint checkpoint;
		checkpoint.stage = "drafting";
		checkpoint.draft = documents::connected_document_draft_checkpoint_json(
			*prepared.connected_document_action);
		checkpoint.draft["kind"] = "connected_document_action";
		// Real product defect (EXEC-2): this rebuild used to replace the whole
		// draft, so a resumed operation silently lost the approval it was waiting
		// on and the decision a person had already given - and then asked for
		// nothing at all before writing.
		if (task.document_operation_checkpoint)
			carry_forward_durable_operation_facts(task.document_operation_checkpoint->draft, checkpoint.draft);
		checkpoint.external_evidence = prepared.request.operation_initial_external_evidence;
		checkpoint.progress = prepared.request.operation_initial_progress;
		task.document_operation_checkpoint = std::move(checkpoint);
		task.external_evidence = prepared.request.operation_initial_external_evidence;
		append_operation_event(task, "turn.contract.compiled", "The next bounded operation turn was compiled.");
		persist_task_checkpoint(workspace_path, matter_id, task);
		prepared.request.operation_checkpoint_handler =
			[workspace_path, matter_id, &task, plan = *prepared.connected_document_action,
					usageBefore = task.usage](
					const std::vector<research::ExternalEvidenceReference> &evidence,
					const research::OperationExecutionProgress &progress,
					const providers::UsageMetadata &usageSoFar) {
				// Usage the provider has already billed is durable before the next
				// turn runs: a crash or a quit never loses it.
				task.usage = merge_usage_metadata(usageBefore, usageSoFar);
				const std::size_t priorEvidence = task.external_evidence.size();
				const std::size_t priorAccepted = task.document_operation_checkpoint
					? task.document_operation_checkpoint->progress.item_deltas_accepted : 0;
				documents::DocumentOperationCheckpoint checkpoint;
				checkpoint.stage = "drafting";
				checkpoint.draft = documents::connected_document_draft_checkpoint_json(plan);
				checkpoint.draft["kind"] = "connected_document_action";
				if (task.document_operation_checkpoint)
					carry_forward_durable_operation_facts(task.document_operation_checkpoint->draft, checkpoint.draft);
				checkpoint.external_evidence = evidence;
				checkpoint.progress = progress;
				task.document_operation_checkpoint = std::move(checkpoint);
				task.external_evidence = evidence;
				task.turn_contract_revision = plan.draft->revision;
				// This callback checkpoints after several distinct state changes
				// inside one model turn (tool acceptance, evidence append, progress
				// update). It is not a model-turn boundary. Emitting
				// model.turn.completed here produced several misleading durable
				// completions per one real generation; authoritative turn boundaries
				// remain the ResearchToolLoop ModelTurnStart/ModelTurnEnd trace.
				if (evidence.size() > priorEvidence)
					append_operation_event(task, "evidence.retrieved", "External evidence was retrieved and checkpointed.");
				if (progress.item_deltas_accepted > priorAccepted)
					append_operation_event(task, "draft.delta.accepted", "A validated document delta was accepted.");
				persist_task_checkpoint(workspace_path, matter_id, task);
			};
		if (prepared.request.tool_execution_context) {
			prepared.request.tool_execution_context->mutation_prepared =
				[workspace_path, matter_id, &task](const std::string &record) {
					const Json parsed = Json::parse(record);
					if (!task.document_operation_checkpoint)
						task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
					task.document_operation_checkpoint->stage = "commit_prepared";
					task.document_operation_checkpoint->mutation_commit = parsed;
					task.operation_stage = "commit_prepared";
					task.operation_updated_at = workspaces::current_timestamp();
					append_operation_event(task, "commit.prepared", "A validated document mutation was prepared.",
						parsed.value("patchId", std::string()));
					persist_task_checkpoint(workspace_path, matter_id, task);
				};
			prepared.request.tool_execution_context->mutation_committed =
				[workspace_path, matter_id, &task](const std::string &record) {
					const Json parsed = Json::parse(record);
					if (!task.document_operation_checkpoint)
						task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
					task.document_operation_checkpoint->stage = "committed";
					// The commit record LAYERS onto the prepared one rather than
					// replacing it: the prepared record is the only durable
					// statement of what this change was meant to do, and a
					// process interrupted anywhere around the commit can only be
					// reconciled against it (recover_prepared_document_mutation()
					// refuses a record with no arguments at all).
					if (task.document_operation_checkpoint->mutation_commit.is_object())
						task.document_operation_checkpoint->mutation_commit.update(parsed);
					else task.document_operation_checkpoint->mutation_commit = parsed;
					task.operation_stage = "committed";
					task.operation_updated_at = workspaces::current_timestamp();
					append_operation_event(task, "commit.completed", "The document mutation was committed and reconciled.",
						parsed.value("patchId", std::string()), "committed");
					persist_task_checkpoint(workspace_path, matter_id, task);
				};
			if (task.document_operation_checkpoint
					&& task.document_operation_checkpoint->mutation_commit.is_object()
					&& task.document_operation_checkpoint->mutation_commit.value(
						"state", std::string()) == "prepared") {
				require_mutation_prepared_here(matter_id, task);
				documents::recover_prepared_document_mutation(
					*prepared.request.tool_execution_context,
					task.document_operation_checkpoint->mutation_commit.dump());
				commitAlreadyRecovered = true;
			}
		}
	}
	// Both DirectDocumentWork and the generic artifact-action candidates are
	// now resolved structurally for EVERY Generative request (see
	// prepare_task_request()'s own comment - which one, if either, the model
	// actually used is its own post-generation choice); they share the SAME
	// single tool_execution_context.mutation_prepared/mutation_committed slot,
	// so only the checkpoint's own recorded "kind" (stamped the moment either
	// path first durably checkpoints - see each ApprovalRequired catch below)
	// may decide which callback set to wire for a RESUMED attempt. A fresh
	// (never-checkpointed) attempt wires neither here - each dispatch branch
	// wires its own callbacks once the model's real choice is known, right
	// before it is used, so the two can never overwrite one another.
	const std::string checkpointKind = task.document_operation_checkpoint
		? task.document_operation_checkpoint->draft.value("kind", std::string()) : std::string();
	if (prepared.direct_document_work && checkpointKind == "direct_document_work") {
		// Same durable write-ahead/commit checkpoint contract as
		// connected_document_action above (mutation_prepared/mutation_committed
		// on the SAME tools::ToolExecutionContext type - see ToolTypes.hpp's
		// own comment), reused rather than reinvented: DirectDocumentWork's
		// mutation is a single document.patch.apply call, not a multi-turn
		// draft, so it needs no per-turn draft/evidence checkpoint of its own -
		// only this same crash-recovery guarantee for the one real write.
		if (prepared.request.tool_execution_context) {
			prepared.request.tool_execution_context->mutation_prepared =
				[workspace_path, matter_id, &task](const std::string &record) {
					const Json parsed = Json::parse(record);
					if (!task.document_operation_checkpoint)
						task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
					task.document_operation_checkpoint->stage = "commit_prepared";
					task.document_operation_checkpoint->mutation_commit = parsed;
					task.operation_stage = "commit_prepared";
					task.operation_updated_at = workspaces::current_timestamp();
					append_operation_event(task, "commit.prepared", "A validated document mutation was prepared.",
						parsed.value("patchId", std::string()));
					persist_task_checkpoint(workspace_path, matter_id, task);
				};
			prepared.request.tool_execution_context->mutation_committed =
				[workspace_path, matter_id, &task](const std::string &record) {
					const Json parsed = Json::parse(record);
					if (!task.document_operation_checkpoint)
						task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
					task.document_operation_checkpoint->stage = "committed";
					// The commit record LAYERS onto the prepared one rather than
					// replacing it: the prepared record is the only durable
					// statement of what this change was meant to do, and a
					// process interrupted anywhere around the commit can only be
					// reconciled against it (recover_prepared_document_mutation()
					// refuses a record with no arguments at all).
					if (task.document_operation_checkpoint->mutation_commit.is_object())
						task.document_operation_checkpoint->mutation_commit.update(parsed);
					else task.document_operation_checkpoint->mutation_commit = parsed;
					task.operation_stage = "committed";
					task.operation_updated_at = workspaces::current_timestamp();
					append_operation_event(task, "commit.completed", "The document mutation was committed and reconciled.",
						parsed.value("patchId", std::string()), "committed");
					persist_task_checkpoint(workspace_path, matter_id, task);
				};
			if (task.document_operation_checkpoint
					&& task.document_operation_checkpoint->mutation_commit.is_object()
					&& task.document_operation_checkpoint->mutation_commit.value(
						"state", std::string()) == "prepared") {
				require_mutation_prepared_here(matter_id, task);
				documents::recover_prepared_document_mutation(
					*prepared.request.tool_execution_context,
					task.document_operation_checkpoint->mutation_commit.dump());
				commitAlreadyRecovered = true;
			}
			if (task.document_operation_checkpoint
					&& task.document_operation_checkpoint->stage == "committed") {
				commitAlreadyRecovered = true;
			}
		}
	}
	if (checkpointKind == "professional_artifact_action" && prepared.request.tool_execution_context) {
		prepared.request.tool_execution_context->mutation_prepared =
			[workspace_path, matter_id, &task](const std::string &record) {
				const Json parsed = Json::parse(record);
				if (!task.document_operation_checkpoint)
					task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
				task.document_operation_checkpoint->stage = "commit_prepared";
				task.document_operation_checkpoint->mutation_commit = parsed;
				task.operation_stage = "commit_prepared";
				task.operation_updated_at = workspaces::current_timestamp();
				append_operation_event(task, "commit.prepared", "A validated artifact action was prepared.",
					parsed.value("artifactActionId", std::string()));
				persist_task_checkpoint(workspace_path, matter_id, task);
			};
		prepared.request.tool_execution_context->mutation_committed =
			[workspace_path, matter_id, &task](const std::string &record) {
				const Json parsed = Json::parse(record);
				if (!task.document_operation_checkpoint)
					task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
				// One write is one step: the artifact is recorded as written the
				// moment the file is committed, and again once its new version is
				// reconciled - the second time adds its version, not a second step.
				const bool alreadyCommitted = task.document_operation_checkpoint->stage == "committed";
				task.document_operation_checkpoint->stage = "committed";
				// The commit record LAYERS onto the prepared one rather than
				// replacing it: the prepared record is the only durable
				// statement of what this change was meant to do, and a
				// process interrupted anywhere around the commit can only be
				// reconciled against it (recover_prepared_document_mutation()
				// refuses a record with no arguments at all).
				if (task.document_operation_checkpoint->mutation_commit.is_object())
					task.document_operation_checkpoint->mutation_commit.update(parsed);
				else task.document_operation_checkpoint->mutation_commit = parsed;
				task.operation_stage = "committed";
				task.operation_updated_at = workspaces::current_timestamp();
				if (!alreadyCommitted)
					append_operation_event(task, "commit.completed", "The artifact action was committed and reconciled.",
						parsed.value("artifactActionId", std::string()), "committed");
				persist_task_checkpoint(workspace_path, matter_id, task);
			};
		if (task.document_operation_checkpoint
				&& task.document_operation_checkpoint->mutation_commit.is_object()
				&& task.document_operation_checkpoint->mutation_commit.value("state", std::string()) == "prepared") {
			require_mutation_prepared_here(matter_id, task);
			documents::recover_prepared_professional_artifact_action(
				*prepared.request.tool_execution_context,
				task.document_operation_checkpoint->mutation_commit.dump());
			commitAlreadyRecovered = true;
		}
		if (task.document_operation_checkpoint
				&& task.document_operation_checkpoint->stage == "committed") commitAlreadyRecovered = true;
	}

  std::optional<std::string> finish_reason;
  // The stored answer as it stood when a model's restored answer became it:
  // the text task.fact_anchors are in (DESIGN3 9.3.4). What Singularity then does
  // to the answer - its own note after it, a quotation written as the document
  // has it, its own line in the answer's place - is followed once, after the
  // attempt, by moving each anchor onto the final answer by its writing.
  std::optional<std::string> anchoredAnswer;
  // The model's answer as restoration gave it, with its anchors in its own
  // coordinates (anchored_answer_of): what a stored answer made of it anchors.
  AnchoredAnswer modelAnswer;
  // That answer as the lawyer reads it (answerAsRead), its anchors carried
  // through each figure left out by position: what a stored answer made of
  // the answer as read anchors - never re-found by a writing's rank.
  std::optional<AnchoredAnswer> readAnswer;
  try {
		routing::InferenceResult result;
		// A model's restored answer became the stored answer: where each amount
		// and date restored into it stands, with the fact it is, so the next turn
		// sends that figure again as the same fact - never as a new one.
		const auto anchorAnswer = [&]() {
			const AnchoredAnswer &answered = readAnswer ? *readAnswer : modelAnswer;
			task.fact_anchors = anchors_onto(answered.text, answered.anchors, task.response_text.value_or(std::string()));
			anchoredAnswer = task.response_text.value_or(std::string());
		};
		// Singularity's own line stands in the answer's place (a change applied, an
		// approval awaited): it states no figure of the model's answer as that
		// answer's fact, so it keeps no anchor.
		const auto ownLine = [&]() {
			task.fact_anchors.clear();
			anchoredAnswer.reset();
		};
		// The model's restored result: a plain answer is the stored answer at
		// once; a structured one (professional_response) only where its
		// "response" becomes the stored answer, below.
		const auto takeModelAnswer = [&](const routing::InferenceResult &answered) {
			modelAnswer = anchored_answer_of(answered.text, answered.fact_anchors);
			readAnswer.reset();
			ownLine();
			if (task.response_text && *task.response_text == modelAnswer.text) anchorAnswer();
		};
		// A figure the model's answer (or a draft it wrote) still states with no
		// symbol, source or local calculation behind it after its one correction
		// (DESIGN3 9.8 d): the lawyer reads one plain line beside the answer -
		// the chat's status line, never the work product.
		const auto noteUnresolvedFigures = [&task, &prepared](const routing::InferenceResult &answered) {
			if (answered.unresolved_figures == 0) return;
			namespace control = context::control_text;
			// Under the protocol the answer contract left out what it could not
			// verify: the status line says so.
			if (prepared.compute_protocol_offered) {
				append_operation_event(task, "answer.figure_withheld", answered.unresolved_figures == 1
					? control::render(control::Template::FigureWithheldOne)
					: control::render(control::Template::FigureWithheldMany, {{control::Slot::Kind::Count,
						std::to_string(std::min<std::size_t>(answered.unresolved_figures, 999))}}));
				return;
			}
			append_operation_event(task, "answer.figure_unresolved", answered.unresolved_figures == 1
				? control::render(control::Template::FigureUnresolvedOne)
				: control::render(control::Template::FigureUnresolvedMany, {{control::Slot::Kind::Count,
					std::to_string(std::min<std::size_t>(answered.unresolved_figures, 999))}}));
		};
		bool resumedWithNewCalls = false;
		bool calledModelNow = false;
		// What this operation had already been billed before this attempt's own
		// call, so a checkpoint written during the call is replaced by the call's
		// real total instead of being counted twice.
		// The pick of documents (Context/SourcePicker) was a call of this attempt's
		// own: what it cost is the task's, before the request itself is billed.
		// An attempt that takes up a pause counts its bill from where the paused
		// attempt began: the calls made before the pause are paid once, by the
		// loop that resumes (research::ResearchPause's "usage"), whether Singularity
		// closed in time to add them to this record or not.
		if (pausedAttempt.contains("usageBefore"))
			task.usage = research::pause_usage_from_json(pausedAttempt.at("usageBefore"));
		if (prepared.pick_usage.input_tokens || prepared.pick_usage.output_tokens)
			task.usage = merge_usage_metadata(task.usage, prepared.pick_usage);
		providers::UsageMetadata usageBeforeAttempt = task.usage;
		research::merge_research_attempt_facts(matter_id, spec.operation_id,
			Json{{"usageBefore", research::pause_usage_to_json(task.usage)}});
		std::optional<std::vector<context::ResolvedSourceReference>> artifactCitations;
		// The model's own typed professional results for this request, carried
		// from wherever the response was parsed to the one place every task
		// records its findings and obligations (below).
		std::optional<std::string> structuredResultsJson;
		std::optional<std::string> workProductJson;
		std::optional<std::string> researchLimitation;
		// Web research the user explicitly asked for that cannot run on this
		// Node is a limitation of whatever this request produces.
		const auto requiredResearchUnavailableLimitation = [&]() -> std::optional<std::string> {
			if (!prepared.research_unavailable_reason
					|| research::resolve_research_mode(spec.prompt) != research::ResearchMode::Required)
				return std::nullopt;
			return "Limitation: web research was requested for this request, but it could not run on this Node ("
				+ *prepared.research_unavailable_reason + "), so conclusions depending on current external authority "
				"are not verified.";
		};
		researchLimitation = requiredResearchUnavailableLimitation();
		// IDEM-1: the one real generation this operation is allowed, and which
		// kind of work it turned out to be, are durable BEFORE the file is
		// touched - not only when a person happens to be asked for approval
		// first. Without this an immediate write (Approve safe actions, Full
		// Project Access) left a prepared-mutation record whose own kind was
		// unknown, so a resume after a crash could neither recognise it nor
		// recover it: it called the model again and wrote a second time. Stamped
		// once; a resumed attempt that already carries these facts keeps them.
		const auto recordGeneratedBeforeMutation = [&](const char *kind) {
			if (!task.document_operation_checkpoint)
				task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
			Json &draft = task.document_operation_checkpoint->draft;
			if (draft.contains("kind")) return;
			draft["kind"] = kind;
			draft["rawProviderResult"] = result.text;
			draft["fulfilledResearchRequests"] = result.fulfilled_research_requests;
			if (researchLimitation) draft["researchLimitation"] = *researchLimitation;
			record_supplied_evidence(draft, prepared);
			if (task.document_operation_checkpoint->stage.empty()
					|| task.document_operation_checkpoint->stage == "drafting")
				task.document_operation_checkpoint->stage = "generated";
			task.document_operation_checkpoint->external_evidence = result.external_evidence;
			note_checkpoint_here(matter_id, spec.operation_id, draft);
			persist_task_checkpoint(workspace_path, matter_id, task);
		};
		// The floor under Full access and Apply routine changes for a change that
		// carries no artifactAction (a structured record, an advanced workflow),
		// read from the lawyer's own words exactly as a document action's is:
		// when they asked to see it first, or when `requestedBy` is not words of
		// theirs asking for it, every change the operation still makes waits for
		// them - so a question is never answered by changing a file.
		const auto holdUnlessAskedFor = [&](const std::string &requestedBy) {
			if (!changesHeld) return;
			const std::string floorReason = approval_floor(task, requestedBy);
			if (floorReason.empty()) return;
			changesHeld->hold();
			append_operation_event(task, "approval.floor", floorReason == "asked_first"
				? "You asked to see this change before it is made, so it waits for your approval."
				: "Your message did not ask for this change in so many words, so it waits for your approval.",
				std::nullopt, floorReason);
		};
		// AdvancedWorkflow's one real document change, under exactly the durable
		// contract DirectDocumentWork and the professional artifact path already
		// follow (EXEC-2): the write-ahead record is durable before the file is
		// touched, the person's decision lives on this operation's own
		// checkpoint rather than in an in-process wait a quit destroys, an
		// approved change is re-checked against the document version it was
		// approved against, and a denial is a refusal rather than a write.
		// std::nullopt means the change is waiting for a decision and the caller
		// must hand back the task as it stands, having made no change.
		const auto applyAdvancedWorkflowChange =
			[&](const documents::ConnectedDocumentActionPlan &plan, const std::string &resultText,
					const std::vector<research::ExternalEvidenceReference> &evidence)
				-> std::optional<documents::ConnectedDocumentActionResult> {
			if (!prepared.request.tool_execution_context)
				throw std::runtime_error("The advanced workflow action has no trusted operation context.");
			tools::ToolExecutionContext &context = *prepared.request.tool_execution_context;
			// Wired here as well as on the resume path above: a FIRST attempt has
			// no checkpoint "kind" yet, so the pre-generation wiring never fires
			// for it, and its real write would otherwise have no durable
			// write-ahead record to recover from after a crash.
			context.mutation_prepared = [workspace_path, matter_id, &task](const std::string &record) {
				const Json parsed = Json::parse(record);
				if (!task.document_operation_checkpoint)
					task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
				task.document_operation_checkpoint->stage = "commit_prepared";
				task.document_operation_checkpoint->mutation_commit = parsed;
				task.operation_stage = "commit_prepared";
				task.operation_updated_at = workspaces::current_timestamp();
				append_operation_event(task, "commit.prepared", "A validated document mutation was prepared.",
					parsed.value("patchId", std::string()));
				persist_task_checkpoint(workspace_path, matter_id, task);
			};
			context.mutation_committed = [workspace_path, matter_id, &task](const std::string &record) {
				const Json parsed = Json::parse(record);
				if (!task.document_operation_checkpoint)
					task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
				task.document_operation_checkpoint->stage = "committed";
				// The commit record LAYERS onto the prepared one rather than
				// replacing it: the prepared record is the only durable
				// statement of what this change was meant to do, and a
				// process interrupted anywhere around the commit can only be
				// reconciled against it (recover_prepared_document_mutation()
				// refuses a record with no arguments at all).
				if (task.document_operation_checkpoint->mutation_commit.is_object())
					task.document_operation_checkpoint->mutation_commit.update(parsed);
				else task.document_operation_checkpoint->mutation_commit = parsed;
				task.operation_stage = "committed";
				task.operation_updated_at = workspaces::current_timestamp();
				append_operation_event(task, "commit.completed",
					"The document mutation was committed and reconciled.",
					parsed.value("patchId", std::string()), "committed");
				persist_task_checkpoint(workspace_path, matter_id, task);
			};
			const std::string decision = trusted_operation_decision(matter_id, task);
			if (decision == "rejected")
				throw std::runtime_error("You rejected this change, so nothing was written.");
			if (decision == "approved")
				require_approved_advanced_workflow_target_unchanged(plan,
					task.document_operation_checkpoint->draft);
			try {
				return documents::apply_connected_document_action_draft(plan, resultText, evidence, context,
					decision == "approved");
			} catch (const documents::ConnectedDocumentActionApprovalRequired &approval) {
				if (!task.document_operation_checkpoint)
					task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
				Json &draft = task.document_operation_checkpoint->draft;
				task.document_operation_checkpoint->stage = "generated";
				draft["kind"] = "connected_document_action";
				// The generation this decision is being given on survives the wait
				// durably, so the resumed attempt never pays for a second one.
				draft["rawProviderResult"] = resultText;
				draft["approval"] = approval_binding_json(approval.request, resultText);
				if (researchLimitation) draft["researchLimitation"] = *researchLimitation;
				task.document_operation_checkpoint->external_evidence = evidence;
				task.tool_call_evidence.push_back(approval.evidence);
				// The context's own approval_requested callback above already
				// recorded the waiting state, the event and the prompt the person
				// sees; only the durable binding this operation resumes from is
				// added here, never a second copy of any of that.
				task.operation_execution_state = "waiting_for_approval";
				task.response_text = "Awaiting approval to apply this change to the Project document.";
				note_checkpoint_here(matter_id, spec.operation_id, draft);
				persist_task_checkpoint(workspace_path, matter_id, task);
				return std::nullopt;
			}
		};
		const bool draftAlreadyComplete = resumingAdvancedWorkflow
			&& !documents::connected_document_draft_completion_issue(
				*prepared.connected_document_action, prepared.request.operation_initial_external_evidence);
		// DirectDocumentWork's own equivalent of draftAlreadyComplete above: a
		// resumed attempt whose mutation was never committed (no real
		// document.patch.apply call ever ran - see the ApprovalRequired path's
		// own early return, before task.direct_document_work_document_id is
		// ever stamped) already has the ONE real generation this operation is
		// ever allowed - the raw provider text was durably persisted into this
		// task's own document_operation_checkpoint.draft at the exact moment
		// approval was first requested (see the checkpoint-creation site
		// below), deliberately never into task.response_text itself, which
		// stays a concise pending-action presentation while genuinely waiting
		// (see that same site's own comment on why raw provider JSON must
		// never be the user-visible response). Re-running routing::execute()
		// here would be a genuine second real provider call for content a
		// human already reviewed once - the real product bug this session's
		// own live acceptance proved, one level up from the ChatExecution.cpp
		// false-completion bug (that bug let a still-pending approval look
		// completed at all; this one is what a correct resume must not do
		// once the pending state is fixed).
		const bool directDocumentWorkResultAlreadyGenerated = prepared.direct_document_work.has_value()
			&& checkpointKind == "direct_document_work"
			&& resumedFromCheckpoint && !task.direct_document_work_document_id.has_value()
			&& task.document_operation_checkpoint
			&& task.document_operation_checkpoint->draft.contains("rawProviderResult");
		// A change put to the lawyer again (proposal_to_offer_again()) is the one
		// its own turn generated, and reviewed: generated already, exactly as a
		// resumed one is.
		const bool artifactResultAlreadyGenerated = (checkpointKind == "professional_artifact_action"
			&& resumedFromCheckpoint && !task.artifact_document_id.has_value()
			&& task.document_operation_checkpoint
			&& task.document_operation_checkpoint->draft.contains("rawProviderResult")) || reoffer.has_value();
		if (reoffer) {
			// Zero model calls. The model that drafted the change is named as the one
			// that produced it; this turn itself sent nothing and cost nothing.
			restore_generated_attempt(result, reoffer->task);
			result.usage = providers::UsageMetadata{};
			result.tool_call_evidence.clear();
			result.protected_api_applied = false;
			result.privacy_metadata = protected_api::papinian::PrivacyMetadata{};
			result.privacy_metadata.mode = reoffer->task.privacy_metadata.mode;
			result.external_evidence = reoffer->task.document_operation_checkpoint->external_evidence;
			result.text = reoffer->draft.at("rawProviderResult").get<std::string>();
			// The passages it was prepared from, exactly those, under the handles
			// its own request issued them.
			prepared.supplied_chunk_ids.clear();
			restore_supplied_evidence(prepared, reoffer->draft, workspace_path, matter, false);
			task.request_coverage = reoffer->task.request_coverage;
			task.source_references = reoffer->task.source_references;
			if (reoffer->draft.contains("fulfilledResearchRequests") && reoffer->draft.at("fulfilledResearchRequests").is_array())
				result.fulfilled_research_requests = reoffer->draft.at("fulfilledResearchRequests").get<std::vector<std::string>>();
			append_operation_event(task, "proposal.reoffered", "The change you were shown is put to you again for "
				"approval. Nothing was drafted anew.", reoffer->task.agent_operation_id);
		} else if (prepared.local_response) {
			result.text = *prepared.local_response;
			// A resolved-locally answer never reaches a provider either way, but
			// only ProductState is an actual deterministic product-state fact;
			// an unresolved artifact target/format is a safe refusal to guess
			// (Section 10/23), not a product-state answer, so it gets its own
			// execution_kind rather than being misreported as one.
			result.execution_kind = prepared.professional_intent.capability
					== professional::ProfessionalCapability::ProductState
				? "product_state" : "clarification_required";
		} else if (resumedFromCheckpoint && draftAlreadyComplete) {
			// Zero model calls: the operation's recorded provider, usage, privacy
			// and tool evidence are restored, never replaced by an empty attempt.
			restore_generated_attempt(result, task);
			result.text = prepared.request.operation_completion_text;
			result.external_evidence = prepared.request.operation_initial_external_evidence;
		} else if (directDocumentWorkResultAlreadyGenerated) {
			restore_generated_attempt(result, task);
			result.text = task.document_operation_checkpoint->draft.at("rawProviderResult").get<std::string>();
			restore_supplied_evidence(prepared, task.document_operation_checkpoint->draft, workspace_path, matter,
				task.document_operation_checkpoint->mutation_commit.is_object()
					&& !task.document_operation_checkpoint->mutation_commit.empty());
			// What the generation carried is what its own record says; a resume
			// that asks the model nothing counts nothing again.
			task.request_coverage = resumable->request_coverage;
			if (task.document_operation_checkpoint->draft.contains("fulfilledResearchRequests"))
				result.fulfilled_research_requests = task.document_operation_checkpoint->draft
					.at("fulfilledResearchRequests").get<std::vector<std::string>>();
		} else if (artifactResultAlreadyGenerated) {
			restore_generated_attempt(result, task);
			result.text = task.document_operation_checkpoint->draft.at("rawProviderResult").get<std::string>();
			restore_supplied_evidence(prepared, task.document_operation_checkpoint->draft, workspace_path, matter,
				task.document_operation_checkpoint->mutation_commit.is_object()
					&& !task.document_operation_checkpoint->mutation_commit.empty());
			// What the generation carried is what its own record says; a resume
			// that asks the model nothing counts nothing again.
			task.request_coverage = resumable->request_coverage;
			// The research the loop ran before the approval pause (see the
			// checkpoint's own comment) - restored, never re-run, so the
			// accepted request compares as served on this resumed attempt.
			if (task.document_operation_checkpoint->draft.contains("fulfilledResearchRequests"))
				result.fulfilled_research_requests = task.document_operation_checkpoint->draft
					.at("fulfilledResearchRequests").get<std::vector<std::string>>();
		} else {
			// Same rule as the AdvancedWorkflow call below: checkpoints during this
			// call record partial usage, and the call's own total replaces it.
			usageBeforeAttempt = task.usage;
			result = routing::execute(prepared.request);
			calledModelNow = true;
			resumedWithNewCalls = resumedFromCheckpoint;
			absorb_project_evidence(prepared, task, matter);
		}
    task.status = matters::TaskStatus::Success;
    task.response_text = result.text;
    takeModelAnswer(result);
    noteUnresolvedFigures(result);
    task.provider_id = result.provider_id;
    task.model_id = result.model_id;
	task.execution_kind = result.execution_kind;
	task.runtime_id = result.runtime_id;
	task.runtime_version = result.runtime_version;
	task.runtime_source_commit = result.runtime_source_commit;
	task.runtime_platform = result.runtime_platform;
	task.runtime_backend = result.runtime_backend;
	task.runtime_model_id = result.runtime_model_id;
	task.protected_api_applied = result.protected_api_applied;
    // A resumed operation that made new calls adds them to the usage it had
    // already recorded; a restored attempt already carries its usage.
    // A fresh attempt's own total is its request's calls and the pick of
    // documents that preceded them - the pick is a billed call of this turn too,
    // and replacing the usage with the request's own total used to lose it.
    task.usage = resumedWithNewCalls ? merge_usage_metadata(usageBeforeAttempt, result.usage)
        : calledModelNow && (prepared.pick_usage.input_tokens || prepared.pick_usage.output_tokens)
            ? merge_usage_metadata(prepared.pick_usage, result.usage) : result.usage;
    task.privacy_metadata = result.privacy_metadata;
	// A resumed operation keeps the tool calls (and the research they paid for)
	// earlier rounds already recorded; a fresh attempt records its own.
	if (resumedWithNewCalls) append_tool_call_evidence(task.tool_call_evidence, result.tool_call_evidence);
	else task.tool_call_evidence = result.tool_call_evidence;
		// Keep accumulated research truth if a later generic document-action
		// validation or patch boundary rejects the model proposal. On success
		// this is narrowed below to cited/action-referenced evidence.
		task.external_evidence = result.external_evidence;
		if (resumingAdvancedWorkflow && !commitAlreadyRecovered) {
			// The floor the first attempt applied, applied again to the change a
			// resume is about to make, from the words that attempt recorded.
			if (trusted_operation_decision(matter_id, task).empty() && task.document_operation_checkpoint)
				holdUnlessAskedFor(task.document_operation_checkpoint->draft.value("requestedBy", std::string()));
			for (const documents::DocumentRoleAssignment &assignment :
					prepared.connected_document_action->roles.assignments()) {
				task.document_role_bindings.push_back(assignment);
			}
			const std::optional<documents::ConnectedDocumentActionResult> applied =
				applyAdvancedWorkflowChange(*prepared.connected_document_action, result.text,
					result.external_evidence);
			if (!applied) return task;
			const documents::ConnectedDocumentActionResult &action = *applied;
			task.response_text = action.response_text;
			ownLine();
			task.document_action_evidence.push_back(action.action_evidence);
			task.tool_call_evidence.push_back(action.tool_call_evidence);
			task.operation_execution_state = "completed";
			task.professional_write_intent = true;
			if (task.document_operation_checkpoint) {
				task.document_operation_checkpoint->stage = "committed";
				++task.document_operation_checkpoint->progress.document_mutations_committed;
			}
		} else if (resumingAdvancedWorkflow) {
			task.response_text = prepared.request.operation_completion_text;
			ownLine();
			task.operation_execution_state = "completed";
			if (task.document_operation_checkpoint) {
				task.document_operation_checkpoint->stage = "committed";
				++task.document_operation_checkpoint->progress.document_mutations_committed;
			}
		} else if (prepared.professional_intent.capability == professional::ProfessionalCapability::Generative) {
			// THE MODEL'S OWN declared actionType, chosen from exactly what the
			// two structurally resolved candidates above made available, is the
			// sole authority for what happens next - see this file's own comment
			// on prepare_task_request()'s professional_response_output_contract()
			// wiring. Singularity validates the choice against real, current Project
			// state (build_professional_artifact_plan()/DirectDocumentWork's own
			// target resolution) before ever touching a file.
			auto responseProposal = documents::parse_professional_response_proposal(result.text);
			if (!responseProposal) throw std::runtime_error(
				"The model's response did not match the required professional-response schema.");
			// An answer that still says it changed a document it did not change
			// (after its one correction, claimed_change_issue) is taken - with Singularity's
			// own words of what happened, so it never reads as done.
			if (claims_a_change_not_made(*responseProposal))
				responseProposal->response += "\n\nNo document was changed.";
			// Independent of whatever side effect (if any) this response proposes.
			structuredResultsJson = responseProposal->structured_results_json;
			workProductJson = responseProposal->work_product_json;
			// Carried to run_task()'s save, which decides what becomes of it.
			task.suggested_chat_title = responseProposal->chat_title;
			// A research request still open on an accepted result means external
			// evidence the model judged necessary never reached it (research off,
			// denied by policy, unavailable, out of budget, or a declaration Singularity
			// could not run). That is a runtime fact Singularity states itself -
			// never a silent closed-world answer. It is a statement about THIS
			// OPERATION's research: under Off nothing can ever serve it, so every
			// replacement proposal carries it and the pending checkpoint persists
			// it for a resume after approval; under Auto a later round of the same
			// operation (a correction with its own research budget) may serve the
			// very request that was open, and then the declaration is lifted.
			// An open request that this operation already served (the loop ran
			// that exact search and the model merely repeated it) is not a gap,
			// and under explicit @web the model researched with tools directly -
			// an echoed request there says nothing about what ran.
			enum class OpenRequestState { Served, Unserved, Unusable };
			const auto isServed = [&](const nlohmann::json &request) {
				for (const std::string &fulfilled : result.fulfilled_research_requests)
					if (nlohmann::json::parse(fulfilled, nullptr, false) == request) return true;
				return false;
			};
			// Compared in the same normalized identity the loop validated,
			// escalated and recorded as fulfilled (normalize_research_request()).
			// A declaration that fails validation was never runnable, so the
			// need it declares stayed unserved.
			const auto openRequestState = [&](const std::string &requestJson) {
				const auto request = normalize_research_request_json(requestJson);
				if (!request) return OpenRequestState::Unusable;
				return isServed(*request) ? OpenRequestState::Served : OpenRequestState::Unserved;
			};
			// The exact (normalized) unserved request behind the current
			// declaration, so a later round that serves it can lift it.
			std::optional<nlohmann::json> unservedResearchRequest;
			const bool firstStepResearchSuppressed = prepared.connected_document_action
				&& prepared.connected_document_action->compact_context_mode;
			const auto applyOpenResearchRequest = [&](documents::ProfessionalResponseProposal &proposal) {
				const research::ResearchMode mode = prepared.request.research_mode;
				if (mode != research::ResearchMode::Required) {
					if (researchLimitation && unservedResearchRequest && mode != research::ResearchMode::Off
							&& isServed(*unservedResearchRequest)) {
						researchLimitation.reset();
						unservedResearchRequest.reset();
					}
					if (proposal.external_research_request_json && !researchLimitation) {
						const OpenRequestState state = openRequestState(*proposal.external_research_request_json);
						if (state == OpenRequestState::Unusable)
							researchLimitation = std::string("Limitation: this answer declared a need for external research, but "
								"the request it declared was not usable, so conclusions depending on current external "
								"authority are not verified.");
						else if (state == OpenRequestState::Unserved && mode == research::ResearchMode::Off
								&& prepared.research_unavailable_reason)
							researchLimitation = "Limitation: this answer needed external research, which cannot run on this "
								"Node (" + *prepared.research_unavailable_reason + "), so conclusions depending on current "
								"external authority are not verified.";
						else if (state == OpenRequestState::Unserved && mode == research::ResearchMode::Off
								&& firstStepResearchSuppressed)
							researchLimitation = std::string("Limitation: this answer needed external research, which was not "
								"available for this request because the documents it involves are too large for this model to "
								"research in a single step, so conclusions depending on current external authority are not "
								"verified.");
						else if (state == OpenRequestState::Unserved && mode == research::ResearchMode::Off)
							researchLimitation = std::string("Limitation: this answer needed external research, which is turned "
								"off for this request, so conclusions depending on current external authority are not verified.");
						else if (state == OpenRequestState::Unserved
								&& std::any_of(result.tool_call_evidence.begin(), result.tool_call_evidence.end(),
									[](const tools::ToolCallEvidence &call) { return call.error_category == "research_protection"; }))
							// Said as it happened: the search would have carried this
							// Matter's own details outside this computer.
							researchLimitation = std::string("Limitation: the web research this answer needed was not run, "
								"because it would have sent names, amounts or dates of this Matter outside this computer, so "
								"conclusions depending on current external authority are not verified.");
						else if (state == OpenRequestState::Unserved)
							researchLimitation = std::string("Limitation: this answer needed external research that Singularity "
								"could not complete for this request, so conclusions depending on current external authority "
								"are not verified.");
						if (state == OpenRequestState::Unserved)
							unservedResearchRequest = normalize_research_request_json(*proposal.external_research_request_json);
					}
				}
				if (!researchLimitation || !proposal.artifact_action) return;
				proposal.artifact_action->evidence_sufficient = false;
				auto &gaps = proposal.artifact_action->evidence_gaps;
				if (std::find(gaps.begin(), gaps.end(), *researchLimitation) == gaps.end()) gaps.push_back(*researchLimitation);
			};
			// A result generated earlier carries exactly the limitation recorded
			// when it was generated - never one worked out again from today's
			// research availability.
			if ((artifactResultAlreadyGenerated || directDocumentWorkResultAlreadyGenerated)
					&& task.document_operation_checkpoint) {
				const nlohmann::json &draft = task.document_operation_checkpoint->draft;
				researchLimitation = draft.contains("researchLimitation") && draft.at("researchLimitation").is_string()
					? std::optional<std::string>(draft.at("researchLimitation").get<std::string>()) : std::nullopt;
			}
			applyOpenResearchRequest(*responseProposal);
			// The model's answer as the lawyer reads it, wherever it is shown - on
			// its own, beside a change for approval, after a change is written or
			// under the note that one was not: a figure it asked for that still
			// could not be worked out after its corrections is left out, and one
			// calm sentence says which and why - never a guess.
			const auto answerAsRead = [&prepared, &modelAnswer, &readAnswer](const std::string &answer) {
				if (answer != modelAnswer.text) {
					readAnswer.reset();
					return prepared.computed_statements_offered ? figures_left_out_of(answer) : answer;
				}
				readAnswer = prepared.computed_statements_offered ? figures_left_out_of(modelAnswer) : modelAnswer;
				return readAnswer->text;
			};
			// A change that is not written: the note first, then the answer - less
			// any sentence of it that speaks of an approval still to come, since no
			// change waits for one.
			const auto answerWithNote = [&](const ArtifactNotPlaced &notPlaced) {
				if (notPlaced.answer == modelAnswer.text) {
					const AnchoredAnswer answer = without_pending_approval_talk(modelAnswer);
					if (answer.text.empty()) {
						readAnswer.reset();
						return notPlaced.note;
					}
					readAnswer = prepared.computed_statements_offered ? figures_left_out_of(answer) : answer;
					return notPlaced.note + "\n\n" + readAnswer->text;
				}
				const std::string answer = without_pending_approval_talk(notPlaced.answer);
				if (answer.empty()) readAnswer.reset();
				return answer.empty() ? notPlaced.note : notPlaced.note + "\n\n" + answerAsRead(answer);
			};
			if (responseProposal->action_type == documents::ArtifactActionKind::NoAction) {
				task.response_text = answerAsRead(responseProposal->response);
				anchorAnswer();
				// A document was bound as this request's write target and this
				// answer changed nothing. Singularity says so itself, as a fact: an
				// answer that describes work is not the work, and a lawyer who
				// asked for a file must never have to infer from a missing
				// version that none was written.
				if (prepared.professional_artifact_candidates.existing_target_document_id) {
					const matters::MatterDocument *bound = nullptr;
					for (const matters::MatterDocument &document : matter.documents)
						if (document.id == *prepared.professional_artifact_candidates.existing_target_document_id)
							bound = &document;
					task.response_text = task.response_text.value_or(std::string())
						+ "\n\nNothing was written: "
						+ (bound != nullptr ? bound->name : std::string("the document this request works on"))
						+ " is unchanged by this answer.";
				}
				task.operation_execution_state = "completed";
			} else if (responseProposal->action_type == documents::ArtifactActionKind::AdvancedWorkflow) {
				// The first call only decided WHAT work is needed; the AdvancedWorkflow
				// call below researches with its own real mode, so nothing about the
				// first call's research request is a limitation of the final result -
				// unless research the user asked for cannot run at all.
				researchLimitation = requiredResearchUnavailableLimitation();
				// AdvancedWorkflow: the model's own judgment that this request
				// needs ConnectedDocumentAction's richer, multi-turn, evidence/
				// methodology-aware choreography - reused verbatim (see
				// wire_connected_document_action_request()'s own comment), never
				// rebuilt. This is the SECOND real generation this operation pays
				// for, and only because the FIRST one just said it is needed - not
				// a mandatory router call every request pays regardless.
				if (!prepared.connected_document_action.has_value()) throw std::runtime_error(
					"The model proposed advanced_workflow with no real multi-document/methodology candidate "
					"resolved for it.");
				if (trusted_operation_decision(matter_id, task).empty()) holdUnlessAskedFor(responseProposal->requested_by);
				const documents::ConnectedDocumentActionPlan &plan = *prepared.connected_document_action;
				routing::InferenceRequest cdaRequest = prepared.request;
				cdaRequest.call_purpose = routing::observability::CallPurpose::EvidenceExpansion;
				cdaRequest.project_evidence_calls_from_final_text = nullptr;
				cdaRequest.maximum_project_evidence_requests = 0;
				// Its typed turns carry no question to the lawyer: what the lawyer
				// already answered reaches them in the request instead.
				cdaRequest.lawyer_question_from_final_text = nullptr;
				cdaRequest.maximum_lawyer_questions = 0;
				cdaRequest.local_facts_from_final_text = nullptr;
				cdaRequest.maximum_local_fact_rounds = 0;
				cdaRequest.local_facts_step_contract.reset();
				// The protected computation protocol stays (compute_budget is kept):
				// its operations ride beside the typed turns as lm_compute (DESIGN4
				// §1.2 carrier N), so a document edit works out its figures here.
				// The FIRST call's own research_mode may have been deliberately
				// forced Off for a compact_context_mode candidate (see prepare_
				// task_request()'s own comment - that call's job is only to learn
				// whether AdvancedWorkflow is wanted, never to itself research).
				// This second, now-confirmed AdvancedWorkflow call is exactly the
				// bounded, properly-wired call that DOES know how to research -
				// it must see the real resolved mode, never the first call's own
				// possibly-suppressed one.
				cdaRequest.research_mode = prepared.research_unavailable_reason
					? research::ResearchMode::Off : research::resolve_research_mode(spec.prompt);
				task.research_mode = research::to_string(cdaRequest.research_mode);
				const auto availableContextWindowTokens = forced_context_window_tokens_for_experiment()
					? forced_context_window_tokens_for_experiment()
					: routing::estimate_model_context_window_tokens(spec.model_target);
				if (prepared.operation_request_capacity_refusal)
					throw providers::TaskError(providers::TaskErrorCode::ContextCapacityExceeded,
						*prepared.operation_request_capacity_refusal);
				wire_connected_document_action_request(cdaRequest, plan,
					operation_request_data_with_answers(prepared.operation_request_data, task),
					spec.operation_id, availableContextWindowTokens);
				// Research the first call already did belongs to this operation: its
				// evidence and the searches and openings that found it carry into the
				// AdvancedWorkflow turns, which neither lose it nor search again.
				cdaRequest.operation_initial_external_evidence = result.external_evidence;
				for (const tools::ToolCallEvidence &call : result.tool_call_evidence) {
					if (call.outcome != tools::ToolOutcome::Executed) continue;
					if (call.tool_id == "web.search") ++cdaRequest.operation_initial_progress.search_calls_used;
					else if (call.tool_id == "web.open") ++cdaRequest.operation_initial_progress.open_calls_used;
					else continue;
					++cdaRequest.operation_initial_progress.successful_tool_calls_used;
				}
				task.turn_contract_revision = plan.draft->revision;
				documents::DocumentOperationCheckpoint advancedCheckpoint;
				advancedCheckpoint.stage = "drafting";
				advancedCheckpoint.draft = documents::connected_document_draft_checkpoint_json(plan);
				advancedCheckpoint.draft["kind"] = "connected_document_action";
				if (task.document_operation_checkpoint)
					carry_forward_durable_operation_facts(
						task.document_operation_checkpoint->draft, advancedCheckpoint.draft);
				// The documents the first call's pick chose, for a resume to read again.
				advancedCheckpoint.draft["pickedDocumentIds"] = prepared.picked_document_ids;
				// The words the first call quoted as the lawyer's request, for a
				// resume to hold the change to the same floor.
				advancedCheckpoint.draft["requestedBy"] = responseProposal->requested_by;
				task.document_operation_checkpoint = std::move(advancedCheckpoint);
				append_operation_event(task, "turn.contract.compiled",
					"AdvancedWorkflow's own bounded operation turn was compiled.");
				persist_task_checkpoint(workspace_path, matter_id, task);
				cdaRequest.operation_checkpoint_handler =
					[workspace_path, matter_id, &task, plan, usageBefore = task.usage](
							const std::vector<research::ExternalEvidenceReference> &evidence,
							const research::OperationExecutionProgress &progress,
							const providers::UsageMetadata &usageSoFar) {
						task.usage = merge_usage_metadata(usageBefore, usageSoFar);
						const std::size_t priorEvidence = task.external_evidence.size();
						const std::size_t priorAccepted = task.document_operation_checkpoint
							? task.document_operation_checkpoint->progress.item_deltas_accepted : 0;
						documents::DocumentOperationCheckpoint checkpoint;
						checkpoint.stage = "drafting";
						checkpoint.draft = documents::connected_document_draft_checkpoint_json(plan);
						checkpoint.draft["kind"] = "connected_document_action";
						if (task.document_operation_checkpoint)
							carry_forward_durable_operation_facts(
								task.document_operation_checkpoint->draft, checkpoint.draft);
						checkpoint.external_evidence = evidence;
						checkpoint.progress = progress;
						task.document_operation_checkpoint = std::move(checkpoint);
						task.external_evidence = evidence;
						task.turn_contract_revision = plan.draft->revision;
						if (evidence.size() > priorEvidence)
							append_operation_event(task, "evidence.retrieved",
								"External evidence was retrieved and checkpointed.");
						if (progress.item_deltas_accepted > priorAccepted)
							append_operation_event(task, "draft.delta.accepted",
								"A validated document delta was accepted.");
						persist_task_checkpoint(workspace_path, matter_id, task);
					};
				require_operation_current(cdaRequest);
				// Checkpoints during the call already recorded what was billed turn
				// by turn; the final total replaces those, never adds to them.
				const providers::UsageMetadata usageBeforeWorkflow = task.usage;
				std::optional<routing::InferenceResult> workflowResult;
				try {
					workflowResult = routing::execute(cdaRequest);
				} catch (const research::OutputPlanningFoundNoRegion &) {
					// The workflow looked at every table region of every candidate
					// document and the model selected none: this request asks for no
					// document change. The first generation already answered it (its
					// response is mandatory in the unified contract), so that answer
					// is what the lawyer receives - never a failed operation and a
					// bill with nothing to show for it. What the workflow's turns cost
					// is already on the task (the checkpoint handler above).
					task.document_operation_checkpoint.reset();
					append_operation_event(task, "operation.working",
						"No Project document region fits this request, so the answer is delivered in the chat.");
				}
				if (!workflowResult) {
					task.response_text = answerAsRead(responseProposal->response);
					anchorAnswer();
					task.operation_execution_state = "completed";
				} else {
				const routing::InferenceResult &cdaResult = *workflowResult;
				task.usage = merge_usage_metadata(usageBeforeWorkflow, cdaResult.usage);
				// Every later, generic post-processing step in this function reads
				// `result` (provider id, external evidence, etc.) - reassigned here
				// so none of it silently falls back to the FIRST call's own empty/
				// research-Off result once AdvancedWorkflow's real second call is
				// what actually did the work.
				result = cdaResult;
				noteUnresolvedFigures(cdaResult);
				// The early, pre-dispatch task.tool_call_evidence = result.
				// tool_call_evidence assignment recorded the FIRST call's own tool
				// calls (none when its research was off); the operation loop's real
				// evidence (web search, source retrieval, their usage) lives on
				// cdaResult and is added to it, never replacing what the first call
				// did.
				append_tool_call_evidence(task.tool_call_evidence, cdaResult.tool_call_evidence);
				for (const documents::DocumentRoleAssignment &assignment : plan.roles.assignments())
					task.document_role_bindings.push_back(assignment);
				const std::optional<documents::ConnectedDocumentActionResult> applied =
					applyAdvancedWorkflowChange(plan, cdaResult.text, cdaResult.external_evidence);
				if (!applied) return task;
				const documents::ConnectedDocumentActionResult &action = *applied;
				task.response_text = action.response_text;
				ownLine();
				task.document_action_evidence.push_back(action.action_evidence);
				task.tool_call_evidence.push_back(action.tool_call_evidence);
				task.external_evidence = cdaResult.external_evidence;
				task.operation_execution_state = "completed";
				task.professional_write_intent = true;
				if (task.document_operation_checkpoint) {
					task.document_operation_checkpoint->stage = "committed";
					++task.document_operation_checkpoint->progress.document_mutations_committed;
				}
				}
			} else if (responseProposal->action_type == documents::ArtifactActionKind::StructuredRecordFillTemplate
					|| responseProposal->action_type == documents::ArtifactActionKind::StructuredRecordUpdateExisting) try {
			// DirectDocumentWork's own deterministic mutation, through the exact
			// same production document.patch.apply path the old connected-
			// document-action branch above already uses - never a second
			// mutation mechanism. The model owns only ProfessionalDocumentResult;
			// Singularity owns the target, the locator and the physical write.
			if (!prepared.direct_document_work.has_value()) throw std::runtime_error(
				"The model proposed structured-record work with no real candidate block resolved for it.");
			if (!responseProposal->structured_record_json) throw std::runtime_error(
				"The model declared structured-record work with no structuredRecord object.");
			auto parsed = documents::parse_professional_document_result(*responseProposal->structured_record_json);
			if (!parsed.has_value())
				throw std::runtime_error(
					"The model's response did not match the required professional-document schema.");
			// The record is written into its target document as plain text, so its
			// citations render to lawyer-facing document labels (never a raw source
			// handle in the document) and become this task's provenance. A resumed
			// attempt renders from the references recorded when the proposal was
			// accepted, never from a fresh retrieval's numbering.
			{
				std::vector<std::string *> recordTexts{&parsed->title};
				for (std::string &paragraph : parsed->description) recordTexts.push_back(&paragraph);
				for (std::string &paragraph : parsed->recommendations) recordTexts.push_back(&paragraph);
				if (parsed->additional_information) recordTexts.push_back(&*parsed->additional_information);
				for (std::string &support : parsed->source_support) recordTexts.push_back(&support);
				artifactCitations = documents::render_plain_source_labels(recordTexts,
					directDocumentWorkResultAlreadyGenerated ? task.source_references : prepared.supplied_markers.references(),
					matter);
				task.source_references = *artifactCitations;
			}
			// The record as its document receives it: what it asks Singularity to work
			// out is worked out first, so the register is written with the figures,
			// never "{{...}}" or an operand mark. The record kept for the chat stays
			// as the model wrote it and is worked out where the lawyer reads it. A
			// record with a figure that still cannot be worked out is not written.
			documents::ProfessionalDocumentResult recordWritten = *parsed;
			if (const std::optional<std::string> unworked = work_out_record(recordWritten)) {
				append_operation_event(task, "artifact.refused", "A figure of the proposed record could not be worked out.",
					std::nullopt, "refused");
				throw ArtifactNotPlaced(responseProposal->response, *unworked);
			}
			const auto &plan = *prepared.direct_document_work;
			// The same real selection MatterRequestExecution.cpp and any
			// deterministic acceptance harness that cannot run a real model
			// share - see select_direct_document_work_target()'s own comment.
			// For a rerun of a turn that already wrote a record, updating that
			// record is a fact of what this turn is, not a claim the model makes:
			// the same work redone lands where it landed before (SLR-3).
			const auto selected = documents::select_direct_document_work_target(
				plan, spec.realized_record_document_id.has_value() || parsed->updates_previous_object);
			if (!selected.has_value())
				throw std::runtime_error("No free structured record template is available in this request's source scope, "
					"so the new record was not added and nothing was changed. Bind the register as the target or widen "
					"the source selection.");
			const std::string &targetDocumentId = selected->document_id;
			const documents::DocumentBlockSurface &targetBlock = selected->block;
			const auto severityPresentation = documents::document_native_severity_presentation(
				plan.candidates, parsed->severity);
			const auto operations = documents::build_fill_template_operations(
				targetDocumentId, targetBlock, recordWritten, task.id, severityPresentation, plan.candidates);
			if (!operations.has_value())
				throw std::runtime_error("The resolved target block cannot host this content (no compatible field mapping).");
			if (!commitAlreadyRecovered) {
				if (!prepared.request.tool_execution_context)
					throw std::runtime_error("The DirectDocumentWork task has no trusted operation context.");
				const auto targetDocument = std::find_if(matter.documents.begin(), matter.documents.end(),
					[&](const matters::MatterDocument &candidate) {
						return candidate.id == targetDocumentId; });
				if (targetDocument == matter.documents.end())
					throw std::runtime_error("The target document no longer exists in this Project.");
				documents::ensure_production_document_patch_tools();
				// Wired freshly here (never relying on the pre-generation wiring
				// above, which only ever fires for a RESUME whose
				// checkpoint.draft["kind"] already matches - a first-time attempt
				// has no checkpoint yet) so this real mutation still gets its own
				// durable write-ahead/commit checkpoint.
				prepared.request.tool_execution_context->mutation_prepared =
					[workspace_path, matter_id, &task](const std::string &record) {
						const Json parsed = Json::parse(record);
						if (!task.document_operation_checkpoint)
							task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
						task.document_operation_checkpoint->stage = "commit_prepared";
						task.document_operation_checkpoint->mutation_commit = parsed;
						task.operation_stage = "commit_prepared";
						task.operation_updated_at = workspaces::current_timestamp();
						append_operation_event(task, "commit.prepared", "A validated document mutation was prepared.",
							parsed.value("patchId", std::string()));
						persist_task_checkpoint(workspace_path, matter_id, task);
					};
				prepared.request.tool_execution_context->mutation_committed =
					[workspace_path, matter_id, &task](const std::string &record) {
						const Json parsed = Json::parse(record);
						if (!task.document_operation_checkpoint)
							task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
						task.document_operation_checkpoint->stage = "committed";
						// The commit record LAYERS onto the prepared one rather than
						// replacing it: the prepared record is the only durable
						// statement of what this change was meant to do, and a
						// process interrupted anywhere around the commit can only be
						// reconciled against it (recover_prepared_document_mutation()
						// refuses a record with no arguments at all).
						if (task.document_operation_checkpoint->mutation_commit.is_object())
							task.document_operation_checkpoint->mutation_commit.update(parsed);
						else task.document_operation_checkpoint->mutation_commit = parsed;
						task.operation_stage = "committed";
						task.operation_updated_at = workspaces::current_timestamp();
						append_operation_event(task, "commit.completed",
							"The document mutation was committed and reconciled.",
							parsed.value("patchId", std::string()), "committed");
						persist_task_checkpoint(workspace_path, matter_id, task);
					};
				// Real product bug (caught live, this session's own test_lm
				// acceptance, then hardened for a genuine process restart): the
				// durable decision ("approved"/"rejected") lives ONLY in this
				// task's own document_operation_checkpoint.draft, NEVER in
				// ToolBroker::pending_ - that map is in-process memory only and
				// is lost across a real restart even though the Project/task/
				// checkpoint on disk survives untouched. record_approval_
				// response() (ChatExecution.cpp) is what durably writes
				// "decision" here the moment a human approves or denies,
				// independent of whether ToolBroker's own live pending-approval
				// record still exists. This is the ONLY thing ever consulted on
				// a resume, including after a real restart - never
				// task.operation_approval (persist_new_operation() resets that
				// to std::nullopt at the start of every resume) and never
				// ToolBroker::wait_for_approval()'s own stored context (whose
				// mutation_prepared/mutation_committed callbacks capture the
				// FIRST attempt's own now-destroyed local `task` by reference -
				// a real, live-reproduced dangling-reference crash this
				// session's own approval-resume acceptance test caught).
				recordGeneratedBeforeMutation("direct_document_work");
				const std::string decision = task.document_operation_checkpoint
					? trusted_operation_decision(matter_id, task) : std::string();
				if (decision.empty()) holdUnlessAskedFor(responseProposal->requested_by);
				const Json patchArguments{{"documentId", targetDocument->id},
					{"expectedVersion", targetDocument->version},
					{"expectedFingerprint", targetDocument->fingerprint}, {"operations", *operations}};
				documents::forget_document_patch_conflict();
				tools::ToolAttempt attempt;
				if (decision == "rejected") {
					attempt.result.outcome = tools::ToolOutcome::Denied;
					attempt.result.error_message = "The required Project-document change was denied.";
				} else if (decision == "approved") {
					// ToolBroker::execute_preapproved() never re-asks
					// permission_for() and never touches ToolBroker::pending_ -
					// this Operation's own durable "approved" record IS the
					// authorization, and only for the exact change it names: the
					// target must still be the document version the approval
					// was requested against, and the rebuilt intent must be the
					// approved intent. A change nobody approved is never written.
					require_approved_target_unchanged(task.document_operation_checkpoint->draft, targetDocument->id,
						targetDocument->version, targetDocument->fingerprint);
					attempt = tools::production_tool_broker().execute_preapproved(
						*prepared.request.tool_execution_context, {"document.patch.apply", patchArguments});
				} else {
					attempt = tools::production_tool_broker().request(
						*prepared.request.tool_execution_context, {"document.patch.apply", patchArguments});
					// Real product bug (caught live, this session's own test_lm
					// acceptance): resume_chat_operation() refuses to resume any
					// task with no document_operation_checkpoint at all - a real,
					// durable guard against resuming a request that was never
					// actually pending anything. DirectDocumentWork never created
					// one before this point (ConnectedDocumentAction always does,
					// eagerly, before its own first tool call), so a
					// DirectDocumentWork task that reached ApprovalRequired below
					// had no way to ever be resumed - "This request has no durable
					// checkpoint to resume." Created only once, right after the
					// one real mutation attempt this Operation is allowed turns
					// out to need approval. Returning the already-correct task
					// here (never completing it, never throwing) lets the real
					// production approval path (respond_tool_approval() ->
					// record_approval_response(), ChatExecutionBridge.cpp) durably
					// record the human's decision above, entirely independent of
					// this in-process ToolBroker call ever being reachable again.
					if (attempt.result.outcome == tools::ToolOutcome::ApprovalRequired) {
						documents::DocumentOperationCheckpoint checkpoint;
						checkpoint.stage = "generated";
						// The raw provider text durably survives a pending
						// approval here, in the checkpoint's own generic draft
						// field - deliberately NEVER in task.response_text (see
						// below). Real product bug (caught live): raw structured
						// JSON is internal machine data, not a professional
						// presentation - a lawyer waiting for approval must see a
						// concise pending-action message, not "{\"title\":...}".
						checkpoint.draft = Json{{"rawProviderResult", result.text}, {"kind", "direct_document_work"}};
						if (attempt.approval) checkpoint.draft["approval"] = approval_binding_json(*attempt.approval, result.text);
						// The same operation-level facts the professional-artifact
						// checkpoint keeps: research this operation already ran, and a
						// declared limitation - so the resumed attempt neither
						// re-declares a served request nor loses a real limitation.
						checkpoint.draft["fulfilledResearchRequests"] = result.fulfilled_research_requests;
						if (researchLimitation) checkpoint.draft["researchLimitation"] = *researchLimitation;
						record_supplied_evidence(checkpoint.draft, prepared);
						checkpoint.external_evidence = result.external_evidence;
						note_checkpoint_here(matter_id, spec.operation_id, checkpoint.draft);
						task.document_operation_checkpoint = std::move(checkpoint);
						task.response_text = "Awaiting approval to apply this change to the Project document.";
						ownLine();
						persist_task_checkpoint(workspace_path, matter_id, task);
						return task;
					}
				}
				// A change the document could not take - changed on disk, open in
				// its program, locked - ends as the conflict the lawyer can prepare
				// again, a full disk as a full disk: the one mapping every patch
				// caller uses (end_unexecuted_document_patch).
				documents::end_unexecuted_document_patch(attempt.result,
					"The change to the document was not approved or could not be made.");
				task.tool_call_evidence.push_back(attempt.evidence);
			}
			const std::string severityLabel = documents::document_native_severity_label(plan.candidates,
				targetBlock.banner_metadata_text, parsed->severity);
			task.response_text = documents::describe_direct_document_work_result(*parsed, selected->used_update_target,
				severityLabel);
			ownLine();
			task.direct_document_work_severity = severityLabel.empty() ? parsed->severity : severityLabel;
			task.direct_document_work_updated = selected->used_update_target;
			task.direct_document_work_memo = documents::direct_document_work_memo(recordWritten, severityLabel);
			task.operation_execution_state = "completed";
			// Professional-object lineage (see Chat.hpp's own field comments):
			// stamped on every successful mutation, new-work or update alike, so
			// the NEXT same-Chat follow-up can resolve back to this exact block
			// deterministically instead of re-parsing document text.
			task.direct_document_work_document_id = targetDocumentId;
			task.direct_document_work_table_id = targetBlock.table_id;
			task.direct_document_work_banner_row_index = targetBlock.banner_row_index;
			task.direct_document_work_title = recordWritten.title;
			task.professional_write_intent = true;
			if (task.document_operation_checkpoint) {
				task.document_operation_checkpoint->stage = "committed";
				++task.document_operation_checkpoint->progress.document_mutations_committed;
			}
		} catch (const ArtifactNotPlaced &notPlaced) {
			task.response_text = answerWithNote(notPlaced);
			anchorAnswer();
		} else try {
			// Generic create/append/replace/update_cells - ProfessionalArtifactAction's
			// own existing, unchanged validate/apply/recover pipeline. The plan is
			// built HERE, from the model's own declared action/format against the
			// structurally resolved candidates - never guessed pre-generation (see
			// ProfessionalArtifactAction.hpp's own header comment). A declared
			// action with no matching real candidate fails closed (throws), never
			// silently guesses a target. A change that still cannot be placed after
			// the model's one correction ends in the model's answer instead
			// (ArtifactNotPlaced, caught at the end of this branch).
			if (!prepared.request.tool_execution_context)
				throw std::runtime_error("The artifact action has no trusted operation context.");
			if (!responseProposal->artifact_action) throw std::runtime_error(
				"The model declared a generic artifact action with no artifactAction object.");
			// A change to a document the Project already has is named by that
			// document, as the in-turn document_change tool names it, and its
			// summary is what the answer itself says was done. A model that follows
			// a described contract rather than an enforced one can leave both empty;
			// that says nothing about the change, and refusing the change for it
			// would lose work the lawyer asked for. A NEW document's title is its
			// file name, so it is never filled in for the model.
			const auto settleSummary = [&] {
				if (responseProposal->artifact_action->action == documents::ArtifactActionKind::CreateContent) return;
				documents::ProfessionalArtifactProposal &named = *responseProposal->artifact_action;
				if (named.summary.empty())
					named.summary = responseProposal->response.empty()
						? std::string("The document was changed.") : responseProposal->response;
			};
			settleSummary();
			const std::string decision = task.document_operation_checkpoint
				? trusted_operation_decision(matter_id, task) : std::string();
			// A proposal this attempt generated itself, not one restored from its
			// checkpoint or already decided by the lawyer.
			const bool freshProposal = !artifactResultAlreadyGenerated && !commitAlreadyRecovered && decision.empty();

			/* - - - - - - */

			// Every passage of this operation's evidence basis (a targeted
			// continuation's included) as source material under its stable
			// marker, each keeping the first call's admission priority.
			const auto operationEvidenceChunks = [&](context::PromptAssemblyInput &input) {
				const context::SourceMarkerRegistry markers(prepared.document_context);
				std::unordered_map<std::string, std::size_t> draftPriority;
				for (const context::PromptSourceMaterial &chunk : prepared.prompt_input.source_chunks)
					draftPriority.emplace(chunk.id, chunk.priority);
				input.source_chunks.clear();
				for (const context::RetrievedContextChunk &passage : prepared.document_context) {
					const auto known = draftPriority.find(passage.chunk.chunk_id);
					input.source_chunks.push_back({passage.chunk.chunk_id, markers.format_chunk(passage),
						passage.explicitly_selected, passage.document_id,
						known != draftPriority.end() ? known->second : draftPriority.size() + input.source_chunks.size(),
						markers.document_token_for(passage.chunk.chunk_id), passage.document_title});
				}
				return markers;
			};
			// Without a user selection, the passages a review or its continuation
			// must carry are documents the review asked to re-read: coverage and
			// refusals say so instead of speaking of a selection.
			const context::RequiredSourceOrigin reviewEvidenceOrigin = spec.selected_document_ids.empty()
				? context::RequiredSourceOrigin::RequestedEvidence : context::RequiredSourceOrigin::UserSelection;
			// True once a targeted continuation extended the evidence basis beyond
			// what the first call's own prompt carries.
			// Passages the first call's own inspection gave already extend it.
			bool evidenceBasisExtended = prepared.project_evidence && !prepared.project_evidence->given().empty();
			// The one way a proposal is sent back to the model that made it - after a
			// review, or after Singularity found it could not be carried out: the first
			// call's request with the correction after it, and, once the evidence
			// basis grew beyond that prompt, the whole basis under the same
			// capacity-first budget, its passages joining the supplied set the
			// corrected citations resolve against.
			const auto redraftRequest = [&](const std::string &correction) {
				routing::InferenceRequest correctionRequest = prepared.request;
				// The draft had its facts step; a redraft asks for more on demand.
				correctionRequest.local_facts_step_contract.reset();
				correctionRequest.project_evidence_calls_from_final_text = nullptr;
				correctionRequest.maximum_project_evidence_requests = 0;
				correctionRequest.operation_initial_external_evidence = result.external_evidence;
				correctionRequest.call_purpose = routing::observability::CallPurpose::Correction;
				// The redraft can still ask - its schema still carries the question -
				// with what is left of this operation's questions, and it is written
				// knowing every answer the lawyer already gave.
				correctionRequest.maximum_lawyer_questions = remaining_lawyer_questions(task);
				const std::string appended = answered_lawyer_questions_text(task)
					+ local_facts_for_later_call(*operationLocalFacts) + "\n\n" + correction;
				std::string redraftPrompt = prepared.request.prompt;
				if (evidenceBasisExtended) {
					context::PromptAssemblyInput redraftInput = prepared.prompt_input;
					const context::SourceMarkerRegistry redraftMarkers = operationEvidenceChunks(redraftInput);
					redraftInput.required_source_origin = reviewEvidenceOrigin;
					if (redraftInput.maximum_context_tokens) {
						const std::size_t required = research::estimate_serialized_tokens(appended);
						redraftInput.maximum_context_tokens = *redraftInput.maximum_context_tokens > required
							? *redraftInput.maximum_context_tokens - required : 0;
					}
					const std::size_t requiredBytes = appended.size();
					redraftInput.maximum_context_bytes = redraftInput.maximum_context_bytes > requiredBytes
						? redraftInput.maximum_context_bytes - requiredBytes : 0;
					const context::PromptAssemblyResult redraftAssembly = context::PromptAssembler::assemble_with_budget(redraftInput);
					redraftPrompt = redraftAssembly.prompt;
					if (redraftAssembly.budget.output_tokens)
						correctionRequest.maximum_output_tokens = *redraftAssembly.budget.output_tokens;
					for (const context::PromptBudgetItem &item : redraftAssembly.budget.included)
						if (item.category == "explicit-source" || item.category == "retrieved-source")
							prepared.supplied_chunk_ids.insert(item.id);
					prepared.supplied_markers = redraftMarkers.restricted_to(prepared.supplied_chunk_ids);
				}
				correctionRequest.prompt = redraftPrompt + appended;
				return correctionRequest;
			};
			// A correction or review call that fails at the provider (a network
			// error, a rate limit, a refusal) takes nothing back from the draft the
			// operation already has and paid for: the caller keeps that draft and
			// says so. What the failed call billed stays on the task. A Stop still
			// stops, and a protection failure still fails the turn where the lawyer
			// can see the safety state.
			const auto callKeepingDraft = [&](const routing::InferenceRequest &request)
					-> std::optional<routing::InferenceResult> {
				require_operation_current(prepared.request);
				try {
					return routing::execute(request);
				} catch (const providers::TaskError &error) {
					require_operation_current(prepared.request);
					if (error.code() == providers::TaskErrorCode::PrivacyProtectionUnavailable
							|| error.code() == providers::TaskErrorCode::ProtectedPayloadLeakDetected
							|| error.code() == providers::TaskErrorCode::ProtectedResponseInvalid)
						throw;
					if (const auto *failure = dynamic_cast<const research::RequiredResearchFailure *>(&error)) {
						task.usage = merge_usage_metadata(task.usage, failure->usage);
						append_tool_call_evidence(task.tool_call_evidence, failure->tool_call_evidence);
					} else if (const auto *unusable = dynamic_cast<const providers::UnusableGeneration *>(&error)) {
						task.usage = merge_usage_metadata(task.usage, unusable->usage);
					}
					return std::nullopt;
				}
			};

			/* - - - - - - */

			// The plan, and - for a proposal this attempt made - the tool's own
			// validation of it, exactly as it runs before an approval can exist. A
			// proposal Singularity cannot carry out is the model's to correct, never the
			// lawyer's to approve.
			struct ProposalRefusal {
				std::string message;
				std::optional<tools::ToolCallEvidence> evidence;
			};
			documents::ProfessionalArtifactActionPlan plan;
			const auto planOrRefusal = [&]() -> std::optional<ProposalRefusal> {
				// A new document whose answer cites one Project document alone is
				// drawn from that document, though the model left basedOn empty - an
				// amendment "based on the agreement" citing only the agreement. It is
				// named after it; the approved run reads the same answer and names
				// the file the same.
				if (responseProposal->artifact_action->action == documents::ArtifactActionKind::CreateContent
						&& responseProposal->artifact_action->based_on_handle.empty())
					responseProposal->artifact_action->based_on_handle = documents::sole_cited_nameable_handle(
						prepared.professional_artifact_candidates, responseProposal->response);
				try {
					plan = documents::build_professional_artifact_plan(
						prepared.professional_artifact_candidates, *responseProposal->artifact_action, matter);
				} catch (const std::exception &error) {
					return ProposalRefusal{error.what(), std::nullopt};
				}
				// An approved new document is written under the name the lawyer
				// approved it under, whatever Singularity would name it now.
				if (decision == "approved" && plan.creates_new_artifact && task.document_operation_checkpoint) {
					const Json &draft = task.document_operation_checkpoint->draft;
					const std::string approvedName = draft.contains("approval") && draft.at("approval").is_object()
						? draft.at("approval").value("newDocument", std::string()) : std::string();
					const std::string approvedPath = documents::approved_new_document_path(matter, approvedName, plan.format);
					if (!approvedPath.empty()) plan.relative_path = approvedPath;
				}
				// A change put to the lawyer again is shown under the name it was
				// shown under before, while that name is still free.
				if (reoffer && plan.creates_new_artifact && matter.directory_path) {
					const std::string shownPath = documents::approved_new_document_path(matter,
						reoffer->draft.at("approval").value("newDocument", std::string()), plan.format);
					std::error_code missing;
					if (!shownPath.empty()
							&& !std::filesystem::exists(std::filesystem::u8path(*matter.directory_path) / std::filesystem::u8path(shownPath), missing))
						plan.relative_path = shownPath;
				}
				// And against the very document it was shown against: a handle that
				// names another one now is not that change.
				if (reoffer && !plan.creates_new_artifact) {
					const Json &shown = reoffer->draft.at("approval");
					const std::optional<std::string> shownTarget = shown.contains("targetDocumentId")
						&& shown.at("targetDocumentId").is_string()
						? std::optional<std::string>(shown.at("targetDocumentId").get<std::string>()) : std::nullopt;
					if (plan.target_document_id != shownTarget)
						return ProposalRefusal{"The change is no longer aimed at the document it was shown against.", std::nullopt};
				}
				// Named by the document it changes, once the plan knows which one that
				// is - bound by the lawyer or named by the model.
				if (!plan.creates_new_artifact && responseProposal->artifact_action->title.empty())
					for (const matters::MatterDocument &document : matter.documents)
						if (document.id == plan.target_document_id)
							responseProposal->artifact_action->title = document.relative_path;
				// The Project's own names for one party, so a change the model
				// anchors on one writing of it resolves against the document's.
				plan.identity_writings = prepared.request.tool_execution_context->project_identity_writings;
				// A table quoted whole and rewritten with a value changed is that
				// value's cell edit, before anything is checked, shown or written -
				// on the approved run exactly as on the proposing one, or the lawyer
				// approves one change and Singularity attempts another.
				(void)documents::settle_quoted_table_edits(plan, *responseProposal->artifact_action, matter);
				(void)documents::settle_replacement_overlaps(plan, *responseProposal->artifact_action, matter);
				// A change put to the lawyer again is checked against the document as
				// it stands, exactly as a fresh proposal is.
				if (!freshProposal && !reoffer) return std::nullopt;
				const std::optional<documents::ArtifactActionRefused> refused =
					documents::validate_professional_artifact_action(plan, *responseProposal->artifact_action,
						*prepared.request.tool_execution_context);
				if (!refused) return std::nullopt;
				return ProposalRefusal{refused->what(), refused->evidence};
			};
			const auto recordRefusal = [&](const ProposalRefusal &refusal) {
				if (refusal.evidence) task.tool_call_evidence.push_back(*refusal.evidence);
				append_operation_event(task, "artifact.refused", refusal.message, std::nullopt, "refused");
			};
			// The lawyer's "show me first" belongs to the request, not to one draft
			// of the change. A correction replaces the proposal it corrects whole, and
			// one that comes back without askFirst must not turn a change the lawyer
			// asked to see into one written without them. The same holds for a draft
			// the review did not clear. Written into the saved result as well, so the
			// run that resumes once they approve rebuilds the very action they approved.
			const auto forceAskFirst = [&] {
				responseProposal->artifact_action->ask_first = true;
				Json raw = Json::parse(result.text, nullptr, false);
				if (!raw.is_object() || !raw.contains("artifactAction") || !raw.at("artifactAction").is_object()) return;
				const Json &declared = raw.at("artifactAction");
				if (declared.contains("askFirst") && declared.at("askFirst").is_boolean()
						&& declared.at("askFirst").get<bool>())
					return;
				raw["artifactAction"]["askFirst"] = true;
				result.text = raw.dump();
			};
			// A change whose figures Singularity still could not work out after its
			// corrections is never written with a figure missing: the lawyer reads
			// the answer, and one plain sentence says which figure and why.
			const auto requireFiguresWorkedOut = [&] {
				if (!freshProposal || responseProposal->artifact_action->statement_failures.empty()) return;
				const auto &unworked = responseProposal->artifact_action->statement_failures;
				append_operation_event(task, "artifact.refused", "A figure of the proposed change could not be worked out.",
					std::nullopt, "refused");
				throw ArtifactNotPlaced(responseProposal->response,
					change_figures_note(unworked, written_in_russian(responseProposal->response)));
			};
			// A phrase restoration wrote twice side by side is already written once
			// where the draft was restored (PapinianGuard::restore). A symbol that
			// did not come back as words is told to the review, which asks the
			// drafter for it.
			std::optional<std::string> restorationReviewNote;
			const auto noteUnrestoredSymbols = [&]() {
				if (!responseProposal->artifact_action) return;
				const std::vector<std::string> unrestored = documents::unrestored_symbols(
					*responseProposal->artifact_action, &responseProposal->response);
				restorationReviewNote.reset();
				if (!unrestored.empty())
					restorationReviewNote = "Singularity found text in this draft that did not come back as words ("
						+ std::to_string(unrestored.size()) + (unrestored.size() == 1
							? " place" : " places") + "). Return needs_redraft asking the drafter to write the value "
						"there in words.";
			};
			// A Word change of several steps some of which still cannot be placed
			// after the model's one correction: the steps that can be placed are
			// put to the lawyer, with one plain sentence naming each step that is
			// not in the change - never the whole of the work dropped while the
			// answer speaks of an approval (Golden C, 25 Sep 2026). Each step is
			// tried alone against the document as it stands, exactly as the whole
			// change is, and what is kept is checked again together. What is kept
			// always waits for the lawyer, whatever the Project's action mode: a
			// part of a change is not what they asked for, and written alone it
			// could leave the document at odds with itself (Section 9 changed, its
			// key-terms row not). The saved result holds only the kept steps, so
			// the run the lawyer's approval resumes rebuilds the very change they
			// were shown. False, with the proposal as it was, when no step or every
			// step can be placed.
			const auto keepPlaceableSteps = [&](bool askedFirst) -> bool {
				Json raw = Json::parse(result.text, nullptr, false);
				if (!raw.is_object() || !raw.contains("artifactAction") || !raw.at("artifactAction").is_object()
						|| !raw.at("artifactAction").contains("operations")
						|| !raw.at("artifactAction").at("operations").is_array()
						|| raw.at("artifactAction").at("operations").size() < 2
						|| responseProposal->artifact_action->action != documents::ArtifactActionKind::EditDocument)
					return false;
				const documents::ProfessionalResponseProposal whole = *responseProposal;
				const auto tryProposal = [&](const Json &steps) {
					Json candidate = raw;
					candidate["artifactAction"]["operations"] = steps;
					const auto parsed = documents::parse_professional_response_proposal(candidate.dump());
					if (!parsed || !parsed->artifact_action) return false;
					*responseProposal = *parsed;
					if (askedFirst) responseProposal->artifact_action->ask_first = true;
					settleSummary();
					return !planOrRefusal().has_value();
				};
				const bool russian = written_in_russian(responseProposal->response);
				Json kept = Json::array();
				std::vector<std::string> left;
				for (const Json &step : raw.at("artifactAction").at("operations")) {
					if (tryProposal(Json::array({step}))) kept.push_back(step);
					else left.push_back(unplaced_step_words(step, russian));
				}
				if (kept.empty() || left.empty() || !tryProposal(kept)) {
					*responseProposal = whole;
					(void)planOrRefusal();
					return false;
				}
				std::string documentName = russian ? std::string("\xD0\xB4\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82") : std::string("the document");
				for (const matters::MatterDocument &document : matter.documents)
					if (document.id == plan.target_document_id && !document.name.empty()) documentName = document.name;
				const bool one = left.size() == 1;
				std::string steps;
				for (std::size_t index = 0; index < left.size(); ++index)
					steps += std::string(index == 0 ? "" : index + 1 == left.size() ? (russian ? " \xD0\xB8 " : " and ") : ", ")
						+ left[index];
				const std::string note = russian
					? std::string(one ? "\xD0\x9E\xD0\xB4\xD0\xBD\xD1\x83 \xD1\x87\xD0\xB0\xD1\x81\xD1\x82\xD1\x8C \xD1\x8D\xD1\x82\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F \xD0\xBD\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB2\xD0\xBD\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8 \xD0\xB2 " : "\xD0\x9D\xD0\xB5\xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD1\x8B\xD0\xB5 \xD1\x87\xD0\xB0\xD1\x81\xD1\x82\xD0\xB8 \xD1\x8D\xD1\x82\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F \xD0\xBD\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB2\xD0\xBD\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8 \xD0\xB2 ") + documentName
						+ std::string(one ? ", \xD0\xBF\xD0\xBE\xD1\x8D\xD1\x82\xD0\xBE\xD0\xBC\xD1\x83 \xD0\xB5\xD1\x91 \xD0\xBD\xD0\xB5\xD1\x82 \xD0\xB2 \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB8, \xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB5 \xD0\xB2\xD0\xB0\xD0\xBC \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBE: " : ", \xD0\xBF\xD0\xBE\xD1\x8D\xD1\x82\xD0\xBE\xD0\xBC\xD1\x83 \xD0\xB8\xD1\x85 \xD0\xBD\xD0\xB5\xD1\x82 \xD0\xB2 \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB8, \xD0\xBA\xD0\xBE\xD1\x82\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB5 \xD0\xB2\xD0\xB0\xD0\xBC \xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xBE: ") + steps
						+ std::string(one ? ". \xD0\xAD\xD1\x82\xD1\x83 \xD1\x87\xD0\xB0\xD1\x81\xD1\x82\xD1\x8C \xD0\xB2\xD1\x8B \xD0\xBC\xD0\xBE\xD0\xB6\xD0\xB5\xD1\x82\xD0\xB5 \xD0\xB2\xD0\xBD\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8 \xD1\x81\xD0\xB0\xD0\xBC\xD0\xB8." : ". \xD0\xAD\xD1\x82\xD0\xB8 \xD1\x87\xD0\xB0\xD1\x81\xD1\x82\xD0\xB8 \xD0\xB2\xD1\x8B \xD0\xBC\xD0\xBE\xD0\xB6\xD0\xB5\xD1\x82\xD0\xB5 \xD0\xB2\xD0\xBD\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8 \xD1\x81\xD0\xB0\xD0\xBC\xD0\xB8.")
					: std::string(one ? "One part" : "Some parts") + " of this change could "
						"not be placed in " + documentName + ", so " + (one ? "it is" : "they are")
						+ " not in the change put to you: " + steps + ". You can make "
						+ (one ? "that part" : "those parts") + " yourself.";
				raw["artifactAction"]["operations"] = kept;
				if (raw.contains("response") && raw.at("response").is_string())
					raw["response"] = raw.at("response").get<std::string>() + "\n\n" + note;
				// The card the lawyer approves says what the change holds - never a
				// part that is not in it.
				if (raw.at("artifactAction").contains("summary") && raw.at("artifactAction").at("summary").is_string())
					raw["artifactAction"]["summary"] = raw.at("artifactAction").at("summary").get<std::string>() + " " + note;
				if (responseProposal->artifact_action) responseProposal->artifact_action->summary += " " + note;
				result.text = raw.dump();
				responseProposal->response += "\n\n" + note;
				forceAskFirst();
				append_operation_event(task, "artifact.partially_placed", note, std::nullopt, "corrected");
				return true;
			};
			std::optional<ProposalRefusal> refusal = planOrRefusal();
			// One Singularity can no longer carry out is not put to the lawyer: they read
			// the answer they were shown, and that nothing was written.
			if (refusal && reoffer) {
				recordRefusal(*refusal);
				throw ArtifactNotPlaced(responseProposal->response, change_not_placed_note(matter,
					plan.target_document_id, plan.creates_new_artifact, written_in_russian(responseProposal->response)));
			}
			if (refusal && !freshProposal) throw std::runtime_error(refusal->message);
			// Put to the lawyer again, it waits for them whatever the Project's action
			// mode: "go ahead" answers the question, the approval makes the change.
			if (reoffer) forceAskFirst();
			if (refusal) {
				recordRefusal(*refusal);
				const std::string category = refusal->evidence ? refusal->evidence->error_category : "artifact_plan_refused";
				// One correction, through the same redraft request a review uses. It
				// may fix how the change is described or which supplied document it
				// names; it may not change what was asked for.
				const documents::ArtifactActionKind askedAction = responseProposal->artifact_action->action;
				const std::optional<std::string> askedTarget = refusal->evidence ? plan.target_document_id : std::nullopt;
				const bool askedFirst = responseProposal->artifact_action->ask_first;
				// What the lawyer reads if the change still cannot be placed: the answer
				// the model gave with it, and one calm sentence naming the document
				// nothing was written to.
				const std::string firstAnswer = responseProposal->response;
				const auto notPlaced = [&](const std::string &answer) {
					std::optional<std::string> documentId = askedTarget;
					if (!documentId) documentId = plan.target_document_id;
					if (!documentId) documentId = prepared.professional_artifact_candidates.existing_target_document_id;
					return ArtifactNotPlaced(answer, change_not_placed_note(matter, documentId,
						askedAction == documents::ArtifactActionKind::CreateContent, written_in_russian(answer)));
				};
				// A document that changed under the proposal is not the model's to
				// repair; the tool's reason is written for the model and stays in the
				// record, and the lawyer keeps the answer.
				if (category == "artifact_target_changed") throw notPlaced(firstAnswer);
				documents::ProfessionalReviewResult repair;
				repair.verdict = documents::ProfessionalReviewVerdict::NeedsRedraft;
				documents::ProfessionalReviewIssue issue;
				issue.category = documents::ReviewIssueCategory::DraftQuality;
				issue.message = refusal->message;
				issue.suggested_next_action = "Propose the same action for the same request again, correcting only what "
					"made it impossible to carry out, against a document supplied to you.";
				repair.issues.push_back(issue);
				const routing::InferenceRequest correctionRequest = redraftRequest(
					documents::professional_artifact_correction_instruction(plan, repair,
						*responseProposal->artifact_action, result.text,
						"Singularity could not carry out the proposed action, so nothing was written or put to the lawyer:"));
				const std::optional<routing::InferenceResult> correctionAttempt = callKeepingDraft(correctionRequest);
				if (!correctionAttempt) throw notPlaced(firstAnswer);
				const routing::InferenceResult &correctionResult = *correctionAttempt;
				task.usage = merge_usage_metadata(task.usage, correctionResult.usage);
				append_tool_call_evidence(task.tool_call_evidence, correctionResult.tool_call_evidence);
				const auto corrected = documents::parse_professional_response_proposal(correctionResult.text);
				// A correction that follows the advice and states the change in its
				// answer instead is that answer, with its own work product and
				// findings; one that cannot be read, or that asks for something else,
				// leaves the answer the model first gave.
				if (corrected && corrected->action_type == documents::ArtifactActionKind::NoAction) {
					if (corrected->work_product_json) workProductJson = corrected->work_product_json;
					if (corrected->structured_results_json) structuredResultsJson = corrected->structured_results_json;
					throw notPlaced(corrected->response);
				}
				if (!corrected || !corrected->artifact_action || corrected->artifact_action->action != askedAction)
					throw notPlaced(firstAnswer);
				result.text = correctionResult.text;
				// Research the correction itself ran is served research of this
				// operation, so repeating that request is not an open gap.
				result.fulfilled_research_requests.insert(result.fulfilled_research_requests.end(),
					correctionResult.fulfilled_research_requests.begin(), correctionResult.fulfilled_research_requests.end());
				for (const research::ExternalEvidenceReference &evidence : correctionResult.external_evidence)
					if (std::none_of(result.external_evidence.begin(), result.external_evidence.end(),
							[&](const research::ExternalEvidenceReference &known) { return known.evidence_id == evidence.evidence_id; }))
						result.external_evidence.push_back(evidence);
				*responseProposal = *corrected;
				if (askedFirst) forceAskFirst();
				applyOpenResearchRequest(*responseProposal);
				settleSummary();
				refusal = planOrRefusal();
				if (refusal) {
					recordRefusal(*refusal);
					if (!keepPlaceableSteps(askedFirst)) throw notPlaced(firstAnswer);
				}
				if (askedTarget && plan.target_document_id != askedTarget) {
					recordRefusal(ProposalRefusal{"The corrected change was aimed at a different document than the one "
						"first proposed, so nothing was written.", std::nullopt});
					throw notPlaced(firstAnswer);
				}
				// Only a correction that can be carried out becomes the work product
				// the lawyer sees beside its answer.
				if (responseProposal->work_product_json) workProductJson = responseProposal->work_product_json;
				append_operation_event(task, "artifact.corrected",
					"The proposed action was corrected and can be carried out.", std::nullopt, "corrected");
			}
			requireFiguresWorkedOut();
			if (freshProposal) noteUnrestoredSymbols();
			// Preserve the separate, explicit target binding before an approval
			// boundary can suspend this operation. A fresh process can then resume
			// the same target without reinterpreting any selected source as one.
			if (!plan.creates_new_artifact) task.professional_target_document_id = plan.target_document_id;
			// Closes a real observability gap found this session (Routing/
			// Observability's own execution-progress acceptance): the generic
			// artifact create/append/replace/update_cells path previously had
			// zero OperationEvent instrumentation anywhere, unlike DirectDocument
			// Work/ConnectedDocumentAction's own DocumentActionProposed - a
			// console/UI progress consumer had no live signal at all between
			// "the model answered" and "waiting_for_approval appeared". Emitted
			// only once the plan has already validated the declared format/
			// action/target against real capability (build_professional_
			// artifact_plan() above already either threw or returned a genuine
			// plan) - never a raw, unvalidated model claim.
			routing::observability::emit({routing::observability::OperationEventKind::DocumentActionProposed,
				"Resolving a generic artifact action proposal",
				{{"format", documents::to_string(plan.format)}, {"createsNewArtifact",
					plan.creates_new_artifact ? "true" : "false"}}, std::nullopt});
			if (decision == "rejected") throw std::runtime_error("The requested artifact action was denied.");
			// Wired freshly here for the exact same reason as DirectDocumentWork's
			// own branch above - a fresh attempt has no checkpoint yet, so the
			// pre-generation wiring (gated on a RESUME whose checkpoint.draft
			// ["kind"] already matches) never fires for it.
			prepared.request.tool_execution_context->mutation_prepared =
				[workspace_path, matter_id, &task](const std::string &record) {
					const Json parsed = Json::parse(record);
					if (!task.document_operation_checkpoint)
						task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
					task.document_operation_checkpoint->stage = "commit_prepared";
					task.document_operation_checkpoint->mutation_commit = parsed;
					task.operation_stage = "commit_prepared";
					task.operation_updated_at = workspaces::current_timestamp();
					append_operation_event(task, "commit.prepared", "A validated artifact action was prepared.",
						parsed.value("artifactActionId", std::string()));
					persist_task_checkpoint(workspace_path, matter_id, task);
				};
			prepared.request.tool_execution_context->mutation_committed =
				[workspace_path, matter_id, &task](const std::string &record) {
					const Json parsed = Json::parse(record);
					if (!task.document_operation_checkpoint)
						task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
					// One write is one step: recorded as written when the file is
					// committed, and again once its new version is reconciled - the
					// second time adds its version, not a second step.
					const bool alreadyCommitted = task.document_operation_checkpoint->stage == "committed";
					task.document_operation_checkpoint->stage = "committed";
					// The commit record LAYERS onto the prepared one rather than
					// replacing it: the prepared record is the only durable
					// statement of what this change was meant to do, and a
					// process interrupted anywhere around the commit can only be
					// reconciled against it (recover_prepared_document_mutation()
					// refuses a record with no arguments at all).
					if (task.document_operation_checkpoint->mutation_commit.is_object())
						task.document_operation_checkpoint->mutation_commit.update(parsed);
					else task.document_operation_checkpoint->mutation_commit = parsed;
					task.operation_stage = "committed";
					task.operation_updated_at = workspaces::current_timestamp();
					if (!alreadyCommitted)
						append_operation_event(task, "commit.completed", "The artifact action was committed and reconciled.",
							parsed.value("artifactActionId", std::string()), "committed");
					persist_task_checkpoint(workspace_path, matter_id, task);
				};
			// Independent Professional Review gate, ported from ConnectedDocumentAction's
			// Professional Completion Challenge (see ProfessionalArtifactAction.hpp's own
			// comment on ProfessionalArtifactActionPlan::requires_professional_review).
			// Runs only once, on the FIRST fresh generation for this operation.
			// `artifactResultAlreadyGenerated` (computed above) is exactly "a prior
			// attempt already produced and durably checkpointed a rawProviderResult
			// for this task" - written only AFTER this loop has already passed (or
			// was not required): either by recordGeneratedBeforeMutation() right
			// before the real write, or by the ApprovalRequired catch below - so
			// whenever it is true, any required review already passed and must
			// never run again; resuming a
			// generated-but-not-yet-committed or already-decided operation must cost
			// zero further provider calls (commitAlreadyRecovered guards the crash-
			// recovery-from-a-live-commit case the same way).
			std::optional<documents::ProfessionalReviewResult> passedReview;
			// The review still had doubts after its correction rounds: what it
			// last said is kept with the draft, so the lawyer can read it beside
			// the change they are asked about.
			bool reviewUnresolved = false;
			std::optional<documents::ProfessionalReviewResult> unresolvedReview;
			if (!artifactResultAlreadyGenerated && !commitAlreadyRecovered && plan.requires_professional_review) {
				// Real sources actually in this request's own assembled context -
				// canonical document id + real name, never a filename-only guess and
				// never proposal.content itself. See professional_artifact_review_
				// instruction()'s own comment: this is what lets the reviewer give
				// EVERY source an honest disposition instead of inferring from a
				// bare count that every one of them must materially appear.
				std::vector<std::pair<std::string, std::string>> availableSources;
				for (const std::string &documentId : prepared.distinct_context_document_ids) {
					const auto found = std::find_if(matter.documents.begin(), matter.documents.end(),
						[&](const matters::MatterDocument &document) { return document.id == documentId; });
					if (found != matter.documents.end()) availableSources.emplace_back(documentId, found->name);
				}
				// The reviewer judges the proposal against the same canonical evidence
				// the drafter received: every passage of this operation's marker
				// universe (a targeted continuation's included), admitted capacity-
				// first for the review's own output reservation with its coverage
				// statement, the review's machine-owned rules in CAPABILITY and the
				// objective, proposal and sources in CURRENT_REQUEST.
				const std::optional<std::size_t> reviewContextWindowTokens =
					routing::estimate_model_context_window_tokens(prepared.request.model_target);
				std::optional<std::string> reviewCompletionNote;
				// Set once a correction is accepted: the next review checks that
				// correction rather than searching the revision afresh.
				std::optional<std::string> revisionReviewNote;
				for (std::size_t attempt = 0;; ++attempt) {
					context::PromptAssemblyInput reviewInput = prepared.prompt_input;
					const context::SourceMarkerRegistry reviewMarkers = operationEvidenceChunks(reviewInput);
					reviewInput.required_source_origin = reviewEvidenceOrigin;
					reviewInput.capability_instructions = documents::professional_artifact_review_capability_instruction(plan);
					// The reviewer reads the proposal as the drafter wrote it - its figures
					// as statements over protected values - and is told what they are. Its
					// verdict has no localFacts: it judges each statement as written.
					if (prepared.compute_protocol_offered)
						*reviewInput.capability_instructions += " Values written [[Amount3]], [[Date4]] \xE2\x80\xA6 are the "
							"lawyer's protected facts; protectionMetadata.symbols gives each one's type and role, and for one "
							"Singularity worked out, how it was worked out. This review cannot ask Singularity for operations: judge "
							"each figure by its symbol and derivation.";
					else if (prepared.computed_statements_offered)
						*reviewInput.capability_instructions += std::string(" ") + context::computed_statements::kInstruction
							+ " This review cannot ask Singularity for facts: judge each statement as written, and each symbol "
							"Singularity worked out by the LOCAL FACTS that say what it was worked out from.";
					reviewInput.candidate_context.reset();
					reviewInput.history_items.clear();
					reviewInput.history.reset();
					reviewInput.plain_chat = false;
					std::vector<documents::ProfessionalReviewSource> reviewSources;
					for (const auto &[documentId, name] : availableSources) {
						documents::ProfessionalReviewSource source{documentId, name, std::string()};
						for (const context::ResolvedSourceReference &reference : reviewMarkers.references()) {
							if (reference.document_id != documentId) continue;
							source.handle = reference.document_token;
							break;
						}
						reviewSources.push_back(std::move(source));
					}
					std::optional<std::string> reviewNote = reviewCompletionNote ? reviewCompletionNote : revisionReviewNote;
					if (restorationReviewNote) reviewNote = reviewNote ? *reviewNote + "\n" + *restorationReviewNote : *restorationReviewNote;
					reviewInput.current_request = documents::professional_artifact_review_request(
						*responseProposal->artifact_action, spec.prompt + lawyer_answers_for_review(task), reviewSources,
						result.external_evidence, researchLimitation, reviewNote, responseProposal->response)
						+ local_facts_for_later_call(*operationLocalFacts);
					reviewInput.maximum_context_tokens.reset();
					reviewInput.estimate_tokens = nullptr;
					apply_capacity_first_budget(reviewInput, reviewContextWindowTokens, kProfessionalReviewMaxOutputTokens);
					reviewInput.continuation_headroom_tokens = 0;
					routing::InferenceRequest reviewRequest;
					reviewRequest.call_purpose = routing::observability::CallPurpose::ProfessionalReview;
					reviewRequest.model_target = prepared.request.model_target;
					reviewRequest.protected_api_policy_override = prepared.request.protected_api_policy_override;
					// The review is protected with the same Project entity identities as the
					// drafting request it judges.
					reviewRequest.project_entity_hints = prepared.request.project_entity_hints;
					// And with the same symbols: a value of the draft is the symbol the
					// draft's own call wrote it with.
					reviewRequest.operation_symbols = prepared.request.operation_symbols;
					const context::PromptAssemblyResult reviewAssembly = context::PromptAssembler::assemble_with_budget(reviewInput);
					reviewRequest.prompt = reviewAssembly.prompt;
					// The documents whose passages this review actually received: only
					// they can be judged, so only they owe a disposition.
					std::unordered_set<std::string> reviewedDocumentIds;
					{
						std::unordered_map<std::string, std::string> documentOfPassage;
						for (const context::RetrievedContextChunk &passage : prepared.document_context)
							documentOfPassage.emplace(passage.chunk.chunk_id, passage.document_id);
						for (const context::PromptBudgetItem &item : reviewAssembly.budget.included) {
							if (item.category != "explicit-source" && item.category != "retrieved-source") continue;
							const auto document = documentOfPassage.find(item.id);
							if (document != documentOfPassage.end()) reviewedDocumentIds.insert(document->second);
						}
					}
					reviewRequest.research_mode = research::ResearchMode::Off;
					// Every source the review received owes a disposition, and the
					// contract itself asks for exactly those, by the handle the prompt
					// names each by: Singularity's own ids never leave the computer.
					const auto handleOf = [&](const std::string &documentId) {
						for (const documents::ProfessionalReviewSource &source : reviewSources)
							if (source.document_id == documentId && !source.handle.empty()) return source.handle;
						return documentId;
					};
					std::vector<std::string> owedDispositions;
					for (const auto &source : availableSources)
						if (reviewedDocumentIds.count(source.first) != 0) owedDispositions.push_back(handleOf(source.first));
					reviewRequest.research_final_output_contract =
						documents::professional_artifact_review_output_contract(owedDispositions);
					// The room its window really has left beside this prompt, as the
					// assembler measured it, up to the call's own ceiling; with the window
					// unknown, that ceiling - as the professional response itself is sent.
					std::size_t reviewOutputTokens = reviewAssembly.budget.output_tokens.value_or(kProfessionalReviewMaxOutputTokens);
					if (!reviewContextWindowTokens) {
						reviewOutputTokens = std::max(reviewOutputTokens, kProfessionalReviewCallMaxOutputTokens);
					} else if (reviewAssembly.budget.assembled_tokens > 0) {
						const std::size_t used = reviewAssembly.budget.assembled_tokens
							+ research::estimate_tokens(routing::conversational_model_turn(std::string()).serialize());
						if (*reviewContextWindowTokens > used)
							reviewOutputTokens = std::max(reviewOutputTokens,
								std::min(kProfessionalReviewCallMaxOutputTokens, *reviewContextWindowTokens - used));
					}
					reviewRequest.maximum_output_tokens = reviewOutputTokens;
					const auto reviewStarted = std::chrono::steady_clock::now();
					const std::optional<routing::InferenceResult> reviewAttempt = callKeepingDraft(reviewRequest);
					const double reviewMs = std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - reviewStarted).count();
					if (!reviewAttempt) {
						append_operation_event(task, "review.unresolved",
							"The review could not be completed, so the draft was put to the lawyer to decide.",
							std::nullopt, "unresolved");
						reviewUnresolved = true;
						break;
					}
					const routing::InferenceResult &reviewResult = *reviewAttempt;
					task.usage = merge_usage_metadata(task.usage, reviewResult.usage);
					auto review = documents::parse_professional_artifact_review(reviewResult.text);
					// The review names sources by their handles: each is read as the
					// document it stands for.
					if (review) {
						const auto documentOf = [&](const std::string &named) {
							std::string wanted;
							for (const char character : named)
								if (character != ' ' && character != '\t')
									wanted.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
							for (const documents::ProfessionalReviewSource &source : reviewSources)
								if (!source.handle.empty() && source.handle == wanted) return source.document_id;
							return named;
						};
						for (documents::ProfessionalReviewSourceDisposition &disposition : review->source_dispositions)
							disposition.document_id = documentOf(disposition.document_id);
						for (documents::ProfessionalReviewIssue &issue : review->issues)
							if (issue.document_id) issue.document_id = documentOf(*issue.document_id);
					}
					// The review is a helper, not a wall: a reply that cannot be read
					// judges nothing, and the draft it was asked about - already checked
					// against the document - goes to the lawyer to decide, never thrown
					// away for the reviewer's fault.
					if (!review) {
						append_operation_event(task, "review.unresolved",
							"The review's reply could not be read, so the draft was put to the lawyer to decide.",
							std::nullopt, "unresolved");
						reviewUnresolved = true;
						break;
					}
					// Every round of the review is a fact of this operation: its verdict
					// and what it asked to change, kept with the operation, so a
					// correction is traced to the criticism that caused it. The trace
					// records only a fingerprint of each criticism, so a repeated one
					// shows as repeated without its words.
					{
						const std::string asked = review_issues_for_people(*review, reviewMarkers.references(),
							result.external_evidence, matter);
						append_operation_event(task, "review.round",
							asked.empty() ? std::string("Singularity's review found nothing to change.") : asked,
							std::nullopt, documents::to_string(review->verdict));
						if (platform::trace::enabled()) {
							std::string categories;
							std::string fingerprints;
							for (const documents::ProfessionalReviewIssue &issue : review->issues) {
								categories += std::string(categories.empty() ? "" : ",") + documents::to_string(issue.category);
								fingerprints += (fingerprints.empty() ? "" : ",") + platform::trace::fingerprint(issue.message);
							}
							platform::trace::record("review", "round", {{"attempt", attempt},
								{"verdict", documents::to_string(review->verdict)}, {"issues", review->issues.size()},
								{"categories", categories}, {"fingerprints", fingerprints},
								{"dispositions", review->source_dispositions.size()}, {"owed", owedDispositions.size()},
								{"revision", revisionReviewNote.has_value()},
								{"promptBytes", reviewRequest.prompt.size()}, {"outputTokens", reviewOutputTokens}}, reviewMs);
						}
					}
					// Singularity's own structural completeness check: every real source
					// this request actually saw must receive SOME disposition (a
					// runtime-owned coverage fact) - never that its disposition must
					// be used_as_evidence (a semantic judgment that stays entirely
					// the model's own). A pass that silently skipped judging a real
					// source is treated exactly like an incomplete review, not
					// trusted at face value.
					const bool dispositionCoverageComplete = std::all_of(availableSources.begin(),
						availableSources.end(), [&](const std::pair<std::string, std::string> &source) {
							return reviewedDocumentIds.count(source.first) == 0
								|| std::any_of(review->source_dispositions.begin(), review->source_dispositions.end(),
									[&](const documents::ProfessionalReviewSourceDisposition &disposition) {
										return disposition.document_id == source.first;
									});
						});
					if (review->verdict == documents::ProfessionalReviewVerdict::Pass && dispositionCoverageComplete) {
						passedReview = review;
						// A disposition for a document whose passages the review never
						// received would be provenance nobody established.
						passedReview->source_dispositions.erase(std::remove_if(passedReview->source_dispositions.begin(),
							passedReview->source_dispositions.end(),
							[&](const documents::ProfessionalReviewSourceDisposition &disposition) {
								return reviewedDocumentIds.count(disposition.document_id) == 0; }),
							passedReview->source_dispositions.end());
						break;
					}
					// A pass that left a real source without a disposition is an
					// incomplete REVIEW, not a defect of the proposal: within the same
					// cycle budget the review is asked again for the sources it skipped,
					// and the proposal is never redrafted over the reviewer's omission.
					const bool incompletePass = review->verdict == documents::ProfessionalReviewVerdict::Pass;
					if (incompletePass && attempt < documents::kMaxProfessionalArtifactReviewCycles) {
						std::string skipped;
						for (const auto &source : availableSources)
							if (reviewedDocumentIds.count(source.first) != 0
									&& std::none_of(review->source_dispositions.begin(), review->source_dispositions.end(),
									[&](const documents::ProfessionalReviewSourceDisposition &disposition) {
										return disposition.document_id == source.first; }))
								skipped += " id=" + source.first;
						reviewCompletionNote = "Your previous review returned pass but gave no sourceDispositions entry for:"
							+ skipped + ". Review again and give every source listed above exactly one disposition.";
						continue;
					}
					// The note described that incomplete pass only; a later review of a
					// revised proposal is never told its previous review passed.
					reviewCompletionNote.reset();
					if (attempt >= documents::kMaxProfessionalArtifactReviewCycles) {
						// The review is a helper, not a wall: after its correction rounds the
						// best draft goes to the lawyer as a proposal to look at - never
						// written on its own, never a failure. The lawyer decides; the
						// corrections so far are not thrown away (owner, 2026-09-23).
						reviewUnresolved = true;
						unresolvedReview = *review;
						break;
					}
					// NeedsMoreEvidence gets a bounded, TARGETED completion turn (the
					// accepted draft + only the source(s) the review itself named) -
					// never a full redraft with the entire original Project context
					// resent. NeedsRedraft (or a review whose issues name no specific
					// source at all) keeps the full-redraft path: the defect is in
					// the drafting itself, not in which evidence was consulted.
					// Only a source this request itself already had in scope can be
					// revisited: a review-named id outside availableSources (an
					// unselected Project document, or an invented id) must never pull
					// new content into the model context. A named source whose current
					// bytes cannot be read is skipped too; with nothing left to revisit,
					// the round falls back to the full redraft below.
					std::vector<std::pair<std::string, std::string>> revisitedDocuments;
					std::vector<context::RetrievedContextChunk> revisitedPassages;
					if (review->verdict == documents::ProfessionalReviewVerdict::NeedsMoreEvidence) {
						std::vector<std::string> namedDocumentIds;
						for (const documents::ProfessionalReviewIssue &issue : review->issues)
							if (issue.document_id
									&& std::find(namedDocumentIds.begin(), namedDocumentIds.end(), *issue.document_id)
										== namedDocumentIds.end())
								namedDocumentIds.push_back(*issue.document_id);
						for (const std::string &documentId : namedDocumentIds) {
							const auto inScope = std::find_if(availableSources.begin(), availableSources.end(),
								[&](const std::pair<std::string, std::string> &source) { return source.first == documentId; });
							if (inScope == availableSources.end()) continue;
							std::vector<context::RetrievedContextChunk> passages;
							try {
								passages = matters::retrieve_document_context(workspace_path, matter, spec.prompt,
									{documentId}, nullptr, matters::SelectedSourceDelivery::EveryPassage);
							} catch (const std::exception &) {
								continue;
							}
							passages.erase(std::remove_if(passages.begin(), passages.end(),
								[&](const context::RetrievedContextChunk &passage) { return passage.document_id != documentId; }),
								passages.end());
							if (passages.empty()) continue;
							revisitedDocuments.emplace_back(documentId, inScope->second);
							revisitedPassages.insert(revisitedPassages.end(), passages.begin(), passages.end());
						}
					}
					const bool useTargetedCompletion = !revisitedPassages.empty();
					// Set only when a targeted continuation is accepted: its passages join
					// the provenance registry the committed result is rendered from.
					std::optional<context::SourceMarkerRegistry> continuationMarkers;
					std::vector<context::RetrievedContextChunk> continuationMarkerBasis;
					std::unordered_set<std::string> continuationSuppliedIds;
					std::optional<routing::InferenceResult> correctionAttempt;
					const auto correctionStarted = std::chrono::steady_clock::now();
					// External evidence already retrieved in this operation is carried
					// into every correction round (never re-researched, never lost from
					// the review or the task) - see ResearchToolLoop.hpp's handling of
					// operationInitialExternalEvidence in escalation mode.
					if (useTargetedCompletion) {
						// The continuation receives what the review pointed to the way the
						// first call received its evidence: the same SYSTEM/TASK/CAPABILITY
						// framing (so the allowed action kinds still apply), every current
						// passage of the named document(s) admitted by the same capacity-
						// first budget, and markers numbered consistently with the first
						// call's, so the proposal's preserved citations stay valid and new
						// ones resolve.
						continuationMarkerBasis = prepared.document_context;
						std::unordered_set<std::string> knownPassages;
						for (const context::RetrievedContextChunk &passage : continuationMarkerBasis)
							knownPassages.insert(passage.chunk.chunk_id);
						std::vector<std::string> revisitedIds;
						for (const auto &document : revisitedDocuments) revisitedIds.push_back(document.first);
						const auto revisitPriority = ordered_for_capacity_first_delivery(revisitedPassages, revisitedIds);
						for (const context::RetrievedContextChunk &passage : revisitedPassages)
							if (knownPassages.insert(passage.chunk.chunk_id).second) continuationMarkerBasis.push_back(passage);
						const context::SourceMarkerRegistry markers(continuationMarkerBasis);
						context::PromptAssemblyInput targetedInput = prepared.prompt_input;
						targetedInput.required_source_origin = reviewEvidenceOrigin;
						targetedInput.source_chunks.clear();
						for (const context::RetrievedContextChunk &passage : revisitedPassages)
							targetedInput.source_chunks.push_back({passage.chunk.chunk_id, markers.format_chunk(passage), true,
								passage.document_id, revisitPriority.at(passage.chunk.chunk_id),
								markers.document_token_for(passage.chunk.chunk_id), passage.document_title});
						const std::string completion = documents::professional_artifact_targeted_evidence_completion_instruction(
							*responseProposal->artifact_action, result.text, *review, revisitedDocuments);
						// The completion text is required content of this continuation,
						// under whichever bound the assembly applies - with every answer
						// the lawyer already gave this operation before it.
						const std::string appended = answered_lawyer_questions_text(task)
							+ local_facts_for_later_call(*operationLocalFacts) + "\n\n" + completion;
						if (targetedInput.maximum_context_tokens) {
							const std::size_t required = research::estimate_serialized_tokens(appended);
							targetedInput.maximum_context_tokens = *targetedInput.maximum_context_tokens > required
								? *targetedInput.maximum_context_tokens - required : 0;
						}
						const std::size_t requiredBytes = appended.size();
						targetedInput.maximum_context_bytes = targetedInput.maximum_context_bytes > requiredBytes
							? targetedInput.maximum_context_bytes - requiredBytes : 0;
						const auto targetedAssembly = context::PromptAssembler::assemble_with_budget(targetedInput);
						for (const context::PromptBudgetItem &item : targetedAssembly.budget.included)
							if (item.category == "explicit-source" || item.category == "retrieved-source")
								continuationSuppliedIds.insert(item.id);
						continuationMarkers = markers;
						routing::InferenceRequest targetedRequest = prepared.request;
						targetedRequest.local_facts_step_contract.reset();
						targetedRequest.project_evidence_calls_from_final_text = nullptr;
						targetedRequest.maximum_project_evidence_requests = 0;
						targetedRequest.operation_initial_external_evidence = result.external_evidence;
						targetedRequest.call_purpose = routing::observability::CallPurpose::EvidenceExpansion;
						targetedRequest.maximum_lawyer_questions = remaining_lawyer_questions(task);
						targetedRequest.prompt = targetedAssembly.prompt + appended;
						if (targetedAssembly.budget.output_tokens)
							targetedRequest.maximum_output_tokens = *targetedAssembly.budget.output_tokens;
						correctionAttempt = callKeepingDraft(targetedRequest);
					} else {
						const routing::InferenceRequest correctionRequest = redraftRequest(
							documents::professional_artifact_correction_instruction(plan, *review,
								*responseProposal->artifact_action, result.text));
						correctionAttempt = callKeepingDraft(correctionRequest);
					}
					platform::trace::record("review", "correction", {{"attempt", attempt}, {"targeted", useTargetedCompletion},
						{"delivered", correctionAttempt.has_value()}}, std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - correctionStarted).count());
					// A revision the provider could not deliver leaves the reviewed
					// draft to the lawyer, with what the review last said.
					if (!correctionAttempt) {
						append_operation_event(task, "review.unresolved",
							"The revised draft could not be prepared, so the reviewed draft was put to the lawyer to decide.",
							std::nullopt, "unresolved");
						reviewUnresolved = true;
						unresolvedReview = *review;
						break;
					}
					const routing::InferenceResult &correctionResult = *correctionAttempt;
					task.usage = merge_usage_metadata(task.usage, correctionResult.usage);
					append_tool_call_evidence(task.tool_call_evidence, correctionResult.tool_call_evidence);
					const auto correctedResponse = documents::parse_professional_response_proposal(correctionResult.text);
					// The review is a helper, not a wall. A correction that cannot be
					// read, that proposes no change at all, or that turns the reviewed
					// proposal into a different action (a create into an update of an
					// artifact that does not exist yet) is no revision of it: the draft
					// the review judged goes to the lawyer to decide, with what the review
					// last said - never a failure that throws the drafted work away.
					if (!correctedResponse || !correctedResponse->artifact_action
							|| correctedResponse->artifact_action->action != responseProposal->artifact_action->action) {
						recordRefusal(ProposalRefusal{!correctedResponse || !correctedResponse->artifact_action
							? std::string("The revised draft proposed no change, so the reviewed draft was kept.")
							: std::string("The revised draft changed its action from ")
								+ documents::to_string(responseProposal->artifact_action->action) + " to "
								+ documents::to_string(correctedResponse->artifact_action->action)
								+ ", so the reviewed draft was kept.", std::nullopt});
						reviewUnresolved = true;
						unresolvedReview = *review;
						break;
					}
					// What the correction replaces, so a revision Singularity cannot carry out
					// leaves the draft it revised exactly as it was.
					const documents::ProfessionalResponseProposal previousProposal = *responseProposal;
					const std::string previousText = result.text;
					const documents::ProfessionalArtifactActionPlan previousPlan = plan;
					const std::optional<std::string> previousWorkProduct = workProductJson;
					const std::optional<std::string> previousResearchLimitation = researchLimitation;
					const std::optional<nlohmann::json> previousUnservedResearch = unservedResearchRequest;
					const bool askedFirst = responseProposal->artifact_action->ask_first;
					if (correctedResponse->work_product_json) workProductJson = correctedResponse->work_product_json;
					result.text = correctionResult.text;
					result.fulfilled_research_requests.insert(result.fulfilled_research_requests.end(),
						correctionResult.fulfilled_research_requests.begin(), correctionResult.fulfilled_research_requests.end());
					for (const research::ExternalEvidenceReference &evidence : correctionResult.external_evidence)
						if (std::none_of(result.external_evidence.begin(), result.external_evidence.end(),
								[&](const research::ExternalEvidenceReference &known) { return known.evidence_id == evidence.evidence_id; }))
							result.external_evidence.push_back(evidence);
					*responseProposal = *correctedResponse;
					if (askedFirst) forceAskFirst();
					applyOpenResearchRequest(*responseProposal);
					settleSummary();
					// The revision is planned, settled and checked against the document
					// exactly as the draft it replaces was, before it is reviewed again or
					// put to the lawyer: a revision Singularity could not carry out never
					// fails the request at the write, and a table edit is never shown one
					// way and written another. One it cannot carry out leaves the reviewed
					// draft in place.
					if (const std::optional<ProposalRefusal> refused = planOrRefusal()) {
						recordRefusal(*refused);
						*responseProposal = previousProposal;
						result.text = previousText;
						plan = previousPlan;
						workProductJson = previousWorkProduct;
						researchLimitation = previousResearchLimitation;
						unservedResearchRequest = previousUnservedResearch;
						reviewUnresolved = true;
						unresolvedReview = *review;
						break;
					}
					if (continuationMarkers) {
						prepared.supplied_chunk_ids.insert(continuationSuppliedIds.begin(), continuationSuppliedIds.end());
						prepared.supplied_markers = continuationMarkers->restricted_to(prepared.supplied_chunk_ids);
						prepared.document_context = std::move(continuationMarkerBasis);
						evidenceBasisExtended = true;
					}
					noteUnrestoredSymbols();
					revisionReviewNote = documents::professional_artifact_revision_review_note(*review);
				}
			}
			// A revision the review asked for is held to the same rule.
			requireFiguresWorkedOut();
			// What the lawyer approves and what is committed carry lawyer-facing
			// source labels, never internal handles. A resumed operation renders
			// from the references it recorded when this proposal was accepted
			// (never a fresh retrieval's marker numbering); the labels themselves
			// always come from current Project metadata.
			artifactCitations = documents::render_artifact_source_labels(*responseProposal->artifact_action, plan.format,
				artifactResultAlreadyGenerated ? task.source_references : prepared.supplied_markers.references(),
				matter);
			// The answer the lawyer reads beside the change is the model's own
			// response - what it found and on what evidence - and what it cites is
			// this operation's provenance as much as what the change itself cites:
			// recorded together, so a resumed operation renders both from the
			// references it had when the proposal was made, never from a fresh
			// retrieval's numbering.
			{
				const std::vector<context::ResolvedSourceReference> answered = artifactResultAlreadyGenerated
					? task.source_references
					: context::cited_references(prepared.supplied_markers.validate_response(responseProposal->response));
				for (const context::ResolvedSourceReference &reference : answered)
					if (std::none_of(artifactCitations->begin(), artifactCitations->end(),
							[&](const context::ResolvedSourceReference &known) { return known.chunk_id == reference.chunk_id; }))
						artifactCitations->push_back(reference);
			}
			task.source_references = *artifactCitations;
			// A draft the review did not fully clear waits for the lawyer whatever
			// the Project's action mode, exactly as one they asked to see first.
			// Written into the saved result as well, so the run that resumes once
			// they approve rebuilds the very action they approved.
			if (reviewUnresolved) forceAskFirst();
			// The floor under Full access and Apply routine changes, read from the
			// lawyer's own words rather than the model's account of them: a change
			// they asked to see first, or one whose requestedBy is not words they
			// wrote in this request, waits for them - so a question is never
			// answered by changing a file. Declared as askFirst, so the run that
			// resumes once they approve rebuilds the very action they approved.
			if (freshProposal && !responseProposal->artifact_action->ask_first) {
				const std::string floorReason = approval_floor(task, responseProposal->artifact_action->requested_by);
				if (!floorReason.empty()) {
					forceAskFirst();
					append_operation_event(task, "approval.floor", floorReason == "asked_first"
						? "You asked to see this change before it is made, so it waits for your approval."
						: "Your message did not ask for this change in so many words, so it waits for your approval.",
						std::nullopt, floorReason);
				}
			}
			const documents::ProfessionalArtifactProposal &proposal = *responseProposal->artifact_action;
			routing::observability::emit({routing::observability::OperationEventKind::DocumentPatchValidated,
				"Professional artifact action validated",
				{{"format", documents::to_string(plan.format)},
				 {"mode", plan.creates_new_artifact ? "create" : "update"}}, std::nullopt});
			// The review verdict, once passed, must survive whichever checkpoint
			// this operation ends up persisting (a pending-approval checkpoint
			// below, or none at all for an immediate-execute permission mode) so
			// ChatConversation.cpp's professional_review_from_task() can still
			// project it later - never lost by an unrelated checkpoint overwrite.
			// A change put to the lawyer again carries the review its own turn gave it.
			const Json reviewJson = [&] {
				if (passedReview) return documents::professional_artifact_review_json(*passedReview, 1);
				if (unresolvedReview) return documents::professional_artifact_review_json(*unresolvedReview, 1);
				Json carried = Json::object();
				if (reoffer)
					for (const char *key : {"verdict", "issues", "sourceDispositions", "reviewedRevision", "revision",
							"professionalReview", "professionalReviewReviewedRevision"})
						if (reoffer->draft.contains(key)) carried[key] = reoffer->draft.at(key);
				return carried;
			}();
			// What that review still doubted is shown with it again.
			std::optional<documents::ProfessionalReviewResult> carriedDoubts;
			if (reoffer && reviewJson.contains("professionalReview") && reviewJson.at("professionalReview").is_object()
					&& reviewJson.at("professionalReview").value("verdict", std::string("pass")) != "pass")
				carriedDoubts = documents::parse_professional_artifact_review(reviewJson.at("professionalReview").dump());
			documents::ProfessionalArtifactActionResult action;
			if (commitAlreadyRecovered) {
				const Json &commit = task.document_operation_checkpoint->mutation_commit;
				action.response_text = proposal.summary;
				action.document_id = commit.at("documentId").get<std::string>();
				action.title = proposal.title;
				action.format = plan.format;
				action.resulting_version = commit.at("newVersion").get<int>();
			} else {
				// IDEM-1: durable after any Professional Review or correction has
				// settled what this operation actually produced, and before the
				// file is touched - so a resume restores the result that was
				// reviewed, never the draft that preceded it.
				recordGeneratedBeforeMutation("professional_artifact_action");
				try {
					if (decision == "approved" && task.document_operation_checkpoint)
						require_approved_target_unchanged(task.document_operation_checkpoint->draft,
							plan.target_document_id, plan.expected_version, plan.expected_fingerprint);
					action = documents::apply_professional_artifact_action(
						plan, proposal, *prepared.request.tool_execution_context, decision == "approved");
				} catch (const documents::ArtifactActionRefused &refused) {
					// The refusal is a fact of this operation: recorded, never an
					// approval to ask for. Its words are written for the model, so the
					// lawyer reads that nothing was written, then the answer.
					task.tool_call_evidence.push_back(refused.evidence);
					append_operation_event(task, "artifact.refused", refused.what(), std::nullopt, "refused");
					throw ArtifactNotPlaced(responseProposal->response, change_not_placed_note(matter,
						plan.target_document_id, plan.creates_new_artifact, written_in_russian(responseProposal->response)));
				} catch (const documents::ArtifactActionApprovalRequired &approval) {
					documents::DocumentOperationCheckpoint checkpoint;
					checkpoint.stage = "generated";
					checkpoint.draft = reviewJson;
					checkpoint.draft["rawProviderResult"] = result.text;
					checkpoint.draft["kind"] = "professional_artifact_action";
					checkpoint.draft["approval"] = approval_binding_json(approval.request, result.text);
					if (researchLimitation) checkpoint.draft["researchLimitation"] = *researchLimitation;
					// Research this operation already ran before the approval pause is
					// a fact of the OPERATION, not of one attempt: persisted so the
					// resumed attempt still compares the accepted request as served
					// (requestUnserved()) instead of declaring a false limitation.
					checkpoint.draft["fulfilledResearchRequests"] = result.fulfilled_research_requests;
					record_supplied_evidence(checkpoint.draft, prepared);
					checkpoint.external_evidence = result.external_evidence;
					task.external_evidence = result.external_evidence;
					note_checkpoint_here(matter_id, spec.operation_id, checkpoint.draft);
					task.document_operation_checkpoint = std::move(checkpoint);
					task.tool_call_evidence.push_back(approval.evidence);
					if (task.operation_approval && task.operation_approval->approval_id == approval.request.approval_id) {
						attach_proposal_answer(*task.operation_approval, answerAsRead(responseProposal->response),
							task.source_references);
						if (reviewUnresolved && unresolvedReview)
							attach_review_note(*task.operation_approval, *unresolvedReview, task.source_references,
								result.external_evidence, matter);
						else if (carriedDoubts)
							attach_review_note(*task.operation_approval, *carriedDoubts, task.source_references,
								result.external_evidence, matter);
					}
					task.response_text = "Awaiting approval to create or update the requested Project artifact.";
					ownLine();
					persist_task_checkpoint(workspace_path, matter_id, task);
					return task;
				}
				task.tool_call_evidence.push_back(action.tool_call_evidence);
			}
			if (passedReview) {
				if (!task.document_operation_checkpoint) task.document_operation_checkpoint = documents::DocumentOperationCheckpoint{};
				for (const auto &[key, value] : reviewJson.items()) task.document_operation_checkpoint->draft[key] = value;
			}
			task.artifact_document_id = action.document_id;
			task.artifact_title = action.title;
			task.artifact_format = documents::to_string(action.format);
			task.artifact_change_kind = plan.creates_new_artifact ? "created" : "updated";
			task.artifact_resulting_version = action.resulting_version;
			task.artifact_change = documents::artifact_change_json(commitAlreadyRecovered
				? documents::describe_artifact_change(proposal) : action.change).dump();
			// What happened to which document is already said where a lawyer looks
			// for it - on the document's own band, in his own language. The answer
			// is his to read, so it is the model's own words; Singularity's own line
			// stands in only when the model wrote none.
			task.response_text = !responseProposal->response.empty() ? answerAsRead(responseProposal->response)
				: action.response_text.empty() ? (plan.creates_new_artifact ? "Created: " : "Updated: ") + action.title
				: action.response_text;
			if (!responseProposal->response.empty()) anchorAnswer(); else ownLine();
			task.operation_execution_state = "completed";
			task.professional_write_intent = true;
			task.professional_target_document_id = plan.target_document_id;
			task.professional_destination_format = documents::to_string(plan.format);
			if (task.document_operation_checkpoint) task.document_operation_checkpoint->stage = "committed";
			// Skip semantics/provenance (ported from ConnectedDocumentAction's
			// AppliedDocumentActionEvidence): reuse the exact same generic task
			// field DirectDocumentWork/ConnectedDocumentAction already populate,
			// never a second provenance record.
			if (!proposal.skipped_candidates.empty() || !proposal.evidence_gaps.empty()) {
				documents::AppliedDocumentActionEvidence evidence;
				evidence.document_id = action.document_id;
				evidence.resulting_version = action.resulting_version;
				documents::DocumentActionRationale rationale;
				rationale.summary = proposal.summary;
				for (const documents::ArtifactSkippedCandidate &skipped : proposal.skipped_candidates)
					rationale.summary += " [Skipped " + skipped.location + ": " + skipped.reason + "]";
				for (const std::string &gap : proposal.evidence_gaps)
					rationale.summary += " [Evidence gap: " + gap + "]";
				evidence.rationales.push_back(std::move(rationale));
				task.document_action_evidence.push_back(std::move(evidence));
			}
		} catch (const ArtifactNotPlaced &notPlaced) {
			// The change could not be placed even after the model's one correction.
			// The request did not fail: the lawyer reads, first, that nothing was
			// written - the model's answer may describe the change as made - and
			// then what the model proposed, and can ask again.
			task.response_text = answerWithNote(notPlaced);
			anchorAnswer();
		}
		}
		// Research the answer needed and did not have is a fact of this operation,
		// kept where the operation keeps its facts - the evidence gaps of the change
		// it proposed, its checkpoint and its record of events - and never appended
		// to the lawyer's answer as a notice (owner: no alarm notes in answers).
		if (researchLimitation)
			append_operation_event(task, "research.unserved", *researchLimitation, std::nullopt, "unserved");
		const std::string validatedResponse = task.response_text.value_or(std::string());
		task.operation_execution_state = "completed";
		task.operation_stage = "completed";
		task.operation_updated_at = workspaces::current_timestamp();
		// Keep the complete canonical research record. Rationale-level typed
		// IDs still distinguish evidence the action used from discovery that
		// was reviewed but not relied on; narrowing this vector to model-authored
		// marker strings would erase what the successful research loop knew.
		task.external_evidence = result.external_evidence;
    task.source_references = artifactCitations ? *artifactCitations
        : context::cited_references(prepared.supplied_markers.validate_response(validatedResponse));
    // Every quotation in the answer, checked against the passage it cited. A
    // § reference that reads confidently is not evidence; the words standing
    // in the document are. What cannot be checked is said to be unchecked -
    // never quietly presented as if it had been.
    task.quoted_passages = context::verify_quotations(validatedResponse, task.source_references,
        [&](const context::ResolvedSourceReference &reference) -> std::optional<std::string> {
          // Checked against the passage THIS request carried, not against a
          // fresh read of the file: what the answer quotes is what the model
          // was given, and a document edited since must not make a true
          // quotation look invented.
          if (prepared.supplied_chunk_ids.count(reference.chunk_id) == 0) return std::nullopt;
          for (const context::RetrievedContextChunk &supplied : prepared.document_context)
            if (supplied.chunk.chunk_id == reference.chunk_id) return supplied.chunk.text;
          return std::nullopt;
        }, prepared.supplied_markers.references());
    // A quotation found in another supplied passage is that passage's: its
    // document is one this answer relies on, listed with its sources and cited
    // where the quotation stands (below).
    for (const context::QuotedPassage &quoted : task.quoted_passages) {
      if (!quoted.rebound) continue;
      if (std::any_of(task.source_references.begin(), task.source_references.end(),
              [&](const context::ResolvedSourceReference &known) { return known.chunk_id == quoted.chunk_id; }))
        continue;
      for (const context::ResolvedSourceReference &reference : prepared.supplied_markers.references())
        if (reference.chunk_id == quoted.chunk_id) { task.source_references.push_back(reference); break; }
    }
    // A verified quotation is then written exactly as its document has it. A
    // model nesting a quotation inside its own re-marks the quotation marks in
    // it, and this Node's own restoration can give a name back in another
    // case; neither changes what the passage says, and neither belongs in
    // legal work that presents itself as verbatim.
    if (!task.quoted_passages.empty()) {
      const std::string exact = context::quote_documents_exactly(validatedResponse, task.quoted_passages);
      // A quotation re-bound to the passage its words really stand in carries
      // that passage's citation right where it closes, so the source the lawyer
      // opens beside it is the one that holds the words.
      const std::string cited = cite_rebound_quotations(exact, task.quoted_passages, task.source_references);
      if (cited != validatedResponse) task.response_text = cited;
    }
    // The universal Work Product document: kept only in sanitized form, every
    // block's sources validated against exactly the markers this request
    // supplied and turned into document labels, and any stray handle in its
    // text rendered to its plain label - never a machine handle in legal work.
    if (workProductJson) {
      Json declared = Json::parse(*workProductJson, nullptr, false);
      // Its body is the answer as the lawyer reads it (heading_schema): the
      // model named the work, Singularity splits the answer into its blocks.
      if (declared.is_object() && !declared.contains("blocks") && task.response_text)
        declared["blocks"] = work_product_document::blocks_from_markdown(*task.response_text);
      if (auto document = work_product_document::sanitize(declared)) {
        for (Json &block : (*document)["blocks"]) {
          std::string markers;
          for (const Json &marker : block["sources"]) markers += marker.get<std::string>() + " ";
          Json labels = Json::array();
          for (const context::ResolvedSourceReference &reference :
               context::cited_references(prepared.supplied_markers.validate_response(markers))) {
            const std::string label = context::format_reference_label(reference);
            if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
            const bool known = std::any_of(task.source_references.begin(), task.source_references.end(),
                [&](const context::ResolvedSourceReference &existing) { return existing.chunk_id == reference.chunk_id; });
            if (!known) task.source_references.push_back(reference);
          }
          block["sourceLabels"] = std::move(labels);
          block.erase("sources");
        }
        (void)documents::render_plain_source_labels(work_product_document::text_fields(*document),
          prepared.supplied_markers.references(), matter);
        task.work_product_document = document->dump();
      }
    }
    // The typed results the model declared in its own professional_response
    // (SLR-1). Every citation is validated through the exact same
    // supplied_markers registry as the prose below - a marker the request
    // never supplied is dropped, never turned into a citation, and never
    // fetched a second time. These are the production path; the "[FINDING]"/
    // "[OBLIGATION]" prose blocks below remain only for a response that
    // genuinely still uses that older shape.
    if (structuredResultsJson) {
      const Json declared = Json::parse(*structuredResultsJson, nullptr, false);
      // An entry's sources are what it names in "sources" and what its own text
      // cites inline: both are checked against the same supplied set, so a
      // real citation in a description is recorded, never later shown raw.
      const auto citedSources = [&](const Json &entry) {
        std::string markers;
        if (entry.contains("sources") && entry.at("sources").is_array())
          for (const Json &marker : entry.at("sources"))
            if (marker.is_string()) markers += marker.get<std::string>() + " ";
        if (entry.is_object())
          for (const auto &[key, value] : entry.items())
            if (key != "sources" && value.is_string()) markers += value.get<std::string>() + " ";
        return context::cited_references(prepared.supplied_markers.validate_response(markers));
      };
      const auto declaredText = [](const Json &entry, const char *key) -> std::optional<std::string> {
        if (!entry.contains(key) || !entry.at(key).is_string()) return std::nullopt;
        std::string value = entry.at(key).get<std::string>();
        if (value.empty()) return std::nullopt;
        return value;
      };
      if (declared.is_object() && declared.contains("findings") && declared.at("findings").is_array())
        for (const Json &entry : declared.at("findings")) {
          // A finding with no title or no description is not a finding; the
          // rest of the answer is never thrown away because one entry is
          // incomplete (Master Spec Sec.25).
          const std::optional<std::string> title = declaredText(entry, "title");
          const std::optional<std::string> description = declaredText(entry, "description");
          if (!title || !description) continue;
          matters::Finding finding;
          finding.id = matters::generate_finding_id();
          finding.matter_id = matter_id;
          finding.title = *title;
          finding.description = *description;
          finding.recommendation = declaredText(entry, "recommendation");
          finding.severity = declaredText(entry, "severity");
          finding.source_refs = citedSources(entry);
          task.findings.push_back(std::move(finding));
        }
      if (declared.is_object() && declared.contains("obligations") && declared.at("obligations").is_array())
        for (const Json &entry : declared.at("obligations")) {
          const std::optional<std::string> what = declaredText(entry, "obligation");
          if (!what) continue;
          matters::Obligation obligation;
          obligation.id = matters::generate_obligation_id();
          obligation.matter_id = matter_id;
          obligation.party = declaredText(entry, "party");
          obligation.obligation = *what;
          obligation.trigger = declaredText(entry, "trigger");
          obligation.deadline = declaredText(entry, "deadline");
          obligation.source_refs = citedSources(entry);
          task.obligations.push_back(std::move(obligation));
        }
      if (declared.is_object() && declared.contains("events") && declared.at("events").is_array())
        for (const Json &entry : declared.at("events")) {
          const std::optional<std::string> description = declaredText(entry, "description");
          if (!description) continue;
          matters::Event event;
          event.id = matters::generate_event_id();
          event.matter_id = matter_id;
          event.date = declaredText(entry, "date");
          event.description = *description;
          event.source_refs = citedSources(entry);
          task.events.push_back(std::move(event));
        }
      if (declared.is_object() && declared.contains("changes") && declared.at("changes").is_array())
        for (const Json &entry : declared.at("changes")) {
          const std::optional<std::string> description = declaredText(entry, "description");
          if (!description) continue;
          matters::Change change;
          change.id = matters::generate_change_id();
          change.matter_id = matter_id;
          change.category = declaredText(entry, "category");
          change.description = *description;
          change.source_refs = citedSources(entry);
          task.changes.push_back(std::move(change));
        }
    }
    // Reuses the exact same supplied_markers registry parse_findings()
    // validates "[SOURCE:...]" citations against - no second retrieval, no
    // re-derivation against current Matter state (Master Spec Sec.22/25).
    for (auto &parsed : context::parse_findings(validatedResponse, prepared.supplied_markers)) {
      matters::Finding finding;
      finding.id = matters::generate_finding_id();
      finding.matter_id = matter_id;
      finding.title = std::move(parsed.title);
      finding.description = std::move(parsed.description);
      finding.recommendation = std::move(parsed.recommendation);
      finding.severity = std::move(parsed.severity);
      finding.source_refs = std::move(parsed.source_refs);
      task.findings.push_back(std::move(finding));
    }
    // Same reuse contract as findings above - identical supplied_markers
    // registry, no second retrieval, no re-derivation against current
    // Matter state (Master Spec Sec.22/72).
    for (auto &parsed : context::parse_obligations(validatedResponse, prepared.supplied_markers)) {
      matters::Obligation obligation;
      obligation.id = matters::generate_obligation_id();
      obligation.matter_id = matter_id;
      obligation.party = std::move(parsed.party);
      obligation.obligation = std::move(parsed.obligation);
      obligation.trigger = std::move(parsed.trigger);
      obligation.deadline = std::move(parsed.deadline);
      obligation.source_refs = std::move(parsed.source_refs);
      task.obligations.push_back(std::move(obligation));
    }
    // Same reuse contract as findings/obligations above - identical
    // supplied_markers registry, no second retrieval, no re-derivation
    // against current Matter state (Master Spec Sec.22/73).
    for (auto &parsed : context::parse_chronology(validatedResponse, prepared.supplied_markers)) {
      matters::Event event;
      event.id = matters::generate_event_id();
      event.matter_id = matter_id;
      event.date = std::move(parsed.date);
      event.description = std::move(parsed.description);
      event.source_refs = std::move(parsed.source_refs);
      task.events.push_back(std::move(event));
    }
    // Same reuse contract as findings/obligations/events above - identical
    // supplied_markers registry, no second retrieval, no re-derivation
    // against current Matter state (Master Spec Sec.22/71). A change's
    // sourceRefs may span more than one of this request's supplied
    // documents - supplied_markers has no per-document restriction on which
    // markers can co-occur in one citation.
    for (auto &parsed : context::parse_comparison(validatedResponse, prepared.supplied_markers)) {
      matters::Change change;
      change.id = matters::generate_change_id();
      change.matter_id = matter_id;
      change.category = std::move(parsed.category);
      change.description = std::move(parsed.description);
      change.source_refs = std::move(parsed.source_refs);
      task.changes.push_back(std::move(change));
    }
    finish_reason = result.finish_reason;
	} catch (const ApprovedTargetChanged &error) {
		task.status = matters::TaskStatus::Error;
		task.error_code = providers::TaskErrorCode::TargetChanged;
		task.error = error.what();
		task.operation_execution_state = "conflict";
		if (task.document_operation_checkpoint) task.document_operation_checkpoint->stage = "conflict";
	} catch (const ProposalEvidenceChanged &error) {
		task.status = matters::TaskStatus::Error;
		task.error_code = providers::TaskErrorCode::EvidenceChanged;
		task.error = error.what();
		task.operation_execution_state = "conflict";
		if (task.document_operation_checkpoint) task.document_operation_checkpoint->stage = "conflict";
	} catch (const research::RequiredResearchFailure &error) {
    // The one difference from the generic std::exception case below: this
    // failure already carries the real ToolCallEvidence a tool attempt
    // produced before the loop had to fail the whole operation - previously
    // discarded here along with every other exception's lost stack state,
    // now attached to the failed task exactly like a successful attempt's
    // evidence already is at task.tool_call_evidence = result.tool_call_evidence
    // above.
    task.status = matters::TaskStatus::Error;
    task.error_code = error.code();
    task.error = error.what();
    task.usage = merge_usage_metadata(task.usage, error.usage);
    if (error.code() == providers::TaskErrorCode::ProtectedResponseInvalid)
      task.privacy_metadata.response_integrity = "needs_review";
	apply_safe_response_diagnostics(task.privacy_metadata, error.diagnostics);
    // Appended, never replacing: a failure in a later round (a review
    // correction) keeps the tool calls and research the earlier rounds made.
    append_tool_call_evidence(task.tool_call_evidence, error.tool_call_evidence);
    // Real ExternalEvidence already accumulated before the loop had to fail
    // the whole operation - a physically-proven Phase 24.1D gap: a budget-
    // exhausted run with four real successful web.search executions still
    // showed externalEvidence == [] on the failed task, because this type
    // used to carry only ToolCallEvidence. A failed professional operation
    // must not erase the fact that real research already occurred. Unlike
    // the success path above (task.external_evidence = cited_external_evidence(...),
    // filtered to what the final answer actually cited), there is no final
    // answer to check citations against here - this is the raw accumulated
    // set, exactly as ResearchToolLoop.hpp collected it, never fabricated.
		for (const research::ExternalEvidenceReference &evidence : error.external_evidence)
			if (std::none_of(task.external_evidence.begin(), task.external_evidence.end(),
					[&](const research::ExternalEvidenceReference &known) { return known.evidence_id == evidence.evidence_id; }))
				task.external_evidence.push_back(evidence);
		task.operation_execution_state = "failed";
		if (task.document_operation_checkpoint) {
			task.document_operation_checkpoint->stage = "failed";
			task.document_operation_checkpoint->external_evidence = task.external_evidence;
		}
	} catch (const providers::TaskError &error) {
    task.status = matters::TaskStatus::Error;
    task.error_code = error.code();
    task.error = error.what();
		// A response that arrived but could not be used (a refusal, an output-
		// limit truncation) was still billed; keep that evidence on the task.
		if (const auto *unusable = dynamic_cast<const providers::UnusableGeneration *>(&error))
			task.usage = merge_usage_metadata(task.usage, unusable->usage);
		if (error.code() == providers::TaskErrorCode::ProtectedResponseInvalid) {
      task.privacy_metadata.response_integrity = "needs_review";
		}
		apply_safe_response_diagnostics(task.privacy_metadata, error.diagnostics);
		task.operation_execution_state = "failed";
		if (task.document_operation_checkpoint) task.document_operation_checkpoint->stage = "failed";
	} catch (const context::ContextCapacityExceeded &) {
		// A professional review, redraft or targeted evidence completion the
		// known capacity cannot hold: nothing further was sent. The assembler's
		// own wording describes a first request, so the cause is restated.
		task.status = matters::TaskStatus::Error;
		task.error_code = providers::TaskErrorCode::ContextCapacityExceeded;
		task.error = "The professional review of this result, or the evidence it asked for, does not fit the selected "
			"model's context window, so the result could not be completed and nothing was saved. Choose a model with a "
			"larger context window or select fewer documents.";
		task.operation_execution_state = "failed";
		if (task.document_operation_checkpoint) task.document_operation_checkpoint->stage = "failed";
	} catch (const std::exception &error) {
    task.status = matters::TaskStatus::Error;
    task.error_code = providers::TaskErrorCode::Unknown;
		task.error = error.what();
		task.operation_execution_state = "failed";
		if (task.document_operation_checkpoint) task.document_operation_checkpoint->stage = "failed";
  }
  // The anchors follow the answer as it finally stands; one whose writing it
  // no longer holds is dropped, and that figure is then sent as a fact of its
  // own - still masked.
  if (anchoredAnswer && task.response_text != anchoredAnswer)
    task.fact_anchors = anchors_onto(*anchoredAnswer, task.fact_anchors, task.response_text.value_or(std::string()));
  const auto attempt_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - attempt_started_at)
                                      .count();

  routing::RoutingLogEntry log_entry;
  log_entry.matter_id = matter_id;
  log_entry.operation_id = task.id;
  // The same logical task keeps the same Routing Log task_id across every
  // attempt - only `attempt` increments - so a later routing-log/usage
  // viewer (Phase 08) can group retries without cross-referencing
  // matter.tasks. See routing_log.hpp's RoutingLogEntry::attempt.
  log_entry.task_id = spec.root_task_id.value_or(task.id);
  log_entry.attempt = spec.attempt;
  log_entry.time = task.created_at;
  log_entry.provider_id = task.provider_id;
  log_entry.model_id = task.model_id;
  log_entry.execution_kind = task.execution_kind;
  log_entry.legal_task_preset_id = task.legal_task_preset_id;
  log_entry.legal_task_preset_version = task.legal_task_preset_version;
  log_entry.document_count = static_cast<int>(supplied_document_ids(prepared).size());
  log_entry.chunk_count = static_cast<int>(prepared.supplied_chunk_ids.size());
  // Beta operation-unit rule: one local unit per retrieved/context chunk
  // processed plus one for an engaged Papinian Guard stage; one external
  // unit for the provider inference operation. This is intentionally not a
  // FLOP/time estimate and is persisted with a versioned rule name.
  log_entry.processing.local_operation_units = static_cast<int>(prepared.chunk_count) +
      (prepared.effective_policy.mode == protected_api::Mode::Off ? 0 : 1);
  log_entry.processing.external_operation_units = task.provider_id ? 1 : 0;
  log_entry.privacy_metadata = task.privacy_metadata;
  // What became of the request at the protection boundary. One that failed
  // before its protection result came back still ran under its policy: its
  // record says that mode, never the "off" of an empty record, and a refusal
  // keeps Papinian Guard's own words for it - by kind and place, never a value.
  if (log_entry.privacy_metadata.decision.empty()) {
    protected_api::papinian::PrivacyMetadata &recorded = log_entry.privacy_metadata;
    const bool isLocal = task.execution_kind == "local";
    const bool isSuccess = task.status == matters::TaskStatus::Success;
    if (isLocal) {
      recorded.decision = isSuccess ? "local" : "failed";
    } else if (isSuccess) {
      recorded.decision = "sent";
    } else {
      const bool isBlocked = task.error_code == providers::TaskErrorCode::ProtectedPayloadLeakDetected
          || task.error_code == providers::TaskErrorCode::PrivacyProtectionUnavailable;
      recorded.decision = isBlocked ? "blocked" : "failed";
      if (isBlocked && task.error) recorded.decision_reason = *task.error;
      if (!recorded.final_payload_validated && recorded.protected_count == 0 && recorded.outbound_calls.empty())
        recorded.mode = protected_api::to_string(prepared.effective_policy.mode);
    }
  }
  log_entry.success = task.status == matters::TaskStatus::Success;
  log_entry.latency_ms = attempt_latency_ms;
  log_entry.usage = task.usage;
  log_entry.finish_reason = finish_reason;
  log_entry.error_code = task.error_code;
  try {
    routing::append_routing_log_entry(workspace_path, log_entry);
  } catch (const std::exception &) {
    // The Routing Log is operational telemetry, not legal work product - a
    // storage failure here must not block or roll back the MatterTask this
    // attempt already produced (see routing_log.hpp).
  }

  return task;
}

std::vector<matters::MatterTask>::iterator find_task(matters::Matter &matter, const std::string &task_id) {
  return std::find_if(matter.tasks.begin(), matter.tasks.end(),
                      [&](const matters::MatterTask &task) { return task.id == task_id; });
}

// Parses the params matter.runTask and matter.previewOutboundRequest share
// (Phase 08 WP8-B) into one TaskAttemptSpec - the same fields, the same
// defaults, the same validation - so a preview and the real task it
// previews can never be built from two different readings of the same
// renderer request. `prompt` is passed in already trimmed/validated by the
// caller, which does so before acquiring any lock/loading the matter, so
// both callers keep their own exact prior error-ordering behavior.
//
// Phase 09's "Ask in Chat": when this attempt names a real chatId but
// supplies no selectedDocumentIds of its own, it inherits that Chat's real
// persisted starting selection (matters::MatterChat::selected_document_ids)
// - this is the one place a Chat's selection actually reaches context
// assembly (matters::retrieve_document_context() below, via
// prepare_task_request()'s ContextRetriever call), never merely a stored
// display value. An explicit selectedDocumentIds on this one attempt still
// wins outright - this never adds a second context assembler, only resolves
// which selection PromptAssembler/ContextRetriever's existing pipeline uses.
// A Chat that itself carries no selection (every Chat before Phase 09, or
// one opened without Ask in Chat) resolves to an empty vector exactly as
// before - the existing "relevant Project sources" automatic-retrieval
// behavior (ContextRetriever.cpp's explicit_selection) is unchanged for it.
TaskAttemptSpec task_attempt_spec_from_params(const Json &params, const std::string &prompt,
                                              const matters::Matter &matter) {
  TaskAttemptSpec spec;
  spec.prompt = prompt;
	if (params.contains("operationId") && params.at("operationId").is_string()) {
		spec.operation_id = params.at("operationId").get<std::string>();
	}
  const matters::MatterChat* chat = nullptr;
  if (params.contains("chatId") && params.at("chatId").is_string()) {
    const auto chat_id = params.at("chatId").get<std::string>();
    const auto found_chat = std::find_if(matter.chats.begin(), matter.chats.end(), [&](const matters::MatterChat& entry) { return entry.id == chat_id && !entry.archived; });
    if (found_chat == matter.chats.end()) throw std::runtime_error("This chat no longer exists.");
    spec.chat_id = chat_id;
    chat = &(*found_chat);
  }
  spec.matter_instructions = matter.instructions;
  // An agent's thread carries the agent's brief as its own standing
  // instructions, after the Matter's: whoever writes in it is answered by
  // the agent.
  if (chat != nullptr && trim(chat->instructions).empty() == false) {
    const bool hasMatterInstructions = matter.instructions.has_value() == true && trim(*matter.instructions).empty() == false;
    spec.matter_instructions = (hasMatterInstructions == true ? *matter.instructions + "\n\n" : std::string()) + chat->instructions;
    spec.agent_id = chat->agent_id;
  }
  if (params.contains("agentInitiated") && params.at("agentInitiated").is_boolean())
    spec.agent_initiated = params.at("agentInitiated").get<bool>();
  // An agent answering in a thread that is not its own carries its brief with
  // the request, after the Matter's and the chat's instructions.
  if (params.contains("agentBrief") && params.at("agentBrief").is_string() && !trim(params.at("agentBrief").get<std::string>()).empty()) {
    const std::string brief = params.at("agentBrief").get<std::string>();
    const bool hasInstructions = spec.matter_instructions.has_value() == true && trim(*spec.matter_instructions).empty() == false;
    spec.matter_instructions = (hasInstructions == true ? *spec.matter_instructions + "\n\n" : std::string()) + brief;
  }
  if (params.contains("agentId") && params.at("agentId").is_string()) spec.agent_id = params.at("agentId").get<std::string>();
  if (params.contains("agentName") && params.at("agentName").is_string()) spec.agent_name = params.at("agentName").get<std::string>();
  if (params.contains("answersMessageId") && params.at("answersMessageId").is_string())
    spec.answers_message_id = params.at("answersMessageId").get<std::string>();
  if (params.contains("authorActorId") && params.at("authorActorId").is_string()) {
    spec.author_actor_id = params.at("authorActorId").get<std::string>();
    spec.author_name = params.value("authorName", std::string());
  }
  if (params.contains("legalTaskPresetId") &&
      params.at("legalTaskPresetId").is_string()) {
    spec.legal_task_preset_id = params.at("legalTaskPresetId").get<std::string>();
  }
  if (params.contains("legalTaskPresetVersion") &&
      params.at("legalTaskPresetVersion").is_number_integer()) {
    spec.legal_task_preset_version = params.at("legalTaskPresetVersion").get<int>();
  }
  if (params.contains("promptProfileId") &&
      params.at("promptProfileId").is_string()) {
    spec.prompt_profile_id = params.at("promptProfileId").get<std::string>();
  }
  if (params.contains("promptProfileVersion") &&
      params.at("promptProfileVersion").is_number_integer()) {
    spec.prompt_profile_version = params.at("promptProfileVersion").get<int>();
  }
  if (params.contains("selectedDocumentIds")) {
    if (!params.at("selectedDocumentIds").is_array()) {
      throw std::runtime_error("Selected document IDs must be an array.");
    }
    for (const auto &document_id : params.at("selectedDocumentIds")) {
      if (!document_id.is_string()) {
        throw std::runtime_error("Selected document IDs must be strings.");
      }
      spec.selected_document_ids.push_back(document_id.get<std::string>());
    }
  } else if (chat != nullptr) {
    spec.selected_document_ids = chat->selected_document_ids;
  }
	if (params.contains("targetDocumentId")) {
		if (!params.at("targetDocumentId").is_string()
				|| params.at("targetDocumentId").get<std::string>().empty())
			throw std::runtime_error("The explicit target document ID must be a non-empty string.");
		spec.target_document_id = params.at("targetDocumentId").get<std::string>();
	}
  if (params.contains("providerId") && params.contains("modelId") &&
      params.at("providerId").is_string() && params.at("modelId").is_string()) {
    spec.model_target =
        routing::ModelTarget{params.at("providerId").get<std::string>(), params.at("modelId").get<std::string>()};
  }
	// Where the model came from, as the AI Setup resolver actually decided it
	// (SET-1). A caller that states nothing is one that chose this exact model
	// for this one request, which is what "operation_override" means.
	if (params.contains("aiSetupSource") && params.at("aiSetupSource").is_string())
		spec.ai_setup_source = params.at("aiSetupSource").get<std::string>();
	if (params.contains("operationRole") && params.at("operationRole").is_string())
		spec.operation_role = params.at("operationRole").get<std::string>();
	// Said only by a caller that can answer a question while the request runs.
	if (params.contains("canAskLawyer") && params.at("canAskLawyer").is_boolean())
		spec.can_ask_lawyer = params.at("canAskLawyer").get<bool>();
	if (params.contains("unattended") && params.at("unattended").is_boolean())
		spec.unattended = params.at("unattended").get<bool>();
	// A re-run of an earlier task joins its retry chain, keeps the instructions
	// that task ran under, and never protects less than it did.
	if (params.contains("retryOfTaskId") && params.at("retryOfTaskId").is_string()) {
		const std::string retryOf = params.at("retryOfTaskId").get<std::string>();
		const auto original = std::find_if(matter.tasks.begin(), matter.tasks.end(),
			[&](const matters::MatterTask &task) { return task.id == retryOf; });
		if (original == matter.tasks.end()) throw std::runtime_error("This task no longer exists.");
		spec.root_task_id = matters::task_group_id(*original);
		spec.attempt = matters::next_attempt_number(matter.tasks, *original);
		spec.matter_instructions = matters::resolve_retry_matter_instructions(*original, matter.instructions);
		spec.protection_mode_override = retry_protection_mode(*original);
		spec.prompt_profile_id = original->prompt_profile_id;
		spec.prompt_profile_version = original->prompt_profile_version;
		// SLR-3: what this turn ALREADY did to the Project. Running the same
		// turn again - Retry, Run with another model, Edit and continue - is
		// redoing one piece of work, not asking for a second piece: its own
		// record or artifact is the only thing it may write, so a rerun can
		// neither leave a duplicate beside it nor land on someone else's.
		spec.realized_record_document_id = original->direct_document_work_document_id;
		spec.realized_record_table_id = original->direct_document_work_table_id;
		spec.realized_record_banner_row_index = original->direct_document_work_banner_row_index;
		spec.realized_artifact_document_id = original->artifact_document_id;
		if (!spec.target_document_id) {
			// An artifact this turn created or updated is the target its rerun
			// binds to, so a Retry updates that file instead of creating a
			// second one beside it.
			spec.target_document_id = original->artifact_document_id;
		}
	}
	// Always derive the effect at the backend authority boundary. The JSON a
	// Chat composer carries is inspectable handoff evidence, never permission a
	// caller can forge to make a read-only request mutating.
	spec.professional_intent = professional::resolve_professional_execution_intent(
		prompt, spec.selected_document_ids);
	// Nothing else binds the document a change lands in. The request says
	// which document is to change, if any, and the model names it among the
	// documents it was given or found (ProfessionalArtifactAction.hpp's
	// ArtifactTargetHandle), checked the way a citation is. A Chat's saved
	// working document and a file name quoted in the request are not bindings:
	// a picker, or a name matched in the text, decided for the lawyer what the
	// lawyer's own words already say.
  return spec;
}

} // namespace

bool operation_works_only_here(const matters::Matter &matter, const std::string &operation_id) {
	return chat::operation_tasks_work_only_here(matter, operation_id);
}

const char *works_only_here_refusal(const matters::Matter &matter, const std::string &operation_id) {
	return chat::origin_unconfirmed_here(matter, operation_id) ? kOriginUnconfirmedModelRefused
		: kWorksOnlyHereModelRefused;
}

std::string operation_checkpoint_digest(const documents::DocumentOperationCheckpoint &checkpoint) {
	Json draft = checkpoint.draft;
	if (draft.is_object()) {
		draft.erase("decision");
		if (draft.contains("approval") && draft.at("approval").is_object()) {
			draft["approval"].erase("decision");
			draft["approval"].erase("decidedAt");
		}
	}
	return platform::sha256_hex_digest(
		Json{{"stage", checkpoint.stage}, {"draft", draft}, {"mutationCommit", checkpoint.mutation_commit}}.dump());
}

// The merge every writer of a canonical operation record applies before it
// saves a task another writer may have changed meanwhile: persist_task_
// checkpoint() above and run_task()'s own final save both call it. Exposed
// so the rule itself can be proven directly.
void preserve_concurrent_operation_state(const matters::MatterTask &current, matters::MatterTask &replacement) {
	preserve_newer_lifecycle(current, replacement);
}

providers::UsageMetadata accumulate_operation_usage(
		const providers::UsageMetadata &accumulated, const providers::UsageMetadata &addition) {
	return merge_usage_metadata(accumulated, addition);
}

std::string answer_with_figures_left_out(const std::string &answer) {
	return figures_left_out_of(answer);
}

std::string answer_with_figures_left_out(const std::string &answer,
		std::vector<protected_api::papinian::FactAnchor> &anchors) {
	AnchoredAnswer read = figures_left_out_of(AnchoredAnswer{answer, anchors});
	anchors = std::move(read.anchors);
	return read.text;
}

std::string answer_without_pending_approval_talk(const std::string &answer,
		std::vector<protected_api::papinian::FactAnchor> &anchors) {
	AnchoredAnswer read = without_pending_approval_talk(AnchoredAnswer{answer, anchors});
	anchors = std::move(read.anchors);
	return read.text;
}

std::optional<std::string> work_out_record(documents::ProfessionalDocumentResult &record) {
	documents::ProfessionalDocumentResult worked = record;
	std::vector<context::computed_statements::StatementFailure> unworked;
	std::string words;
	const auto workOut = [&unworked, &words](std::string &text) {
		words += text + "\n";
		context::computed_statements::CheckedText checked = context::computed_statements::compute_checked(text);
		for (context::computed_statements::StatementFailure &failure : checked.failures)
			unworked.push_back(std::move(failure));
		text = std::move(checked.text);
	};
	workOut(worked.title);
	for (std::string &paragraph : worked.description) workOut(paragraph);
	for (std::string &paragraph : worked.recommendations) workOut(paragraph);
	if (worked.additional_information) workOut(*worked.additional_information);
	for (std::string &support : worked.source_support) workOut(support);
	if (!unworked.empty()) return change_figures_note(unworked, written_in_russian(words));
	record = std::move(worked);
	return std::nullopt;
}

std::string cite_rebound_quotations(const std::string &response,
		const std::vector<context::QuotedPassage> &quoted,
		const std::vector<context::ResolvedSourceReference> &references) {
	// The quotation marks QuoteVerification reads a quotation between.
	static const std::vector<std::pair<std::string, std::string>> marks{
		{"\"", "\""}, {"\xe2\x80\x9c", "\xe2\x80\x9d"}, {"\xc2\xab", "\xc2\xbb"}};
	std::string cited = response;
	for (const context::QuotedPassage &passage : quoted) {
		if (!passage.verified || !passage.rebound || passage.chunk_id.empty()) continue;
		const auto reference = std::find_if(references.begin(), references.end(),
			[&](const context::ResolvedSourceReference &known) { return known.chunk_id == passage.chunk_id; });
		if (reference == references.end()) continue;
		// A marker this operation also records for another passage would open
		// that passage, not the one the words stand in.
		if (std::any_of(references.begin(), references.end(), [&](const context::ResolvedSourceReference &other) {
				return other.chunk_id != reference->chunk_id && other.document_token == reference->document_token
					&& other.chunk_token == reference->chunk_token; }))
			continue;
		const std::string marker = reference->marker();
		// The quotation as the answer now reads it: in its document's own
		// wording where that was put back, otherwise as the model wrote it.
		std::vector<std::string> writings;
		if (passage.repaired && !passage.exact.empty()) writings.push_back(passage.exact);
		if (!passage.quote.empty()) writings.push_back(passage.quote);
		bool placed = false;
		for (const std::string &words : writings) {
			for (std::size_t at = cited.find(words); !placed && at != std::string::npos; at = cited.find(words, at + 1)) {
				for (const auto &[opening, closing] : marks) {
					if (at < opening.size() || cited.compare(at - opening.size(), opening.size(), opening) != 0
							|| cited.compare(at + words.size(), closing.size(), closing) != 0)
						continue;
					const std::size_t after = at + words.size() + closing.size();
					std::size_t next = after;
					while (next < cited.size() && (cited[next] == ' ' || cited[next] == '\t')) ++next;
					// Already cited to the right passage right where it closes: this
					// occurrence needs nothing, and a later one may.
					if (cited.compare(next, marker.size(), marker) == 0) break;
					cited.insert(after, " " + marker);
					placed = true;
					break;
				}
			}
			if (placed) break;
		}
	}
	return cited;
}

// Test-only: resolves exactly the document selection and real local context
// one task attempt would use for `params` - including the Chat-inheritance
// fallback in task_attempt_spec_from_params() and the expectedDocumentVersions
// staleness guard in validate_expected_document_versions() above - by
// running the same production pipeline every real matter.runTask/
// matter.previewOutboundRequest attempt uses (a fresh working copy of
// `matter`, exactly like preview_outbound_request()'s own working_matter,
// so this never mutates the caller's Matter), stopping short of
// prepare_task_request()'s only caller that ever touches the network/a
// local model (execute_task_attempt() below). Never called by production
// code paths.
ResolvedTaskContextForTesting resolve_task_context_for_testing(const Json& params, const std::string& prompt,
		const std::filesystem::path& workspace_path, const matters::Matter& matter) {
	matters::Matter working_matter = matter;
	validate_expected_document_versions(params, working_matter);
	const TaskAttemptSpec spec = task_attempt_spec_from_params(params, prompt, working_matter);
	const PreparedTaskRequest prepared = prepare_task_request(workspace_path, working_matter, spec);
	std::vector<std::string> distinct_context_document_ids(
			prepared.distinct_context_document_ids.begin(), prepared.distinct_context_document_ids.end());
	std::sort(distinct_context_document_ids.begin(), distinct_context_document_ids.end());
	std::vector<std::string> advancedWorkflowDocumentIds;
	if (prepared.connected_document_action) {
		advancedWorkflowDocumentIds.push_back(prepared.connected_document_action->target_document_id);
		for (const documents::DocumentActionCandidate &candidate : prepared.connected_document_action->other_candidate_documents)
			advancedWorkflowDocumentIds.push_back(candidate.document_id);
		if (prepared.connected_document_action->methodology_document_id)
			advancedWorkflowDocumentIds.push_back(*prepared.connected_document_action->methodology_document_id);
	}
	return ResolvedTaskContextForTesting{
			spec.selected_document_ids, std::move(distinct_context_document_ids), prepared.context_snapshot,
			prepared.request.prompt, prepared.context_budget_excluded_material, recorded_coverage(prepared.request_coverage),
			std::move(advancedWorkflowDocumentIds),
			prepared.request.research_final_output_contract ? prepared.request.research_final_output_contract->json_schema
				: Json(nullptr),
			prepared.request.maximum_local_fact_rounds,
			prepared.answer_text_checked,
			prepared.request.local_facts_step_contract ? prepared.request.local_facts_step_contract->json_schema
				: Json(nullptr)};
}

// Runs one fresh task against a matter - see execute_task_attempt() for the
// pipeline itself. Only throws for a request that could not be attempted at
// all (empty prompt, matter/workspace gone); an inference-side failure
// becomes an error MatterTask instead.
namespace {

// The model names the conversation once, from its first exchange, and never
// over anyone: a title a person chose stays, and so does one the quick local
// title (ChatExecution.hpp) has already given. What is kept of the name is
// what is kept of any name a model gives a chat (chat::chat_title_from_reply),
// read against the lawyer's own message.
void apply_suggested_chat_title(matters::Matter &matter, const matters::MatterTask &task,
		const std::string &timestamp) {
	if (!task.suggested_chat_title || !task.chat_id) return;
	const std::string title = chat::chat_title_from_reply(*task.suggested_chat_title, task.prompt);
	if (title.empty()) return;
	const auto chat = std::find_if(matter.chats.begin(), matter.chats.end(),
		[&](const matters::MatterChat &entry) { return entry.id == *task.chat_id; });
	if (chat == matter.chats.end()) return;
	// A chat is named once: by a person, by the quick local title, or here.
	if (chat->title_authority != matters::ChatTitleAuthority::Default) return;
	chat->title = title;
	chat->title_authority = matters::ChatTitleAuthority::Automatic;
	chat->updated_at = timestamp;
	chat->revision += 1;
	chat->operation_id = matters::generate_chat_operation_id();
}


} // namespace

namespace {

std::mutex &answered_observer_mutex() {
	static std::mutex mutex;
	return mutex;
}

AnsweredObserver &answered_observer() {
	static AnsweredObserver observer;
	return observer;
}

// Never lets a listener's failure reach the request it listened to.
void notify_answered(const std::string &project_id, const std::string &task_id) noexcept {
	try {
		AnsweredObserver observer;
		{
			const std::lock_guard<std::mutex> guard(answered_observer_mutex());
			observer = answered_observer();
		}
		if (observer) observer(project_id, task_id);
	} catch (...) {
	}
}

std::mutex &source_picker_mutex() {
	static std::mutex mutex;
	return mutex;
}

SourcePicker &source_picker() {
	static SourcePicker picker;
	return picker;
}

} // namespace

void set_answered_observer(AnsweredObserver observer) {
	const std::lock_guard<std::mutex> guard(answered_observer_mutex());
	answered_observer() = std::move(observer);
}

void set_source_picker(SourcePicker picker) {
	const std::lock_guard<std::mutex> guard(source_picker_mutex());
	source_picker() = std::move(picker);
}

namespace {
std::mutex &approval_filed_hook_mutex() {
	static std::mutex mutex;
	return mutex;
}
ApprovalFiledHook &approval_filed_hook() {
	static ApprovalFiledHook hook;
	return hook;
}
} // namespace

void set_approval_filed_hook_for_testing(ApprovalFiledHook hook) {
	const std::lock_guard<std::mutex> guard(approval_filed_hook_mutex());
	approval_filed_hook() = std::move(hook);
}

namespace {

// How long a request waits for the documents it names to be read, and - when
// it names none - for the few just added to the folder
// (matter::kFewDocumentsForARequest).
constexpr std::chrono::seconds kDocumentReadingWait{120};
constexpr std::chrono::seconds kRelevantDocumentReadingWait{15};

// The documents a request selects: its own selection, else its Chat's. None:
// it reads the Project's relevant sources (and its explicit target, which is
// waited for either way - wait_for_request_documents_read()).
std::vector<std::string> documents_request_selects(const Json &params, const std::string &matter_id) {
	std::vector<std::string> named;
	if (params.contains("selectedDocumentIds") && params.at("selectedDocumentIds").is_array()) {
		for (const Json &documentId : params.at("selectedDocumentIds"))
			if (documentId.is_string()) named.push_back(documentId.get<std::string>());
	} else if (params.contains("chatId") && params.at("chatId").is_string()) {
		const std::string chatId = params.at("chatId").get<std::string>();
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto project = matters::load_matter(node::require_active_workspace_path(), matter_id);
		if (project)
			for (const matters::MatterChat &chat : project->chats)
				if (chat.id == chatId && !chat.archived) named = chat.selected_document_ids;
	}
	return named;
}

// A request is answered from the documents it needs as they are once read: a
// selected document, or a file the lawyer dropped into the folder a moment ago,
// is read now, before the request snapshots the Project, instead of being
// skipped because the folder watcher's reading had not reached it. Nothing else
// is waited for: a data room being read never holds a request, which goes on
// from what is read and says what is still being read (the source coverage
// names each one). The wait is the request's first "Reading N sources" step,
// timed by the backend. Called before the persistence lock is taken - never
// under it (the reading records its results under that lock).
void wait_for_project_documents(const Json &params, const std::string &matter_id) {
	platform::trace::Span span("task", "run_task.wait_for_documents");
	const std::vector<std::string> selected = documents_request_selects(params, matter_id);
	const std::optional<std::string> target = params.contains("targetDocumentId") && params.at("targetDocumentId").is_string()
		? std::optional<std::string>(params.at("targetDocumentId").get<std::string>()) : std::nullopt;
	const std::size_t namedCount = selected.size() + (target ? 1 : 0);
	const matter::DocumentReadingWait waited = matter::wait_for_request_documents_read(matter_id, selected, target,
		std::chrono::duration_cast<std::chrono::milliseconds>(kDocumentReadingWait),
		std::chrono::duration_cast<std::chrono::milliseconds>(kRelevantDocumentReadingWait), [](std::size_t count) {
			routing::observability::emit({routing::observability::OperationEventKind::SourceReadStart,
				"Reading the Project's newly added or changed documents",
				{{"documentCount", std::to_string(count)}, {"mode", "project_documents"}}, std::nullopt});
		});
	span.add("named", namedCount);
	span.add("awaited", waited.awaited_count);
	span.add("waitedMs", static_cast<long long>(waited.waited.count()));
	span.add("unread", waited.unread_document_ids.size());
	if (waited.awaited_count == 0 && waited.unread_document_ids.empty()) return;
	routing::observability::emit({routing::observability::OperationEventKind::SourceReadEnd,
		waited.unread_document_ids.empty() ? "The documents this request needs are read"
			: waited.unread_document_ids.size() == 1 ? "One document is still being read; the request goes on without it"
			: std::to_string(waited.unread_document_ids.size())
				+ " documents are still being read; the request goes on without them",
		{{"documentCount", std::to_string(waited.awaited_count)},
		 {"unreadCount", std::to_string(waited.unread_document_ids.size())},
		 {"waitedMs", std::to_string(waited.waited.count())}}, std::nullopt});
}

} // namespace

std::string approval_floor(const matters::MatterTask &task, const std::string &requested_by) {
	const std::vector<std::string> words = lawyer_words(task);
	if (lawyer_asks_to_see_changes_first(words)) return "asked_first";
	if (!tools::quotes_lawyer(requested_by, words)) return "not_requested";
	return std::string();
}

namespace {

// The words that name a document: its name's own words, less the ones that
// name nothing ("the", "docx") and figures.
std::vector<std::u32string> document_name_words(const std::string &name) {
	std::vector<std::u32string> words;
	for (std::u32string &word : folded_words(name)) {
		if (has_digit(word) || request_free_words().count(word) != 0 || confirming_words().count(word) != 0
				|| file_name_words().count(word) != 0 || document_words().count(word) != 0)
			continue;
		words.push_back(std::move(word));
	}
	return words;
}

bool contains_any(const std::vector<std::u32string> &said, const std::set<std::u32string> &words) {
	return std::any_of(said.begin(), said.end(), [&](const std::u32string &word) { return words.count(word) != 0; });
}

// True when the instruction sends the change somewhere other than where the
// proposal put it - so it is a new request, whatever else it says.
bool changes_destination(const std::vector<std::u32string> &said, const ProposalDestination &destination) {
	std::vector<std::u32string> otherNames;
	for (const std::string &name : destination.other_document_names)
		for (std::u32string &word : document_name_words(name)) otherNames.push_back(std::move(word));
	const bool asksForSeparateDocument = contains_any(said, separate_document_words())
		|| (contains_any(said, new_thing_words()) && contains_any(said, document_words()));
	if (!destination.creates_new_document) {
		// A change to one document: a document of its own, or another document
		// of the Project, is somewhere else.
		if (asksForSeparateDocument) return true;
		const std::vector<std::u32string> targetNames = document_name_words(destination.target_document_name);
		for (const std::u32string &word : said)
			if (is_proposal_word(word, otherNames) && !is_proposal_word(word, targetNames)) return true;
		return false;
	}
	// A new document: "into", "in" or "в" puts it inside something that already
	// exists - unless the lawyer says it is a document of its own.
	if (asksForSeparateDocument) return false;
	if (contains_any(said, inside_words())) return true;
	// "to" relates the new document to another ("create the amendment to the
	// MSA") only when the new document is named before it; with nothing named -
	// "Add it to the MSA." - it is where the change goes.
	std::vector<std::u32string> ownNames;
	for (std::u32string &word : document_name_words(destination.new_document_name))
		if (!is_proposal_word(word, otherNames)) ownNames.push_back(std::move(word));
	bool isNewDocumentNamed = false;
	for (const std::u32string &word : said) {
		if (relating_words().count(word) != 0 && !isNewDocumentNamed) return true;
		if (is_proposal_word(word, ownNames)) isNewDocumentNamed = true;
		// A document of the Project named with no new document before it.
		else if (!isNewDocumentNamed && is_proposal_word(word, otherNames)) return true;
	}
	return false;
}

} // namespace

bool only_confirms_proposal(const std::string &instruction, const std::string &proposal_words,
		const ProposalDestination &destination) {
	// A confirmation is short; a paragraph is a request, whatever words it uses.
	constexpr std::size_t kLongestConfirmation = 16;
	const std::vector<std::u32string> said = folded_words(instruction);
	if (said.empty() || said.size() > kLongestConfirmation) return false;
	const std::vector<std::u32string> proposal = folded_words(proposal_words);
	bool isConfirmed = false;
	for (const std::u32string &word : said) {
		// Any figure is a new request: "1 million" is not confirmed by "Amendment
		// No. 1", and a figure the change already holds is no confirmation either.
		if (has_digit(word)) return false;
		if (new_request_words().count(word) != 0) return false;
		if (confirming_words().count(word) != 0) {
			isConfirmed = true;
			continue;
		}
		if (request_free_words().count(word) != 0) continue;
		if (!is_proposal_word(word, proposal)) return false;
	}
	return isConfirmed && !changes_destination(said, destination);
}

matters::Matter run_task(const Json &params) {
	// The request reads its settings once, and again only after a save
	// (platform::AppSettingsReadScope) - not at every Project read and every
	// model turn.
	const platform::AppSettingsReadScope settingsScope;
	// Its requests stop on their connection once Singularity is closing; a
	// chat operation keeps its own Stop token (Platform/AppLifecycle).
	const platform::lifecycle::ClosingCancelScope closingStops;
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto prompt = trim(params.at("prompt").get<std::string>());
  if (prompt.empty()) {
    throw std::runtime_error("Enter a task to run.");
  }

  const auto workspace_path = node::require_active_workspace_path();
	platform::trace::Span runSpan("task", "run_task", {{"promptBytes", prompt.size()}}, true);
	wait_for_project_documents(params, matter_id);
	matters::Matter execution_snapshot;
	TaskAttemptSpec spec;
	{
		platform::trace::Span snapshotSpan("task", "run_task.snapshot");
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto found = matters::load_matter(workspace_path, matter_id);
		if (!found) throw std::runtime_error("This matter no longer exists.");
		platform::trace::Span refreshSpan("task", "run_task.refresh_documents");
		const bool documentsChanged = matters::refresh_matter_documents(workspace_path, *found);
		refreshSpan.add("changed", documentsChanged);
		refreshSpan.finish();
		if (documentsChanged) {
			found->updated_at = workspaces::current_timestamp();
			matters::save_matter(workspace_path, *found);
		}
		validate_expected_document_versions(params, *found);
		spec = task_attempt_spec_from_params(params, prompt, *found);
		execution_snapshot = *found;
	}

	// Model and tool work must never monopolize Project persistence. Stop,
	// reconnect snapshots and operation checkpoints remain available while a
	// slow local or remote request is in flight.
	platform::trace::Span attemptSpan("task", "run_task.execute_attempt", {}, true);
	auto task = execute_task_attempt(workspace_path, matter_id, execution_snapshot, spec);
	attemptSpan.finish();
	platform::trace::Span finalSaveSpan("task", "run_task.final_save");
	// A connected document action reconciles the Document Registry through
	// the patch tool. Reload and merge only this operation record so concurrent
	// Project edits and lifecycle transitions cannot be overwritten.
	matters::Matter finalMatter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto updated = matters::load_matter(workspace_path, matter_id);
		if (!updated) throw std::runtime_error("This matter no longer exists.");
		if (matters::MatterTask *current = !spec.operation_id.empty()
				? [&]() -> matters::MatterTask * {
					const auto found = std::find_if(updated->tasks.begin(), updated->tasks.end(),
						[&](const matters::MatterTask &entry) { return entry.agent_operation_id == spec.operation_id; });
					return found == updated->tasks.end() ? nullptr : &*found;
				}() : nullptr) {
			// Preserve lifecycle events/revision written while inference was active.
			preserve_newer_lifecycle(*current, task);
		}
		note_checkpoint_saved(matter_id, task);
		upsert_task_record(*updated, task);
		updated->updated_at = workspaces::current_timestamp();
		// What the answer offered beyond itself, applied in the same save: the
		// conversation's name. Only a successful answer is a source for it.
		if (task.status == matters::TaskStatus::Success) {
			apply_suggested_chat_title(*updated, task, updated->updated_at);
		}
		routing::observability::emit({routing::observability::OperationEventKind::PersistenceStart,
			"Saving canonical Project task", {{"taskId", task.id}}, std::nullopt});
		matters::save_matter(workspace_path, *updated);
		routing::observability::emit({routing::observability::OperationEventKind::PersistenceComplete,
			"Canonical Project task saved", {{"taskId", task.id}}, std::nullopt});
		finalMatter = *updated;
	}
	// The answer is saved: whoever asked to hear of answered requests is told
	// now, outside the persistence lock. (The Project's memory listens here - a
	// local model folds the exchange into it afterwards, on its own time, so an
	// answer never waits for its memory.)
	// Only a finished answer: a change still waiting for the lawyer's decision
	// was not given to them, and may yet be rejected.
	// Never the answer of work kept on this computer: the memory is the Matter's,
	// and later requests send it to the Matter's own models.
	if (task.status == matters::TaskStatus::Success
			&& (task.operation_execution_state.empty() || task.operation_execution_state == "completed")
			&& !chat::task_works_only_here(finalMatter, task))
		notify_answered(matter_id, task.id);
	// A completed professional mutation - DirectDocumentWork's structured
	// record, the model-native AdvancedWorkflow choreography, or a generic
	// ProfessionalArtifactAction create/update - is also durably retained as
	// a first-class Work Product (reusing work_product::retain_task_result()
	// - the SAME mechanism Context Curation's own result_context_item_ids
	// already relies on for its analogous domain, never a second one). This
	// runs in its OWN transaction, after the task above is already durably
	// Success, since retain_task_result() re-loads canonical state and
	// requires the task it looks up to already be persisted that way -
	// retain_task_result() also takes its own persistence_mutex() lock
	// internally, so this must run only after the block above releases it.
	// A retention failure is never allowed to undo or fail an already-
	// completed, already-persisted professional operation.
	//
	// Gated on task.professional_write_intent - the SAME generic "a real
	// professional mutation actually committed" signal all three paths
	// already set (never on task.status==Success alone, which is equally
	// true for a plain read-only Chat answer that must never auto-retain -
	// see ChatExecution.test.cpp's item17). Narrowing this to direct_
	// document_work_document_id alone (the old condition) silently excluded
	// AdvancedWorkflow's and generic artifact create/update's own real,
	// committed professional deliverables from ever becoming a retained
	// Work Product automatically.
	if (task.status == matters::TaskStatus::Success && task.professional_write_intent) {
		try {
			std::string title = task.direct_document_work_title.value_or(std::string());
			if (title.empty()) title = task.artifact_title.value_or(std::string());
			// AdvancedWorkflow's own result carries neither of the above (its
			// professional content lives in task.response_text/document_action_
			// evidence, not a single named object) - fall back to the real
			// mutated document's own current name, the same identity its own
			// AppliedDocumentActionEvidence already names.
			if (title.empty() && !task.document_action_evidence.empty()) {
				const std::string &mutatedDocumentId = task.document_action_evidence.front().document_id;
				const auto mutatedDocument = std::find_if(finalMatter.documents.begin(), finalMatter.documents.end(),
					[&](const matters::MatterDocument &candidate) { return candidate.id == mutatedDocumentId; });
				if (mutatedDocument != finalMatter.documents.end()) title = mutatedDocument->name;
			}
			if (title.empty()) title = "Professional document update";
			routing::observability::emit({routing::observability::OperationEventKind::PersistenceStart,
				"Retaining the committed professional result",
				{{"kind", "work_product"}, {"taskId", task.id}}, std::nullopt});
			// retain_task_result() links the task to its Work Product in the same
			// save and returns the existing one if this result was already kept.
			const matters::WorkProductObject workProduct = work_product::retain_task_result(
				matter_id, task.id, matters::kWorkProductTypeDraft, title);
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			const auto updated = matters::load_matter(workspace_path, matter_id);
			if (updated) finalMatter = *updated;
			routing::observability::emit({routing::observability::OperationEventKind::PersistenceComplete,
				"Committed professional result retained",
				{{"kind", "work_product"}, {"taskId", task.id}, {"workProductId", workProduct.id}}, std::nullopt});
		} catch (const std::exception &) {
			// Retention is a durability convenience layered on an already-
			// successful task, never its own success criterion.
			routing::observability::emit({routing::observability::OperationEventKind::PersistenceComplete,
				"The committed task remains available, but Work Product retention failed",
				{{"kind", "work_product"}, {"taskId", task.id}, {"outcome", "failed"}}, std::nullopt});
		}
	}
	return finalMatter;
}

// "What leaves this device?" (Master Spec Sec.54, Phase 08 WP8-B): builds
// the exact same TaskAttemptSpec/PreparedTaskRequest a real matter.runTask
// with these same params would - task_attempt_spec_from_params() and
// prepare_task_request() are literally the same functions run_matter_task()
// calls above - then calls routing::preview() instead of routing::execute(),
// which captures the exact outbound request instead of sending it. Never
// persists anything: no matters::save_matters(), no Routing Log entry, no
// MatterTask - this is inspection only. Throws exactly like matter.runTask
// for every resolution failure (no workspace, matter gone, no provider/
// model/secret, invalid params), and for a leak-guard block - a request
// unsafe to send is unsafe to preview.
Json preview_outbound_request(const Json &params) {
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto prompt = trim(params.at("prompt").get<std::string>());
  if (prompt.empty()) {
    throw std::runtime_error("Enter a task to run.");
  }

  const auto workspace_path = node::require_active_workspace_path();
  // Refreshed on a local copy only, exactly like run_matter_task() refreshes
  // before retrieval, so preview's document context reflects the same
  // up-to-date extraction state a real send would use - but never saved,
  // since preview must not persist anything (no side effect on the actual
  // Matter/workspace state). Persistence is held only to read the Project;
  // retrieval and local protection run without it.
  matters::Matter working_matter;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto found = matters::load_matter(workspace_path, matter_id);
    if (!found) {
      throw std::runtime_error("This matter no longer exists.");
    }
    working_matter = *found;
  }
  matters::refresh_matter_documents(workspace_path, working_matter);
  validate_expected_document_versions(params, working_matter);

  const auto spec = task_attempt_spec_from_params(params, prompt, working_matter);
  const auto prepared = prepare_task_request(workspace_path, working_matter, spec);
  const auto preview = routing::preview(prepared.request);

  const std::unordered_set<std::string> preview_supplied_documents = supplied_document_ids(prepared);
  Json source_document_names = Json::array();
  for (const auto &document : working_matter.documents) {
    if (preview_supplied_documents.count(document.id) > 0) {
      source_document_names.push_back(document.name);
    }
  }

  // "Protected preview" / "Understanding replacements" (Phase 11): presented
  // from the very protection routing::preview() built the outbound request
  // with - the prompt as it leaves and the mapping behind its symbols, handed
  // back in native memory - so what is shown, and what a reveal answers, is
  // exactly what leaves. A second protect() over the whole prompt once showed
  // "the PERSON 1's current objective" where "the user's current objective"
  // was sent: it scored Singularity's own instruction sections, which the real
  // request never does (24 Sep 2026). The mapping never leaves this function:
  // only the redacted ProtectedPreviewSummary (no original values) and an
  // opaque preview session token cross into the returned JSON. Skipped for a
  // local-model target, since nothing leaves the device to preview.
  Json human_readable_preview = nullptr;
  Json occurrences = Json::array();
  Json preview_token = nullptr;
  if (preview.provider_id != "local") {
    auto protection = preview.protection;
    auto sources = origin_sources_for(workspace_path, working_matter, spec);
    const auto summary = protected_api::papinian::summarize_for_preview(protection, sources);
    human_readable_preview = summary.human_readable_text;
    for (const auto &occurrence : summary.occurrences) occurrences.push_back(occurrence_summary_json(occurrence));

    // Ephemeral native preview session (Phase 11, PapinianGuard/PreviewSession):
    // `protection`/`sources` - the only place a reveal can later find a real
    // protected value - never leave native memory. Only this opaque,
    // cryptographically random token crosses into the returned JSON; the
    // frontend must return it unchanged with any reveal_protected_
    // occurrence() call for one of the occurrences above. Reveal looks this
    // exact session up by token and fails closed if it is missing or if the
    // request/source/provider/model/policy state captured alongside it no
    // longer matches - never by recomputing and silently resolving against
    // whatever the current state has since become.
    const auto settings = platform::load_app_settings();
    const auto effective_target =
        effective_reasoning_target(settings.local_node_setup, working_matter, spec.chat_id);

    protected_api::papinian::PreviewSessionState session;
    session.matter_id = matter_id;
    session.chat_id = spec.chat_id;
    session.fingerprint = build_state_fingerprint(prepared.effective_policy, effective_target.provider_id,
                                                  effective_target.model_id, working_matter,
                                                  prepared.distinct_context_document_ids);
    session.protection = std::move(protection);
    session.sources = std::move(sources);
    preview_token = protected_api::papinian::production_preview_session_store().create(std::move(session));
  }

  return Json{
      {"providerId", preview.provider_id},
      {"modelId", preview.model_id},
      {"protectedApiMode", protected_api::to_string(prepared.effective_policy.mode)},
      {"protectedApiApplied", preview.protected_api_applied},
      {"privacyMetadata", privacy_metadata_json(preview.privacy_metadata)},
      {"documentCount", static_cast<int>(preview_supplied_documents.size())},
      // The real number of local source excerpts this exact prepared
      // request carries - the passages its capacity-first budget admitted,
      // never every passage retrieval considered, a second retrieval or a
      // frontend estimate (phase-23: "Do not guess document/excerpt
      // /protection counts").
      {"sourceExcerptCount", static_cast<int>(prepared.supplied_chunk_ids.size())},
      {"sourceDocumentNames", source_document_names},
      {"outboundText", preview.outbound_text},
      {"humanReadablePreview", human_readable_preview},
      {"occurrences", occurrences},
      {"previewToken", preview_token},
  };
}

// "Reveal original locally" (Phase 11): the one place a real protected value
// may surface, on demand, for exactly one occurrence - never a stored or
// returned Restoration Map. `previewToken` must be the opaque token
// preview_outbound_request() returned alongside this occurrenceId; this
// function never recomputes/re-protects anything itself, it only looks the
// exact ephemeral session up (PapinianGuard/PreviewSession) and answers from
// the ProtectionResult that session already holds in native memory. The
// look-up fails closed - "no longer available", never a guess - when the
// token is unknown/expired, when it was minted for a different matter, or
// when the request/source/provider/model/policy state captured alongside it
// no longer matches the current one: without that check, occurrenceId alone
// (e.g. "PERSON_1") is only ever stable within the one session it came from,
// so a stale client could otherwise ask a *fresh* recomputation to resolve
// the same id and receive a different person's real value in its place.
Json reveal_protected_occurrence(const Json &params) {
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto occurrence_id = params.at("occurrenceId").get<std::string>();
  const auto preview_token = params.at("previewToken").get<std::string>();

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();

  auto &session_store = protected_api::papinian::production_preview_session_store();
  const auto recipe = session_store.recipe_for(preview_token, matter_id);
  if (!recipe) {
    throw std::runtime_error("This protected value is no longer available. The request may have changed.");
  }

  const auto found = matters::load_matter(workspace_path, matter_id);
  if (!found) {
    throw std::runtime_error("This matter no longer exists.");
  }

  matters::Matter working_matter = *found;
  matters::refresh_matter_documents(workspace_path, working_matter);

  const auto settings = platform::load_app_settings();
  // Same authoritative Node -> Project -> Chat resolution preview built its
  // own fingerprint from (effective_reasoning_target() above) - never Node-
  // default-only, so a Project (or Chat) AI Setup override does not
  // spuriously read as "state changed" here. `recipe->chat_id` is the exact
  // chat (if any) the original preview resolved against, recovered from the
  // session itself rather than assumed absent.
  const auto current_target =
      effective_reasoning_target(settings.local_node_setup, working_matter, recipe->chat_id);
  const std::unordered_set<std::string> document_id_set(recipe->document_ids.begin(), recipe->document_ids.end());
  const auto current_fingerprint =
      build_state_fingerprint(settings.local_node_setup.protected_api_policy, current_target.provider_id,
                              current_target.model_id, working_matter, document_id_set);

  const auto session = session_store.look_up(preview_token, matter_id, current_fingerprint);
  if (!session) {
    throw std::runtime_error("This protected value is no longer available. The request may have changed.");
  }

  const auto revealed =
      protected_api::papinian::reveal_occurrence(session->protection, session->sources, occurrence_id);
  if (!revealed) {
    throw std::runtime_error("This protected value is no longer available. The request may have changed.");
  }

  return Json{
      {"occurrenceId", revealed->occurrence_id},
      {"category", revealed->category},
      {"displayPlaceholder", revealed->display_placeholder},
      {"originKind", revealed->origin_kind},
      {"originObjectId", revealed->origin_object_id ? Json(*revealed->origin_object_id) : Json(nullptr)},
      {"excerptBefore", revealed->excerpt_before},
      {"excerptMatch", revealed->excerpt_match},
      {"excerptAfter", revealed->excerpt_after},
  };
}

// Explicit, immediate teardown of one ephemeral preview session (Phase 11) -
// the normal way a session's life ends, not TTL expiry. The frontend calls
// this the moment a displayed preview stops being "the current one": the
// review sheet is closed or cancelled, or the prompt/source selection that
// produced it changes (see PreviewSessionStore::invalidate()'s own doc
// comment for the full lifecycle). A no-op, not an error, for a token that
// is already gone/unknown/for a different matter - closing twice, or
// closing after the token already expired, is not a failure.
Json close_outbound_preview(const Json &params) {
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto preview_token = params.at("previewToken").get<std::string>();
  protected_api::papinian::production_preview_session_store().invalidate(preview_token, matter_id);
  return Json(true);
}

// Project persistence is never held while a model or tool works: an attempt
// runs against a snapshot taken under the persistence lock, and its resulting
// tasks are merged into the Project as it is by the time they are recorded, so
// Stop, status reads and other edits stay responsive and are never overwritten.
matters::Matter project_snapshot_for_attempts(const std::filesystem::path &workspace_path, const std::string &matter_id) {
	wait_for_project_documents(Json::object(), matter_id);
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	auto found = matters::load_matter(workspace_path, matter_id);
	if (!found) throw std::runtime_error("This matter no longer exists.");
	if (matters::refresh_matter_documents(workspace_path, *found)) {
		found->updated_at = workspaces::current_timestamp();
		matters::save_matter(workspace_path, *found);
	}
	return *found;
}

matters::Matter record_attempt_results(const std::filesystem::path &workspace_path, const std::string &matter_id,
		std::vector<matters::MatterTask> tasks, const std::function<void(matters::Matter &)> &alsoApply = {}) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	auto found = matters::load_matter(workspace_path, matter_id);
	if (!found) throw std::runtime_error("This matter no longer exists.");
	for (auto &task : tasks) upsert_task_record(*found, std::move(task));
	if (alsoApply) alsoApply(*found);
	found->updated_at = workspaces::current_timestamp();
	matters::save_matter(workspace_path, *found);
	return *found;
}

namespace {

// Work kept on this computer - asked to stay, or begun on another computer
// this one cannot confirm - is run again here only: never on a model outside
// it. With no model named, the attempt runs kept here (ScopedLocalOnlyWork),
// where inference itself takes only this computer's model.
// Whether any of these answers is work kept on this computer (D-012). A
// request that carries an answer word for word - a synthesis, a seat shown the
// answers before it - carries that work, so it stays here too (#229).
bool answers_kept_here(const matters::Matter &snapshot, const std::vector<matters::MatterTask> &answers) {
  return std::any_of(answers.begin(), answers.end(),
      [&](const matters::MatterTask &answer) { return chat::task_works_only_here(snapshot, answer); });
}

void keep_attempt_here(const matters::Matter &snapshot, const matters::MatterTask &original, TaskAttemptSpec &spec) {
  if (!spec.model_target || spec.model_target->provider_id == "local") return;
  throw std::runtime_error(original.agent_operation_id ? works_only_here_refusal(snapshot, *original.agent_operation_id)
      : kWorksOnlyHereModelRefused);
}

} // namespace

// Reruns an existing task's exact preset, prompt profile, source selection,
// Matter instructions snapshot and Protected API mode as a new MatterTask,
// preserving the original - Master Spec Sec.66 (Retry) and Sec.67 (Run with
// another model). Both products are the same real backend behavior: Retry
// is this call with no providerId/modelId (reusing the original task's own
// model, or the current default if it never resolved one); Run with
// another model is this call with both set to a genuinely different
// target. `taskId` names which prior task to reuse, not which one to
// replace - it is never mutated or removed, only followed by a new one
// recorded with a higher `attempt` in the same chain. Every eligibility
// rule below is enforced here, not just hidden in the frontend - a caller
// invoking this bridge method directly cannot bypass Retry/Run-with-
// another-model semantics.
matters::Matter retry_task(const Json &params) {
  const platform::lifecycle::ClosingCancelScope closingStops;
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto task_id = params.at("taskId").get<std::string>();

  const bool has_provider_id = params.contains("providerId") && params.at("providerId").is_string();
  const bool has_model_id = params.contains("modelId") && params.at("modelId").is_string();
  if (has_provider_id != has_model_id) {
    throw std::runtime_error("To run with another model, choose both a provider and a model.");
  }
  std::optional<routing::ModelTarget> explicit_target;
  if (has_provider_id && has_model_id) {
    explicit_target =
        routing::ModelTarget{params.at("providerId").get<std::string>(), params.at("modelId").get<std::string>()};
  }

  const auto workspace_path = node::require_active_workspace_path();
  const matters::Matter snapshot = project_snapshot_for_attempts(workspace_path, matter_id);
  const auto original = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
      [&](const matters::MatterTask &task) { return task.id == task_id; });
  if (original == snapshot.tasks.end()) {
    throw std::runtime_error("This task no longer exists.");
  }

  if (!explicit_target && matters::has_committed_document_change(*original)) {
    throw std::runtime_error("Its change is already in the document, so running it again would write it twice. Write a follow-up instead.");
  }
  if (explicit_target) {
    // Run with another model (Sec.67) must actually compare a different
    // model - silently accepting the same target the original attempt
    // already used would present a no-op as a real comparison.
    if (matters::is_same_model_target(*original, explicit_target->provider_id, explicit_target->model_id)) {
      throw std::runtime_error(
          "Choose a different connected model - this is the same model the original task already used.");
    }
  } else if (!matters::is_retry_eligible(*original)) {
    // Retry (same model, Sec.66) is only honest when it can plausibly
    // produce a different outcome. Both ineligible cases point the caller
    // at Run with another model instead, which remains available either
    // way - see matters::is_retry_eligible()'s doc comment.
    if (original->status == matters::TaskStatus::Success) {
      throw std::runtime_error(
          "This task already completed. Choose Run with another model to compare a different model's output.");
    }
    throw std::runtime_error(
        "Retrying with the same settings won't fix this error. Choose Run with another model instead.");
  }

  TaskAttemptSpec spec;
  spec.prompt = original->prompt;
  spec.chat_id = original->chat_id;
  spec.matter_instructions = matters::resolve_retry_matter_instructions(*original, snapshot.instructions);
  spec.legal_task_preset_id = original->legal_task_preset_id;
  spec.legal_task_preset_version = original->legal_task_preset_version;
  spec.prompt_profile_id = original->prompt_profile_id;
  spec.prompt_profile_version = original->prompt_profile_version;
  spec.selected_document_ids = original->selected_document_ids;
  spec.target_document_id = original->professional_target_document_id;
  spec.protection_mode_override = retry_protection_mode(*original);
  spec.root_task_id = matters::task_group_id(*original);
  spec.attempt = matters::next_attempt_number(snapshot.tasks, *original);
  if (explicit_target) {
    spec.model_target = explicit_target;
  } else if (original->provider_id && original->model_id) {
    spec.model_target = routing::ModelTarget{*original->provider_id, *original->model_id};
  }

  const bool keptHere = chat::task_works_only_here(snapshot, *original);
  if (keptHere) keep_attempt_here(snapshot, *original, spec);
  const routing::ScopedLocalOnlyWork localOnlyWork(keptHere);
  auto task = execute_task_attempt(workspace_path, matter_id, snapshot, spec);
  return record_attempt_results(workspace_path, matter_id, {std::move(task)});
}

matters::Matter revise_task(const Json &params) {
  const platform::lifecycle::ClosingCancelScope closingStops;
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto task_id = params.at("taskId").get<std::string>();

  std::optional<std::string> prompt_override;
  if (params.contains("prompt") && params.at("prompt").is_string()) {
    std::string prompt = trim(params.at("prompt").get<std::string>());
    if (prompt.empty()) throw std::runtime_error("Enter a message before continuing.");
    prompt_override = std::move(prompt);
  }

  const bool has_source_policy_mode = params.contains("sourcePolicyMode") && params.at("sourcePolicyMode").is_string();
  std::vector<std::string> selected_document_ids_override;
  if (has_source_policy_mode) {
    const auto mode = params.at("sourcePolicyMode").get<std::string>();
    if (mode != "relevant" && mode != "selected") {
      throw std::runtime_error("This is not a real source policy.");
    }
    if (params.contains("selectedDocumentIds") && params.at("selectedDocumentIds").is_array()) {
      for (const auto &document_id : params.at("selectedDocumentIds")) {
        if (document_id.is_string()) selected_document_ids_override.push_back(document_id.get<std::string>());
      }
    }
    if (mode == "selected" && selected_document_ids_override.empty()) {
      throw std::runtime_error("Choose at least one document, or use Project default sources.");
    }
  }

  const auto workspace_path = node::require_active_workspace_path();
  const matters::Matter snapshot = project_snapshot_for_attempts(workspace_path, matter_id);
  const auto original = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
      [&](const matters::MatterTask &task) { return task.id == task_id; });
  if (original == snapshot.tasks.end()) {
    throw std::runtime_error("This task no longer exists.");
  }
  if (!original->chat_id) {
    throw std::runtime_error("Only a Chat message can be edited.");
  }
  if (original->consilium_group_id || original->workflow_run_id) {
    throw std::runtime_error("This step is part of a Consilium or workflow and can't be edited here.");
  }

  TaskAttemptSpec spec;
  spec.prompt = prompt_override.value_or(original->prompt);
  spec.chat_id = original->chat_id;
  spec.matter_instructions = matters::resolve_retry_matter_instructions(*original, snapshot.instructions);
  spec.legal_task_preset_id = original->legal_task_preset_id;
  spec.legal_task_preset_version = original->legal_task_preset_version;
  spec.prompt_profile_id = original->prompt_profile_id;
  spec.prompt_profile_version = original->prompt_profile_version;
  spec.selected_document_ids = has_source_policy_mode ? selected_document_ids_override : original->selected_document_ids;
  spec.target_document_id = original->professional_target_document_id;
  spec.protection_mode_override = retry_protection_mode(*original);
  spec.root_task_id = matters::task_group_id(*original);
  spec.attempt = matters::next_attempt_number(snapshot.tasks, *original);
  if (original->provider_id && original->model_id) {
    spec.model_target = routing::ModelTarget{*original->provider_id, *original->model_id};
  }

  const bool keptHere = chat::task_works_only_here(snapshot, *original);
  if (keptHere) keep_attempt_here(snapshot, *original, spec);
  const routing::ScopedLocalOnlyWork localOnlyWork(keptHere);
  auto task = execute_task_attempt(workspace_path, matter_id, snapshot, spec);
  return record_attempt_results(workspace_path, matter_id, {std::move(task)});
}

// Master Spec Sec.68 (Consilium): "User chooses 2+ connected models. Each
// independently receives the same logically equivalent Protected task."
// Builds one shared TaskAttemptSpec from `params` exactly like
// task_attempt_spec_from_params() does for matter.runTask, then calls
// execute_task_attempt() once per target - the same pipeline every other
// task uses, never a parallel one. Independence is enforced structurally,
// not by convention: every call is prepared against `*found` as it was
// loaded, before any of this run's own member tasks are appended to it, so
// no member's prepare_task_request() (Matter-history retrieval) can ever
// see a sibling member's answer. All members are appended to the real
// matter and saved together, once, after every target has run - a member
// that fails is still real history (an Error-status MatterTask), same as
// any other task attempt. Deliberately does not reuse
// root_task_id/attempt/exclude_chain: that machinery means "another attempt
// at the same logical task" (Retry/Run with another model), not
// "independent parallel members" - see MatterTask::consilium_group_id.
// One participant of a run, already resolved to the model that will actually
// answer for it. `seat` fields are empty for a legacy targets-only run, which
// genuinely has no seats - only models.
struct ConsiliumSeat {
	routing::ModelTarget target;
	std::string participant_id;
	std::string participant_role;
	std::string stage_id;
	std::string stage_name;
	bool sees_earlier_answers = false;
};

// Turns a SAVED Consilium Configuration into the run it describes.
//
// Until this existed, consilium::Configuration - participants, their AI Setup
// roles and model overrides, the stages and their mode, the objective and the
// source policy - was read by nothing but its own CRUD: matter.runConsilium
// took a flat targets[] array and fanned it out. A lawyer could therefore
// configure a panel in full detail and have none of it govern any run, which
// is worse than not offering the controls at all.
//
// Resolution reuses consilium::list()'s own effective_assignments, so a
// participant's model is decided by exactly the same AI Setup inheritance the
// rest of the product uses - never a second resolver.
std::vector<ConsiliumSeat> seats_from_configuration(const std::string &matter_id,
		const std::string &configuration_id, std::string &objective,
		std::vector<std::string> &selected_document_ids) {
	const auto views = consilium::list(matter_id);
	const auto found = std::find_if(views.begin(), views.end(),
		[&](const consilium::ConfigurationView &view) { return view.configuration.id == configuration_id; });
	if (found == views.end()) throw std::runtime_error("This Consilium no longer exists.");
	const consilium::Configuration &configuration = found->configuration;

	if (objective.empty()) objective = trim(configuration.objective);
	if (objective.empty())
		throw std::runtime_error("This Consilium has no objective yet. Add one before running it.");

	// The source policy is part of the plan, not a per-run afterthought.
	if (configuration.source_policy.mode == consilium::SourceMode::SelectedDocuments) {
		selected_document_ids = configuration.source_policy.selected_document_ids;
		if (configuration.source_policy.requires_selection && selected_document_ids.empty())
			throw std::runtime_error(
				"This Consilium requires selected documents, and none are selected.");
	}

	const auto model_for = [&](const std::string &participant_id) -> routing::ModelTarget {
		const auto assignment = std::find_if(found->effective_assignments.begin(),
			found->effective_assignments.end(),
			[&](const consilium::EffectiveParticipantAssignment &entry) {
				return entry.participant_id == participant_id;
			});
		if (assignment == found->effective_assignments.end() || !assignment->model.has_value())
			throw std::runtime_error(
				"One Consilium participant has no model assigned. Assign a model to every participant "
				"before running it.");
		return *assignment->model;
	};
	const auto participant_for = [&](const std::string &participant_id) -> const consilium::Participant * {
		const auto entry = std::find_if(configuration.participants.begin(),
			configuration.participants.end(),
			[&](const consilium::Participant &candidate) { return candidate.id == participant_id; });
		return entry == configuration.participants.end() ? nullptr : &*entry;
	};

	std::vector<ConsiliumSeat> seats;
	for (const consilium::PlanStage &stage : configuration.stages) {
		for (std::size_t index = 0; index < stage.participant_ids.size(); ++index) {
			const std::string &participant_id = stage.participant_ids[index];
			const consilium::Participant *participant = participant_for(participant_id);
			if (participant == nullptr)
				throw std::runtime_error("This Consilium refers to a participant that no longer exists.");
			ConsiliumSeat seat;
			seat.target = model_for(participant_id);
			seat.participant_id = participant_id;
			seat.participant_role = participant->role;
			seat.stage_id = stage.id;
			seat.stage_name = stage.name;
			// This is what the stage MODE actually means for a deliberation, and
			// the only reading under which it changes anything: in a Parallel
			// stage every seat answers independently, so the views are genuinely
			// separate evidence; in a Sequential stage a seat answers AFTER the
			// ones before it and is shown their answers, which is the whole
			// point of giving a participant a challenger or reviewer role.
			// Across stages, a later stage always sees everything decided
			// earlier - that is what staging a panel is for.
			seat.sees_earlier_answers = !seats.empty()
				&& (stage.mode == consilium::StageMode::Sequential || seat.stage_id != seats.back().stage_id);
			seats.push_back(std::move(seat));
		}
	}
	if (seats.size() < 2)
		throw std::runtime_error(
			"A Consilium needs at least two participants in its stages before it can run.");
	return seats;
}

matters::Matter run_consilium(const Json &params) {
  // A member's request stops on its connection once Singularity is closing,
  // and no other member is asked (Platform/AppLifecycle).
  const platform::lifecycle::ClosingCancelScope closingStops;
  const auto matter_id = params.at("matterId").get<std::string>();
  auto prompt = params.contains("prompt") && params.at("prompt").is_string()
      ? trim(params.at("prompt").get<std::string>()) : std::string();
  const bool from_configuration = params.contains("configurationId")
      && params.at("configurationId").is_string();
  std::vector<std::string> configured_sources;
  std::vector<ConsiliumSeat> seats;
  if (from_configuration) {
    seats = seats_from_configuration(matter_id,
        params.at("configurationId").get<std::string>(), prompt, configured_sources);
  } else {
  if (prompt.empty()) {
    throw std::runtime_error("Enter a task to run.");
  }
  if (!params.contains("targets") || !params.at("targets").is_array()) {
    throw std::runtime_error("Choose at least two connected models for Consilium.");
  }
  std::vector<routing::ModelTarget> targets;
  for (const auto &entry : params.at("targets")) {
    if (!entry.contains("providerId") || !entry.contains("modelId") ||
        !entry.at("providerId").is_string() || !entry.at("modelId").is_string()) {
      throw std::runtime_error("Choose at least two connected models for Consilium.");
    }
    targets.push_back(routing::ModelTarget{entry.at("providerId").get<std::string>(),
                                           entry.at("modelId").get<std::string>()});
  }
  if (targets.size() < 2) {
    throw std::runtime_error("Choose at least two connected models for Consilium.");
  }
  for (std::size_t i = 0; i < targets.size(); ++i) {
    for (std::size_t j = i + 1; j < targets.size(); ++j) {
      if (targets[i].provider_id == targets[j].provider_id && targets[i].model_id == targets[j].model_id) {
        throw std::runtime_error("Choose a different connected model for each Consilium member.");
      }
    }
  }
  // The legacy path is kept working unchanged: every target becomes a seat
  // with no participant identity and no cross-visibility, which is exactly
  // what a targets-only fan-out always was.
  for (const auto &target : targets) seats.push_back(ConsiliumSeat{target, {}, {}, {}, {}, false});
  }

  const auto workspace_path = node::require_active_workspace_path();
  const matters::Matter snapshot = project_snapshot_for_attempts(workspace_path, matter_id);
  auto base_spec = task_attempt_spec_from_params(params, prompt, snapshot);
  if (from_configuration && !configured_sources.empty()) base_spec.selected_document_ids = configured_sources;
  const auto group_id = matters::generate_consilium_group_id();

  std::vector<matters::MatterTask> members;
  members.reserve(seats.size());
  for (const auto &seat : seats) {
    // Singularity is closing: the members who answered are kept, and no other
    // is asked.
    if (platform::lifecycle::is_closing()) break;
    auto spec = base_spec;
    spec.model_target = seat.target;
    // A seat in a Sequential stage, or in any stage after the first, answers
    // WITH the answers already given - that is what the configured stage mode
    // means. A seat in a Parallel stage keeps the original independence, so
    // `snapshot` never gains this loop's own prior members before the next
    // iteration runs.
    bool seesWorkKeptHere = false;
    if (seat.sees_earlier_answers) {
      std::vector<matters::MatterTask> answered;
      for (const auto &earlier : members)
        if (earlier.status == matters::TaskStatus::Success && earlier.response_text)
          answered.push_back(earlier);
      seesWorkKeptHere = answers_kept_here(snapshot, answered);
      if (!answered.empty())
        spec.prompt = matters::build_consilium_deliberation_prompt(prompt, answered,
            seat.participant_role);
    }
    matters::MatterTask task;
    try {
      // A seat shown an answer kept on this computer answers here, or its cloud
      // model is refused and nothing is sent (#229).
      const routing::ScopedLocalOnlyWork keptHere(seesWorkKeptHere);
      task = execute_task_attempt(workspace_path, matter_id, snapshot, spec);
    } catch (const providers::TaskError &error) {
      // A member refused before anything was sent (e.g. its model's known
      // context capacity cannot hold the request) is recorded as that member's
      // failed attempt; the members that ran keep their results.
      task.id = matters::generate_task_id();
      task.created_at = workspaces::current_timestamp();
      task.prompt = spec.prompt;
      task.chat_id = spec.chat_id;
      task.selected_document_ids = spec.selected_document_ids;
      task.legal_task_preset_id = spec.legal_task_preset_id;
      task.legal_task_preset_version = spec.legal_task_preset_version;
      task.requested_provider_id = seat.target.provider_id;
      task.requested_model_id = seat.target.model_id;
      task.status = matters::TaskStatus::Error;
      task.error_code = error.code();
      task.error = error.what();
      task.operation_execution_state = "failed";
    }
    task.consilium_group_id = group_id;
    task.consilium_role = matters::ConsiliumRole::Member;
    // Which seat answered, not merely which model - the thing a lawyer reasons
    // about when a panel disagrees. Empty for a legacy targets-only run, which
    // genuinely had no seats.
    if (!seat.participant_id.empty()) task.consilium_participant_id = seat.participant_id;
    if (!seat.participant_role.empty()) task.consilium_participant_role = seat.participant_role;
    if (from_configuration)
      task.consilium_configuration_id = params.at("configurationId").get<std::string>();
    if (!seat.stage_id.empty()) task.consilium_stage_id = seat.stage_id;
    if (!seat.stage_name.empty()) task.consilium_stage_name = seat.stage_name;
    // CON-1 (billing truth): each member is a real provider call this Project
    // has ALREADY been charged for. Recording the whole run only after the last
    // member returned meant a quit, a crash or a provider hang mid-run threw
    // away every completed member - money spent with nothing to show for it,
    // and no record that it happened. Each member is durable the moment it
    // finishes, exactly like any other task attempt.
    members.push_back(task);
    try {
      (void)record_attempt_results(workspace_path, matter_id, {std::move(task)});
    } catch (const std::exception &) {
      // A Project that cannot be written to is reported by the final save
      // below; one member's persistence failure never stops the run itself.
    }
  }

  return record_attempt_results(workspace_path, matter_id, std::move(members));
}

// "Synthesize with..." (Sec.68): builds a deterministic prompt embedding
// every Consilium member's own provider/model and response verbatim (see
// matters::build_consilium_synthesis_prompt()) and runs it as one ordinary
// task attempt through the chosen model - the same pipeline, so the exact
// same Papinian Guard pass that protected each member's own outbound
// request runs on this embedded transcript too. Not a fake agent: one real
// prompt, one real provider call, appended to Routing Log/Usage exactly
// like any other task.
matters::Matter synthesize_consilium(const Json &params) {
  const platform::lifecycle::ClosingCancelScope closingStops;
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto group_id = params.at("groupId").get<std::string>();
  const auto provider_id = params.at("providerId").get<std::string>();
  const auto model_id = params.at("modelId").get<std::string>();

  const auto workspace_path = node::require_active_workspace_path();
  const matters::Matter snapshot = project_snapshot_for_attempts(workspace_path, matter_id);
  auto members = matters::consilium_members(snapshot.tasks, group_id);
  std::vector<matters::MatterTask> successful_members;
  for (auto &member : members) {
    if (member.status == matters::TaskStatus::Success && member.response_text) {
      successful_members.push_back(std::move(member));
    }
  }
  if (successful_members.empty()) {
    throw std::runtime_error("This Consilium run has no successful answers to synthesize.");
  }
  // Every member's answer goes into the synthesis word for word, so an answer
  // kept on this computer keeps the synthesis here too (#229, D-012): it runs
  // on this computer's model, and a cloud model is refused before anything is
  // sent.
  const bool keptHere = answers_kept_here(snapshot, successful_members)
      || (successful_members.front().chat_id.has_value()
          && chat::chat_holds_work_kept_here(snapshot, *successful_members.front().chat_id));
  if (keptHere && provider_id != "local")
    throw std::runtime_error("An answer in this Consilium was kept on this computer, so its synthesis runs only on "
        "this computer's model. Nothing was sent. Choose a model on this computer to synthesize it.");
  const routing::ScopedLocalOnlyWork localOnlyWork(keptHere);

  TaskAttemptSpec spec;
  spec.prompt = matters::build_consilium_synthesis_prompt(successful_members.front().prompt, successful_members);
  spec.matter_instructions = snapshot.instructions;
  // Phase 26's own real Consilium/operation linkage: every member of this
  // exact group was started under the same chat_id (task_attempt_spec_from_
  // params()'s own chatId handling, above) when this run was started from a
  // Chat, so the synthesis task inherits it here too - never re-supplied by
  // the caller, since it is already real, recorded fact on the group itself.
  spec.chat_id = successful_members.front().chat_id;
  spec.legal_task_preset_id = successful_members.front().legal_task_preset_id;
  spec.legal_task_preset_version = successful_members.front().legal_task_preset_version;
  spec.prompt_profile_id = successful_members.front().prompt_profile_id;
  spec.prompt_profile_version = successful_members.front().prompt_profile_version;
  spec.model_target = routing::ModelTarget{provider_id, model_id};

  auto task = execute_task_attempt(workspace_path, matter_id, snapshot, spec);
  task.consilium_group_id = group_id;
  task.consilium_role = matters::ConsiliumRole::Synthesis;
  return record_attempt_results(workspace_path, matter_id, {std::move(task)});
}

// Master Spec Sec.69 (Completor): "WorkflowDefinition: id, name, version,
// steps... Each step uses the SAME secure inference pipeline. No separate
// fake agent architecture." Runs every step of the chosen
// context::WorkflowDefinition in order, inside this one call - not a
// resumable background job and not an autonomous agent loop: the step
// sequence and each step's preset/instructions are fixed by the
// definition, never chosen by a model. An Inference step is exactly one
// execute_task_attempt() call - the same pipeline matter.runTask/
// matter.retryTask/Consilium all use - appended to `found->tasks` (and
// saved) immediately after it completes, unlike Consilium's members: a
// workflow step is a sequential follow-up, not an independent parallel
// answer, so it deliberately keeps spec.root_task_id unset and lets later
// steps see earlier ones through the exact same unrestricted Matter-history
// retrieval an ordinary follow-up task already gets. A
// CreateIssuesFromFindings step makes no model call at all: it
// deterministically promotes the Findings its named source step's own task
// already parsed into real Issues via matters::promote_finding_to_issue()
// (Sec.26) - the same local action "Add to issues" already performs,
// applied once per Finding, never asking a model to author structured
// Issues. Every step is required in V1 (Sec.69 lists no optional step) - the
// first Error-status Inference task stops the run immediately; steps
// already completed remain real, persisted history, and no placeholder/
// "skipped" task or Issue is ever created for a step that never ran.
matters::Matter run_workflow(const Json &params) {
  const auto matter_id = params.at("matterId").get<std::string>();
  const auto workflow_id = params.at("workflowId").get<std::string>();
  const auto workflow_version = params.at("workflowVersion").get<int>();
  // Phase 26 corrective closeout: a workflow step never receives a caller-
  // supplied providerId/modelId. Proven wrong once already in this exact
  // closeout: leaving TaskAttemptSpec::model_target unset does NOT reach
  // resolve_role()'s real Node -> Project -> Chat inheritance at all -
  // InferenceExecutor.cpp's own unset-target fallback
  // (resolve_generation_inputs()) calls node_default_reasoning_target()
  // directly, which reads only the raw Node-wide role_assignments and has
  // no idea a Project or Chat override even exists. The canonical seam that
  // actually performs the real inheritance is effective_reasoning_target()
  // above (the same function build_state_fingerprint()'s own preview/reveal
  // freshness check already trusts) - called explicitly here, exactly once,
  // never a second resolver implementation.
  // Phase 26's own real workflow/operation linkage: a workflow started from a
  // specific Chat records that Chat's id on every one of its own step tasks,
  // exactly like matter.runTask/runConsilium already do (task_attempt_spec_
  // from_params()'s own chatId handling above) - so ChatConversation::
  // conversation() can honestly group this run inside the Chat it was
  // actually started from, never as an unrelated, unlinked Project task. A
  // workflow started outside any Chat (chatId absent) keeps no Chat linkage.
  const std::optional<std::string> chat_id =
      params.contains("chatId") && params.at("chatId").is_string()
          ? std::optional<std::string>(params.at("chatId").get<std::string>())
          : std::nullopt;

  const auto workflow = context::find_workflow_definition(workflow_id, workflow_version);
  if (!workflow) {
    throw std::runtime_error("This workflow is no longer available.");
  }

  std::vector<std::string> selected_document_ids;
  if (params.contains("selectedDocumentIds")) {
    if (!params.at("selectedDocumentIds").is_array()) {
      throw std::runtime_error("Selected document IDs must be an array.");
    }
    for (const auto &document_id : params.at("selectedDocumentIds")) {
      if (!document_id.is_string()) {
        throw std::runtime_error("Selected document IDs must be strings.");
      }
      selected_document_ids.push_back(document_id.get<std::string>());
    }
  }

  const auto workspace_path = node::require_active_workspace_path();
  matters::Matter snapshot = project_snapshot_for_attempts(workspace_path, matter_id);

  // The one real Node -> Project -> Chat effective Reasoning resolution,
  // read once for the whole run (every step is an ordinary Inference step
  // under the same effective AI Setup - a workflow never mixes roles mid
  // run). An empty result means no Reasoning model is assigned anywhere;
  // model_target is then left unset below so the existing "No default model
  // is configured" refusal still fires, exactly as it would for an
  // ordinary unset-target request.
  const auto settings = platform::load_app_settings();
  const routing::ModelTarget effective_target =
      effective_reasoning_target(settings.local_node_setup, snapshot, chat_id);
  const bool has_effective_target =
      !effective_target.provider_id.empty() && !effective_target.model_id.empty();

  const auto run_id = matters::generate_workflow_run_id();
  // Keyed by this run's own step id, not a matter-wide task id - a
  // CreateIssuesFromFindings step only ever needs to look back within its
  // own workflow run.
  std::unordered_map<std::string, matters::MatterTask> step_tasks_by_id;

  for (const auto &step : workflow->steps) {
    if (step.kind == context::WorkflowStepKind::CreateIssuesFromFindings) {
      const auto source = step_tasks_by_id.find(*step.findings_source_step_id);
      if (source != step_tasks_by_id.end()) {
        snapshot = record_attempt_results(workspace_path, matter_id, {}, [&](matters::Matter &project) {
          const auto timestamp = workspaces::current_timestamp();
          for (const auto &finding : source->second.findings) {
            project.issues.push_back(matters::promote_finding_to_issue(
                finding, matter_id, matters::generate_issue_id(), timestamp));
          }
        });
      }
      continue;
    }

    TaskAttemptSpec spec;
    spec.prompt = step.instructions;
    spec.matter_instructions = snapshot.instructions;
    spec.chat_id = chat_id;
    spec.legal_task_preset_id = step.preset_id;
    spec.legal_task_preset_version = step.preset_version;
    spec.selected_document_ids = selected_document_ids;
    if (has_effective_target) spec.model_target = effective_target;

    auto task = execute_task_attempt(workspace_path, matter_id, snapshot, spec);
    task.workflow_run_id = run_id;
    task.workflow_id = workflow->id;
    task.workflow_version = workflow->version;
    task.workflow_step_id = step.id;
    const bool succeeded = task.status == matters::TaskStatus::Success;
    step_tasks_by_id.emplace(step.id, task);
    snapshot = record_attempt_results(workspace_path, matter_id, {std::move(task)});

    if (!succeeded) {
      break;
    }
  }

  return snapshot;
}


} // namespace singularity::routing::matter_request_execution
