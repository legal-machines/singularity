#include "DocumentReview/DocumentReviewActions/DocumentReviewActions.hpp"
#include "Agent/AgentActions/AgentActions.hpp"
#include "Chat/ChatActions/ChatActions.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

namespace singularity::document_review {
namespace {

using Json = nlohmann::json;

std::string trimmed(const std::string& text) {
	std::size_t first = 0;
	while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
	std::size_t last = text.size();
	while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
	return text.substr(first, last - first);
}

matters::Matter loadMatter(const std::string& projectId) {
	const std::optional<matters::Matter> matter = matters::load_matter(node::require_active_workspace_path(), projectId);
	if (matter.has_value() == false) throw ReviewRefused("This Matter is not open in Singularity on this computer.");
	return *matter;
}

DocumentReview* reviewIn(matters::Matter& matter, const std::string& reviewId) {
	for (DocumentReview& review : matter.document_reviews) {
		if (review.id == reviewId) return &review;
	}
	return nullptr;
}

void announce(const std::string& projectId) {
	platform::events::publish("matter.changed", Json{{"projectId", projectId}});
}

std::string personName(const platform::ActorIdentity& actor) {
	if (actor.display_name.has_value() == true && trimmed(*actor.display_name).empty() == false) return *actor.display_name;
	return "The lawyer";
}

// A step that wrote a document of the colleague's own (D-141): handed in, with
// no review of an original behind it.
bool isWrittenStep(const AssignmentStepRecord& step) {
	return step.reviewId.empty() == true && step.status == "completed" && step.documentId.empty() == false;
}

// The step of the Matter's assignments that wrote the document, if one did.
const AssignmentStepRecord* writtenStepOf(const std::vector<AssignmentRecord>& records, const std::string& documentId) {
	if (documentId.empty() == true) return nullptr;
	for (const AssignmentRecord& record : records) {
		for (const AssignmentStepRecord& step : record.steps) {
			if (isWrittenStep(step) == true && step.documentId == documentId) return &step;
		}
	}
	return nullptr;
}

// The name a lawyer knows a written document by: its title, else its file's.
std::string writtenName(const AssignmentStepRecord& step) {
	return step.label.empty() == false ? step.label : std::filesystem::u8path(step.sourceName).stem().u8string();
}

// ---- Budget ------------------------------------------------------------------

std::mutex& budgetMutex() {
	static std::mutex mutex;
	return mutex;
}

std::filesystem::path budgetPath() {
	return platform::computer_records_directory() / "DocumentIntelligence" / "review-budget.json";
}

ReviewBudget readBudget() {

	ReviewBudget budget;
	std::ifstream input(budgetPath(), std::ios::binary);
	if (!input) return budget;
	std::stringstream text;
	text << input.rdbuf();
	const Json value = Json::parse(text.str(), nullptr, false);
	if (value.is_object() == false) return budget;
	// Only a limit this computer chose (D-132); never the "limitUsd" earlier
	// versions wrote back as their default.
	if (value.contains("chosenLimitUsd") == true && value.at("chosenLimitUsd").is_number() == true)
		budget.limitUsd = std::max(value.at("chosenLimitUsd").get<double>(), 0.0);
	budget.spentMicros = value.value("spentMicros", 0LL);
	budget.calls = value.value("calls", 0);
	budget.estimatedCalls = value.value("estimatedCalls", 0);
	budget.ceilingInputPerMillionUsd = std::max(value.value("ceilingInputPerMillionUsd", 5.0), 0.5);
	budget.ceilingOutputPerMillionUsd = std::max(value.value("ceilingOutputPerMillionUsd", 20.0), 2.0);
	return budget;

}

void writeBudget(const ReviewBudget& budget) {

	const std::filesystem::path path = budgetPath();
	std::filesystem::create_directories(path.parent_path());
	Json value{{"spentMicros", budget.spentMicros}, {"calls", budget.calls},
		{"estimatedCalls", budget.estimatedCalls}, {"ceilingInputPerMillionUsd", budget.ceilingInputPerMillionUsd},
		{"ceilingOutputPerMillionUsd", budget.ceilingOutputPerMillionUsd}};
	if (budget.limitUsd.has_value() == true) value["chosenLimitUsd"] = *budget.limitUsd;
	platform::write_file_durably(path, value.dump(2));

}

} // namespace

std::vector<DocumentReview> listReviews(const std::string& projectId, const std::string& chatId) {

	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = loadMatter(projectId);
	}
	std::vector<DocumentReview> reviews;
	for (const DocumentReview& review : matter.document_reviews) {
		if (chatId.empty() == false && review.chatId != chatId) continue;
		reviews.push_back(review);
	}
	std::sort(reviews.begin(), reviews.end(),
		[](const DocumentReview& left, const DocumentReview& right) { return left.createdAt > right.createdAt; });
	return reviews;

}

