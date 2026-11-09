#include "Bridge/DocumentReviewBridge/DocumentReviewBridge.hpp"
#include "Bridge/AgentBridge/AgentBridge.hpp"
#include "DocumentReview/AssignmentRun/AssignmentRun.hpp"
#include "DocumentReview/DocumentReviewActions/DocumentReviewActions.hpp"
#include "DocumentReview/Types/DocumentReviewJson.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"

#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

namespace singularity::bridge::document_review_bridge {
namespace {

using Json = nlohmann::json;
namespace review = document_review;

std::string required(const Json& params, const char* key) {
	if (params.contains(key) == false || params.at(key).is_string() == false || params.at(key).get<std::string>().empty() == true) {
		throw std::invalid_argument(std::string("Missing ") + key + ".");
	}
	return params.at(key).get<std::string>();
}

int requiredVersion(const Json& params) {
	if (params.contains("version") == false || params.at("version").is_number_integer() == false) {
		throw std::invalid_argument("Missing version.");
	}
	return params.at("version").get<int>();
}

} // namespace

Json list(const Json& params) {
	const std::string chatId = params.contains("chatId") == true && params.at("chatId").is_string() == true
		? params.at("chatId").get<std::string>() : std::string();
	Json reviews = Json::array();
	for (const review::DocumentReview& entry : review::listReviews(required(params, "projectId"), chatId)) {
		reviews.push_back(review::reviewToJson(entry));
	}
	return Json{{"reviews", std::move(reviews)}};
}

Json get(const Json& params) {
	return review::reviewToJson(review::getReview(required(params, "projectId"), required(params, "reviewId")));
}

Json decide(const Json& params) {
	review::DecisionCommand command;
	command.projectId = required(params, "projectId");
	command.reviewId = required(params, "reviewId");
	command.version = requiredVersion(params);
	command.decision = required(params, "decision");
	command.comment = params.contains("comment") == true && params.at("comment").is_string() == true
		? params.at("comment").get<std::string>() : std::string();
	return review::reviewToJson(review::decide(command));
}

Json retry(const Json& params) {
	return review::reviewToJson(review::retry(required(params, "projectId"), required(params, "reviewId")));
}

namespace {

// An assignment as a lawyer reads it now: its record, where each step's review
// stands, and whether its email is ready to send.
Json assignmentViewJson(const review::AssignmentView& view) {
	Json value = review::assignmentToJson(view.record);
	// The execution's own record - each tool call, the document handles, the
	// turns - is the backend's; the window reads the work's results.
	value.erase("toolCalls");
	value.erase("handles");
	value.erase("turns");
	for (std::size_t index = 0; index < view.steps.size() && index < value["steps"].size(); ++index) {
		const review::AssignmentStepState& state = view.steps[index];
		Json& step = value["steps"][index];
		// A written document's decisions are read as a review's are: in its
		// state, who decided and when, not as their records.
		step.erase("writtenSha256");
		step.erase("decisions");
		step["state"] = state.state;
		step["latestVersion"] = state.latestVersion;
		step["decidedBy"] = state.decidedBy;
		step["decidedAt"] = state.decidedAt;
		step["isAsWritten"] = state.isAsWritten;
		step["stateWords"] = state.words;
	}
	Json& email = value["email"];
	email["isReady"] = view.email.isReady;
	email["waitingFor"] = view.email.waitingFor;
	email["replaced"] = view.email.replaced;
	email["changedSince"] = view.email.changedSince;
	email["readiness"] = view.email.words;
	return value;
}

} // namespace

Json assignments(const Json& params) {
	const std::string chatId = params.contains("chatId") == true && params.at("chatId").is_string() == true
		? params.at("chatId").get<std::string>() : std::string();
	Json listed = Json::array();
	for (const review::AssignmentView& view : review::viewAssignments(required(params, "projectId"), chatId))
		listed.push_back(assignmentViewJson(view));
	return Json{{"assignments", std::move(listed)}};
}

Json answerDecision(const Json& params) {
	const std::string projectId = required(params, "projectId");
	const std::string assignmentId = required(params, "assignmentId");
	const review::DecisionAnswer answered = review::answerDecision(projectId, assignmentId, required(params, "decisionId"),
		required(params, "answer"));
	// The lawyer's word goes to the colleague as any message of theirs does: it
	// names the colleague, who takes the work up again, and replies to the
	// closing message - where the domain says so. Otherwise the work takes the
	// answer from its record, and the words stand in the thread addressed to no
	// one: named, or replying to the colleague, they would line the same work up
	// a second time.
	Json message{{"projectId", projectId}, {"chatId", answered.record.chatId}, {"text", answered.words}};
	if (answered.namesColleague == true) {
		message["text"] = "@" + answered.record.agentName + " " + answered.words;
		message["mentions"] = Json::array({answered.record.agentId});
		if (answered.record.summaryMessageId.empty() == false) message["quoteMessageId"] = answered.record.summaryMessageId;
	}
	const Json posted = agent_bridge::post_message(message);
	Json view;
	for (const review::AssignmentView& entry : review::viewAssignments(projectId, answered.record.chatId)) {
		if (entry.record.id == assignmentId) view = assignmentViewJson(entry);
	}
	return Json{{"assignment", view}, {"message", posted}};
}

Json decideWritten(const Json& params) {
	review::WrittenDecisionCommand command;
	command.projectId = required(params, "projectId");
	command.assignmentId = required(params, "assignmentId");
	if (params.contains("step") == false || params.at("step").is_number_integer() == false) {
		throw std::invalid_argument("Missing step.");
	}
	command.step = params.at("step").get<int>();
	command.decision = required(params, "decision");
	command.comment = params.contains("comment") == true && params.at("comment").is_string() == true
		? params.at("comment").get<std::string>() : std::string();
	const review::AssignmentRecord decided = review::decideWritten(command);
	for (const review::AssignmentView& entry : review::viewAssignments(command.projectId, decided.chatId)) {
		if (entry.record.id == decided.id) return assignmentViewJson(entry);
	}
	throw review::ReviewRefused("This assignment's record is no longer in the Matter.");
}

Json leaveOut(const Json& params) {
	std::vector<std::string> changeIds;
	if (params.contains("changeIds") == true && params.at("changeIds").is_array() == true) {
		for (const Json& id : params.at("changeIds")) {
			if (id.is_string() == true && id.get<std::string>().empty() == false) changeIds.push_back(id.get<std::string>());
		}
	}
	return review::reviewToJson(review::leaveOutChanges(required(params, "projectId"), required(params, "reviewId"),
		requiredVersion(params), changeIds));
}

PreparedCopy prepareCopy(const Json& params) {

	const std::string projectId = required(params, "projectId");
	const review::DocumentReview found = review::getReview(projectId, required(params, "reviewId"));
	const int number = requiredVersion(params);
	const review::ReviewVersion* version = nullptr;
	for (const review::ReviewVersion& entry : found.versions) {
		if (entry.number == number) version = &entry;
	}
	if (version == nullptr || version->resultRelativePath.empty() == true) {
		throw review::ReviewRefused("This version has no revised draft to save.");
	}
	std::optional<matters::Matter> matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = matters::load_matter(node::require_active_workspace_path(), projectId);
	}
	if (matter.has_value() == false || matter->directory_path.has_value() == false) {
		throw review::ReviewRefused("This Matter is not open in Singularity on this computer.");
	}
	const std::filesystem::path folder = std::filesystem::u8path(*matter->directory_path);
	if (documents::registry::is_relative_path_safe(folder, version->resultRelativePath) == false) {
		throw review::ReviewRefused("The revised draft is not inside this Matter's folder, so it was not copied.");
	}
	PreparedCopy prepared;
	prepared.source = (folder / std::filesystem::u8path(version->resultRelativePath)).lexically_normal();
	std::error_code error;
	if (std::filesystem::is_regular_file(prepared.source, error) == false) {
		throw review::ReviewRefused("The revised draft is no longer in the Matter's folder.");
	}
	prepared.defaultName = version->resultFileName.empty() == false ? version->resultFileName
		: prepared.source.filename().u8string();
	return prepared;

}

Json finishCopy(const PreparedCopy& prepared, const std::optional<std::filesystem::path>& destination) {

	if (destination.has_value() == false) return Json(false);
	std::error_code error;
	std::filesystem::copy_file(prepared.source, *destination, std::filesystem::copy_options::overwrite_existing, error);
	if (error) throw review::ReviewRefused("The copy could not be saved there. Choose another place and try again.");
	return Json(true);

}

} // namespace singularity::bridge::document_review_bridge
