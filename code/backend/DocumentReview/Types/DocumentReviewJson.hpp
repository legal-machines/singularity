#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "DocumentReview/Types/DocumentReview.hpp"

namespace singularity::document_review {

// A document review as the Matter's data keeps it (MatterRepository, the
// "documentReviews" collection) and as the Native API answers it. The parts
// kept as JSON text (a version's result, an execution's preparation and
// stages, the source's structure) are written as JSON, not as strings.

namespace json_detail {

inline nlohmann::json parsedOrNull(const std::string& text) {
	if (text.empty() == true) return nlohmann::json(nullptr);
	nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
	return value.is_discarded() == true ? nlohmann::json(nullptr) : value;
}

inline std::string dumpedOrEmpty(const nlohmann::json& value, const char* key) {
	if (value.contains(key) == false || value.at(key).is_null() == true) return std::string();
	return value.at(key).dump();
}

} // namespace json_detail

inline nlohmann::json executionToJson(const ReviewExecution& execution) {
	return nlohmann::json{
		{"runId", execution.runId}, {"assignmentId", execution.assignmentId}, {"operationId", execution.operationId},
		{"providerId", execution.providerId}, {"modelId", execution.modelId}, {"servedBy", execution.servedBy},
		{"destination", execution.destination},
		{"isProtectionApplied", execution.isProtectionApplied}, {"inputTokens", execution.inputTokens},
		{"outputTokens", execution.outputTokens},
		{"costMicros", execution.costMicros.has_value() == true ? nlohmann::json(*execution.costMicros) : nlohmann::json(nullptr)},
		{"costCurrency", execution.costCurrency}, {"costSource", execution.costSource},
		{"latencyMilliseconds", execution.latencyMilliseconds},
		{"preparationMilliseconds", execution.preparationMilliseconds}, {"finishReason", execution.finishReason},
		{"startedAt", execution.startedAt}, {"finishedAt", execution.finishedAt},
		{"preparation", json_detail::parsedOrNull(execution.preparation)},
		{"stages", json_detail::parsedOrNull(execution.stages)}, {"attempt", execution.attempt}};
}

inline ReviewExecution executionFromJson(const nlohmann::json& value) {
	ReviewExecution execution;
	if (value.is_object() == false) return execution;
	execution.runId = value.value("runId", std::string());
	execution.assignmentId = value.value("assignmentId", std::string());
	execution.operationId = value.value("operationId", std::string());
	execution.providerId = value.value("providerId", std::string());
	execution.modelId = value.value("modelId", std::string());
	execution.servedBy = value.value("servedBy", std::string());
	execution.destination = value.value("destination", std::string());
	execution.isProtectionApplied = value.value("isProtectionApplied", false);
	execution.inputTokens = value.value("inputTokens", 0LL);
	execution.outputTokens = value.value("outputTokens", 0LL);
	if (value.contains("costMicros") == true && value.at("costMicros").is_number_integer() == true) {
		execution.costMicros = value.at("costMicros").get<long long>();
	}
	execution.costCurrency = value.value("costCurrency", std::string());
	execution.costSource = value.value("costSource", std::string());
	execution.latencyMilliseconds = value.value("latencyMilliseconds", 0LL);
	execution.preparationMilliseconds = value.value("preparationMilliseconds", 0LL);
	execution.finishReason = value.value("finishReason", std::string());
	execution.startedAt = value.value("startedAt", std::string());
	execution.finishedAt = value.value("finishedAt", std::string());
	execution.preparation = json_detail::dumpedOrEmpty(value, "preparation");
	execution.stages = json_detail::dumpedOrEmpty(value, "stages");
	execution.attempt = value.value("attempt", 1);
	return execution;
}

inline nlohmann::json versionToJson(const ReviewVersion& version) {
	return nlohmann::json{
		{"number", version.number}, {"requestMessageId", version.requestMessageId}, {"instruction", version.instruction},
		{"createdAt", version.createdAt}, {"state", version.state}, {"stateMessage", version.stateMessage},
		{"resultDocumentId", version.resultDocumentId}, {"resultRelativePath", version.resultRelativePath},
		{"resultFileName", version.resultFileName}, {"resultSha256", version.resultSha256},
		{"workProductId", version.workProductId}, {"answerMessageId", version.answerMessageId},
		{"result", json_detail::parsedOrNull(version.result)}, {"execution", executionToJson(version.execution)}};
}

inline ReviewVersion versionFromJson(const nlohmann::json& value) {
	ReviewVersion version;
	if (value.is_object() == false) return version;
	version.number = value.value("number", 0);
	version.requestMessageId = value.value("requestMessageId", std::string());
	version.instruction = value.value("instruction", std::string());
	version.createdAt = value.value("createdAt", std::string());
	version.state = value.value("state", std::string("queued"));
	version.stateMessage = value.value("stateMessage", std::string());
	version.resultDocumentId = value.value("resultDocumentId", std::string());
	version.resultRelativePath = value.value("resultRelativePath", std::string());
	version.resultFileName = value.value("resultFileName", std::string());
	version.resultSha256 = value.value("resultSha256", std::string());
	version.workProductId = value.value("workProductId", std::string());
	version.answerMessageId = value.value("answerMessageId", std::string());
	version.result = json_detail::dumpedOrEmpty(value, "result");
	if (value.contains("execution") == true) version.execution = executionFromJson(value.at("execution"));
	return version;
}

inline nlohmann::json decisionToJson(const ReviewDecision& decision) {
	nlohmann::json value{{"id", decision.id}, {"version", decision.version}, {"decision", decision.decision},
		{"actorId", decision.actorId}, {"actorName", decision.actorName}, {"decidedAt", decision.decidedAt},
		{"comment", decision.comment}, {"messageId", decision.messageId}};
	// Written only where there is something to keep, so an older decision is
	// stored exactly as before.
	if (decision.sha256.empty() == false) {
		value["sha256"] = decision.sha256;
		value["isAsWritten"] = decision.isAsWritten;
	}
	if (decision.leftOut.empty() == false) value["leftOut"] = decision.leftOut;
	return value;
}

inline ReviewDecision decisionFromJson(const nlohmann::json& value) {
	ReviewDecision decision;
	if (value.is_object() == false) return decision;
	decision.id = value.value("id", std::string());
	decision.version = value.value("version", 0);
	decision.decision = value.value("decision", std::string());
	decision.actorId = value.value("actorId", std::string());
	decision.actorName = value.value("actorName", std::string());
	decision.decidedAt = value.value("decidedAt", std::string());
	decision.comment = value.value("comment", std::string());
	decision.messageId = value.value("messageId", std::string());
	decision.sha256 = value.value("sha256", std::string());
	decision.isAsWritten = value.value("isAsWritten", true);
	if (value.contains("leftOut") == true && value.at("leftOut").is_array() == true) {
		for (const nlohmann::json& id : value.at("leftOut")) {
			if (id.is_string() == true) decision.leftOut.push_back(id.get<std::string>());
		}
	}
	return decision;
}

inline nlohmann::json reviewToJson(const DocumentReview& review) {
	nlohmann::json versions = nlohmann::json::array();
	for (const ReviewVersion& version : review.versions) versions.push_back(versionToJson(version));
	nlohmann::json decisions = nlohmann::json::array();
	for (const ReviewDecision& decision : review.decisions) decisions.push_back(decisionToJson(decision));
	return nlohmann::json{
		{"id", review.id}, {"chatId", review.chatId}, {"agentId", review.agentId}, {"agentName", review.agentName},
		{"sourceDocumentId", review.sourceDocumentId}, {"sourceRelativePath", review.sourceRelativePath},
		{"sourceFileName", review.sourceFileName}, {"sourceSha256", review.sourceSha256},
		{"sourceVersion", review.sourceVersion}, {"playbookId", review.playbookId},
		{"playbookName", review.playbookName}, {"playbookVersion", review.playbookVersion},
		{"requestedByActorId", review.requestedByActorId}, {"requestedByName", review.requestedByName},
		{"createdAt", review.createdAt}, {"updatedAt", review.updatedAt},
		{"structure", json_detail::parsedOrNull(review.structure)}, {"versions", std::move(versions)},
		{"decisions", std::move(decisions)}};
}

inline DocumentReview reviewFromJson(const nlohmann::json& value) {
	DocumentReview review;
	if (value.is_object() == false) return review;
	review.id = value.value("id", std::string());
	review.chatId = value.value("chatId", std::string());
	review.agentId = value.value("agentId", std::string());
	review.agentName = value.value("agentName", std::string());
	review.sourceDocumentId = value.value("sourceDocumentId", std::string());
	review.sourceRelativePath = value.value("sourceRelativePath", std::string());
	review.sourceFileName = value.value("sourceFileName", std::string());
	review.sourceSha256 = value.value("sourceSha256", std::string());
	review.sourceVersion = value.value("sourceVersion", 0);
	review.playbookId = value.value("playbookId", std::string());
	review.playbookName = value.value("playbookName", std::string());
	review.playbookVersion = value.value("playbookVersion", 0);
	review.requestedByActorId = value.value("requestedByActorId", std::string());
	review.requestedByName = value.value("requestedByName", std::string());
	review.createdAt = value.value("createdAt", std::string());
	review.updatedAt = value.value("updatedAt", std::string());
	review.structure = json_detail::dumpedOrEmpty(value, "structure");
	if (value.contains("versions") == true && value.at("versions").is_array() == true) {
		for (const nlohmann::json& entry : value.at("versions")) review.versions.push_back(versionFromJson(entry));
	}
	if (value.contains("decisions") == true && value.at("decisions").is_array() == true) {
		for (const nlohmann::json& entry : value.at("decisions")) review.decisions.push_back(decisionFromJson(entry));
	}
	return review;
}

// ---- An assignment's record ---------------------------------------------------

namespace json_detail {

inline std::vector<std::string> stringsIn(const nlohmann::json& value, const char* key) {
	std::vector<std::string> strings;
	if (value.contains(key) == false || value.at(key).is_array() == false) return strings;
	for (const nlohmann::json& entry : value.at(key)) {
		if (entry.is_string() == true) strings.push_back(entry.get<std::string>());
	}
	return strings;
}

} // namespace json_detail

inline nlohmann::json assignmentStepToJson(const AssignmentStepRecord& step) {
	nlohmann::json value{{"number", step.number}, {"documentId", step.documentId}, {"sourceName", step.sourceName},
		{"kind", step.kind}, {"intended", step.intended}, {"basisNames", step.basisNames}, {"reviewId", step.reviewId},
		{"version", step.version}, {"status", step.status}, {"outcome", step.outcome}, {"label", step.label},
		{"statusWords", step.statusWords}, {"remaining", step.remaining}, {"calls", step.calls}, {"retries", step.retries},
		{"recoveries", step.recoveries}};
	// Written only where there is something to keep, so an older step is
	// stored exactly as before.
	if (step.writtenSha256.empty() == false) value["writtenSha256"] = step.writtenSha256;
	if (step.decisions.empty() == false) {
		nlohmann::json decisions = nlohmann::json::array();
		for (const ReviewDecision& decision : step.decisions) decisions.push_back(decisionToJson(decision));
		value["decisions"] = std::move(decisions);
	}
	return value;
}

inline AssignmentStepRecord assignmentStepFromJson(const nlohmann::json& value) {
	AssignmentStepRecord step;
	if (value.is_object() == false) return step;
	step.number = value.value("number", 0);
	step.documentId = value.value("documentId", std::string());
	step.sourceName = value.value("sourceName", std::string());
	step.kind = value.value("kind", std::string());
	step.intended = value.value("intended", std::string());
	step.basisNames = json_detail::stringsIn(value, "basisNames");
	step.reviewId = value.value("reviewId", std::string());
	step.version = value.value("version", 0);
	step.status = value.value("status", std::string());
	step.outcome = value.value("outcome", std::string());
	step.label = value.value("label", std::string());
	step.statusWords = value.value("statusWords", std::string());
	step.remaining = json_detail::stringsIn(value, "remaining");
	step.calls = value.value("calls", 0);
	step.retries = value.value("retries", 0);
	step.recoveries = json_detail::stringsIn(value, "recoveries");
	step.writtenSha256 = value.value("writtenSha256", std::string());
	if (value.contains("decisions") == true && value.at("decisions").is_array() == true) {
		for (const nlohmann::json& entry : value.at("decisions")) step.decisions.push_back(decisionFromJson(entry));
	}
	return step;
}

inline nlohmann::json assignmentEmailToJson(const AssignmentEmailRecord& email) {
	nlohmann::json attachments = nlohmann::json::array();
	for (const AssignmentAttachment& attached : email.attachments) {
		attachments.push_back(nlohmann::json{{"name", attached.name}, {"documentId", attached.documentId},
			{"reviewId", attached.reviewId}, {"version", attached.version}, {"sha256", attached.sha256}});
	}
	return nlohmann::json{{"isWanted", email.isWanted}, {"state", email.state}, {"reason", email.reason},
		{"documentId", email.documentId}, {"name", email.name}, {"subject", email.subject}, {"to", email.to}, {"cc", email.cc},
		{"attachments", attachments}, {"unverified", email.unverified}, {"calls", email.calls}, {"retries", email.retries},
		{"recoveries", email.recoveries}};
}

inline AssignmentEmailRecord assignmentEmailFromJson(const nlohmann::json& value) {
	AssignmentEmailRecord email;
	if (value.is_object() == false) return email;
	email.isWanted = value.value("isWanted", false);
	email.state = value.value("state", std::string());
	email.reason = value.value("reason", std::string());
	email.documentId = value.value("documentId", std::string());
	email.name = value.value("name", std::string());
	email.subject = value.value("subject", std::string());
	email.to = value.value("to", std::string());
	email.cc = value.value("cc", std::string());
	if (value.contains("attachments") == true && value.at("attachments").is_array() == true) {
		for (const nlohmann::json& entry : value.at("attachments")) {
			if (entry.is_object() == false) continue;
			AssignmentAttachment attached;
			attached.name = entry.value("name", std::string());
			attached.documentId = entry.value("documentId", std::string());
			attached.reviewId = entry.value("reviewId", std::string());
			attached.version = entry.value("version", 0);
			attached.sha256 = entry.value("sha256", std::string());
			email.attachments.push_back(attached);
		}
	}
	email.unverified = json_detail::stringsIn(value, "unverified");
	email.calls = value.value("calls", 0);
	email.retries = value.value("retries", 0);
	email.recoveries = json_detail::stringsIn(value, "recoveries");
	return email;
}

inline nlohmann::json assignmentDecisionToJson(const AssignmentDecision& decision) {
	return nlohmann::json{{"id", decision.id}, {"stepNumber", decision.stepNumber}, {"reviewId", decision.reviewId},
		{"version", decision.version}, {"documentId", decision.documentId}, {"documentName", decision.documentName},
		{"place", decision.place}, {"question", decision.question}, {"why", decision.why}, {"options", decision.options},
		{"isRecommended", decision.isRecommended}, {"reasoning", decision.reasoning}, {"isBlocking", decision.isBlocking},
		{"messageId", decision.messageId}, {"state", decision.state},
		{"answer", decision.answer},
		{"answeredByActorId", decision.answeredByActorId}, {"answeredByName", decision.answeredByName},
		{"answeredAt", decision.answeredAt}};
}

inline AssignmentDecision assignmentDecisionFromJson(const nlohmann::json& value) {
	AssignmentDecision decision;
	if (value.is_object() == false) return decision;
	decision.id = value.value("id", std::string());
	decision.stepNumber = value.value("stepNumber", 0);
	decision.reviewId = value.value("reviewId", std::string());
	decision.version = value.value("version", 0);
	decision.documentId = value.value("documentId", std::string());
	decision.documentName = value.value("documentName", std::string());
	decision.place = value.value("place", std::string());
	decision.question = value.value("question", std::string());
	decision.why = value.value("why", std::string());
	decision.options = json_detail::stringsIn(value, "options");
	decision.isRecommended = value.value("isRecommended", false);
	decision.reasoning = value.value("reasoning", std::string());
	decision.isBlocking = value.value("isBlocking", false);
	decision.messageId = value.value("messageId", std::string());
	decision.state = value.value("state", std::string("open"));
	decision.answer = value.value("answer", std::string());
	decision.answeredByActorId = value.value("answeredByActorId", std::string());
	decision.answeredByName = value.value("answeredByName", std::string());
	decision.answeredAt = value.value("answeredAt", std::string());
	return decision;
}

inline nlohmann::json assignmentCallToJson(const AssignmentCall& call) {
	return nlohmann::json{{"turn", call.turn}, {"tool", call.tool}, {"arguments", call.arguments}, {"state", call.state},
		{"result", call.result}, {"startedAt", call.startedAt}, {"finishedAt", call.finishedAt}};
}

inline AssignmentCall assignmentCallFromJson(const nlohmann::json& value) {
	AssignmentCall call;
	if (value.is_object() == false) return call;
	call.turn = value.value("turn", 0);
	call.tool = value.value("tool", std::string());
	call.arguments = value.value("arguments", std::string());
	call.state = value.value("state", std::string("running"));
	call.result = value.value("result", std::string());
	call.startedAt = value.value("startedAt", std::string());
	call.finishedAt = value.value("finishedAt", std::string());
	return call;
}

inline nlohmann::json assignmentToJson(const AssignmentRecord& record) {
	nlohmann::json steps = nlohmann::json::array();
	for (const AssignmentStepRecord& step : record.steps) steps.push_back(assignmentStepToJson(step));
	nlohmann::json decisions = nlohmann::json::array();
	for (const AssignmentDecision& decision : record.decisions) decisions.push_back(assignmentDecisionToJson(decision));
	nlohmann::json toolCalls = nlohmann::json::array();
	for (const AssignmentCall& call : record.toolCalls) toolCalls.push_back(assignmentCallToJson(call));
	nlohmann::json handles = nlohmann::json::object();
	for (const auto& [handle, documentId] : record.handles) handles[handle] = documentId;
	return nlohmann::json{{"id", record.id}, {"chatId", record.chatId}, {"requestMessageId", record.requestMessageId},
		{"ackMessageId", record.ackMessageId}, {"summaryMessageId", record.summaryMessageId}, {"agentId", record.agentId},
		{"agentName", record.agentName}, {"givenByActorId", record.givenByActorId},
		{"givenByName", record.givenByName}, {"instruction", record.instruction}, {"understanding", record.understanding},
		{"setAside", record.setAside}, {"providerId", record.providerId}, {"modelId", record.modelId},
		{"startedAt", record.startedAt}, {"finishedAt", record.finishedAt}, {"state", record.state}, {"steps", steps},
		{"email", assignmentEmailToJson(record.email)}, {"decisions", decisions}, {"toolCalls", toolCalls},
		{"handles", handles}, {"turns", record.turns}, {"calls", record.calls},
		{"retries", record.retries},
		{"recoveries", record.recoveries}, {"inputTokens", record.inputTokens}, {"outputTokens", record.outputTokens},
		{"costMicros", record.costMicros.has_value() == true ? nlohmann::json(*record.costMicros) : nlohmann::json(nullptr)},
		{"milliseconds", record.milliseconds}};
}

inline AssignmentRecord assignmentFromJson(const nlohmann::json& value) {
	AssignmentRecord record;
	if (value.is_object() == false) return record;
	record.id = value.value("id", std::string());
	record.chatId = value.value("chatId", std::string());
	record.requestMessageId = value.value("requestMessageId", std::string());
	record.ackMessageId = value.value("ackMessageId", std::string());
	record.summaryMessageId = value.value("summaryMessageId", std::string());
	record.agentId = value.value("agentId", std::string());
	record.agentName = value.value("agentName", std::string());
	record.givenByActorId = value.value("givenByActorId", std::string());
	record.givenByName = value.value("givenByName", std::string());
	record.instruction = value.value("instruction", std::string());
	record.understanding = value.value("understanding", std::string());
	record.setAside = json_detail::stringsIn(value, "setAside");
	record.providerId = value.value("providerId", std::string());
	record.modelId = value.value("modelId", std::string());
	record.startedAt = value.value("startedAt", std::string());
	record.finishedAt = value.value("finishedAt", std::string());
	record.state = value.value("state", std::string("working"));
	if (value.contains("steps") == true && value.at("steps").is_array() == true) {
		for (const nlohmann::json& entry : value.at("steps")) record.steps.push_back(assignmentStepFromJson(entry));
	}
	if (value.contains("email") == true) record.email = assignmentEmailFromJson(value.at("email"));
	if (value.contains("decisions") == true && value.at("decisions").is_array() == true) {
		for (const nlohmann::json& entry : value.at("decisions")) record.decisions.push_back(assignmentDecisionFromJson(entry));
	}
	if (value.contains("toolCalls") == true && value.at("toolCalls").is_array() == true) {
		for (const nlohmann::json& entry : value.at("toolCalls")) record.toolCalls.push_back(assignmentCallFromJson(entry));
	}
	if (value.contains("handles") == true && value.at("handles").is_object() == true) {
		for (const auto& [handle, documentId] : value.at("handles").items()) {
			if (documentId.is_string() == true) record.handles.push_back({handle, documentId.get<std::string>()});
		}
	}
	record.turns = value.value("turns", 0);
	record.calls = value.value("calls", 0);
	record.retries = value.value("retries", 0);
	record.recoveries = json_detail::stringsIn(value, "recoveries");
	record.inputTokens = value.value("inputTokens", 0LL);
	record.outputTokens = value.value("outputTokens", 0LL);
	if (value.contains("costMicros") == true && value.at("costMicros").is_number_integer() == true) {
		record.costMicros = value.at("costMicros").get<long long>();
	}
	record.milliseconds = value.value("milliseconds", 0LL);
	return record;
}

} // namespace singularity::document_review