DocumentReview getReview(const std::string& projectId, const std::string& reviewId) {

	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	matters::Matter matter = loadMatter(projectId);
	const DocumentReview* review = reviewIn(matter, reviewId);
	if (review == nullptr) throw ReviewRefused("This review is no longer in the Matter.");
	return *review;

}

std::optional<DocumentReview> openReviewInThread(const std::string& projectId, const std::string& chatId,
	const std::string& agentId) {

	std::optional<DocumentReview> latest;
	for (const DocumentReview& review : listReviews(projectId, chatId)) {
		if (review.agentId != agentId || review.versions.empty() == true) continue;
		if (latest.has_value() == false || review.createdAt > latest->createdAt) latest = review;
	}
	if (latest.has_value() == false || latest->versions.back().state == "accepted") return std::nullopt;
	return latest;

}

DocumentReview addReview(const std::string& projectId, DocumentReview review) {

	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = loadMatter(projectId);
		if (review.createdAt.empty() == true) review.createdAt = workspaces::current_timestamp();
		review.updatedAt = review.createdAt;
		matter.document_reviews.push_back(review);
		matters::save_matter(workspace, matter);
	}
	announce(projectId);
	return review;

}

DocumentReview changeReview(const std::string& projectId, const std::string& reviewId,
	const std::function<void(DocumentReview&)>& change) {

	DocumentReview changed;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = loadMatter(projectId);
		DocumentReview* review = reviewIn(matter, reviewId);
		if (review == nullptr) throw ReviewRefused("This review is no longer in the Matter.");
		change(*review);
		review->updatedAt = workspaces::current_timestamp();
		changed = *review;
		matters::save_matter(workspace, matter);
	}
	announce(projectId);
	return changed;

}

std::vector<AssignmentRecord> listAssignments(const std::string& projectId, const std::string& chatId) {

	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = loadMatter(projectId);
	}
	std::vector<AssignmentRecord> records;
	for (const AssignmentRecord& record : matter.assignment_records) {
		if (chatId.empty() == false && record.chatId != chatId) continue;
		records.push_back(record);
	}
	std::sort(records.begin(), records.end(),
		[](const AssignmentRecord& left, const AssignmentRecord& right) { return left.startedAt > right.startedAt; });
	return records;

}

AssignmentRecord addAssignment(const std::string& projectId, AssignmentRecord record) {

	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = loadMatter(projectId);
		if (record.id.empty() == true) record.id = "assignment_" + platform::secure_random_hex(8);
		if (record.startedAt.empty() == true) record.startedAt = workspaces::current_timestamp();
		matter.assignment_records.push_back(record);
		matters::save_matter(workspace, matter);
	}
	announce(projectId);
	return record;

}

AssignmentRecord changeAssignment(const std::string& projectId, const std::string& recordId,
	const std::function<void(AssignmentRecord&)>& change) {

	AssignmentRecord changed;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = loadMatter(projectId);
		AssignmentRecord* record = nullptr;
		for (AssignmentRecord& entry : matter.assignment_records) {
			if (entry.id == recordId) record = &entry;
		}
		if (record == nullptr) throw ReviewRefused("This assignment's record is no longer in the Matter.");
		change(*record);
		changed = *record;
		matters::save_matter(workspace, matter);
	}
	announce(projectId);
	return changed;

}

DecisionAnswer answerDecision(const std::string& projectId, const std::string& recordId, const std::string& decisionId,
	const std::string& answer) {

	const platform::ActorIdentity actor = platform::resolve_actor_identity();
	const std::string words = trimmed(answer);
	if (words.empty() == true) throw ReviewRefused("An answer needs its words.");
	if (words.size() > 2000) throw ReviewRefused("An answer is longer than 2,000 characters.");
	DecisionAnswer answered;
	bool isWaiting = false;
	answered.record = changeAssignment(projectId, recordId, [&](AssignmentRecord& record) {
		// Read under the record's lock, as the work reads it before it waits.
		isWaiting = record.state == "waiting";
		AssignmentDecision* decision = nullptr;
		for (AssignmentDecision& entry : record.decisions) {
			if (entry.id == decisionId) decision = &entry;
		}
		if (decision == nullptr) throw ReviewRefused("This decision is no longer in the assignment.");
		if (decision->state == "answered") throw ReviewRefused("This decision was answered already: " + decision->answer);
		decision->state = "answered";
		decision->answer = words;
		decision->answeredByActorId = actor.actor_id;
		decision->answeredByName = personName(actor);
		decision->answeredAt = workspaces::current_timestamp();
		// What the lawyer says to the colleague: the document as the colleague
		// speaks of it, the place, and the answer.
		std::string about;
		if (decision->stepNumber > 0 && static_cast<std::size_t>(decision->stepNumber) <= record.steps.size())
			about = record.steps[static_cast<std::size_t>(decision->stepNumber) - 1].label;
		if (about.empty() == true && decision->documentId.empty() == false && decision->documentId == record.email.documentId)
			about = "the email";
		if (about.empty() == true) about = std::filesystem::u8path(decision->documentName).stem().u8string();
		std::string said = decision->place.empty() == false ? decision->place + " of " + about : about;
		if (said.empty() == false) said.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(said.front())));
		said += ": " + words;
		if (std::string(".?!").find(said.back()) == std::string::npos) said += ".";
		answered.words = said;
	});
	answered.namesColleague = isWaiting == true
		&& agents::has_work_in_line(answered.record.agentId, answered.record.chatId) == false;
	return answered;

}

std::vector<AssignmentView> viewAssignments(const std::string& projectId, const std::string& chatId) {

	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = loadMatter(projectId);
	}
	const auto reviewOf = [&](const std::string& id) -> const DocumentReview* {
		for (const DocumentReview& review : matter.document_reviews) {
			if (review.id == id) return &review;
		}
		return nullptr;
	};
	const auto decisionOn = [](const DocumentReview& review, int version) -> const ReviewDecision* {
		const ReviewDecision* found = nullptr;
		for (const ReviewDecision& decision : review.decisions) {
			if (decision.version == version) found = &decision;
		}
		return found;
	};
	const auto stemOf = [](const std::string& name) { return std::filesystem::u8path(name).stem().u8string(); };
	std::vector<AssignmentView> views;
	for (const AssignmentRecord& record : matter.assignment_records) {
		if (chatId.empty() == false && record.chatId != chatId) continue;
		AssignmentView view;
		view.record = record;
		for (const AssignmentStepRecord& step : record.steps) {
			AssignmentStepState state;
			const DocumentReview* review = reviewOf(step.reviewId);
			if (review != nullptr && review->versions.empty() == false) {
				const ReviewVersion& latest = review->versions.back();
				state.state = latest.state;
				state.latestVersion = latest.number;
				if (const ReviewDecision* decided = decisionOn(*review, latest.number); decided != nullptr) {
					state.decidedBy = decided->actorName;
					state.decidedAt = decided->decidedAt;
					state.isAsWritten = decided->isAsWritten;
				}
				const std::string number = std::to_string(latest.number);
				// Plain workflow states, said as a firm says them (D-174): a version
				// ready for review is no alarm, and only what failed reads as one.
				if (latest.state == "accepted") {
					state.words = "Version " + number + " \xC2\xB7 Approved by " + (state.decidedBy.empty() == true ? std::string("you")
						: state.decidedBy) + (state.isAsWritten == true ? std::string(", as handed in")
						: std::string(", with later edits"));
				} else if (latest.state == "ready_for_review") {
					state.words = "Version " + number + " \xC2\xB7 Ready for review";
				} else if (latest.state == "returned") {
					state.words = "Version " + number + " \xC2\xB7 Returned for revision";
				} else if (latest.state == "failed") {
					state.words = "Version " + number + " \xC2\xB7 Not done: " + latest.stateMessage;
				} else if (latest.state == "running" || latest.state == "preparing" || latest.state == "producing"
						|| latest.state == "queued") {
					state.words = "In progress";
				} else if (latest.state == "waiting_for_information") {
					state.words = "Decision required";
				} else {
					state.words = latest.stateMessage.empty() == false ? latest.stateMessage : step.statusWords;
				}
			} else if (isWrittenStep(step) == true) {
				// A document the colleague wrote waits for the lawyer as a revised
				// draft does, until one accepts or returns it (D-143).
				state.latestVersion = 1;
				if (step.decisions.empty() == true) {
					state.state = "ready_for_review";
					state.words = step.statusWords.empty() == false ? step.statusWords : std::string("Ready for review");
				} else {
					const ReviewDecision& decided = step.decisions.back();
					state.state = decided.decision;
					state.decidedBy = decided.actorName;
					state.decidedAt = decided.decidedAt;
					state.isAsWritten = decided.isAsWritten;
					const std::string who = decided.actorName.empty() == true ? std::string("you") : decided.actorName;
					state.words = decided.decision == "accepted"
						? "Approved by " + who + (decided.isAsWritten == true ? std::string(", as written")
							: std::string(", with later edits"))
						: "Returned for revision by " + who;
				}
			} else {
				state.words = step.statusWords;
			}
			view.steps.push_back(state);
		}
		// The email is ready only while every draft it carries is the version
		// accepted, as it was attached.
		if (record.email.state == "drafted") {
			for (const AssignmentAttachment& attached : record.email.attachments) {
				// A document a colleague wrote goes only once a lawyer accepted it,
				// as it was then (D-143); a document of the Matter attached as it
				// is - the NDA as signed - asks for no acceptance.
				if (attached.reviewId.empty() == true) {
					const AssignmentStepRecord* written = writtenStepOf(matter.assignment_records, attached.documentId);
					if (written == nullptr) continue;
					const std::string name = writtenName(*written);
					if (written->decisions.empty() == true) {
						view.email.waitingFor.push_back(name);
					} else if (written->decisions.back().decision != "accepted") {
						view.email.replaced.push_back(name);
					} else if (written->decisions.back().sha256.empty() == false && attached.sha256.empty() == false
						&& written->decisions.back().sha256 != attached.sha256) {
						view.email.changedSince.push_back(name);
					}
					continue;
				}
				const DocumentReview* review = reviewOf(attached.reviewId);
				const std::string name = review != nullptr ? stemOf(review->sourceFileName) : attached.name;
				const ReviewVersion* version = nullptr;
				if (review != nullptr) {
					for (const ReviewVersion& entry : review->versions) {
						if (entry.number == attached.version) version = &entry;
					}
				}
				if (version == nullptr) {
					view.email.replaced.push_back(name);
					continue;
				}
				const std::string which = name + ", version " + std::to_string(version->number);
				if (version->state == "accepted") {
					const ReviewDecision* decided = decisionOn(*review, version->number);
					const std::string acceptedSha = decided != nullptr && decided->sha256.empty() == false ? decided->sha256
						: version->resultSha256;
					if (acceptedSha.empty() == false && attached.sha256.empty() == false && acceptedSha != attached.sha256)
						view.email.changedSince.push_back(which);
				} else if (version->state == "ready_for_review") {
					view.email.waitingFor.push_back(which);
				} else {
					view.email.replaced.push_back(which);
				}
			}
			// An email with no one to send it to is never ready, whatever it carries:
			// a reply drafted with its recipient blank was called "Ready to send"
			// (2026-10-26).
			const bool hasRecipient = trimmed(record.email.to).empty() == false;
			// Nor is one whose sending waits on a decision of the lawyer's still
			// open: a reply was called "Ready to send once you accept the draft"
			// while the term it proposes waited on the lawyer (2026-10-26).
			std::size_t openDecisions = 0;
			for (const AssignmentDecision& decision : record.decisions) {
				if (decision.isBlocking == true && decision.state == "open") ++openDecisions;
			}
			view.email.isReady = hasRecipient == true && openDecisions == 0 && view.email.waitingFor.empty() == true
				&& view.email.replaced.empty() == true && view.email.changedSince.empty() == true;
			// Said in a line a list row holds; which versions they are is in
			// waitingFor, replaced and changedSince.
			if (view.email.isReady == true) {
				view.email.words = record.email.attachments.empty() == true ? std::string("Ready to send")
					: std::string("Ready to send: it carries the versions you accepted");
			} else if (view.email.replaced.empty() == false || view.email.changedSince.empty() == false) {
				const std::size_t stale = view.email.replaced.size() + view.email.changedSince.size();
				view.email.words = std::string(stale == 1 ? "A draft it carries was" : "Drafts it carries were")
					+ " replaced or changed after it was written: ask for the email again";
			} else if (hasRecipient == false) {
				view.email.words = "Not ready to send: it has no recipient yet";
			} else if (openDecisions > 0) {
				view.email.words = openDecisions == 1 ? std::string("Not ready to send: it waits for your decision")
					: std::string("Not ready to send: it waits for your decisions");
			} else {
				const std::size_t waiting = view.email.waitingFor.size();
				view.email.words = waiting == 1 ? std::string("Ready to send once you accept the draft it carries")
					: "Ready to send once you accept the " + std::to_string(waiting) + " drafts it carries";
			}
		}
		views.push_back(std::move(view));
	}
	std::sort(views.begin(), views.end(),
		[](const AssignmentView& left, const AssignmentView& right) { return left.record.startedAt > right.record.startedAt; });
	return views;

}

void linkTurn(const std::string& projectId, const std::string& taskId, const std::string& reviewId,
	const std::vector<std::string>& workProductIds) {

	if (taskId.empty() == true) return;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = loadMatter(projectId);
		bool isFound = false;
		for (matters::MatterTask& task : matter.tasks) {
			if (task.id != taskId) continue;
			task.document_review_id = reviewId;
			for (const std::string& id : workProductIds) {
				if (std::find(task.result_work_product_ids.begin(), task.result_work_product_ids.end(), id)
					== task.result_work_product_ids.end()) {
					task.result_work_product_ids.push_back(id);
				}
			}
			isFound = true;
		}
		if (isFound == false) return;
		matters::save_matter(workspace, matter);
	}
	announce(projectId);

}

DocumentReview decide(const DecisionCommand& command) {

	const std::string decision = trimmed(command.decision);
	if (decision != "accept" && decision != "return") throw ReviewRefused("A version is either accepted or returned.");
	const std::string comment = trimmed(command.comment);
	if (decision == "return" && comment.empty() == true) {
		throw ReviewRefused("Say what should change, so the draft can be revised.");
	}
	const auto requireDecidable = [&](const DocumentReview& review) {
		if (review.versions.empty() == true) throw ReviewRefused("This review has no version to decide on yet.");
		const ReviewVersion& latest = review.versions.back();
		if (latest.number != command.version) {
			throw ReviewRefused("Version " + std::to_string(command.version) + " is no longer the latest; decide on version "
				+ std::to_string(latest.number) + ".");
		}
		if (latest.state != "ready_for_review") {
			throw ReviewRefused("Version " + std::to_string(latest.number) + " is not waiting for your review.");
		}
	};
	const DocumentReview current = getReview(command.projectId, command.reviewId);
	requireDecidable(current);

	// What is accepted is the draft as it is now (D-119): read again, so the
	// record says whether it is the colleague's version as written or as someone
	// changed it since in Word, Excel or PowerPoint - and a draft no longer there
	// is never accepted.
	std::string acceptedSha;
	bool isAsWritten = true;
	if (decision == "accept" && current.versions.back().resultRelativePath.empty() == false) {
		std::optional<matters::Matter> matter;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			matter = matters::load_matter(node::require_active_workspace_path(), command.projectId);
		}
		const std::filesystem::path folder = std::filesystem::u8path(matter.has_value() == true
			? matter->directory_path.value_or(std::string()) : std::string());
		const std::filesystem::path draft = (folder / std::filesystem::u8path(current.versions.back().resultRelativePath))
			.lexically_normal();
		std::error_code missing;
		if (folder.empty() == true || std::filesystem::is_regular_file(draft, missing) == false) {
			throw ReviewRefused("Version " + std::to_string(command.version) + "'s draft is no longer in the Matter's folder, so "
				"it was not accepted.");
		}
		acceptedSha = platform::sha256_hex_digest_of_file(draft);
		isAsWritten = current.versions.back().resultSha256.empty() == true || acceptedSha == current.versions.back().resultSha256;
	}

	// Said in the thread as the lawyer's own message; a return names the
	// colleague, who takes the comments up as its next request.
	const platform::ActorIdentity actor = platform::resolve_actor_identity();
	const std::string number = std::to_string(command.version);
	const std::string text = decision == "accept"
		? "Accepted version " + number + " of the revised draft" + (isAsWritten == true ? std::string(".")
			: std::string(", with the changes made to it since it was handed in."))
		: "Returning version " + number + " with comments: " + comment;
	std::vector<std::string> mentions;
	if (decision == "return") mentions.push_back(current.agentId);
	const matters::MatterTask message = chat::post_member_message(command.projectId, current.chatId, text, mentions);

	const DocumentReview decided = changeReview(command.projectId, command.reviewId, [&](DocumentReview& review) {
		requireDecidable(review);
		ReviewDecision record;
		record.id = "decision_" + platform::secure_random_hex(8);
		record.version = command.version;
		record.decision = decision == "accept" ? "accepted" : "returned";
		record.actorId = actor.actor_id;
		record.actorName = message.author_name.empty() == false ? message.author_name : personName(actor);
		record.decidedAt = workspaces::current_timestamp();
		record.comment = comment;
		record.messageId = message.id;
		record.sha256 = acceptedSha;
		record.isAsWritten = isAsWritten;
		review.decisions.push_back(record);
		review.versions.back().state = record.decision;
	});
	linkTurn(command.projectId, message.id, command.reviewId);

	if (decision == "return") {
		agents::MentionMessage mentioned;
		mentioned.matter_id = command.projectId;
		mentioned.chat_id = current.chatId;
		mentioned.message_id = message.id;
		mentioned.text = message.prompt;
		mentioned.author_actor_id = message.author_actor_id;
		mentioned.author_name = message.author_name;
		mentioned.mentions = message.mentions;
		mentioned.posted_here = true;
		(void)agents::take_up_mentions(mentioned);
	}
	return decided;

}

int giveBackEarlierVersion(DocumentReview& review) {
	if (review.versions.size() < 2) return 0;
	const ReviewVersion& pass = review.versions.back();
	if (pass.resultDocumentId.empty() == false) return 0;
	const bool isUnderway = pass.state == "running" || pass.state == "preparing" || pass.state == "producing"
		|| pass.state == "queued";
	// A pass that changed nothing gives it back too: it wrote no draft of its own
	// (2026-10-30).
	if (pass.state != "failed" && pass.state != "unchanged" && isUnderway == false) return 0;
	review.versions.pop_back();
	ReviewVersion& earlier = review.versions.back();
	if (earlier.state == "superseded") {
		earlier.state = "ready_for_review";
		earlier.stateMessage.clear();
	}
	return earlier.state == "ready_for_review" ? earlier.number : 0;
}

std::string writtenAwaitingAcceptance(const std::vector<AssignmentRecord>& records, const std::string& documentId) {
	const AssignmentStepRecord* written = writtenStepOf(records, documentId);
	if (written == nullptr) return std::string();
	if (written->decisions.empty() == false && written->decisions.back().decision == "accepted") return std::string();
	return writtenName(*written);
}

AssignmentRecord decideWritten(const WrittenDecisionCommand& command) {

	const std::string decision = trimmed(command.decision);
	if (decision != "accept" && decision != "return") throw ReviewRefused("A document is either accepted or returned.");
	const std::string comment = trimmed(command.comment);
	if (decision == "return" && comment.empty() == true) {
		throw ReviewRefused("Say what should change, so the document can be revised.");
	}
	if (comment.size() > 4000) throw ReviewRefused("The comments are longer than 4,000 characters.");
	const auto decidable = [&](AssignmentRecord& record) -> AssignmentStepRecord& {
		if (command.step < 1 || static_cast<std::size_t>(command.step) > record.steps.size()) {
			throw ReviewRefused("This assignment has no such document.");
		}
		AssignmentStepRecord& step = record.steps[static_cast<std::size_t>(command.step) - 1];
		if (isWrittenStep(step) == false) throw ReviewRefused("This step wrote no document of its own to decide on.");
		if (step.decisions.empty() == false) {
			const ReviewDecision& earlier = step.decisions.back();
			throw ReviewRefused(writtenName(step) + " was " + earlier.decision + " already"
				+ (earlier.actorName.empty() == true ? std::string(".") : " by " + earlier.actorName + "."));
		}
		return step;
	};
	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = loadMatter(command.projectId);
	}
	AssignmentRecord current;
	bool isFound = false;
	for (const AssignmentRecord& record : matter.assignment_records) {
		if (record.id == command.assignmentId) {
			current = record;
			isFound = true;
		}
	}
	if (isFound == false) throw ReviewRefused("This assignment's record is no longer in the Matter.");
	const AssignmentStepRecord step = decidable(current);
	const std::string name = writtenName(step);

	// What is accepted is the document as it is now: read again, so the record
	// says whether it is the colleague's as written or as someone changed it
	// since - and a document no longer there is never accepted.
	std::string acceptedSha;
	bool isAsWritten = true;
	if (decision == "accept") {
		const matters::MatterDocument* document = nullptr;
		for (const matters::MatterDocument& entry : matter.documents) {
			if (entry.id == step.documentId) document = &entry;
		}
		const std::filesystem::path folder = std::filesystem::u8path(matter.directory_path.value_or(std::string()));
		std::filesystem::path file;
		if (document != nullptr && folder.empty() == false
			&& documents::registry::is_relative_path_safe(folder, document->relative_path) == true) {
			file = (folder / std::filesystem::u8path(document->relative_path)).lexically_normal();
		}
		std::error_code missing;
		if (file.empty() == true || std::filesystem::is_regular_file(file, missing) == false) {
			throw ReviewRefused(name + " is no longer in the Matter's folder, so it was not accepted.");
		}
		acceptedSha = platform::sha256_hex_digest_of_file(file);
		isAsWritten = step.writtenSha256.empty() == true || acceptedSha == step.writtenSha256;
	}

	// Said in the thread as the lawyer's own message; a return names the
	// colleague, who takes the comments up as its next request.
	const platform::ActorIdentity actor = platform::resolve_actor_identity();
	const std::string text = decision == "accept"
		? "Accepted " + name + (isAsWritten == true ? std::string(".") : std::string(", with the changes made to it since it was written."))
		: "Returning " + name + " with comments: " + comment;
	std::vector<std::string> mentions;
	if (decision == "return") mentions.push_back(current.agentId);
	const matters::MatterTask message = chat::post_member_message(command.projectId, current.chatId, text, mentions);

	const AssignmentRecord decided = changeAssignment(command.projectId, command.assignmentId, [&](AssignmentRecord& record) {
		AssignmentStepRecord& kept = decidable(record);
		ReviewDecision recorded;
		recorded.id = "decision_" + platform::secure_random_hex(8);
		recorded.version = 1;
		recorded.decision = decision == "accept" ? "accepted" : "returned";
		recorded.actorId = actor.actor_id;
		recorded.actorName = message.author_name.empty() == false ? message.author_name : personName(actor);
		recorded.decidedAt = workspaces::current_timestamp();
		recorded.comment = comment;
		recorded.messageId = message.id;
		recorded.sha256 = acceptedSha;
		recorded.isAsWritten = isAsWritten;
		kept.decisions.push_back(recorded);
	});

	if (decision == "return") {
		agents::MentionMessage mentioned;
		mentioned.matter_id = command.projectId;
		mentioned.chat_id = current.chatId;
		mentioned.message_id = message.id;
		mentioned.text = message.prompt;
		mentioned.author_actor_id = message.author_actor_id;
		mentioned.author_name = message.author_name;
		mentioned.mentions = message.mentions;
		mentioned.posted_here = true;
		(void)agents::take_up_mentions(mentioned);
	}
	return decided;

}

DocumentReview retry(const std::string& projectId, const std::string& reviewId) {

	const DocumentReview review = getReview(projectId, reviewId);
	if (review.versions.empty() == true) throw ReviewRefused("This review has nothing to try again.");
	const ReviewVersion& latest = review.versions.back();
	if (latest.state != "failed" && latest.state != "waiting_for_information") {
		throw ReviewRefused("Version " + std::to_string(latest.number) + " did not fail, so there is nothing to try again.");
	}
	const platform::ActorIdentity actor = platform::resolve_actor_identity();
	(void)agents::assign_in_thread(review.agentId, projectId, latest.instruction.empty() == true ? std::string("Try the review again.")
		: latest.instruction, review.chatId, actor.actor_id);
	return changeReview(projectId, reviewId, [&](DocumentReview& changed) {
		changed.versions.back().state = "queued";
		changed.versions.back().stateMessage.clear();
	});

}

ReviewBudget loadReviewBudget() {
	const std::lock_guard<std::mutex> lock(budgetMutex());
	return readBudget();
}

void recordReviewSpend(long long micros, bool isEstimated) {

	const std::lock_guard<std::mutex> lock(budgetMutex());
	ReviewBudget budget = readBudget();
	budget.spentMicros += std::max(micros, 0LL);
	budget.calls += 1;
	if (isEstimated == true) budget.estimatedCalls += 1;
	writeBudget(budget);

}

long long reviewCallCeilingMicros(std::size_t promptCharacters, std::uint64_t maximumOutputTokens,
	const ReviewBudget& budget) {

	// Three characters a token at the most, so the bound is never below the
	// request's real size.
	const double inputTokens = static_cast<double>(promptCharacters) / 3.0;
	const double micros = inputTokens * budget.ceilingInputPerMillionUsd
		+ static_cast<double>(maximumOutputTokens) * budget.ceilingOutputPerMillionUsd;
	return static_cast<long long>(std::ceil(micros));

}

std::optional<long long> reviewCostBoundMicros(std::size_t promptCharacters, std::optional<std::uint64_t> outputTokens,
	std::optional<double> inputPricePerToken, std::optional<double> outputPricePerToken, const ReviewBudget& budget) {

	if (outputTokens.has_value() == false) return std::nullopt;
	// Three characters a token at the most, so the bound is never below the
	// request's real size.
	const double inputTokens = static_cast<double>(promptCharacters) / 3.0;
	const double inputMicros = inputPricePerToken.has_value() == true ? inputTokens * *inputPricePerToken * 1000000.0
		: inputTokens * budget.ceilingInputPerMillionUsd;
	const double outputMicros = outputPricePerToken.has_value() == true
		? static_cast<double>(*outputTokens) * *outputPricePerToken * 1000000.0
		: static_cast<double>(*outputTokens) * budget.ceilingOutputPerMillionUsd;
	return static_cast<long long>(std::ceil(inputMicros + outputMicros));

}

std::uint64_t affordableOutputTokens(std::size_t promptCharacters, std::uint64_t wanted,
	std::optional<double> inputPricePerToken, std::optional<double> outputPricePerToken, const ReviewBudget& budget,
	std::uint64_t least) {

	// A review asked for the model's whole limit (128,000 tokens for Claude
	// Sonnet 5.5) was bounded at $1.32 and refused with $1.26 of a $2.00
	// budget left, though it would have cost $0.08 (a live run, 25 Oct 2026):
	// the budget held a third of what it said.
	// No limit chosen: the request asks for all it wants (D-132).
	if (budget.limitUsd.has_value() == false) return wanted;
	const long long left = static_cast<long long>(*budget.limitUsd * 1000000.0) - budget.spentMicros;
	const std::optional<long long> whole = reviewCostBoundMicros(promptCharacters, wanted, inputPricePerToken,
		outputPricePerToken, budget);
	if (whole.has_value() == true && *whole <= left) return wanted;
	const long long input = reviewCostBoundMicros(promptCharacters, std::uint64_t{0}, inputPricePerToken,
		outputPricePerToken, budget).value_or(0);
	// Micro-dollars a written token costs at most: the model's price, or the
	// budget's ceiling (dollars per million tokens are micro-dollars a token).
	const double perToken = outputPricePerToken.has_value() == true ? *outputPricePerToken * 1000000.0
		: budget.ceilingOutputPerMillionUsd;
	if (left <= input || perToken <= 0.0) return 0;
	const std::uint64_t fits = static_cast<std::uint64_t>(std::floor(static_cast<double>(left - input) / perToken));
	std::uint64_t asked = std::min(fits, wanted);
	// The bound itself decides: a price that is no exact binary fraction can
	// round the bound a micro-dollar over what is left.
	for (int step = 0; step < 64 && asked > 0; ++step) {
		const std::optional<long long> bound = reviewCostBoundMicros(promptCharacters, asked, inputPricePerToken,
			outputPricePerToken, budget);
		if (bound.has_value() == true && *bound <= left) break;
		--asked;
	}
	const std::optional<long long> bound = reviewCostBoundMicros(promptCharacters, asked, inputPricePerToken,
		outputPricePerToken, budget);
	if (bound.has_value() == false || *bound > left) return 0;
	return asked >= std::min(least, wanted) ? asked : 0;

}

} // namespace singularity::document_review
